/**
 * @file ra8_tile_cache.c
 * @brief Fixed-budget image-tile cache -- implementation (Layer 3b, #147).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * A thin typed facade over ::ra8_keycache, the image counterpart of
 * ::ra8_glyph_atlas: the tile key is the cache key, the decoded tile is the cell
 * payload, and the decoded width/height ride in the per-cell ::ra8_tile_dims_t
 * user descriptor. A small decode trampoline adapts the public
 * ::ra8_tile_decode_fn (out_w/out_h) to the keycache's render-on-miss seam (user
 * descriptor) so the production stb_image-backed decoder and the test stubs are
 * unchanged. All cache mechanics -- the single LRU list, pinned-cell skip, hash
 * chaining, eviction -- live in ::ra8_keycache.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_tile_cache.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_keycache.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_tile_cache";

/**
 * @brief Decode trampoline: adapt ::ra8_tile_decode_fn to the keycache seam.
 *
 * @details Casts the keycache render context back to the owning cache, calls the
 *          caller's tile decoder for the cell, and on success records the decoded
 *          width/height into the cell's ::ra8_tile_dims_t descriptor.
 *
 * @param[in]  ctx        The owning ::ra8_tile_cache_t (keycache `render_ctx`).
 * @param[in]  key        The ::ra8_tile_key_t to decode.
 * @param[out] cell       Destination pixel buffer (`cell_bytes` writable).
 * @param[in]  cell_bytes Cell capacity in bytes.
 * @param[out] user       The cell's ::ra8_tile_dims_t descriptor.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    The tile was decoded and the descriptor recorded.
 * @retval k_ra8_err_* The caller's decoder error (descriptor left untouched).
 *
 * @pre `ctx` and `user` are non-NULL (the keycache guarantees both here).
 * @pre `key` points at a valid ::ra8_tile_key_t.
 * @post On success `*user` holds the decoded tile dimensions.
 * @post On failure the descriptor is not written.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_tile_decode(void* ctx, const void* key, uint8_t* cell, uint32_t cell_bytes, void* user)
{
  const ra8_tile_cache_t* tc   = (const ra8_tile_cache_t*)ctx;
  const ra8_tile_key_t*   tk   = (const ra8_tile_key_t*)key;
  ra8_tile_dims_t*        dims = (ra8_tile_dims_t*)user;
  uint16_t                w    = 0;
  uint16_t                h    = 0;
  const ra8_err_t         rerr = tc->decode(tc->decode_ctx, tk, cell, cell_bytes, &w, &h);
  if (rerr != k_ra8_ok) {
    return rerr;
  }
  dims->w = w;
  dims->h = h;
  return k_ra8_ok;
}

ra8_err_t ra8_tile_cache_init(ra8_tile_cache_t* tc, const ra8_tile_cache_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(tc, s_tag, "tc must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->decode, s_tag, "decode must not be nullptr");
  (void)memset(tc, 0, sizeof(*tc));
  tc->decode              = cfg->decode;
  tc->decode_ctx          = cfg->decode_ctx;
  ra8_keycache_cfg_t kcfg = {};
  kcfg.cell_mem           = cfg->cell_mem;
  kcfg.cell_bytes         = cfg->cell_bytes;
  kcfg.cell_count         = cfg->cell_count;
  kcfg.key_mem            = (uint8_t*)cfg->keys;
  kcfg.key_bytes          = (uint32_t)sizeof(ra8_tile_key_t);
  kcfg.user_mem           = (uint8_t*)cfg->dims;
  kcfg.user_bytes         = (uint32_t)sizeof(ra8_tile_dims_t);
  kcfg.meta               = cfg->meta;
  kcfg.buckets            = cfg->buckets;
  kcfg.bucket_count       = cfg->bucket_count;
  kcfg.render             = priv_tile_decode;
  kcfg.render_ctx         = tc;
  return ra8_keycache_init(&tc->kc, &kcfg);
}

ra8_err_t ra8_tile_cache_get(ra8_tile_cache_t* tc, const ra8_tile_key_t* key, ra8_tile_t* out_tile)
{
  RA8_CHECK_NULL_PTR(tc, s_tag, "tc must not be nullptr");
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA8_CHECK_NULL_PTR(out_tile, s_tag, "out_tile must not be nullptr");
  ra8_keycache_view_t v   = {};
  const ra8_err_t     err = ra8_keycache_get(&tc->kc, key, &v);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_tile_dims_t* dims = (const ra8_tile_dims_t*)v.user;
  *out_tile                   = (ra8_tile_t){.pixels = v.data, .width = dims->w, .height = dims->h};
  return k_ra8_ok;
}

ra8_err_t ra8_tile_cache_put(ra8_tile_cache_t* tc, const uint8_t* pixels)
{
  RA8_CHECK_NULL_PTR(tc, s_tag, "tc must not be nullptr");
  RA8_CHECK_NULL_PTR(pixels, s_tag, "pixels must not be nullptr");
  return ra8_keycache_put(&tc->kc, pixels);
}

uint32_t ra8_tile_cache_capacity(const ra8_tile_cache_t* tc)
{
  if (tc == nullptr) {
    return 0U;
  }
  if (tc->kc.cfg.cell_mem == nullptr) {
    return 0U; /* never initialised: report no capacity */
  }
  return tc->kc.cfg.cell_count;
}

