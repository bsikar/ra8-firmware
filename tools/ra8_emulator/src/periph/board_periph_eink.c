/**
 * @file board_periph_eink.c
 * @brief IT8951 e-paper SPI-device model for ra8_emulator.
 *
 * @details
 * Implements board_periph_eink.h. The controller is a byte state machine on the
 * SPI_B bus: it assembles the 16-bit words the ``ra8_epaper`` driver clocks
 * MSB-first, tracks the current preamble / command / register, and returns the
 * controller's response bytes during a read burst so the driver's
 * GET_DEV_INFO drain and its LUTAFSR "LUT idle" poll complete exactly as they
 * do on silicon (EIL == HIL).
 *
 * Wire protocol (IT8951 datasheet rev 0.2 chapter 3.4 "SPI Interface" +
 * chapter 4 "Application Note"): every transaction opens with a 16-bit
 * preamble -- ``0x6000`` (command follows), ``0x0000`` (data write follows) or
 * ``0x1000`` (data read follows) -- then 16-bit words are clocked MSB-first.
 * The driver's ``read_data16`` reads one dummy word then the value word after a
 * read preamble, so each read burst here serves two words (four bytes).
 *
 * The model is transport-symmetric with ``board_periph_sd.c``: it self-frames
 * off the preamble words (no chip-select wiring needed) and exposes attach /
 * exchange / reset the SPI_B block calls. The panel HRDY "ready" GPIO is driven
 * high from the GPIO block's reset via ::board_eink_apply_gpio_defaults.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "board_periph_eink.h"

#include <stdint.h>
#include <stdio.h>

#include "board_periph.h"

/**
 * @enum eink_preamble_t
 * @brief SPI preamble words (IT8951 DS chapter 3.4 table 3-3).
 */
typedef enum : uint16_t {
  k_eink_pre_cmd = 0x6000U, /**< Host -> command write. */
  k_eink_pre_wr  = 0x0000U, /**< Host -> data write.    */
  k_eink_pre_rd  = 0x1000U, /**< Host <- data read.     */
} eink_preamble_t;

/**
 * @enum eink_cmd_t
 * @brief IT8951 user commands the ``ra8_epaper`` driver issues (DS chapter 4.2).
 */
typedef enum : uint16_t {
  k_eink_cmd_sys_run      = 0x0001U, /**< Wake from standby.      */
  k_eink_cmd_sleep        = 0x0003U, /**< Enter deep-sleep.       */
  k_eink_cmd_reg_rd       = 0x0010U, /**< Register read.          */
  k_eink_cmd_reg_wr       = 0x0011U, /**< Register write.         */
  k_eink_cmd_ld_img_area  = 0x0021U, /**< Begin load (rectangle). */
  k_eink_cmd_ld_img_end   = 0x0022U, /**< End load.               */
  k_eink_cmd_dpy_area     = 0x0034U, /**< Refresh rectangle.      */
  k_eink_cmd_vcom         = 0x0039U, /**< Get / set VCOM bias.    */
  k_eink_cmd_get_dev_info = 0x0302U, /**< 40-byte info block.     */
} eink_cmd_t;

/**
 * @enum eink_vcom_arg_t
 * @brief Direction word of the VCOM command (DS user command set).
 */
typedef enum : uint16_t {
  k_eink_vcom_get = 0x0000U, /**< Controller answers with its VCOM.  */
  k_eink_vcom_set = 0x0001U, /**< Controller consumes the next word. */
} eink_vcom_arg_t;

/**
 * @enum eink_vcom_default_t
 * @brief VCOM the modelled controller powers up holding.
 *
 * @details
 * A vendor-provisioned IT8951 driver board boots with the VCOM its own
 * configuration stores, and the firmware's calibration resolver reads it
 * back as its first-choice source. The model reports a plausible in-range
 * magnitude in millivolts so that path exercises end-to-end in the emulator exactly
 * as it does on silicon (EIL == HIL). This is a *modelled* controller
 * value, not a calibration default for any real panel -- real panels carry
 * their own VCOM printed on the flex cable.
 */
