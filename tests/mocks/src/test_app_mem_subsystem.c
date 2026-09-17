/**
 * @file test_app_mem_subsystem.c
 * @brief Integration test: mirrors the mem_subsystem example (#263).
 *
 * @details
 * Replays each drive the app
 * examples/ek_ra8d2/hil_needs_revalidation/mem_subsystem/src/main.c performs on the #147
 * ra8_mem memory hierarchy, on the host-compiled library, and asserts the exact
 * observables the app prints in its banner:
 *
 *   mem: slab_free=8 arena_free=4096 tile_evict=5 vmem_crc=D1E67963 PASS
 *
 * Exercised modules:
 *   - ra8_slab         (alloc-to-exhaustion, free, reset-by-reinit)
 *   - ra8_arena        (aligned bump carves, over-subscription, reset-by-reinit)
 *   - ra8_tile_cache   (LRU eviction past capacity: hits/misses/evictions)
 *   - ra8_vmem_stream  (byte windows through a fixed page cache over 1 MiB backing)
 *
 * Because ra8_mem is pure computation with no MMIO, the host result is identical
 * to ra8_emulator and silicon, so the CRC + counters asserted here are the same
 * values the EIL / HIL gate pins (EIL == HIL). Two compound decisions carry
 * MC/DC vectors.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_arena.h"
#include "ra8_err.h"
#include "ra8_slab.h"
#include "ra8_tile_cache.h"
#include "ra8_vmem_stream.h"
#include "ra8_vsource.h"
#include "unity_minimal.h"

/** @brief Slab fixture sizing (matches main.c). */
typedef enum : uint32_t {
  k_slab_pool_bytes = 512U, /**< Slab pool bytes. */
  k_slab_cell_bytes = 64U,  /**< Slab cell bytes. */
  k_slab_cells      = 8U,   /**< Slab cells.      */
  k_slab_freed      = 3U,   /**< Slab freed.      */
} slab_fix_t;

/** @brief Arena fixture sizing (matches main.c). */
typedef enum : uint32_t {
  k_arena_pool_bytes = 4096U, /**< Arena pool bytes. */
  k_arena_carve0     = 1000U, /**< Arena carve0.     */
  k_arena_carve1     = 2000U, /**< Arena carve1.     */
  k_arena_big        = 2000U, /**< Arena big.        */
  k_arena_align0     = 8U,    /**< Arena align0.     */
  k_arena_align1     = 16U,   /**< Arena align1.     */
} arena_fix_t;

/** @brief Tile-cache fixture sizing + expected counters (matches main.c). */
typedef enum : uint32_t {
  k_tile_cell_bytes = 64U, /**< Tile cell bytes. */
  k_tile_cells      = 4U,  /**< Tile cells.      */
  k_tile_buckets    = 8U,  /**< Tile buckets.    */
  k_tile_distinct   = 8U,  /**< Tile distinct.   */
  k_tile_exp_hits   = 4U,  /**< Tile exp hits.   */
  k_tile_exp_miss   = 9U,  /**< Tile exp miss.   */
  k_tile_exp_evict  = 5U,  /**< Tile exp evict.  */
  k_tile_dim        = 16U, /**< Tile dim.        */
} tile_fix_t;

