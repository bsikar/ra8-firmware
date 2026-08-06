/**
 * @file ra8_ota_verify.c
 * @brief Phase-5 OTA signature-verification cluster -- implementation.
 *
 * @details
 * Companion translation unit to ``ra8_ota.c``. Holds the cohesive
 * signature-verification responsibility so the orchestration TU stays
 * under the per-file line budget:
 *
 *   - ``priv_rehash_bank`` re-derives the inactive-bank SHA-256 after
 *     the streamed program pass.
 *   - ``priv_bind_manifest_material`` binds the manifest metadata
 *     (version / URL / size) plus the image digest into the canonical
 *     material the server ECDSA signature authenticates (T5-05).
 *   - ``ra8_ota_verify_signature`` (public API) compares the re-derived
 *     digest under a constant-time equality (T5-12) and then runs the
 *     injected ECDSA verifier over that bound material.
 *
 * The verify path reads the module-static state defined in ``ra8_ota.c``
 * (configuration, state byte, init flag, streaming buffer) through the
 * ``extern`` declarations in ``ra8_ota_internal.h`` and drives state
 * transitions through ``ra8_ota_internal_set_state``. The read-only log
 * tag is duplicated locally (cheap, correct for an immutable literal).
 * No malloc anywhere (NASA Rule 3); every loop has a static upper bound
 * (NASA Rule 2).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_ota.h"
#include "ra8_ota_internal.h"
#include "ra8_secure.h"

/** @brief Module log tag (private copy; immutable literal). */
static const char* const s_tag = "ra8_ota";

/**
 * @brief Re-hash the inactive bank to re-derive the digest after program.
 *
 * @details
 * Re-initialises the SHA accumulator then walks the inactive bank in
 * ``k_ra8_ota_chunk_bytes`` chunks via ``s_ra8_ota_cfg.flash.readback``,
 * feeding each one to ``s_ra8_ota_cfg.crypto.sha256_update``. Finalises
 * into ``out_digest``. Loop is bounded by
 * ``(k_ra8_ota_max_image_bytes / k_ra8_ota_chunk_bytes) + 1``.
 *
 * @param[in]  m          Manifest (provides ``image_size_bytes``).
 * @param[out] out_digest 32-byte SHA-256 destination.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok Digest derived.
 * @retval other   Whatever the readback / crypto callbacks returned.
 *
 * @pre Module is in ``verifying``.
 * @pre Pointers non-NULL.
 * @post On success ``out_digest`` holds SHA-256 of the bank contents.
 * @post On failure ``out_digest`` content is unspecified.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_rehash_bank(const ra8_ota_manifest_t* m, uint8_t out_digest[32])
{
  ra8_err_t e = s_ra8_ota_cfg.crypto.sha256_init(s_ra8_ota_cfg.crypto.ctx);
  if (e != k_ra8_ok) {
    return e;
  }
  uint32_t       offset     = 0U;
  const uint32_t max_chunks = (k_ra8_ota_max_image_bytes / k_ra8_ota_chunk_bytes) + 1U;
  for (uint32_t i = 0U; i < max_chunks; ++i) {
    if (offset >= m->image_size_bytes) {
      break;
    }
    const uint32_t remaining = m->image_size_bytes - offset;
    const uint32_t want = (remaining < k_ra8_ota_chunk_bytes) ? remaining : k_ra8_ota_chunk_bytes;
    e                   = s_ra8_ota_cfg.flash.readback(s_ra8_ota_cfg.flash.ctx,
                                     s_ra8_ota_cfg.flash.inactive_bank_addr + offset,
                                     s_ra8_ota_buf,
                                     want);
    if (e != k_ra8_ok) {
      return e;
    }
    e = s_ra8_ota_cfg.crypto.sha256_update(s_ra8_ota_cfg.crypto.ctx, s_ra8_ota_buf, want);
    if (e != k_ra8_ok) {
      return e;
    }
    offset += want;
  }
  return s_ra8_ota_cfg.crypto.sha256_final(s_ra8_ota_cfg.crypto.ctx, out_digest);
}

/**
 * @enum ra8_ota_bind_field_t
 * @brief Fixed byte-widths bound into the OTA signature material (T5-05).
 *
 * @details
 * The manifest metadata is bound into the signed digest with a fixed-width
 * canonical layout: the NUL-padded ``version`` / ``image_url`` char arrays plus
 * a 4-byte little-endian image size, then the image digest.
 *
 * @see priv_bind_manifest_material
 */