typedef enum : uint16_t {
  k_eink_vcom_power_on_mv = 1530U, /**< Reported VCOM magnitude (mV). */
} eink_vcom_default_t;

/**
 * @enum eink_reg_t
 * @brief Controller registers whose read value the model must answer.
 */
typedef enum : uint16_t {
  k_eink_reg_lutafsr = 0x1224U, /**< LUT busy status: 0 = idle (DS 4.2.4). */
} eink_reg_t;

/**
 * @enum eink_geom_t
 * @brief Fixed panel geometry the model reports through GET_DEV_INFO.
 *
 * @details Informational only -- the driver drains and discards the device
 * block -- but kept faithful to the demo panel so the emulator report reads
 * sensibly.
 */
typedef enum : uint16_t {
  k_eink_panel_w = 128U, /**< Reported panel width  (px). */
  k_eink_panel_h = 128U, /**< Reported panel height (px). */
} eink_geom_t;

/**
 * @enum eink_datawr_idx_t
 * @brief Data-word positions within a command's argument stream.
 */
typedef enum : uint16_t {
  k_eink_idx_reg_addr    = 0U, /**< REG_RD/REG_WR: register address word.  */
  k_eink_idx_ld_arg0     = 0U, /**< LD_IMG_AREA: endian/pf/rotate word.    */
  k_eink_idx_ld_w        = 3U, /**< LD_IMG_AREA: width  (arg3 after arg0). */
  k_eink_idx_ld_h        = 4U, /**< LD_IMG_AREA: height (arg4 after arg0). */
  k_eink_idx_ld_px_start = 5U, /**< LD_IMG_AREA: first pixel word.         */
  k_eink_idx_dpy_wf      = 4U, /**< DPY_AREA: waveform word (arg4).        */
  k_eink_idx_vcom_dir    = 0U, /**< VCOM: direction word (get / set).      */
  k_eink_idx_vcom_val    = 1U, /**< VCOM: value word on a set.             */
} eink_datawr_idx_t;

/**
 * @enum eink_wire_pf_t
 * @brief LD_IMG_AREA ``arg0`` pixel-format codes.
 *
 * @details
 * The controller numbers its formats 2 bpp = 0, 3 bpp = 1, 4 bpp = 2,
 * 8 bpp = 3. The model decodes the field so its pixel accounting stays
 * correct when the firmware switches depth -- at 4 bpp one 16-bit word
 * carries four pixels, not two.
 */
typedef enum : uint16_t {
  k_eink_wire_pf_2bpp = 0U, /**< 2 bits per pixel. */
  k_eink_wire_pf_3bpp = 1U, /**< 3 bits per pixel. */
  k_eink_wire_pf_4bpp = 2U, /**< 4 bits per pixel. */
  k_eink_wire_pf_8bpp = 3U, /**< 8 bits per pixel. */
} eink_wire_pf_t;

/**
 * @enum eink_read_idx_t
 * @brief Byte positions in the 4-byte (2-word) read-burst response buffer.
 */
typedef enum : uint8_t {
  k_eink_rd_dummy_hi = 0U, /**< High byte of the dummy word. */
  k_eink_rd_dummy_lo = 1U, /**< Low byte of the dummy word.  */
  k_eink_rd_val_hi   = 2U, /**< High byte of the value word. */
  k_eink_rd_val_lo   = 3U, /**< Low byte of the value word.  */
} eink_read_idx_t;

/**
 * @enum eink_sizing_t
 * @brief Byte-assembly + read-burst sizing constants (no magic numbers).
 */
typedef enum : uint32_t {
  k_eink_byte_shift = 8U,    /**< Bits per byte.                        */
  k_eink_byte_mask  = 0xFFU, /**< Low-byte extraction mask.             */
  k_eink_read_bytes = 4U,    /**< Bytes per read burst (dummy + value). */
  k_eink_dev_words  = 20U,   /**< GET_DEV_INFO block length (40 / 2).   */
  k_eink_word_bits  = 16U,   /**< Bits carried by one 16-bit word.      */
  k_eink_pf_shift   = 4U,    /**< LD_IMG_AREA arg0 pixel-format shift.  */
  k_eink_pf_mask    = 0x3U,  /**< LD_IMG_AREA arg0 pixel-format mask.   */
  k_eink_idle_byte  = 0x00U, /**< Non-read response / idle byte.        */
} eink_sizing_t;

