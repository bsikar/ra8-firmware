/**
 * @file ra8_epaper.c
 * @brief IT8951 e-paper SPI driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements ra8_epaper.h. The wire protocol follows the IT8951
 * datasheet rev 0.2 chapter 3.4 ("SPI Interface") and chapter 4
 * ("Application Note") plus the Waveshare IT8951 e-paper user
 * guide. References to "DS" / "AN" in comments cite those documents.
 *
 * Every SPI transaction begins with a 16-bit preamble:
 *
 *   - 0x6000 = host -> controller, command code follows
 *   - 0x0000 = host -> controller, data words follow
 *   - 0x1000 = host <- controller, host reads data words
 *
 * The host also has to honour HRDY (a GPIO from the panel) before
 * each preamble: when HRDY is low, the controller is still busy
 * processing the previous request. The driver polls HRDY through
 * ``ra8_gpio_read`` with a bounded retry budget to satisfy NASA
 * Power-of-10 Rule 2.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_epaper.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_port_regs.h"
#include "ra8_port_utils.h"
#include "ra8_spi_bus_ops.h"
#include "ra8_time.h"

/**
 * @var s_tag
 * @brief Logging tag used by every error path in this TU.
 */
static const char* s_tag = "EPAPER";

/* =============================================================================
 * Constants -- typed enums per the no-magic-number rule.
 * =============================================================================
 */

/**
 * @enum ra8_epaper_preamble_t
 * @brief SPI preamble words (DS chapter 3.4 table 3-3).
 */
typedef enum : uint16_t {
  k_ra8_epaper_preamble_cmd = 0x6000U, /**< Host -> command write. */
  k_ra8_epaper_preamble_wr  = 0x0000U, /**< Host -> data write.    */
  k_ra8_epaper_preamble_rd  = 0x1000U, /**< Host <- data read.     */
} ra8_epaper_preamble_t;

/**
 * @enum ra8_epaper_cmd_t
 * @brief Subset of IT8951 user commands used by this driver.
 *
 * @details
 * Codes from DS chapter 4.2 "User Command Set".
 */
typedef enum : uint16_t {
  k_ra8_epaper_cmd_sys_run      = 0x0001U, /**< Wake from standby.      */
  k_ra8_epaper_cmd_sleep        = 0x0003U, /**< Enter deep-sleep.       */
  k_ra8_epaper_cmd_reg_rd       = 0x0010U, /**< Register read.          */
  k_ra8_epaper_cmd_reg_wr       = 0x0011U, /**< Register write.         */
  k_ra8_epaper_cmd_ld_img_area  = 0x0021U, /**< Begin load (rectangle). */
  k_ra8_epaper_cmd_ld_img_end   = 0x0022U, /**< End load.               */
  k_ra8_epaper_cmd_dpy_area     = 0x0034U, /**< Refresh rectangle.      */
  k_ra8_epaper_cmd_vcom         = 0x0039U, /**< Get / set VCOM bias.    */
  k_ra8_epaper_cmd_get_dev_info = 0x0302U, /**< 40-byte info block.     */
} ra8_epaper_cmd_t;

/**
 * @enum ra8_epaper_vcom_arg_t
 * @brief Argument word selecting the direction of the VCOM command.
 *
 * @details
 * The VCOM command (::k_ra8_epaper_cmd_vcom) is bidirectional: the first
 * data word chooses whether the controller answers with its current VCOM
 * or consumes a following word as the new one. Matches the Waveshare
 * IT8951 reference driver's ``EPD_IT8951_GetVCOM`` / ``SetVCOM``.
 */
typedef enum : uint16_t {
  k_ra8_epaper_vcom_get = 0x0000U, /**< Read VCOM; one word follows back. */
  k_ra8_epaper_vcom_set = 0x0001U, /**< Write VCOM; one word follows out. */
} ra8_epaper_vcom_arg_t;

/**
 * @enum ra8_epaper_reg_t
 * @brief Memory-mapped controller registers we touch.
 */
typedef enum : uint16_t {
  k_ra8_epaper_reg_lisar_lo = 0x0208U, /**< LISAR low half (DS 4.4). */
  k_ra8_epaper_reg_lisar_hi = 0x020AU, /**< LISAR high half.         */
  k_ra8_epaper_reg_lutafsr  = 0x1224U, /**< LUT busy status.         */
} ra8_epaper_reg_t;

/**
 * @enum ra8_epaper_limits_t
 * @brief Bounded retry / sizing limits.
 */
