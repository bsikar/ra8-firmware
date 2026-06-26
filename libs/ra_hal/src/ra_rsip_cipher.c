/**
 * @file ra_rsip_cipher.c
 * @brief RSIP-E50D symmetric cipher + wrapped-key install path
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Symmetric-cipher and key-install slice of the RA8D2 RSIP-E50D HAL
 * driver, split out of ``ra_rsip.c`` to keep every translation unit
 * under the file-size budget. Covers HUM Ch 51 (Security Features
 * p 3263-3301) + Ch 52 (RSIP-E50D mailbox p 3302-3307) for:
 *
 * - the round-3 little-endian byte-packing primitives and the mailbox
 *   completion driver shared with the asymmetric path;
 * - wrapped-key install (plaintext + OEM (PE5/PE6) flows);
 * - symmetric AES (ECB / CBC / CTR / GCM / CCM / XTS / CMAC / GMAC)
 *   for both encrypt and decrypt;
 * - ChaCha20 + Poly1305 (stream + AEAD + standalone MAC).
 *
 * Cross-TU primitives shared with ``ra_rsip.c`` and ``ra_rsip_asym.c``
 * are declared in ``ra_rsip_internal.h``. The engine itself is opaque
 * (HUM Ch 52, p 3302-3307); sequences here are derived from the FSP
 * RSIP primitive layer but no FSP code is included verbatim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_rsip_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_rsip.h"
#include "ra_rsip_internal.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ``ra_log_*`` call in this TU.
 *
 * @details
 * Kept short ("RSIP") so it fits in the fixed-width log prefix without
 * truncation. Each RSIP translation unit keeps its own private copy.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const char* s_tag = "RSIP";

/* ===========================================================================
 * Round-3 byte-packing primitives + mailbox completion
 * ===========================================================================
 */

uint32_t internal_pack_le(const uint8_t* p)
{
  return ((uint32_t)p[0]) | (((uint32_t)p[1]) << k_ra_rsip_byte_bits) |
         (((uint32_t)p[2]) << k_ra_rsip_byte_shift_2) |
         (((uint32_t)p[3]) << k_ra_rsip_byte_shift_3);
}

void internal_unpack_le(uint32_t word, uint8_t* p)
{
  p[0] = (uint8_t)(word & k_ra_rsip_byte_mask);
  p[1] = (uint8_t)((word >> k_ra_rsip_byte_bits) & k_ra_rsip_byte_mask);
  p[2] = (uint8_t)((word >> k_ra_rsip_byte_shift_2) & k_ra_rsip_byte_mask);
  p[3] = (uint8_t)((word >> k_ra_rsip_byte_shift_3) & k_ra_rsip_byte_mask);
}

uint32_t internal_handle_words_for(ra_rsip_oem_cmd_t cmd)
{
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  /* Handle-body sizes mirror FSP r_rsip_key_injection.c. */
  switch (cmd) {
    case k_ra_rsip_oem_cmd_aes128:
      return (uint32_t)k_ra_rsip_handle_words_aes128;
    case k_ra_rsip_oem_cmd_aes192:
      return (uint32_t)k_ra_rsip_handle_words_aes192;
    case k_ra_rsip_oem_cmd_aes256:
      return (uint32_t)k_ra_rsip_handle_words_aes256;
    case k_ra_rsip_oem_cmd_chacha20:
      return (uint32_t)k_ra_rsip_handle_words_chacha20;
    case k_ra_rsip_oem_cmd_hmac_sha224:
      return (uint32_t)k_ra_rsip_handle_words_hmac_sha224;
    case k_ra_rsip_oem_cmd_hmac_sha256:
      return (uint32_t)k_ra_rsip_handle_words_hmac_sha256;
    case k_ra_rsip_oem_cmd_hmac_sha384:
      return (uint32_t)k_ra_rsip_handle_words_hmac_sha384;
    case k_ra_rsip_oem_cmd_hmac_sha512:
    case k_ra_rsip_oem_cmd_hmac_sha512_224:
    case k_ra_rsip_oem_cmd_hmac_sha512_256:
      return (uint32_t)k_ra_rsip_handle_words_hmac_sha512;
    case k_ra_rsip_oem_cmd_ecc_secp256r1_priv:
    case k_ra_rsip_oem_cmd_ecc_secp256k1_priv:
    case k_ra_rsip_oem_cmd_ecc_brain256r1_priv:
    case k_ra_rsip_oem_cmd_ecc_ed25519_priv:
      return (uint32_t)k_ra_rsip_handle_words_ecc256_priv;
    case k_ra_rsip_oem_cmd_ecc_secp384r1_priv:
    case k_ra_rsip_oem_cmd_ecc_brain384r1_priv:
    case k_ra_rsip_oem_cmd_ecc_brain512r1_priv:
      return (uint32_t)k_ra_rsip_handle_words_ecc384_priv;
    case k_ra_rsip_oem_cmd_ecc_secp521r1_priv:
      return (uint32_t)k_ra_rsip_handle_words_ecc521_priv;
    case k_ra_rsip_oem_cmd_rsa2048_priv:
      return (uint32_t)k_ra_rsip_handle_words_rsa2048_priv;
    case k_ra_rsip_oem_cmd_rsa3072_priv:
      return (uint32_t)k_ra_rsip_handle_words_rsa3072_priv;
    case k_ra_rsip_oem_cmd_rsa4096_priv:
      return (uint32_t)k_ra_rsip_handle_words_rsa4096_priv;
    case k_ra_rsip_oem_cmd_aes128_xts:
    case k_ra_rsip_oem_cmd_aes256_xts:
    case k_ra_rsip_oem_cmd_invalid:
    default:
      return 0U;
  }
}

