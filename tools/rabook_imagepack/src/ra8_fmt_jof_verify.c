/**
 * @file ra8_fmt_jof_verify.c
 * @brief JOF round-trip verification: banded encode versus untiled reference.
 *
 * @details
 * Encodes one source image twice through the firmware producer -- once with the
 * long-strip banding a real import uses, once as a single whole-image tile --
 * then reassembles both back to full rasters and compares them byte for byte.
 *
 * The single-tile encode is the reference: it runs the identical decoder over
 * the identical source but never exercises the tile cut, the band accumulator
 * or the edge-clamp arithmetic. So the two rasters must be equal, and any
 * difference is attributable to the tiling path alone. That is what makes this
 * the mechanism that separates a producer defect from a consumer defect: if
 * `verify` passes, the bytes on disk are right and a wrong render is the
 * reader's fault.
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 *

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

/** @brief Module log tag. */
static const char* const s_tag = "ra8_fmt_jof_ver";

/**
 * @enum ra8_fmt_ver_const_t
 * @brief Verification banding and reporting constants.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_fmt_ver_band_h    = 256U, /**< Band height under test (matches media_dl). */
  k_fmt_ver_max_diffs = 8U,   /**< Differing pixels reported before eliding.  */
  k_fmt_ver_gray      = 1U,   /**< bpp for 8-bit grayscale rasters.           */
  k_fmt_ver_rgb       = 3U,   /**< bpp for 24-bit RGB rasters.                */
  k_fmt_ver_rgba      = 4U,   /**< bpp for 32-bit RGBA rasters (WebP / PNG).  */
  k_fmt_ppm_max       = 255U, /**< PPM maximum channel value.                 */
} ra8_fmt_ver_const_t;

/**
 * @struct fmt_ver_pair_t
 * @brief The two rasters a verification compares, plus their geometries.
 * @details `ref` comes from a single whole-image tile (no tiling exercised);
 *          `got` from the banded encode under test. Equal content is the pass
 *          condition.
 * @invariant `ref != nullptr` implies `refn > 0`; likewise for `got`/`gotn`.
 * @see ra8_fmt_jof_verify()
 * @since 0.1.0
 */
typedef struct {
  uint8_t*       ref;   /**< Reference raster (single-tile encode). */
  size_t         refn;  /**< Reference raster byte count.           */
  uint8_t*       got;   /**< Banded raster under test.              */
  size_t         gotn;  /**< Banded raster byte count.              */
  ra8_jof_info_t rinfo; /**< Reference atlas geometry.              */
  ra8_jof_info_t ginfo; /**< Banded atlas geometry.                 */
} fmt_ver_pair_t;

/**
 * @brief Write a reassembled raster out as a binary PPM for visual inspection.
 * @details Grayscale rasters are emitted as P5, RGB as P6; any other depth is
 *          skipped rather than mis-labelled. RGBA rasters (what the WebP arm
 *          and alpha PNGs decode to) are emitted as P6 with the alpha channel
 *          dropped per pixel: PPM has no alpha, and dumping the raw 4-byte
 *          pixels under a P6 header would mislabel the file. The dump is a
 *          visual-inspection aid only -- the pass/fail verdict is the raster
 *          comparison, which runs on the full RGBA bytes.
 * @param[in] path Destination path (non-nullptr).
 * @param[in] px   Raster bytes (non-nullptr).
 * @param[in] w    Image width, pixels.
 * @param[in] h    Image height, pixels.
 * @param[in] bpp  Bytes per pixel (1, 3 or 4).
 * @return Result code.
 * @retval k_ra8_ok                The PPM was written.
 * @retval k_ra8_err_not_supported @p bpp is not 1, 3 or 4.
 * @retval k_ra8_fail              The file could not be written.
 * @pre @p px holds `w * h * bpp` readable bytes.
 * @pre @p path names a writable location.
 * @post On success the file holds a valid P5/P6 PPM.
 * @post On any error no partial file is left behind.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_write_ppm(const char* path, const uint8_t* px, uint16_t w, uint16_t h, uint8_t bpp)
{
  if ((bpp != (uint8_t)k_fmt_ver_gray) && (bpp != (uint8_t)k_fmt_ver_rgb) &&
      (bpp != (uint8_t)k_fmt_ver_rgba)) {
    return k_ra8_err_not_supported;
  }
  FILE* f = fopen(path, "wb");
  if (f == nullptr) {
    return k_ra8_fail;
  }
  (void)fprintf(f,
                "P%c\n%u %u\n%u\n",
                (bpp == (uint8_t)k_fmt_ver_gray) ? '5' : '6',
                (unsigned)w,
                (unsigned)h,
                (unsigned)k_fmt_ppm_max);
  const size_t npx = (size_t)w * (size_t)h;
  bool         ok  = true;
  if (bpp == (uint8_t)k_fmt_ver_rgba) {
    /* Drop alpha one pixel at a time: PPM carries colour channels only. */
    for (size_t i = 0U; (i < npx) && ok; ++i) {
      ok = (fwrite(&px[i * (size_t)k_fmt_ver_rgba], 1U, (size_t)k_fmt_ver_rgb, f) ==
            (size_t)k_fmt_ver_rgb);
    }
  } else {
    const size_t n = npx * (size_t)bpp;
    ok             = (fwrite(px, 1U, n, f) == n);
  }
  const bool cl = (fclose(f) == 0);
  if (!ok || !cl) {
    (void)remove(path);
    return k_ra8_fail;
  }
  return k_ra8_ok;
}

