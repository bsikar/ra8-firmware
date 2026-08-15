/**
 * @file mdl_export_jof.c
 * @brief JOF tile-atlas exporter: one `.jof` band atlas per chapter page.
 *
 * @details
 * The JOF arm of the chapter exporter, split out of mdl_export.c because it is
 * the only format that reaches into the firmware's `ra8_jof` producer --
 * a whole decode (JPEG / PNG / WebP) and encode stack that the ZIP and tar
 * writers have no use for. Producing through the firmware's own code is the
 * point: a container this tool writes is byte-identical to one the board would
 * produce from the same page, so a defect reproduced here is a defect on
 * silicon.
 *
 * Each page becomes a full-width-column atlas -- `tile_w` is the whole image
 * width, so every tile is one horizontal band -- which is the shape the
 * `ra8_longstrip` engine scrolls a viewport at a time.
 *
 * WebP is the one format that cannot stream: its arm needs a single arena
 * holding the compressed source, the decoded RGBA frame and libwebp's scratch
 * at once. That arena is carved only for pages that really are WebP, so JPEG
 * and PNG pages do not pay a whole-frame cost for a format they do not use.
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdio.h>
#include <string.h>

#include "mdl_export.h"
#include "mdl_export_internal.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"

/**
 * @enum mdl_jof_geom_t
 * @brief JOF atlas tiling geometry for a long-strip page.
 * @details `tile_w` is set to the full image width, so each tile is one
 *          full-width horizontal band -- the shape the ra8_longstrip engine
 *          scrolls a viewport at a time.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_jof_band_h = 256, /**< Tile-band height in pixels. */
} mdl_jof_geom_t;

/**
 * @enum mdl_jof_webp_t
 * @brief WebP container-head offsets for the whole-frame-arena decision.
 * @details The exporter must know whether a page is WebP *before* producing, to
 *          decide whether to carve the whole-frame arena the WebP arm needs.
 * @see internal_jof_is_webp()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_jof_webp_riff_ofs   = 0U,  /**< Offset of the "RIFF" fourCC.        */
  k_jof_webp_fourcc_ofs = 8U,  /**< Offset of the "WEBP" fourCC.        */
  k_jof_webp_head_len   = 12U, /**< Bytes needed to sniff both fourCCs. */
  k_jof_webp_tag_len    = 4U,  /**< Length of one fourCC tag.           */
} mdl_jof_webp_t;
/** @brief Hard memory ceilings for the non-reentrant transcode workspace. */
typedef enum : uint32_t {
  k_jof_source_cap    = 16U * 1024U * 1024U, /**< Maximum encoded source bytes. */
  k_jof_work_cap      = 16U * 1024U * 1024U, /**< Maximum streaming work bytes. */
  k_jof_webp_work_cap = 64U * 1024U * 1024U, /**< Maximum WebP frame workspace. */
} mdl_jof_workspace_t;

/** @brief WebP RIFF container tag (page head bytes 0..3). */
static const uint8_t s_jof_webp_riff[k_jof_webp_tag_len] =
  {'R', 'I', 'F', 'F'}; /* MAGIC-OK: the RIFF container fourCC */

/** @brief WebP form-type fourCC (page head bytes 8..11). */
static const uint8_t s_jof_webp_webp[k_jof_webp_tag_len] =
  {'W', 'E', 'B', 'P'}; /* MAGIC-OK: the WEBP form-type fourCC */

/**
 * @brief Test whether a page's bytes carry the WebP RIFF container head.
 * @details Mirrors the producer's own dispatch sniff: both fourCCs must match,
 *          so a non-WebP RIFF (WAVE, AVI) is not mistaken for a WebP page and
 *          charged the whole-frame arena.
 * @param[in] data Page bytes (non-NULL).
 * @param[in] len  Readable byte count at @p data.
 * @return Whether @p data begins with a WebP container head.
 * @retval true  Both the "RIFF" and "WEBP" fourCCs are present.
 * @retval false Too short, or either fourCC differs.
 * @pre @p data holds @p len readable bytes.
 * @pre The page has been slurped whole (the sniff reads the head only).
 * @post No state is mutated.
 * @post A false result means the streaming JPEG / PNG arms handle the page.
 * @note Pure; thread-safe.
 * @see internal_jof_one()
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_jof_is_webp(const uint8_t* data, size_t len)
{
  if ((data == nullptr) || (len < (size_t)k_jof_webp_head_len)) {
    return false;
  }
  return (memcmp(&data[k_jof_webp_riff_ofs], s_jof_webp_riff, sizeof(s_jof_webp_riff)) == 0) &&
         (memcmp(&data[k_jof_webp_fourcc_ofs], s_jof_webp_webp, sizeof(s_jof_webp_webp)) == 0);
}

/** @brief Pull cursor over an in-RAM encoded image. */
typedef struct {
  const uint8_t* data; /**< Encoded bytes. */
  size_t         len;  /**< Total length.  */
  size_t         pos;  /**< Read cursor.   */
} jof_pull_ctx_t;

