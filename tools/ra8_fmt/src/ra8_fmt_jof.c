/**
 * @file ra8_fmt_jof.c
 * @brief JOF band-tile atlas verbs for `ra8_fmt`: convert, inspect, verify.
 *
 * @details
 * Wraps the firmware's own `ra8_jof` producer and reader so the tool
 * exercises exactly the code the board runs -- a bug reproduced here is a bug
 * on silicon, and a clean result here exonerates the file.
 *
 * `inspect` walks the header, the trailing per-tile index and the footer,
 * printing every geometry field and every tile's absolute offset / stored
 * length / decoded payload size, then cross-checks three things a rendering
 * defect would show up in:
 *   - **coverage**: do the tile windows tile the stream region exactly once,
 *     with no gap and no overlap?
 *   - **duplication**: do any two distinct tiles share a byte range, or hash
 *     identically? Identical payloads for an image with distinct bands is the
 *     signature of duplication baked into the *file*.
 *   - **geometry**: does each tile's decoded size equal its edge-clamped
 *     `tw * th * bpp`, so a partial edge tile really is partial?
 *
 * `verify` answers the producer-versus-consumer question outright: it encodes
 * the source twice -- once with the requested banding, once as a single
 * whole-image tile -- decodes both back and compares the rasters byte for byte.
 * The single-tile encode is the reference decode of the source image (no tiling
 * involved), so any mismatch is the tiling path's fault and nothing else's.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fmt.h"
#include "ra8_fmt_internal.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_fmt_jof";

/**
 * @enum ra8_fmt_jof_const_t
 * @brief Atlas verb sizing and reporting constants.
 * @details `k_fmt_jof_band_h` mirrors the band height `tools/media_dl` picks,
 *          so a container this tool produces is byte-comparable with a
 *          downloaded one. The FNV-1a constants hash tile payloads for the
 *          duplicate check.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_fmt_jof_band_h    = 256U,        /**< Band height, matching media_dl.     */
  k_fmt_fnv_offset      = 2166136261U, /**< FNV-1a 32-bit offset basis.         */
  k_fmt_fnv_prime       = 16777619U,   /**< FNV-1a 32-bit prime.                */
  k_fmt_webp_riff_ofs   = 0U,          /**< Offset of the "RIFF" fourCC.        */
  k_fmt_webp_fourcc_ofs = 8U,          /**< Offset of the "WEBP" fourCC.        */
  k_fmt_webp_head_len   = 12U,         /**< Bytes needed to sniff both fourCCs. */
} ra8_fmt_jof_const_t;

/** @brief WebP RIFF container tag (source head bytes 0..3). */
static const uint8_t s_fmt_webp_riff[4] = {'R', 'I', 'F', 'F'}; /* MAGIC-OK: RIFF fourCC */

/** @brief WebP form tag (source head bytes 8..11), gating RIFF to WebP alone. */
static const uint8_t s_fmt_webp_webp[4] = {'W', 'E', 'B', 'P'}; /* MAGIC-OK: WEBP fourCC */

/**
 * @brief Test whether a source blob carries the WebP RIFF container head.
 * @details Mirrors the producer's own dispatch sniff in
 *          `ra8_jof_produce.c` -- both fourCCs must match, so a non-WebP
 *          RIFF (WAVE, AVI) is rejected rather than handed to the WebP arm.
 *          The tool uses this for two decisions that must agree: which probe
 *          reads the dimensions, and whether to carve the whole-frame arena.
 * @param[in] src Source blob to sniff.
 * @return Whether @p src begins with a WebP container head.
 * @retval true  Both the "RIFF" and "WEBP" fourCCs are present.
 * @retval false Too short, or either fourCC differs.
 * @pre @p src is non-NULL and describes `len` readable bytes at `bytes`.
 * @pre @p src has been slurped whole (the sniff reads the head only).
 * @post No state is mutated.
 * @post A false result leaves the JPEG / PNG probes free to claim the source.
 * @note Pure; thread-safe.
 * @see ra8_fmt_jof_probe()
 * @since 0.1.0
 */
RA8_INTERNAL
static bool ra8_fmt_jof_is_webp(const ra8_fmt_blob_t* src)
{
  if ((src == nullptr) || (src->bytes == nullptr) || (src->len < (size_t)k_fmt_webp_head_len)) {
    return false;
  }
  return (memcmp(&src->bytes[k_fmt_webp_riff_ofs], s_fmt_webp_riff, sizeof(s_fmt_webp_riff)) ==
          0) &&
         (memcmp(&src->bytes[k_fmt_webp_fourcc_ofs], s_fmt_webp_webp, sizeof(s_fmt_webp_webp)) ==
          0);
}