/**
 * @brief Compare two rasters and report the first differing pixels.
 * @details Reports differences as `(x, y)` pixel coordinates rather than flat
 *          byte offsets, because a duplication defect shows as a contiguous run
 *          of differing rows -- the row number is the diagnostic signal.
 * @param[in] a      Reference raster (non-nullptr).
 * @param[in] b      Raster under test (non-nullptr).
 * @param[in] len    Byte length of both rasters.
 * @param[in] stride Bytes per raster row.
 * @param[in] bpp    Bytes per pixel.
 * @param[in] out    Report sink (non-nullptr).
 * @return Count of differing bytes.
 * @retval 0 The rasters are byte-identical.
 * @pre @p a and @p b each hold @p len readable bytes.
 * @pre @p stride is non-zero.
 * @post At most ::k_fmt_ver_max_diffs difference lines were written.
 * @post Neither raster is mutated.
 * @note Not thread-safe (writes one stream).
 * @since 0.1.0
 */
RA8_INTERNAL
static size_t priv_diff_rasters(const uint8_t* a,
                                const uint8_t* b,
                                size_t         len,
                                size_t         stride,
                                uint8_t        bpp,
                                FILE*          out)
{
  size_t diffs = 0U;
  size_t shown = 0U;
  for (size_t i = 0U; i < len; ++i) { /* bounded by len */
    if (a[i] == b[i]) {
      continue;
    }
    diffs++;
    if (shown < (size_t)k_fmt_ver_max_diffs) {
      const size_t y = i / stride;
      const size_t x = (i % stride) / (size_t)bpp;
      (void)fprintf(out,
                    "  DIFF at pixel (%zu, %zu): reference=0x%02X banded=0x%02X\n",
                    x,
                    y,
                    (unsigned)a[i],
                    (unsigned)b[i]);
      shown++;
    }
  }
  return diffs;
}