typedef enum : uint32_t {
  k_ra8_epaper_busy_poll_max  = 200000U, /**< Outer HRDY poll budget.    */
  k_ra8_epaper_lut_poll_max   = 200000U, /**< LUT-busy poll budget.      */
  k_ra8_epaper_dev_info_words = 20U,     /**< 40-byte block / 2.         */
  k_ra8_epaper_panel_max_dim  = 4096U,   /**< Sanity ceiling on cfg.     */
  k_ra8_epaper_reset_pulse_ms = 10U,     /**< Reset assert dwell.        */
  k_ra8_epaper_status_unset   = 0xFFFFU, /**< Pre-read sentinel value.   */
  k_ra8_epaper_byte_mask      = 0xFFU,   /**< Low-byte extraction mask.  */
  k_ra8_epaper_dummy_tx       = 0xFFU,   /**< Dummy byte for SPI reads.  */
  k_ra8_epaper_white_pad      = 0x00FFU, /**< 0xFF pad for odd tail.     */
  k_ra8_epaper_byte_shift     = 8U,      /**< Bits per byte.             */
  k_ra8_epaper_pf_shift       = 4U,      /**< LD_IMG_AREA arg0 PF shift. */
  k_ra8_epaper_word_shift     = 16U,     /**< Bits per 16-bit word.      */
  k_ra8_epaper_ver_per_word   = 2U,      /**< Version chars per word.    */
  k_ra8_epaper_ascii_min      = 0x20U,   /**< Lowest printable ASCII.    */
  k_ra8_epaper_ascii_max      = 0x7FU,   /**< One past printable ASCII.  */
} ra8_epaper_limits_t;

/**
 * @enum ra8_epaper_wire_pf_t
 * @brief LD_IMG_AREA ``arg0`` pixel-format codes (IT8951 programming guide).
 *
 * @details
 * Deliberately distinct from ::ra8_epaper_pixel_format_t: the controller
 * numbers its formats 2 bpp = 0, 3 bpp = 1, 4 bpp = 2, 8 bpp = 3, which
 * is neither the bit depth nor the driver's selector ordering. 1 bpp has
 * no code of its own -- the controller consumes bi-level data through the
 * 8 bpp path with the bitmap-mode register armed.
 */
typedef enum : uint16_t {
  k_ra8_epaper_wire_pf_2bpp = 0U, /**< 2 bits per pixel. */
  k_ra8_epaper_wire_pf_3bpp = 1U, /**< 3 bits per pixel. */
  k_ra8_epaper_wire_pf_4bpp = 2U, /**< 4 bits per pixel. */
  k_ra8_epaper_wire_pf_8bpp = 3U, /**< 8 bits per pixel. */
} ra8_epaper_wire_pf_t;

/**
 * @enum ra8_epaper_dev_info_idx_t
 * @brief Word offsets inside the 20-word ``GET_DEV_INFO`` response.
 */
typedef enum : uint8_t {
  k_ra8_epaper_dev_idx_width   = 0U,  /**< Panel width.                  */
  k_ra8_epaper_dev_idx_height  = 1U,  /**< Panel height.                 */
  k_ra8_epaper_dev_idx_buf_lo  = 2U,  /**< Image-buffer base, low half.  */
  k_ra8_epaper_dev_idx_buf_hi  = 3U,  /**< Image-buffer base, high half. */
  k_ra8_epaper_dev_idx_fw      = 4U,  /**< First firmware-version word.  */
  k_ra8_epaper_dev_idx_lut     = 12U, /**< First LUT-version word.       */
  k_ra8_epaper_dev_idx_ver_end = 20U, /**< One past the last word.       */
} ra8_epaper_dev_info_idx_t;

/**
 * @enum ra8_epaper_state_t
 * @brief Driver lifecycle state.
 */
typedef enum : uint8_t {
  k_ra8_epaper_state_uninit = 0U, /**< Not initialized yet.  */
  k_ra8_epaper_state_ready  = 1U, /**< Initialized and idle. */
} ra8_epaper_state_t;

/**
 * @struct ra8_epaper_panel_t
 * @brief File-scope panel context.
 *
 * @details
 * Static allocation only; we keep one panel per build. ``cfg`` is
 * copied from the caller-supplied descriptor at ``ra8_epaper_init``.
 */
typedef struct {
  ra8_epaper_cfg_t      cfg;   /**< Copy of init cfg.               */
  ra8_epaper_dev_info_t info;  /**< Decoded GET_DEV_INFO from init. */
  ra8_epaper_state_t    state; /**< Current lifecycle state.        */
} ra8_epaper_panel_t;

/**
 * @var s_panel
 * @brief Single-instance panel context.
 */
static ra8_epaper_panel_t s_panel;

/* =============================================================================
 * Low-level SPI helpers
 * =============================================================================
 */

