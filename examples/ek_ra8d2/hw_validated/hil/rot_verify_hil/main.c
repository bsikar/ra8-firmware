/**
 * @file examples/ek_ra8d2/hw_validated/hil/rot_verify_hil/main.c
 * @brief On-silicon root-of-trust image verify (ECDSA-P256 via tf-psa-crypto)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Runs the REAL ``ra_rot`` verify chain on the M85 against a signed image
 * fixture (``rot_fixture.h``, produced by tools/rot_sign.py with the project RoT
 * key): SHA-256 of the body, the anti-rollback version-bind hash, and an
 * ECDSA-P256 signature check via the tf-psa-crypto software backend (the RSIP
 * crypto hardware is dead -- see libs/ra_hal/src/ra_rsip.c). It proves a working
 * root of trust can authenticate images on this hardware, and that a tampered
 * body is rejected. This is a self-test: it does NOT touch the boot path, so
 * there is no brick risk.
 *
 * Reports "rot verify: PASS" (genuine accepted AND tamper rejected) or
 * "rot verify: FAIL" over SCI8 (115200 8N1).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "mbedtls/memory_buffer_alloc.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_psa_crypto.h"
#include "ra_rot.h"
#include "ra_time.h"
#include "src/rot_fixture.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_rot_baud       = 115200U,
  k_rot_period_ms  = 1000U,
  k_rot_heap_bytes = 0x10000U, /**< 64 KiB static heap for tf-psa mbedtls_calloc. */
} rot_const_t;

/** @brief Byte flipped in the tamper copy (first body octet). */
typedef enum : uint8_t {
  k_rot_tamper_bit = 0x01U,
} rot_bit_t;

static const uint8_t k_rot_msg_pass[]      = "rot verify: PASS\r\n";
static const uint8_t k_rot_msg_fail[]      = "rot verify: FAIL\r\n";
static const uint8_t k_rot_diag_boot[]     = "rot: boot (console up)\r\n";
static const uint8_t k_rot_diag_psa_fail[] = "rot: psa_init FAIL\r\n";
static const uint8_t k_rot_diag_psa_ok[]   = "rot: psa_init ok\r\n";
static const uint8_t k_rot_diag_genuine[]  = "rot: genuine image ACCEPTED\r\n";
static const uint8_t k_rot_diag_no_gen[]   = "rot: genuine image REJECTED (bug)\r\n";
static const uint8_t k_rot_diag_tamper[]   = "rot: tampered image REJECTED\r\n";
static const uint8_t k_rot_diag_no_tamp[]  = "rot: tampered image ACCEPTED (bug)\r\n";

/** @brief Static heap tf-psa's mbedtls_calloc draws from (no libc heap on target). */
static uint8_t s_rot_heap[k_rot_heap_bytes];

/** @brief Mutable copy of the fixture used for the tamper case. */
static uint8_t s_rot_tamper[sizeof(k_rot_fixture_image)];

static void rot_write(const uint8_t* msg, size_t len)
{
  (void)ra_board_uart_console_write(msg, len);
}

/** @brief The genuine signed fixture must verify (k_ra_ok). */
static bool rot_genuine_ok(void)
{
  const ra_rot_trailer_t* trailer =
    ra_rot_trailer_after(k_rot_fixture_image, k_rot_fixture_body_len);
  if (trailer == nullptr) {
    return false;
  }
  return ra_rot_verify_image(k_rot_fixture_image, k_rot_fixture_body_len, trailer) == k_ra_ok;
}

/** @brief A one-bit body corruption must be rejected (not k_ra_ok). */
static bool rot_tamper_rejected(void)
{
  memcpy(s_rot_tamper, k_rot_fixture_image, sizeof(s_rot_tamper));
  s_rot_tamper[0] ^= (uint8_t)k_rot_tamper_bit;
  const ra_rot_trailer_t* trailer = ra_rot_trailer_after(s_rot_tamper, k_rot_fixture_body_len);
  if (trailer == nullptr) {
    return false;
  }
  return ra_rot_verify_image(s_rot_tamper, k_rot_fixture_body_len, trailer) != k_ra_ok;
}

static void rot_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void rot_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    rot_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    rot_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    rot_panic_halt();
  }
  if (ra_board_uart_console_init((uint32_t)k_rot_baud) != k_ra_ok) {
    rot_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    rot_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    rot_panic_halt();
  }
  rot_write(k_rot_diag_boot, (size_t)(sizeof(k_rot_diag_boot) - 1U));
  mbedtls_memory_buffer_alloc_init(s_rot_heap, sizeof(s_rot_heap));
  if (ra_psa_crypto_init() != k_ra_ok) {
    rot_write(k_rot_diag_psa_fail, (size_t)(sizeof(k_rot_diag_psa_fail) - 1U));
    rot_panic_halt();
  }
  rot_write(k_rot_diag_psa_ok, (size_t)(sizeof(k_rot_diag_psa_ok) - 1U));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  rot_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    bool ok = true;

    if (rot_genuine_ok()) {
      rot_write(k_rot_diag_genuine, (size_t)(sizeof(k_rot_diag_genuine) - 1U));
    } else {
      rot_write(k_rot_diag_no_gen, (size_t)(sizeof(k_rot_diag_no_gen) - 1U));
      ok = false;
    }

    if (rot_tamper_rejected()) {
      rot_write(k_rot_diag_tamper, (size_t)(sizeof(k_rot_diag_tamper) - 1U));
    } else {
      rot_write(k_rot_diag_no_tamp, (size_t)(sizeof(k_rot_diag_no_tamp) - 1U));
      ok = false;
    }

    if (ok) {
      rot_write(k_rot_msg_pass, (size_t)(sizeof(k_rot_msg_pass) - 1U));
      (void)ra_board_led_toggle(k_ra_board_led1);
    } else {
      rot_write(k_rot_msg_fail, (size_t)(sizeof(k_rot_msg_fail) - 1U));
      (void)ra_board_led_toggle(k_ra_board_led2);
    }
    ra_delay_ms(k_rot_period_ms);
  }
  rot_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