ra_err_t internal_complete(uint32_t done_mask)
{
  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Pre-assert the DONE bit so the host sim spin terminates. */
  *ra_rsip_reg32(k_ra_rsip_off_isr) |= done_mask;

  const ra_err_t wait_err = internal_wait_bit(k_ra_rsip_off_isr, done_mask);
  if (wait_err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
    return wait_err;
  }
  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Read MBOX_RET; non-zero indicates engine-side error. */
  const uint32_t ret = *ra_rsip_reg32(k_ra_rsip_off_mbox_ret);
  /* W1C ack on the completion bit. */
  *ra_rsip_reg32(k_ra_rsip_off_isr) = done_mask;
  if (ret != 0U) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/** @brief Implementation of `internal_push_bytes_to_port()` -- LE word stream + zero-padded tail. */
void internal_push_bytes_to_port(ra_rsip_off_t off, const uint8_t* in, uint32_t len)
{
  volatile uint32_t* port = ra_rsip_reg32(off);
  uint32_t           i    = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= len) {
    *port = internal_pack_le(&in[i]);
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
  }
  if (i < len) {
    uint32_t tail = 0U;
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      tail |= ((uint32_t)in[i + b]) << (b * k_ra_rsip_byte_bits);
    }
    *port = tail;
  }
}

/* Stream ``len`` bytes into the data input window -- see surrounding code and HUM citations. */
static void internal_push_data(const uint8_t* in, uint32_t len)
{
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  /* Stream 32-bit words through DATA_IN0..3 round-robin. */
  uint32_t i    = 0U;
  uint8_t  lane = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= len) {
    const uint32_t      word = internal_pack_le(&in[i]);
    const ra_rsip_off_t off =
      (ra_rsip_off_t)(k_ra_rsip_off_data_in0 + (uint16_t)((uint32_t)lane << k_ra_rsip_word_shift));
    *ra_rsip_reg32(off) = word;
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
    lane = (uint8_t)((lane + 1U) & (k_ra_rsip_aes_block_w - 1U));
  }
  if (i < len) {
    uint32_t tail = 0U;
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      tail |= ((uint32_t)in[i + b]) << (b * k_ra_rsip_byte_bits);
    }
    const ra_rsip_off_t off =
      (ra_rsip_off_t)(k_ra_rsip_off_data_in0 + (uint16_t)((uint32_t)lane << k_ra_rsip_word_shift));
    *ra_rsip_reg32(off) = tail;
  }
}

/* Pull ``len`` bytes back from the data output window -- see surrounding code and HUM citations. */
static void internal_pull_data(uint8_t* out, uint32_t len)
{
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  uint32_t i    = 0U;
  uint8_t  lane = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= len) {
    const ra_rsip_off_t off =
      (ra_rsip_off_t)(k_ra_rsip_off_data_out0 + (uint16_t)((uint32_t)lane << k_ra_rsip_word_shift));
    const uint32_t word = *ra_rsip_reg32(off);
    internal_unpack_le(word, &out[i]);
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
    lane = (uint8_t)((lane + 1U) & (k_ra_rsip_aes_block_w - 1U));
  }
  if (i < len) {
    const ra_rsip_off_t off =
      (ra_rsip_off_t)(k_ra_rsip_off_data_out0 + (uint16_t)((uint32_t)lane << k_ra_rsip_word_shift));
    const uint32_t word = *ra_rsip_reg32(off);
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      out[i + b] = (uint8_t)((word >> (b * k_ra_rsip_byte_bits)) & k_ra_rsip_byte_mask);
    }
  }
}