/** @brief vmem_stream fixture sizing + windows + expected CRC (matches main.c). */
typedef enum : uint32_t {
  k_vmem_frame_bytes = 512U,        /**< Vmem frame bytes.                                      */
  k_vmem_frames      = 4U,          /**< Vmem frames.                                           */
  k_vmem_buckets     = 8U,          /**< Vmem buckets.                                          */
  k_vmem_objs        = 1U,          /**< Vmem objs.                                             */
  k_vmem_obj_bytes   = 1048576U,    /**< Vmem obj bytes.                                        */
  k_win_buf_bytes    = 1024U,       /**< Win buffer bytes.                                      */
  k_fail_obj_bytes   = 2048U,       /**< Backing size of the fault-injection object (4 frames). */
  k_fail_read_bytes  = 1024U,       /**< Two frames requested from it: frame 0 loads, frame 1
                                         faults, so the read must come back short.           */
  k_probe_obj_id     = 7U,          /**< Opaque object id for the unbound-cache probe.        */
  k_probe_obj_bytes  = 1U,          /**< Any non-zero size; the frame-size guard fires first. */
  k_win0_off         = 0U,          /**< Win0 off.                                            */
  k_win0_len         = 100U,        /**< Win0 length.                                         */
  k_win1_off         = 500U,        /**< Win1 off.                                            */
  k_win1_len         = 100U,        /**< Win1 length.                                         */
  k_win2_off         = 1000U,       /**< Win2 off.                                            */
  k_win2_len         = 1000U,       /**< Win2 length.                                         */
  k_win_eof_back     = 50U,         /**< Win eof back.                                        */
  k_win_eof_len      = 100U,        /**< Win eof length.                                      */
  k_win_past_len     = 10U,         /**< Win past length.                                     */
  k_vmem_exp_crc     = 0xD1E67963U, /**< Vmem exp CRC.                                        */
} vmem_fix_t;

/** @brief Deterministic generator + CRC-32 constants (match main.c). */
typedef enum : uint32_t {
  k_gen_mul       = 0x9E3779B1U, /**< Gen mul.       */
  k_gen_add       = 0x00A5A5A5U, /**< Gen add.       */
  k_gen_shift_a   = 5U,          /**< Gen shift a.   */
  k_gen_shift_b   = 13U,         /**< Gen shift b.   */
  k_gen_shift_out = 24U,         /**< Gen shift out. */
  k_crc_init      = 0xFFFFFFFFU, /**< CRC init.      */
  k_crc_xorout    = 0xFFFFFFFFU, /**< CRC xorout.    */
  k_crc_poly      = 0xEDB88320U, /**< CRC poly.      */
  k_crc_bits      = 8U,          /**< CRC bits.      */
} gen_crc_t;

/* --- shared drive helpers (identical logic to the app) -------------------- */

/** @brief Deterministic backing byte at absolute @p off (matches main.c). */
static uint8_t gen_byte(uint64_t off)
{
  const uint32_t x = (uint32_t)off;
  uint32_t       h = x ^ (x >> (uint32_t)k_gen_shift_a) ^ (x >> (uint32_t)k_gen_shift_b);
  h                = (h * (uint32_t)k_gen_mul) + (uint32_t)k_gen_add;
  return (uint8_t)(h >> (uint32_t)k_gen_shift_out);
}

/** @brief Fold one byte into a running CRC-32 (matches main.c). */
static uint32_t crc32_byte(uint32_t crc, uint8_t b)
{
  crc ^= (uint32_t)b;
  for (uint32_t bit = 0U; bit < (uint32_t)k_crc_bits; bit++) {
    const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
    crc                 = (crc >> 1U) ^ ((uint32_t)k_crc_poly & mask);
  }
  return crc;
}

/** @brief Backing read callback: fill @p buf with the generator. */
static ra8_err_t backing_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if (buf == nullptr) {
    return k_ra8_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = gen_byte(offset + (uint64_t)i);
  }
  return k_ra8_ok;
}

/**
 * @brief Backing reader that serves frame 0 and then faults.
 *
 * @details
 * Models a paged source whose media dies part-way through an object: any read
 * whose offset is at or past the first frame boundary reports
 * k_ra8_err_hw_error instead of filling the buffer. That is what makes
 * ra8_vmem_get() fail on the SECOND frame of a two-frame request, so
 * ra8_vmem_stream_read() has to break out of its copy loop with bytes already
 * copied and report a short count.
 *
 * @param[in]  ctx    Unused loader cookie.
 * @param[in]  offset Absolute byte offset being loaded.
 * @param[out] buf    Destination frame buffer.
 * @param[in]  len    Bytes to fill.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Offset is inside frame 0; the frame was filled.
 * @retval k_ra8_err_null_ptr `buf` was NULL.
 * @retval k_ra8_err_hw_error  Offset is at or past the first frame boundary.
 *
 * @pre `len` bytes of `buf` are writable when `buf` is non-NULL.
 * @pre The caller treats a non-ok return as "this frame is not resident".
 * @post On k_ra8_ok, `buf` holds the same bytes backing_read() would produce.
 * @post On a fault return, `buf` is not modified.
 *
 * @note Not thread-safe; stateless.
 * @since 0.1.0
 */