/**
 * @brief Send a 16-bit word MSB-first over the injected SPI bus seam.
 *
 * @param[in] word Word to send.
 *
 * @return ``ra8_err_t`` from the underlying ``bus.xfer8``.
 *
 * @pre  ``ra8_epaper_init`` validated the injected bus seam.
 * @post One word has been clocked out; receive bytes discarded.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_send16(uint16_t word)
{
  const ra8_spi_bus_ops_t* bus = &s_panel.cfg.bus;
  uint8_t                  hi =
    (uint8_t)((word >> (uint16_t)k_ra8_epaper_byte_shift) & (uint16_t)k_ra8_epaper_byte_mask);
  uint8_t   lo  = (uint8_t)(word & (uint16_t)k_ra8_epaper_byte_mask);
  uint8_t   rx  = 0U;
  ra8_err_t err = bus->xfer8(bus->ctx, hi, &rx);
  if (err != k_ra8_ok) {
    return err;
  }
  return bus->xfer8(bus->ctx, lo, &rx);
}

/**
 * @brief Receive one 16-bit word MSB-first.
 *
 * @param[out] out_word Receive slot; non-NULL.
 *
 * @return ``ra8_err_t`` from the underlying SPI bus.
 *
 * @pre  ``out_word`` non-NULL.
 * @post On success, ``*out_word`` contains the received word.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_recv16(uint16_t* out_word)
{
  const ra8_spi_bus_ops_t* bus = &s_panel.cfg.bus;
  uint8_t                  hi  = 0U;
  uint8_t                  lo  = 0U;
  ra8_err_t                err = bus->xfer8(bus->ctx, (uint8_t)k_ra8_epaper_dummy_tx, &hi);
  if (err != k_ra8_ok) {
    return err;
  }
  err = bus->xfer8(bus->ctx, (uint8_t)k_ra8_epaper_dummy_tx, &lo);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_word = (uint16_t)(((uint16_t)hi << (uint16_t)k_ra8_epaper_byte_shift) | (uint16_t)lo);
  return k_ra8_ok;
}

/**
 * @brief Block until the panel asserts HRDY.
 *
 * @details
 * Polls ``cfg.busy_pin`` through ``ra8_gpio_read`` with a bounded
 * retry budget. On the host unit-test build the busy pin is mmap'd
 * RAM with no panel to deassert it, so the ra8_sim_mmio fault seam owns
 * the loop-exit decision -- first-poll success unless a test arms a
 * fault on the pin's input register (PCNTR2) to drive the timeout leg.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              HRDY high.
 * @retval k_ra8_err_hw_timeout  Budget exhausted with HRDY still low.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_wait_ready(void)
{
  const ra8_port_pin_t pin = (ra8_port_pin_t)s_panel.cfg.busy_pin;
#if defined(RA8_SIMULATOR_MODE) && defined(UNIT_TEST)
  volatile const void* hrdy_key = (volatile const void*)&ra8_port(RA8_PIN_PORT(pin))->PCNTR2;
#endif
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_epaper_busy_poll_max; i++) {
    ra8_level_t level = k_ra8_level_low;
    bool        ready = false;
    if (ra8_gpio_read(pin, &level) == k_ra8_ok) {
      ready = (level == k_ra8_level_high);
    }
#if defined(RA8_SIMULATOR_MODE) && defined(UNIT_TEST)
    if (ra8_sim_mmio_wait_eval(hrdy_key, i, ready)) {
      return k_ra8_ok;
    }
#else
    if (ready) {
      return k_ra8_ok;
    }
#endif
  }
  ra8_log_error(s_tag, "HRDY poll timeout");
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Send a command code (DS chapter 3.4).
 *
 * @param[in] cmd Command word.
 *
 * @return ``ra8_err_t`` -- propagates SPI / busy-poll errors.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_write_cmd(uint16_t cmd)
{
  ra8_err_t err = internal_ra8_epaper_wait_ready();
  if (err != k_ra8_ok) {
    return err; /* GCOVR_EXCL_LINE -- redundant wait_ready-timeout re-raise; the timeout leg itself is seam-driven and tested. */
  }
  err = internal_ra8_epaper_send16((uint16_t)k_ra8_epaper_preamble_cmd);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_wait_ready();
  if (err != k_ra8_ok) {
    return err; /* GCOVR_EXCL_LINE -- redundant wait_ready-timeout re-raise; the timeout leg itself is seam-driven and tested. */
  }
  return internal_ra8_epaper_send16(cmd);
}

/**
 * @brief Send a 16-bit data word (DS chapter 3.4).
 *
 * @param[in] word Data word.
 * @return ``ra8_err_t`` error code.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_write_data16(uint16_t word)
{
  ra8_err_t err = internal_ra8_epaper_wait_ready();
  if (err != k_ra8_ok) {
    return err; /* GCOVR_EXCL_LINE -- redundant wait_ready-timeout re-raise; the timeout leg itself is seam-driven and tested. */
  }
  err = internal_ra8_epaper_send16((uint16_t)k_ra8_epaper_preamble_wr);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_wait_ready();
  if (err != k_ra8_ok) {
    return err; /* GCOVR_EXCL_LINE -- redundant wait_ready-timeout re-raise; the timeout leg itself is seam-driven and tested. */
  }
  return internal_ra8_epaper_send16(word);
}

