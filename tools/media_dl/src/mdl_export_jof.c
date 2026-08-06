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

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdl_atomic.h"
#include "mdl_export_internal.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "ra8_log.h"

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
 * @see jof_is_webp()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_jof_webp_riff_ofs   = 0U,  /**< Offset of the "RIFF" fourCC.        */
  k_jof_webp_fourcc_ofs = 8U,  /**< Offset of the "WEBP" fourCC.        */
  k_jof_webp_head_len   = 12U, /**< Bytes needed to sniff both fourCCs. */
  k_jof_webp_tag_len    = 4U,  /**< Length of one fourCC tag.           */
} mdl_jof_webp_t;

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
 * @see jof_one()
 * @since 0.1.0
 */
RA8_INTERNAL static bool jof_is_webp(const uint8_t* data, size_t len)
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

/** @brief ra8_log byte sink -> stderr (host-safe; avoids the ITM MMIO read). */
RA8_INTERNAL static void jof_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)fputc((int)byte, stderr);
}

/** @brief Producer pull callback: hand over the next source bytes. */
RA8_INTERNAL static ra8_err_t jof_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
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

/** @brief Producer sink callback: append atlas bytes to an open file. */
RA8_INTERNAL static ra8_err_t jof_sink(void* ctx, const uint8_t* buf, size_t len)
{
  return (fwrite(buf, 1U, len, (FILE*)ctx) == len) ? k_ra8_ok : k_ra8_fail;
}