/**
 * @enum eink_hrdy_pin_t
 * @brief Panel HRDY "ready" GPIO on the modelled carrier.
 *
 * @details Must match the ``epaper_refresh`` app's ``busy_pin`` (P4_01) so the
 * firmware's HRDY poll reads the level this model drives.
 */
typedef enum : uint8_t {
  k_eink_hrdy_port = 4U, /**< HRDY on PORT4.         */
  k_eink_hrdy_pin  = 1U, /**< HRDY on pin 1 (P4_01). */
} eink_hrdy_pin_t;

/**
 * @enum eink_state_t
 * @brief Host-word interpretation state.
 */
typedef enum : uint8_t {
  k_eink_st_preamble = 0U, /**< Next assembled word is a preamble. */
  k_eink_st_cmd      = 1U, /**< Next assembled word is a command.  */
  k_eink_st_datawr   = 2U, /**< Next assembled word is data.       */
} eink_state_t;

/**
 * @struct eink_model_t
 * @brief The modelled IT8951 controller's whole state.
 */
typedef struct {
  bool         attached;                  /**< --eink armed the controller.            */
  eink_state_t st;                        /**< Host-word interpretation state.         */
  bool         have_hi;                   /**< A high byte is latched in @c hi.        */
  uint8_t      hi;                        /**< Latched high byte of the pending word.  */
  uint16_t     cur_cmd;                   /**< Command in flight (0 if none).          */
  uint16_t     datawr_idx;                /**< Data-word index within @c cur_cmd.      */
  uint16_t     reg_addr;                  /**< Register address for a REG_RD/REG_WR.   */
  uint16_t     dev_idx;                   /**< Next GET_DEV_INFO word index.           */
  uint16_t     vcom_mv;                   /**< Modelled VCOM magnitude in millivolts.  */
  uint16_t     vcom_dir;                  /**< Direction word of the VCOM in flight.   */
  uint16_t     px_per_word;               /**< Pixels per data word at the loaded pf.  */
  bool         reading;                   /**< A read burst is serving response bytes. */
  uint8_t      rd_pos;                    /**< Cursor into @c rd_buf.                  */
  uint8_t      rd_len;                    /**< Valid bytes in @c rd_buf.               */
  uint8_t      rd_buf[k_eink_read_bytes]; /**< Read-burst response.                    */
  uint64_t     pixels;                    /**< Pixels streamed via LD_IMG_AREA.        */
  uint32_t     refreshes;                 /**< DPY_AREA refreshes issued.              */
  uint16_t     last_wf;                   /**< Waveform of the last DPY_AREA.          */
} eink_model_t;

/** @brief The single modelled controller. */
static eink_model_t s_eink;

/**
 * @brief Read-back value for a controller register.
 *
 * @details The only register the driver reads is LUTAFSR, which must read 0
 * ("no LUT busy") so the display poll completes; every other read is 0 too.
 *
 * @param[in] reg Register address the driver selected.
 * @return The 16-bit register value (0 for LUTAFSR / anything else).
 */
static uint16_t eink_reg_value(uint16_t reg)
{
  if (reg == (uint16_t)k_eink_reg_lutafsr) {
    return 0U; /* LUT idle -> ra8_epaper_display_area poll exits. */
  }
  return 0U;
}

/** @brief Word i of the (discarded) GET_DEV_INFO block: W, H, then zeros. */
static uint16_t eink_dev_info_word(uint16_t idx)
{
  if (idx == 0U) {
    return (uint16_t)k_eink_panel_w;
  }
  if (idx == 1U) {
    return (uint16_t)k_eink_panel_h;
  }
  return 0U;
}

