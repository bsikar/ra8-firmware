/**
 * @file main.c
 * @brief AES-128-GCM encrypt + decrypt round-trip on EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * Imports a known 16-byte AES-128 key into the ``ra_psa_crypto``
 * facade, encrypts an 8-byte known plaintext under a fixed nonce,
 * decrypts the resulting ciphertext + tag, and verifies the
 * recovered plaintext matches the original. LED1 toggles on each
 * successful round-trip; LED2 latches on if the decrypt-and-compare
 * fails for any reason (key import error, AEAD error, byte
 * mismatch).
 *
 * Build configuration:
 * - The per-app CMake forces ``RA_SIMULATOR_MODE`` so the facade
 *   uses its in-tree soft-fallback AEAD implementation. This means
 *   the demo runs on a bare EK-RA8D2 with no RSIP keys provisioned
 *   and no Mbed TLS in the link. The same source file works
 *   unchanged once ``RA_USE_MBEDTLS=ON`` is wired up later.
 *
 * Sequence:
 *   1. CGC + SysTick + UART (SCI8) bring-up.
 *   2. ``ra_psa_crypto_init`` -- spin up the static key pool.
 *   3. Once per second:
 *      a. ``ra_psa_key_import`` AES-128 key with encrypt+decrypt usage.
 *      b. ``ra_psa_aead_encrypt`` -> ``ra_psa_aead_decrypt`` round-trip.
 *      c. ``memcmp`` the recovered plaintext against the original.
 *      d. ``ra_psa_key_destroy``.
 *      e. Log result over UART, toggle LED1 / LED2 accordingly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_psa_crypto.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_aes_demo_baud        = 115200U,
  k_aes_demo_sci_channel = 8U,
  k_aes_demo_period_ms   = 1000U,
} aes_demo_const_t;

/** @brief AES-128 key + plaintext sizing. */
typedef enum : uint8_t {
  k_aes_demo_key_bytes   = 16U,
  k_aes_demo_plain_bytes = 8U,
  k_aes_demo_aad_bytes   = 4U,
} aes_demo_layout_t;

/**
 * @brief Combined AEAD usage flag (encrypt + decrypt).
 *
 * @details
 * Declared explicitly so the bitwise-OR of two ``ra_psa_key_usage_t``
 * values keeps producing a value that is itself a member of the enum
 * (clang-tidy ``clang-analyzer-optin.core.EnumCastOutOfRange``).
 */
typedef enum : uint32_t {
  k_aes_demo_usage_aead = (uint32_t)k_ra_psa_usage_encrypt | (uint32_t)k_ra_psa_usage_decrypt,
} aes_demo_usage_t;

static const ra_port_pin_t k_aes_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_aes_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Fixed 128-bit AES key. */
static const uint8_t k_aes_demo_key[k_aes_demo_key_bytes] = {
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

/** @brief Fixed 12-byte nonce (deterministic for the demo). */
static const uint8_t k_aes_demo_nonce[k_ra_psa_gcm_nonce_len] = {
  0xA0U,
  0xA1U,
  0xA2U,
  0xA3U,
  0xA4U,
  0xA5U,
  0xA6U,
  0xA7U,
  0xA8U,
  0xA9U,
  0xAAU,
  0xABU,
};

/** @brief Plaintext "RA8D2_OK" -- 8 ASCII bytes. */
static const uint8_t k_aes_demo_plain[k_aes_demo_plain_bytes] = {
  'R',
  'A',
  '8',
  'D',
  '2',
  '_',
  'O',
  'K',
};

/** @brief AAD (additional authenticated data). */
static const uint8_t k_aes_demo_aad[k_aes_demo_aad_bytes] = {'A', 'E', 'A', 'D'};

static const uint8_t k_aes_demo_msg_ok[]   = "aes: round-trip OK\r\n";
static const uint8_t k_aes_demo_msg_fail[] = "aes: round-trip FAIL\r\n";

static void aes_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

[[nodiscard]] static ra_err_t aes_demo_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_aes_demo_pin_txd, k_ra_psel_sci_async, "crypto.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_aes_demo_pin_rxd, k_ra_psel_sci_async, "crypto.rxd8");
}

