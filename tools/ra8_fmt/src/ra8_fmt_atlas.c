/**
 * @file ra8_fmt_atlas.c
 * @brief JOF band-tile atlas verbs for `ra8_fmt`: convert, inspect, verify.
 *
 * @details
 * Wraps the firmware's own `ra8_tileatlas` producer and reader so the tool
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
#include "ra8_jpeg_sw.h"
#include "ra8_tileatlas.h"
#include "ra8_tileatlas_produce.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_fmt_atlas";

/**
 * @enum ra8_fmt_atlas_const_t
 * @brief Atlas verb sizing and reporting constants.
 * @details `k_fmt_atlas_band_h` mirrors the band height `tools/media_dl` picks,
 *          so a container this tool produces is byte-comparable with a
 *          downloaded one. The FNV-1a constants hash tile payloads for the
 *          duplicate check.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_fmt_atlas_band_h = 256U,        /**< Band height, matching media_dl.   */
  k_fmt_fnv_offset   = 2166136261U, /**< FNV-1a 32-bit offset basis.       */
  k_fmt_fnv_prime    = 16777619U,   /**< FNV-1a 32-bit prime.              */
  k_fmt_png_ihdr_w   = 16U,         /**< PNG IHDR width field offset.      */
  k_fmt_png_ihdr_h   = 20U,         /**< PNG IHDR height field offset.     */
  k_fmt_png_ihdr_end = 24U,         /**< Bytes needed to read both fields. */
  k_fmt_be_sh8       = 8U,          /**< Big-endian shift, byte 2.         */
  k_fmt_be_sh16      = 16U,         /**< Big-endian shift, byte 1.         */
  k_fmt_be_sh24      = 24U,         /**< Big-endian shift, byte 0.         */
} ra8_fmt_atlas_const_t;

/** @brief PNG 8-byte signature, for distinguishing PNG sources from JPEG. */
static const uint8_t s_fmt_png_sig[8] =
  {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU}; /* MAGIC-OK: the PNG spec signature */

/**
 * @brief Read a big-endian uint32 (PNG stores its header fields big-endian).
 * @details The JOF container is little-endian throughout, but the PNG IHDR the
 *          probe reads is not, so the probe needs its own reader.
 * @param[in] buf Source bytes (at least 4 readable).
 * @return The decoded value.
 * @retval 0 All four source bytes were zero.
 * @pre @p buf holds 4 readable bytes.
 * @pre The field is big-endian per the PNG specification.
 * @post No state is mutated.
 * @post The result equals the four bytes assembled big-endian.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t ra8_fmt_rd_be32(const uint8_t* buf)
{
  return ((uint32_t)buf[0] << k_fmt_be_sh24) | ((uint32_t)buf[1] << k_fmt_be_sh16) |
         ((uint32_t)buf[2] << k_fmt_be_sh8) | (uint32_t)buf[3];
}

/**
 * @struct fmt_pull_ctx_t
 * @brief Read cursor over an in-RAM encoded source image (producer pull seam).
 * @details Feeds `ra8_tileatlas_produce()` from a slurped blob rather than a
 *          file, so the same bytes can be replayed for the second encode.
 * @invariant `pos <= len` at all times.
 * @see fmt_atlas_pull()
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* data; /**< Encoded source bytes. */
  size_t         len;  /**< Total source length.  */
  size_t         pos;  /**< Read cursor.          */
} fmt_pull_ctx_t;

/**
 * @brief Producer pull callback: hand over the next source bytes.
 * @details Matching `ra8_tileatlas_pull_fn`; reports `*got == 0` at the end of
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
static ra8_err_t fmt_atlas_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
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

ra8_err_t ra8_fmt_atlas_probe(const ra8_fmt_blob_t* src, uint16_t* out_w, uint16_t* out_h)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(out_w, s_tag, "out_w must not be nullptr");
  RA8_CHECK_NULL_PTR(out_h, s_tag, "out_h must not be nullptr");
  if (src->len > (size_t)k_fmt_png_ihdr_end) {
    const bool is_png = memcmp(src->bytes, s_fmt_png_sig, sizeof(s_fmt_png_sig)) == 0;
    if (is_png) {
      /* PNG IHDR: width/height are big-endian uint32 at byte 16 and 20. */
      const uint32_t w = ra8_fmt_rd_be32(&src->bytes[k_fmt_png_ihdr_w]);
      const uint32_t h = ra8_fmt_rd_be32(&src->bytes[k_fmt_png_ihdr_h]);
      if ((w == 0U) || (h == 0U) || (w > (uint32_t)k_ra8_tileatlas_max_dim) ||
          (h > (uint32_t)k_ra8_tileatlas_max_dim)) {
        return k_ra8_err_invalid_size;
      }
      *out_w = (uint16_t)w;
      *out_h = (uint16_t)h;
      return k_ra8_ok;
    }
  }
  return ra8_jpeg_sw_get_dimensions(src->bytes, (uint32_t)src->len, out_w, out_h);
}

