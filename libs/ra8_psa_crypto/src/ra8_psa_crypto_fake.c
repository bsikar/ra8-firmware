/**
 * @file ra8_psa_crypto_fake.c
 * @brief Off-target crypto primitives for the ``ra8_psa_crypto`` facade.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Companion translation unit to ``ra8_psa_crypto.c``. When the host
 * unit-test build defines ``RA8_OFF_TARGET`` the heavy TF-PSA-Crypto
 * object library is not linked; this file supplies tiny in-memory crypto
 * stand-ins so the public ``ra8_psa_*`` surface still behaves:
 *
 * - SHA-256 is implemented inline (FIPS 180-4 reference algorithm) so
 *   ``ra8_psa_hash_compute`` can produce real digests in the test suite.
 * - AES-GCM is replaced by a deterministic XOR-and-checksum cipher that
 *   exercises the AEAD contract (ciphertext + tag layout, tag tampering
 *   detection) without requiring the real AES core.
 *
 * None of this code is compiled into a firmware image: the whole unit is
 * guarded by ``RA8_OFF_TARGET`` and never runs on the target.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_psa_crypto.h"
#include "ra8_psa_crypto_internal.h"

#ifdef RA8_OFF_TARGET

/* ===========================================================================
 * Off-target crypto primitives
 * ===========================================================================
 *
 * Real SHA-256 + tiny stand-ins for AES-GCM and ECDSA. These are
 * exercised by the host unit tests and never run on the target.
 */

/* -- SHA-256 (FIPS 180-4 reference) ---------------------------------------- */

/** @brief SHA-256 round constants K[0..63] (FIPS 180-4 Sec 4.2.2). */
static const uint32_t k_sha256_round_const[k_ra8_psa_sha256_schedule_len] = {
  0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
  0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
  0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
  0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
  0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
  0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
  0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
  0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
  0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
  0xc67178f2U,
};

/* Sha256 rotr -- see implementation for details. */
static inline uint32_t internal_sha256_rotr(uint32_t x, uint32_t n)
{
  return (x >> n) | (x << (k_ra8_psa_word_bits - n));
}

/* Build the 64-word message schedule W[] from a single block -- see implementation for details. */
static void internal_sha256_schedule(uint32_t      schedule[k_ra8_psa_sha256_schedule_len],
                                     const uint8_t block[k_ra8_psa_sha256_block_bytes])
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_psa_sha256_init_words; ++i) {
    schedule[i] =
      ((uint32_t)block[(i * (uint32_t)k_ra8_psa_bytes_per_word) + 0U] << k_ra8_psa_shift_b3) |
      ((uint32_t)block[(i * (uint32_t)k_ra8_psa_bytes_per_word) + 1U] << k_ra8_psa_shift_b2) |
      ((uint32_t)block[(i * (uint32_t)k_ra8_psa_bytes_per_word) + 2U] << k_ra8_psa_shift_b1) |
      ((uint32_t)block[(i * (uint32_t)k_ra8_psa_bytes_per_word) + 3U]);
  }
  for (uint32_t i = (uint32_t)k_ra8_psa_sha256_init_words;
       i < (uint32_t)k_ra8_psa_sha256_schedule_len;
       ++i) {
    const uint32_t w15 = schedule[i - (uint32_t)k_ra8_psa_w_back_15];
    const uint32_t w2  = schedule[i - (uint32_t)k_ra8_psa_w_back_2];
    const uint32_t s0  = internal_sha256_rotr(w15, k_ra8_psa_rot_s0_a) ^
                        internal_sha256_rotr(w15, k_ra8_psa_rot_s0_b) ^ (w15 >> k_ra8_psa_shr_s0);
    const uint32_t s1 = internal_sha256_rotr(w2, k_ra8_psa_rot_s1_a) ^
                        internal_sha256_rotr(w2, k_ra8_psa_rot_s1_b) ^ (w2 >> k_ra8_psa_shr_s1);
    schedule[i] = schedule[i - (uint32_t)k_ra8_psa_w_back_16] + s0 +
                  schedule[i - (uint32_t)k_ra8_psa_w_back_7] + s1;
  }
}

