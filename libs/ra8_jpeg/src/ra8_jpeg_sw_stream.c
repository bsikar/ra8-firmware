/**
 * @file ra8_jpeg_sw_stream.c
 * @brief Streaming baseline JPEG decoder: bounded-RAM MCU-row stripes (#231).
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * Implements `ra8_jpeg_sw_decode_stripes()`: the same baseline (8-bit,
 * sequential, Huffman) decode as `ra8_jpeg_sw_decode()`, but the compressed
 * input arrives through a pull callback into a caller-owned sliding window
 * and the decoded output leaves through a stripe callback one MCU row at a
 * time -- resident RAM is `window + one stripe`, independent of image size.
 *
 * The marker parsers, the per-block entropy decoder and the MCU block
 * helpers are shared with the whole-buffer driver (`ra8_jpeg_sw_decode.c`)
 * via `ra8_jpeg_sw_internal.h`; this unit adds only the window management
 * (slide + refill around the shared ::ra8_jpeg_bitreader_t) and the
 * emit-into-stripe pixel stage.
 *
 * Spec citations are tagged `T.81 sec X.Y "..."` and refer to
 * ITU-T Recommendation T.81 (1992) | ISO/IEC 10918-1.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_jpeg_sw.h"
#include "ra8_jpeg_sw_internal.h"
#include "ra8_log.h"

/** @brief Component log tag. */
static const char* s_tag = "JPEG_SW";

/**
 * @enum ra8_jpeg_stream_const_t
 * @brief Streaming-driver bounds (loop caps and phase sizes).
 */
typedef enum : uint16_t {
  k_ra8_jpeg_stream_max_markers = 1024U, /**< Marker-segment dispatch cap. */
  k_ra8_jpeg_stream_soi_bytes   = 2U,    /**< SOI marker size.             */
  k_ra8_jpeg_stream_gray_ch     = 1U,    /**< Grayscale output channels.   */
  k_ra8_jpeg_stream_rgb_ch      = 3U,    /**< Colour output channels.      */
} ra8_jpeg_stream_const_t;

/**
 * @struct ra8_jpeg_stream_state_t
 * @brief Whole streaming-decode state: source window + geometry + stripe.
 *
 * @details One module-static instance (mirrors the whole-buffer driver's
 *          static context): the decoder tables alone are tens of KiB, which
 *          would blow the stack budget.
 *
 * @invariant `win_len <= win_cap` and `stripe_cap >= width * mcu_h * ch`
 *            once the geometry callback has accepted the image.
 */
typedef struct {
  ra8_jpeg_sw_pull_fn pull;     /**< Sequential byte source.              */
  void*               pull_ctx; /**< Context for `pull`.                  */
  uint8_t*            win;      /**< Sliding window base.                 */
  uint32_t            win_cap;  /**< Window capacity, bytes.              */
  uint32_t            win_len;  /**< Valid bytes currently in the window. */
  uint8_t             eof;      /**< 1 once the source reported EOF.      */

  ra8_jpeg_sw_rows_fn on_rows; /**< Stripe sink.                         */
  void*               cb_ctx;  /**< Consumer context for both callbacks. */

  uint8_t* stripe;     /**< Consumer stripe buffer (geometry cb). */
  uint32_t stripe_cap; /**< Stripe buffer capacity, bytes.        */
  uint8_t  channels;   /**< Output channels (1 or 3).             */
  uint16_t mcu_w;      /**< MCU width, pixels.                    */
  uint16_t mcu_h;      /**< MCU height, pixels (== stripe rows).  */
  uint16_t mcus_x;     /**< MCU columns.                          */
  uint16_t mcus_y;     /**< MCU rows.                             */

  ra8_jpeg_dec_ctx_t dec; /**< Shared decoder tables + parse state. */
} ra8_jpeg_stream_state_t;

/** @brief Module-static streaming state (codec documented not thread-safe). */
static ra8_jpeg_stream_state_t s_js;

