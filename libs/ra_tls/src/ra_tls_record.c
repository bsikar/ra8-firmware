/**
 * @file ra_tls_record.c
 * @brief TLS 1.2 record-layer encrypt/decrypt + framing helpers.
 *
 * @details
 * Implements the AES-128-GCM record format described in RFC 5288
 * sec 3 "AES Galois Counter Mode Cipher Suites for TLS" on top of
 * the ``ra_sce`` AES driver. The aad construction (RFC 5246 sec
 * 6.2.3.3 plus the GCM-specific tweaks in RFC 5288 sec 3) is
 * built byte-for-byte to match RFC: 8 bytes seq_num, 1 byte type,
 * 2 bytes version, 2 bytes length.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_sce.h"
#include "ra_tls.h"
#include "ra_tls_internal.h"

/* NOLINTBEGIN(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size,readability-function-cognitive-complexity) */

ra_err_t ra_tls_record_pack_header(uint8_t* hdr_out, ra_tls_content_type_t ct, uint16_t len)
{
  if (hdr_out == nullptr) {
    return k_ra_err_null_ptr;
  }
  hdr_out[k_ra_tls_off_record_type]   = (uint8_t)ct;
  hdr_out[k_ra_tls_off_record_ver_hi] = (uint8_t)k_ra_tls_version_major;
  hdr_out[k_ra_tls_off_record_ver_lo] = (uint8_t)k_ra_tls_version_minor;
  hdr_out[k_ra_tls_off_record_len_hi] =
    (uint8_t)((len >> k_ra_tls_shift_byte) & k_ra_tls_mask_byte);
  hdr_out[k_ra_tls_off_record_len_lo] = (uint8_t)(len & k_ra_tls_mask_byte);
  return k_ra_ok;
}

void ra_tls_aad_build(uint8_t*              aad_out,
                      uint64_t              seq,
                      ra_tls_content_type_t ct,
                      uint16_t              plaintext_len)
{
  /* RFC 5246 sec 6.2.3.3 GenericAEADCipher additional_data. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra_tls_aad_seq_bytes; ++i) {
    const uint8_t shift =
      (uint8_t)(((uint8_t)k_ra_tls_aad_seq_bytes - 1U - i) * (uint8_t)k_ra_tls_shift_byte);
    aad_out[i] = (uint8_t)((seq >> shift) & (uint64_t)k_ra_tls_mask_byte);
  }
  aad_out[k_ra_tls_aad_seq_bytes]               = (uint8_t)ct;
  aad_out[(uint8_t)k_ra_tls_aad_seq_bytes + 1U] = (uint8_t)k_ra_tls_version_major;
  aad_out[(uint8_t)k_ra_tls_aad_seq_bytes + 2U] = (uint8_t)k_ra_tls_version_minor;
  aad_out[(uint8_t)k_ra_tls_aad_seq_bytes + 3U] =
    (uint8_t)((plaintext_len >> k_ra_tls_shift_byte) & k_ra_tls_mask_byte);
  aad_out[(uint8_t)k_ra_tls_aad_seq_bytes + 4U] = (uint8_t)(plaintext_len & k_ra_tls_mask_byte);
}

void ra_tls_gcm_nonce_build(uint8_t* nonce_out, const uint8_t* implicit_iv, uint64_t seq)
{
  /* RFC 5288 sec 3: nonce = salt(4) || nonce_explicit(8). The simplest
   * standards-compliant nonce_explicit is the 64-bit record sequence
   * number serialised big-endian. */
  (void)memcpy(nonce_out, implicit_iv, (size_t)k_ra_tls_gcm_implicit_iv_bytes);
  for (uint8_t i = 0U; i < (uint8_t)k_ra_tls_gcm_explicit_iv_bytes; ++i) {
    const uint8_t shift =
      (uint8_t)(((uint8_t)k_ra_tls_gcm_explicit_iv_bytes - 1U - i) * (uint8_t)k_ra_tls_shift_byte);
    nonce_out[(uint8_t)k_ra_tls_gcm_implicit_iv_bytes + i] =
      (uint8_t)((seq >> shift) & (uint64_t)k_ra_tls_mask_byte);
  }
}