static void aes_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    aes_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    aes_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    aes_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    aes_demo_panic_halt();
  }
  if (aes_demo_pins_init() != k_ra_ok) {
    aes_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_aes_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_aes_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    aes_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    aes_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    aes_demo_panic_halt();
  }
  if (ra_psa_crypto_init() != k_ra_ok) {
    aes_demo_panic_halt();
  }
}

/**
 * @brief Single AES-128-GCM encrypt -> decrypt -> compare round-trip.
 *
 * @par MC/DC:
 * Compound decision: ``import != ok || encrypt != ok || decrypt != ok
 * || memcmp != 0``. Four atomic conditions x N+1 = 5 vectors -- this
 * test exercises the all-ok vector; the unit tests in
 * test_app_crypto_aes_demo.c cover the four fail vectors.
 *
 * @return ``k_ra_ok`` on success, error otherwise.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t aes_demo_one_round_trip(void)
{
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
  /* The PSA usage enum is intentionally a bitfield -- combining
   * encrypt + decrypt yields 0x0C, which is a valid policy mask but
   * not a declared enumerator. */
  const ra_psa_key_attr_t attr = {
    .type  = k_ra_psa_key_type_aes,
    .alg   = k_ra_psa_alg_aes_gcm,
    .usage = (ra_psa_key_usage_t)k_aes_demo_usage_aead,
  };
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  ra_psa_key_t key = nullptr;
  ra_err_t     err = ra_psa_key_import(&key, &attr, k_aes_demo_key, (size_t)k_aes_demo_key_bytes);
  if (err != k_ra_ok) {
    return err;
  }

  uint8_t ct[k_aes_demo_plain_bytes + k_ra_psa_gcm_tag_len] = {};
  size_t  ct_len                                            = 0U;
  err = ra_psa_aead_encrypt(key,
                            k_ra_psa_alg_aes_gcm,
                            k_aes_demo_nonce,
                            (size_t)k_ra_psa_gcm_nonce_len,
                            k_aes_demo_aad,
                            (size_t)k_aes_demo_aad_bytes,
                            k_aes_demo_plain,
                            (size_t)k_aes_demo_plain_bytes,
                            ct,
                            sizeof(ct),
                            &ct_len);
  if (err != k_ra_ok) {
    (void)ra_psa_key_destroy(key);
    return err;
  }

  uint8_t recovered[k_aes_demo_plain_bytes] = {};
  size_t  rec_len                           = 0U;
  err                                       = ra_psa_aead_decrypt(key,
                                                                  k_ra_psa_alg_aes_gcm,
                                                                  k_aes_demo_nonce,
                                                                  (size_t)k_ra_psa_gcm_nonce_len,
                                                                  k_aes_demo_aad,
                                                                  (size_t)k_aes_demo_aad_bytes,
                                                                  ct,
                                                                  ct_len,
                                                                  recovered,
                                                                  sizeof(recovered),
                                                                  &rec_len);
  (void)ra_psa_key_destroy(key);
  if (err != k_ra_ok) {
    return err;
  }
  if (rec_len != (size_t)k_aes_demo_plain_bytes) {
    return k_ra_err_invalid_size;
  }
  if (memcmp(recovered, k_aes_demo_plain, (size_t)k_aes_demo_plain_bytes) != 0) {
    return k_ra_err_crc_mismatch;
  }
  return k_ra_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  aes_demo_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    if (aes_demo_one_round_trip() == k_ra_ok) {
      (void)ra_sci_write_polling((uint8_t)k_aes_demo_sci_channel,
                                 k_aes_demo_msg_ok,
                                 (uint32_t)(sizeof(k_aes_demo_msg_ok) - 1U));
      (void)ra_board_led_toggle(k_ra_board_led1);
    } else {
      (void)ra_sci_write_polling((uint8_t)k_aes_demo_sci_channel,
                                 k_aes_demo_msg_fail,
                                 (uint32_t)(sizeof(k_aes_demo_msg_fail) - 1U));
      (void)ra_board_led_toggle(k_ra_board_led2);
    }
    ra_delay_ms(k_aes_demo_period_ms);
  }
  aes_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
