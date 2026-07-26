/**
 * @file cache_store_demo.c
 * @brief Hardware-free ra8_cache_store on-media cache demo -- implementation (#257).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives ra8_cache_store end to end over an injected LevelX NOR driver: mount,
 * put/get four keyed "render/glyph cache" blobs, pin one, force an eviction,
 * re-put into the reclaimed sectors, checkpoint-close, then remount over the
 * same media and confirm the survivors + the pin persisted. Touches no
 * peripheral register, so the emulator and silicon runs are identical.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "cache_store_demo.h"

#include <stdint.h>
#include <string.h>

#include "ra8_cache_store.h"
#include "ra8_check.h"
#include "ra8_err.h"

/**
 * @enum demo_key_t
 * @brief Content keys for the demo's cached blobs (source CRC-32 stand-ins).
 * @details The cover atlas is the pinned entry; tile_b is the one forcibly
 *          evicted; tile_d is re-put into the space tile_b freed.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_demo_key_cover  = 0xC07E5001U, /**< Open-book cover glyph atlas (pinned).  */
  k_demo_key_tile_a = 0x11E0A002U, /**< Render tile A (survivor).              */
  k_demo_key_tile_b = 0x11E0B003U, /**< Render tile B (forcibly evicted).      */
  k_demo_key_tile_c = 0x11E0C004U, /**< Render tile C (survivor).              */
  k_demo_key_tile_d = 0x11E0D005U, /**< Render tile D (re-put after eviction). */
} demo_key_t;

/**
 * @enum demo_len_t
 * @brief Payload lengths (bytes) for the demo blobs; several cross a sector.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_demo_len_cover  = 1400U, /**< Cover atlas length. */
  k_demo_len_tile_a = 300U,  /**< Tile A length.      */
  k_demo_len_tile_b = 900U,  /**< Tile B length.      */
  k_demo_len_tile_c = 1100U, /**< Tile C length.      */
  k_demo_len_tile_d = 400U,  /**< Tile D length.      */
} demo_len_t;

/**
 * @enum demo_seed_t
 * @brief Per-blob deterministic fill seeds (a distinct byte ramp per key).
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_demo_seed_cover  = 0x40U, /**< Cover atlas seed. */
  k_demo_seed_tile_a = 0x11U, /**< Tile A seed.      */
  k_demo_seed_tile_b = 0x22U, /**< Tile B seed.      */
  k_demo_seed_tile_c = 0x33U, /**< Tile C seed.      */
  k_demo_seed_tile_d = 0x55U, /**< Tile D seed.      */
} demo_seed_t;

/**
 * @enum demo_count_t
 * @brief Expected live-entry count after the remount.
 * @details cover + tile_a + tile_c + tile_d survive; tile_b was evicted.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_demo_survivors = 4U, /**< Entries expected live after the remount. */
} demo_count_t;

/**
 * @brief Fill @p buf with the demo's deterministic byte ramp.
 * @param[out] buf  Destination buffer.
 * @param[in]  len  Byte count.
 * @param[in]  seed Ramp seed (`buf[i] == (seed + i) & 0xFF`).
 * @return Nothing.
 * @pre @p buf is non-NULL, or @p len is 0.
 * @pre @p len is the intended pattern length.
 * @post On a non-NULL buffer, `buf[i] == (uint8_t)(seed + i)` for every i.
 * @post No state outside @p buf changes.
 * @note Pure over its arguments (thread-safe).
 * @since 0.1.0
 */
static void demo_fill(uint8_t* buf, uint32_t len, uint8_t seed)
{
  if (buf == nullptr) {
    return;
  }
  if (len == 0U) {
    return;
  }
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)(seed + (uint8_t)i);
  }
}

/**
 * @brief Test whether @p buf holds the demo's deterministic byte ramp.
 * @param[in] buf  Buffer to check.
 * @param[in] len  Byte count.
 * @param[in] seed Expected ramp seed.
 * @return True iff every byte matches `(seed + i) & 0xFF`.
 * @retval true  Every byte matches (or @p len is 0).
 * @retval false @p buf is NULL, or a byte diverges.
 * @pre @p buf is non-NULL, or @p len is 0.
 * @pre @p seed matches the seed the buffer was filled with.
 * @post @p buf is unmodified (pure read).
 * @post The result depends only on the arguments.
 * @note Pure over its arguments (thread-safe).
 * @since 0.1.0
 */
