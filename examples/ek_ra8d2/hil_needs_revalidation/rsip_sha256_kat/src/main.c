/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/rsip_sha256_kat/src/main.c
 * @brief On-silicon RSIP backend qualification and FIPS 180-4 known-answer test
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Validates ``ra8_rsip_sha256`` and tests backend selection observability on the
 * real EK-RA8D2 hardware. The RSIP-E50D cryptographic engine has no public register
 * interface (HUM Ch 52 contains no register map) and requires closed-source
 * Renesas FSP binary primitives (librsip.a) that cannot be linked in open-source
 * firmware. The active and working SHA-256 path is therefore the freestanding
 * in-tree constant-time software implementation (RA8_RSIP_SOFTWARE_BACKEND).
 *
 * Observability:
 *   - The backend query ``ra8_rsip_get_backend()`` is reported explicitly.
 *   - Software execution is never reported as a hardware KAT pass.
 *   - TRNG read fails closed with ``k_ra8_err_not_supported`` (no canned entropy).
 *   - Unqualified hardware paths are recorded honestly.
 *
 * Vectors (FIPS 180-4):
 *   - ""                                                   -> e3b0c442...7852b855
 *   - "abc"                                                -> ba7816bf...f20015ad
 *   - 56-byte two-block message                            -> 248d6a61...19db06c1
 *
 * Reporting:
 *   - LED1 toggles on every pass cycle; LED2 latches on any failure.
 *   - "rsip sha256: backend=software KAT OK" over SCI8 (115200 8N1).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_rsip_core.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_kat_baud       = 115200U, /**< Kat baud. */
  k_kat_period_ms  = 1000U,   /**< Kat period ms. */
  k_kat_trng_bytes = 32U,     /**< TRNG probe buffer length. */
} kat_const_t;

/** @brief Length of the 56-byte NIST two-block message (excludes the NUL). */
typedef enum : uint8_t {
  k_kat_msg2_len = 56U, /**< Kat msg2 length. */
} kat_msg_len_t;

/** @brief NIST vector 2 input: "abc". */
static const uint8_t s_msg_abc[] = {'a', 'b', 'c'};