/* Run the 64 SHA-256 compression rounds, updating ``state`` in place -- see implementation for details. */
static void internal_sha256_rounds(uint32_t       state[k_ra8_psa_sha256_state_words],
                                   const uint32_t schedule[k_ra8_psa_sha256_schedule_len])
{
  uint32_t a = state[k_ra8_psa_state_idx_a];
  uint32_t b = state[k_ra8_psa_state_idx_b];
  uint32_t c = state[k_ra8_psa_state_idx_c];
  uint32_t d = state[k_ra8_psa_state_idx_d];
  uint32_t e = state[k_ra8_psa_state_idx_e];
  uint32_t f = state[k_ra8_psa_state_idx_f];
  uint32_t g = state[k_ra8_psa_state_idx_g];
  uint32_t h = state[k_ra8_psa_state_idx_h];
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_psa_sha256_schedule_len; ++i) {
    const uint32_t big_sigma1 = internal_sha256_rotr(e, k_ra8_psa_rot_e_a) ^
                                internal_sha256_rotr(e, k_ra8_psa_rot_e_b) ^
                                internal_sha256_rotr(e, k_ra8_psa_rot_e_c);
    const uint32_t ch         = (e & f) ^ ((~e) & g);
    const uint32_t temp1      = h + big_sigma1 + ch + k_sha256_round_const[i] + schedule[i];
    const uint32_t big_sigma0 = internal_sha256_rotr(a, k_ra8_psa_rot_a_a) ^
                                internal_sha256_rotr(a, k_ra8_psa_rot_a_b) ^
                                internal_sha256_rotr(a, k_ra8_psa_rot_a_c);
    const uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = big_sigma0 + maj;
    h                    = g;
    g                    = f;
    f                    = e;
    e                    = d + temp1;
    d                    = c;
    c                    = b;
    b                    = a;
    a                    = temp1 + temp2;
  }
  state[k_ra8_psa_state_idx_a] += a;
  state[k_ra8_psa_state_idx_b] += b;
  state[k_ra8_psa_state_idx_c] += c;
  state[k_ra8_psa_state_idx_d] += d;
  state[k_ra8_psa_state_idx_e] += e;
  state[k_ra8_psa_state_idx_f] += f;
  state[k_ra8_psa_state_idx_g] += g;
  state[k_ra8_psa_state_idx_h] += h;
}

/* Process one 64-byte block into the hash ``state`` -- see implementation for details. */
static void internal_sha256_block(uint32_t      state[k_ra8_psa_sha256_state_words],
                                  const uint8_t block[k_ra8_psa_sha256_block_bytes])
{
  uint32_t schedule[k_ra8_psa_sha256_schedule_len];
  internal_sha256_schedule(schedule, block);
  internal_sha256_rounds(state, schedule);
}

void ra8_psa_fake_sha256_oneshot(const uint8_t* in,
                                 size_t         in_len,
                                 uint8_t        out[k_ra8_psa_sha256_len])
{
  uint32_t state[k_ra8_psa_sha256_state_words] = {
    k_ra8_psa_sha256_h0,
    k_ra8_psa_sha256_h1,
    k_ra8_psa_sha256_h2,
    k_ra8_psa_sha256_h3,
    k_ra8_psa_sha256_h4,
    k_ra8_psa_sha256_h5,
    k_ra8_psa_sha256_h6,
    k_ra8_psa_sha256_h7,
  };

  /* Process complete blocks. */
  const size_t   complete_blocks = in_len / (size_t)k_ra8_psa_sha256_block_bytes;
  const size_t   remaining = in_len - (complete_blocks * (size_t)k_ra8_psa_sha256_block_bytes);
  const uint8_t* p         = in;
  for (size_t b = 0U; b < complete_blocks; ++b) {
    internal_sha256_block(state, p);
    p += (size_t)k_ra8_psa_sha256_block_bytes;
  }

  /* Final block(s) with padding. ``remaining`` is guaranteed < 64 by the
   * arithmetic above; the explicit clamp keeps clang-analyzer's bound
   * tracking happy when reading the tail byte-copy loop. */
  uint8_t      tail[k_ra8_psa_sha256_pad_buf_len] = {};
  const size_t tail_copy_len = (remaining < (size_t)k_ra8_psa_sha256_block_bytes)
                                 ? remaining
                                 : (size_t)k_ra8_psa_sha256_block_bytes - 1U;
  /* memcpy's source is declared nonnull; for an empty input ``p`` may be NULL
   * (and ``tail_copy_len`` is then 0), so skip the otherwise-UB call. */
  if (tail_copy_len > 0U) {
    (void)memcpy(tail, p, tail_copy_len);
  }
  tail[tail_copy_len]        = (uint8_t)k_ra8_psa_pad_marker;
  const size_t   tail_blocks = (remaining < (size_t)k_ra8_psa_sha256_pad_threshold) ? 1U : 2U;
  const uint64_t bitlen      = (uint64_t)in_len * (uint64_t)k_ra8_psa_byte_bits;
  const size_t   tail_len    = tail_blocks * (size_t)k_ra8_psa_sha256_block_bytes;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_psa_length_field_bytes; ++i) {
    tail[tail_len - 1U - i] = (uint8_t)(bitlen >> (i * (uint32_t)k_ra8_psa_byte_bits));
  }
  for (size_t b = 0U; b < tail_blocks; ++b) {
    internal_sha256_block(state, &tail[b * (size_t)k_ra8_psa_sha256_block_bytes]);
  }

  for (uint32_t i = 0U; i < (uint32_t)k_ra8_psa_sha256_state_words; ++i) {
    out[(i * (uint32_t)k_ra8_psa_bytes_per_word) + 0U] = (uint8_t)(state[i] >> k_ra8_psa_shift_b3);
    out[(i * (uint32_t)k_ra8_psa_bytes_per_word) + 1U] = (uint8_t)(state[i] >> k_ra8_psa_shift_b2);
    out[(i * (uint32_t)k_ra8_psa_bytes_per_word) + 2U] = (uint8_t)(state[i] >> k_ra8_psa_shift_b1);
    out[(i * (uint32_t)k_ra8_psa_bytes_per_word) + 3U] = (uint8_t)(state[i]);
  }
}

