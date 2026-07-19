/**
 * @file ra8_tileatlas_produce.c
 * @brief Transcode producer: sniff, decode, tile, encode, emit (#231, #290).
 *
 * @details
 * Implements `ra8_tileatlas_produce()` and `ra8_tileatlas_work_bytes()`. The
 * producer owns the band accumulator (one tile row of decoded pixels), the
 * tile cut + intra-encode + sink stage, and the trailing index/footer
 * emission; the pixel rows arrive from `ra8_jpeg_sw_decode_stripes()` (JPEG)
 * or `ra8_ta_priv_png_rows()` (PNG), both bounded-RAM by construction. Every
 * streaming buffer is carved from the caller's `work` arena through the bump
 * allocator -- the producer allocates nothing on the heap. The whole-frame
 * WebP arm (#290) is dispatched here but lives in the sibling
 * `ra8_tileatlas_produce_webp.c`, sharing the producer state and the
 * geometry / rows / prefix-pull seams through `ra8_tileatlas_internal.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Domain]
 * {World: NS}
 */

#include "ra8_tileatlas_produce.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_compress.h"
#include "ra8_jpeg_sw.h"
#include "ra8_log.h"
#include "ra8_tileatlas.h"
#include "ra8_tileatlas_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_ta_prod";

/**
 * @enum ra8_ta_prod_const_t
 * @brief Producer sizing and sniffing constants.
 */
typedef enum : uint32_t {
  k_ra8_ta_sniff_bytes     = 12U,    /**< Magic bytes pulled up front (WebP needs 12). */
  k_ra8_ta_png_sig_len     = 8U,     /**< PNG signature length.                        */
  k_ra8_ta_jpeg_soi_first  = 0xFFU,  /**< JPEG SOI first byte.                         */
  k_ra8_ta_jpeg_soi_second = 0xD8U,  /**< JPEG SOI second byte.                        */
  k_ra8_ta_webp_fourcc_ofs = 8U,     /**< Offset of the "WEBP" fourCC.                 */
  k_ra8_ta_png_ring        = 65536U, /**< PNG inflate ring carve (bytes).              */
  k_ra8_ta_png_inbuf       = 4096U,  /**< PNG input-buffer carve (bytes).              */
  k_ra8_ta_byte_mask       = 0xFFU,  /**< Low-byte mask.                               */
  k_ra8_ta_le_sh8          = 8U,     /**< Little-endian shift.                         */
  k_ra8_ta_le_sh16         = 16U,    /**< Little-endian shift.                         */
  k_ra8_ta_le_sh24         = 24U,    /**< Little-endian shift.                         */
} ra8_ta_prod_const_t;

/** @brief Module-static producer state (producer documented not thread-safe). */
static ra8_ta_prod_state_t s_prod;

/** @brief PNG signature for source sniffing (mirrors the PNG decoder unit). */
static const uint8_t s_prod_png_sig[k_ra8_ta_png_sig_len] =
  {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU}; /* MAGIC-OK: the 8 spec signature bytes */

/** @brief WebP RIFF container tag (source head bytes 0..3). */
static const uint8_t s_prod_webp_riff[k_ra8_tileatlas_magic_len] =
  {'R', 'I', 'F', 'F'}; /* MAGIC-OK: the RIFF container fourCC */

/** @brief WebP form-type fourCC (source head bytes 8..11). */
static const uint8_t s_prod_webp_webp[k_ra8_tileatlas_magic_len] =
  {'W', 'E', 'B', 'P'}; /* MAGIC-OK: the WEBP form-type fourCC */

/** @brief Implementation of `ra8_ta_priv_bump_take()` -- aligned linear carve. */
RA8_PRIV void* ra8_ta_priv_bump_take(ra8_ta_bump_t* bump, size_t len)
{
  if ((bump == nullptr) || (len == 0U)) {
    return nullptr;
  }
  const size_t mask    = (size_t)k_ra8_ta_bump_align - 1U;
  const size_t aligned = (bump->off + mask) & ~mask;
  if ((aligned > bump->cap) || (len > (bump->cap - aligned))) {
    return nullptr;
  }
  bump->off = aligned + len;
  return &bump->base[aligned];
}