static ra8_err_t failing_backing_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if (buf == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (offset >= (uint64_t)k_vmem_frame_bytes) {
    return k_ra8_err_hw_error;
  }
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = gen_byte(offset + (uint64_t)i);
  }
  return k_ra8_ok;
}

/** @brief Synthetic tile decoder: stamps the key column into the cell. */
static ra8_err_t tile_decode(void*                 ctx,
                             const ra8_tile_key_t* key,
                             uint8_t*              cell,
                             uint32_t              cell_bytes,
                             uint16_t*             out_w,
                             uint16_t*             out_h)
{
  (void)ctx;
  (void)memset(cell, 0, (size_t)cell_bytes);
  cell[0] = (uint8_t)key->tile_x;
  *out_w  = (uint16_t)k_tile_dim;
  *out_h  = (uint16_t)k_tile_dim;
  return k_ra8_ok;
}

/** @brief get + immediately put tile column @p tx (so it ends unpinned). */
static ra8_err_t tile_touch(ra8_tile_cache_t* tc, uint16_t tx)
{
  ra8_tile_key_t k    = {};
  k.tile_x            = tx;
  ra8_tile_t      t   = {};
  const ra8_err_t err = ra8_tile_cache_get(tc, &k, &t);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_tile_cache_put(tc, t.pixels);
}

/* --- compound-decision predicates (MC/DC subjects) ------------------------ */

/**
 * @brief Overall verdict: every exercise passed.
 *
 * @details Compound 4-condition AND, mirroring the app's four sequential
 *          "fail on any layer" guards collapsed into one decision.
 */
static bool mem_verdict(bool slab_ok, bool arena_ok, bool tile_ok, bool vmem_ok)
{
  return slab_ok && arena_ok && tile_ok && vmem_ok;
}

/**
 * @brief One window read is correct: exact length AND every byte matched.
 */
static bool window_ok(size_t got, size_t want, bool bytes_match)
{
  return (got == want) && bytes_match;
}

/* --- tests ---------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decision under test -- alloc every cell, the next alloc fails,
 * free 3, and a reset re-init returns all 8 cells to the freelist)
 */
static void test_slab_reset(void)
{
  TEST_BEGIN("mem_subsystem: slab alloc/free/reset");
  [[gnu::aligned(8)]] static uint8_t s_pool[k_slab_pool_bytes];
  ra8_slab_t                         slab = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_slab_init(&slab, s_pool, (uint32_t)sizeof(s_pool), (uint32_t)k_slab_cell_bytes));

  void* cells[k_slab_cells] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_slab_cells; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_alloc(&slab, &cells[i]));
  }
  void* extra = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_slab_alloc(&slab, &extra)); /* exhausted */

  for (uint32_t i = 0U; i < (uint32_t)k_slab_freed; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_free(&slab, cells[i]));
  }
  uint32_t freev = 0U;
  uint32_t total = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_stats(&slab, &freev, &total));
  TEST_ASSERT_EQ(k_slab_freed, freev);
  TEST_ASSERT_EQ(k_slab_cells, total);

  /* Reset by re-init: every cell free again. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_slab_init(&slab, s_pool, (uint32_t)sizeof(s_pool), (uint32_t)k_slab_cell_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_slab_stats(&slab, &freev, &total));
  TEST_ASSERT_EQ(k_slab_cells, freev);
  TEST_END("mem_subsystem: slab alloc/free/reset");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- two aligned carves succeed, an
 * over-subscribed carve returns no_mem, and a reset re-init frees the region)
 */