/**
 * @brief Read a 16-bit data word (DS chapter 3.4).
 *
 * @param[out] out_word Receive slot; non-NULL.
 * @return ``ra8_err_t`` error code.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_read_data16(uint16_t* out_word)
{
  ra8_err_t err = internal_ra8_epaper_wait_ready();
  if (err != k_ra8_ok) {
    return err; /* GCOVR_EXCL_LINE -- redundant wait_ready-timeout re-raise; the timeout leg itself is seam-driven and tested. */
  }
  err = internal_ra8_epaper_send16((uint16_t)k_ra8_epaper_preamble_rd);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_wait_ready();
  if (err != k_ra8_ok) {
    return err; /* GCOVR_EXCL_LINE -- redundant wait_ready-timeout re-raise; the timeout leg itself is seam-driven and tested. */
  }
  /* IT8951 inserts one dummy word after the read preamble (DS 3.4). */
  uint16_t dummy = 0U;
  err            = internal_ra8_epaper_recv16(&dummy);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_ra8_epaper_recv16(out_word);
}

/**
 * @brief Write to an IT8951 internal register.
 *
 * @details
 * The "register write" sequence is REG_WR (0x0011) followed by two
 * data words: the register address then the value (DS 4.2.5).
 *
 * @param[in] reg   Register address.
 * @param[in] value Value to write.
 * @return ``ra8_err_t`` error code.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_reg_write(uint16_t reg, uint16_t value)
{
  ra8_err_t err = internal_ra8_epaper_write_cmd((uint16_t)k_ra8_epaper_cmd_reg_wr);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_write_data16(reg);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_ra8_epaper_write_data16(value);
}

/**
 * @brief Read from an IT8951 internal register.
 *
 * @param[in]  reg   Register address.
 * @param[out] value Receive slot; non-NULL.
 * @return ``ra8_err_t`` error code.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_reg_read(uint16_t reg, uint16_t* value)
{
  ra8_err_t err = internal_ra8_epaper_write_cmd((uint16_t)k_ra8_epaper_cmd_reg_rd);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_write_data16(reg);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_ra8_epaper_read_data16(value);
}

/**
 * @brief Pulse the panel /RESET line low for 10 ms then back high.
 *
 * @details
 * Drives ``cfg.reset_pin`` high -> low -> high through ``ra8_gpio_write``
 * with a ::k_ra8_epaper_reset_pulse_ms dwell after each edge (Waveshare
 * IT8951 user guide reset sequence). The write results are deliberately
 * discarded: the pulse runs before the panel can report anything, and a
 * mis-wired pin surfaces on the HRDY wait that immediately follows. The
 * same body runs on every build -- the host unit-test build drives the
 * RAM-backed PORT window (``ra8_delay_ms`` is a host no-op inside
 * ``ra8_core``), so tests observe the final POSR set-bit write on the
 * reset port's PCNTR3.
 *
 * @pre ``ra8_epaper_init`` copied a validated cfg into ``s_panel``.
 * @pre ``cfg.reset_pin`` addresses a mapped PORT pin.
 * @post The /RESET line is left driven high (panel out of reset).
 * @post Three reset-dwell delays have elapsed (firmware builds).
 * @note Not thread-safe; init path only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_ra8_epaper_pulse_reset(void)
{
  const ra8_port_pin_t pin = (ra8_port_pin_t)s_panel.cfg.reset_pin;
  (void)ra8_gpio_write(pin, k_ra8_level_high);
  ra8_delay_ms((uint32_t)k_ra8_epaper_reset_pulse_ms);
  (void)ra8_gpio_write(pin, k_ra8_level_low);
  ra8_delay_ms((uint32_t)k_ra8_epaper_reset_pulse_ms);
  (void)ra8_gpio_write(pin, k_ra8_level_high);
  ra8_delay_ms((uint32_t)k_ra8_epaper_reset_pulse_ms);
}

/**
 * @brief Validate the caller's waveform map against the LUT mode range.
 *
 * @details
 * A zero-initialised map would silently refresh every mode as INIT, so
 * DU / GC16 / A2 are additionally required to be non-zero. That single
 * rule is what stops a board descriptor inheriting another panel's
 * numbering by omission.
 *
 * @param[in] wf Caller-supplied waveform map.
 * @return ``k_ra8_ok`` when usable, ``k_ra8_err_invalid_arg`` otherwise.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t
internal_ra8_epaper_validate_waveform(const ra8_epaper_waveform_cfg_t* wf)
{
  if ((wf->du == 0U) || (wf->gc16 == 0U) || (wf->a2 == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if ((wf->init > (uint8_t)k_ra8_epaper_wf_mode_max) ||
      (wf->du > (uint8_t)k_ra8_epaper_wf_mode_max) ||
      (wf->gc16 > (uint8_t)k_ra8_epaper_wf_mode_max) ||
      (wf->a2 > (uint8_t)k_ra8_epaper_wf_mode_max)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate every field of an ``ra8_epaper_cfg_t``.
 *
 * @param[in] cfg Caller-supplied (already-non-NULL) config.
 * @return ``k_ra8_ok`` if every field is in range, ``k_ra8_err_invalid_arg``
 *         otherwise.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_validate_cfg(const ra8_epaper_cfg_t* cfg)
{
  if ((cfg->bus.xfer8 == nullptr) || (cfg->panel_width == 0U) || (cfg->panel_height == 0U) ||
      (cfg->panel_width > (uint16_t)k_ra8_epaper_panel_max_dim) ||
      (cfg->panel_height > (uint16_t)k_ra8_epaper_panel_max_dim)) {
    return k_ra8_err_invalid_arg;
  }
  return internal_ra8_epaper_validate_waveform(&cfg->waveform);
}

/**
 * @brief Unpack one 16-bit version word into two ASCII chars.
 *
 * @details
 * Version strings arrive packed two characters per word, high byte first.
 * Any byte outside printable ASCII is dropped to NUL so a garbled read
 * cannot inject control characters into a string the app may log.
 *
 * @param[in]     word    Source word.
 * @param[out]    dst     Destination for two chars; non-NULL.
 */