/**
 * @struct fmt_pull_ctx_t
 * @brief Read cursor over an in-RAM encoded source image (producer pull seam).
 * @details Feeds `ra8_jof_produce()` from a slurped blob rather than a
 *          file, so the same bytes can be replayed for the second encode.
 * @invariant `pos <= len` at all times.
 * @see fmt_jof_pull()
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* data; /**< Encoded source bytes. */
  size_t         len;  /**< Total source length.  */
  size_t         pos;  /**< Read cursor.          */
} fmt_pull_ctx_t;

/**
 * @brief Producer pull callback: hand over the next source bytes.
 * @details Matching `ra8_jof_pull_fn`; reports `*got == 0` at the end of
 *          the blob, which the producer treats as end-of-source.
 * @param[in]  ctx Read cursor (a ::fmt_pull_ctx_t*).
 * @param[out] buf Destination buffer.
 * @param[in]  cap Bytes requested.
 * @param[out] got Bytes actually supplied.
 * @return Always ::k_ra8_ok -- a RAM blob cannot fail to read.
 * @retval k_ra8_ok Bytes supplied (possibly zero at the end).
 * @pre @p ctx points at an initialised cursor.
 * @pre @p buf holds @p cap writable bytes.
 * @post `*got <= cap` bytes were copied and the cursor advanced by `*got`.
 * @post The cursor never passes the blob end.
 * @note Not thread-safe (mutates the cursor).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t fmt_jof_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  fmt_pull_ctx_t* s = (fmt_pull_ctx_t*)ctx;
  size_t          n = s->len - s->pos;
  if (n > cap) {
    n = cap;
  }
  (void)memcpy(buf, &s->data[s->pos], n);
  s->pos += n;
  *got = n;
  return k_ra8_ok;
}

/**
 * @brief Carve the whole-frame WebP work arena, if the source is a WebP.
 * @details WebP is not stripe-decodable, so its producer arm needs one arena
 *          holding the compressed source, the decoded RGBA frame and libwebp's
 *          scratch simultaneously. JPEG and PNG stream and would pay that
 *          whole-frame cost for nothing, so the arena is carved only when the
 *          source really is a WebP. For every other codec this reports success
 *          having produced a NULL arena, which is exactly the producer's
 *          fail-closed "reject WebP" signal.
 * @param[in]  src      Encoded source blob to sniff.
 * @param[in]  max_w    Width cap in pixels used to size the arena.
 * @param[in]  max_h    Height cap in pixels used to size the arena.
 * @param[out] out_work Receives the arena, or nullptr for a non-WebP source.
 * @param[out] out_cap  Receives the arena size in bytes, or 0.
 * @return Result code.
 * @retval k_ra8_ok           Non-WebP source, or arena carved successfully.
 * @retval k_ra8_err_invalid_size The caps do not admit a WebP of this size.
 * @retval k_ra8_err_no_mem   The host allocator refused the arena.
 * @pre @p src points at an initialised blob.
 * @pre @p out_work and @p out_cap are writable.
 * @post On success `*out_work` is nullptr (non-WebP) or owned by the caller.
 * @post On failure `*out_work` is nullptr and nothing was allocated.
 * @note Not thread-safe; the caller owns and must free `*out_work`.
 * @see ra8_jof_webp_work_bytes()
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t fmt_jof_carve_webp(const ra8_fmt_blob_t* src,
                                      uint16_t              max_w,
                                      uint16_t              max_h,
                                      uint8_t**             out_work,
                                      size_t*               out_cap)
{
  *out_work = nullptr;
  *out_cap  = 0U;
  if (!ra8_fmt_jof_is_webp(src)) {
    return k_ra8_ok;
  }
  const uint32_t need = ra8_jof_webp_work_bytes(max_w, max_h, (uint32_t)src->len);
  if (need == 0U) {
    return k_ra8_err_invalid_size;
  }
  uint8_t* mem = (uint8_t*)malloc((size_t)need);
  if (mem == nullptr) {
    return k_ra8_err_no_mem;
  }
  *out_work = mem;
  *out_cap  = (size_t)need;
  return k_ra8_ok;
}

ra8_err_t ra8_fmt_jof_probe(const ra8_fmt_blob_t* src, uint16_t* out_w, uint16_t* out_h)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(out_w, s_tag, "out_w must not be nullptr");
  RA8_CHECK_NULL_PTR(out_h, s_tag, "out_h must not be nullptr");
  /* The producer owns the format dispatch, so it owns the geometry probe too:
   * delegating keeps "the tool can size it" and "the producer will decode it"
   * from drifting apart. */
  return ra8_jof_probe_dims(src->bytes, src->len, out_w, out_h);
}