typedef enum : uint8_t {
  k_ra8_ota_size_field_bytes = 4U, /**< Little-endian width of ``image_size_bytes``. */
  k_ra8_ota_octet_bits       = 8U, /**< Bits per octet for the LE size spread.       */
} ra8_ota_bind_field_t;

/**
 * @brief Bind manifest metadata (version/url/size) into the signed digest.
 *
 * @details
 * The server ECDSA signature must authenticate the manifest metadata an
 * attacker would tamper -- not just the image bytes -- so this recomputes the
 * digest the verify runs over as ``SHA-256`` of ``version[32]``, ``image_url[256]``,
 * the 4-byte little-endian image size, then ``image_digest`` concatenated in that
 * order (T5-05). Every field is fixed-width (the NUL-padded ``version`` / ``image_url``
 * char arrays and a 4-byte little-endian size), so the concatenation is an
 * unambiguous canonical encoding; ``ra8_ota_internal_manifest_decode`` zero-fills
 * the manifest before decode so the padding is deterministic. The hash streams
 * through the injected SHA-256 interface, the same engine that produced
 * ``image_digest``.
 *
 * @param[in]  manifest     Decoded manifest (metadata + digest); non-NULL.
 * @param[in]  image_digest ::k_ra8_ota_sha256_bytes body digest; non-NULL.
 * @param[out] out_bound    ::k_ra8_ota_sha256_bytes signed-material digest; non-NULL.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok           ``out_bound`` holds the metadata-bound digest.
 * @retval k_ra8_err_null_ptr A pointer argument was NULL.
 * @retval other             SHA-256 interface error propagated from a step.
 *
 * @pre The injected crypto SHA-256 interface is wired.
 * @pre ``out_bound`` addresses ::k_ra8_ota_sha256_bytes writable bytes.
 * @post On ``k_ra8_ok`` the signature authority runs over metadata + image digest.
 * @post No manifest bytes are modified.
 *
 * @note Static helper; not thread-safe (shares the single SHA context).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_bind_manifest_material(const ra8_ota_manifest_t* manifest,
                                             const uint8_t*            image_digest,
                                             uint8_t out_bound[k_ra8_ota_sha256_bytes])
{
  RA8_CHECK_NULL_PTR(manifest, s_tag, "bind: manifest");
  RA8_CHECK_NULL_PTR(image_digest, s_tag, "bind: image_digest");
  RA8_CHECK_NULL_PTR(out_bound, s_tag, "bind: out_bound");

  uint8_t size_le[k_ra8_ota_size_field_bytes] = {};
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_ota_size_field_bytes; ++i) {
    size_le[i] =
      (uint8_t)(manifest->image_size_bytes >> ((uint32_t)i * (uint32_t)k_ra8_ota_octet_bits));
  }

  /* Parallel arrays: the fixed-width segments hashed in order, and their byte
   * counts. Kept in step by the static_assert below. */
  const uint8_t* const segments[] = {
    (const uint8_t*)manifest->version,
    (const uint8_t*)manifest->image_url,
    size_le,
    image_digest,
  };
  const uint32_t lengths[] = {
    (uint32_t)k_ra8_ota_version_str_bytes,
    (uint32_t)k_ra8_ota_url_max_bytes,
    (uint32_t)sizeof size_le,
    (uint32_t)k_ra8_ota_sha256_bytes,
  };
  static_assert(sizeof segments / sizeof segments[0] == sizeof lengths / sizeof lengths[0],
                "segments[] and lengths[] must describe the same number of chunks");

  ra8_err_t e = s_ra8_ota_cfg.crypto.sha256_init(s_ra8_ota_cfg.crypto.ctx);
  if (e != k_ra8_ok) {
    return e;
  }
  for (uint8_t i = 0U; i < (uint8_t)(sizeof segments / sizeof segments[0]); ++i) {
    e = s_ra8_ota_cfg.crypto.sha256_update(s_ra8_ota_cfg.crypto.ctx, segments[i], lengths[i]);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return s_ra8_ota_cfg.crypto.sha256_final(s_ra8_ota_cfg.crypto.ctx, out_bound);
}