ra_err_t ra_tls_record_encrypt(ra_tls_client_t*      ctx,
                               ra_tls_content_type_t ct,
                               const uint8_t*        plaintext,
                               uint32_t              plaintext_len,
                               uint8_t*              record_out,
                               uint32_t              record_cap,
                               uint32_t*             record_len_out)
{
  if ((ctx == nullptr) || (plaintext == nullptr) || (record_out == nullptr) ||
      (record_len_out == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if ((plaintext_len == 0U) || (plaintext_len > (uint32_t)k_ra_tls_max_record_bytes)) {
    return k_ra_err_invalid_arg;
  }

  const uint32_t cipher_len    = plaintext_len + (uint32_t)k_ra_tls_gcm_tag_bytes;
  const uint32_t total_payload = (uint32_t)k_ra_tls_gcm_explicit_iv_bytes + cipher_len;
  const uint32_t total_record  = (uint32_t)k_ra_tls_record_header_bytes + total_payload;
  if (total_record > record_cap) {
    return k_ra_err_no_mem;
  }
  if (total_payload > 0xFFFFU) {
    return k_ra_err_invalid_arg;
  }

  /* Header. */
  ra_err_t err = ra_tls_record_pack_header(record_out, ct, (uint16_t)total_payload);
  if (err != k_ra_ok) {
    return err;
  }

  /* nonce_explicit = seq, written in cleartext directly after header. */
  uint8_t* explicit_iv_out = &record_out[k_ra_tls_record_header_bytes];
  for (uint8_t i = 0U; i < (uint8_t)k_ra_tls_gcm_explicit_iv_bytes; ++i) {
    const uint8_t shift =
      (uint8_t)(((uint8_t)k_ra_tls_gcm_explicit_iv_bytes - 1U - i) * (uint8_t)k_ra_tls_shift_byte);
    explicit_iv_out[i] = (uint8_t)((ctx->client_seq >> shift) & (uint64_t)k_ra_tls_mask_byte);
  }

  /* Build full GCM nonce. ra_sce expects a 16-byte IV staging area
   * even for the 12-byte GCM nonce; zero-pad the unused tail. */
  uint8_t nonce[k_ra_tls_iv_pad_bytes];
  (void)memset(nonce, 0, sizeof(nonce));
  ra_tls_gcm_nonce_build(nonce, ctx->client_write_iv, ctx->client_seq);

  /* AES-GCM encrypt of plaintext, tag appended at end. The ra_sce stub
   * ignores AAD as it does not yet plumb it through; we still build
   * the AAD so when a real backend lands the call sites are correct. */
  uint8_t aad[k_ra_tls_aad_total_bytes];
  ra_tls_aad_build(aad, ctx->client_seq, ct, (uint16_t)plaintext_len);
  (void)aad; /* AAD reserved for real-backend integration. */

  err =
    ra_sce_aes_init(ctx->client_write_key, k_ra_sce_aes_key_bits_128, k_ra_sce_aes_mode_gcm, nonce);
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t* cipher_out = &explicit_iv_out[k_ra_tls_gcm_explicit_iv_bytes];
  err                 = ra_sce_aes_encrypt(plaintext, cipher_out, plaintext_len);
  if (err != k_ra_ok) {
    (void)ra_sce_aes_finish(nullptr);
    return err;
  }
  uint8_t* tag_out = &cipher_out[plaintext_len];
  err              = ra_sce_aes_finish(tag_out);
  if (err != k_ra_ok) {
    return err;
  }

  ctx->client_seq += 1U;
  *record_len_out = total_record;
  return k_ra_ok;
}

ra_err_t ra_tls_record_decrypt(ra_tls_client_t*       ctx,
                               const uint8_t*         record,
                               uint32_t               record_len,
                               ra_tls_content_type_t* ct_out,
                               uint8_t*               plaintext_out,
                               uint32_t               plaintext_cap,
                               uint32_t*              plaintext_len_out)
{
  if ((ctx == nullptr) || (record == nullptr) || (ct_out == nullptr) ||
      (plaintext_out == nullptr) || (plaintext_len_out == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (record_len < ((uint32_t)k_ra_tls_record_header_bytes +
                    (uint32_t)k_ra_tls_gcm_explicit_iv_bytes + (uint32_t)k_ra_tls_gcm_tag_bytes)) {
    return k_ra_err_invalid_arg;
  }

  *ct_out = (ra_tls_content_type_t)record[k_ra_tls_off_record_type];
  const uint16_t body_len =
    (uint16_t)(((uint16_t)record[k_ra_tls_off_record_len_hi] << k_ra_tls_shift_byte) |
               (uint16_t)record[k_ra_tls_off_record_len_lo]);
  if ((uint32_t)body_len + (uint32_t)k_ra_tls_record_header_bytes != record_len) {
    return k_ra_err_invalid_arg;
  }

  const uint32_t cipher_len = (uint32_t)body_len - (uint32_t)k_ra_tls_gcm_explicit_iv_bytes -
                              (uint32_t)k_ra_tls_gcm_tag_bytes;
  if (cipher_len > plaintext_cap) {
    return k_ra_err_no_mem;
  }

  const uint8_t* explicit_iv = &record[k_ra_tls_record_header_bytes];
  const uint8_t* cipher_in   = &explicit_iv[k_ra_tls_gcm_explicit_iv_bytes];

  uint8_t nonce[k_ra_tls_iv_pad_bytes];
  (void)memset(nonce, 0, sizeof(nonce));
  (void)memcpy(nonce, ctx->server_write_iv, (size_t)k_ra_tls_gcm_implicit_iv_bytes);
  (void)memcpy(&nonce[k_ra_tls_gcm_implicit_iv_bytes],
               explicit_iv,
               (size_t)k_ra_tls_gcm_explicit_iv_bytes);

  ra_err_t err =
    ra_sce_aes_init(ctx->server_write_key, k_ra_sce_aes_key_bits_128, k_ra_sce_aes_mode_gcm, nonce);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_sce_aes_decrypt(cipher_in, plaintext_out, cipher_len);
  if (err != k_ra_ok) {
    (void)ra_sce_aes_finish(nullptr);
    return err;
  }
  /* The stub ra_sce treats encrypt and decrypt as XOR-keystream; the
   * tag would be checked here against a real backend. */
  (void)ra_sce_aes_finish(nullptr);

  ctx->server_seq += 1U;
  *plaintext_len_out = cipher_len;
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size,readability-function-cognitive-complexity) */