/**
 * @brief Compute the AEAD tag stand-in for the off-target path.
 *
 * @details
 * Real AES-GCM produces a 16-byte tag from key, nonce, ciphertext and
 * AAD. The fake replaces it with SHA-256(key || nonce || aad ||
 * cipher) truncated to 16 bytes. This is *not* cryptographically
 * secure but reproduces the same input dependencies, so tampering any
 * byte changes the tag and the decrypt-side check fails.
 *
 * @param[in,out] aad See function signature.
 * @param[in,out] aad_len See function signature.
 * @param[in,out] cipher See function signature.
 * @param[in,out] cipher_len See function signature.
 * @param[in,out] key See function signature.
 * @param[in,out] key_len See function signature.
 * @param[in,out] nonce See function signature.
 * @param[in,out] nonce_len See function signature.
 * @param[in,out] out_tag See function signature.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_fake_aead_tag(const uint8_t* key,
                                   size_t         key_len,
                                   const uint8_t* nonce,
                                   size_t         nonce_len,
                                   const uint8_t* aad,
                                   size_t         aad_len,
                                   const uint8_t* cipher,
                                   size_t         cipher_len,
                                   uint8_t        out_tag[k_ra8_psa_gcm_tag_len])
{
  uint8_t buf[k_ra8_psa_fake_scratch_bytes];
  size_t  off = 0U;
  for (size_t i = 0U; (i < key_len) && (off < sizeof(buf)); ++i) {
    buf[off++] = key[i];
  }
  for (size_t i = 0U; (i < nonce_len) && (off < sizeof(buf)); ++i) {
    buf[off++] = nonce[i];
  }
  for (size_t i = 0U; (i < aad_len) && (off < sizeof(buf)); ++i) {
    buf[off++] = aad[i];
  }
  for (size_t i = 0U; (i < cipher_len) && (off < sizeof(buf)); ++i) {
    buf[off++] = cipher[i];
  }
  uint8_t digest[k_ra8_psa_sha256_len];
  ra8_psa_fake_sha256_oneshot(buf, off, digest);
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_psa_gcm_tag_len; ++i) {
    out_tag[i] = digest[i];
  }
}

/**
 * @brief Generate the keystream byte for offset ``i`` (fake AES-GCM).
 *
 * @details
 * Derives a per-position keystream from SHA-256(key || nonce || "ks").
 * Pseudo-AES, sufficient for round-trip tests.
 *
 * @param[in,out] dst See function signature.
 * @param[in,out] key See function signature.
 * @param[in,out] key_len See function signature.
 * @param[in,out] len See function signature.
 * @param[in,out] nonce See function signature.
 * @param[in,out] nonce_len See function signature.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_fake_keystream(const uint8_t* key,
                                    size_t         key_len,
                                    const uint8_t* nonce,
                                    size_t         nonce_len,
                                    uint8_t*       dst,
                                    size_t         len)
{
  uint8_t seed[k_ra8_psa_fake_scratch_bytes];
  size_t  off = 0U;
  for (size_t i = 0U; (i < key_len) && (off < sizeof(seed)); ++i) {
    seed[off++] = key[i];
  }
  for (size_t i = 0U; (i < nonce_len) && (off < sizeof(seed)); ++i) {
    seed[off++] = nonce[i];
  }
  uint8_t  block[k_ra8_psa_sha256_len];
  uint32_t counter  = 0U;
  size_t   produced = 0U;
  while (produced < len) {
    seed[off]      = (uint8_t)(counter >> k_ra8_psa_shift_b3);
    seed[off + 1U] = (uint8_t)(counter >> k_ra8_psa_shift_b2);
    seed[off + 2U] = (uint8_t)(counter >> k_ra8_psa_shift_b1);
    seed[off + 3U] = (uint8_t)counter;
    ra8_psa_fake_sha256_oneshot(seed, off + (size_t)k_ra8_psa_bytes_per_word, block);
    const size_t take = ((len - produced) < (size_t)k_ra8_psa_sha256_len)
                          ? (len - produced)
                          : (size_t)k_ra8_psa_sha256_len;
    for (size_t i = 0U; i < take; ++i) {
      dst[produced + i] = block[i];
    }
    produced += take;
    ++counter;
  }
}

ra8_err_t ra8_psa_fake_aead_encrypt(const struct ra8_psa_key_handle* slot,
                                    const uint8_t*                   nonce,
                                    size_t                           nonce_len,
                                    const uint8_t*                   aad,
                                    size_t                           aad_len,
                                    const uint8_t*                   plain,
                                    size_t                           plain_len,
                                    uint8_t*                         out,
                                    size_t*                          out_len)
{
  if (plain_len > 0U) {
    uint8_t ks[k_ra8_psa_fake_scratch_bytes];
    if (plain_len > sizeof(ks)) {
      return k_ra8_err_invalid_size;
    }
    internal_fake_keystream(slot->key, slot->key_len, nonce, nonce_len, ks, plain_len);
    for (size_t i = 0U; i < plain_len; ++i) {
      out[i] = (uint8_t)(plain[i] ^ ks[i]);
    }
  }
  internal_fake_aead_tag(slot->key,
                         slot->key_len,
                         nonce,
                         nonce_len,
                         aad,
                         aad_len,
                         out,
                         plain_len,
                         &out[plain_len]);
  *out_len = plain_len + (size_t)k_ra8_psa_gcm_tag_len;
  return k_ra8_ok;
}

ra8_err_t ra8_psa_fake_aead_decrypt(const struct ra8_psa_key_handle* slot,
                                    const uint8_t*                   nonce,
                                    size_t                           nonce_len,
                                    const uint8_t*                   aad,
                                    size_t                           aad_len,
                                    const uint8_t*                   cipher,
                                    size_t                           plain_len,
                                    uint8_t*                         out,
                                    size_t*                          out_len)
{
  uint8_t expected_tag[k_ra8_psa_gcm_tag_len];
  internal_fake_aead_tag(slot->key,
                         slot->key_len,
                         nonce,
                         nonce_len,
                         aad,
                         aad_len,
                         cipher,
                         plain_len,
                         expected_tag);
  uint8_t diff = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_psa_gcm_tag_len; ++i) {
    diff |= (uint8_t)(expected_tag[i] ^ cipher[plain_len + i]);
  }
  if (diff != 0U) {
    return k_ra8_err_crc_mismatch;
  }
  if (plain_len > 0U) {
    uint8_t ks[k_ra8_psa_fake_scratch_bytes];
    if (plain_len > sizeof(ks)) {
      return k_ra8_err_invalid_size;
    }
    internal_fake_keystream(slot->key, slot->key_len, nonce, nonce_len, ks, plain_len);
    for (size_t i = 0U; i < plain_len; ++i) {
      out[i] = (uint8_t)(cipher[i] ^ ks[i]);
    }
  }
  *out_len = plain_len;
  return k_ra8_ok;
}

#endif /* RA8_OFF_TARGET */