/* ---------------------------------------------------------------------------
 * Little-endian field writers + sink accounting.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Store a uint16 little-endian at @p buf.
 * @details Serializes @p v as two little-endian bytes.
 * @param[out] buf Destination (2 writable bytes).
 * @param[in]  v   Value to store.
 * @pre @p buf holds 2 writable bytes.
 * @pre None (total over uint16_t).
 * @post @p buf[0..1] holds @p v little-endian.
 * @post No other state mutated.
 * @note Pure over its output; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_wr_u16(uint8_t* buf, uint16_t v)
{
  buf[0] = (uint8_t)(v & (uint16_t)k_ra8_ta_byte_mask);
  buf[1] = (uint8_t)((uint32_t)v >> k_ra8_ta_le_sh8);
}

/**
 * @brief Store a uint32 little-endian at @p buf.
 * @details Serializes @p v as four little-endian bytes.
 * @param[out] buf Destination (4 writable bytes).
 * @param[in]  v   Value to store.
 * @pre @p buf holds 4 writable bytes.
 * @pre None (total over uint32_t).
 * @post @p buf[0..3] holds @p v little-endian.
 * @post No other state mutated.
 * @note Pure over its output; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_wr_u32(uint8_t* buf, uint32_t v)
{
  buf[0] = (uint8_t)(v & (uint32_t)k_ra8_ta_byte_mask);
  buf[1] = (uint8_t)((v >> k_ra8_ta_le_sh8) & (uint32_t)k_ra8_ta_byte_mask);
  buf[2] = (uint8_t)((v >> k_ra8_ta_le_sh16) & (uint32_t)k_ra8_ta_byte_mask);
  buf[3] = (uint8_t)((v >> k_ra8_ta_le_sh24) & (uint32_t)k_ra8_ta_byte_mask);
}

/**
 * @brief Append bytes to the caller sink, tracking the atlas offset.
 * @details Guards the u32 atlas size cap before delegating to the caller sink.
 * @param[in,out] st  Producer state (`written` advances).
 * @param[in]     buf Bytes to append.
 * @param[in]     len Byte count.
 * @return Result code.
 * @retval k_ra8_ok               Bytes sunk and accounted.
 * @retval k_ra8_err_invalid_size The atlas would exceed the u32 format cap.
 * @retval other                  Propagated from the caller sink.
 * @pre @p buf holds @p len readable bytes.
 * @pre `st->cfg->sink` is bound.
 * @post On success `st->written` grew by @p len.
 * @post On error the transcode aborts (partial atlas discarded).
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_sink(ra8_ta_prod_state_t* st, const uint8_t* buf, size_t len)
{
  if ((uint64_t)len > ((uint64_t)UINT32_MAX - (uint64_t)st->written)) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t err = st->cfg->sink(st->cfg->sink_ctx, buf, len);
  if (err != k_ra8_ok) {
    return err;
  }
  st->written += (uint32_t)len;
  return k_ra8_ok;
}

/* ---------------------------------------------------------------------------
 * Prefix-replay pull adapter.
 * ---------------------------------------------------------------------------
 */

RA8_PRIV ra8_err_t ra8_ta_priv_prefix_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  ra8_ta_prefix_pull_t* pfx = (ra8_ta_prefix_pull_t*)ctx;
  if (pfx->pos < pfx->head_len) {
    const size_t left = pfx->head_len - pfx->pos;
    const size_t take = (cap < left) ? cap : left;
    (void)memcpy(buf, &pfx->head[pfx->pos], take);
    pfx->pos += take;
    *got = take;
    return k_ra8_ok;
  }
  return pfx->inner(pfx->inner_ctx, buf, cap, got);
}

