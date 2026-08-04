/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rsip_asym.c
 * @brief RSIP-E50D hash / HMAC + key-management (fail-closed)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Hash / HMAC + key-management slice of the RA8D2 RSIP-E50D HAL driver, split
 * out of ``ra8_rsip.c`` to keep every translation unit under the file-size
 * budget.
 *
 * The generic multi-algorithm hash family (SHA-2 / SHA-3 / SHAKE) + HMAC and
 * the whole key-management surface -- the OEM boot-loader anti-rollback
 * counter, the wrapped-key vault, the KEK-backed key wrap / unwrap engine,
 * HKDF / HUK / UID key derivation, and DOTF key delivery routing -- are
 * FAIL-CLOSED in production. HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" is a
 * six-page feature overview (p 3302-3307) with no hash / key command-register
 * map, so the ``HUM Ch 52.1`` / ``52.2.3`` citations that used to sit on
 * those register pokes were fabricated (they passed cite_check while being
 * false, exactly the #214 / #181 finding). The off-target-only command path is
 * gated behind the stub-crypto guard and a production build returns
 * ``k_ra8_err_not_supported`` -- never a plausible-looking wrong digest, MAC,
 * wrapped key, or derived key. The only real hash path on this part is
 * ``ra8_rsip_sha256`` -> the software SHA-256 backend in ``ra8_rsip.c`` (proven
 * in rsip_sha256_kat); it is untouched. Any real hash / HMAC / KDF need is
 * served by tf-psa-crypto on the M85 (silicon-proven in psa_crypto_hil),
 * issue #215.
 *
 * The device-security paths (device lifecycle, the three debug-authorisation
 * levels, the tamper subsystem, and the SPA / DPA side-channel arm) were split
 * out into ``ra8_rsip_devsec.c`` and fail-closed the same way (issue #216): they
 * drove an invented "RSIP security-state" register block cited to HUM Ch 51,
 * which is a prose feature index with no register map.
 *
 * Cross-TU primitives shared with ``ra8_rsip.c`` and ``ra8_rsip_cipher.c`` are
 * declared in ``ra8_rsip_internal.h``. The asymmetric byte-lane
 * (``internal_asym_push`` / ``internal_asym_pull``) + handle-tail
 * (``internal_zero_handle_tail``) helpers shared with ``ra8_rsip_rsa.c`` /
 * ``ra8_rsip_ecc.c`` are declared in ``ra8_rsip_asym_internal.h`` and defined
 * below; because every consumer references them only from inside its own
 * stub-crypto guard, they too live inside the guard here and are absent from a
 * production image.
 *
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_rsip.h"
#include "ra8_rsip_asym_internal.h"
#include "ra8_rsip_internal.h"
#include "ra8_rsip_regs.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ``ra8_log_*`` call in this TU.
 *
 * @details
 * Kept short ("RSIP") so it fits in the fixed-width log prefix without
 * truncation. Each RSIP translation unit keeps its own private copy.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const char* s_tag = "RSIP";

/*
 * The RSIP-E50D generic hash / HMAC family and the whole key-management surface
 * (OEM anti-rollback counter, wrapped-key vault, KEK wrap / unwrap, HKDF / HUK /
 * UID key derivation, DOTF key routing) are NOT backed by a documented register
 * interface on this silicon. HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" is a
 * six-page feature overview (p 3302-3307) with no hash / key command-register
 * map; the vendor engine is driven through an encrypted firmware mailbox, not
 * the MMIO opcodes modelled below. The command-path bodies here only round-trip
 * the host register fake; they do NOT compute a real digest, HMAC, wrapped
 * key, or derived key. They compile only under the insecure-stub / fake
 * guard so a production image gets the fail-closed #else and can never mistake
 * these bytes for a valid hash, MAC, or key handle. The only real hash path is
 * ra8_rsip_sha256 -> the software SHA-256 backend in ra8_rsip.c (untouched); any
 * real hash / HMAC / KDF need is served by tf-psa-crypto on the M85,
 * silicon-proven in psa_crypto_hil (issue #215). The register pokes below
 * therefore carry NO HUM citation: there is no real register map to cite. The
 * former "HUM Ch 52.1" / "52.2.3" citations were fabricated and are removed.
 */
#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)

/* ===========================================================================
 * Round-3 entry points: hash + HMAC (off-target-only fiction)
 * ===========================================================================
 */

/* Map a hash algorithm selector to its natural digest length -- see surrounding code and HUM citations. */
RA8_INTERNAL
static uint32_t internal_hash_size(ra8_rsip_hash_alg_t alg)
{
  switch (alg) {
    case k_ra8_rsip_hash_sha224:
    case k_ra8_rsip_hash_sha512_224:
    case k_ra8_rsip_hash_sha3_224:
      return (uint32_t)k_ra8_rsip_sha224_digest_bytes;
    case k_ra8_rsip_hash_sha256:
    case k_ra8_rsip_hash_sha512_256:
    case k_ra8_rsip_hash_sha3_256:
      return (uint32_t)k_ra8_rsip_sha256_digest_bytes;
    case k_ra8_rsip_hash_sha384:
    case k_ra8_rsip_hash_sha3_384:
      return (uint32_t)k_ra8_rsip_sha384_digest_bytes;
    case k_ra8_rsip_hash_sha512:
    case k_ra8_rsip_hash_sha3_512:
      return (uint32_t)k_ra8_rsip_sha512_digest_bytes;
    case k_ra8_rsip_hash_shake128:
    case k_ra8_rsip_hash_shake256:
      /* Variable-length output: caller supplies. */
      return 1U;
    default:
      return 0U;
  }
}

/* Validate the hash + digest length arguments before any MMIO -- see surrounding code and HUM citations. */
RA8_INTERNAL
static ra8_err_t internal_hash_validate(ra8_rsip_hash_alg_t alg,
                                        const uint8_t*      msg,
                                        uint32_t            msg_len,
                                        uint32_t            digest_len,
                                        uint32_t*           needed)
{
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  const uint32_t n = internal_hash_size(alg);
  if (n == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((alg != k_ra8_rsip_hash_shake128) && (alg != k_ra8_rsip_hash_shake256) && (digest_len < n)) {
    return k_ra8_err_invalid_arg;
  }
  *needed = n;
  return k_ra8_ok;
}

/* Read a variable-length digest from the modelled HASH_DIGEST window -- see implementation for details. */
RA8_INTERNAL
static void internal_hash_pull_digest(uint8_t* digest, uint32_t to_read)
{
  uint32_t i   = 0U;
  uint32_t off = (uint32_t)k_ra8_rsip_off_hash_digest;
  while ((i + (uint32_t)k_ra8_rsip_trng_word_bytes) <= to_read) {
    /* Computed digest-word offset is a modelled register location (off-target-only
     * fiction), not a literal enumerator -- the analyzer can't see that. */
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) -- computed digest-word offset, not an enumerator.
    const uint32_t word = *ra8_rsip_reg32((ra8_rsip_off_t)off);
    internal_unpack_le(word, &digest[i]);
    i += (uint32_t)k_ra8_rsip_trng_word_bytes;
    off += (uint32_t)k_ra8_rsip_trng_word_bytes;
  }
  if (i < to_read) {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) -- computed digest-word offset, not an enumerator.
    const uint32_t word = *ra8_rsip_reg32((ra8_rsip_off_t)off);
    for (uint32_t b = 0U; (i + b) < to_read; ++b) {
      digest[i + b] = (uint8_t)((word >> (b * k_ra8_rsip_byte_bits)) & k_ra8_rsip_byte_mask);
    }
  }
  *ra8_rsip_reg32(k_ra8_rsip_off_hash_status) &= ~k_ra8_rsip_mask_isr_done;
}

ra8_err_t ra8_rsip_hash(ra8_rsip_hash_alg_t alg,
                        const uint8_t*      msg,
                        uint32_t            msg_len,
                        uint8_t*            digest,
                        uint32_t            digest_len)
{
  RA8_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  uint32_t        needed = 0U;
  const ra8_err_t v_err  = internal_hash_validate(alg, msg, msg_len, digest_len, &needed);
  RA8_RETURN_ON_ERROR(v_err, s_tag, "rsip_hash: validate"); /* GCOVR_EXCL_BR_LINE */

  *ra8_rsip_reg32(k_ra8_rsip_off_hash_ctrl) = (uint32_t)alg;

  if (msg_len > 0U) {
    internal_push_bytes_to_port(k_ra8_rsip_off_hash_data_in, msg, msg_len);
  }

  /* Wait for DONE; the bounded poll routes through the host wait seam. */
  const ra8_err_t wait_err = internal_hash_wait_done();
  RA8_RETURN_ON_ERROR(wait_err, s_tag, "rsip_hash: hash done");

  /* Read digest_len for SHAKE; algo-natural otherwise. */
  const uint32_t to_read =
    ((alg == k_ra8_rsip_hash_shake128) || (alg == k_ra8_rsip_hash_shake256)) ? digest_len : needed;
  internal_hash_pull_digest(digest, to_read);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_hmac(const ra8_rsip_key_handle_t* key,
                        const uint8_t*               msg,
                        uint32_t                     msg_len,
                        uint8_t*                     mac,
                        uint32_t                     mac_len)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(mac, s_tag, "mac must not be nullptr");
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  /* Determine the underlying hash size from the install opcode. */
  uint32_t needed = 0U;
  switch (key->alg) {
    case k_ra8_rsip_oem_cmd_hmac_sha224:
    case k_ra8_rsip_oem_cmd_hmac_sha512_224:
      needed = (uint32_t)k_ra8_rsip_sha224_digest_bytes;
      break;
    case k_ra8_rsip_oem_cmd_hmac_sha256:
    case k_ra8_rsip_oem_cmd_hmac_sha512_256:
      needed = (uint32_t)k_ra8_rsip_sha256_digest_bytes;
      break;
    case k_ra8_rsip_oem_cmd_hmac_sha384:
      needed = (uint32_t)k_ra8_rsip_sha384_digest_bytes;
      break;
    case k_ra8_rsip_oem_cmd_hmac_sha512:
      needed = (uint32_t)k_ra8_rsip_sha512_digest_bytes;
      break;
    default:
      return k_ra8_err_invalid_arg;
  }
  if (mac_len < needed) {
    return k_ra8_err_invalid_arg;
  }
  /* Stage HMAC key handle, then drive the hash unit in HMAC mode. */
  *ra8_rsip_reg32(k_ra8_rsip_off_hash_hmac) = key->alg;
  internal_load_handle(key);
  return ra8_rsip_hash(k_ra8_rsip_hash_sha256, msg, msg_len, mac, needed);
}

/* ===========================================================================
 * Round-3: asymmetric byte-lane + handle-tail helpers
 *
 * Shared with ra8_rsip_rsa.c / ra8_rsip_ecc.c via ra8_rsip_asym_internal.h. Every
 * consumer references them only from inside its own stub-crypto guard, so they
 * live inside the guard here and are absent from a production image.
 * ===========================================================================
 */

/* Zero-fill the unused tail of a key-handle body buffer -- see ra8_rsip_asym_internal.h. */
void internal_zero_handle_tail(ra8_rsip_key_handle_t* handle, uint32_t words)
{
  for (uint32_t w = words; w < (uint32_t)k_ra8_rsip_handle_words_rsa4096_priv; ++w) {
    handle->body[w] = 0U;
  }
}

/* Push a buffer through an asymmetric input lane (off-target-only fiction) -- see implementation for details. */
void internal_asym_push(ra8_rsip_off_t off, const uint8_t* buf, uint32_t len)
{
  uint32_t i = 0U;
  while ((i + (uint32_t)k_ra8_rsip_trng_word_bytes) <= len) {
    *ra8_rsip_reg32(off) = internal_pack_le(&buf[i]);
    i += (uint32_t)k_ra8_rsip_trng_word_bytes;
  }
  if (i < len) {
    uint32_t tail = 0U;
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      tail |= ((uint32_t)buf[i + b]) << (b * k_ra8_rsip_byte_bits);
    }
    *ra8_rsip_reg32(off) = tail;
  }
}

