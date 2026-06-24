/**
 * @file ra_glyph_atlas.c
 * @brief Fixed-budget glyph cache -- implementation (Layer 3, #147).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * A thin typed facade over ::ra_keycache: the glyph key is the cache key, the
 * glyph bitmap is the cell payload, and the rendered width/height ride in the
 * per-cell ::ra_glyph_dims_t user descriptor. A small render trampoline adapts
 * the public ::ra_glyph_render_fn (out_w/out_h) to the keycache's render-on-miss
 * seam (user descriptor) so the production rasteriser and the test stubs are
 * unchanged. All cache mechanics -- the single LRU list, pinned-cell skip, hash
 * chaining, eviction -- live in ::ra_keycache.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_glyph_atlas.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_keycache.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_glyph_atlas";

/**
 * @brief Render trampoline: adapt ::ra_glyph_render_fn to the keycache seam.
 *
 * @details Casts the keycache render context back to the owning atlas, calls the
 *          caller's glyph renderer for the cell, and on success records the
 *          rendered width/height into the cell's ::ra_glyph_dims_t descriptor.
 *
 * @param[in]  ctx        The owning ::ra_glyph_atlas_t (keycache `render_ctx`).
 * @param[in]  key        The ::ra_glyph_key_t to render.
 * @param[out] cell       Destination bitmap buffer (`cell_bytes` writable).
 * @param[in]  cell_bytes Cell capacity in bytes.
 * @param[out] user       The cell's ::ra_glyph_dims_t descriptor.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok    The glyph was rendered and the descriptor recorded.
 * @retval k_ra_err_* The caller's renderer error (descriptor left untouched).
 *
 * @pre `ctx` and `user` are non-NULL (the keycache guarantees both here).
 * @pre `key` points at a valid ::ra_glyph_key_t.
 * @post On success `*user` holds the rendered glyph dimensions.
 * @post On failure the descriptor is not written.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t
priv_glyph_render(void* ctx, const void* key, uint8_t* cell, uint32_t cell_bytes, void* user)
{
  const ra_glyph_atlas_t* atlas = (const ra_glyph_atlas_t*)ctx;
  const ra_glyph_key_t*   gk    = (const ra_glyph_key_t*)key;
  ra_glyph_dims_t*        dims  = (ra_glyph_dims_t*)user;
  uint16_t                w     = 0;
  uint16_t                h     = 0;
  const ra_err_t          rerr  = atlas->render(atlas->render_ctx, gk, cell, cell_bytes, &w, &h);
  if (rerr != k_ra_ok) {
    return rerr;
  }
  dims->w = w;
  dims->h = h;
  return k_ra_ok;
}

ra_err_t ra_glyph_atlas_init(ra_glyph_atlas_t* atlas, const ra_glyph_atlas_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(atlas, s_tag, "atlas must not be nullptr");
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA_CHECK_NULL_PTR(cfg->render, s_tag, "render must not be nullptr");
  (void)memset(atlas, 0, sizeof(*atlas));
  atlas->render          = cfg->render;
  atlas->render_ctx      = cfg->render_ctx;
  ra_keycache_cfg_t kcfg = {};
  kcfg.cell_mem          = cfg->cell_mem;
  kcfg.cell_bytes        = cfg->cell_bytes;
  kcfg.cell_count        = cfg->cell_count;
  kcfg.key_mem           = (uint8_t*)cfg->keys;
  kcfg.key_bytes         = (uint32_t)sizeof(ra_glyph_key_t);
  kcfg.user_mem          = (uint8_t*)cfg->dims;
  kcfg.user_bytes        = (uint32_t)sizeof(ra_glyph_dims_t);
  kcfg.meta              = cfg->meta;
  kcfg.buckets           = cfg->buckets;
  kcfg.bucket_count      = cfg->bucket_count;
  kcfg.render            = priv_glyph_render;
  kcfg.render_ctx        = atlas;
  return ra_keycache_init(&atlas->kc, &kcfg);
}

ra_err_t
ra_glyph_atlas_get(ra_glyph_atlas_t* atlas, const ra_glyph_key_t* key, ra_glyph_t* out_glyph)
{
  RA_CHECK_NULL_PTR(atlas, s_tag, "atlas must not be nullptr");
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out_glyph, s_tag, "out_glyph must not be nullptr");
  ra_keycache_view_t v   = {};
  const ra_err_t     err = ra_keycache_get(&atlas->kc, key, &v);
  if (err != k_ra_ok) {
    return err;
  }
  const ra_glyph_dims_t* dims = (const ra_glyph_dims_t*)v.user;
  *out_glyph                  = (ra_glyph_t){.bitmap = v.data, .width = dims->w, .height = dims->h};
  return k_ra_ok;
}

ra_err_t ra_glyph_atlas_put(ra_glyph_atlas_t* atlas, const uint8_t* bitmap)
{
  RA_CHECK_NULL_PTR(atlas, s_tag, "atlas must not be nullptr");
  RA_CHECK_NULL_PTR(bitmap, s_tag, "bitmap must not be nullptr");
  return ra_keycache_put(&atlas->kc, bitmap);
}

ra_err_t ra_glyph_atlas_stats(const ra_glyph_atlas_t* atlas,
                              uint32_t*               out_hits,
                              uint32_t*               out_misses,
                              uint32_t*               out_evictions)
{
  RA_CHECK_NULL_PTR(atlas, s_tag, "atlas must not be nullptr");
  if (atlas->kc.cfg.cell_mem == nullptr) {
    return k_ra_err_invalid_state;
  }
  return ra_keycache_stats(&atlas->kc, out_hits, out_misses, out_evictions);
}
