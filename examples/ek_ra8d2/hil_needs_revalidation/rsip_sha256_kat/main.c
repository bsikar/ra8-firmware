/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/rsip_sha256_kat/main.c
 * @brief On-silicon FIPS 180-4 known-answer test for the RSIP HASH engine
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Validates ``ra8_rsip_sha256`` -- the exact primitive ``ra8_rot`` uses to
 * re-compute an image digest on silicon -- against the published FIPS 180-4
 * NIST vectors, running on the real EK-RA8D2 hardware. ``ra8_rsip_sha256`` is
 * backed by the in-tree software SHA-256: the RSIP-E50D HASH hardware has no
 * usable register interface (HUM Ch 52 documents none) and is non-functional on
 * silicon -- this app first proved that, then the driver was fixed to route the
 * digest through software (see libs/ra8_hal/src/ra8_rsip.c). The software backend
 * is the working implementation until the FSP procedural RSIP driver is ported.
 *
 * Vectors (FIPS 180-4):
 *   - ""                                                   -> e3b0c442...7852b855
 *   - "abc"                                                -> ba7816bf...f20015ad
 *   - 56-byte two-block message                            -> 248d6a61...19db06c1
 *
 * Reporting:
 *   - LED1 toggles on every all-vectors-pass cycle; LED2 latches on any failure.
 *   - "rsip sha256: KAT OK" / "rsip sha256: KAT FAIL" over SCI8 (115200 8N1,
 *     J-Link OB CDC), scraped by the HIL rig (hil.conf).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_rsip_core.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_kat_baud      = 115200U, /**< Kat baud.      */
  k_kat_period_ms = 1000U,   /**< Kat period ms. */
} kat_const_t;

/** @brief Length of the 56-byte NIST two-block message (excludes the NUL). */
typedef enum : uint8_t {
  k_kat_msg2_len = 56U, /**< Kat msg2 length. */
} kat_msg_len_t;

/** @brief NIST vector 2 input: "abc". */
static const uint8_t k_msg_abc[] = {'a', 'b', 'c'};

/** @brief NIST vector 3 input: the 448-bit two-block message. */
static const uint8_t k_msg_two_block[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

/** @brief Expected SHA-256("") -- FIPS 180-4. */
static const uint8_t k_exp_empty[k_ra8_rsip_sha256_digest_bytes] = {
  0xE3U, 0xB0U, 0xC4U, 0x42U, 0x98U, 0xFCU, 0x1CU, 0x14U, 0x9AU, 0xFBU, 0xF4U,
  0xC8U, 0x99U, 0x6FU, 0xB9U, 0x24U, 0x27U, 0xAEU, 0x41U, 0xE4U, 0x64U, 0x9BU,
  0x93U, 0x4CU, 0xA4U, 0x95U, 0x99U, 0x1BU, 0x78U, 0x52U, 0xB8U, 0x55U,
};

/** @brief Expected SHA-256("abc") -- FIPS 180-4. */
static const uint8_t k_exp_abc[k_ra8_rsip_sha256_digest_bytes] = {
  0xBAU, 0x78U, 0x16U, 0xBFU, 0x8FU, 0x01U, 0xCFU, 0xEAU, 0x41U, 0x41U, 0x40U,
  0xDEU, 0x5DU, 0xAEU, 0x22U, 0x23U, 0xB0U, 0x03U, 0x61U, 0xA3U, 0x96U, 0x17U,
  0x7AU, 0x9CU, 0xB4U, 0x10U, 0xFFU, 0x61U, 0xF2U, 0x00U, 0x15U, 0xADU,
};

/** @brief Expected SHA-256(56-byte two-block message) -- FIPS 180-4. */
static const uint8_t k_exp_two_block[k_ra8_rsip_sha256_digest_bytes] = {
  0x24U, 0x8DU, 0x6AU, 0x61U, 0xD2U, 0x06U, 0x38U, 0xB8U, 0xE5U, 0xC0U, 0x26U,
  0x93U, 0x0CU, 0x3EU, 0x60U, 0x39U, 0xA3U, 0x3CU, 0xE4U, 0x59U, 0x64U, 0xFFU,
  0x21U, 0x67U, 0xF6U, 0xECU, 0xEDU, 0xD4U, 0x19U, 0xDBU, 0x06U, 0xC1U,
};

static const uint8_t k_kat_msg_ok[]   = "rsip sha256: KAT OK\r\n";
static const uint8_t k_kat_msg_fail[] = "rsip sha256: KAT FAIL\r\n";

static void kat_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void kat_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    kat_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    kat_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    kat_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_kat_baud) != k_ra8_ok) {
    kat_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    kat_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    kat_panic_halt();
  }
  /* ra8_rsip_sha256 uses the software SHA-256 backend (the RSIP HASH hardware has
   * no usable register interface -- see libs/ra8_hal/src/ra8_rsip.c), so no RSIP
   * engine bring-up is required. */
}

/**
 * @brief Hash one message on the RSIP engine and compare to a known answer.
 *
 * @param[in] msg    Message bytes; may be NULL only when @p len is 0.
 * @param[in] len    Message length in bytes.
 * @param[in] expect ::k_ra8_rsip_sha256_digest_bytes expected digest; non-NULL.
 *
 * @return true iff the engine digest matches @p expect exactly.
 *
 * @pre @p expect addresses ::k_ra8_rsip_sha256_digest_bytes readable bytes.
 * @pre The RSIP engine is initialised (::ra8_rsip_init returned success).
 * @post No global state is mutated.
 * @post The transient digest buffer leaves scope on return.
 * @note Single condition -- no compound decision, so no MC/DC obligation.
 * @since 0.1.0
 */
[[nodiscard]] static bool kat_one(const uint8_t* msg, uint32_t len, const uint8_t* expect)
{
  uint8_t digest[k_ra8_rsip_sha256_digest_bytes] = {};
  if (ra8_rsip_sha256(msg, len, digest) != k_ra8_ok) {
    return false;
  }
  return memcmp(digest, expect, (size_t)k_ra8_rsip_sha256_digest_bytes) == 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  kat_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    bool ok = true;
    if (!kat_one(k_msg_abc, (uint32_t)sizeof(k_msg_abc), k_exp_abc)) {
      ok = false;
    }
    if (!kat_one(k_msg_two_block, (uint32_t)k_kat_msg2_len, k_exp_two_block)) {
      ok = false;
    }
    if (!kat_one(k_msg_abc, 0U, k_exp_empty)) { /* zero-length: msg ignored */
      ok = false;
    }

    if (ok) {
      (void)ra8_board_uart_console_write(k_kat_msg_ok, (size_t)(sizeof(k_kat_msg_ok) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_kat_msg_fail, (size_t)(sizeof(k_kat_msg_fail) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ra8_delay_ms(k_kat_period_ms);
  }
  kat_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
