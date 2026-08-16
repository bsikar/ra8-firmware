/**
 * @file examples/ek_ra8d2/hw_validated/hil/crc_demo/main.c
 * @brief CRC-32 hardware vs. software cross-check demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + SCI8 + LED1 + the on-chip CRC unit
 * (CRC-32 / IEEE 802.3 polynomial). Once a second computes the CRC
 * over a fixed 16-byte test buffer two ways:
 *
 *   1. Through ``ra8_crc_compute`` (hardware CRC engine).
 *   2. Through a tiny software reference implementation
 *      (bit-serial CRC-32 with the standard 0xEDB88320 reflected
 *      polynomial).
 *
 * Emits ``"crc: hw=XXXXXXXX sw=XXXXXXXX match=Y\r\n"`` on the
 * J-Link OB CDC channel and toggles LED1 on every match. On a match
 * it also emits the fixed verdict line ``"crc: selftest PASS\r\n"``
 * (success-only -- never printed on the match=N path), which the HIL
 * scrape keys on. LED2 latches if the two ever diverge.
 *
 * Bare EK-RA8D2 only -- no shields or external transceiver.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_crc.h"
#include "ra8_crc_regs.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_crc_demo_baud      = 115200U, /**< CRC demo baud.      */
  k_crc_demo_period_ms = 1000U,   /**< CRC demo period ms. */
} crc_demo_config_t;

/** @brief Software CRC-32 polynomial + bit constants (reflected). */
typedef enum : uint32_t {
  k_crc_demo_sw_poly_rev = 0xEDB88320UL, /**< CRC demo sw poly rev. */
  k_crc_demo_sw_init     = 0xFFFFFFFFUL, /**< CRC demo sw init.     */
  k_crc_demo_sw_xor_out  = 0xFFFFFFFFUL, /**< CRC demo sw xor out.  */
  k_crc_demo_byte_mask   = 0xFFUL,       /**< CRC demo byte mask.   */
  k_crc_demo_lsb_mask    = 0x1UL,        /**< CRC demo lsb mask.    */
} crc_demo_sw_const_t;

/** @brief Inner-loop bit count for the software CRC. */
typedef enum : uint8_t {
  k_crc_demo_bits_per_byte = 8U,    /**< CRC demo bits per byte.  */
  k_crc_demo_nibble_mask   = 0x0FU, /**< CRC demo nibble mask.    */
  k_crc_demo_nibble_shift  = 4U,    /**< CRC demo nibble shift.   */
  k_crc_demo_alpha_thresh  = 10U,   /**< CRC demo alpha thresh.   */
  k_crc_demo_payload_len   = 16U,   /**< CRC demo payload length. */
  k_crc_demo_hex_per_word  = 8U,    /**< CRC demo hex per word.   */
} crc_demo_byte_t;

/** @brief Fixed 16-byte test payload. */
static const uint8_t k_crc_demo_payload[k_crc_demo_payload_len] = {
  0x00U,
  0x11U,
  0x22U,
  0x33U,
  0x44U,
  0x55U,
  0x66U,
  0x77U,
  0x88U,
  0x99U,
  0xAAU,
  0xBBU,
  0xCCU,
  0xDDU,
  0xEEU,
  0xFFU,
};

/** @brief Output line tags. */
static const uint8_t k_crc_demo_prefix[]  = "crc: hw=";
static const uint8_t k_crc_demo_sw_tag[]  = " sw=";
static const uint8_t k_crc_demo_ok_tag[]  = " match=Y\r\n";
static const uint8_t k_crc_demo_bad_tag[] = " match=N\r\n";

/** @brief Self-test verdict emitted only on a hw==sw match (success-only). */
static const uint8_t k_crc_demo_pass_msg[] = "crc: selftest PASS\r\n";

/** @brief Park the CPU forever after a fatal init failure. */
static void crc_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Convert the low nibble of ``n`` to its ASCII hex char.
 *
 * @param[in] nibble Lower 4 bits used; upper 4 bits ignored.
 * @return Printable ASCII byte in '0'..'9' / 'a'..'f'.
 *
 * @pre None.
 * @post Return value is always printable.
 *
 * @since 0.1.0
 */
static uint8_t crc_demo_nibble_to_hex(uint8_t nibble)
{
  uint8_t n = (uint8_t)(nibble & (uint8_t)k_crc_demo_nibble_mask);
  if (n < (uint8_t)k_crc_demo_alpha_thresh) {
    return (uint8_t)('0' + n);
  }
  return (uint8_t)('a' + (n - (uint8_t)k_crc_demo_alpha_thresh));
}

/**
 * @brief Render a 32-bit value as 8 ASCII hex chars (big-endian).
 *
 * @param[in]  v   Value to render.
 * @param[out] dst 8-byte buffer that receives the hex characters.
 *
 * @pre ``dst`` is non-NULL and has at least 8 bytes capacity.
 * @post ``dst[0..7]`` contains printable hex.
 *
 * @since 0.1.0
 */
static void crc_demo_word_to_hex(uint32_t v, uint8_t* dst)
{
  for (uint8_t i = 0U; i < (uint8_t)k_crc_demo_hex_per_word; ++i) {
    const uint8_t shift =
      (uint8_t)(((uint8_t)k_crc_demo_hex_per_word - 1U - i) * (uint8_t)k_crc_demo_nibble_shift);
    const uint8_t nibble = (uint8_t)((v >> shift) & (uint32_t)k_crc_demo_nibble_mask);
    dst[i]               = crc_demo_nibble_to_hex(nibble);
  }
}

