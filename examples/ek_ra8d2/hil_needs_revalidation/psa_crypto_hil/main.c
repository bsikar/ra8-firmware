/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/psa_crypto_hil/main.c
 * @brief On-silicon NIST/RFC known-answer test for the REAL crypto backend
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Runs the vendored TF-PSA-Crypto software backend -- the exact ``psa_*``
 * primitives the production ``ra8_psa_crypto`` (on-target) path calls -- on the real
 * M85 and checks it against published vectors:
 *   - SHA-256("abc")                         FIPS 180-4
 *   - AES-128-GCM encrypt + decrypt          GCM spec (McGrew/Viega) Test Case 3
 *   - ECDSA-P256 / SHA-256 verify            RFC 6979 A.2.5 ("sample") + tamper
 *
 * This is the on-silicon counterpart of tests/test_psa_real_kat.c and proves that
 * the crypto the root of trust needs actually works on hardware (the RSIP crypto
 * hardware is non-functional; see libs/ra8_hal/src/ra8_rsip.c). Reports
 * "psa crypto: KAT OK" / "psa crypto: KAT FAIL" over SCI8 (115200 8N1).
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "mbedtls/memory_buffer_alloc.h"
#include "psa/crypto.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_kat_baud       = 115200U,  /**< Kat baud.                                     */
  k_kat_period_ms  = 1000U,    /**< Kat period ms.                                */
  k_kat_heap_bytes = 0x10000U, /**< 64 KiB static heap for tf-psa mbedtls_calloc. */
} kat_const_t;

/** @brief Buffer/length sizing constants for the vectors. */
typedef enum : uint8_t {
  k_sha_len   = 32U, /**< SHA length.             */
  k_gcm_key   = 16U, /**< Gcm key.                */
  k_gcm_nonce = 12U, /**< Gcm nonce.              */
  k_gcm_pt    = 64U, /**< Gcm pt.                 */
  k_gcm_ct    = 80U, /**< 64 ciphertext + 16 tag. */
  k_ec_point  = 65U, /**< 0x04 || X || Y.         */
  k_ec_sig    = 64U, /**< r || s.                 */
  k_ec_coord  = 32U, /**< P-256 coordinate width. */
  k_abc_len   = 3U,  /**< strlen("abc").          */
  k_smpl_len  = 6U,  /**< strlen("sample").       */
  k_hex_ten   = 10U, /**< Hex digit A/a == 10.    */
  k_hex_shift = 4U,  /**< High-nibble shift.      */
} kat_size_t;

static const uint8_t k_kat_msg_ok[]   = "psa crypto: KAT OK\r\n";
static const uint8_t k_kat_msg_fail[] = "psa crypto: KAT FAIL\r\n";

/* Boot-path diagnostics. */
static const uint8_t k_kat_diag_boot[]      = "psa: boot (console up)\r\n";
static const uint8_t k_kat_diag_init_fail[] = "psa: crypto_init FAIL\r\n";
static const uint8_t k_kat_diag_init_ok[]   = "psa: crypto_init ok\r\n";

/** @brief Static heap tf-psa's mbedtls_calloc draws from (no libc heap on target). */
static uint8_t s_kat_heap[k_kat_heap_bytes];

/** @brief One hex digit -> nibble (ASCII order: '0'-'9' < 'A'-'F' < 'a'-'f'). */
static uint8_t hex_nibble(char c)
{
  uint8_t v = 0U;
  if (c >= 'a') {
    v = (uint8_t)((c - 'a') + (int)k_hex_ten);
  } else if (c >= 'A') {
    v = (uint8_t)((c - 'A') + (int)k_hex_ten);
  } else {
    v = (uint8_t)(c - '0');
  }
  return v;
}

/** @brief Decode ``2*n`` hex chars into ``out`` (n bytes). */
static void unhex(const char* hex, uint8_t* out, size_t n)
{
  for (size_t i = 0U; i < n; ++i) {
    uint8_t hi = hex_nibble(hex[2U * i]);
    uint8_t lo = hex_nibble(hex[(2U * i) + 1U]);
    out[i]     = (uint8_t)((int)hi << (int)k_hex_shift) | lo;
  }
}

/** @brief FIPS 180-4 SHA-256("abc"). */
static bool kat_sha256(void)
{
  uint8_t want[k_sha_len];
  unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, k_sha_len);
  uint8_t      got[k_sha_len];
  size_t       got_len = 0U;
  psa_status_t st      = psa_hash_compute(PSA_ALG_SHA_256,
                                          (const uint8_t*)"abc",
                                          (size_t)k_abc_len,
                                          got,
                                          sizeof(got),
                                          &got_len);
  return (st == PSA_SUCCESS) && (got_len == (size_t)k_sha_len) &&
         (memcmp(got, want, k_sha_len) == 0);
}

