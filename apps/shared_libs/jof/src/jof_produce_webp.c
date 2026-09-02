/**
 * @file jof_produce_webp.c
 * @brief Transcode producer: whole-frame WebP arm (#290 normalize-on-import).
 *
 * @details
 * The producer normalises WebP manifest images to the one on-device JOF
 * band-tile format alongside JPEG and PNG, so the reader consumes a single
 * format regardless of source. WebP is not stripe-decodable (its lossless
 * VP8L mode back-references the whole frame), so -- unlike the streaming
 * JPEG/PNG arms -- it carries an honest whole-frame memory cost isolated in
 * the caller's separate `webp_work` arena: the compressed source, the decoded
 * RGBA frame and libwebp's scratch are all resident at once, fail-closed on
 * any shortfall, no downscaling. Once decoded the frame is banded out through
 * the shared band accumulator (`priv_jof_on_rows()`), so the emitted atlas
 * bytes are identical to the streaming arms for the same pixels. This arm is
 * dispatched from `jof_produce.c` and shares the producer state and
 * geometry / rows / prefix-pull seams through `jof_internal.h`; it
 * allocates nothing on the heap (libwebp scratch is drawn from `webp_work`
 * through the `ra8_webp` bump arena).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Domain]
 * {World: NS}
 */

#include <stddef.h>
#include <stdint.h>

#include "jof_internal.h"
#include "jof_produce.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_webp.h"
#include "ra8_webp_arena.h"

/**
 * @enum jof_webp_const_t
 * @brief WebP whole-frame arena sizing constants (`webp_work` layout).
 *
 * @details The WebP decode holds the compressed source, the decoded RGBA frame
 *          and libwebp's scratch simultaneously; the scratch bound is a whole-
 *          frame-scale over-estimate (libwebp's VP8L path reconstructs the full
 *          image internally) plus a fixed slack. Under-sizing merely fails the
 *          decode closed -- it can never overrun.
 */
typedef enum : uint32_t {
  k_jof_webp_bpp           = 4U,                 /**< WebP decodes to RGBA8888.      */
  k_jof_webp_scratch_mult  = 2U,                 /**< Scratch >= this * frame bytes. */
  k_jof_webp_scratch_slack = 1U * 1024U * 1024U, /**< Fixed scratch slack (bytes).   */
} jof_webp_const_t;

uint32_t jof_webp_work_bytes(uint16_t max_width, uint16_t max_height, uint32_t max_src_bytes)
{
  if ((max_width == 0U) || ((uint32_t)max_width > (uint32_t)k_ra8_webp_max_dim) ||
      (max_height == 0U) || ((uint32_t)max_height > (uint32_t)k_ra8_webp_max_dim) ||
      (max_src_bytes == 0U)) {
    return 0U;
  }
  const uint64_t frame = (uint64_t)max_width * (uint64_t)max_height * (uint64_t)k_jof_webp_bpp;
  const uint64_t scratch =
    ((uint64_t)k_jof_webp_scratch_mult * frame) + (uint64_t)k_jof_webp_scratch_slack;
  const uint64_t total = (uint64_t)max_src_bytes + frame + scratch + (uint64_t)k_jof_carve_slack;
  if (total > (uint64_t)UINT32_MAX) {
    return 0U;
  }
  return (uint32_t)total;
}

/**
 * @brief Pull the whole compressed WebP source into the `webp_work` arena.
 * @details WebP is not stripe-decodable (its lossless mode back-references the
 *          whole frame), so libwebp needs the entire compressed source
 *          contiguous. Fills @p dst front to back; a source longer than the
 *          arena affords fails closed (`k_ra8_err_invalid_size`).
 * @param[in]  pfx     Prefix-replay pull adapter over the source.
 * @param[out] dst     Arena front (receives the source bytes).
 * @param[in]  cap     Bytes available at @p dst.
 * @param[out] out_len Receives the source byte count.
 * @return Result code.
 * @retval k_ra8_ok               Whole source resident in @p dst.
 * @retval k_ra8_err_invalid_size The source exceeds @p cap (fail-closed).
 * @retval other                  Propagated from the pull callback.
 * @pre @p dst holds @p cap writable bytes.
 * @pre @p pfx replays the sniffed head before the live source.
 * @post On success `*out_len <= cap` bytes are resident from `dst[0]`.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_webp_pull_all(jof_prefix_pull_t* pfx, uint8_t* dst, size_t cap, size_t* out_len)
{
  size_t len = 0U;
  while (len < cap) {
    size_t          got = 0U;
    const ra8_err_t err = priv_jof_prefix_pull(pfx, &dst[len], cap - len, &got);
    if (err != k_ra8_ok) {
      return err;
    }
    if (got == 0U) {
      *out_len = len;
      return k_ra8_ok;
    }
    len += got;
  }
  /* The arena filled without hitting end of stream: the source is at least as
   * large as the whole arena, so there is no room for the frame + scratch. */
  return k_ra8_err_invalid_size;
}