/** @brief Implementation of `internal_push_iv_lanes()` -- 4-lane LE IV window writer. */
void internal_push_iv_lanes(ra_rsip_off_t base, const uint8_t* iv)
{
  for (uint32_t w = 0U; w < k_ra_rsip_iv_words; ++w) {
    /* Computed lane offset is a HUM-defined register, not an enumerator. */
    const ra_rsip_off_t off =
      (ra_rsip_off_t)( // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
        (uint32_t)base + (uint16_t)(w << k_ra_rsip_word_shift));
    *ra_rsip_reg32(off) = internal_pack_le(&iv[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
}

/* Push a 16-byte IV / nonce into the SYM_IV0 -- see surrounding code and HUM citations. */
static void internal_push_iv(const uint8_t* iv)
{
  if (iv == nullptr) {
    return;
  }
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  internal_push_iv_lanes(k_ra_rsip_off_sym_iv0, iv);
}

/** @brief Implementation of `internal_push_handle_body()` -- KEY_STAGE body word stream. */
void internal_push_handle_body(const ra_rsip_key_handle_t* handle)
{
  for (uint32_t w = 0U; w < handle->body_words; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_key_stage) = handle->body[w];
  }
}

void internal_load_handle(const ra_rsip_key_handle_t* handle)
{
  if (handle == nullptr) {
    return;
  }
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_sym_keyh) = handle->alg;
  internal_push_handle_body(handle);
}

/* Issue an OEM key-install opcode and read the wrapped body back -- see surrounding code and HUM citations. */
static ra_err_t internal_oem_install(ra_rsip_oem_cmd_t     cmd,
                                     const uint8_t*        iv,
                                     const uint8_t*        src,
                                     uint32_t              src_len,
                                     ra_rsip_key_handle_t* out)
{
  const uint32_t words = internal_handle_words_for(cmd);
  if (words == 0U) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  /* Set the OEM opcode + push the IV + plaintext body. */
  *ra_rsip_reg32(k_ra_rsip_off_oem_ctrl) = (uint32_t)cmd;
  *ra_rsip_reg32(k_ra_rsip_off_oem_arg)  = src_len;

  if (iv != nullptr) {
    for (uint32_t w = 0U; w < k_ra_rsip_iv_words; ++w) {
      *ra_rsip_reg32(k_ra_rsip_off_oem_iv) =
        internal_pack_le(&iv[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
    }
  }
  if (src != nullptr) {
    internal_push_data(src, src_len);
  }

  /* Fire the install command via MBOX. */
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = (uint32_t)cmd;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }

  out->alg        = (uint32_t)cmd;
  out->body_words = words;
  /* Pull body words back from the staging port. */
  for (uint32_t w = 0U; w < words; ++w) {
    out->body[w] = *ra_rsip_reg32(k_ra_rsip_off_key_stage);
  }
  /* Zero the remainder so unused bytes don't leak old data. */
  for (uint32_t w = words; w < (uint32_t)k_ra_rsip_handle_words_rsa4096_priv; ++w) {
    out->body[w] = 0U;
  }
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: key install
 * ===========================================================================
 */

ra_err_t ra_rsip_aes128_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return internal_oem_install(k_ra_rsip_oem_cmd_aes128,
                              nullptr,
                              key,
                              (uint32_t)k_ra_rsip_aes128_key_bytes,
                              out);
}

ra_err_t ra_rsip_aes192_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return internal_oem_install(k_ra_rsip_oem_cmd_aes192,
                              nullptr,
                              key,
                              (uint32_t)k_ra_rsip_aes192_key_bytes,
                              out);
}

ra_err_t ra_rsip_aes256_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return internal_oem_install(k_ra_rsip_oem_cmd_aes256,
                              nullptr,
                              key,
                              (uint32_t)k_ra_rsip_aes256_key_bytes,
                              out);
}

