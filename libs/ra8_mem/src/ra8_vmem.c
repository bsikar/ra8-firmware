/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_vmem.c
 * @brief Byte-range page cache with SLRU eviction -- implementation (Layer 2).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * A thin typed facade over ::ra8_keycache, the one reader cache engine (#345
 * folded this module's once-duplicate hash/pin/SLRU machinery into that engine).
 * The cache key is an (object_id, frame-aligned offset) pair; the byte page is
 * the cell payload; the ::ra8_vmem_loader_fn adapts to the engine's
 * render-on-miss seam through ::priv_vmem_fill; and ::priv_vmem_hash injects a
 * page-oriented hash of the key. All eviction mechanics -- the probationary /
 * protected SLRU lists, the pinned-frame skip, hash chaining -- live in
 * ::ra8_keycache. What this file adds is the pointer-handle API, frame-boundary
 * alignment, and the ::ra8_vmem_prefetch read-ahead helper.
 */

#include "ra8_vmem.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_keycache.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_vmem";

/**
 * @enum ra8_vmem_hash_const_t
 * @brief Multiplicative constants mixing the two halves of the page key.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_vmem_hash_mul_obj  = 2654435761U, /**< Knuth multiplicative hash (object id). */
  k_vmem_hash_mul_page = 40503U,      /**< Odd multiplier mixing the offset.      */
} ra8_vmem_hash_const_t;

/**
 * @enum ra8_vmem_shift_const_t
 * @brief Bit shift splitting a 64-bit offset into two 32-bit halves.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_vmem_off_shift = 32U, /**< Shift for the high 32 bits of the offset. */
} ra8_vmem_shift_const_t;

