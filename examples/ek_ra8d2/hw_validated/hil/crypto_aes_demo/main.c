/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/crypto_aes_demo/main.c
 * @brief AES-128-GCM encrypt + decrypt round-trip on EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * Imports a known 16-byte AES-128 key into the ``ra8_psa_crypto``
 * facade, encrypts an 8-byte known plaintext under a fixed nonce,
 * decrypts the resulting ciphertext + tag, and verifies the
 * recovered plaintext matches the original. LED1 toggles on each
 * successful round-trip; LED2 latches on if the decrypt-and-compare
 * fails for any reason (key import error, AEAD error, byte
 * mismatch).
 *
 * Build configuration:
 * - The per-app CMake forces ``RA8_OFF_TARGET`` so the facade
 *   uses its in-tree soft-fallback AEAD implementation. This means
 *   the demo runs on a bare EK-RA8D2 with no RSIP keys provisioned
 *   and no Mbed TLS in the link. The same source file works
 *   unchanged once ``RA8_USE_MBEDTLS=ON`` is wired up later.
 *
 * Sequence:
 *   1. CGC + SysTick + UART (SCI8) bring-up.
 *   2. ``ra8_psa_crypto_init`` -- spin up the static key pool.
 *   3. Once per second:
 *      a. ``ra8_psa_key_import`` AES-128 key with encrypt+decrypt usage.
 *      b. ``ra8_psa_aead_encrypt`` -> ``ra8_psa_aead_decrypt`` round-trip.
 *      c. ``memcmp`` the recovered plaintext against the original.
 *      d. ``ra8_psa_key_destroy``.
 *      e. Log result over UART, toggle LED1 / LED2 accordingly.
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_psa_crypto.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_aes_demo_baud      = 115200U, /**< AES demo baud.      */
  k_aes_demo_period_ms = 1000U,   /**< AES demo period ms. */
} aes_demo_const_t;

/** @brief AES-128 key + plaintext sizing. */
typedef enum : uint8_t {
  k_aes_demo_key_bytes   = 16U, /**< AES demo key bytes.   */
  k_aes_demo_plain_bytes = 8U,  /**< AES demo plain bytes. */
  k_aes_demo_aad_bytes   = 4U,  /**< AES demo aad bytes.   */
} aes_demo_layout_t;

/**
 * @brief Combined AEAD usage flag (encrypt + decrypt).
 *
 * @details
 * Declared explicitly so the bitwise-OR of two ``ra8_psa_key_usage_t``
 * values keeps producing a value that is itself a member of the enum
 * (clang-tidy ``clang-analyzer-optin.core.EnumCastOutOfRange``).
 */
typedef enum : uint32_t {
  k_aes_demo_usage_aead = (uint32_t)k_ra8_psa_usage_encrypt |
                          (uint32_t)k_ra8_psa_usage_decrypt, /**< AES demo usage aead. */
} aes_demo_usage_t;

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
static const uint8_t k_aes_demo_nonce[k_ra8_psa_gcm_nonce_len] = {
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

static void aes_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    aes_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    aes_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    aes_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_aes_demo_baud) != k_ra8_ok) {
    aes_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    aes_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    aes_demo_panic_halt();
  }
  if (ra8_psa_crypto_init() != k_ra8_ok) {
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
 * @return ``k_ra8_ok`` on success, error otherwise.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t aes_demo_one_round_trip(void)
{
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- OR-combined PSA usage bits form a valid policy mask outside the enumerator list. */
  /* The PSA usage enum is intentionally a bitfield -- combining
   * encrypt + decrypt yields 0x0C, which is a valid policy mask but
   * not a declared enumerator. */
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_aes,
    .alg   = k_ra8_psa_alg_aes_gcm,
    .usage = (ra8_psa_key_usage_t)k_aes_demo_usage_aead,
  };
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  ra8_psa_key_t key = nullptr;
  ra8_err_t     err = ra8_psa_key_import(&key, &attr, k_aes_demo_key, (size_t)k_aes_demo_key_bytes);
  if (err != k_ra8_ok) {
    return err;
  }

  uint8_t ct[k_aes_demo_plain_bytes + k_ra8_psa_gcm_tag_len] = {};
  size_t  ct_len                                             = 0U;
  err = ra8_psa_aead_encrypt(key,
                             k_ra8_psa_alg_aes_gcm,
                             k_aes_demo_nonce,
                             (size_t)k_ra8_psa_gcm_nonce_len,
                             k_aes_demo_aad,
                             (size_t)k_aes_demo_aad_bytes,
                             k_aes_demo_plain,
                             (size_t)k_aes_demo_plain_bytes,
                             ct,
                             sizeof(ct),
                             &ct_len);
  if (err != k_ra8_ok) {
    (void)ra8_psa_key_destroy(key);
    return err;
  }

  uint8_t recovered[k_aes_demo_plain_bytes] = {};
  size_t  rec_len                           = 0U;
  err                                       = ra8_psa_aead_decrypt(key,
                                                                   k_ra8_psa_alg_aes_gcm,
                                                                   k_aes_demo_nonce,
                                                                   (size_t)k_ra8_psa_gcm_nonce_len,
                                                                   k_aes_demo_aad,
                                                                   (size_t)k_aes_demo_aad_bytes,
                                                                   ct,
                                                                   ct_len,
                                                                   recovered,
                                                                   sizeof(recovered),
                                                                   &rec_len);
  (void)ra8_psa_key_destroy(key);
  if (err != k_ra8_ok) {
    return err;
  }
  if (rec_len != (size_t)k_aes_demo_plain_bytes) {
    return k_ra8_err_invalid_size;
  }
  if (memcmp(recovered, k_aes_demo_plain, (size_t)k_aes_demo_plain_bytes) != 0) {
    return k_ra8_err_crc_mismatch;
  }
  return k_ra8_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  aes_demo_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    if (aes_demo_one_round_trip() == k_ra8_ok) {
      (void)ra8_board_uart_console_write(k_aes_demo_msg_ok,
                                         (size_t)(sizeof(k_aes_demo_msg_ok) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_aes_demo_msg_fail,
                                         (size_t)(sizeof(k_aes_demo_msg_fail) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ra8_delay_ms(k_aes_demo_period_ms);
  }
  aes_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