static void test_arena_reset(void)
{
  TEST_BEGIN("mem_subsystem: arena carve/oversub/reset");
  [[gnu::aligned(8)]] static uint8_t s_pool[k_arena_pool_bytes];
  ra8_arena_t                        arena = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_init(&arena, s_pool, (uint32_t)sizeof(s_pool)));

  void* b0 = nullptr;
  void* b1 = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_arena_carve(&arena, (uint32_t)k_arena_carve0, (uint32_t)k_arena_align0, &b0));
  TEST_ASSERT_NOT_NULL(b0);
  TEST_ASSERT_EQ(0U, ((uintptr_t)b0 % (uintptr_t)k_arena_align0)); /* aligned */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_arena_carve(&arena, (uint32_t)k_arena_carve1, (uint32_t)k_arena_align1, &b1));
  TEST_ASSERT_EQ(0U, ((uintptr_t)b1 % (uintptr_t)k_arena_align1)); /* aligned */

  void* big = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_arena_carve(&arena, (uint32_t)k_arena_big, (uint32_t)k_arena_align0, &big));

  /* Reset by re-init: whole region available again. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_init(&arena, s_pool, (uint32_t)sizeof(s_pool)));
  uint32_t rem = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_arena_remaining(&arena, &rem));
  TEST_ASSERT_EQ(k_arena_pool_bytes, rem);
  TEST_END("mem_subsystem: arena carve/oversub/reset");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- 8 distinct tiles through a 4-cell cache
 * force LRU eviction; the exact hit/miss/eviction counters are asserted)
 */
static void test_tile_eviction(void)
{
  TEST_BEGIN("mem_subsystem: tile-cache overfill eviction");
  [[gnu::aligned(8)]] static uint8_t s_cells[(size_t)k_tile_cells * (size_t)k_tile_cell_bytes];
  static ra8_tile_key_t              s_keys[k_tile_cells];
  static ra8_tile_dims_t             s_dims[k_tile_cells];
  static ra8_keycache_cell_t         s_meta[k_tile_cells];
  static int32_t                     s_buckets[k_tile_buckets];

  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = {};
  cfg.cell_mem             = s_cells;
  cfg.cell_bytes           = (uint32_t)k_tile_cell_bytes;
  cfg.cell_count           = (uint32_t)k_tile_cells;
  cfg.meta                 = s_meta;
  cfg.keys                 = s_keys;
  cfg.dims                 = s_dims;
  cfg.buckets              = s_buckets;
  cfg.bucket_count         = (uint32_t)k_tile_buckets;
  cfg.decode               = tile_decode;
  cfg.decode_ctx           = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  for (uint32_t i = 0U; i < (uint32_t)k_tile_distinct; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, tile_touch(&tc, (uint16_t)i));
  }
  for (uint32_t i = 0U; i < (uint32_t)k_tile_cells; i++) {
    const uint16_t tx = (uint16_t)((uint32_t)k_tile_distinct - 1U - i);
    TEST_ASSERT_EQ(k_ra8_ok, tile_touch(&tc, tx)); /* resident -> hit */
  }
  TEST_ASSERT_EQ(k_ra8_ok, tile_touch(&tc, 0U)); /* evicted -> miss + evict */

  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t ev   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&tc, &hits, &miss, &ev));
  TEST_ASSERT_EQ(k_tile_exp_hits, hits);
  TEST_ASSERT_EQ(k_tile_exp_miss, miss);
  TEST_ASSERT_EQ(k_tile_exp_evict, ev);
  TEST_END("mem_subsystem: tile-cache overfill eviction");
}

/** @brief Read + byte-verify one window; fold into @p crc; return got. */
static size_t
stream_window(ra8_vmem_stream_t* st, uint64_t off, uint32_t len, uint32_t* crc, bool* bytes_ok)
{
  static uint8_t s_buf[k_win_buf_bytes];
  const size_t   got = ra8_vmem_stream_read(st, off, s_buf, (size_t)len);
  bool           ok  = true;
  for (size_t i = 0U; i < got; i++) {
    if (s_buf[i] != gen_byte(off + (uint64_t)i)) {
      ok = false;
    }
    *crc = crc32_byte(*crc, s_buf[i]);
  }
  *bytes_ok = ok;
  return got;
}