/**
 * @brief Top the window up from the pull source until full or EOF.
 * @details Each non-EOF pull delivers at least one byte, so the loop is
 *          bounded by the window capacity (NASA Rule 2).
 * @param[in,out] st Streaming state (window mutates).
 * @return Result code.
 * @retval k_ra8_ok Window is full or the source hit EOF.
 * @retval other    Propagated from the pull callback.
 * @pre `st->win` covers `st->win_cap` bytes.
 * @pre `st->win_len <= st->win_cap`.
 * @post `st->win_len == st->win_cap` or `st->eof == 1` (on success).
 * @post On error the window content below `win_len` is still valid.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t js_refill(ra8_jpeg_stream_state_t* st)
{
  while ((st->eof == 0U) && (st->win_len < st->win_cap)) {
    size_t          got = 0U;
    const ra8_err_t err =
      st->pull(st->pull_ctx, &st->win[st->win_len], (size_t)(st->win_cap - st->win_len), &got);
    if (err != k_ra8_ok) {
      return err;
    }
    if (got == 0U) {
      st->eof = 1U;
    } else {
      st->win_len += (uint32_t)got;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Discard @p consumed leading window bytes and refill.
 * @details Memmoves the unread tail to the window head, then tops the window up.
 * @param[in,out] st       Streaming state (window mutates).
 * @param[in]     consumed Bytes to drop from the window head.
 * @return Result code.
 * @retval k_ra8_ok The window slid and refilled (or EOF).
 * @retval other    Propagated from the pull callback.
 * @pre `consumed <= st->win_len`.
 * @pre `st->win` covers `st->win_cap` bytes.
 * @post The former byte at `consumed` is now at offset 0.
 * @post `st->win_len` reflects the slide plus any refill.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t js_slide(ra8_jpeg_stream_state_t* st, uint32_t consumed)
{
  if (consumed > 0U) {
    (void)memmove(st->win, &st->win[consumed], (size_t)(st->win_len - consumed));
    st->win_len -= consumed;
  }
  return js_refill(st);
}

/**
 * @brief Validate the SOF0 geometry and fire the consumer geometry callback.
 * @details Computes the MCU grid, asks the consumer for its stripe buffer,
 *          and checks the buffer covers one full stripe. The channel count
 *          is 1 for grayscale sources and 3 (RGB888) otherwise.
 * @param[in,out] st      Streaming state (geometry fields written).
 * @param[in]     on_geom Consumer geometry callback.
 * @return Result code.
 * @retval k_ra8_ok               Geometry accepted; stripe bound.
 * @retval k_ra8_err_invalid_size The supplied stripe buffer is too small.
 * @retval k_ra8_err_null_ptr     The consumer supplied a NULL stripe.
 * @retval other                  The consumer aborted the decode.
 * @pre `st->dec` holds a successfully parsed SOF0 (hmax/vmax non-zero).
 * @pre @p on_geom is non-NULL.
 * @post On success `st->stripe`/`stripe_cap`/grid fields are set.
 * @post On any error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t js_bind_geometry(ra8_jpeg_stream_state_t* st, ra8_jpeg_sw_geom_fn on_geom)
{
  const ra8_jpeg_dec_ctx_t* d = &st->dec;
  st->channels =
    (d->ncomp == 1U) ? (uint8_t)k_ra8_jpeg_stream_gray_ch : (uint8_t)k_ra8_jpeg_stream_rgb_ch;
  st->mcu_w  = (uint16_t)((uint16_t)k_ra8_jpeg_block_dim * d->hmax);
  st->mcu_h  = (uint16_t)((uint16_t)k_ra8_jpeg_block_dim * d->vmax);
  st->mcus_x = (uint16_t)((d->width + st->mcu_w - 1U) / st->mcu_w);
  st->mcus_y = (uint16_t)((d->height + st->mcu_h - 1U) / st->mcu_h);

  uint8_t*        stripe     = nullptr;
  uint32_t        stripe_cap = 0U;
  const ra8_err_t err =
    on_geom(st->cb_ctx, d->width, d->height, st->channels, st->mcu_h, &stripe, &stripe_cap);
  if (err != k_ra8_ok) {
    return err;
  }
  RA8_CHECK_NULL_PTR(stripe, s_tag, "geometry callback supplied a NULL stripe");
  const uint32_t need = (uint32_t)d->width * (uint32_t)st->mcu_h * (uint32_t)st->channels;
  if (stripe_cap < need) {
    return k_ra8_err_invalid_size;
  }
  st->stripe     = stripe;
  st->stripe_cap = stripe_cap;
  return k_ra8_ok;
}

/**
 * @brief Walk marker segments until SOS, firing the geometry callback.
 * @details Each iteration slides the consumed bytes off, refills the window
 *          (so any single segment, <= 65539 bytes, is fully materialised),
 *          and dispatches one marker through the shared parser. Bounded by
 *          ::k_ra8_jpeg_stream_max_markers dispatches (NASA Rule 2).
 * @param[in,out] st           Streaming state.
 * @param[in]     on_geom      Consumer geometry callback.
 * @param[out]    out_scan_pos Window offset of the entropy-coded data.
 * @return Result code.
 * @retval k_ra8_ok                 SOS reached; scan may start.
 * @retval k_ra8_err_protocol_error Malformed stream / EOI before SOS /
 *                                  marker budget exhausted.
 * @retval k_ra8_err_not_supported  Non-baseline stream.
 * @retval other                    Propagated from pull / the callback.
 * @pre The SOI marker has already been consumed.
 * @pre `st->win` has been refilled once.
 * @post On success the geometry callback fired exactly once.
 * @post On any error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t
js_parse_markers(ra8_jpeg_stream_state_t* st, ra8_jpeg_sw_geom_fn on_geom, uint32_t* out_scan_pos)
{
  bool got_sof   = false;
  bool geom_done = false;
  for (uint16_t iter = 0U; iter < (uint16_t)k_ra8_jpeg_stream_max_markers; iter++) {
    if (st->win_len == 0U) {
      return k_ra8_err_protocol_error; /* stream ended before SOS */
    }
    st->dec.src                         = st->win;
    st->dec.src_len                     = st->win_len;
    st->dec.cursor                      = 0U;
    ra8_jpeg_dec_marker_action_t action = k_ra8_jpeg_dec_continue;
    ra8_err_t                    err    = ra8_jpeg_sw_priv_dispatch(&st->dec, &got_sof, &action);
    if (err != k_ra8_ok) {
      return err;
    }
    if (got_sof && !geom_done) {
      err = js_bind_geometry(st, on_geom);
      if (err != k_ra8_ok) {
        return err;
      }
      geom_done = true;
    }
    if (action == k_ra8_jpeg_dec_scan) {
      *out_scan_pos = st->dec.cursor;
      return k_ra8_ok;
    }
    if (action == k_ra8_jpeg_dec_eoi) {
      return k_ra8_err_protocol_error; /* EOI before any scan */
    }
    err = js_slide(st, st->dec.cursor);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_err_protocol_error; /* marker budget exhausted (hostile stream) */
}