/**
 * @brief Software reference CRC-32 (IEEE 802.3, reflected).
 *
 * @details
 * Bit-serial Sarwate-style implementation; no lookup table so we
 * keep the code review-able and avoid pulling in 1 KB of constants
 * for a HIL test app. Matches the hardware CRC unit when the
 * latter is configured for ``k_ra8_crc_poly_32_ieee802_3`` with
 * the standard preset / xor-out.
 *
 * @par MC/DC:
 * Compound decision in the inner loop: ``(crc & 1U) != 0``. One
 * atomic condition x 2 vectors -- both branches are exercised by
 * any non-trivial input (host integration test covers this).
 *
 * @param[in] data Pointer to ``len`` bytes (non-NULL).
 * @param[in] len  Byte count.
 * @return 32-bit CRC-32 result.
 *
 * @pre ``data`` is non-NULL.
 * @post Return value matches the hardware engine for the same
 *       polynomial selector.
 *
 * @since 0.1.0
 */
static uint32_t crc_demo_sw_crc32(const uint8_t* data, uint32_t len)
{
  uint32_t crc = (uint32_t)k_crc_demo_sw_init;
  for (uint32_t i = 0U; i < len; ++i) {
    crc ^= (uint32_t)data[i];
    for (uint8_t b = 0U; b < (uint8_t)k_crc_demo_bits_per_byte; ++b) {
      if ((crc & (uint32_t)k_crc_demo_lsb_mask) != 0U) {
        crc = (crc >> 1) ^ (uint32_t)k_crc_demo_sw_poly_rev;
      } else {
        crc = (crc >> 1);
      }
    }
  }
  return crc ^ (uint32_t)k_crc_demo_sw_xor_out;
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + CRC unit up. */
static void crc_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    crc_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    crc_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    crc_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_crc_demo_baud) != k_ra8_ok) {
    crc_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    crc_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    crc_demo_panic_halt();
  }
  if (ra8_crc_init(k_ra8_crc_poly_32_ieee802_3) != k_ra8_ok) {
    crc_demo_panic_halt();
  }
}

/**
 * @brief One iteration: compute hw + sw CRC and emit the comparison line.
 *
 * @par MC/DC:
 * Single atomic decision: ``ra8_crc_compute(...) != k_ra8_ok``. One
 * condition x 2 vectors -- the steady-state success path plus the
 * compute-failure branch (covered by the host integration test). The
 * console writes are fire-and-forget so they add no decision.
 *
 * @param[out] out_match Receives 1 if hw == sw, 0 otherwise.
 * @return Error code from the CRC compute primitive.
 * @retval k_ra8_ok Comparison line emitted; ``*out_match`` valid.
 * @retval k_ra8_err_hw_error Hardware CRC compute failed.
 *
 * @pre ``out_match`` non-NULL.
 * @post On success ``*out_match`` is 0 or 1.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t crc_demo_one_iter(uint8_t* out_match)
{
  uint32_t hw = 0U;
  ra8_crc_reset();
  if (ra8_crc_compute(k_crc_demo_payload, (uint32_t)k_crc_demo_payload_len, &hw) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  const uint32_t sw = crc_demo_sw_crc32(k_crc_demo_payload, (uint32_t)k_crc_demo_payload_len);

  uint8_t hwhex[k_crc_demo_hex_per_word] = {};
  uint8_t swhex[k_crc_demo_hex_per_word] = {};
  crc_demo_word_to_hex(hw, hwhex);
  crc_demo_word_to_hex(sw, swhex);

  (void)ra8_board_uart_console_write(k_crc_demo_prefix, (size_t)(sizeof(k_crc_demo_prefix) - 1U));
  (void)ra8_board_uart_console_write(hwhex, (size_t)k_crc_demo_hex_per_word);
  (void)ra8_board_uart_console_write(k_crc_demo_sw_tag, (size_t)(sizeof(k_crc_demo_sw_tag) - 1U));
  (void)ra8_board_uart_console_write(swhex, (size_t)k_crc_demo_hex_per_word);
  *out_match = (hw == sw) ? 1U : 0U;
  if (*out_match != 0U) {
    (void)ra8_board_uart_console_write(k_crc_demo_ok_tag, (size_t)(sizeof(k_crc_demo_ok_tag) - 1U));
    (void)ra8_board_uart_console_write(k_crc_demo_pass_msg,
                                       (size_t)(sizeof(k_crc_demo_pass_msg) - 1U));
    return k_ra8_ok;
  }
  (void)ra8_board_uart_console_write(k_crc_demo_bad_tag, (size_t)(sizeof(k_crc_demo_bad_tag) - 1U));
  return k_ra8_ok;
}

void main(void)
{
  crc_demo_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    uint8_t         match = 0U;
    const ra8_err_t err   = crc_demo_one_iter(&match);
    if (err != k_ra8_ok) {
      break;
    }
    if (match != 0U) {
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ra8_delay_ms(k_crc_demo_period_ms);
  }
  crc_demo_panic_halt();
}