/* ---------------------------------------------------------------------------
 * Geometry + band accumulation + tile flush.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Serialize + sink the 32-byte JOF header from the bound geometry.
 * @details Serializes every geometry field per the JOF layout table in ra8_tileatlas.h.
 * @param[in,out] st Producer state (`written` advances via the sink).
 * @return Result code.
 * @retval k_ra8_ok The header is the first 32 atlas bytes.
 * @retval other    Propagated from the sink.
 * @pre The geometry fields of @p st are bound and validated.
 * @pre No atlas byte has been sunk yet (`written == 0`).
 * @post On success the sink holds exactly the header.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_emit_header(ra8_ta_prod_state_t* st)
{
  uint8_t hdr[k_ra8_tileatlas_hdr_bytes] = {};
  hdr[0]                                 = 'J';
  hdr[1]                                 = 'O';
  hdr[2]                                 = 'F';
  hdr[3]                                 = '1';
  priv_wr_u16(&hdr[k_ra8_tileatlas_ofs_width], st->w);
  priv_wr_u16(&hdr[k_ra8_tileatlas_ofs_height], st->h);
  priv_wr_u16(&hdr[k_ra8_tileatlas_ofs_tile_w], st->cfg->tile_w);
  priv_wr_u16(&hdr[k_ra8_tileatlas_ofs_tile_h], st->cfg->tile_h);
  hdr[k_ra8_tileatlas_ofs_bpp]   = st->bpp;
  hdr[k_ra8_tileatlas_ofs_codec] = st->cfg->codec;
  priv_wr_u32(&hdr[k_ra8_tileatlas_ofs_tile_count], st->tile_count);
  return priv_sink(st, hdr, sizeof(hdr));
}

/**
 * @brief Bind the source geometry: validate caps, carve buffers, emit header.
 * @details Fires once per transcode (from either decoder). Rejects, fail
 *          closed: dimensions above the caps, a tile grid above the format
 *          cap, and any carve the arena cannot fit. On success the 32-byte
 *          JOF header has been sunk.
 * @param[in] ctx      The producer state.
 * @param[in] width    Source width, pixels.
 * @param[in] height   Source height, pixels.
 * @param[in] channels Output bytes per pixel (1, 3 or 4).
 * @return Result code.
 * @retval k_ra8_ok               Geometry bound; header written.
 * @retval k_ra8_err_invalid_size Over the caps / grid cap / arena exhausted.
 * @retval k_ra8_err_invalid_state The geometry hook fired twice.
 * @retval other                  Propagated from the sink.
 * @pre The decoder validated @p width/@p height non-zero.
 * @pre `st->cfg` and `st->bump` are bound.
 * @post On success `band`/`stage`/`cmp`/`idx` are carved and header sunk.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
/**
 * @brief Carve the band, stage, index and compressed-tile buffers.
 * @details Sizes are computed in 64-bit and checked against the u32 format
 *          cap before any carve; arena exhaustion fails closed.
 * @param[in,out] st Producer state (geometry fields already bound).
 * @return Result code.
 * @retval k_ra8_ok               Every pixel-path buffer is carved.
 * @retval k_ra8_err_invalid_size Overflow or arena exhaustion.
 * @pre `st->w`/`st->h`/`st->bpp` and the grid fields are bound.
 * @pre `st->bump` has the geometry carve set available.
 * @post On success `band`/`stage`/`idx` (and `cmp` for deflate) are bound.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_carve_pixel_path(ra8_ta_prod_state_t* st)
{
  const uint16_t tw          = st->cfg->tile_w;
  const uint16_t th          = st->cfg->tile_h;
  const uint64_t band_bytes  = (uint64_t)st->w * (uint64_t)th * (uint64_t)st->bpp;
  const uint32_t tw_eff      = (tw < st->w) ? tw : st->w;
  const uint32_t th_eff      = (th < st->h) ? th : st->h;
  const uint64_t stage_bytes = (uint64_t)tw_eff * (uint64_t)th_eff * (uint64_t)st->bpp;
  // mcdc-deactivated: stage_bytes = tw_eff*th_eff*bpp with tw_eff <= w and th_eff <= tile_h, so stage_bytes <= w*tile_h*bpp = band_bytes always; the stage-overflow arm cannot flip independently of the band arm (which tests/test_ra8_tileatlas_produce_guards.c drives true via a 32768-wide RGBA source and a 65535-tall tile).
  if ((band_bytes > (uint64_t)UINT32_MAX) || (stage_bytes > (uint64_t)UINT32_MAX)) {
    return k_ra8_err_invalid_size;
  }
  st->band  = ra8_ta_priv_bump_take(st->bump, (size_t)band_bytes);
  st->stage = ra8_ta_priv_bump_take(st->bump, (size_t)stage_bytes);
  st->idx =
    ra8_ta_priv_bump_take(st->bump, (size_t)st->tile_count * (size_t)k_ra8_tileatlas_index_entry);
  if ((st->band == nullptr) || (st->stage == nullptr) || (st->idx == nullptr)) {
    return k_ra8_err_invalid_size;
  }
  if (st->cfg->codec == (uint8_t)k_ra8_tileatlas_codec_deflate) {
    st->cmp_cap = ra8_tileatlas_stored_bound((uint32_t)stage_bytes);
    st->cmp     = ra8_ta_priv_bump_take(st->bump, (size_t)st->cmp_cap);
    if (st->cmp == nullptr) {
      return k_ra8_err_invalid_size;
    }
  }
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t ra8_ta_priv_on_geom(void* ctx, uint16_t width, uint16_t height, uint8_t channels)
{
  ra8_ta_prod_state_t* st = (ra8_ta_prod_state_t*)ctx;
  if (st->geom_done != 0U) {
    return k_ra8_err_invalid_state;
  }
  if ((width == 0U) || (width > st->cap_w) || (height == 0U) || (height > st->cap_h)) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t cols = ((uint32_t)width + st->cfg->tile_w - 1U) / st->cfg->tile_w;
  const uint32_t rows = ((uint32_t)height + st->cfg->tile_h - 1U) / st->cfg->tile_h;
  if ((cols * rows) > (uint32_t)k_ra8_tileatlas_max_tiles) {
    return k_ra8_err_invalid_size;
  }
  st->w          = width;
  st->h          = height;
  st->bpp        = channels;
  st->tile_cols  = (uint16_t)cols;
  st->tile_rows  = (uint16_t)rows;
  st->tile_count = cols * rows;
  ra8_err_t err  = priv_carve_pixel_path(st);
  if (err != k_ra8_ok) {
    return err;
  }
  err = priv_emit_header(st);
  if (err != k_ra8_ok) {
    return err;
  }
  st->geom_done = 1U;
  return k_ra8_ok;
}

/**
 * @brief Encode one packed tile payload and append it to the atlas.
 * @details Raw codec sinks the payload verbatim; the deflate codec runs it
 *          through the zero-heap `ra8_io_compress()` into the bounded
 *          compressed-tile buffer. The tile's index entry (absolute offset
 *          plus stored length) is recorded before the bytes are sunk.
 * @param[in,out] st      Producer state.
 * @param[in]     payload Packed tile byte count (`tw * th * bpp`).
 * @return Result code.
 * @retval k_ra8_ok                    Tile encoded, recorded and sunk.
 * @retval k_ra8_err_validation_failed The compressor overran its bound
 *                                     (cannot happen for real inputs).
 * @retval other                       Propagated from the sink.
 * @pre `st->stage` holds @p payload packed bytes.
 * @pre `st->tiles_done < st->tile_count`.
 * @post On success `tiles_done` advanced and the index entry is recorded.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_encode_tile(ra8_ta_prod_state_t* st, uint32_t payload)
{
  const uint8_t* out_bytes = st->stage;
  uint32_t       out_len   = payload;
  if (st->cfg->codec == (uint8_t)k_ra8_tileatlas_codec_deflate) {
    uint32_t clen = 0U;
    if (ra8_io_compress(st->stage, payload, st->cmp, st->cmp_cap, st->dfl, st->dfl_len, &clen) !=
        k_ra8_ok) {
      return k_ra8_err_validation_failed;
    }
    out_bytes = st->cmp;
    out_len   = clen;
  }
  uint8_t* entry = &st->idx[(size_t)st->tiles_done * (size_t)k_ra8_tileatlas_index_entry];
  priv_wr_u32(&entry[k_ra8_tileatlas_idx_ofs_offset], st->written);
  priv_wr_u32(&entry[k_ra8_tileatlas_idx_ofs_length], out_len);
  const ra8_err_t err = priv_sink(st, out_bytes, (size_t)out_len);
  if (err != k_ra8_ok) {
    return err;
  }
  st->tiles_done++;
  return k_ra8_ok;
}

/**
 * @brief Cut the accumulated band into tiles and append each to the atlas.
 * @details Iterates the tile columns left to right (row-major tile order),
 *          packing each tile's rows tightly into the stage buffer before
 *          encoding. Edge columns carry their clamped width.
 * @param[in,out] st Producer state (band consumed; tiles emitted).
 * @param[in]     th Rows in this band (tile_h, or less for the last band).
 * @return Result code.
 * @retval k_ra8_ok Every tile of the band was encoded and sunk.
 * @retval other    Propagated from the encode/sink stage.
 * @pre `st->band` holds `th` complete rows.
 * @pre `th >= 1`.
 * @post On success `tiles_done` advanced by `tile_cols`.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_flush_band(ra8_ta_prod_state_t* st, uint32_t th)
{
  const uint32_t stride = (uint32_t)st->w * (uint32_t)st->bpp;
  for (uint32_t tx = 0U; tx < (uint32_t)st->tile_cols; tx++) {
    const uint32_t x0        = tx * (uint32_t)st->cfg->tile_w;
    const uint32_t tw        = (((uint32_t)st->w - x0) < (uint32_t)st->cfg->tile_w)
                                 ? ((uint32_t)st->w - x0)
                                 : (uint32_t)st->cfg->tile_w;
    const uint32_t row_bytes = tw * (uint32_t)st->bpp;
    for (uint32_t r = 0U; r < th; r++) {
      (void)memcpy(&st->stage[(size_t)r * (size_t)row_bytes],
                   &st->band[((size_t)r * (size_t)stride) + ((size_t)x0 * (size_t)st->bpp)],
                   (size_t)row_bytes);
    }
    const ra8_err_t err = priv_encode_tile(st, row_bytes * th);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t ra8_ta_priv_on_rows(void*          ctx,
                                       const uint8_t* px,
                                       uint16_t       width,
                                       uint16_t       y0,
                                       uint16_t       nrows,
                                       uint8_t        channels)
{
  ra8_ta_prod_state_t* st = (ra8_ta_prod_state_t*)ctx;
  RA8_CHECK_NULL_PTR(px, s_tag, "px must not be nullptr");
  // mcdc-deactivated: row-sink contract guard; both in-tree decoders fire the geometry hook before any row (a failed hook aborts the decode) and pass their bound width/channels verbatim into every on_rows call, so no public-API source can flip geom_done/width/channels here independently.
  if ((st->geom_done == 0U) || (width != st->w) || (channels != st->bpp)) {
    return k_ra8_err_validation_failed;
  }
  // mcdc-deactivated: row-ordering contract guard; the PNG scanline assembler emits exactly one row per call at y0 == rows_done and the JPEG stripe walker emits edge-clamped nrows >= 1 at strictly increasing MCU-row origins, so zero/duplicated/overshooting deliveries are not constructible from a public-API source.
  if ((nrows == 0U) || ((uint32_t)y0 != st->rows_seen) ||
      (((uint32_t)y0 + (uint32_t)nrows) > (uint32_t)st->h)) {
    return k_ra8_err_validation_failed;
  }
  const uint32_t stride = (uint32_t)st->w * (uint32_t)st->bpp;
  const uint8_t* src    = px;
  uint32_t       left   = nrows;
  while (left > 0U) {
    const uint32_t room = (uint32_t)st->cfg->tile_h - st->band_fill;
    const uint32_t take = (left < room) ? left : room;
    (void)memcpy(&st->band[(size_t)st->band_fill * (size_t)stride],
                 src,
                 (size_t)take * (size_t)stride);
    st->band_fill += take;
    st->rows_seen += take;
    src += (size_t)take * (size_t)stride;
    left -= take;
    if (st->band_fill == (uint32_t)st->cfg->tile_h) {
      const ra8_err_t err = priv_flush_band(st, st->band_fill);
      if (err != k_ra8_ok) {
        return err;
      }
      st->band_fill = 0U;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief JPEG geometry adapter: bind geometry, then carve the MCU stripe.
 * @details Wraps ::ra8_ta_priv_on_geom for `ra8_jpeg_sw_decode_stripes()`, which
 *          additionally needs a caller-owned stripe buffer sized
 *          `width * stripe_rows * channels`.
 * @param[in]  ctx            The producer state.
 * @param[in]  width          Source width, pixels.
 * @param[in]  height         Source height, pixels.
 * @param[in]  channels       Output channels (1 or 3).
 * @param[in]  stripe_rows    Rows per stripe (8 or 16).
 * @param[out] out_stripe     Receives the carved stripe buffer.
 * @param[out] out_stripe_cap Receives its capacity.
 * @return Result code.
 * @retval k_ra8_ok               Geometry bound; stripe carved.
 * @retval k_ra8_err_invalid_size Caps exceeded or arena exhausted.
 * @retval other                  Propagated from ::ra8_ta_priv_on_geom.
 * @pre @p out_stripe / @p out_stripe_cap are writable.
 * @pre `st->bump` has the JPEG carve set available.
 * @post On success the stripe buffer is bound for the scan.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_jpeg_geom(void*     ctx,
                                uint16_t  width,
                                uint16_t  height,
                                uint8_t   channels,
                                uint16_t  stripe_rows,
                                uint8_t** out_stripe,
                                uint32_t* out_stripe_cap)
{
  ra8_ta_prod_state_t* st  = (ra8_ta_prod_state_t*)ctx;
  const ra8_err_t      err = ra8_ta_priv_on_geom(ctx, width, height, channels);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t cap = (uint32_t)width * (uint32_t)stripe_rows * (uint32_t)channels;
  uint8_t*       buf = ra8_ta_priv_bump_take(st->bump, (size_t)cap);
  if (buf == nullptr) {
    return k_ra8_err_invalid_size;
  }
  *out_stripe     = buf;
  *out_stripe_cap = cap;
  return k_ra8_ok;
}

/* ---------------------------------------------------------------------------
 * Trailer emission + entry point.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Emit the tile index and footer, then fill the caller's info.
 * @details Emits the trailing tile index and 16-byte footer, then mirrors the parsed-info fields.
 * @param[in,out] st       Producer state.
 * @param[out]    out_info Receives the finished atlas geometry.
 * @return Result code.
 * @retval k_ra8_ok                    Atlas complete and accounted.
 * @retval k_ra8_err_validation_failed Not every tile was flushed.
 * @retval other                       Propagated from the sink.
 * @pre All rows arrived and the last band was flushed.
 * @pre `st->idx` holds `tile_count` serialized entries.
 * @post On success the sink holds a complete JOF atlas.
 * @post On error the partial atlas must be discarded.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_finish(ra8_ta_prod_state_t* st, ra8_tileatlas_info_t* out_info)
{
  if (st->tiles_done != st->tile_count) {
    return k_ra8_err_validation_failed;
  }
  const uint32_t index_off = st->written;
  ra8_err_t      err =
    priv_sink(st, st->idx, (size_t)st->tile_count * (size_t)k_ra8_tileatlas_index_entry);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t ftr[k_ra8_tileatlas_footer_bytes] = {};
  priv_wr_u32(&ftr[k_ra8_tileatlas_ftr_index_off], index_off);
  priv_wr_u32(&ftr[k_ra8_tileatlas_ftr_tile_count], st->tile_count);
  priv_wr_u32(&ftr[k_ra8_tileatlas_ftr_total_size],
              st->written + (uint32_t)k_ra8_tileatlas_footer_bytes);
  ftr[k_ra8_tileatlas_ftr_magic]      = 'J';
  ftr[k_ra8_tileatlas_ftr_magic + 1U] = 'O';
  ftr[k_ra8_tileatlas_ftr_magic + 2U] = 'F';
  ftr[k_ra8_tileatlas_ftr_magic + 3U] = 'E';
  err                                 = priv_sink(st, ftr, sizeof(ftr));
  if (err != k_ra8_ok) {
    return err;
  }
  out_info->width      = st->w;
  out_info->height     = st->h;
  out_info->tile_w     = st->cfg->tile_w;
  out_info->tile_h     = st->cfg->tile_h;
  out_info->tile_cols  = st->tile_cols;
  out_info->tile_rows  = st->tile_rows;
  out_info->bpp        = st->bpp;
  out_info->codec      = st->cfg->codec;
  out_info->tile_count = st->tile_count;
  out_info->index_off  = index_off;
  out_info->total_size = st->written;
  return k_ra8_ok;
}

/**
 * @brief Validate the caller configuration's non-pointer invariants.
 * @details Single-condition guards over the tile geometry and codec selector.
 * @param[in] cfg Producer configuration.
 * @return Result code.
 * @retval k_ra8_ok              Geometry and codec are usable.
 * @retval k_ra8_err_invalid_arg Zero tile geometry or unknown codec.
 * @pre @p cfg is non-NULL (caller-validated).
 * @pre Pointer members were validated by the caller.
 * @post No state mutated.
 * @post Return depends solely on @p cfg.
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_check_cfg(const ra8_tileatlas_produce_cfg_t* cfg)
{
  if ((cfg->tile_w == 0U) || (cfg->tile_h == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->codec > (uint8_t)k_ra8_tileatlas_codec_deflate) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

uint32_t
ra8_tileatlas_work_bytes(uint16_t max_width, uint16_t max_height, uint16_t tile_w, uint16_t tile_h)
{
  if ((max_width == 0U) || ((uint32_t)max_width > (uint32_t)k_ra8_tileatlas_max_dim) ||
      (max_height == 0U) || ((uint32_t)max_height > (uint32_t)k_ra8_tileatlas_max_dim)) {
    return 0U;
  }
  if ((tile_w == 0U) || (tile_h == 0U)) {
    return 0U;
  }
  const uint64_t cols = ((uint64_t)max_width + tile_w - 1U) / tile_w;
  const uint64_t rows = ((uint64_t)max_height + tile_h - 1U) / tile_h;
  if ((cols * rows) > (uint64_t)k_ra8_tileatlas_max_tiles) {
    return 0U;
  }
  const uint64_t bpp = (uint64_t)k_ra8_tileatlas_bpp_max;
  const uint64_t jpeg_set =
    (uint64_t)k_ra8_jpeg_sw_stream_min_window +
    ((uint64_t)max_width * (uint64_t)k_ra8_jpeg_sw_stream_mcu_rows_max * 3U);
  const uint64_t png_set = (uint64_t)sizeof(tinfl_decompressor) + (uint64_t)k_ra8_ta_png_ring +
                           (uint64_t)k_ra8_ta_png_inbuf + (3U * ((uint64_t)max_width * bpp)) + 2U;
  const uint64_t dec_set = (jpeg_set > png_set) ? jpeg_set : png_set;
  const uint64_t band    = (uint64_t)max_width * (uint64_t)tile_h * bpp;
  const uint64_t tw_eff  = ((uint64_t)tile_w < (uint64_t)max_width) ? tile_w : max_width;
  const uint64_t th_eff  = ((uint64_t)tile_h < (uint64_t)max_height) ? tile_h : max_height;
  const uint64_t stage   = tw_eff * th_eff * bpp;
  const uint64_t cmp     = (uint64_t)ra8_tileatlas_stored_bound((uint32_t)stage);
  const uint64_t index_bytes = cols * rows * (uint64_t)k_ra8_tileatlas_index_entry;
  const uint64_t dfl         = (uint64_t)k_ra8_io_compress_scratch_bytes;
  const uint64_t total =
    dec_set + band + stage + cmp + index_bytes + dfl + (uint64_t)k_ra8_ta_carve_slack;
  if ((stage > (uint64_t)UINT32_MAX) || (total > (uint64_t)UINT32_MAX)) {
    return 0U;
  }
  return (uint32_t)total;
}

/**
 * @brief Reset the transcode state and carve the deflate scratch.
 * @details Clamps the caller caps to the format maximum, binds the owned
 *          bump allocator over the caller's work arena, and carves the codec
 *          scratch.
 * @param[out] st  Producer state to (re)initialise.
 * @param[in]  cfg Caller configuration (validated).
 * @return Result code.
 * @retval k_ra8_ok               State bound; scratch carved when needed.
 * @retval k_ra8_err_invalid_size The arena cannot fit the deflate scratch.
 * @pre @p cfg passed the pointer + geometry guards.
 * @pre @p cfg->work covers `work_cap` bytes.
 * @post On success the effective caps and codec scratch are bound.
 * @post On error the transcode aborts before any sink write.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_init_state(ra8_ta_prod_state_t* st, const ra8_tileatlas_produce_cfg_t* cfg)
{
  (void)memset(st, 0, sizeof(*st));
  st->cfg        = cfg;
  st->bump_store = (ra8_ta_bump_t){.base = cfg->work, .cap = cfg->work_cap, .off = 0U};
  st->bump       = &st->bump_store;
  st->cap_w =
    ((cfg->max_width != 0U) && ((uint32_t)cfg->max_width < (uint32_t)k_ra8_tileatlas_max_dim))
      ? cfg->max_width
      : (uint16_t)k_ra8_tileatlas_max_dim;
  st->cap_h =
    ((cfg->max_height != 0U) && ((uint32_t)cfg->max_height < (uint32_t)k_ra8_tileatlas_max_dim))
      ? cfg->max_height
      : (uint16_t)k_ra8_tileatlas_max_dim;
  if (cfg->codec == (uint8_t)k_ra8_tileatlas_codec_deflate) {
    st->dfl_len = (uint32_t)k_ra8_io_compress_scratch_bytes;
    st->dfl     = ra8_ta_priv_bump_take(st->bump, (size_t)st->dfl_len);
    if (st->dfl == nullptr) {
      return k_ra8_err_invalid_size;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Pull the sniff head (both formats need at least these bytes).
 * @details Loops the pull seam until the fixed sniff length arrives (bounded by that length).
 * @param[in]  cfg  Caller configuration.
 * @param[out] head Receives ::k_ra8_ta_sniff_bytes source bytes.
 * @return Result code.
 * @retval k_ra8_ok                 Head filled.
 * @retval k_ra8_err_protocol_error The source is too short to be an image.
 * @retval other                    Propagated from the pull callback.
 * @pre @p head holds ::k_ra8_ta_sniff_bytes writable bytes.
 * @pre The source is positioned at byte 0.
 * @post On success the head bytes are consumed from the source.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_sniff_head(const ra8_tileatlas_produce_cfg_t* cfg, uint8_t* head)
{
  size_t head_len = 0U;
  while (head_len < (size_t)k_ra8_ta_sniff_bytes) {
    size_t          got = 0U;
    const ra8_err_t err =
      cfg->pull(cfg->pull_ctx, &head[head_len], (size_t)k_ra8_ta_sniff_bytes - head_len, &got);
    if (err != k_ra8_ok) {
      return err;
    }
    if (got == 0U) {
      return k_ra8_err_protocol_error; /* too short to be a JPEG/PNG */
    }
    head_len += got;
  }
  return k_ra8_ok;
}