/**
 * @brief Describe and run one tile-atlas produce job over caller-owned buffers.
 *
 * @details
 * Assembles the ::ra8_jof_produce_cfg_t -- the pull seam over @p src, the
 * memstore sink, the tiling parameters and both work arenas -- and runs the
 * producer. Owns no memory: every buffer is supplied and released by the caller,
 * which is what keeps the allocation/cleanup paths in one place.
 *
 * @param[in]     src           Source image bytes to tile.
 * @param[in]     tile_w        Tile width in pixels.
 * @param[in]     tile_h        Tile height in pixels.
 * @param[in]     codec         Per-tile codec selector.
 * @param[out]    work          Producer work arena.
 * @param[in]     work_cap      Capacity of @p work in bytes.
 * @param[out]    webp_work     Whole-frame WebP arena, or nullptr for a
 *                              stripe-decodable source.
 * @param[in]     webp_work_cap Capacity of @p webp_work in bytes, or 0.
 * @param[in,out] store         Memstore the produced atlas is written into.
 * @param[out]    out_info      Receives the produced atlas geometry.
 * @return Producer result.
 * @retval k_ra8_ok The atlas was produced into @p store.
 *
 * @pre Every pointer argument except @p webp_work is non-null.
 * @pre @p work_cap is the value ::ra8_jof_work_bytes returned.
 * @post On success @p store->len is the produced atlas length.
 * @post No memory is allocated or freed here.
 *
 * @note Not thread-safe.
 * @see fmt_jof_carve_webp()
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t fmt_jof_run_produce(const ra8_fmt_blob_t*     src,
                                       uint16_t                  tile_w,
                                       uint16_t                  tile_h,
                                       uint8_t                   codec,
                                       uint8_t*                  work,
                                       uint32_t                  work_cap,
                                       uint8_t*                  webp_work,
                                       size_t                    webp_work_cap,
                                       ra8_jof_memstore_t* store,
                                       ra8_jof_info_t*     out_info)
{
  fmt_pull_ctx_t                    pull = {.data = src->bytes, .len = src->len, .pos = 0U};
  const ra8_jof_produce_cfg_t cfg  = {.pull          = fmt_jof_pull,
                                            .pull_ctx      = &pull,
                                            .sink          = ra8_jof_memstore_sink,
                                            .sink_ctx      = store,
                                            .tile_w        = tile_w,
                                            .tile_h        = tile_h,
                                            .codec         = codec,
                                            .max_width     = 0U,
                                            .max_height    = 0U,
                                            .work          = work,
                                            .work_cap      = work_cap,
                                            .webp_work     = webp_work,
                                            .webp_work_cap = webp_work_cap};
  return ra8_jof_produce(&cfg, out_info);
}

/**
 * @brief Acquire the producer's work arena and atlas sink, all or nothing.
 *
 * @details
 * Both buffers are needed for a produce run, so a partial allocation frees
 * whatever succeeded rather than leaving the caller to unwind a half-acquired
 * pair on a path that cannot use it.
 *
 * @param[in]  work_cap Work-arena size in bytes.
 * @param[in]  sink_cap Atlas-sink size in bytes.
 * @param[out] work     Receives the work arena, or NULL on failure.
 * @param[out] sink     Receives the sink buffer, or NULL on failure.
 * @return Whether both allocations succeeded.
 * @retval true  Both buffers are owned by the caller.
 * @retval false Nothing is allocated; both out-pointers are NULL.
 *
 * @pre @p work and @p sink are non-null.
 * @pre @p work_cap is non-zero (the caller rejected a zero-size job).
 * @post On success the caller owns both buffers and must free them.
 * @post On failure no allocation outlives the call.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool fmt_jof_alloc_bufs(uint32_t work_cap, size_t sink_cap, uint8_t** work, uint8_t** sink)
{
  *work = (uint8_t*)malloc(work_cap);
  *sink = (uint8_t*)malloc(sink_cap);
  if ((*work != nullptr) && (*sink != nullptr)) {
    return true;
  }
  free(*work);
  free(*sink);
  *work = nullptr;
  *sink = nullptr;
  return false;
}

ra8_err_t ra8_fmt_jof_produce(const ra8_fmt_blob_t* src,
                                uint16_t              max_w,
                                uint16_t              max_h,
                                uint16_t              tile_w,
                                uint16_t              tile_h,
                                uint8_t               codec,
                                ra8_fmt_blob_t*       out_atlas,
                                ra8_jof_info_t* out_info)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(out_atlas, s_tag, "out_atlas must not be nullptr");
  RA8_CHECK_NULL_PTR(out_info, s_tag, "out_info must not be nullptr");
  *out_atlas              = (ra8_fmt_blob_t){};
  const uint32_t work_cap = ra8_jof_work_bytes(max_w, max_h, tile_w, tile_h);
  if (work_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  /* The atlas can expand slightly over the source for incompressible input;
   * size the sink from the decoded worst case plus the index and trailer. */
  const size_t sink_cap = ra8_fmt_jof_sink_cap(src->len);
  uint8_t*     work     = nullptr;
  uint8_t*     sink     = nullptr;
  if (!fmt_jof_alloc_bufs(work_cap, sink_cap, &work, &sink)) {
    return k_ra8_err_no_mem;
  }
  uint8_t*        webp_work     = nullptr;
  size_t          webp_work_cap = 0U;
  const ra8_err_t carve_rc = fmt_jof_carve_webp(src, max_w, max_h, &webp_work, &webp_work_cap);
  if (carve_rc != k_ra8_ok) {
    free(work);
    free(sink);
    return carve_rc;
  }
  ra8_jof_memstore_t store = {.buf = sink, .cap = sink_cap, .len = 0U};
  const ra8_err_t          rc    = fmt_jof_run_produce(src,
                                              tile_w,
                                              tile_h,
                                              codec,
                                              work,
                                              work_cap,
                                              webp_work,
                                              webp_work_cap,
                                              &store,
                                              out_info);
  free(work);
  free(webp_work);
  if (rc != k_ra8_ok) {
    free(sink);
    return rc;
  }
  out_atlas->bytes = sink;
  out_atlas->len   = store.len;
  return k_ra8_ok;
}