ra8_err_t ra8_tile_cache_prefetch(ra8_tile_cache_t* tc, const ra8_tile_key_t* key)
{
  RA8_CHECK_NULL_PTR(tc, s_tag, "tc must not be nullptr");
  RA8_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  return ra8_keycache_prefetch(&tc->kc, key);
}

/**
 * @struct priv_pan_line_t
 * @brief The lead-edge tile run a pan prefetch walks (one row or one column).
 *
 * @details `(x, y)` is the first tile; each successive tile steps by
 *          `(step_x, step_y)` (one of them 1, the other 0); `count` tiles lie on
 *          the run before the tile-grid clamp / budget cap are applied.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t x;      /**< First lead tile column.              */
  uint16_t y;      /**< First lead tile row.                 */
  uint16_t step_x; /**< Per-tile column delta (0 or 1).      */
  uint16_t step_y; /**< Per-tile row delta (0 or 1).         */
  uint16_t count;  /**< Tiles on the lead edge (pre-budget). */
} priv_pan_line_t;

/**
 * @brief Reject a structurally invalid prefetch request.
 *
 * @details Each guard is an independent single-condition check kept intact here:
 *          the visible rectangle must be ordered (`tx0<=tx1`, `ty0<=ty1`) and lie
 *          inside the tile grid (`tx1<tile_cols`, `ty1<tile_rows`).
 *
 * @param[in] req The prefetch request to validate (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The request is well-formed.
 * @retval k_ra8_err_invalid_arg A rectangle bound is unordered or off-grid.
 *
 * @pre `req` is non-NULL (the caller checked it).
 * @pre `req->view` is the caller's visible tile rectangle.
 * @post No state is modified (pure validation).
 * @post A non-ok return means a bound was unordered or off-grid.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_validate_req(const ra8_tile_prefetch_req_t* req)
{
  const ra8_tile_rect_t v = req->view;
  if (v.tx0 > v.tx1) {
    return k_ra8_err_invalid_arg;
  }
  if (v.ty0 > v.ty1) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint32_t)v.tx1 >= (uint32_t)req->tile_cols) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint32_t)v.ty1 >= (uint32_t)req->tile_rows) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Compute the lead-edge tile run for a pan direction, or none at an edge.
 *
 * @details Selects the row/column one step beyond @p req->view in @p req->dir and
 *          clamps it to the tile grid: a pan already against the image edge
 *          (or ::k_ra8_tile_pan_none) yields no run. Each direction's edge test
 *          is an independent single-condition guard.
 *
 * @param[in]  req The validated prefetch request.
 * @param[out] out Receives the lead-edge run when the return is true.
 *
 * @return true if a lead edge exists to warm, false at an image edge / no pan.
 * @retval true  `*out` holds the run to walk.
 * @retval false Nothing to warm (`*out` is untouched).
 *
 * @pre `req` passed ::priv_validate_req; `out` is writable.
 * @pre `req->view` lies inside the tile grid.
 * @post A true return leaves `*out` fully populated.
 * @post No cache or request state is modified.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_pan_line(const ra8_tile_prefetch_req_t* req, priv_pan_line_t* out)
{
  const ra8_tile_rect_t v     = req->view;
  const uint16_t        v_run = (uint16_t)((v.ty1 - v.ty0) + 1U);
  const uint16_t        h_run = (uint16_t)((v.tx1 - v.tx0) + 1U);
  switch (req->dir) {
    case k_ra8_tile_pan_right:
      if (((uint32_t)v.tx1 + 1U) >= (uint32_t)req->tile_cols) {
        return false;
      }
      *out = (priv_pan_line_t){.x      = (uint16_t)(v.tx1 + 1U),
                               .y      = v.ty0,
                               .step_x = 0U,
                               .step_y = 1U,
                               .count  = v_run};
      return true;
    case k_ra8_tile_pan_left:
      if (v.tx0 == 0U) {
        return false;
      }
      *out = (priv_pan_line_t){.x      = (uint16_t)(v.tx0 - 1U),
                               .y      = v.ty0,
                               .step_x = 0U,
                               .step_y = 1U,
                               .count  = v_run};
      return true;
    case k_ra8_tile_pan_down:
      if (((uint32_t)v.ty1 + 1U) >= (uint32_t)req->tile_rows) {
        return false;
      }
      *out = (priv_pan_line_t){.x      = v.tx0,
                               .y      = (uint16_t)(v.ty1 + 1U),
                               .step_x = 1U,
                               .step_y = 0U,
                               .count  = h_run};
      return true;
    case k_ra8_tile_pan_up:
      if (v.ty0 == 0U) {
        return false;
      }
      *out = (priv_pan_line_t){.x      = v.tx0,
                               .y      = (uint16_t)(v.ty0 - 1U),
                               .step_x = 1U,
                               .step_y = 0U,
                               .count  = h_run};
      return true;
    case k_ra8_tile_pan_none:
    default:
      return false;
  }
}

ra8_err_t ra8_tile_cache_prefetch_pan(ra8_tile_cache_t*              tc,
                                      const ra8_tile_prefetch_req_t* req,
                                      uint16_t*                      out_warmed)
{
  RA8_CHECK_NULL_PTR(tc, s_tag, "tc must not be nullptr");
  RA8_CHECK_NULL_PTR(req, s_tag, "req must not be nullptr");
  if (out_warmed != nullptr) {
    *out_warmed = 0U;
  }
  const ra8_err_t verr = priv_validate_req(req);
  if (verr != k_ra8_ok) {
    return verr;
  }
  priv_pan_line_t line = {};
  if (!priv_pan_line(req, &line)) {
    return k_ra8_ok; /* at an image edge or not panning: nothing to warm */
  }
  const uint16_t cap    = (uint16_t)((line.count < req->max_tiles) ? line.count : req->max_tiles);
  uint16_t       warmed = 0U;
  for (uint16_t i = 0U; i < cap; ++i) {
    const ra8_tile_key_t key = {.image_id = req->image_id,
                                .tile_x   = (uint16_t)(line.x + (uint16_t)(line.step_x * i)),
                                .tile_y   = (uint16_t)(line.y + (uint16_t)(line.step_y * i)),
                                .zoom     = req->zoom};
    if (ra8_tile_cache_prefetch(tc, &key) != k_ra8_ok) {
      break; /* best-effort: a full/failed cache stops the sweep, not the pan */
    }
    warmed++;
  }
  if (out_warmed != nullptr) {
    *out_warmed = warmed;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_tile_cache_stats(const ra8_tile_cache_t* tc,
                               uint32_t*               out_hits,
                               uint32_t*               out_misses,
                               uint32_t*               out_evictions)
{
  RA8_CHECK_NULL_PTR(tc, s_tag, "tc must not be nullptr");
  if (tc->kc.cfg.cell_mem == nullptr) {
    return k_ra8_err_invalid_state;
  }
  return ra8_keycache_stats(&tc->kc, out_hits, out_misses, out_evictions);
}
