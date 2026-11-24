/**
 * @file ra8_bundle.h
 * @brief Signed N-image firmware update container ("RA8U") wire format and verifier
 *
 * @par Tag
 * [Ring 4 / Service] {World: Any}
 *
 * @details
 * Implements the single-container, single-signature multi-processor bundle
 * specified in `docs/design/unified_update_pipeline.md`:
 *
 * - Fixed 32-byte header (magic `"RA8U"` = `0x52413855`, `bundle_version`,
 *   `image_count`, `payloads_offset`, `total_bytes`, `sig_len`).
 * - Followed by `N` 64-byte image descriptors (`target`, `format`,
 *   `image_version`, `compat_proto_ver`, 32-byte-aligned `offset`, `size`,
 *   `sha256[32]`).
 * - Followed by a 64-byte raw ECDSA-P256 signature over
 *   `SHA-256(header || descriptors[0..N])`.
 * - Followed by 32-byte-aligned payloads starting at `payloads_offset`.
 *
 * Both OTA (`libs/ra8_ota`) and DFU (`libs/ra8_dfu`) consume the exact same
 * signed container format.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_update.h"

/**
 * @enum ra8_bundle_const_t
 * @brief Fixed layout constants and bounds for the `"RA8U"` bundle container.
 */
typedef enum : uint32_t {
  k_ra8_bundle_magic        = 0x52413855U, /**< ASCII "RA8U" magic word.                */
  k_ra8_bundle_format_ver   = 1U,          /**< Supported container format revision.    */
  k_ra8_bundle_header_bytes = 32U,         /**< Fixed serialized bundle header size.    */
  k_ra8_bundle_desc_bytes   = 64U,         /**< Fixed serialized image descriptor size. */
  k_ra8_bundle_sig_bytes    = 64U,         /**< Fixed signature slot size (ECDSA raw).  */
  k_ra8_bundle_page_align   = 32U,         /**< Required payload offset alignment.      */
  k_ra8_bundle_max_images   = 4U,          /**< Maximum image entries per container.    */
} ra8_bundle_const_t;

/**
 * @enum ra8_bundle_target_t
 * @brief Target processor identifiers inside ::ra8_bundle_image_desc_t.
 */
typedef enum : uint32_t {
  k_ra8_bundle_target_ra8_app  = 1U, /**< Primary RA8 Cortex-M85 application image. */
  k_ra8_bundle_target_esp32_c6 = 2U, /**< Companion ESP32-C6 wireless firmware.     */
  k_ra8_bundle_target_display  = 3U, /**< Display controller firmware/asset image.  */
} ra8_bundle_target_t;

/**
 * @enum ra8_bundle_payload_fmt_t
 * @brief Payload encoding kinds recorded per image descriptor.
 */
typedef enum : uint16_t {
  k_ra8_bundle_fmt_raw      = 0U, /**< Unwrapped binary image bytes.             */
  k_ra8_bundle_fmt_rot1_img = 1U, /**< RA8 body followed by 116-byte ROT1 block. */
  k_ra8_bundle_fmt_c6_img   = 2U, /**< Espressif C6 app-image binary.            */
} ra8_bundle_payload_fmt_t;

/**
 * @struct ra8_bundle_hdr_t
 * @brief 32-byte little-endian `"RA8U"` bundle container header.
 */
typedef struct {
  uint32_t magic;           /**< Must equal ::k_ra8_bundle_magic.                    */
  uint16_t format_version;  /**< Must equal ::k_ra8_bundle_format_ver.               */
  uint16_t header_bytes;    /**< Must equal ::k_ra8_bundle_header_bytes.             */
  uint32_t bundle_version;  /**< Monotonic security version for anti-rollback.       */
  uint16_t image_count;     /**< Descriptor count in `[1..k_ra8_bundle_max_images]`. */
  uint16_t flags;           /**< Reserved, must be zero.                             */
  uint32_t total_bytes;     /**< Total container byte length.                        */
  uint32_t payloads_offset; /**< First payload byte offset (`32 + N*64 + 64`).       */
  uint32_t sig_len;         /**< Active ECDSA signature length (`1..64`).            */
  uint32_t reserved;        /**< Reserved, must be zero.                             */
} ra8_bundle_hdr_t;