/**
 * @brief Wire a paged vsource, a frame-backed vmem, and a stream over them.
 *
 * @details
 * Builds the three-layer stack the window tests read through: a paged source
 * that synthesises the 1 MiB backing on demand, a vmem whose frame pool is
 * deliberately far smaller than that backing so reads must evict, and a stream
 * positioned over the whole object. The pools are function-local `static`
 * because they must outlive the call while staying invisible to everything
 * else; @p vs and @p vm are the caller's, since the stream keeps pointing at
 * them after this returns.
 *
 * @param[out] vs Receives the initialised paged source.
 * @param[out] vm Receives the initialised vmem over @p vs.
 * @param[out] st Receives the stream over the whole backing object.
 *
 *
 * @pre All three out-pointers are non-NULL and outlive @p st.
 * @pre No other stack is live over the same static pools.
 * @post @p st reads the full `k_vmem_obj_bytes` object.
 * @post The frame pool is smaller than the object, so paging is forced.
 *
 * @note Not thread-safe; the pools are file-lifetime state.
 *
 * @see backing_read()  The synthetic backing this pages from.
 */
static void vmem_stream_open_backing(ra8_vsource_t* vs, ra8_vmem_t* vm, ra8_vmem_stream_t* st)
{
  [[gnu::aligned(8)]] static uint8_t s_frames[(size_t)k_vmem_frames * (size_t)k_vmem_frame_bytes];
  static ra8_vmem_frame_t            s_fmeta[k_vmem_frames];
  static ra8_vmem_key_t              s_fkeys[k_vmem_frames];
  static int32_t                     s_fbuckets[k_vmem_buckets];
  static ra8_vsource_obj_t           s_objs[k_vmem_objs];

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(vs, s_objs, (uint32_t)k_vmem_objs));
  uint32_t obj_id = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_vsource_add_paged(vs, backing_read, nullptr, 0U, (uint64_t)k_vmem_obj_bytes, &obj_id));

  ra8_vmem_cfg_t vcfg = {};
  vcfg.frame_mem      = s_frames;
  vcfg.frame_bytes    = (uint32_t)k_vmem_frame_bytes;
  vcfg.frame_count    = (uint32_t)k_vmem_frames;
  vcfg.meta           = s_fmeta;
  vcfg.keys           = s_fkeys;
  vcfg.buckets        = s_fbuckets;
  vcfg.bucket_count   = (uint32_t)k_vmem_buckets;
  vcfg.loader         = ra8_vsource_loader;
  vcfg.loader_ctx     = vs;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(vm, &vcfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stream_init(st, vm, obj_id, (uint64_t)k_vmem_obj_bytes));
}

/**
 * @par MC/DC:
 * (no compound decision under test here -- windows crossing frame boundaries,
 * clamped at EOF, and past EOF are byte-verified; the CRC + paging are pinned.
 * The window_ok / mem_verdict compound decisions are covered below.)
 */
static void test_vmem_stream_windows(void)
{
  TEST_BEGIN("mem_subsystem: vmem_stream windows over 1 MiB backing");
  ra8_vsource_t     vs = {};
  ra8_vmem_t        vm = {};
  ra8_vmem_stream_t st = {};
  vmem_stream_open_backing(&vs, &vm, &st);

  uint32_t crc = (uint32_t)k_crc_init;
  bool     bok = false;
  size_t   got = 0U;

  got = stream_window(&st, (uint64_t)k_win0_off, (uint32_t)k_win0_len, &crc, &bok);
  TEST_ASSERT(window_ok(got, (size_t)k_win0_len, bok));
  got = stream_window(&st, (uint64_t)k_win1_off, (uint32_t)k_win1_len, &crc, &bok);
  TEST_ASSERT(window_ok(got, (size_t)k_win1_len, bok));
  got = stream_window(&st, (uint64_t)k_win2_off, (uint32_t)k_win2_len, &crc, &bok);
  TEST_ASSERT(window_ok(got, (size_t)k_win2_len, bok));

  const uint64_t eof_off = (uint64_t)k_vmem_obj_bytes - (uint64_t)k_win_eof_back;
  got                    = stream_window(&st, eof_off, (uint32_t)k_win_eof_len, &crc, &bok);
  TEST_ASSERT(window_ok(got, (size_t)k_win_eof_back, bok)); /* clamped to 50 */

  got = stream_window(&st, (uint64_t)k_vmem_obj_bytes, (uint32_t)k_win_past_len, &crc, &bok);
  TEST_ASSERT(window_ok(got, 0U, bok)); /* fully past EOF */

  crc ^= (uint32_t)k_crc_xorout;
  TEST_ASSERT_EQ(k_vmem_exp_crc, crc); /* == banner vmem_crc, EIL == HIL */

  uint32_t ev = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, nullptr, nullptr, &ev));
  TEST_ASSERT(ev >= 1U); /* 1 MiB paged through 2 KiB resident -> eviction */
  TEST_END("mem_subsystem: vmem_stream windows over 1 MiB backing");
}