/**
 * @brief Dispatch the sniffed source to the matching decoder.
 * @details The compound sniff decision carries MC/DC vectors in
 *          `test_ra8_tileatlas_produce.c` (JPEG head), the PNG head vector, and
 *          `test_ra8_tileatlas_produce_webp.c` (WebP RIFF+WEBP head, plus the
 *          RIFF-without-WEBP fail-closed vector).
 * @param[in,out] st   Producer state.
 * @param[in]     head The sniffed source head.
 * @param[in,out] pfx  Prefix-replay pull adapter over the source.
 * @return Result code.
 * @retval k_ra8_ok                Whole source decoded and accumulated.
 * @retval k_ra8_err_not_supported The head is not JPEG/PNG/WebP.
 * @retval k_ra8_err_invalid_size  The arena cannot fit the decoder set.
 * @retval other                   Propagated from the decoders.
 * @pre `priv_init_state()` succeeded.
 * @pre @p pfx replays @p head before the live source.
 * @post On success every source row reached the band accumulator.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_dispatch(ra8_ta_prod_state_t* st, const uint8_t* head, ra8_ta_prefix_pull_t* pfx)
{
  if ((head[0] == (uint8_t)k_ra8_ta_jpeg_soi_first) &&
      (head[1] == (uint8_t)k_ra8_ta_jpeg_soi_second)) {
    uint8_t* window = ra8_ta_priv_bump_take(st->bump, (size_t)k_ra8_jpeg_sw_stream_min_window);
    if (window == nullptr) {
      return k_ra8_err_invalid_size;
    }
    return ra8_jpeg_sw_decode_stripes(ra8_ta_priv_prefix_pull,
                                      pfx,
                                      window,
                                      (uint32_t)k_ra8_jpeg_sw_stream_min_window,
                                      priv_jpeg_geom,
                                      ra8_ta_priv_on_rows,
                                      st);
  }
  if (memcmp(head, s_prod_png_sig, sizeof(s_prod_png_sig)) == 0) {
    return ra8_ta_priv_png_rows(ra8_ta_priv_prefix_pull,
                                pfx,
                                st->bump,
                                st->cap_w,
                                st->cap_h,
                                ra8_ta_priv_on_geom,
                                ra8_ta_priv_on_rows,
                                st);
  }
  if ((memcmp(head, s_prod_webp_riff, sizeof(s_prod_webp_riff)) == 0) &&
      (memcmp(&head[k_ra8_ta_webp_fourcc_ofs], s_prod_webp_webp, sizeof(s_prod_webp_webp)) == 0)) {
    return ra8_ta_priv_webp_transcode(st, pfx);
  }
  return k_ra8_err_not_supported; /* not a JPEG/PNG/WebP source */
}