/**
 * @brief Emit one decoded MCU's visible pixels into the stripe buffer.
 * @details Streaming twin of the whole-buffer driver's emit stage: rows are
 *          stripe-local (`r` counts from the top of the current MCU row),
 *          columns clamp at the image's right edge, and grayscale sources
 *          copy Y directly instead of running the colour transform.
 * @param[in] st      Streaming state (stripe + geometry).
 * @param[in] y_tile  Reconstructed luma tile (`mcu_w * mcu_h`).
 * @param[in] cb_tile Reconstructed Cb tile (64 bytes; colour only).
 * @param[in] cr_tile Reconstructed Cr tile (64 bytes; colour only).
 * @param[in] mx      MCU column index.
 * @param[in] rows    Valid rows in this stripe (edge stripes are short).
 * @pre `st->stripe` covers one full stripe (bound at geometry time).
 * @pre `rows <= st->mcu_h`.
 * @post The MCU's visible pixels are packed into the stripe.
 * @post No decoder state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void js_emit_mcu(const ra8_jpeg_stream_state_t* st,
                        const uint8_t*                 y_tile,
                        const uint8_t*                 cb_tile,
                        const uint8_t*                 cr_tile,
                        uint16_t                       mx,
                        uint16_t                       rows)
{
  const ra8_jpeg_dec_ctx_t* d      = &st->dec;
  const uint32_t            stride = (uint32_t)d->width * (uint32_t)st->channels;
  for (uint16_t r = 0U; r < rows; r++) {
    for (uint16_t c = 0U; c < st->mcu_w; c++) {
      const uint32_t px = ((uint32_t)mx * (uint32_t)st->mcu_w) + (uint32_t)c;
      if (px >= (uint32_t)d->width) {
        break;
      }
      const int32_t y = (int32_t)y_tile[((uint32_t)r * (uint32_t)st->mcu_w) + (uint32_t)c];
      if (st->channels == (uint8_t)k_ra8_jpeg_stream_gray_ch) {
        st->stripe[((uint32_t)r * stride) + px] = (uint8_t)y;
        continue;
      }
      const uint16_t cx  = (uint16_t)(c / d->hmax);
      const uint16_t cy  = (uint16_t)(r / d->vmax);
      const int32_t  cb  = (int32_t)cb_tile[((uint32_t)cy * (uint32_t)k_ra8_jpeg_block_dim) + cx];
      const int32_t  cr  = (int32_t)cr_tile[((uint32_t)cy * (uint32_t)k_ra8_jpeg_block_dim) + cx];
      const uint32_t idx = ((uint32_t)r * stride) + (px * (uint32_t)st->channels);
      ra8_jpeg_sw_ycc_to_rgb(y,
                             cb,
                             cr,
                             &st->stripe[idx],
                             &st->stripe[idx + 1U],
                             &st->stripe[idx + 2U]);
    }
  }
}

/**
 * @brief Keep the entropy window topped up ahead of the bit reader.
 * @details Slides the already-consumed bytes (`br->pos`) off the window and
 *          refills whenever fewer than the scan margin remain unread, then
 *          rebases the bit reader onto the slid window. The accumulator
 *          bits survive the rebase untouched.
 * @param[in,out] st Streaming state (window mutates).
 * @param[in,out] br Bit reader over the window (rebased in place).
 * @return Result code.
 * @retval k_ra8_ok The reader has margin bytes or the source hit EOF.
 * @retval other    Propagated from the pull callback.
 * @pre `br->buf == st->win` and `br->pos <= st->win_len`.
 * @pre `st->win_cap` >= ::k_ra8_jpeg_sw_stream_min_window.
 * @post `br->len == st->win_len` and `br->pos` is rebased.
 * @post On error the scan aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t js_scan_margin(ra8_jpeg_stream_state_t* st, ra8_jpeg_bitreader_t* br)
{
  if ((st->eof == 0U) && ((st->win_len - br->pos) < (uint32_t)k_ra8_jpeg_sw_stream_scan_margin)) {
    const uint32_t  shift = br->pos;
    const ra8_err_t err   = js_slide(st, shift);
    if (err != k_ra8_ok) {
      return err;
    }
    br->pos = 0U;
    br->len = st->win_len;
  }
  return k_ra8_ok;
}

/**
 * @brief Decode + emit one MCU: margin slide, luma, chroma, stripe write.
 * @details Keeps the window margin ahead of the bit reader, then reuses the
 *          shared block decoders and the stripe emit stage.
 * @param[in,out] st      Streaming state.
 * @param[in,out] br      Bit reader over the window.
 * @param[in]     y_tile  Luma tile scratch (mcu_w * mcu_h bytes).
 * @param[in,out] cb_tile Cb tile scratch (64 bytes).
 * @param[in,out] cr_tile Cr tile scratch (64 bytes).
 * @param[in]     mx      MCU column index.
 * @param[in]     rows    Valid rows in this stripe.
 * @return Result code.
 * @retval k_ra8_ok                 MCU decoded and emitted into the stripe.
 * @retval k_ra8_err_protocol_error Entropy stream corrupt / truncated.
 * @retval other                    Propagated from the pull callback.
 * @pre The geometry and tables are bound (post `js_parse_markers()`).
 * @pre All three tile buffers are writable.
 * @post `br` advanced past one MCU's blocks.
 * @post On error the scan aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t js_decode_mcu(ra8_jpeg_stream_state_t* st,
                               ra8_jpeg_bitreader_t*    br,
                               uint8_t*                 y_tile,
                               uint8_t*                 cb_tile,
                               uint8_t*                 cr_tile,
                               uint16_t                 mx,
                               uint16_t                 rows)
{
  ra8_err_t err = js_scan_margin(st, br);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_jpeg_sw_priv_mcu_y(&st->dec, br, y_tile, st->mcu_w);
  if (err != k_ra8_ok) {
    return err;
  }
  if (st->dec.ncomp == 3U) {
    err = ra8_jpeg_sw_priv_mcu_chroma(&st->dec, br, cb_tile, cr_tile);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  js_emit_mcu(st, y_tile, cb_tile, cr_tile, mx, rows);
  return k_ra8_ok;
}

/**
 * @brief Decode the whole entropy-coded scan, one MCU row per stripe.
 * @details Outer loops are bounded by the SOF0-derived MCU grid (NASA
 *          Rule 2). Each MCU row fills the stripe buffer, then the stripe
 *          callback fires with the row's true (edge-clamped) height.
 * @param[in,out] st       Streaming state.
 * @param[in]     scan_pos Window offset of the entropy-coded data.
 * @return Result code.
 * @retval k_ra8_ok                 Every stripe emitted.
 * @retval k_ra8_err_protocol_error Entropy stream corrupt / truncated.
 * @retval other                    Propagated from pull / the stripe sink.
 * @pre `js_parse_markers()` succeeded (geometry + tables bound).
 * @pre `scan_pos <= st->win_len`.
 * @post On success `mcus_y` stripes were emitted in order.
 * @post On any error emission stops at the failing stripe.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t js_scan(ra8_jpeg_stream_state_t* st, uint32_t scan_pos)
{
  ra8_jpeg_dec_ctx_t*  d  = &st->dec;
  ra8_jpeg_bitreader_t br = {
    .buf     = st->win,
    .len     = st->win_len,
    .pos     = scan_pos,
    .acc     = 0U,
    .nbits   = 0U,
    .had_eoi = 0U,
  };
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_max_comps; i++) {
    d->comp_dc_pred[i] = 0;
  }
  uint8_t y_tile[(uint32_t)k_ra8_jpeg_mcu_max_dim * (uint32_t)k_ra8_jpeg_mcu_max_dim] = {};
  uint8_t cb_tile[(uint32_t)k_ra8_jpeg_block_size]                                    = {};
  uint8_t cr_tile[(uint32_t)k_ra8_jpeg_block_size]                                    = {};

  for (uint16_t my = 0U; my < st->mcus_y; my++) {
    const uint32_t y0   = (uint32_t)my * (uint32_t)st->mcu_h;
    const uint32_t left = (uint32_t)d->height - y0;
    const uint16_t rows = (uint16_t)((left < (uint32_t)st->mcu_h) ? left : (uint32_t)st->mcu_h);
    for (uint16_t mx = 0U; mx < st->mcus_x; mx++) {
      const ra8_err_t err = js_decode_mcu(st, &br, y_tile, cb_tile, cr_tile, mx, rows);
      if (err != k_ra8_ok) {
        return err;
      }
    }
    const ra8_err_t err =
      st->on_rows(st->cb_ctx, st->stripe, d->width, (uint16_t)y0, rows, st->channels);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Bind the source, prime the window and consume the SOI marker.
 * @details Resets the module-static state, fills the window once, and
 *          verifies the stream begins with the SOI marker.
 * @param[in,out] st         Streaming state (reset + bound).
 * @param[in]     pull       Sequential byte source.
 * @param[in]     pull_ctx   Context for @p pull.
 * @param[in]     window     Sliding window buffer.
 * @param[in]     window_cap Window capacity, bytes.
 * @param[in]     on_rows    Stripe sink.
 * @param[in]     cb_ctx     Consumer context.
 * @return Result code.
 * @retval k_ra8_ok                 SOI consumed; marker walk may start.
 * @retval k_ra8_err_invalid_size   The source is shorter than a marker.
 * @retval k_ra8_err_protocol_error The stream does not begin with SOI.
 * @retval other                    Propagated from the pull callback.
 * @pre The public entry validated every pointer.
 * @pre @p window covers @p window_cap bytes.
 * @post On success the window sits right after the SOI marker.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t js_begin(ra8_jpeg_stream_state_t* st,
                          ra8_jpeg_sw_pull_fn      pull,
                          void*                    pull_ctx,
                          uint8_t*                 window,
                          uint32_t                 window_cap,
                          ra8_jpeg_sw_rows_fn      on_rows,
                          void*                    cb_ctx)
{
  (void)memset(st, 0, sizeof(*st));
  st->pull      = pull;
  st->pull_ctx  = pull_ctx;
  st->win       = window;
  st->win_cap   = window_cap;
  st->on_rows   = on_rows;
  st->cb_ctx    = cb_ctx;
  ra8_err_t err = js_refill(st);
  if (err != k_ra8_ok) {
    return err;
  }
  if (st->win_len < (uint32_t)k_ra8_jpeg_stream_soi_bytes) {
    return k_ra8_err_invalid_size;
  }
  if (read_be16(st->win) != (uint16_t)k_ra8_jpeg_marker_soi) {
    return k_ra8_err_protocol_error;
  }
  return js_slide(st, (uint32_t)k_ra8_jpeg_stream_soi_bytes);
}

ra8_err_t ra8_jpeg_sw_decode_stripes(ra8_jpeg_sw_pull_fn pull,
                                     void*               pull_ctx,
                                     uint8_t*            window,
                                     uint32_t            window_cap,
                                     ra8_jpeg_sw_geom_fn on_geom,
                                     ra8_jpeg_sw_rows_fn on_rows,
                                     void*               cb_ctx)
{
  RA8_CHECK_NULL_PTR(pull, s_tag, "pull must not be nullptr");
  RA8_CHECK_NULL_PTR(window, s_tag, "window must not be nullptr");
  RA8_CHECK_NULL_PTR(on_geom, s_tag, "on_geom must not be nullptr");
  RA8_CHECK_NULL_PTR(on_rows, s_tag, "on_rows must not be nullptr");
  if (window_cap < (uint32_t)k_ra8_jpeg_sw_stream_min_window) {
    return k_ra8_err_invalid_size;
  }
  ra8_jpeg_stream_state_t* st  = &s_js;
  ra8_err_t                err = js_begin(st, pull, pull_ctx, window, window_cap, on_rows, cb_ctx);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t scan_pos = 0U;
  err               = js_parse_markers(st, on_geom, &scan_pos);
  if (err != k_ra8_ok) {
    return err;
  }
  return js_scan(st, scan_pos);
}