/**
 * @par MC/DC:
 * Decision: `slab_ok && arena_ok && tile_ok && vmem_ok` (4 conditions).
 * - V1: 1,1,1,1 -> true  (control: all true)
 * - V2: 0,1,1,1 -> false (varies slab_ok only)
 * - V3: 1,0,1,1 -> false (varies arena_ok only)
 * - V4: 1,1,0,1 -> false (varies tile_ok only)
 * - V5: 1,1,1,0 -> false (varies vmem_ok only)
 * V1 vs V2/V3/V4/V5 each prove one condition independently flips the outcome.
 * N+1 = 5 vectors for N=4 conditions: minimal MC/DC.
 */
static void test_verdict_mcdc(void)
{
  TEST_BEGIN("mem_subsystem: verdict MC/DC");
  TEST_ASSERT(mem_verdict(true, true, true, true));   /* V1 */
  TEST_ASSERT(!mem_verdict(false, true, true, true)); /* V2 */
  TEST_ASSERT(!mem_verdict(true, false, true, true)); /* V3 */
  TEST_ASSERT(!mem_verdict(true, true, false, true)); /* V4 */
  TEST_ASSERT(!mem_verdict(true, true, true, false)); /* V5 */
  TEST_END("mem_subsystem: verdict MC/DC");
}

/**
 * @par MC/DC:
 * Decision: `(got == want) && bytes_match` (2 conditions).
 * - V1: got==want (true),  bytes_match=true  -> true  (control)
 * - V2: got!=want (false), bytes_match=true  -> false (varies length only)
 * - V3: got==want (true),  bytes_match=false -> false (varies match only)
 * V1 vs V2 proves length independently affects the outcome; V1 vs V3 proves the
 * same for the byte match. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_window_ok_mcdc(void)
{
  TEST_BEGIN("mem_subsystem: window_ok MC/DC");
  TEST_ASSERT(window_ok((size_t)k_win0_len, (size_t)k_win0_len, true));      /* V1 */
  TEST_ASSERT(!window_ok((size_t)k_win_eof_back, (size_t)k_win0_len, true)); /* V2 */
  TEST_ASSERT(!window_ok((size_t)k_win0_len, (size_t)k_win0_len, false));    /* V3 */
  TEST_END("mem_subsystem: window_ok MC/DC");
}

/**
 * @par MC/DC:
 * Decision: `if (fb == 0U)` in `ra8_vmem_stream_init()`
 * (libs/ra8_mem/src/ra8_vmem_stream.c) -- 1 condition.
 * - V1: a cache initialised by ra8_vmem_init, so cfg.frame_bytes == 512 ->
 *   false, and the binding is written (control; every
 *   vmem_stream_open_backing() call above).
 * - V2: a zero-initialised ra8_vmem_t that never went through ra8_vmem_init,
 *   so cfg.frame_bytes == 0 -> true, and the call is rejected (this case).
 * N = 1 condition, N+1 = 2 vectors. V1 and V2 differ only in `fb` and give
 * opposite outcomes, so `fb` independently affects the decision. V2 also
 * proves the guard is load-bearing: without it `st->frame_bytes` would be
 * bound to 0 and every later read would divide by zero.
 */