/**
 * @brief Copy the next encoded source span into a producer buffer
 * @details Advances a bounded in-memory cursor by at most @p cap bytes and
 *          reports zero bytes at end of input.
 * @param[in,out] ctx Pull cursor describing the encoded page.
 * @param[out] buf Destination passed by the producer.
 * @param[in] cap Writable capacity of @p buf.
 * @param[out] got Number of bytes copied.
 * @return Pull status.
 * @retval k_ra8_ok A bounded span, possibly empty at EOF, was returned.
 * @pre All pointers are non-NULL and cursor position does not exceed length.
 * @pre @p buf addresses @p cap writable bytes.
 * @post Cursor position advances by exactly `*got`.
 * @post `*got` never exceeds @p cap or remaining input.
 * @note Not thread-safe when a cursor is shared.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_jof_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  jof_pull_ctx_t* s = (jof_pull_ctx_t*)ctx;
  size_t          n = s->len - s->pos;
  if (n > cap) {
    n = cap;
  }
  memcpy(buf, s->data + s->pos, n);
  s->pos += n;
  *got = n;
  return k_ra8_ok;
}

/**
 * @brief Append producer output bytes to the active publication transaction.
 * @details Converts the bounded transaction writer result into the producer
 *          error contract without exposing a host stream.
 * @param[in,out] ctx Export output transaction owned by the caller.
 * @param[in] buf Atlas bytes to append.
 * @param[in] len Number of readable bytes at @p buf.
 * @return Sink status.
 * @retval k_ra8_ok Every byte was written.
 * @retval k_ra8_err_invalid_size @p len exceeds the transaction count type.
 * @retval k_ra8_fail The transaction sink rejected the write.
 * @pre @p ctx is an active ::mdl_export_output_t.
 * @pre @p buf holds @p len readable bytes.
 * @post Success advances the staged output by @p len bytes.
 * @post Failure remains visible to the producer.
 * @note Not thread-safe for a shared output transaction.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_jof_sink(void* ctx, const uint8_t* buf, size_t len)
{
  if (len > UINT32_MAX) {
    return k_ra8_err_invalid_size;
  }
  return priv_mdl_export_output_write((mdl_export_output_t*)ctx, buf, (uint32_t)len);
}

/**
 * @brief Carve the whole-frame WebP work arena for a WebP page (no-op otherwise).
 * @details WebP cannot stream, so its producer arm needs one arena holding the
 *          compressed source, the decoded RGBA frame and libwebp's scratch at
 *          once. JPEG and PNG stream, so they would pay that whole-frame cost
 *          for nothing; the arena is carved only when the page really is a
 *          WebP. Reporting success with a NULL arena is the producer's
 *          fail-closed "reject WebP" signal, correct for the other codecs.
 * @param[in]  src      Page bytes to sniff.
 * @param[in]  slen     Length of @p src in bytes.
 * @param[in]  w        Decoded page width in pixels.
 * @param[in]  h        Decoded page height in pixels.
 * @param[out] out_work Receives the arena, or nullptr for a non-WebP page.
 * @param[out] out_cap  Receives the arena size in bytes, or 0.
 * @param[in,out] ws    Caller-owned arena from which WebP storage is carved.
 * @return Result code.
 * @retval k_ra8_ok               Non-WebP page, or arena carved successfully.
 * @retval k_ra8_err_invalid_size The geometry does not admit a WebP arena.
 * @retval k_ra8_err_invalid_size The caller workspace cannot satisfy the arena.
 * @pre @p src holds @p slen readable bytes.
 * @pre @p out_work and @p out_cap are writable.
 * @pre @p ws owns writable storage and is exclusive to this conversion.
 * @post On success `*out_work` is nullptr (non-WebP) or a workspace slice.
 * @post On failure `*out_work` is nullptr and the arena remains caller-owned.
 * @note Not thread-safe; the slice remains owned by @p ws.
 * @see ra8_jof_webp_work_bytes()
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_jof_carve_webp(const uint8_t*          src,
                                                      size_t                  slen,
                                                      uint16_t                w,
                                                      uint16_t                h,
                                                      uint8_t**               out_work,
                                                      size_t*                 out_cap,
                                                      mdl_export_workspace_t* ws)
{
  *out_work = nullptr;
  *out_cap  = 0U;
  if (!internal_jof_is_webp(src, slen)) {
    return k_ra8_ok;
  }
  const uint32_t need = ra8_jof_webp_work_bytes(w, h, (uint32_t)slen);
  if ((need == 0U) || ((size_t)need > (size_t)k_jof_webp_work_cap)) {
    return k_ra8_err_invalid_size;
  }
  uint8_t* mem = (uint8_t*)mdl_export_workspace_take(ws, (size_t)need, 16U);
  if (mem == nullptr) {
    return k_ra8_err_invalid_size;
  }
  *out_work = mem;
  *out_cap  = (size_t)need;
  return k_ra8_ok;
}

/**
 * @brief Produce one page's atlas into an already-open output file.
 *
 * @details
 * Carves the producer's two scratch arenas -- the streaming work arena, and the
 * whole-frame arena only a WebP page needs -- runs the producer, and releases
 * both when the caller restores its workspace mark. Splitting this out of
 * internal_jof_one() keeps producer configuration separate from source and
 * publication ownership.
 *
 * @param[in]     src      Encoded page bytes.
 * @param[in]     slen     Length of @p src in bytes.
 * @param[in]     w        Decoded page width in pixels.
 * @param[in]     h        Decoded page height in pixels.
 * @param[in]     tile_h   Band height for this page.
 * @param[in]     work_cap Work-arena size, from ::ra8_jof_work_bytes.
 * @param[in,out] output   Active staged output the atlas is written to.
 * @param[in,out] ws       Caller-owned producer workspace.
 *
 * @return Producer result.
 * @retval k_ra8_ok               The atlas was written to @p out.
 * @retval k_ra8_err_no_mem       An arena could not be allocated.
 * @retval k_ra8_err_invalid_size The geometry admits no WebP arena.
 *
 * @pre @p src holds @p slen readable bytes and @p output is active.
 * @pre @p work_cap is non-zero.
 * @pre @p ws is exclusive and has not been reset during this page.
 * @post Both scratch arenas are released, on success and on every failure.
 * @post Nothing is written to @p output beyond what the producer emitted.
 *
 * @note Not thread-safe.
 * @see internal_jof_carve_webp()
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_jof_produce_page(const uint8_t*          src,
                                                        size_t                  slen,
                                                        uint16_t                w,
                                                        uint16_t                h,
                                                        uint16_t                tile_h,
                                                        uint32_t                work_cap,
                                                        mdl_export_output_t*    output,
                                                        mdl_export_workspace_t* ws)
{
  if ((size_t)work_cap > (size_t)k_jof_work_cap) {
    return k_ra8_err_invalid_size;
  }
  uint8_t* work = (uint8_t*)mdl_export_workspace_take(ws, (size_t)work_cap, 16U);
  if (work == nullptr) {
    return k_ra8_err_invalid_size;
  }
  uint8_t*        webp_work = nullptr;
  size_t          webp_cap  = 0U;
  const ra8_err_t carve_rc  = internal_jof_carve_webp(src, slen, w, h, &webp_work, &webp_cap, ws);
  if (carve_rc != k_ra8_ok) {
    return carve_rc;
  }
  jof_pull_ctx_t        pull = {.data = src, .len = slen, .pos = 0U};
  ra8_jof_produce_cfg_t cfg  = {.pull          = internal_jof_pull,
                                .pull_ctx      = &pull,
                                .sink          = internal_jof_sink,
                                .sink_ctx      = output,
                                .tile_w        = w,
                                .tile_h        = tile_h,
                                .codec         = (uint8_t)k_ra8_jof_codec_deflate,
                                .max_width     = w,
                                .max_height    = h,
                                .work          = work,
                                .work_cap      = work_cap,
                                .webp_work     = webp_work,
                                .webp_work_cap = webp_cap};
  ra8_jof_info_t        info = {};
  const ra8_err_t       rc   = ra8_jof_produce(&cfg, &info);
  return rc;
}

/**
 * @brief Transcode one JPEG, PNG, or WebP page to a JOF band atlas
 * @details Bounds the source and producer arenas, probes dimensions through
 *          the firmware producer, validates the borrowed staged file with the
 *          canonical JOF verifier, and publishes the completed sibling.
 * @param[in,out] storage Injected portable storage and transaction provider.
 * @param[in] in_path NUL-terminated verified source image path.
 * @param[in] out_path NUL-terminated destination JOF path.
 * @param[in,out] ws Exclusive caller-owned export workspace.
 * @return Transcode status.
 * @retval k_ra8_ok A complete independently validated JOF was published.
 * @retval k_ra8_err_invalid_size A source or workspace bound was exceeded.
 * @retval k_ra8_err_not_supported The producer cannot decode the image.
 * @retval k_ra8_fail Storage, validation, or publication operations failed.
 * @pre Paths are non-NULL, NUL-terminated, and stable for the call.
 * @pre @p storage is initialized and exclusive to this operation.
 * @pre @p ws owns writable storage and is exclusive to this page.
 * @post Success leaves one complete reader-consumable JOF.
 * @post Failure before publication aborts the stage and preserves the prior sibling.
 * @note Not thread-safe for a shared workspace or destination.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_jof_one(mdl_storage_t*          storage,
                                               const char*             in_path,
                                               const char*             out_path,
                                               mdl_export_workspace_t* ws)
{
  fw_fs_stat_t stat = {};
  ra8_err_t    rc   = fw_fs_stat(&storage->fs->names, in_path, &stat);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (!stat.exists) {
    return k_ra8_err_not_found;
  }
  if ((stat.type != k_fw_fs_node_file) || (stat.size_bytes == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (stat.size_bytes > (uint64_t)k_jof_source_cap) {
    return k_ra8_err_invalid_size;
  }
  uint8_t* src = (uint8_t*)mdl_export_workspace_take(ws, (size_t)stat.size_bytes, 16U);
  if (src == nullptr) {
    return k_ra8_err_invalid_size;
  }
  size_t slen = 0U;
  rc          = priv_mdl_export_source_slurp(storage, in_path, src, (size_t)stat.size_bytes, &slen);
  if (rc != k_ra8_ok) {
    return rc;
  }
  uint16_t w = 0U;
  uint16_t h = 0U;
  /* Probe through the producer's own dispatch, so every format the producer can
   * decode (JPEG, PNG, WebP) is a format this exporter can size and write. */
  if (ra8_jof_probe_dims(src, slen, &w, &h) != k_ra8_ok) {
    return k_ra8_err_not_supported;
  }
  const uint16_t tile_h   = (h < (uint16_t)k_jof_band_h) ? h : (uint16_t)k_jof_band_h;
  const uint32_t work_cap = ra8_jof_work_bytes(w, h, w, tile_h);
  if (work_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  /* Produce into the storage transaction's borrowed stage; canonical
   * validation must pass before the transaction publishes this page. */
  mdl_export_output_t output = {};
  rc = priv_mdl_export_output_begin(&output, storage, out_path, k_mdl_fmt_jof);
  if (rc == k_ra8_ok) {
    rc = internal_jof_produce_page(src, slen, w, h, tile_h, work_cap, &output, ws);
  }
  if ((rc != k_ra8_ok) && output.writer.transaction.active) {
    const ra8_err_t aborted = priv_mdl_export_output_abort(&output);
    return (aborted == k_ra8_ok) ? rc : aborted;
  }
  bool published = false;
  return (rc == k_ra8_ok) ? priv_mdl_export_output_commit(&output, ws, &published) : rc;
}