ra_err_t ra_rsip_chacha20_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return internal_oem_install(k_ra_rsip_oem_cmd_chacha20,
                              nullptr,
                              key,
                              (uint32_t)k_ra_rsip_chacha_key_bytes,
                              out);
}

ra_err_t ra_rsip_hmac_install_plain(ra_rsip_oem_cmd_t     alg,
                                    const uint8_t*        key,
                                    uint32_t              key_len,
                                    ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (key_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  switch (alg) {
    case k_ra_rsip_oem_cmd_hmac_sha224:
    case k_ra_rsip_oem_cmd_hmac_sha256:
    case k_ra_rsip_oem_cmd_hmac_sha384:
    case k_ra_rsip_oem_cmd_hmac_sha512:
    case k_ra_rsip_oem_cmd_hmac_sha512_224:
    case k_ra_rsip_oem_cmd_hmac_sha512_256:
      break;
    default:
      return k_ra_err_invalid_arg;
  }
  return internal_oem_install(alg, nullptr, key, key_len, out);
}

ra_err_t ra_rsip_oem_install(ra_rsip_oem_cmd_t     cmd,
                             const uint8_t*        iv,
                             const uint8_t*        oem_blob,
                             uint32_t              blob_len,
                             ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR(iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR(oem_blob, s_tag, "oem_blob must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (cmd == k_ra_rsip_oem_cmd_invalid) {
    return k_ra_err_invalid_arg;
  }
  if (blob_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  return internal_oem_install(cmd, iv, oem_blob, blob_len, out);
}

/* ===========================================================================
 * Round-3 entry points: AES symmetric cipher
 * ===========================================================================
 */

/* Drive one block / multi-block cipher transaction -- see surrounding code and HUM citations. */
static ra_err_t internal_sym_run(const ra_rsip_key_handle_t* key,
                                 uint8_t                     alg_byte,
                                 ra_rsip_aes_mode_t          mode,
                                 ra_rsip_aes_dir_t           dir,
                                 const uint8_t*              iv,
                                 const uint8_t*              in,
                                 uint8_t*                    out,
                                 uint32_t                    len)
{
  internal_load_handle(key);
  internal_push_iv(iv);

  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  /* SYM_CTRL = (dir << 16) | (mode << 8) | alg_byte. */
  const uint32_t cmd = ((uint32_t)dir << k_ra_rsip_byte_shift_2) |
                       ((uint32_t)mode << k_ra_rsip_byte_bits) | (uint32_t)alg_byte;
  *ra_rsip_reg32(k_ra_rsip_off_sym_ctrl) = cmd;

  internal_push_data(in, len);
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = cmd;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_pull_data(out, len);
  return k_ra_ok;
}

uint8_t internal_aes_alg_byte(uint32_t alg)
{
  switch (alg) {
    case k_ra_rsip_oem_cmd_aes128:
      return k_ra_rsip_sym_alg_aes128;
    case k_ra_rsip_oem_cmd_aes192:
      return k_ra_rsip_sym_alg_aes192;
    case k_ra_rsip_oem_cmd_aes256:
      return k_ra_rsip_sym_alg_aes256;
    default:
      return 0U;
  }
}

ra_err_t ra_rsip_aes_cipher(const ra_rsip_key_handle_t* key,
                            ra_rsip_aes_mode_t          mode,
                            ra_rsip_aes_dir_t           dir,
                            const uint8_t*              iv,
                            const uint8_t*              in,
                            uint8_t*                    out,
                            uint32_t                    len)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if ((mode == k_ra_rsip_aes_mode_gcm) || (mode == k_ra_rsip_aes_mode_ccm)) {
    return k_ra_err_invalid_arg; /* AEAD has dedicated entry points. */
  }
  if (((mode == k_ra_rsip_aes_mode_ecb) || (mode == k_ra_rsip_aes_mode_cbc) ||
       (mode == k_ra_rsip_aes_mode_cmac)) &&
      ((len & ((uint32_t)k_ra_rsip_aes_block_bytes - 1U)) != 0U)) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t alg_byte = internal_aes_alg_byte(key->alg);
  if (alg_byte == 0U) {
    return k_ra_err_invalid_arg;
  }
  return internal_sym_run(key, alg_byte, mode, dir, iv, in, out, len);
}

/* Push 16 tag bytes through the SYM_TAG port (decrypt path) -- see surrounding code and HUM citations. */
static void internal_aead_push_tag(const uint8_t* tag)
{
  for (uint32_t w = 0U; w < k_ra_rsip_aes_block_w; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_sym_tag) =
      internal_pack_le(&tag[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
}

/* Pull 16 tag bytes from the SYM_TAG port (encrypt path) -- see surrounding code and HUM citations. */
static void internal_aead_pull_tag(uint8_t* tag)
{
  for (uint32_t w = 0U; w < k_ra_rsip_aes_block_w; ++w) {
    const uint32_t word = *ra_rsip_reg32(k_ra_rsip_off_sym_tag);
    internal_unpack_le(word, &tag[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
}

/* internal aead run -- see surrounding code and HUM citations. */
static ra_err_t internal_aead_run(const ra_rsip_key_handle_t* key,
                                  uint8_t                     alg_byte,
                                  ra_rsip_aes_mode_t          mode,
                                  ra_rsip_aes_dir_t           dir,
                                  const uint8_t*              iv,
                                  const uint8_t*              aad,
                                  uint32_t                    aad_len,
                                  const uint8_t*              in,
                                  uint8_t*                    out,
                                  uint32_t                    in_len,
                                  uint8_t*                    tag)
{
  internal_load_handle(key);
  internal_push_iv(iv);
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  /* AAD + body length descriptors. */
  *ra_rsip_reg32(k_ra_rsip_off_sym_aad_len) = aad_len;
  *ra_rsip_reg32(k_ra_rsip_off_sym_pt_len)  = in_len;

  if ((aad != nullptr) && (aad_len > 0U)) {
    /* Push AAD bytes one word at a time through the AAD lane. */
    internal_push_bytes_to_port(k_ra_rsip_off_sym_aad_in, aad, aad_len);
  }

  /* On decrypt, push the supplied tag in for verification. */
  if (dir == k_ra_rsip_dir_decrypt) {
    internal_aead_push_tag(tag);
  }

  const uint32_t cmd = ((uint32_t)dir << k_ra_rsip_byte_shift_2) |
                       ((uint32_t)mode << k_ra_rsip_byte_bits) | (uint32_t)alg_byte;
  *ra_rsip_reg32(k_ra_rsip_off_sym_ctrl) = cmd;
  internal_push_data(in, in_len);
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = cmd;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_pull_data(out, in_len);
  /* On encrypt, read the engine-computed tag back. */
  if (dir == k_ra_rsip_dir_encrypt) {
    internal_aead_pull_tag(tag);
  }
  return k_ra_ok;
}

ra_err_t ra_rsip_aes_gcm(const ra_rsip_key_handle_t* key,
                         ra_rsip_aes_dir_t           dir,
                         const uint8_t*              iv,
                         const uint8_t*              aad,
                         uint32_t                    aad_len,
                         const uint8_t*              in,
                         uint8_t*                    out,
                         uint32_t                    in_len,
                         uint8_t*                    tag)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA_CHECK_NULL_PTR(tag, s_tag, "tag must not be nullptr");
  const uint8_t alg_byte = internal_aes_alg_byte(key->alg);
  if (alg_byte == 0U) {
    return k_ra_err_invalid_arg;
  }
  return internal_aead_run(key,
                           alg_byte,
                           k_ra_rsip_aes_mode_gcm,
                           dir,
                           iv,
                           aad,
                           aad_len,
                           in,
                           out,
                           in_len,
                           tag);
}

ra_err_t ra_rsip_aes_ccm(const ra_rsip_key_handle_t* key,
                         ra_rsip_aes_dir_t           dir,
                         const uint8_t*              iv,
                         const uint8_t*              aad,
                         uint32_t                    aad_len,
                         const uint8_t*              in,
                         uint8_t*                    out,
                         uint32_t                    in_len,
                         uint8_t*                    tag)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA_CHECK_NULL_PTR(tag, s_tag, "tag must not be nullptr");
  const uint8_t alg_byte = internal_aes_alg_byte(key->alg);
  if (alg_byte == 0U) {
    return k_ra_err_invalid_arg;
  }
  return internal_aead_run(key,
                           alg_byte,
                           k_ra_rsip_aes_mode_ccm,
                           dir,
                           iv,
                           aad,
                           aad_len,
                           in,
                           out,
                           in_len,
                           tag);
}

/* ===========================================================================
 * Round-3 entry points: ChaCha20 + Poly1305
 * ===========================================================================
 */

/* Stage the ChaCha20-style 16-byte IV (counter || 12-byte nonce) -- see surrounding code and HUM citations. */
static void internal_chacha20_push_iv(uint32_t counter, const uint8_t* nonce)
{
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  /* ChaCha20 IV layout: counter || 12-byte nonce. */
  *ra_rsip_reg32(k_ra_rsip_off_sym_iv0) = counter;
  *ra_rsip_reg32(k_ra_rsip_off_sym_iv1) = internal_pack_le(&nonce[0]);
  *ra_rsip_reg32(k_ra_rsip_off_sym_iv2) =
    internal_pack_le(&nonce[(uint32_t)k_ra_rsip_trng_word_bytes]);
  *ra_rsip_reg32(k_ra_rsip_off_sym_iv3) =
    internal_pack_le(&nonce[(size_t)2U * (size_t)k_ra_rsip_trng_word_bytes]);
}

ra_err_t ra_rsip_chacha20(const ra_rsip_key_handle_t* key,
                          ra_rsip_aes_dir_t           dir,
                          const uint8_t*              nonce,
                          uint32_t                    counter,
                          const uint8_t*              in,
                          uint8_t*                    out,
                          uint32_t                    len)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(nonce, s_tag, "nonce must not be nullptr");
  RA_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (key->alg != k_ra_rsip_oem_cmd_chacha20) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  internal_chacha20_push_iv(counter, nonce);

  const uint32_t cmd                     = ((uint32_t)dir << k_ra_rsip_byte_shift_2) |
                                           (k_ra_rsip_chacha_op_encrypt << k_ra_rsip_byte_bits) |
                                           (uint32_t)k_ra_rsip_sym_alg_chacha20;
  *ra_rsip_reg32(k_ra_rsip_off_sym_ctrl) = cmd;
  internal_push_data(in, len);
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = cmd;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_pull_data(out, len);
  return k_ra_ok;
}

ra_err_t ra_rsip_chacha20_poly1305(const ra_rsip_key_handle_t* key,
                                   ra_rsip_aes_dir_t           dir,
                                   const uint8_t*              nonce,
                                   const uint8_t*              aad,
                                   uint32_t                    aad_len,
                                   const uint8_t*              in,
                                   uint8_t*                    out,
                                   uint32_t                    in_len,
                                   uint8_t*                    tag)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(nonce, s_tag, "nonce must not be nullptr");
  RA_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA_CHECK_NULL_PTR(tag, s_tag, "tag must not be nullptr");
  if (key->alg != k_ra_rsip_oem_cmd_chacha20) {
    return k_ra_err_invalid_arg;
  }
  return internal_aead_run(key,
                           k_ra_rsip_sym_alg_chacha20,
                           k_ra_rsip_aes_mode_gcm,
                           dir,
                           nonce,
                           aad,
                           aad_len,
                           in,
                           out,
                           in_len,
                           tag);
}

ra_err_t
ra_rsip_poly1305(const uint8_t* one_time_key, const uint8_t* msg, uint32_t msg_len, uint8_t* tag)
{
  RA_CHECK_NULL_PTR(one_time_key, s_tag, "one_time_key must not be nullptr");
  RA_CHECK_NULL_PTR(tag, s_tag, "tag must not be nullptr");
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra_err_null_ptr;
  }

  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  /* Stage one-time key as a ChaCha20 key. */
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_handle_words_chacha20; ++w) {
    if (w < ((uint32_t)k_ra_rsip_chacha_key_bytes / (uint32_t)k_ra_rsip_trng_word_bytes)) {
      *ra_rsip_reg32(k_ra_rsip_off_key_stage) =
        internal_pack_le(&one_time_key[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
    } else {
      *ra_rsip_reg32(k_ra_rsip_off_key_stage) = 0U;
    }
  }
  *ra_rsip_reg32(k_ra_rsip_off_sym_ctrl) = k_ra_rsip_chacha_op_poly1305_mac;
  if (msg_len > 0U) {
    internal_push_data(msg, msg_len);
  }
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = k_ra_rsip_chacha_op_poly1305_mac;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  for (uint32_t w = 0U; w < k_ra_rsip_aes_block_w; ++w) {
    const uint32_t word = *ra_rsip_reg32(k_ra_rsip_off_sym_tag);
    internal_unpack_le(word, &tag[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
  return k_ra_ok;
}