/** @brief Read a whole file into a malloc'd buffer. */
RA8_INTERNAL static ra8_err_t slurp(ra8_arena_t* arena, const char* path, uint8_t** out_buf, size_t* out_len)
{
  FILE* f = fopen(path, "rb");
  if (f == NULL) {
    // cppcheck-suppress resourceLeak
    // f is NULL here
    return k_ra8_fail;
  }
  (void)fseek(f, 0, SEEK_END);
  const long sz = ftell(f);
  (void)fseek(f, 0, SEEK_SET);
  if (sz <= 0) {
    (void)fclose(f);
    return k_ra8_fail;
  }
  uint8_t* buf = (uint8_t*)ra8_arena_alloc(arena, (uint32_t)sz);
  if ((buf == nullptr) || (fread(buf, 1U, (size_t)sz, f) != (size_t)sz)) {
    
    (void)fclose(f);
    return k_ra8_fail;
  }
  (void)fclose(f);
  *out_buf = buf;
  *out_len = (size_t)sz;
  return k_ra8_ok;
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
 * @return Result code.
 * @retval k_ra8_ok               Non-WebP page, or arena carved successfully.
 * @retval k_ra8_err_invalid_size The geometry does not admit a WebP arena.
 * @retval k_ra8_err_no_mem       The host allocator refused the arena.
 * @pre @p src holds @p slen readable bytes.
 * @pre @p out_work and @p out_cap are writable.
 * @post On success `*out_work` is nullptr (non-WebP) or owned by the caller.
 * @post On failure `*out_work` is nullptr and nothing was allocated.
 * @note Not thread-safe; the caller owns and must free `*out_work`.
 * @see ra8_jof_webp_work_bytes()
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t jof_carve_webp(ra8_arena_t* arena, const uint8_t* src,
                                             size_t         slen,
                                             uint16_t       w,
                                             uint16_t       h,
                                             uint8_t**      out_work,
                                             size_t*        out_cap)
{
  *out_work = nullptr;
  *out_cap  = 0U;
  if (!jof_is_webp(src, slen)) {
    return k_ra8_ok;
  }
  const uint32_t need = ra8_jof_webp_work_bytes(w, h, (uint32_t)slen);
  if (need == 0U) {
    return k_ra8_err_invalid_size;
  }
  uint8_t* mem = (uint8_t*)ra8_arena_alloc(arena, need);
  if (mem == NULL) {
    
    return k_ra8_err_no_mem;
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
 * both on every exit path. Splitting this out of jof_one() means each function
 * owns one kind of resource: this one owns the scratch, jof_one() owns the
 * source buffer and the output file.
 *
 * @param[in]     src      Encoded page bytes.
 * @param[in]     slen     Length of @p src in bytes.
 * @param[in]     w        Decoded page width in pixels.
 * @param[in]     h        Decoded page height in pixels.
 * @param[in]     tile_h   Band height for this page.
 * @param[in]     work_cap Work-arena size, from ::ra8_jof_work_bytes.
 * @param[in,out] out      Open output file the atlas is written to.
 *
 * @return Producer result.
 * @retval k_ra8_ok               The atlas was written to @p out.
 * @retval k_ra8_err_no_mem       An arena could not be allocated.
 * @retval k_ra8_err_invalid_size The geometry admits no WebP arena.
 *
 * @pre @p src holds @p slen readable bytes and @p out is open for writing.
 * @pre @p work_cap is non-zero.
 * @post Both scratch arenas are released, on success and on every failure.
 * @post Nothing is written to @p out beyond what the producer emitted.
 *
 * @note Not thread-safe.
 * @see jof_carve_webp()
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t jof_produce_page(ra8_arena_t* arena, const uint8_t* src,
                                               size_t         slen,
                                               uint16_t       w,
                                               uint16_t       h,
                                               uint16_t       tile_h,
                                               uint32_t       work_cap,
                                               FILE*          out)
{
  uint8_t* work = (uint8_t*)ra8_arena_alloc(arena, work_cap);
  if (work == NULL) {
    
    return k_ra8_err_no_mem;
  }
  uint8_t*        webp_work = nullptr;
  size_t          webp_cap  = 0U;
  const ra8_err_t carve_rc  = jof_carve_webp(arena, src, slen, w, h, &webp_work, &webp_cap);
  if (carve_rc != k_ra8_ok) {
    
    return carve_rc;
  }
  jof_pull_ctx_t        pull = {.data = src, .len = slen, .pos = 0U};
  ra8_jof_produce_cfg_t cfg  = {.pull          = jof_pull,
                                .pull_ctx      = &pull,
                                .sink          = jof_sink,
                                .sink_ctx      = out,
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

/** @brief Transcode one JPEG / PNG / WebP page to a `.jof` full-width-column atlas. */
RA8_INTERNAL static ra8_err_t jof_one(ra8_arena_t* arena, const char* in_path, const char* out_path)
{
  uint8_t*  src  = nullptr;
  size_t    slen = 0U;
  ra8_err_t rc   = slurp(arena, in_path, &src, &slen);
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
  /* Produce into a sibling temp and rename on success: re-exporting a chapter
   * must not destroy the `.jof` already sitting there if this page fails to
   * decode (see mdl_atomic.h). */
  char tmp[PATH_MAX];
  if (!mdl_atomic_tmp_path(out_path, tmp, sizeof(tmp))) {
    
    return k_ra8_fail;
  }
  FILE* out = fopen(tmp, "wb");
  if (out == NULL) {
    
    // cppcheck-suppress resourceLeak
    // out is NULL here
    return k_ra8_fail;
  }
  rc = jof_produce_page(arena, src, slen, w, h, tile_h, work_cap, out);
  if (fclose(out) != 0) {
    rc = (rc == k_ra8_ok) ? k_ra8_fail : rc;
  }
  
  if (rc != k_ra8_ok) {
    mdl_atomic_abort(tmp);
    return rc;
  }
  return mdl_atomic_commit(tmp, out_path) ? k_ra8_ok : k_ra8_fail;
}

RA8_PRIV ra8_err_t mdl_export_jof(ra8_arena_t* arena, const char* dir, const char names[][k_name_max], size_t count)
{
  ra8_log_set_byte_sink(jof_log_sink, nullptr);
  ra8_err_t rc = k_ra8_ok;
  for (size_t i = 0U; (rc == k_ra8_ok) && (i < count); ++i) {
    char in_path[PATH_MAX];
    char stem[k_name_max];
    char out_path[PATH_MAX];
    (void)snprintf(in_path, sizeof(in_path), "%s/%s", dir, names[i]);
    (void)snprintf(stem, sizeof(stem), "%s", names[i]);
    char* dot = strrchr(stem, '.');
    if (dot != nullptr) {
      *dot = '\0';
    }
    (void)snprintf(out_path, sizeof(out_path), "%s/%s.jof", dir, stem);
    rc = jof_one(arena, in_path, out_path);
  }
  return rc;
}