static bool demo_matches(const uint8_t* buf, uint32_t len, uint8_t seed)
{
  if (buf == nullptr) {
    return false;
  }
  if (len == 0U) {
    return true;
  }
  for (uint32_t i = 0U; i < len; i++) {
    if (buf[i] != (uint8_t)(seed + (uint8_t)i)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Build an ra8_cache_store init config from the demo config.
 * @param[in]  in     Demo config (injected deps + buffers).
 * @param[in]  format True to format (wipe) before open, false to remount.
 * @param[out] out    Receives the populated store config.
 * @return Error code.
 * @retval k_ra8_ok           Config populated.
 * @retval k_ra8_err_null_ptr @p in or @p out was NULL.
 * @pre @p in and @p out are non-NULL.
 * @pre @p in's buffers out-live the store built from the config.
 * @post On success @p out references @p in's buffers.
 * @post No global state changes.
 * @note Not thread-safe; operates only on the caller's structs.
 * @since 0.1.0
 */
static ra8_err_t
demo_build_cfg(const cache_store_demo_cfg_t* in, bool format, ra8_cache_store_cfg_t* out)
{
  RA8_CHECK_NULL_PTR(in, "rcs_demo", "cfg is NULL");
  RA8_CHECK_NULL_PTR(out, "rcs_demo", "out cfg is NULL");
  *out = (ra8_cache_store_cfg_t){
    .nor_flash         = in->nor_flash,
    .nor_driver_init   = in->nor_driver_init,
    .name              = "rcs_demo",
    .index             = in->index,
    .staging           = in->staging,
    .staging_bytes     = in->staging_bytes,
    .logical_sectors   = in->logical_sectors,
    .index_cap         = in->index_cap,
    .overprovision_pct = 0U, /* 0 => the store's default GC headroom margin. */
    .format            = format,
  };
  return k_ra8_ok;
}

/**
 * @brief Put a blob, then read it back whole and verify it byte-identical.
 * @param[in,out] st   Initialised store.
 * @param[in]     key  Content key.
 * @param[in]     seed Deterministic fill seed for the blob.
 * @param[in]     len  Blob length in bytes.
 * @param[in]     cfg  Demo config (supplies the read/write scratch buffer).
 * @return Error code.
 * @retval k_ra8_ok             Sealed and verified.
 * @retval k_ra8_err_null_ptr   @p st or @p cfg was NULL.
 * @retval k_ra8_err_invalid_size Scratch too small for @p len.
 * @retval k_ra8_err_invalid_state Read-back diverged or wrong length.
 * @retval other                A store put/get/read error, propagated.
 * @pre @p st is mounted and @p cfg is non-NULL.
 * @pre `cfg->scratch_bytes >= len`.
 * @post On `k_ra8_ok`, @p key streams back exactly the written blob.
 * @post On any error the store is left as ra8_cache_store defines.
 * @note Not thread-safe; reuses the single scratch buffer.
 * @since 0.1.0
 */
static ra8_err_t demo_put_and_verify(ra8_cache_store_t*            st,
                                     uint32_t                      key,
                                     uint8_t                       seed,
                                     uint32_t                      len,
                                     const cache_store_demo_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(st, "rcs_demo", "store is NULL");
  RA8_CHECK_NULL_PTR(cfg, "rcs_demo", "cfg is NULL");
  if (len > cfg->scratch_bytes) {
    return k_ra8_err_invalid_size;
  }
  demo_fill(cfg->scratch, len, seed);
  ra8_err_t rc = ra8_cache_store_put(st, key, cfg->scratch, len);
  if (rc != k_ra8_ok) {
    return rc;
  }
  ra8_cache_store_reader_t rd = {};
  rc                          = ra8_cache_store_get(st, key, &rd);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (rd.byte_len != len) {
    return k_ra8_err_invalid_state;
  }
  (void)memset(cfg->scratch, 0, (size_t)len);
  rc = ra8_cache_store_read(&rd, 0U, cfg->scratch, len);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (!demo_matches(cfg->scratch, len, seed)) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Read a sealed blob back and verify it byte-identical (no put).
 * @param[in] st   Initialised store.
 * @param[in] key  Content key to open.
 * @param[in] seed Deterministic fill seed the blob was written with.
 * @param[in] len  Expected blob length in bytes.
 * @param[in] cfg  Demo config (supplies the read scratch buffer).
 * @return Error code.
 * @retval k_ra8_ok             Present and byte-identical.
 * @retval k_ra8_err_null_ptr   @p st or @p cfg was NULL.
 * @retval k_ra8_err_invalid_state Read-back diverged or wrong length.
 * @retval other                A store get/read error, propagated.
 * @pre @p st is mounted and @p cfg is non-NULL.
 * @pre `cfg->scratch_bytes >= len`.
 * @post On `k_ra8_ok`, @p key held exactly the expected blob.
 * @post @p cfg->scratch holds the read-back bytes.
 * @note Not thread-safe; reuses the single scratch buffer.
 * @since 0.1.0
 */
static ra8_err_t demo_get_and_verify(const ra8_cache_store_t*      st,
                                     uint32_t                      key,
                                     uint8_t                       seed,
                                     uint32_t                      len,
                                     const cache_store_demo_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(st, "rcs_demo", "store is NULL");
  RA8_CHECK_NULL_PTR(cfg, "rcs_demo", "cfg is NULL");
  ra8_cache_store_reader_t rd = {};
  ra8_err_t                rc = ra8_cache_store_get(st, key, &rd);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (rd.byte_len != len) {
    return k_ra8_err_invalid_state;
  }
  (void)memset(cfg->scratch, 0, (size_t)len);
  rc = ra8_cache_store_read(&rd, 0U, cfg->scratch, len);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (!demo_matches(cfg->scratch, len, seed)) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Seed the store: put + read-verify the four demo blobs.
 * @param[in,out] st  Initialised store.
 * @param[in]     cfg Demo config.
 * @return Error code (first failure short-circuits).
 * @retval k_ra8_ok           All four sealed + verified.
 * @retval k_ra8_err_null_ptr @p st or @p cfg was NULL.
 * @retval other              The first failing put/verify's error.
 * @pre @p st is mounted and empty.
 * @pre @p cfg is non-NULL with a large-enough scratch buffer.
 * @post On `k_ra8_ok`, four keyed entries are live.
 * @post On any error no further blob is put.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t demo_phase_seed(ra8_cache_store_t* st, const cache_store_demo_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(st, "rcs_demo", "store is NULL");
  RA8_CHECK_NULL_PTR(cfg, "rcs_demo", "cfg is NULL");
  ra8_err_t rc =
    demo_put_and_verify(st, k_demo_key_cover, k_demo_seed_cover, k_demo_len_cover, cfg);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = demo_put_and_verify(st, k_demo_key_tile_a, k_demo_seed_tile_a, k_demo_len_tile_a, cfg);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = demo_put_and_verify(st, k_demo_key_tile_b, k_demo_seed_tile_b, k_demo_len_tile_b, cfg);
  if (rc != k_ra8_ok) {
    return rc;
  }
  return demo_put_and_verify(st, k_demo_key_tile_c, k_demo_seed_tile_c, k_demo_len_tile_c, cfg);
}

/**
 * @brief Pin one entry, force an eviction, and prove the invariants hold.
 *
 * @details Pins the cover atlas (never evict the open book), evicts tile_b,
 *          then checks: tile_b is now a miss; the survivors (tile_a, tile_c)
 *          still resolve; and evicting the pinned cover is refused with busy.
 *
 * @param[in,out] st  Initialised, seeded store.
 * @return Error code.
 * @retval k_ra8_ok             Eviction + all guards behaved.
 * @retval k_ra8_err_null_ptr   @p st was NULL.
 * @retval k_ra8_err_invalid_state An expected guard result did not hold.
 * @retval other                A store pin/evict/get error, propagated.
 * @pre @p st is mounted with all four seed entries live.
 * @pre The cover key is present (to pin it).
 * @post On `k_ra8_ok`, tile_b is evicted and the cover is pinned.
 * @post On `k_ra8_ok`, the pinned cover cannot be evicted.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t demo_phase_evict(ra8_cache_store_t* st)
{
  RA8_CHECK_NULL_PTR(st, "rcs_demo", "store is NULL");
  ra8_err_t rc = ra8_cache_store_pin(st, k_demo_key_cover, true);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = ra8_cache_store_evict(st, k_demo_key_tile_b);
  if (rc != k_ra8_ok) {
    return rc;
  }
  ra8_cache_store_reader_t rd = {};
  if (ra8_cache_store_get(st, k_demo_key_tile_b, &rd) != k_ra8_err_not_found) {
    return k_ra8_err_invalid_state; /* evicted entry must be a miss */
  }
  if (ra8_cache_store_get(st, k_demo_key_tile_a, &rd) != k_ra8_ok) {
    return k_ra8_err_invalid_state; /* survivor must resolve */
  }
  if (ra8_cache_store_get(st, k_demo_key_tile_c, &rd) != k_ra8_ok) {
    return k_ra8_err_invalid_state; /* survivor must resolve */
  }
  if (ra8_cache_store_evict(st, k_demo_key_cover) != k_ra8_err_busy) {
    return k_ra8_err_invalid_state; /* pinned entry must refuse eviction */
  }
  return k_ra8_ok;
}

/**
 * @brief Read-verify the four entries expected to survive the remount.
 * @details Single-condition sequential checks (no compound decision) over the
 *          cover atlas plus tiles A, C, and the re-put tile D.
 * @param[in] st  Re-mounted store.
 * @param[in] cfg Demo config (scratch buffer).
 * @return Error code (first failure short-circuits).
 * @retval k_ra8_ok           All four survivors present + byte-identical.
 * @retval k_ra8_err_null_ptr @p st or @p cfg was NULL.
 * @retval other              The first failing survivor's error.
 * @pre @p st is a store re-mounted over the persisted media.
 * @pre @p cfg is non-NULL with a large-enough scratch buffer.
 * @post On `k_ra8_ok`, every survivor held exactly its written blob.
 * @post On any error no further survivor is read.
 * @note Not thread-safe; reuses the single scratch buffer.
 * @since 0.1.0
 */
static ra8_err_t demo_verify_survivors(const ra8_cache_store_t*      st,
                                       const cache_store_demo_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(st, "rcs_demo", "store is NULL");
  RA8_CHECK_NULL_PTR(cfg, "rcs_demo", "cfg is NULL");
  ra8_err_t rc =
    demo_get_and_verify(st, k_demo_key_cover, k_demo_seed_cover, k_demo_len_cover, cfg);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = demo_get_and_verify(st, k_demo_key_tile_a, k_demo_seed_tile_a, k_demo_len_tile_a, cfg);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = demo_get_and_verify(st, k_demo_key_tile_c, k_demo_seed_tile_c, k_demo_len_tile_c, cfg);
  if (rc != k_ra8_ok) {
    return rc;
  }
  return demo_get_and_verify(st, k_demo_key_tile_d, k_demo_seed_tile_d, k_demo_len_tile_d, cfg);
}

/**
 * @brief Simulate a reboot: re-mount over the same media and confirm survival.
 *
 * @details Zeroes the control RAM (index + staging + a fresh handle) to model a
 *          power cycle -- the on-media content in the injected NOR backing
 *          survives, the RAM does not -- then re-mounts with `format=false` and
 *          verifies the survivors byte-identical, the evicted key still gone,
 *          and the pin persisted across the checkpoint.
 *
 * @param[in]  cfg Demo config.
 * @param[out] out Result to update (survivors, flags, status on failure).
 * @return Error code.
 * @retval k_ra8_ok               Every survivor + invariant held.
 * @retval k_ra8_err_null_ptr     @p cfg or @p out was NULL.
 * @retval k_ra8_err_invalid_state A survivor / eviction / pin did not persist.
 * @pre @p cfg and @p out are non-NULL.
 * @pre A prior run closed the store cleanly over the same media.
 * @post On `k_ra8_ok`, `out->survivors == k_demo_survivors` and both flags set.
 * @post On failure `out->status == k_cache_store_demo_err_remount/_persist`.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t demo_phase_remount(const cache_store_demo_cfg_t* cfg,
                                    cache_store_demo_result_t*    out)
{
  RA8_CHECK_NULL_PTR(cfg, "rcs_demo", "cfg is NULL");
  RA8_CHECK_NULL_PTR(out, "rcs_demo", "out is NULL");
  /* Reboot: control state is lost, the media survives. */
  (void)memset(cfg->index, 0, (size_t)cfg->index_cap * sizeof(cfg->index[0]));
  (void)memset(cfg->staging, 0, (size_t)cfg->staging_bytes);
  ra8_cache_store_cfg_t cs2 = {};
  if (demo_build_cfg(cfg, false, &cs2) != k_ra8_ok) {
    out->status = k_cache_store_demo_err_remount;
    return k_ra8_err_invalid_state;
  }
  ra8_cache_store_t st2 = {};
  if (ra8_cache_store_init(&st2, &cs2) != k_ra8_ok) {
    out->status = k_cache_store_demo_err_remount;
    return k_ra8_err_invalid_state;
  }
  out->last_stage = k_cache_store_demo_stage_remounted;
  if (demo_verify_survivors(&st2, cfg) != k_ra8_ok) {
    out->status = k_cache_store_demo_err_persist;
    return k_ra8_err_invalid_state;
  }
  out->survivors              = k_demo_survivors;
  ra8_cache_store_reader_t rd = {};
  if (ra8_cache_store_get(&st2, k_demo_key_tile_b, &rd) != k_ra8_err_not_found) {
    out->status = k_cache_store_demo_err_persist;
    return k_ra8_err_invalid_state;
  }
  out->evicted_stayed_gone = true;
  if (ra8_cache_store_evict(&st2, k_demo_key_cover) != k_ra8_err_busy) {
    out->status = k_cache_store_demo_err_persist;
    return k_ra8_err_invalid_state;
  }
  out->pin_survived = true;
  (void)ra8_cache_store_close(&st2);
  return k_ra8_ok;
}

ra8_err_t cache_store_demo_run(const cache_store_demo_cfg_t* cfg, cache_store_demo_result_t* out)
{
  RA8_CHECK_NULL_PTR(cfg, "rcs_demo", "cfg is NULL");
  RA8_CHECK_NULL_PTR(out, "rcs_demo", "out is NULL");
  *out = (cache_store_demo_result_t){
    .status              = k_cache_store_demo_err_arg,
    .last_stage          = k_cache_store_demo_stage_none,
    .survivors           = 0U,
    .evicted_key         = k_demo_key_tile_b,
    .evicted_stayed_gone = false,
    .pin_survived        = false,
  };

  ra8_cache_store_cfg_t cs = {};
  if (demo_build_cfg(cfg, true, &cs) != k_ra8_ok) {
    out->status = k_cache_store_demo_err_mount;
    return k_ra8_err_invalid_state;
  }
  ra8_cache_store_t st = {};
  if (ra8_cache_store_init(&st, &cs) != k_ra8_ok) {
    out->status = k_cache_store_demo_err_mount;
    return k_ra8_err_invalid_state;
  }
  out->last_stage = k_cache_store_demo_stage_mounted;

  if (demo_phase_seed(&st, cfg) != k_ra8_ok) {
    out->status = k_cache_store_demo_err_seed;
    return k_ra8_err_invalid_state;
  }
  out->last_stage = k_cache_store_demo_stage_seeded;

  if (demo_phase_evict(&st) != k_ra8_ok) {
    out->status = k_cache_store_demo_err_evict;
    return k_ra8_err_invalid_state;
  }
  out->last_stage = k_cache_store_demo_stage_evicted;

  /* Reuse: the sectors tile_b freed now hold tile_d. */
  if (demo_put_and_verify(&st, k_demo_key_tile_d, k_demo_seed_tile_d, k_demo_len_tile_d, cfg) !=
      k_ra8_ok) {
    out->status = k_cache_store_demo_err_reuse;
    return k_ra8_err_invalid_state;
  }
  out->last_stage = k_cache_store_demo_stage_reused;

  if (ra8_cache_store_close(&st) != k_ra8_ok) {
    out->status = k_cache_store_demo_err_close;
    return k_ra8_err_invalid_state;
  }
  out->last_stage = k_cache_store_demo_stage_closed;

  if (demo_phase_remount(cfg, out) != k_ra8_ok) {
    return k_ra8_err_invalid_state; /* demo_phase_remount set out->status */
  }
  out->last_stage = k_cache_store_demo_stage_persisted;
  out->status     = k_cache_store_demo_ok;
  return k_ra8_ok;
}