static_assert(sizeof(ra8_bundle_hdr_t) == 32U, "ra8_bundle_hdr_t must remain 32 bytes");

/**
 * @struct ra8_bundle_image_desc_t
 * @brief 64-byte little-endian descriptor naming one image inside `"RA8U"`.
 */
typedef struct {
  uint32_t target;                            /**< Target processor (::ra8_bundle_target_t). */
  uint16_t format;                            /**< Encoding (::ra8_bundle_payload_fmt_t).    */
  uint16_t flags;                             /**< Per-image policy flags (0 = default).     */
  uint32_t image_version;                     /**< Component version number.                 */
  uint32_t compat_proto_ver;                  /**< Companion link protocol compatibility.    */
  uint32_t offset;                            /**< 32-byte-aligned offset from container 0.  */
  uint32_t size;                              /**< Unpadded payload length in bytes (> 0).   */
  uint32_t reserved0;                         /**< Reserved, must be zero.                   */
  uint32_t reserved1;                         /**< Reserved, must be zero.                   */
  uint8_t  sha256[k_ra8_update_sha256_bytes]; /**< SHA-256 digest over payload bytes.        */
} ra8_bundle_image_desc_t;

static_assert(sizeof(ra8_bundle_image_desc_t) == 64U,
              "ra8_bundle_image_desc_t must remain 64 bytes");