RA8_INTERNAL
static void internal_ra8_epaper_unpack_ver_word(uint16_t word, char* dst)
{
  const uint8_t hi =
    (uint8_t)((word >> (uint16_t)k_ra8_epaper_byte_shift) & (uint16_t)k_ra8_epaper_byte_mask);
  const uint8_t lo = (uint8_t)(word & (uint16_t)k_ra8_epaper_byte_mask);
  const bool    hi_p =
    (hi >= (uint8_t)k_ra8_epaper_ascii_min) && (hi < (uint8_t)k_ra8_epaper_ascii_max);
  const bool lo_p =
    (lo >= (uint8_t)k_ra8_epaper_ascii_min) && (lo < (uint8_t)k_ra8_epaper_ascii_max);
  dst[0] = hi_p ? (char)hi : '\0';
  dst[1] = lo_p ? (char)lo : '\0';
}

/**
 * @brief Fold one GET_DEV_INFO word into the decoded info block.
 *
 * @param[in]     idx  Word index within the 20-word response.
 * @param[in]     word The word just read.
 * @param[in,out] out  Info block being assembled; non-NULL.
 */
RA8_INTERNAL
static void
internal_ra8_epaper_decode_dev_word(uint32_t idx, uint16_t word, ra8_epaper_dev_info_t* out)
{
  if (idx == (uint32_t)k_ra8_epaper_dev_idx_width) {
    out->panel_width = word;
  } else if (idx == (uint32_t)k_ra8_epaper_dev_idx_height) {
    out->panel_height = word;
  } else if (idx == (uint32_t)k_ra8_epaper_dev_idx_buf_lo) {
    out->image_buf_base |= (uint32_t)word;
  } else if (idx == (uint32_t)k_ra8_epaper_dev_idx_buf_hi) {
    out->image_buf_base |= ((uint32_t)word << (uint32_t)k_ra8_epaper_word_shift);
  } else if (idx < (uint32_t)k_ra8_epaper_dev_idx_lut) {
    const uint32_t off =
      (idx - (uint32_t)k_ra8_epaper_dev_idx_fw) * (uint32_t)k_ra8_epaper_ver_per_word;
    internal_ra8_epaper_unpack_ver_word(word, &out->fw_version[off]);
  } else {
    const uint32_t off =
      (idx - (uint32_t)k_ra8_epaper_dev_idx_lut) * (uint32_t)k_ra8_epaper_ver_per_word;
    internal_ra8_epaper_unpack_ver_word(word, &out->lut_version[off]);
  }
}