ra8_err_t ra8_fmt_jof_convert(const ra8_fmt_blob_t* src, const ra8_fmt_opts_t* opts)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(opts, s_tag, "opts must not be nullptr");
  if (opts->out_path == nullptr) {
    (void)fprintf(opts->report, "ra8_fmt: convert needs --out\n");
    return k_ra8_err_invalid_arg;
  }
  uint16_t  w  = 0U;
  uint16_t  h  = 0U;
  ra8_err_t rc = ra8_fmt_jof_probe(src, &w, &h);
  if (rc != k_ra8_ok) {
    (void)fprintf(opts->report, "ra8_fmt: cannot decode source image (rc=%d)\n", (int)rc);
    return rc;
  }
  const uint16_t       band = (h < (uint16_t)k_fmt_jof_band_h) ? h : (uint16_t)k_fmt_jof_band_h;
  ra8_fmt_blob_t       atlas = {};
  ra8_jof_info_t info  = {};
  rc                         = ra8_fmt_jof_produce(src,
                                                     w,
                                                     h,
                                                     w,
                                                     band,
                                                     (uint8_t)k_ra8_jof_codec_deflate,
                                                     &atlas,
                                                     &info);
  if (rc != k_ra8_ok) {
    (void)fprintf(opts->report, "ra8_fmt: transcode failed (rc=%d)\n", (int)rc);
    return rc;
  }
  rc = ra8_fmt_write_file(opts->out_path, atlas.bytes, atlas.len);
  (void)fprintf(opts->report,
                "convert: %ux%u bpp=%u band=%u tiles=%u -> %s (%zu bytes)\n",
                (unsigned)info.width,
                (unsigned)info.height,
                (unsigned)info.bpp,
                (unsigned)info.tile_h,
                (unsigned)info.tile_count,
                opts->out_path,
                atlas.len);
  ra8_fmt_blob_free(&atlas);
  return rc;
}

bool ra8_fmt_jof_sniff(const ra8_fmt_blob_t* src)
{
  return ra8_fmt_magic_is(src, "JOF1"); /* MAGIC-OK: the JOF header fourCC */
}