ra8_err_t ra8_fmt_atlas_produce(const ra8_fmt_blob_t* src,
                                uint16_t              max_w,
                                uint16_t              max_h,
                                uint16_t              tile_w,
                                uint16_t              tile_h,
                                uint8_t               codec,
                                ra8_fmt_blob_t*       out_atlas,
                                ra8_tileatlas_info_t* out_info)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(out_atlas, s_tag, "out_atlas must not be nullptr");
  RA8_CHECK_NULL_PTR(out_info, s_tag, "out_info must not be nullptr");
  *out_atlas              = (ra8_fmt_blob_t){};
  const uint32_t work_cap = ra8_tileatlas_work_bytes(max_w, max_h, tile_w, tile_h);
  if (work_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  /* The atlas can expand slightly over the source for incompressible input;
   * size the sink from the decoded worst case plus the index and trailer. */
  const size_t sink_cap = ra8_fmt_atlas_sink_cap(src->len);
  uint8_t*     work     = (uint8_t*)malloc(work_cap);
  uint8_t*     sink     = (uint8_t*)malloc(sink_cap);
  if ((work == nullptr) || (sink == nullptr)) {
    free(work);
    free(sink);
    return k_ra8_err_no_mem;
  }
  ra8_tileatlas_memstore_t          store = {.buf = sink, .cap = sink_cap, .len = 0U};
  fmt_pull_ctx_t                    pull  = {.data = src->bytes, .len = src->len, .pos = 0U};
  const ra8_tileatlas_produce_cfg_t cfg   = {.pull          = fmt_atlas_pull,
                                             .pull_ctx      = &pull,
                                             .sink          = ra8_tileatlas_memstore_sink,
                                             .sink_ctx      = &store,
                                             .tile_w        = tile_w,
                                             .tile_h        = tile_h,
                                             .codec         = codec,
                                             .max_width     = 0U,
                                             .max_height    = 0U,
                                             .work          = work,
                                             .work_cap      = work_cap,
                                             .webp_work     = nullptr,
                                             .webp_work_cap = 0U};
  const ra8_err_t                   rc    = ra8_tileatlas_produce(&cfg, out_info);
  free(work);
  if (rc != k_ra8_ok) {
    free(sink);
    return rc;
  }
  out_atlas->bytes = sink;
  out_atlas->len   = store.len;
  return k_ra8_ok;
}

ra8_err_t ra8_fmt_atlas_convert(const ra8_fmt_blob_t* src, const ra8_fmt_opts_t* opts)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(opts, s_tag, "opts must not be nullptr");
  if (opts->out_path == nullptr) {
    (void)fprintf(opts->report, "ra8_fmt: convert needs --out\n");
    return k_ra8_err_invalid_arg;
  }
  uint16_t  w  = 0U;
  uint16_t  h  = 0U;
  ra8_err_t rc = ra8_fmt_atlas_probe(src, &w, &h);
  if (rc != k_ra8_ok) {
    (void)fprintf(opts->report, "ra8_fmt: cannot decode source image (rc=%d)\n", (int)rc);
    return rc;
  }
  const uint16_t       band = (h < (uint16_t)k_fmt_atlas_band_h) ? h : (uint16_t)k_fmt_atlas_band_h;
  ra8_fmt_blob_t       atlas = {};
  ra8_tileatlas_info_t info  = {};
  rc                         = ra8_fmt_atlas_produce(src,
                                                     w,
                                                     h,
                                                     w,
                                                     band,
                                                     (uint8_t)k_ra8_tileatlas_codec_deflate,
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

bool ra8_fmt_atlas_sniff(const ra8_fmt_blob_t* src)
{
  return ra8_fmt_magic_is(src, "JOF1"); /* MAGIC-OK: the JOF header fourCC */
}