/**
 * @brief Post-decode checks, final band flush and trailer emission.
 * @details The decoder must have bound the geometry and delivered every
 *          row; a short delivery is a hostile-source abort.
 * @param[in,out] st       Producer state.
 * @param[out]    out_info Receives the finished atlas geometry.
 * @return Result code.
 * @retval k_ra8_ok                    Atlas fully written to the sink.
 * @retval k_ra8_err_validation_failed The decoder under-delivered.
 * @retval other                       Propagated from the flush / trailer.
 * @pre `priv_dispatch()` returned success.
 * @pre @p out_info is writable.
 * @post On success the sink holds one complete, parseable atlas.
 * @post On error the partial atlas must be discarded.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_epilogue(ra8_ta_prod_state_t* st, ra8_tileatlas_info_t* out_info)
{
  // mcdc-deactivated: post-decode contract guard; both in-tree decoders return success only after the geometry hook fired and every declared row was delivered (short/hostile streams abort inside the decoder), so neither condition can be flipped through the public producer entry.
  if ((st->geom_done == 0U) || (st->rows_seen != (uint32_t)st->h)) {
    return k_ra8_err_validation_failed; /* decoder under-delivered */
  }
  if (st->band_fill > 0U) {
    const ra8_err_t err = priv_flush_band(st, st->band_fill);
    if (err != k_ra8_ok) {
      return err;
    }
    st->band_fill = 0U;
  }
  return priv_finish(st, out_info);
}

