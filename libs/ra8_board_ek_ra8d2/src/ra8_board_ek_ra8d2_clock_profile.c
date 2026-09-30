/**
 * @file ra8_board_ek_ra8d2_clock_profile.c
 * @brief The EK-RA8D2 wiring table behind ra8_board_ek_ra8d2_clock_profile.h.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * One table, one translation, one ops struct that delegates. No register
 * header is included here on purpose: everything chip-specific already lives
 * behind ``fw_if_clock_ra8.h``, so this file stays a statement about a board
 * and can be read against the user's manual without a datasheet open.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_board_ek_ra8d2_clock_profile.h"

#include <stddef.h>
#include <stdint.h>

#include "fw_if_clock.h"
#include "fw_if_clock_ra8.h"
#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_board_ek_ra8d2_peripherals.h"
#include "ra8_err.h"

/** @brief Longest wired run on this board, which is the four SCI channels. */
#define INTERNAL_MAX_WIRED 4U

/**
 * @brief One kind's wiring: the chip instances this board routes, in board order.
 *
 * @details
 * Board index is the subscript, chip instance is the value. Held as an explicit
 * list rather than a base and a stride because the wired channels are scattered
 * (SCI 0, 2, 7, 8) and no arithmetic reproduces them.
 */
typedef struct internal_wiring_s {
  /** @brief Instances this board wires. */
  uint8_t count;
  /** @brief Board index -> chip instance, subscripted by board index. */
  uint8_t chip_index[INTERNAL_MAX_WIRED];
} internal_wiring_t;

/**
 * @brief What the EK-RA8D2 v1 routes, per module kind.
 *
 * @details
 * Each wired row takes its chip instance from the board constant that already
 * names that channel, rather than repeating the number. A connector that moves
 * moves here with it, and the two cannot silently disagree:
 *
 * - uart 0..3 -> SCI 0, 2, 7, 8. Pmod2 J25 is SCI0 and Pmod1 J26 is SCI2
 *   (``ra8_board_ek_ra8d2_connectors.h`` ``k_ra8_board_pmod2_sci_channel`` /
 *   ``k_ra8_board_pmod1_sci_channel``); mikroBUS is SCI7
 *   (``k_ra8_board_mikrobus_uart_sci_channel``); the J-Link OB VCOM console is
 *   SCI8 (``ra8_board_ek_ra8d2_peripherals.h``
 *   ``k_ra8_board_uart_console_sci_channel``, marked verified on real silicon).
 *   Ordered by chip instance so the numbering is stable as connectors are
 *   added, not by any notion of importance.
 * - i2c 0 -> IIC1. The J35 camera SCCB bus is RIIC1
 *   (``k_ra8_board_camera_i2c_channel``). The touch panel bus is deliberately
 *   not a second row: it is I3C in I2C-compatible mode
 *   (``ra8_board_ek_ra8d2_touch.c`` calls ``ra8_i3c_init``), a different block
 *   with its own module-stop bit, so presenting it as I2C 1 would gate the
 *   wrong peripheral.
 * - sdhost 0 -> SDHI0 (``k_ra8_board_sdhi_instance``).
 * - camera 0 -> CEU, display 0 -> GLCDC, ethernet 0 -> ESWM. Single-instance
 *   blocks, so board and chip numbering agree; the rows exist so the profile
 *   answers for them rather than refusing.
 * - core 0 and memory 0 pass through unchanged. Neither is wired in the
 *   connector sense; both are simply present.
 *
 * Absent on purpose, each because this board routes none: spi (both Pmods take
 * their SPI from SCI in Simple-SPI mode, not from RSPI), can (no transceiver
 * and no connector), adc, dac, usb, timer, pwm, dma, rtc, watchdog, crypto.
 * The chip binding can still answer for several of those; going through the
 * board says only what the board can reach.
 */
static const internal_wiring_t k_internal_wiring[K_FW_CLOCK_MODULE_KIND_COUNT] = {
  [k_fw_clock_module_core] = {.count = 1U, .chip_index = {0U}},
  [k_fw_clock_module_uart] =
    {.count      = 4U,
     .chip_index = {(uint8_t)k_ra8_board_pmod2_sci_channel,
                    (uint8_t)k_ra8_board_pmod1_sci_channel,
                    (uint8_t)k_ra8_board_mikrobus_uart_sci_channel,
                    (uint8_t)k_ra8_board_uart_console_sci_channel}},
  [k_fw_clock_module_i2c] =
    {.count = 1U, .chip_index = {(uint8_t)k_ra8_board_camera_i2c_channel}},
  [k_fw_clock_module_sdhost] =
    {.count = 1U, .chip_index = {(uint8_t)k_ra8_board_sdhi_instance}},
  [k_fw_clock_module_camera]   = {.count = 1U, .chip_index = {0U}},
  [k_fw_clock_module_display]  = {.count = 1U, .chip_index = {0U}},
  [k_fw_clock_module_ethernet] = {.count = 1U, .chip_index = {0U}},
  [k_fw_clock_module_memory]   = {.count = 1U, .chip_index = {0U}},
};