/**
 * @brief Band a decoded RGBA frame through the shared tile path.
 * @details Feeds the whole-frame RGBA buffer to ::priv_jof_on_rows in
 *          `tile_h` slices, so the WebP path reuses the identical
 *          band/cut/encode/sink machinery as the streaming decoders
 *          (byte-identical output).
 * @param[in,out] st    Producer state (geometry already bound).
 * @param[in]     frame Decoded RGBA8888 pixels, `h` rows of `w * 4` bytes.
 * @param[in]     w     Frame width, pixels.
 * @param[in]     h     Frame height, pixels.
 * @return Result code.
 * @retval k_ra8_ok Every row was fed; bands flushed as they filled.
 * @retval other    Propagated from ::priv_jof_on_rows.
 * @pre `priv_jof_on_geom()` has fired (`geom_done == 1`) with these w/h/4bpp.
 * @pre @p frame holds `h * w * 4` readable bytes.
 * @post On success `rows_seen == h`; the last partial band is left for the
 *       epilogue to flush.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_webp_feed(jof_prod_state_t* st, const uint8_t* frame, uint16_t w, uint16_t h)
{
  const uint32_t stride = (uint32_t)w * (uint32_t)k_jof_webp_bpp;
  const uint32_t th     = (uint32_t)st->cfg->tile_h;
  uint32_t       y      = 0U;
  while (y < (uint32_t)h) {
    const uint32_t  nrows = (((uint32_t)h - y) < th) ? ((uint32_t)h - y) : th;
    const ra8_err_t err   = priv_jof_on_rows(st,
                                             &frame[(size_t)y * (size_t)stride],
                                             w,
                                             (uint16_t)y,
                                             (uint16_t)nrows,
                                             (uint8_t)k_jof_webp_bpp);
    if (err != k_ra8_ok) {
      return err;
    }
    y += nrows;
  }
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_jof_webp_transcode(jof_prod_state_t* st, jof_prefix_pull_t* pfx)
{
  uint8_t* const arena = st->cfg->webp_work;
  if (arena == nullptr) {
    return k_ra8_err_not_supported; /* WebP source but no whole-frame arena */
  }
  const size_t acap    = st->cfg->webp_work_cap;
  size_t       src_len = 0U;
  ra8_err_t    err     = internal_webp_pull_all(pfx, arena, acap, &src_len);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t w = 0U;
  uint32_t h = 0U;
  err        = ra8_webp_get_info(arena, src_len, &w, &h);
  if (err != k_ra8_ok) {
    return err;
  }
  err = priv_jof_on_geom(st, (uint16_t)w, (uint16_t)h, (uint8_t)k_jof_webp_bpp);
  if (err != k_ra8_ok) {
    return err;
  }
  const size_t   mask    = (size_t)k_jof_bump_align - 1U;
  const size_t   used    = (src_len + mask) & ~mask;
  const uint64_t frame64 = (uint64_t)w * (uint64_t)h * (uint64_t)k_jof_webp_bpp;
  if ((used >= acap) || (frame64 >= ((uint64_t)acap - (uint64_t)used))) {
    return k_ra8_err_invalid_size; /* no room for frame + at least some scratch */
  }
  const size_t     frame_n = (size_t)frame64;
  uint8_t* const   frame   = &arena[used];
  const size_t     scr_off = used + frame_n;
  ra8_webp_arena_t wa = {.base = &arena[scr_off], .cap = acap - scr_off, .offset = 0U, .live = 0U};
  err                 = ra8_webp_decode_rgba(arena,
                                             src_len,
                                             &wa,
                                             frame,
                                             (size_t)w * (size_t)k_jof_webp_bpp,
                                             frame_n,
                                             nullptr,
                                             nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_webp_feed(st, frame, (uint16_t)w, (uint16_t)h);
}