RA8_PRIV ra8_err_t priv_mdl_export_jof(mdl_storage_t*          storage,
                                       const char*             dir,
                                       const char              names[][k_name_max],
                                       size_t                  count,
                                       mdl_export_workspace_t* ws)
{
  const size_t page_mark = ws->used;
  ra8_err_t    rc        = k_ra8_ok;
  for (size_t i = 0U; (rc == k_ra8_ok) && (i < count); ++i) {
    char in_path[k_fw_fs_path_cap];
    char stem[k_name_max];
    char out_path[k_fw_fs_path_cap];
    rc = priv_mdl_export_path_join(in_path, sizeof(in_path), dir, names[i]);
    memcpy(stem, names[i], k_name_max);
    char* dot = strrchr(stem, '.');
    if (dot != nullptr) {
      *dot = '\0';
    }
    char      output_leaf[k_name_max];
    const int written = snprintf(output_leaf, sizeof(output_leaf), "%s.jof", stem);
    if ((rc == k_ra8_ok) && priv_mdl_export_snprintf_fit(written, sizeof(output_leaf))) {
      rc = priv_mdl_export_path_join(out_path, sizeof(out_path), dir, output_leaf);
    } else if (rc == k_ra8_ok) {
      rc = k_ra8_err_invalid_size;
    }
    ws->used = page_mark;
    if (rc == k_ra8_ok) {
      rc = internal_jof_one(storage, in_path, out_path, ws);
    }
  }
  return rc;
}