/** @brief AES-128-GCM encrypt of @p pt under @p kid must equal @p want_ct. */
static bool
gcm_enc_ok(psa_key_id_t kid, const uint8_t* nonce, const uint8_t* pt, const uint8_t* want_ct)
{
  uint8_t ct[k_gcm_ct];
  size_t  n = 0U;
  if (psa_aead_encrypt(kid,
                       PSA_ALG_GCM,
                       nonce,
                       k_gcm_nonce,
                       nullptr,
                       0U,
                       pt,
                       k_gcm_pt,
                       ct,
                       sizeof(ct),
                       &n) != PSA_SUCCESS) {
    return false;
  }
  if (n != (size_t)k_gcm_ct) {
    return false;
  }
  return memcmp(ct, want_ct, k_gcm_ct) == 0;
}

/** @brief AES-128-GCM decrypt of @p want_ct under @p kid must equal @p pt. */
static bool
gcm_dec_ok(psa_key_id_t kid, const uint8_t* nonce, const uint8_t* want_ct, const uint8_t* pt)
{
  uint8_t dec[k_gcm_pt];
  size_t  n = 0U;
  if (psa_aead_decrypt(kid,
                       PSA_ALG_GCM,
                       nonce,
                       k_gcm_nonce,
                       nullptr,
                       0U,
                       want_ct,
                       k_gcm_ct,
                       dec,
                       sizeof(dec),
                       &n) != PSA_SUCCESS) {
    return false;
  }
  if (n != (size_t)k_gcm_pt) {
    return false;
  }
  return memcmp(dec, pt, k_gcm_pt) == 0;
}

/** @brief GCM spec Test Case 3: AES-128-GCM encrypt then decrypt round-trip. */
static bool kat_aes_gcm(void)
{
  uint8_t key[k_gcm_key];
  unhex("feffe9928665731c6d6a8f9467308308", key, k_gcm_key);
  uint8_t nonce[k_gcm_nonce];
  unhex("cafebabefacedbaddecaf888", nonce, k_gcm_nonce);
  uint8_t pt[k_gcm_pt];
  unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e24"
        "49a6b525b16aedf5aa0de657ba637b391aafd255",
        pt,
        k_gcm_pt);
  uint8_t want_ct[k_gcm_ct];
  unhex("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5a"
        "ac84aa051ba30b396a0aac973d58e091473f59854d5c2af327cd64a62cf35abd2ba6fab4",
        want_ct,
        k_gcm_ct);

  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&attr, PSA_ALG_GCM);
  psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
  psa_key_id_t kid = 0;
  if (psa_import_key(&attr, key, sizeof(key), &kid) != PSA_SUCCESS) {
    return false;
  }

  bool ok = gcm_enc_ok(kid, nonce, pt, want_ct);
  if (!gcm_dec_ok(kid, nonce, want_ct, pt)) {
    ok = false;
  }
  (void)psa_destroy_key(kid);
  return ok;
}

/** @brief RFC 6979 A.2.5 ECDSA-P256/SHA-256 verify of "sample" + a tamper check. */
static bool kat_ecdsa_p256(void)
{
  uint8_t pub[k_ec_point];
  pub[0] = 0x04U;
  unhex("60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6",
        &pub[1],
        (size_t)k_ec_coord);
  unhex("7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299",
        &pub[1U + (size_t)k_ec_coord],
        (size_t)k_ec_coord);
  uint8_t sig[k_ec_sig];
  unhex("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716",
        &sig[0],
        (size_t)k_ec_coord);
  unhex("F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8",
        &sig[(size_t)k_ec_coord],
        (size_t)k_ec_coord);

  uint8_t hash[k_sha_len];
  size_t  hl = 0U;
  (void)psa_hash_compute(PSA_ALG_SHA_256,
                         (const uint8_t*)"sample",
                         (size_t)k_smpl_len,
                         hash,
                         sizeof(hash),
                         &hl);

  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
  psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
  psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
  psa_key_id_t kid = 0;
  if (psa_import_key(&attr, pub, sizeof(pub), &kid) != PSA_SUCCESS) {
    return false;
  }

  bool ok = true;
  if (psa_verify_hash(kid, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash), sig, sizeof(sig)) !=
      PSA_SUCCESS) {
    ok = false;
  }
  sig[0] ^= 0x01U; /* flip one bit -> must be rejected */
  if (psa_verify_hash(kid, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash), sig, sizeof(sig)) !=
      PSA_ERROR_INVALID_SIGNATURE) {
    ok = false;
  }
  (void)psa_destroy_key(kid);
  return ok;
}

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
  (void)ra8_board_uart_console_write(k_kat_diag_boot, (size_t)(sizeof(k_kat_diag_boot) - 1U));
  mbedtls_memory_buffer_alloc_init(s_kat_heap, sizeof(s_kat_heap));
  if (psa_crypto_init() != PSA_SUCCESS) {
    (void)ra8_board_uart_console_write(k_kat_diag_init_fail,
                                       (size_t)(sizeof(k_kat_diag_init_fail) - 1U));
    kat_panic_halt();
  }
  (void)ra8_board_uart_console_write(k_kat_diag_init_ok, (size_t)(sizeof(k_kat_diag_init_ok) - 1U));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  kat_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    bool ok = true;
    if (!kat_sha256()) {
      ok = false;
    }
    if (!kat_aes_gcm()) {
      ok = false;
    }
    if (!kat_ecdsa_p256()) {
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