/**
 * @brief Injected key hash: fold the (object_id, offset) page key (division-free).
 *
 * @details Adapts ::ra8_keycache_hash_fn for the byte-range key. Mixes the object
 *          id and both 32-bit halves of the frame-aligned offset through two odd
 *          multipliers, returning a raw 32-bit hash the engine folds to a bucket.
 *          Because the offset is always frame-aligned, its sub-frame low bits are
 *          zero, so hashing the offset is equivalent to hashing the page number
 *          without a run-time division.
 *
 * @param[in] key       The ::ra8_vmem_key_t being hashed.
 * @param[in] key_bytes Key width in bytes (unused; the layout is fixed).
 * @param[in] ctx       Unused (the key carries everything the hash needs).
 *
 * @return A raw 32-bit hash of the page key.
 * @retval 0 The key folded to zero (one possible result).
 *
 * @pre `key` points at a valid ::ra8_vmem_key_t.
 * @pre The key was zero-filled before its fields were set (no stray padding).
 * @post No state is modified.
 * @post The result depends only on the key's object id and offset.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_vmem_hash(const void* key, uint32_t key_bytes, void* ctx)
{
  (void)key_bytes;
  (void)ctx;
  const ra8_vmem_key_t* k  = (const ra8_vmem_key_t*)key;
  const uint32_t        lo = (uint32_t)k->offset;
  const uint32_t        hi = (uint32_t)(k->offset >> (uint64_t)k_vmem_off_shift);
  return (k->object_id * (uint32_t)k_vmem_hash_mul_obj) ^
         ((lo ^ hi) * (uint32_t)k_vmem_hash_mul_page);
}

/**
 * @brief Fill trampoline: adapt ::ra8_vmem_loader_fn to the keycache seam.
 *
 * @details Casts the keycache render context back to the owning cache and calls
 *          the caller's page loader for the cell, forwarding the key's object id
 *          and frame-aligned offset. The page cache carries no per-cell user
 *          descriptor, so @p user is ignored.
 *
 * @param[in]  ctx        The owning ::ra8_vmem_t (keycache `render_ctx`).
 * @param[in]  key        The ::ra8_vmem_key_t to load.
 * @param[out] cell       Destination frame buffer (`cell_bytes` writable).
 * @param[in]  cell_bytes Frame capacity in bytes.
 * @param[out] user       Unused (the page cache has no user descriptor).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    The page was loaded into the frame.
 * @retval k_ra8_err_* The caller's loader error (returned verbatim).
 *
 * @pre `ctx` and `key` are non-NULL (the keycache guarantees both here).
 * @pre `cell` addresses `cell_bytes` of writable frame storage.
 * @post On success the frame holds the requested page.
 * @post On failure the frame contents are unspecified (the victim is dropped).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_vmem_fill(void* ctx, const void* key, uint8_t* cell, uint32_t cell_bytes, void* user)
{
  (void)user;
  ra8_vmem_t*           vm = (ra8_vmem_t*)ctx;
  const ra8_vmem_key_t* k  = (const ra8_vmem_key_t*)key;
  return vm->cfg.loader(vm->cfg.loader_ctx, k->object_id, k->offset, cell, cell_bytes);
}

ra8_err_t ra8_vmem_init(ra8_vmem_t* vm, const ra8_vmem_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(vm, s_tag, "vm must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  (void)memset(vm, 0, sizeof(*vm));
  vm->cfg                 = *cfg;
  ra8_keycache_cfg_t kcfg = {};
  kcfg.cell_mem           = cfg->frame_mem;
  kcfg.cell_bytes         = cfg->frame_bytes;
  kcfg.cell_count         = cfg->frame_count;
  kcfg.key_mem            = (uint8_t*)cfg->keys;
  kcfg.key_bytes          = (uint32_t)sizeof(ra8_vmem_key_t);
  kcfg.user_mem           = nullptr;
  kcfg.user_bytes         = 0U;
  kcfg.meta               = cfg->meta;
  kcfg.buckets            = cfg->buckets;
  kcfg.bucket_count       = cfg->bucket_count;
  kcfg.render             = priv_vmem_fill;
  kcfg.render_ctx         = vm;
  kcfg.evict              = k_ra8_keycache_evict_slru;
  kcfg.protected_pct      = cfg->protected_pct;
  kcfg.hash               = priv_vmem_hash;
  kcfg.hash_ctx           = nullptr;
  const ra8_err_t err     = ra8_keycache_init(&vm->kc, &kcfg);
  if (err != k_ra8_ok) {
    return err;
  }
  vm->protected_cap = vm->kc.protected_cap;
  return k_ra8_ok;
}

ra8_err_t ra8_vmem_get(ra8_vmem_t* vm, uint32_t object_id, uint64_t offset, void** out_page)
{
  RA8_CHECK_NULL_PTR(vm, s_tag, "vm must not be nullptr");
  RA8_CHECK_NULL_PTR(out_page, s_tag, "out_page must not be nullptr");
  const uint32_t fb       = vm->cfg.frame_bytes;
  ra8_vmem_key_t k        = {};
  k.object_id             = object_id;
  k.offset                = offset - (offset % (uint64_t)fb);
  ra8_keycache_view_t v   = {};
  const ra8_err_t     err = ra8_keycache_get(&vm->kc, &k, &v);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_page = v.data;
  return k_ra8_ok;
}

ra8_err_t ra8_vmem_put(ra8_vmem_t* vm, void* page)
{
  RA8_CHECK_NULL_PTR(vm, s_tag, "vm must not be nullptr");
  RA8_CHECK_NULL_PTR(page, s_tag, "page must not be nullptr");
  return ra8_keycache_put(&vm->kc, (const uint8_t*)page);
}

ra8_err_t ra8_vmem_prefetch(ra8_vmem_t* vm, uint32_t object_id, uint64_t offset)
{
  RA8_CHECK_NULL_PTR(vm, s_tag, "vm must not be nullptr");
  void*           page     = nullptr;
  const ra8_err_t load_err = ra8_vmem_get(vm, object_id, offset, &page);
  if (load_err != k_ra8_ok) {
    return load_err;
  }
  /* Drop the pin now: the page is resident but evictable (SLRU probationary), so
   * a wrong read-ahead guess ages out before hot data. */
  return ra8_vmem_put(vm, page);
}

ra8_err_t ra8_vmem_stats(const ra8_vmem_t* vm,
                         uint32_t*         out_hits,
                         uint32_t*         out_misses,
                         uint32_t*         out_evictions)
{
  RA8_CHECK_NULL_PTR(vm, s_tag, "vm must not be nullptr");
  if (vm->cfg.frame_mem == nullptr) {
    return k_ra8_err_invalid_state;
  }
  return ra8_keycache_stats(&vm->kc, out_hits, out_misses, out_evictions);
}
