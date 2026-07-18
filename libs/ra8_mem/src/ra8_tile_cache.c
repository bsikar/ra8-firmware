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