/* Pull a buffer back through an asymmetric output lane (off-target-only fiction) -- see implementation for details. */
void internal_asym_pull(ra8_rsip_off_t off, uint8_t* buf, uint32_t len)
{
  uint32_t i = 0U;
  while ((i + (uint32_t)k_ra8_rsip_trng_word_bytes) <= len) {
    internal_unpack_le(*ra8_rsip_reg32(off), &buf[i]);
    i += (uint32_t)k_ra8_rsip_trng_word_bytes;
  }
  if (i < len) {
    const uint32_t word = *ra8_rsip_reg32(off);
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      buf[i + b] = (uint8_t)((word >> (b * k_ra8_rsip_byte_bits)) & k_ra8_rsip_byte_mask);
    }
  }
}

/* ===========================================================================
 * Round-3 entry points: OEM boot loader version (anti-rollback, off-target-only fiction)
 * ===========================================================================
 */

ra8_err_t ra8_rsip_oem_bl_version_get(uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  *out = *ra8_rsip_reg32(k_ra8_rsip_off_oem_bl_ver);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_oem_bl_version_increment(void)
{
  if (*ra8_rsip_reg32(k_ra8_rsip_off_oem_bl_lock) != 0U) {
    return k_ra8_err_invalid_state;
  }
  /* W1 trigger; engine increments the latched counter. */
  *ra8_rsip_reg32(k_ra8_rsip_off_oem_bl_inc) = 1U;
  *ra8_rsip_reg32(k_ra8_rsip_off_oem_bl_ver) = *ra8_rsip_reg32(k_ra8_rsip_off_oem_bl_ver) + 1U;
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_oem_bl_version_lock(void)
{
  *ra8_rsip_reg32(k_ra8_rsip_off_oem_bl_lock) = 1U;
  return k_ra8_ok;
}

/* ===========================================================================
 * Round-3 entry points: wrapped-key vault (off-target-only fiction)
 * ===========================================================================
 */

/* Issue a vault command and wait for completion -- see implementation for details. */
RA8_INTERNAL
static ra8_err_t internal_kv_op(ra8_rsip_kv_op_t op, uint8_t slot)
{
  *ra8_rsip_reg32(k_ra8_rsip_off_kv_slot) = slot;
  *ra8_rsip_reg32(k_ra8_rsip_off_kv_ctrl) = (uint32_t)op;
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op) = (uint32_t)op;
  return internal_complete(k_ra8_rsip_mask_isr_kv_done);
}

ra8_err_t ra8_rsip_kv_read(uint8_t slot, uint8_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (slot >= (uint8_t)k_ra8_rsip_kv_slot_count) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t err = internal_kv_op(k_ra8_rsip_kv_op_read, slot);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t w = 0U; w < k_ra8_rsip_kv_slot_w; ++w) {
    const uint32_t word = *ra8_rsip_reg32(k_ra8_rsip_off_kv_data);
    internal_unpack_le(word, &out[(size_t)w * (size_t)k_ra8_rsip_trng_word_bytes]);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_kv_write(uint8_t slot, const uint8_t* in)
{
  RA8_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
  if (slot >= (uint8_t)k_ra8_rsip_kv_slot_count) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t w = 0U; w < k_ra8_rsip_kv_slot_w; ++w) {
    *ra8_rsip_reg32(k_ra8_rsip_off_kv_data) =
      internal_pack_le(&in[(size_t)w * (size_t)k_ra8_rsip_trng_word_bytes]);
  }
  return internal_kv_op(k_ra8_rsip_kv_op_write, slot);
}

ra8_err_t ra8_rsip_kv_erase(uint8_t slot)
{
  if (slot >= (uint8_t)k_ra8_rsip_kv_slot_count) {
    return k_ra8_err_invalid_arg;
  }
  return internal_kv_op(k_ra8_rsip_kv_op_erase, slot);
}

ra8_err_t ra8_rsip_kv_count(uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  *out = *ra8_rsip_reg32(k_ra8_rsip_off_kv_count);
  return k_ra8_ok;
}

/* ===========================================================================
 * Round-3 entry points: key wrap / unwrap engine (off-target-only fiction)
 * ===========================================================================
 */

/**
 * @brief Stage the wrap engine's KEK selector, body, and IV.
 *
 * @details
 * Both wrap and unwrap start by publishing the KEK algorithm to
 * ``KW_KEK``, streaming the KEK body into the staging port, and
 * loading the 16-byte IV into ``KW_IV0..3``. Centralised here.
 *
 * @param[in] kek KEK handle.
 * @param[in] iv  16-byte IV.
 *
 * @pre ``kek`` and ``iv`` are non-NULL.
 *
 * @post ``KW_KEK`` carries ``kek->alg``.
 * @post ``KEY_STAGE`` has observed ``kek->body_words`` writes.
 * @post ``KW_IV0..3`` reflect ``iv``.
 *
 * @note Internal helper.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 */
RA8_INTERNAL
static void internal_kw_stage_kek(const ra8_rsip_key_handle_t* kek, const uint8_t* iv)
{
  *ra8_rsip_reg32(k_ra8_rsip_off_kw_kek) = kek->alg;
  internal_push_handle_body(kek);
  internal_push_iv_lanes(k_ra8_rsip_off_kw_iv0, iv);
}

/* Stream the wrap-engine output blob (16 words) into a byte buffer -- see implementation for details. */
RA8_INTERNAL
static void internal_kw_pull_blob(uint8_t* blob)
{
  for (uint32_t w = 0U; w < k_ra8_rsip_kv_slot_w; ++w) {
    const uint32_t word = *ra8_rsip_reg32(k_ra8_rsip_off_kw_blob_out);
    internal_unpack_le(word, &blob[(size_t)w * (size_t)k_ra8_rsip_trng_word_bytes]);
  }
}

/* Push the source-handle body into the wrap-engine input FIFO -- see implementation for details. */
RA8_INTERNAL
static void internal_kw_push_src(const ra8_rsip_key_handle_t* src)
{
  *ra8_rsip_reg32(k_ra8_rsip_off_kw_handle) = src->alg;
  for (uint32_t w = 0U; w < src->body_words; ++w) {
    *ra8_rsip_reg32(k_ra8_rsip_off_kw_blob_in) = src->body[w];
  }
}

ra8_err_t ra8_rsip_key_wrap(const ra8_rsip_key_handle_t* kek,
                            const uint8_t*               iv,
                            const ra8_rsip_key_handle_t* src,
                            uint8_t*                     blob)
{
  RA8_CHECK_NULL_PTR(kek, s_tag, "kek must not be nullptr");
  RA8_CHECK_NULL_PTR(iv, s_tag, "iv must not be nullptr");
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(blob, s_tag, "blob must not be nullptr");
  if (internal_aes_alg_byte(kek->alg) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  internal_kw_stage_kek(kek, iv);
  internal_kw_push_src(src);
  *ra8_rsip_reg32(k_ra8_rsip_off_kw_ctrl) = k_ra8_rsip_kw_op_wrap;
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op) = k_ra8_rsip_kw_op_wrap;

  const ra8_err_t err = internal_complete(k_ra8_rsip_mask_isr_done);
  if (err != k_ra8_ok) {
    return err;
  }
  internal_kw_pull_blob(blob);
  return k_ra8_ok;
}

/* Pull the unwrapped algorithm + body into a destination handle -- see implementation for details. */
RA8_INTERNAL
static ra8_err_t internal_kw_pull_handle(ra8_rsip_key_handle_t* dest)
{
  /* Pull the unwrapped algorithm + body out. */
  dest->alg            = *ra8_rsip_reg32(k_ra8_rsip_off_kw_handle);
  const uint32_t words = internal_handle_words_for((ra8_rsip_oem_cmd_t)dest->alg);
  if (words == 0U) {
    return k_ra8_err_hw_error;
  }
  dest->body_words = words;
  for (uint32_t w = 0U; w < words; ++w) {
    dest->body[w] = *ra8_rsip_reg32(k_ra8_rsip_off_kw_blob_out);
  }
  internal_zero_handle_tail(dest, words);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_key_unwrap(const ra8_rsip_key_handle_t* kek,
                              const uint8_t*               iv,
                              const uint8_t*               blob,
                              ra8_rsip_key_handle_t*       dest)
{
  RA8_CHECK_NULL_PTR(kek, s_tag, "kek must not be nullptr");
  RA8_CHECK_NULL_PTR(iv, s_tag, "iv must not be nullptr");
  RA8_CHECK_NULL_PTR(blob, s_tag, "blob must not be nullptr");
  RA8_CHECK_NULL_PTR(dest, s_tag, "dest must not be nullptr");
  if (internal_aes_alg_byte(kek->alg) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  internal_kw_stage_kek(kek, iv);
  for (uint32_t w = 0U; w < k_ra8_rsip_kv_slot_w; ++w) {
    *ra8_rsip_reg32(k_ra8_rsip_off_kw_blob_in) =
      internal_pack_le(&blob[(size_t)w * (size_t)k_ra8_rsip_trng_word_bytes]);
  }
  *ra8_rsip_reg32(k_ra8_rsip_off_kw_ctrl) = k_ra8_rsip_kw_op_unwrap;
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op) = k_ra8_rsip_kw_op_unwrap;

  const ra8_err_t err = internal_complete(k_ra8_rsip_mask_isr_done);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_kw_pull_handle(dest);
}

/* ===========================================================================
 * Round-3 entry points: key derivation (off-target-only fiction)
 * ===========================================================================
 */

/* Validate the KDF arguments before any MMIO is touched -- see surrounding code and HUM citations. */
RA8_INTERNAL
static ra8_err_t internal_kdf_validate(ra8_rsip_kdf_op_t            op,
                                       const ra8_rsip_key_handle_t* ikm,
                                       const uint8_t*               label,
                                       uint32_t                     label_len,
                                       const uint8_t*               salt,
                                       uint32_t                     salt_len,
                                       uint32_t                     out_len)
{
  if ((label == nullptr) && (label_len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  if ((salt == nullptr) && (salt_len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  if (out_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  /* HKDF modes need an IKM handle; HUK / UID modes do not. */
  if (((op == k_ra8_rsip_kdf_op_hkdf_sha256) || (op == k_ra8_rsip_kdf_op_hkdf_sha384) ||
       (op == k_ra8_rsip_kdf_op_hkdf_sha512)) &&
      (ikm == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  return k_ra8_ok;
}

/* Stage the KDF inputs (op + length + optional IKM + label + salt) -- see implementation for details. */
RA8_INTERNAL
static void internal_kdf_stage(ra8_rsip_kdf_op_t            op,
                               const ra8_rsip_key_handle_t* ikm,
                               const uint8_t*               label,
                               uint32_t                     label_len,
                               const uint8_t*               salt,
                               uint32_t                     salt_len,
                               uint32_t                     out_len)
{
  *ra8_rsip_reg32(k_ra8_rsip_off_kdf_ctrl) = (uint32_t)op;
  *ra8_rsip_reg32(k_ra8_rsip_off_kdf_len)  = out_len;
  if (ikm != nullptr) {
    *ra8_rsip_reg32(k_ra8_rsip_off_kdf_ikm) = ikm->alg;
    internal_push_handle_body(ikm);
  }
  if (label_len > 0U) {
    internal_push_bytes_to_port(k_ra8_rsip_off_kdf_label, label, label_len);
  }
  if (salt_len > 0U) {
    internal_push_bytes_to_port(k_ra8_rsip_off_kdf_salt, salt, salt_len);
  }
}

/* Pull the wrapped derived-key handle out of the KDF engine -- see implementation for details. */
RA8_INTERNAL
static void internal_kdf_pull_handle(ra8_rsip_key_handle_t* out)
{
  /* Wrapped derived key delivered through KDF_OUT. */
  out->alg        = *ra8_rsip_reg32(k_ra8_rsip_off_kdf_out);
  out->body_words = (uint32_t)k_ra8_rsip_handle_words_hmac_sha256;
  for (uint32_t w = 0U; w < out->body_words; ++w) {
    out->body[w] = *ra8_rsip_reg32(k_ra8_rsip_off_kdf_out);
  }
  internal_zero_handle_tail(out, out->body_words);
}

ra8_err_t ra8_rsip_kdf(ra8_rsip_kdf_op_t            op,
                       const ra8_rsip_key_handle_t* ikm,
                       const uint8_t*               label,
                       uint32_t                     label_len,
                       const uint8_t*               salt,
                       uint32_t                     salt_len,
                       uint32_t                     out_len,
                       ra8_rsip_key_handle_t*       out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  const ra8_err_t v_err = internal_kdf_validate(op, ikm, label, label_len, salt, salt_len, out_len);
  RA8_RETURN_ON_ERROR(v_err, s_tag, "rsip_kdf: validate"); /* GCOVR_EXCL_BR_LINE */

  internal_kdf_stage(op, ikm, label, label_len, salt, salt_len, out_len);
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op) = (uint32_t)op;

  const ra8_err_t err = internal_complete(k_ra8_rsip_mask_isr_kdf_done);
  if (err != k_ra8_ok) {
    return err;
  }
  internal_kdf_pull_handle(out);
  return k_ra8_ok;
}

/* ===========================================================================
 * Round-3 entry points: DOTF key delivery routing (off-target-only fiction)
 * ===========================================================================
 */

ra8_err_t ra8_rsip_dotf_route(uint8_t which, uint8_t slot, bool on)
{
  if (which > 1U) {
    return k_ra8_err_invalid_arg;
  }
  if (on && (slot >= (uint8_t)k_ra8_rsip_kv_slot_count)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_rsip_off_t off = (which == 0U) ? k_ra8_rsip_off_dotf0_ctrl : k_ra8_rsip_off_dotf1_ctrl;
  /* DOTFn_CTRL = (slot << 16) | route_enable */
  uint32_t word = k_ra8_rsip_dotf_off;
  if (on) {
    word = ((uint32_t)slot << k_ra8_rsip_byte_shift_2) | k_ra8_rsip_dotf_on;
  }
  *ra8_rsip_reg32(off) = word;
  return k_ra8_ok;
}

#else /* production build: neither RA8_INSECURE_STUB_CRYPTO nor RA8_OFF_TARGET */

/*
 * Fail-closed production variant. With no real RSIP hash / HMAC / key-management
 * backend on this silicon, every entry point returns a hard error (never
 * k_ra8_ok) so a production image cannot mistake the fake command-path for a
 * real digest, MAC, wrapped key, or derived key. The only real hash is
 * ra8_rsip_sha256 -> the software SHA-256 backend in ra8_rsip.c; callers needing
 * hash / HMAC / KDF use tf-psa-crypto on the M85 (issue #215).
 */

ra8_err_t ra8_rsip_hash(ra8_rsip_hash_alg_t alg,
                        const uint8_t*      msg,
                        uint32_t            msg_len,
                        uint8_t*            digest,
                        uint32_t            digest_len)
{
  RA8_CHECK_NULL_PTR(digest, s_tag, "hash: digest must not be nullptr");
  (void)alg;
  (void)msg;
  (void)msg_len;
  (void)digest_len;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_hmac(const ra8_rsip_key_handle_t* key,
                        const uint8_t*               msg,
                        uint32_t                     msg_len,
                        uint8_t*                     mac,
                        uint32_t                     mac_len)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "hmac: key must not be nullptr");
  RA8_CHECK_NULL_PTR(mac, s_tag, "hmac: mac must not be nullptr");
  (void)msg;
  (void)msg_len;
  (void)mac_len;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_oem_bl_version_get(uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "oem_bl_version_get: out must not be nullptr");
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_oem_bl_version_increment(void)
{
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_oem_bl_version_lock(void)
{
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_kv_read(uint8_t slot, uint8_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "kv_read: out must not be nullptr");
  (void)slot;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_kv_write(uint8_t slot, const uint8_t* in)
{
  RA8_CHECK_NULL_PTR(in, s_tag, "kv_write: in must not be nullptr");
  (void)slot;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_kv_erase(uint8_t slot)
{
  (void)slot;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_kv_count(uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "kv_count: out must not be nullptr");
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_key_wrap(const ra8_rsip_key_handle_t* kek,
                            const uint8_t*               iv,
                            const ra8_rsip_key_handle_t* src,
                            uint8_t*                     blob)
{
  RA8_CHECK_NULL_PTR(kek, s_tag, "key_wrap: kek must not be nullptr");
  RA8_CHECK_NULL_PTR(blob, s_tag, "key_wrap: blob must not be nullptr");
  (void)iv;
  (void)src;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_key_unwrap(const ra8_rsip_key_handle_t* kek,
                              const uint8_t*               iv,
                              const uint8_t*               blob,
                              ra8_rsip_key_handle_t*       dest)
{
  RA8_CHECK_NULL_PTR(kek, s_tag, "key_unwrap: kek must not be nullptr");
  RA8_CHECK_NULL_PTR(dest, s_tag, "key_unwrap: dest must not be nullptr");
  (void)iv;
  (void)blob;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_kdf(ra8_rsip_kdf_op_t            op,
                       const ra8_rsip_key_handle_t* ikm,
                       const uint8_t*               label,
                       uint32_t                     label_len,
                       const uint8_t*               salt,
                       uint32_t                     salt_len,
                       uint32_t                     out_len,
                       ra8_rsip_key_handle_t*       out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "kdf: out must not be nullptr");
  (void)op;
  (void)ikm;
  (void)label;
  (void)label_len;
  (void)salt;
  (void)salt_len;
  (void)out_len;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_dotf_route(uint8_t which, uint8_t slot, bool on)
{
  (void)which;
  (void)slot;
  (void)on;
  return k_ra8_err_not_supported;
}

#endif /* RA8_INSECURE_STUB_CRYPTO || RA8_OFF_TARGET */