static void test_vmem_stream_init_rejects_zero_frame(void)
{
  TEST_BEGIN("mem_subsystem: vmem_stream_init rejects a zero frame size");
  ra8_vmem_stream_t st = {};
  ra8_vmem_t        vm = {}; /* never through ra8_vmem_init -> cfg.frame_bytes == 0 */

  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_vmem_stream_init(&st, &vm, (uint32_t)k_probe_obj_id, (uint64_t)k_probe_obj_bytes));
  /* Rejected before anything was bound. */
  TEST_ASSERT(st.vm == nullptr);
  TEST_ASSERT_EQ(0U, st.frame_bytes);
  TEST_END("mem_subsystem: vmem_stream_init rejects a zero frame size");
}

/**
 * @par MC/DC:
 * Decision: `if (ra8_vmem_get(...) != k_ra8_ok)` inside the copy loop of
 * `ra8_vmem_stream_read()` (libs/ra8_mem/src/ra8_vmem_stream.c) -- 1
 * condition.
 * - V1: the frame pages in -> false, the slice is copied and the loop
 *   continues (control; the first frame of this very read, and every window in
 *   test_vmem_stream_windows above).
 * - V2: the loader reports k_ra8_err_hw_error for the second frame -> true,
 *   the loop breaks and the call returns the bytes copied so far (this case).
 * N = 1 condition, N+1 = 2 vectors. V1 and V2 occur in the same call over the
 * same buffer and differ only in the ra8_vmem_get() outcome, so that condition
 * independently affects the decision. The short return count is the observable
 * consequence: a 1024-byte request comes back as 512.
 */
static void test_vmem_stream_read_stops_on_get_failure(void)
{
  TEST_BEGIN("mem_subsystem: vmem_stream_read stops at the first unloadable frame");
  [[gnu::aligned(8)]] static uint8_t s_ffrm[(size_t)k_vmem_frames * (size_t)k_vmem_frame_bytes];
  static ra8_vmem_frame_t            s_fmeta2[k_vmem_frames];
  static ra8_vmem_key_t              s_fkeys2[k_vmem_frames];
  static int32_t                     s_fbuck2[k_vmem_buckets];
  static ra8_vsource_obj_t           s_objs2[k_vmem_objs];
  static uint8_t                     s_out[k_win_buf_bytes];

  ra8_vsource_t vs     = {};
  ra8_vmem_t    vm     = {};
  uint32_t      obj_id = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, s_objs2, (uint32_t)k_vmem_objs));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vsource_add_paged(&vs,
                                       failing_backing_read,
                                       nullptr,
                                       0U,
                                       (uint64_t)k_fail_obj_bytes,
                                       &obj_id));

  ra8_vmem_cfg_t vcfg = {};
  vcfg.frame_mem      = s_ffrm;
  vcfg.frame_bytes    = (uint32_t)k_vmem_frame_bytes;
  vcfg.frame_count    = (uint32_t)k_vmem_frames;
  vcfg.meta           = s_fmeta2;
  vcfg.keys           = s_fkeys2;
  vcfg.buckets        = s_fbuck2;
  vcfg.bucket_count   = (uint32_t)k_vmem_buckets;
  vcfg.loader         = ra8_vsource_loader;
  vcfg.loader_ctx     = &vs;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &vcfg));

  ra8_vmem_stream_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stream_init(&st, &vm, obj_id, (uint64_t)k_fail_obj_bytes));

  /* Two frames requested. Frame 0 loads (V1); frame 1 faults (V2). */
  const size_t got = ra8_vmem_stream_read(&st, 0U, s_out, (size_t)k_fail_read_bytes);
  TEST_ASSERT_EQ(k_vmem_frame_bytes, got);
  /* The bytes that WERE copied are the real ones, so the break happened after
     a genuine copy rather than instead of one. */
  bool bytes_ok = true;
  for (size_t i = 0U; i < got; i++) {
    if (s_out[i] != gen_byte((uint64_t)i)) {
      bytes_ok = false;
    }
  }
  TEST_ASSERT(bytes_ok);
  TEST_END("mem_subsystem: vmem_stream_read stops at the first unloadable frame");
}

int main(void)
{
  test_slab_reset();
  test_arena_reset();
  test_tile_eviction();
  test_vmem_stream_windows();
  test_verdict_mcdc();
  test_window_ok_mcdc();
  test_vmem_stream_init_rejects_zero_frame();
  test_vmem_stream_read_stops_on_get_failure();
  return 0;
}