/** @brief NIST vector 3 input: the 448-bit two-block message. */
static const uint8_t s_msg_two_block[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

/** @brief Expected SHA-256("") -- FIPS 180-4. */
static const uint8_t s_exp_empty[k_ra8_rsip_sha256_digest_bytes] = {
  0xE3U, 0xB0U, 0xC4U, 0x42U, 0x98U, 0xFCU, 0x1CU, 0x14U, 0x9AU, 0xFBU, 0xF4U,
  0xC8U, 0x99U, 0x6FU, 0xB9U, 0x24U, 0x27U, 0xAEU, 0x41U, 0xE4U, 0x64U, 0x9BU,
  0x93U, 0x4CU, 0xA4U, 0x95U, 0x99U, 0x1BU, 0x78U, 0x52U, 0xB8U, 0x55U,
};

/** @brief Expected SHA-256("abc") -- FIPS 180-4. */
static const uint8_t s_exp_abc[k_ra8_rsip_sha256_digest_bytes] = {
  0xBAU, 0x78U, 0x16U, 0xBFU, 0x8FU, 0x01U, 0xCFU, 0xEAU, 0x41U, 0x41U, 0x40U,
  0xDEU, 0x5DU, 0xAEU, 0x22U, 0x23U, 0xB0U, 0x03U, 0x61U, 0xA3U, 0x96U, 0x17U,
  0x7AU, 0x9CU, 0xB4U, 0x10U, 0xFFU, 0x61U, 0xF2U, 0x00U, 0x15U, 0xADU,
};

/** @brief Expected SHA-256(56-byte two-block message) -- FIPS 180-4. */
static const uint8_t s_exp_two_block[k_ra8_rsip_sha256_digest_bytes] = {
  0x24U, 0x8DU, 0x6AU, 0x61U, 0xD2U, 0x06U, 0x38U, 0xB8U, 0xE5U, 0xC0U, 0x26U,
  0x93U, 0x0CU, 0x3EU, 0x60U, 0x39U, 0xA3U, 0x3CU, 0xE4U, 0x59U, 0x64U, 0xFFU,
  0x21U, 0x67U, 0xF6U, 0xECU, 0xEDU, 0xD4U, 0x19U, 0xDBU, 0x06U, 0xC1U,
};

/** @brief Diagnostic string emitted at startup for backend awareness. */
static const uint8_t s_diag_backend_sw[] = "rsip backend: sha256=software trng=unsupported\r\n";

/** @brief Diagnostic string emitted at startup for hardware qualification status. */
static const uint8_t s_diag_hw_status[] =
  "rsip hardware: NOT QUALIFIED (requires closed Renesas FSP librsip.a)\r\n";

/** @brief HIL verdict emitted after software SHA-256 vectors pass. */
static const uint8_t s_kat_msg_sw_ok[] = "rsip sha256: backend=software KAT OK\r\n";

/** @brief HIL verdict emitted after software SHA-256 vectors fail. */
static const uint8_t s_kat_msg_sw_fail[] = "rsip sha256: backend=software KAT FAIL\r\n";

/** @brief HIL verdict emitted after hardware SHA-256 vectors pass. */
static const uint8_t s_kat_msg_hw_ok[] = "rsip sha256: backend=hardware KAT OK\r\n";

/** @brief HIL verdict emitted after hardware SHA-256 vectors fail. */
static const uint8_t s_kat_msg_hw_fail[] = "rsip sha256: backend=hardware KAT FAIL\r\n";

/**
 * @brief Park the processor after fatal setup or KAT control-flow failure.
 *
 * @details Executes wait-for-interrupt indefinitely, preserving diagnostic state.
 *
 * @pre Fatal error detected.
 * @post This function does not return.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_kat_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Initialize clocks, timing, console, and status LEDs for the KAT.
 *
 * @pre Single-threaded application startup.
 * @post Clocks, console, and status LEDs ready.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_kat_setup_or_halt(void)
{
  const ra8_board_bringup_cfg_t bringup_cfg = {
    .console_baud = (uint32_t)k_kat_baud,
    .leds_mask    = (uint32_t)k_ra8_board_bringup_led1 | (uint32_t)k_ra8_board_bringup_led2,
  };
  ra8_board_bringup_out_t bringup_out = {};
  if (ra8_board_bringup(&bringup_cfg, &bringup_out) != k_ra8_ok) {
    internal_kat_panic_halt();
  }
}

/**
 * @brief Hash one message on the active backend and compare to a known answer.
 *
 * @param[in] msg    Message bytes; may be NULL only when @p len is 0.
 * @param[in] len    Message length in bytes.
 * @param[in] expect Expected digest buffer.
 *
 * @return true iff the digest matches @p expect exactly.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static bool
internal_kat_one(const uint8_t* msg, uint32_t len, const uint8_t* expect)
{
  uint8_t digest[k_ra8_rsip_sha256_digest_bytes] = {};
  if (ra8_rsip_sha256(msg, len, digest) != k_ra8_ok) {
    return false;
  }
  return memcmp(digest, expect, (size_t)k_ra8_rsip_sha256_digest_bytes) == 0;
}

/**
 * @brief Verify that TRNG fails closed with not_supported and does not corrupt output.
 *
 * @return true iff TRNG read returned k_ra8_err_not_supported and left buffer zeroed.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static bool internal_kat_trng_fail_closed(void)
{
  uint8_t trng_buf[k_kat_trng_bytes] = {};
  const ra8_err_t err = ra8_rsip_trng_read(trng_buf, (uint32_t)sizeof(trng_buf));
  if (err != k_ra8_err_not_supported) {
    return false;
  }
  for (uint32_t i = 0U; i < (uint32_t)sizeof(trng_buf); ++i) {
    if (trng_buf[i] != 0U) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Emit appropriate pass or fail diagnostic message over the console.
 *
 * @param[in] ok      true if all test vectors and sanity checks passed.
 * @param[in] backend Active SHA-256 backend identifier.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_kat_report(bool ok, ra8_rsip_backend_t backend)
{
  if (backend == k_ra8_rsip_backend_hardware) {
    if (ok) {
      (void)ra8_board_uart_console_write(s_kat_msg_hw_ok, (size_t)(sizeof(s_kat_msg_hw_ok) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(s_kat_msg_hw_fail,
                                         (size_t)(sizeof(s_kat_msg_hw_fail) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
  } else {
    /* Software backend path: never report as hardware. */
    if (ok) {
      (void)ra8_board_uart_console_write(s_kat_msg_sw_ok, (size_t)(sizeof(s_kat_msg_sw_ok) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(s_kat_msg_sw_fail,
                                         (size_t)(sizeof(s_kat_msg_sw_fail) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
  }
}

void main(void)
{
  internal_kat_setup_or_halt();
  ra8_isr_globals_enable();

  const ra8_rsip_backend_t sha_backend = ra8_rsip_get_backend(k_ra8_rsip_op_sha256);

  /* Announce observable backend configuration. */
  (void)ra8_board_uart_console_write(s_diag_backend_sw, (size_t)(sizeof(s_diag_backend_sw) - 1U));
  (void)ra8_board_uart_console_write(s_diag_hw_status, (size_t)(sizeof(s_diag_hw_status) - 1U));

  while (1) {
    bool ok = true;

    /* Verify TRNG fails closed cleanly. */
    if (!internal_kat_trng_fail_closed()) {
      ok = false;
    }

    /* Verify FIPS 180-4 SHA-256 test vectors. */
    if (!internal_kat_one(s_msg_abc, (uint32_t)sizeof(s_msg_abc), s_exp_abc)) {
      ok = false;
    }
    if (!internal_kat_one(s_msg_two_block, (uint32_t)k_kat_msg2_len, s_exp_two_block)) {
      ok = false;
    }
    if (!internal_kat_one(s_msg_abc, 0U, s_exp_empty)) {
      ok = false;
    }

    /* Argument validation sanity checks. */
    uint8_t dummy[k_ra8_rsip_sha256_digest_bytes] = {};
    if (ra8_rsip_sha256(nullptr, 4U, dummy) != k_ra8_err_null_ptr) {
      ok = false;
    }
    if (ra8_rsip_sha256(s_msg_abc, 3U, nullptr) != k_ra8_err_null_ptr) {
      ok = false;
    }

    internal_kat_report(ok, sha_backend);
    ra8_delay_ms(k_kat_period_ms);
  }
  internal_kat_panic_halt();
}