/** @brief Pixels a 16-bit data word carries at the given wire pixel format. */
static uint16_t eink_px_per_word(uint16_t wire_pf)
{
  if (wire_pf == (uint16_t)k_eink_wire_pf_2bpp) {
    return (uint16_t)((uint16_t)k_eink_word_bits / 2U);
  }
  if (wire_pf == (uint16_t)k_eink_wire_pf_3bpp) {
    return (uint16_t)((uint16_t)k_eink_word_bits / 3U);
  }
  if (wire_pf == (uint16_t)k_eink_wire_pf_4bpp) {
    return (uint16_t)((uint16_t)k_eink_word_bits / 4U);
  }
  return (uint16_t)((uint16_t)k_eink_word_bits / 8U); /* k_eink_wire_pf_8bpp */
}

/** @brief Load @c rd_buf with [dummy word, value word] and open the read burst. */
static void eink_begin_read(void)
{
  uint16_t value = 0U;
  if (s_eink.cur_cmd == (uint16_t)k_eink_cmd_reg_rd) {
    value = eink_reg_value(s_eink.reg_addr);
  } else if (s_eink.cur_cmd == (uint16_t)k_eink_cmd_vcom) {
    /* Only a "get" reads back; the driver never reads after a set. */
    value = s_eink.vcom_mv;
  } else if (s_eink.cur_cmd == (uint16_t)k_eink_cmd_get_dev_info) {
    value          = eink_dev_info_word(s_eink.dev_idx);
    s_eink.dev_idx = (uint16_t)(s_eink.dev_idx + 1U);
  }
  s_eink.rd_buf[k_eink_rd_dummy_hi] = (uint8_t)k_eink_idle_byte;
  s_eink.rd_buf[k_eink_rd_dummy_lo] = (uint8_t)k_eink_idle_byte;
  s_eink.rd_buf[k_eink_rd_val_hi] =
    (uint8_t)((value >> (uint16_t)k_eink_byte_shift) & (uint16_t)k_eink_byte_mask);
  s_eink.rd_buf[k_eink_rd_val_lo] = (uint8_t)(value & (uint16_t)k_eink_byte_mask);
  s_eink.rd_len                   = (uint8_t)k_eink_read_bytes;
  s_eink.rd_pos                   = 0U;
  s_eink.reading                  = true;
}

/** @brief Interpret one data word against the command currently in flight. */
static void eink_consume_data(uint16_t word)
{
  if ((s_eink.cur_cmd == (uint16_t)k_eink_cmd_reg_rd) ||
      (s_eink.cur_cmd == (uint16_t)k_eink_cmd_reg_wr)) {
    if (s_eink.datawr_idx == (uint16_t)k_eink_idx_reg_addr) {
      s_eink.reg_addr = word;
    }
  } else if (s_eink.cur_cmd == (uint16_t)k_eink_cmd_vcom) {
    if (s_eink.datawr_idx == (uint16_t)k_eink_idx_vcom_dir) {
      s_eink.vcom_dir = word;
    } else if ((s_eink.datawr_idx == (uint16_t)k_eink_idx_vcom_val) &&
               (s_eink.vcom_dir == (uint16_t)k_eink_vcom_set)) {
      s_eink.vcom_mv = word;
    } else {
      /* A get takes no value word; nothing further to consume. */
    }
  } else if (s_eink.cur_cmd == (uint16_t)k_eink_cmd_ld_img_area) {
    if (s_eink.datawr_idx == (uint16_t)k_eink_idx_ld_arg0) {
      const uint16_t wire_pf =
        (uint16_t)((word >> (uint16_t)k_eink_pf_shift) & (uint16_t)k_eink_pf_mask);
      s_eink.px_per_word = eink_px_per_word(wire_pf);
    } else if (s_eink.datawr_idx >= (uint16_t)k_eink_idx_ld_px_start) {
      s_eink.pixels += (uint64_t)s_eink.px_per_word;
    } else {
      /* Geometry args: no pixel accounting. */
    }
  } else if (s_eink.cur_cmd == (uint16_t)k_eink_cmd_dpy_area) {
    if (s_eink.datawr_idx == (uint16_t)k_eink_idx_dpy_wf) {
      s_eink.last_wf   = word;
      s_eink.refreshes = (uint32_t)(s_eink.refreshes + 1U);
    }
  }
}