/**
 * @brief Verify the freshly-programmed bank against the manifest signature.
 *
 * @details
 * Re-hashes the inactive bank via ``priv_rehash_bank`` and compares the digest
 * against ``manifest->image_sha256``. On a match it binds the manifest metadata
 * (version / URL / size) into the signed material via
 * ``priv_bind_manifest_material`` and invokes the configured ECDSA verifier over
 * that metadata-bound digest, so a MITM that alters the declared version
 * (defeating anti-rollback), redirects the URL, or changes the size cannot ride a
 * signature made over the bare image digest (T5-05). On success the state machine
 * lands in ``committing``.
 *
 * @param[in] manifest Manifest used for the download.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok                  Image authenticated.
 * @retval k_ra8_err_not_initialized Module not initialized.
 * @retval k_ra8_err_null_ptr        ``manifest`` was NULL.
 * @retval k_ra8_err_invalid_state   Module not in ``verifying``.
 * @retval k_ra8_err_crc_mismatch    SHA-256 mismatch (image corrupt).
 * @retval k_ra8_err_hw_error        ECDSA verify rejected the signature.
 * @retval other                    Crypto / flash backend error.
 *
 * @pre ``ra8_ota_download_to_inactive_bank`` succeeded.
 * @pre ``manifest`` non-NULL.
 * @post On success state == ``committing``.
 * @post On failure state == ``error``.
 *
 * @see ra8_ota_commit_and_reboot()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
ra8_err_t ra8_ota_verify_signature(const ra8_ota_manifest_t* manifest)
{
  if (!s_ra8_ota_initialized) {
    return k_ra8_err_not_initialized;
  }
  RA8_CHECK_NULL_PTR(manifest, s_tag, "manifest");
  if (s_ra8_ota_state != k_ra8_ota_state_verifying) {
    return k_ra8_err_invalid_state;
  }

  uint8_t   digest[k_ra8_ota_sha256_bytes] = {};
  ra8_err_t e                              = priv_rehash_bank(manifest, digest);
  if (e != k_ra8_ok) {
    ra8_ota_internal_set_state(k_ra8_ota_state_error, e);
    return e;
  }
  /* Constant-time compare: this digest gates the ECDSA signature check, so a
   * data-dependent early-out would leak how many leading bytes of the expected
   * hash matched -- a byte-at-a-time forgery primitive (T5-12). */
  if (!ra8_ct_equal(digest, manifest->image_sha256, k_ra8_ota_sha256_bytes)) {
    ra8_ota_internal_set_state(k_ra8_ota_state_error, k_ra8_err_crc_mismatch);
    return k_ra8_err_crc_mismatch;
  }
  /* Bind the manifest metadata (version/url/size) into the material the ECDSA
   * signature authenticates so a forged version (anti-rollback bypass), a
   * redirected URL, or a changed size cannot ride a signature made over the bare
   * image digest (T5-05). */
  uint8_t bound[k_ra8_ota_sha256_bytes] = {};
  e                                     = priv_bind_manifest_material(manifest, digest, bound);
  if (e != k_ra8_ok) {
    ra8_ota_internal_set_state(k_ra8_ota_state_error, e);
    return e;
  }
  e = s_ra8_ota_cfg.crypto.ecdsa_verify(s_ra8_ota_cfg.crypto.ctx,
                                        s_ra8_ota_cfg.pubkey_handle,
                                        bound,
                                        manifest->signature,
                                        manifest->signature_len);
  if (e != k_ra8_ok) {
    ra8_ota_internal_set_state(k_ra8_ota_state_error, k_ra8_err_hw_error);
    return k_ra8_err_hw_error;
  }
  ra8_ota_internal_set_state(k_ra8_ota_state_committing, k_ra8_ok);
  return k_ra8_ok;
}