/**
 * @brief Reject any NULL `ra8_tileatlas_produce` argument or seam.
 * @details Split out so the public entry stays under the statement budget.
 * @param[in] cfg      Producer configuration to validate.
 * @param[in] out_info Output pointer to validate.
 * @return Result code.
 * @retval k_ra8_ok           Every required pointer is non-NULL.
 * @retval k_ra8_err_null_ptr Some pointer is NULL.
 * @pre Only @p cfg is dereferenced (after its own check).
 * @pre The caller forwards its own arguments.
 * @post No state mutated.
 * @post Return depends solely on the inputs.
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_produce_args_ok(const ra8_tileatlas_produce_cfg_t* cfg,
                                      const ra8_tileatlas_info_t*        out_info)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA8_CHECK_NULL_PTR(out_info, s_tag, "out_info must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->pull, s_tag, "cfg->pull must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->sink, s_tag, "cfg->sink must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->work, s_tag, "cfg->work must not be nullptr");
  return k_ra8_ok;
}

ra8_err_t ra8_tileatlas_produce(const ra8_tileatlas_produce_cfg_t* cfg,
                                ra8_tileatlas_info_t*              out_info)
{
  ra8_err_t err = priv_produce_args_ok(cfg, out_info);
  if (err != k_ra8_ok) {
    return err;
  }
  err = priv_check_cfg(cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_ta_prod_state_t* st = &s_prod;
  err                     = priv_init_state(st, cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t head[k_ra8_ta_sniff_bytes] = {};
  err                                = priv_sniff_head(cfg, head);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_ta_prefix_pull_t pfx = {
    .head      = head,
    .head_len  = sizeof(head),
    .pos       = 0U,
    .inner     = cfg->pull,
    .inner_ctx = cfg->pull_ctx,
  };
  err = priv_dispatch(st, head, &pfx);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_epilogue(st, out_info);
}