/**
 * @brief Read and decode the 40-byte GET_DEV_INFO response.
 *
 * @details
 * The block is read once at init and cached in ``s_panel.info``; the
 * reported geometry is cross-checked against the configured panel size
 * and logged (not enforced) on mismatch, because a controller that has
 * not finished loading its waveform can report zeroes on the first read.
 *
 * @param[out] out Info block to populate; non-NULL.
 * @return ``ra8_err_t`` propagating SPI errors.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_read_dev_info(ra8_epaper_dev_info_t* out)
{
  *out          = (ra8_epaper_dev_info_t){};
  ra8_err_t err = internal_ra8_epaper_write_cmd((uint16_t)k_ra8_epaper_cmd_get_dev_info);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_epaper_dev_info_words; i++) {
    uint16_t word = 0U;
    err           = internal_ra8_epaper_read_data16(&word);
    if (err != k_ra8_ok) {
      return err;
    }
    internal_ra8_epaper_decode_dev_word(i, word, out);
  }
  out->fw_version[k_ra8_epaper_ver_chars]  = '\0';
  out->lut_version[k_ra8_epaper_ver_chars] = '\0';
  return k_ra8_ok;
}

/**
 * @brief Map a driver pixel-format selector onto the LD_IMG_AREA wire code.
 *
 * @details
 * The controller's ``arg0`` bitfield numbers formats 2/3/4/8 bpp as
 * 0/1/2/3. 1 bpp has no code of its own: bi-level data rides the 8 bpp
 * path with bitmap mode armed, so it maps to the 8 bpp code here.
 *
 * @param[in] pf Driver-side pixel format.
 * @return The two-bit code for ``arg0``.
 */
RA8_INTERNAL
[[nodiscard]] static uint16_t internal_ra8_epaper_wire_pf(ra8_epaper_pixel_format_t pf)
{
  switch (pf) {
    case k_ra8_epaper_pf_2bpp:
      return (uint16_t)k_ra8_epaper_wire_pf_2bpp;
    case k_ra8_epaper_pf_4bpp:
      return (uint16_t)k_ra8_epaper_wire_pf_4bpp;
    case k_ra8_epaper_pf_1bpp:
    case k_ra8_epaper_pf_8bpp:
    default:
      return (uint16_t)k_ra8_epaper_wire_pf_8bpp;
  }
}

/**
 * @brief Send the LD_IMG_AREA argument block (5 words: arg0..arg4).
 *
 * @param[in] area    Rectangle to load.
 * @param[in] pf      Source pixel depth.
 * @param[in] endian  Source-buffer endianness.
 * @return ``ra8_err_t`` error code.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_send_load_args(const ra8_epaper_area_t*  area,
                                                                  ra8_epaper_pixel_format_t pf,
                                                                  ra8_epaper_endian_t       endian)
{
  /* DS 4.2.7 LD_IMG_AREA argument layout:
   *   arg0 = (endian << 8) | (pf << 4) | rotate
   *   arg1..arg4 = x, y, width, height
   */
  const uint16_t arg0 =
    (uint16_t)(((uint16_t)endian << (uint16_t)k_ra8_epaper_byte_shift) |
               (internal_ra8_epaper_wire_pf(pf) << (uint16_t)k_ra8_epaper_pf_shift));
  ra8_err_t err = internal_ra8_epaper_write_data16(arg0);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_write_data16(area->x);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_write_data16(area->y);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_write_data16(area->width);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_ra8_epaper_write_data16(area->height);
}

/**
 * @brief Stream a packed pixel buffer into the controller as 16-bit words.
 *
 * @details
 * The IT8951 frame RAM is word-wide; pack pairs of source bytes into
 * one word. Odd tails are padded with 0xFF (white). Depth-agnostic --
 * the bytes are already packed at the caller's ``pf``, and the
 * controller unpacks them per the ``arg0`` format code.
 *
 * @param[in] buf     Source bytes; non-NULL, length ``buf_len``.
 * @param[in] buf_len Total number of bytes.
 * @return ``ra8_err_t`` error code.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_stream_pixels(const uint8_t* buf, size_t buf_len)
{
  for (size_t i = 0U; (i + 1U) < buf_len; i += 2U) {
    const uint16_t word =
      (uint16_t)(((uint16_t)buf[i] << (uint16_t)k_ra8_epaper_byte_shift) | (uint16_t)buf[i + 1U]);
    ra8_err_t err = internal_ra8_epaper_write_data16(word);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  if ((buf_len & 1U) != 0U) {
    const uint16_t word =
      (uint16_t)(((uint16_t)buf[buf_len - 1U] << (uint16_t)k_ra8_epaper_byte_shift) |
                 (uint16_t)k_ra8_epaper_white_pad);
    return internal_ra8_epaper_write_data16(word);
  }
  return k_ra8_ok;
}

/**
 * @brief Resolve a symbolic waveform selector into this panel's LUT mode.
 *
 * @details
 * The mapping is per-panel data carried in ``s_panel.cfg.waveform`` --
 * never the enumerator's own value, which is only an ordinal. An
 * unrecognised selector is refused rather than defaulted, because
 * defaulting would put an arbitrary waveform on the panel.
 *
 * @param[in]  waveform Symbolic selector.
 * @param[out] out_mode Receives the LUT mode number; non-NULL.
 * @return ``k_ra8_ok`` or ``k_ra8_err_invalid_arg`` for an unknown selector.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_resolve_waveform(ra8_epaper_waveform_t waveform,
                                                                    uint16_t*             out_mode)
{
  const ra8_epaper_waveform_cfg_t* wf = &s_panel.cfg.waveform;
  switch (waveform) {
    case k_ra8_epaper_wf_init:
      *out_mode = (uint16_t)wf->init;
      return k_ra8_ok;
    case k_ra8_epaper_wf_du:
      *out_mode = (uint16_t)wf->du;
      return k_ra8_ok;
    case k_ra8_epaper_wf_gc16:
      *out_mode = (uint16_t)wf->gc16;
      return k_ra8_ok;
    case k_ra8_epaper_wf_a2:
      *out_mode = (uint16_t)wf->a2;
      return k_ra8_ok;
    default:
      return k_ra8_err_invalid_arg;
  }
}

/**
 * @brief Send the DPY_AREA argument block (5 words: x,y,w,h,wf).
 *
 * @param[in] area     Rectangle to refresh.
 * @param[in] wf_mode  Resolved LUT mode number for this panel.
 * @return ``ra8_err_t`` error code.
 */
