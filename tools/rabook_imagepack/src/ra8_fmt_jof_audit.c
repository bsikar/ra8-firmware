/**
 * @file ra8_fmt_jof_audit.c
 * @brief No-heap implementation of the backing-agnostic JOF audit.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_fmt_jof_audit.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief FNV-1a constants used for decoded tile evidence. */
typedef enum : uint32_t {
  k_fmt_audit_fnv_basis    = 2166136261U,
  k_fmt_audit_fnv_prime    = 16777619U,
  k_fmt_audit_u32_b3_shift = 24U,
} fmt_audit_hash_t;

/**
 * @brief Read exactly one bounded window from the injected backing.
 * @param[in]  pread   Read callback.
 * @param[in]  ctx     Callback context.
 * @param[in]  offset  Absolute offset.
 * @param[out] buf     Destination.
 * @param[in]  len     Exact byte count.
 * @return ::k_ra8_ok, a callback error, or validation failure on short read.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_read_exact(ra8_jof_pread_fn pread, void* ctx, uint64_t offset, uint8_t* buf, size_t len)
{
  size_t          got = 0U;
  const ra8_err_t rc  = pread(ctx, offset, buf, len, &got);
  if (rc != k_ra8_ok) {
    return rc;
  }
  return (got == len) ? k_ra8_ok : k_ra8_err_validation_failed;
}

/**
 * @brief Decode a little-endian u32 from an index entry.
 * @param[in] p Four readable bytes.
 * @return Decoded value.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_rd_u32(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
         ((uint32_t)p[3] << k_fmt_audit_u32_b3_shift);
}

/**
 * @brief Hash decoded bytes and report whether all bytes are equal.
 * @param[in]  bytes       Decoded tile.
 * @param[in]  len         Tile byte count.
 * @param[out] out_uniform Receives uniformity evidence.
 * @return FNV-1a hash.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_hash(const uint8_t* bytes, size_t len, bool* out_uniform)
{
  uint32_t h       = (uint32_t)k_fmt_audit_fnv_basis;
  bool     uniform = true;
  for (size_t i = 0U; i < len; ++i) {
    if ((i != 0U) && (bytes[i] != bytes[0])) {
      uniform = false;
    }
    h ^= (uint32_t)bytes[i];
    h *= (uint32_t)k_fmt_audit_fnv_prime;
  }
  *out_uniform = uniform;
  return h;
}

ra8_err_t ra8_fmt_jof_audit_requirements(ra8_jof_pread_fn                  pread,
                                         void*                             pread_ctx,
                                         uint64_t                          total_size,
                                         ra8_fmt_jof_audit_requirements_t* out)
{
  if ((pread == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  ra8_jof_info_t  info = {};
  const ra8_err_t rc   = ra8_jof_parse(pread, pread_ctx, total_size, &info);
  if (rc != k_ra8_ok) {
    return rc;
  }
  const uint64_t tile64 = (uint64_t)info.tile_w * (uint64_t)info.tile_h * (uint64_t)info.bpp;
  if (tile64 > UINT32_MAX) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t tile = (uint32_t)tile64;
  *out                = (ra8_fmt_jof_audit_requirements_t){
    .record_count = info.tile_count,
    .tile_bytes   = tile,
    .scratch_bytes =
      (info.codec == (uint8_t)k_ra8_jof_codec_deflate) ? ra8_jof_stored_bound(tile) : 0U,
  };
  if ((out->tile_bytes == 0U) ||
      ((info.codec == (uint8_t)k_ra8_jof_codec_deflate) && (out->scratch_bytes == 0U))) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate caller workspace against parsed requirements.
 * @param[in] ws   Caller workspace.
 * @param[in] need Exact requirements.
 * @return ::k_ra8_ok or a pointer/capacity error.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_check_workspace(const ra8_fmt_jof_audit_workspace_t*    ws,
                                                       const ra8_fmt_jof_audit_requirements_t* need)
{
  if ((ws == nullptr) || (ws->records == nullptr) || (ws->tile == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((need->scratch_bytes != 0U) && (ws->scratch == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((ws->record_cap < need->record_count) || (ws->tile_cap < need->tile_bytes) ||
      (ws->scratch_cap < need->scratch_bytes)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Count earlier records matching one non-uniform tile fingerprint.
 * @param[in] records Completed records.
 * @param[in] count   Earlier record count.
 * @param[in] item    Current record.
 * @return Number of matching earlier records, as diagnostic candidates only.
 * @note A fingerprint match is not byte-equality proof and cannot invalidate
 *       an otherwise well-formed atlas.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_duplicate_count(const ra8_fmt_jof_audit_record_t* records,
                                                      uint32_t                          count,
                                                      const ra8_fmt_jof_audit_record_t* item)
{
  if (item->uniform) {
    return 0U;
  }
  uint32_t matches = 0U;
  for (uint32_t i = 0U; i < count; ++i) {
    if (!records[i].uniform && (records[i].payload == item->payload) &&
        (records[i].content_hash == item->content_hash)) {
      matches++;
    }
  }
  return matches;
}

ra8_err_t ra8_fmt_jof_audit(ra8_jof_pread_fn               pread,
                            void*                          pread_ctx,
                            uint64_t                       total_size,
                            ra8_fmt_jof_audit_workspace_t* workspace,
                            ra8_fmt_jof_audit_result_t*    out)
{
  if ((pread == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  ra8_fmt_jof_audit_requirements_t need = {};
  ra8_err_t rc = ra8_fmt_jof_audit_requirements(pread, pread_ctx, total_size, &need);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_check_workspace(workspace, &need);
  if (rc != k_ra8_ok) {
    return rc;
  }
  *out = (ra8_fmt_jof_audit_result_t){};
  rc   = ra8_jof_parse(pread, pread_ctx, total_size, &out->info);
  if (rc != k_ra8_ok) {
    return rc;
  }

  uint32_t expected_offset = (uint32_t)k_ra8_jof_hdr_bytes;
  for (uint32_t i = 0U; i < out->info.tile_count; ++i) {
    uint8_t        index[k_ra8_jof_index_entry] = {};
    const uint64_t index_at =
      (uint64_t)out->info.index_off + ((uint64_t)i * (uint64_t)k_ra8_jof_index_entry);
    rc = internal_read_exact(pread, pread_ctx, index_at, index, sizeof(index));
    if (rc != k_ra8_ok) {
      return rc;
    }
    ra8_fmt_jof_audit_record_t* const record = &workspace->records[i];
    *record                                  = (ra8_fmt_jof_audit_record_t){
      .offset = internal_rd_u32(&index[k_ra8_jof_idx_ofs_offset]),
      .length = internal_rd_u32(&index[k_ra8_jof_idx_ofs_length]),
    };
    if (record->offset != expected_offset) {
      out->coverage_errors++;
    }
    const uint64_t next = (uint64_t)record->offset + (uint64_t)record->length;
    if (next > UINT32_MAX) {
      return k_ra8_err_validation_failed;
    }
    expected_offset = (uint32_t)next;

    const uint16_t tx = (uint16_t)(i % (uint32_t)out->info.tile_cols);
    const uint16_t ty = (uint16_t)(i / (uint32_t)out->info.tile_cols);
    rc                = ra8_jof_read_tile(pread,
                                          pread_ctx,
                                          &out->info,
                                          tx,
                                          ty,
                                          workspace->scratch,
                                          workspace->scratch_cap,
                                          workspace->tile,
                                          workspace->tile_cap,
                                          &record->width,
                                          &record->height);
    if (rc != k_ra8_ok) {
      return rc;
    }
    record->payload = (uint32_t)record->width * (uint32_t)record->height * (uint32_t)out->info.bpp;
    uint16_t want_w = 0U;
    uint16_t want_h = 0U;
    rc              = ra8_jof_tile_dims(&out->info, tx, ty, &want_w, &want_h);
    if (rc != k_ra8_ok) {
      return rc;
    }
    if ((record->width != want_w) || (record->height != want_h)) {
      out->geometry_errors++;
    }
    record->content_hash = internal_hash(workspace->tile, record->payload, &record->uniform);
    out->duplicate_candidates += internal_duplicate_count(workspace->records, i, record);
    out->decoded_tiles++;
  }
  if (expected_offset != out->info.index_off) {
    out->coverage_errors++;
  }
  return ((out->coverage_errors == 0U) && (out->geometry_errors == 0U))
           ? k_ra8_ok
           : k_ra8_err_validation_failed;
}