/**
 * @brief Encode @p src with the given tiling and reassemble it to a raster.
 * @details Produces a deflate-coded JOF atlas from @p src at the requested tile
 *          size, then reassembles that atlas straight back to a flat raster and
 *          frees the intermediate container. `verify` runs this twice -- once
 *          whole-image, once banded -- and compares the two rasters, so an
 *          encoder/decoder mismatch surfaces as a pixel difference.
 * @param[in,out] arena Bump allocator for encode/decode scratch.
 * @param[in]     src   Source image bytes (non-nullptr).
 * @param[in]  max_w  Width cap for the work-arena sizing.
 * @param[in]  max_h  Height cap for the work-arena sizing.
 * @param[in]  tile_w Tile width to request.
 * @param[in]  tile_h Tile height to request.
 * @param[out] out_px Receives the owned raster.
 * @param[out] out_n  Receives the raster byte count.
 * @param[out] info   Receives the produced geometry.
 * @return Result code propagated from the producer or the reassembler.
 * @retval k_ra8_ok Raster reassembled into `*out_px`.
 * @pre @p src holds a decodable image.
 * @pre Every output pointer is writable.
 * @post On success `*out_px` must be released by the caller.
 * @post On any error `*out_px` is nullptr.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_encode_roundtrip(ra8_arena_t*          arena,
                                       const ra8_fmt_blob_t* src,
                                       uint16_t              max_w,
                                       uint16_t              max_h,
                                       uint16_t              tile_w,
                                       uint16_t              tile_h,
                                       uint8_t**             out_px,
                                       size_t*               out_n,
                                       ra8_jof_info_t*       info)
{
  ra8_fmt_blob_t atlas = {};
  ra8_err_t      rc    = ra8_fmt_jof_produce(arena,
                                     src,
                                     max_w,
                                     max_h,
                                     tile_w,
                                     tile_h,
                                     (uint8_t)k_ra8_jof_codec_deflate,
                                     &atlas,
                                     info);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = ra8_fmt_jof_reassemble(arena, &atlas, info, out_px, out_n);
  ra8_fmt_blob_free(&atlas);
  return rc;
}

/**
 * @brief Encode the source both ways and report the geometry line.
 * @details Splits the two round-trips out of ::ra8_fmt_jof_verify so that
 *          entry point stays within the statement budget. The reference is one
 *          whole-image tile; the subject is banded at ::k_fmt_ver_band_h.
 * @param[in]  src   Source image bytes (non-nullptr).
 * @param[in]  opts  Options carrying the report sink.
 * @param[out] pair  Receives both rasters and both geometries.
 * @return Result code from the probe or either encode.
 * @retval k_ra8_ok Both rasters are resident in @p pair.
 * @pre @p src holds a decodable image.
 * @pre @p pair is writable.
 * @post On success both `pair->ref` and `pair->got` must be freed by the caller.
 * @post On any error neither raster is owned by the caller.
 * @note Not thread-safe (the producer keeps module-static state).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_encode_pair(ra8_arena_t*          arena,
                                  const ra8_fmt_blob_t* src,
                                  const ra8_fmt_opts_t* opts,
                                  fmt_ver_pair_t*       pair)
{
  uint16_t  w  = 0U;
  uint16_t  h  = 0U;
  ra8_err_t rc = ra8_fmt_jof_probe(src, &w, &h);
  if (rc != k_ra8_ok) {
    (void)fprintf(opts->report, "verify: cannot read source dimensions (rc=%d)\n", (int)rc);
    return rc;
  }
  rc = priv_encode_roundtrip(arena, src, w, h, w, h, &pair->ref, &pair->refn, &pair->rinfo);
  if (rc != k_ra8_ok) {
    (void)fprintf(opts->report, "verify: reference encode failed (rc=%d)\n", (int)rc);
    return rc;
  }
  const uint16_t band = (pair->rinfo.height < (uint16_t)k_fmt_ver_band_h)
                          ? pair->rinfo.height
                          : (uint16_t)k_fmt_ver_band_h;
  rc                  = priv_encode_roundtrip(arena,
                             src,
                             w,
                             h,
                             pair->rinfo.width,
                             band,
                             &pair->got,
                             &pair->gotn,
                             &pair->ginfo);
  if (rc != k_ra8_ok) {
    (void)fprintf(opts->report, "verify: banded encode failed (rc=%d)\n", (int)rc);
    pair->ref = nullptr;
    return rc;
  }
  (void)fprintf(opts->report,
                "verify: %ux%u bpp=%u | reference 1 tile | banded %u tiles of %u rows\n",
                (unsigned)pair->rinfo.width,
                (unsigned)pair->rinfo.height,
                (unsigned)pair->rinfo.bpp,
                (unsigned)pair->ginfo.tile_count,
                (unsigned)pair->ginfo.tile_h);
  return k_ra8_ok;
}

ra8_err_t
ra8_fmt_jof_verify(ra8_arena_t* arena, const ra8_fmt_blob_t* src, const ra8_fmt_opts_t* opts)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(opts, s_tag, "opts must not be nullptr");
  fmt_ver_pair_t  pair = {};
  const ra8_err_t rc   = priv_encode_pair(arena, src, opts, &pair);
  if (rc != k_ra8_ok) {
    return rc;
  }
  const uint8_t* const ref   = pair.ref;
  const uint8_t* const got   = pair.got;
  const size_t         refn  = pair.refn;
  const size_t         gotn  = pair.gotn;
  const ra8_jof_info_t rinfo = pair.rinfo;
  const ra8_jof_info_t ginfo = pair.ginfo;
  size_t               diffs = (refn != gotn) ? refn : 0U;
  if (refn == gotn) {
    diffs = priv_diff_rasters(ref,
                              got,
                              refn,
                              (size_t)rinfo.width * (size_t)rinfo.bpp,
                              rinfo.bpp,
                              opts->report);
  } else {
    (void)fprintf(opts->report, "  raster size mismatch: %zu vs %zu bytes\n", refn, gotn);
  }
  if (opts->out_path != nullptr) {
    const ra8_err_t prc = priv_write_ppm(opts->out_path, got, ginfo.width, ginfo.height, ginfo.bpp);
    (void)fprintf(opts->report,
                  "  wrote reassembled raster to %s (%s)\n",
                  opts->out_path,
                  (prc == k_ra8_ok) ? "ok" : "FAILED");
  }
  (void)fprintf(opts->report,
                "verdict: %s (%zu differing bytes)\n",
                (diffs == 0U) ? "ROUND-TRIP EXACT -- the produced file is correct" : "MISMATCH",
                diffs);
  return (diffs == 0U) ? k_ra8_ok : k_ra8_err_validation_failed;
}