RA8_INTERNAL
[[nodiscard]] static ra8_err_t internal_ra8_epaper_send_display_args(const ra8_epaper_area_t* area,
                                                                     uint16_t wf_mode)
{
  ra8_err_t err = internal_ra8_epaper_write_data16(area->x);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_write_data16(area->y);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_write_data16(area->width);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_write_data16(area->height);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_ra8_epaper_write_data16(wf_mode);
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_epaper_init(const ra8_epaper_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "epaper_init: cfg null");

  if (s_panel.state != k_ra8_epaper_state_uninit) {
    ra8_log_error(s_tag, "epaper_init: already initialized");
    return k_ra8_err_invalid_state;
  }
  ra8_err_t err = internal_ra8_epaper_validate_cfg(cfg);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "epaper_init: cfg field out of range");
    return err;
  }

  s_panel.cfg = *cfg;

  internal_ra8_epaper_pulse_reset();
  err = internal_ra8_epaper_wait_ready();
  if (err != k_ra8_ok) {
    return err; /* GCOVR_EXCL_LINE -- redundant wait_ready-timeout re-raise; the timeout leg itself is seam-driven and tested. */
  }
  err = internal_ra8_epaper_write_cmd((uint16_t)k_ra8_epaper_cmd_sys_run);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_read_dev_info(&s_panel.info);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((s_panel.info.panel_width != cfg->panel_width) ||
      (s_panel.info.panel_height != cfg->panel_height)) {
    /* Reported geometry disagrees with the descriptor. Logged, not fatal:
     * a controller that has not finished loading its waveform answers the
     * first GET_DEV_INFO with zeroes, and the app may legitimately drive a
     * sub-window of a larger panel. */
    ra8_log_warn(s_tag, "epaper_init: GET_DEV_INFO geometry differs from cfg");
  }

  s_panel.state = k_ra8_epaper_state_ready;
  ra8_log_info(s_tag, "epaper_init ok");
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_epaper_dev_info(ra8_epaper_dev_info_t* out_info)
{
  RA8_CHECK_NULL_PTR(out_info, s_tag, "dev_info: out null");
  if (s_panel.state != k_ra8_epaper_state_ready) {
    return k_ra8_err_invalid_state;
  }
  *out_info = s_panel.info;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_epaper_get_vcom(uint16_t* out_mv)
{
  RA8_CHECK_NULL_PTR(out_mv, s_tag, "get_vcom: out null");
  if (s_panel.state != k_ra8_epaper_state_ready) {
    return k_ra8_err_invalid_state;
  }
  ra8_err_t err = internal_ra8_epaper_write_cmd((uint16_t)k_ra8_epaper_cmd_vcom);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_write_data16((uint16_t)k_ra8_epaper_vcom_get);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_ra8_epaper_read_data16(out_mv);
}

[[nodiscard]] ra8_err_t ra8_epaper_set_vcom(uint16_t mv)
{
  if (s_panel.state != k_ra8_epaper_state_ready) {
    return k_ra8_err_invalid_state;
  }
  if (mv == 0U) {
    ra8_log_error(s_tag, "set_vcom: refusing a zero VCOM");
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = internal_ra8_epaper_write_cmd((uint16_t)k_ra8_epaper_cmd_vcom);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_write_data16((uint16_t)k_ra8_epaper_vcom_set);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_ra8_epaper_write_data16(mv);
}

[[nodiscard]] ra8_err_t ra8_epaper_load_image(const ra8_epaper_area_t*  area,
                                              const uint8_t*            buf,
                                              size_t                    buf_len,
                                              ra8_epaper_pixel_format_t pf,
                                              ra8_epaper_endian_t       endian)
{
  RA8_CHECK_NULL_PTR(area, s_tag, "load_image: area null");
  RA8_CHECK_NULL_PTR(buf, s_tag, "load_image: buf null");

  if (s_panel.state != k_ra8_epaper_state_ready) {
    return k_ra8_err_invalid_state;
  }
  size_t          expect = 0U;
  const ra8_err_t serr   = ra8_epaper_image_bytes(area, pf, &expect);
  if (serr != k_ra8_ok) {
    return k_ra8_err_invalid_size;
  }
  if (buf_len != expect) {
    return k_ra8_err_invalid_size;
  }
  if (!ra8_epaper_area_is_aligned(area, pf)) {
    /* A 1 bpp update off the 32-pixel grid does not render at all -- the
     * controller accepts the command and shows nothing. Refuse it here so
     * the failure is a return code rather than a blank screen. */
    ra8_log_error(s_tag, "load_image: 1bpp area violates 32px X/width alignment");
    return k_ra8_err_invalid_arg;
  }

  /* DS 4.4 -- LISAR holds the 32-bit target frame address; on a
   * single-image driver we keep image base at 0 (the controller's
   * default frame buffer after SYS_RUN). */
  ra8_err_t err = internal_ra8_epaper_reg_write((uint16_t)k_ra8_epaper_reg_lisar_lo, 0U);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_reg_write((uint16_t)k_ra8_epaper_reg_lisar_hi, 0U);
  if (err != k_ra8_ok) {
    return err;
  }

  err = internal_ra8_epaper_write_cmd((uint16_t)k_ra8_epaper_cmd_ld_img_area);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_send_load_args(area, pf, endian);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_stream_pixels(buf, buf_len);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_ra8_epaper_write_cmd((uint16_t)k_ra8_epaper_cmd_ld_img_end);
}

[[nodiscard]] ra8_err_t ra8_epaper_display_area(const ra8_epaper_area_t* area,
                                                ra8_epaper_waveform_t    waveform)
{
  RA8_CHECK_NULL_PTR(area, s_tag, "display_area: area null");

  if (s_panel.state != k_ra8_epaper_state_ready) {
    return k_ra8_err_invalid_state;
  }
  uint16_t  wf_mode = 0U;
  ra8_err_t err     = internal_ra8_epaper_resolve_waveform(waveform, &wf_mode);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "display_area: unknown waveform selector");
    return err;
  }

  err = internal_ra8_epaper_write_cmd((uint16_t)k_ra8_epaper_cmd_dpy_area);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_send_display_args(area, wf_mode);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Poll LUTAFSR until the controller reports zero busy LUTs. The per-poll
   * "LUT idle" comparison is routed through the ra8_sim_mmio fault seam under the
   * host unit-test build (issue #177 / T1-01) so this real poll/timeout loop
   * executes on host instead of a compiled-out short-circuit; un-armed the seam
   * is transparent and honours the comparison. The LUTAFSR value is clocked in
   * over the injected bus, so the seam is keyed on the seam's context cookie --
   * a stable, test-addressable object the test itself bound into cfg (a stack
   * local cannot be armed). Firmware and board_sim take the plain comparison
   * path. */
#if defined(RA8_SIMULATOR_MODE) && defined(UNIT_TEST)
  /* Not a register access: the address is only used as a fault-table key. */
  volatile const void* lut_probe = (volatile const void*)s_panel.cfg.bus.ctx;
#endif
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_epaper_lut_poll_max; i++) {
    uint16_t status = (uint16_t)k_ra8_epaper_status_unset;
    err             = internal_ra8_epaper_reg_read((uint16_t)k_ra8_epaper_reg_lutafsr, &status);
    if (err != k_ra8_ok) {
      return err;
    }
#if defined(RA8_SIMULATOR_MODE) && defined(UNIT_TEST)
    if (ra8_sim_mmio_wait_eval(lut_probe, i, (status == 0U))) {
      return k_ra8_ok;
    }
#else
    if (status == 0U) {
      return k_ra8_ok;
    }
#endif
  }
  return k_ra8_err_hw_timeout;
}

[[nodiscard]] ra8_err_t ra8_epaper_sleep(void)
{
  if (s_panel.state != k_ra8_epaper_state_ready) {
    return k_ra8_err_invalid_state;
  }
  ra8_err_t err = internal_ra8_epaper_write_cmd((uint16_t)k_ra8_epaper_cmd_sleep);
  /* Whether the SLEEP command made it out or not, the controller is
   * no longer in a defined state from the driver's perspective. */
  s_panel.state = k_ra8_epaper_state_uninit;
  return err;
}