/**
 * @brief Whether @p kind is a real subscript into the wiring table.
 * @param[in] kind Module kind.
 * @return True when in range.
 */
static bool internal_kind_valid(fw_clock_module_kind_t kind)
{
  return (uint32_t)kind < (uint32_t)K_FW_CLOCK_MODULE_KIND_COUNT;
}

ra8_err_t ra8_board_clock_profile_to_chip(fw_clock_module_t module, fw_clock_module_t* out_chip)
{
  if (out_chip == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_kind_valid(module.kind)) {
    return k_ra8_err_not_found;
  }
  const internal_wiring_t row = k_internal_wiring[module.kind];
  if (module.index >= row.count) {
    return k_ra8_err_not_found;
  }
  *out_chip = (fw_clock_module_t){
    .kind  = module.kind,
    .index = row.chip_index[module.index],
  };
  return k_ra8_ok;
}

uint8_t ra8_board_clock_profile_count(fw_clock_module_kind_t kind)
{
  if (!internal_kind_valid(kind)) {
    return 0U;
  }
  return k_internal_wiring[kind].count;
}

/**
 * @brief Read the rate feeding a board-numbered module.
 * @param[in]  ctx     Unused; the profile is stateless.
 * @param[in]  module  Board-numbered module.
 * @param[out] out_hz  Rate in hertz.
 * @return Chip binding status, or ::k_ra8_err_not_found when unwired.
 */
static ra8_err_t internal_rate_for(void* ctx, fw_clock_module_t module, uint32_t* out_hz)
{
  (void)ctx;
  fw_clock_module_t chip = {};
  const ra8_err_t   err  = ra8_board_clock_profile_to_chip(module, &chip);
  if (err != k_ra8_ok) {
    return err;
  }
  return fw_clock_ra8_iface()->rate_for(nullptr, chip, out_hz);
}

/**
 * @brief Gate or release a board-numbered module.
 * @param[in] ctx    Unused; the profile is stateless.
 * @param[in] module Board-numbered module.
 * @param[in] on     True to enable, false to release.
 * @return Chip binding status, or ::k_ra8_err_not_found when unwired.
 */
static ra8_err_t internal_set_gate(void* ctx, fw_clock_module_t module, bool on)
{
  (void)ctx;
  fw_clock_module_t chip = {};
  const ra8_err_t   err  = ra8_board_clock_profile_to_chip(module, &chip);
  if (err != k_ra8_ok) {
    return err;
  }
  return fw_clock_ra8_iface()->set_gate(nullptr, chip, on);
}

/**
 * @brief Whether this board wires @p module.
 * @details Answers from the wiring table alone, so "present" means the board
 *          routes it, not that the chip binding happens to have a row. An
 *          unwired module is a false answer, not an error: the caller asked a
 *          question and got one.
 * @param[in]  ctx         Unused; the profile is stateless.
 * @param[in]  module      Board-numbered module.
 * @param[out] out_present True when wired.
 * @return Query status.
 * @retval k_ra8_ok @p out_present is filled.
 * @retval k_ra8_err_invalid_arg @p out_present is null.
 */
static ra8_err_t internal_has_module(void* ctx, fw_clock_module_t module, bool* out_present)
{
  (void)ctx;
  if (out_present == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  fw_clock_module_t chip = {};
  *out_present           = ra8_board_clock_profile_to_chip(module, &chip) == k_ra8_ok;
  return k_ra8_ok;
}

/** @brief The profile's ops, filled once at compile time. */
static const fw_clock_iface_t k_internal_iface = {
  .rate_for   = internal_rate_for,
  .set_gate   = internal_set_gate,
  .has_module = internal_has_module,
};

ra8_err_t ra8_board_clock_profile_bind(fw_clock_t* clk)
{
  return fw_clock_bind(clk, &k_internal_iface, nullptr);
}

/** @brief The board's one handle, bound on first acquisition. */
static fw_clock_t internal_board_clock = {};

const fw_clock_t* ra8_board_clock(void)
{
  if (!internal_board_clock.bound) {
    /* Cannot fail: the argument is this file-static, and a non-null handle with
     * a fully populated ops struct is the only thing fw_clock_bind checks. The
     * status is discarded deliberately rather than surfaced, because there is
     * no failure for a caller to handle and a handle-returning accessor that
     * could answer NULL would make every call site carry a dead branch. */
    (void)ra8_board_clock_profile_bind(&internal_board_clock);
  }
  return &internal_board_clock;
}
