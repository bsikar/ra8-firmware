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
  k_ra8_epaper_cmd_get_dev_info = 0x0302U, /**< 40-byte info block.     */
} ra8_epaper_cmd_t;

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
} ra8_epaper_limits_t;

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
  ra8_epaper_cfg_t   cfg;   /**< Copy of init cfg.        */
  ra8_epaper_state_t state; /**< Current lifecycle state. */
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
 * @brief Validate every field of an ``ra8_epaper_cfg_t``.
 *
 * @param[in] cfg Caller-supplied (already-non-NULL) config.
 * @return ``k_ra8_ok`` if every field is in range, ``k_ra8_err_invalid_arg``
 *         otherwise.
 */
[[nodiscard]] static ra8_err_t internal_ra8_epaper_validate_cfg(const ra8_epaper_cfg_t* cfg)
{
  if ((cfg->bus.xfer8 == nullptr) || (cfg->panel_width == 0U) || (cfg->panel_height == 0U) ||
      (cfg->panel_width > (uint16_t)k_ra8_epaper_panel_max_dim) ||
      (cfg->panel_height > (uint16_t)k_ra8_epaper_panel_max_dim)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Drain the 40-byte GET_DEV_INFO response.
 *
 * @return ``ra8_err_t`` propagating SPI errors.
 */
[[nodiscard]] static ra8_err_t internal_ra8_epaper_drain_dev_info(void)
{
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
    (void)word;
  }
  return k_ra8_ok;
}

/**
 * @brief Send the LD_IMG_AREA argument block (5 words: arg0..arg4).
 *
 * @param[in] area    Rectangle to load.
 * @param[in] endian  Source-buffer endianness.
 * @return ``ra8_err_t`` error code.
 */
[[nodiscard]] static ra8_err_t internal_ra8_epaper_send_load_args(const ra8_epaper_area_t* area,
                                                                  ra8_epaper_endian_t      endian)
{
  /* DS 4.2.7 LD_IMG_AREA argument layout:
   *   arg0 = (endian << 8) | (pf << 4) | rotate
   *   arg1..arg4 = x, y, width, height
   */
  const uint16_t arg0 =
    (uint16_t)(((uint16_t)endian << (uint16_t)k_ra8_epaper_byte_shift) |
               ((uint16_t)k_ra8_epaper_pf_8bpp << (uint16_t)k_ra8_epaper_pf_shift));
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
 * @brief Stream an 8 bpp buffer into the controller as 16-bit words.
 *
 * @details
 * The IT8951 frame RAM is word-wide; pack pairs of source bytes into
 * one word. Odd tails are padded with 0xFF (white).
 *
 * @param[in] buf     Source bytes; non-NULL, length ``buf_len``.
 * @param[in] buf_len Total number of bytes.
 * @return ``ra8_err_t`` error code.
 */
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
 * @brief Send the DPY_AREA argument block (5 words: x,y,w,h,wf).
 *
 * @param[in] area     Rectangle to refresh.
 * @param[in] waveform Waveform mode.
 * @return ``ra8_err_t`` error code.
 */
[[nodiscard]] static ra8_err_t internal_ra8_epaper_send_display_args(const ra8_epaper_area_t* area,
                                                                     ra8_epaper_waveform_t waveform)
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
  return internal_ra8_epaper_write_data16((uint16_t)waveform);
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
  err = internal_ra8_epaper_drain_dev_info();
  if (err != k_ra8_ok) {
    return err;
  }

  s_panel.state = k_ra8_epaper_state_ready;
  ra8_log_info(s_tag, "epaper_init ok");
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_epaper_load_image(const ra8_epaper_area_t* area,
                                              const uint8_t*           buf,
                                              size_t                   buf_len,
                                              ra8_epaper_endian_t      endian)
{
  RA8_CHECK_NULL_PTR(area, s_tag, "load_image: area null");
  RA8_CHECK_NULL_PTR(buf, s_tag, "load_image: buf null");

  if (s_panel.state != k_ra8_epaper_state_ready) {
    return k_ra8_err_invalid_state;
  }
  const size_t expect = (size_t)area->width * (size_t)area->height;
  if ((buf_len != expect) || (expect == 0U)) {
    return k_ra8_err_invalid_size;
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
  err = internal_ra8_epaper_send_load_args(area, endian);
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

  ra8_err_t err = internal_ra8_epaper_write_cmd((uint16_t)k_ra8_epaper_cmd_dpy_area);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_ra8_epaper_send_display_args(area, waveform);
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