/**
 * @brief Decode and validate a 32-byte `"RA8U"` bundle header
 *
 * @details
 * Verifies magic, format revision, header size, bounded `image_count`,
 * `payloads_offset`, `sig_len`, and `total_bytes <= max_bytes`. Does not read
 * descriptors or verify cryptographic digests.
 *
 * @param[in]  raw       Serialized buffer containing at least 32 header bytes.
 * @param[in]  raw_len   Length of @p raw in bytes.
 * @param[in]  max_bytes Upper container size limit enforced by caller.
 * @param[out] out_hdr   Decoded header destination (non-NULL).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Valid header decoded into @p out_hdr.
 * @retval k_ra8_err_null_ptr    @p raw or @p out_hdr was NULL.
 * @retval k_ra8_err_invalid_size Length, offset, or total_bytes invalid.
 * @retval k_ra8_err_invalid_arg  Magic, format version, flags, or count invalid.
 *
 * @pre @p raw and @p out_hdr are non-NULL.
 * @pre @p max_bytes >= ::k_ra8_bundle_header_bytes.
 * @post On ::k_ra8_ok, @p *out_hdr holds validated header fields.
 * @post On error, @p *out_hdr is left unmodified.
 *
 * @note Thread-safe (pure parser).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bundle_parse_header(const uint8_t*    raw,
                                                uint32_t          raw_len,
                                                uint32_t          max_bytes,
                                                ra8_bundle_hdr_t* out_hdr);

/**
 * @brief Parse and validate a complete `"RA8U"` container header and descriptors
 *
 * @details
 * Decodes the 32-byte header and all `image_count` descriptors, enforcing:
 * valid targets and formats, unique non-overlapping 32-byte-aligned payload
 * spans strictly inside `[payloads_offset, total_bytes)`, and `total_bytes ==
 * container_len`. Does NOT perform SHA-256 or ECDSA verification.
 *
 * @param[in]  container     In-memory bundle container bytes.
 * @param[in]  container_len Length of @p container in bytes.
 * @param[in]  max_bytes     Upper allowed bundle size.
 * @param[out] out_hdr       Validated header output (non-NULL).
 * @param[out] out_descs     Caller array receiving decoded descriptors (non-NULL).
 * @param[in]  descs_cap     Element capacity of @p out_descs.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Header and descriptors structurally valid.
 * @retval k_ra8_err_null_ptr    Required pointer NULL.
 * @retval k_ra8_err_invalid_size Buffer truncated, oversized, or misaligned span.
 * @retval k_ra8_err_invalid_arg  Malformed metadata, overlap, or duplicate target.
 *
 * @pre All output buffers are non-NULL and writable.
 * @pre @p descs_cap >= 1 (up to ::k_ra8_bundle_max_images).
 * @post On ::k_ra8_ok, @p out_descs[0 .. out_hdr->image_count - 1] populated.
 * @post On failure, caller outputs are left untouched.
 *
 * @note Thread-safe (pure structural validator).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bundle_parse_and_validate(const uint8_t*           container,
                                                      uint32_t                 container_len,
                                                      uint32_t                 max_bytes,
                                                      ra8_bundle_hdr_t*        out_hdr,
                                                      ra8_bundle_image_desc_t* out_descs,
                                                      uint32_t                 descs_cap);

/**
 * @brief Find descriptor for @p target within an already-parsed descriptor table
 *
 * @details
 * Scans `descs[0 .. image_count - 1]` for an entry whose `target` matches
 * @p target and copies it into @p out_desc on match.
 *
 * @param[in]  descs       Descriptor array from ::ra8_bundle_parse_and_validate.
 * @param[in]  image_count Number of valid entries in @p descs (`1..4`).
 * @param[in]  target      Desired processor target.
 * @param[out] out_desc    Destination descriptor copy (non-NULL).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Matching descriptor copied to @p out_desc.
 * @retval k_ra8_err_null_ptr    @p descs or @p out_desc was NULL.
 * @retval k_ra8_err_invalid_arg @p image_count out of range.
 * @retval k_ra8_err_not_found   No image targets @p target.
 *
 * @pre @p descs points to @p image_count validated descriptors.
 * @pre @p out_desc is non-NULL.
 * @post On ::k_ra8_ok, @p *out_desc holds the matching entry.
 * @post On ::k_ra8_err_not_found, @p *out_desc is untouched.
 *
 * @note Thread-safe (pure lookup).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bundle_find_target(const ra8_bundle_image_desc_t* descs,
                                               uint16_t                       image_count,
                                               ra8_bundle_target_t            target,
                                               ra8_bundle_image_desc_t*       out_desc);

/**
 * @brief Verify signature, anti-rollback version, and every payload SHA-256 in memory
 *
 * @details
 * Validates structure via ::ra8_bundle_parse_and_validate, rejects any bundle
 * with `bundle_version < min_version`, hashes `header || descriptors[0..N]` and
 * calls `crypto->ecdsa_verify`, then hashes each image payload in bounded chunks
 * using @p scratch and constant-time-compares against `desc[i].sha256`.
 *
 * @param[in]     container     Full signed bundle in contiguous readable memory.
 * @param[in]     container_len Total length of @p container in bytes.
 * @param[in]     pubkey_handle Trusted RoT ECDSA public key handle.
 * @param[in]     min_version   Minimum permitted `bundle_version` floor.
 * @param[in]     crypto        Injected SHA-256 + ECDSA interface (non-NULL).
 * @param[in,out] scratch       Working buffer for chunk hashing (optional for mem).
 * @param[in]     scratch_cap   Reserved for parity (`0` allowed if unused).
 * @param[out]    out_hdr       Validated header copy (non-NULL).
 * @param[out]    out_descs     Validated descriptor table copy (non-NULL).
 * @param[in]     descs_cap     Element capacity of @p out_descs.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok                    Bundle authentic, version valid, hashes match.
 * @retval k_ra8_err_null_ptr          Required pointer NULL.
 * @retval k_ra8_err_invalid_size      Structural length/alignment violation.
 * @retval k_ra8_err_invalid_arg       Structural header/descriptor violation.
 * @retval k_ra8_err_crc_mismatch      One or more payload SHA-256 digests mismatched.
 * @retval k_ra8_err_validation_failed ECDSA verification failed or version downgrade.
 *
 * @pre @p container holds all @p container_len bytes cleanly readable.
 * @pre @p crypto supplies valid SHA-256 + ECDSA callbacks.
 * @post Outputs populated ONLY when all checks pass (default-deny).
 * @post No hardware state mutated.
 *
 * @note Thread-safe when @p crypto context is caller-isolated.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bundle_verify_mem(const uint8_t*                   container,
                                              uint32_t                         container_len,
                                              uint32_t                         pubkey_handle,
                                              uint32_t                         min_version,
                                              const ra8_update_crypto_iface_t* crypto,
                                              uint8_t*                         scratch,
                                              uint32_t                         scratch_cap,
                                              ra8_bundle_hdr_t*                out_hdr,
                                              ra8_bundle_image_desc_t*         out_descs,
                                              uint32_t                         descs_cap);

/**
 * @brief Verify a staged `"RA8U"` N-image bundle directly inside an update session
 *
 * @details
 * Reads the staged container header, descriptors, and signature from
 * `session->cfg.storage.readback`, authenticates `SHA-256(header || descs)`
 * via `crypto.ecdsa_verify` when `enforce_rot` is enabled, enforces monotonic
 * `bundle_version >= floor` when `enforce_rollback` is enabled, checks
 * optional companion protocol compatibility (`req_compat_proto`), and streams
 * every image payload through @p scratch to confirm each `desc[i].sha256`.
 *
 * On success, marks `session->verified = 1` and advances `session->stage` to
 * `k_ra8_update_stage_verified` so ::ra8_update_commit and ::ra8_update_confirm
 * proceed identically to single-image updates. On any failure, invokes
 * rollback cleanly leaving caller outputs unmodified.
 *
 * @param[in,out] session          Staged ::ra8_update_session_t (non-NULL).
 * @param[in]     req_compat_proto Required companion protocol version (`0` = ignore).
 * @param[in,out] scratch          Working chunk buffer for storage readback.
 * @param[in]     scratch_cap      Capacity of @p scratch in bytes (`>= 1`).
 * @param[out]    out_hdr          Decoded validated bundle header (non-NULL).
 * @param[out]    out_descs        Decoded validated descriptor array (non-NULL).
 * @param[in]     descs_cap        Element capacity of @p out_descs.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok                    Container verified; session ready for commit.
 * @retval k_ra8_err_null_ptr          Required pointer NULL.
 * @retval k_ra8_err_invalid_state     Session not in `k_ra8_update_stage_staged`.
 * @retval k_ra8_err_invalid_size      Truncated, oversized, or misaligned span.
 * @retval k_ra8_err_invalid_arg       Malformed header or descriptor fields.
 * @retval k_ra8_err_crc_mismatch      One or more payload SHA-256 checks failed.
 * @retval k_ra8_err_validation_failed ECDSA failed, downgrade, or proto mismatch.
 *
 * @pre @p session completed staging (`ra8_update_stage_finish`).
 * @pre All output buffers are non-NULL and writable.
 * @post On ::k_ra8_ok, `session->stage == k_ra8_update_stage_verified`.
 * @post Rollback floor is NOT bumped here when `defer_floor_bump` is true.
 *
 * @note Caller-synchronized per session.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_update_verify_bundle(ra8_update_session_t*    session,
                                                 uint32_t                 req_compat_proto,
                                                 uint8_t*                 scratch,
                                                 uint32_t                 scratch_cap,
                                                 ra8_bundle_hdr_t*        out_hdr,
                                                 ra8_bundle_image_desc_t* out_descs,
                                                 uint32_t                 descs_cap);

#ifdef __cplusplus
}
#endif