/** @brief Advance the state machine by one fully-assembled 16-bit word. */
static void eink_consume_word(uint16_t word)
{
  if (s_eink.st == (eink_state_t)k_eink_st_cmd) {
    s_eink.cur_cmd    = word;
    s_eink.datawr_idx = 0U;
    if (word == (uint16_t)k_eink_cmd_get_dev_info) {
      s_eink.dev_idx = 0U;
    }
    s_eink.st = (eink_state_t)k_eink_st_preamble;
    return;
  }
  if (s_eink.st == (eink_state_t)k_eink_st_datawr) {
    eink_consume_data(word);
    s_eink.datawr_idx = (uint16_t)(s_eink.datawr_idx + 1U);
    s_eink.st         = (eink_state_t)k_eink_st_preamble;
    return;
  }
  /* k_eink_st_preamble: classify the preamble word. */
  if (word == (uint16_t)k_eink_pre_cmd) {
    s_eink.st = (eink_state_t)k_eink_st_cmd;
  } else if (word == (uint16_t)k_eink_pre_wr) {
    s_eink.st = (eink_state_t)k_eink_st_datawr;
  } else if (word == (uint16_t)k_eink_pre_rd) {
    eink_begin_read(); /* subsequent dummy bytes are answered from rd_buf */
  }
}

bool board_eink_attach(void)
{
  s_eink.attached = true;
  board_eink_reset();
  return true;
}

bool board_eink_attached(void)
{
  return s_eink.attached;
}

uint8_t board_eink_exchange(uint8_t tx)
{
  if (!s_eink.attached) {
    return (uint8_t)k_eink_idle_byte;
  }
  if (s_eink.reading) {
    const uint8_t r =
      (s_eink.rd_pos < s_eink.rd_len) ? s_eink.rd_buf[s_eink.rd_pos] : (uint8_t)k_eink_idle_byte;
    s_eink.rd_pos = (uint8_t)(s_eink.rd_pos + 1U);
    if (s_eink.rd_pos >= s_eink.rd_len) {
      s_eink.reading = false;
    }
    return r;
  }
  if (!s_eink.have_hi) {
    s_eink.hi      = tx;
    s_eink.have_hi = true;
    return (uint8_t)k_eink_idle_byte;
  }
  const uint16_t word =
    (uint16_t)(((uint16_t)s_eink.hi << (uint16_t)k_eink_byte_shift) | (uint16_t)tx);
  s_eink.have_hi = false;
  eink_consume_word(word);
  return (uint8_t)k_eink_idle_byte;
}

void board_eink_reset(void)
{
  const bool     was_attached = s_eink.attached;
  const uint64_t pixels       = s_eink.pixels;
  const uint32_t refreshes    = s_eink.refreshes;
  const uint16_t vcom_mv      = s_eink.vcom_mv;
  s_eink                      = (eink_model_t){};
  s_eink.attached             = was_attached;
  s_eink.pixels               = pixels;    /* keep run totals across a mid-run reset        */
  s_eink.refreshes            = refreshes; /* (the SPI block resets framing, not the panel) */
  s_eink.st                   = (eink_state_t)k_eink_st_preamble;
  /* VCOM is controller configuration, not bus framing: a mid-run SPI reset
   * must not clear it, but a cold attach starts from the power-on value. */
  s_eink.vcom_mv     = (vcom_mv != 0U) ? vcom_mv : (uint16_t)k_eink_vcom_power_on_mv;
  s_eink.px_per_word = eink_px_per_word((uint16_t)k_eink_wire_pf_8bpp);
}

void board_eink_apply_gpio_defaults(void)
{
  if (!s_eink.attached) {
    return;
  }
  board_periph_gpio_set_input((uint8_t)k_eink_hrdy_port, (uint8_t)k_eink_hrdy_pin, true);
}

void board_eink_report(void)
{
  if (!s_eink.attached) {
    return;
  }
  (void)fprintf(stderr,
                "  IT8951 e-ink  : %llu pixel(s) loaded, %u refresh(es), last wf=0x%X, "
                "vcom=%umV\n",
                (unsigned long long)s_eink.pixels,
                s_eink.refreshes,
                (unsigned)s_eink.last_wf,
                (unsigned)s_eink.vcom_mv);
}
