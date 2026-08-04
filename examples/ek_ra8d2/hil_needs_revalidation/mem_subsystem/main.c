/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/mem_subsystem/main.c
 * @brief Drives each ra8_mem (#147) memory-hierarchy layer in isolation (#263).
 *
 * @details
 * The #147 memory primitives (ra8_slab, ra8_arena, ra8_tile_cache, and the
 * ra8_vmem / ra8_vsource / ra8_vmem_stream page cache) are exercised inside the
 * e-reader but had no direct example. This app drives each one on its own so the
 * behaviour is observable without the whole reader stack:
 *
 *   1. **Slab (Layer 0)** -- carve a fixed pool into equal cells, allocate every
 *      cell (the next allocation fails), free a few, then *reset* by re-init and
 *      confirm every cell is free again.
 *   2. **Arena (Layer 0)** -- bump two aligned sub-blocks out of one region, prove
 *      an over-subscribed carve fails, then *reset* by re-init and confirm the
 *      whole region is available again.
 *   3. **Tile cache (Layer 3b)** -- fault more distinct tiles through the cache
 *      than it has cells so LRU eviction is forced, then observe the hit / miss /
 *      eviction counters.
 *   4. **vmem_stream (Layer 2 helper)** -- front a 1 MiB synthetic backing object
 *      with a fixed 4-frame page cache and read back several byte windows (some
 *      crossing frame boundaries, one clamped at EOF, one fully past EOF),
 *      verifying every byte and folding them into a CRC-32 fingerprint. The
 *      resident set stays 2 KiB while the object is 1 MiB -- paging, not residency.
 *
 * The console banner is:
 *
 *   `mem: slab_free=8 arena_free=4096 tile_evict=5 vmem_crc=<8hex> PASS`
 *
 * Every value is computed by the deterministic ra8_mem library from fixed inputs
 * with no MMIO, so the banner is byte-identical on the host unit test
 * (tests/test_app_mem_subsystem.c), ra8_emulator, and silicon. Any failure on any
 * layer prints a FAIL banner and traps on a BKPT before the PASS line, so the
 * EIL / HIL gate is exact.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / App] {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_arena.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_slab.h"
#include "ra8_tile_cache.h"
#include "ra8_time.h"
#include "ra8_vmem_stream.h"
#include "ra8_vsource.h"

/** @enum mem_console_t @brief Console + banner-format knobs (no magic numbers). */
typedef enum : uint32_t {
  k_mem_uart_baud   = 115200U,     /**< Console baud.                      */
  k_mem_crc_init    = 0xFFFFFFFFU, /**< CRC-32 initial value.              */
  k_mem_crc_xorout  = 0xFFFFFFFFU, /**< CRC-32 final xor (== bitwise not). */
  k_mem_crc_poly    = 0xEDB88320U, /**< CRC-32 reflected polynomial.       */
  k_mem_crc_bits    = 8U,          /**< Bits folded per byte.              */
  k_mem_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value.      */
  k_mem_nibble_bits = 4U,          /**< Bits per hex nibble.               */
  k_mem_nibble_mask = 0x0FU,       /**< Low-nibble mask.                   */
  k_mem_dec_base    = 10U,         /**< Decimal base / hex-digit split.    */
} mem_console_t;

/** @enum mem_slab_cfg_t @brief Slab-exercise sizing. */
typedef enum : uint32_t {
  k_mem_slab_pool_bytes = 512U, /**< Backing pool size.            */
  k_mem_slab_cell_bytes = 64U,  /**< Bytes per cell (512/64 = 8).  */
  k_mem_slab_cells      = 8U,   /**< Cells the pool divides into.  */
  k_mem_slab_freed      = 3U,   /**< Cells freed before the reset. */
} mem_slab_cfg_t;

/** @enum mem_arena_cfg_t @brief Arena-exercise sizing. */
typedef enum : uint32_t {
  k_mem_arena_pool_bytes = 4096U, /**< Backing region size.               */
  k_mem_arena_carve0     = 1000U, /**< First carve size.                  */
  k_mem_arena_carve1     = 2000U, /**< Second carve size.                 */
  k_mem_arena_big        = 2000U, /**< Over-subscribed carve (must fail). */
  k_mem_arena_align0     = 8U,    /**< First / big carve alignment.       */
  k_mem_arena_align1     = 16U,   /**< Second carve alignment.            */
} mem_arena_cfg_t;

/** @enum mem_tile_cfg_t @brief Tile-cache-exercise sizing + expected counters. */
typedef enum : uint32_t {
  k_mem_tile_cell_bytes = 64U, /**< Bytes per tile cell.                */
  k_mem_tile_cells      = 4U,  /**< Cells in the cache.                 */
  k_mem_tile_buckets    = 8U,  /**< Hash buckets.                       */
  k_mem_tile_distinct   = 8U,  /**< Distinct tiles faulted (> cells).   */
  k_mem_tile_exp_hits   = 4U,  /**< Expected hits after the drive.      */
  k_mem_tile_exp_miss   = 9U,  /**< Expected misses after the drive.    */
  k_mem_tile_exp_evict  = 5U,  /**< Expected evictions after the drive. */
} mem_tile_cfg_t;

/** @enum mem_tile_dim_t @brief Synthetic decoded-tile dimensions. */
typedef enum : uint16_t {
  k_mem_tile_dim = 16U, /**< Synthetic decoded tile edge length. */
} mem_tile_dim_t;

/** @enum mem_vmem_cfg_t @brief vmem_stream-exercise sizing + read windows. */
typedef enum : uint32_t {
  k_mem_vmem_frame_bytes = 512U,     /**< Page-cache frame size.                       */
  k_mem_vmem_frames      = 4U,       /**< Frames in the fixed pool (2 KiB resident).   */
  k_mem_vmem_buckets     = 8U,       /**< Page-cache hash buckets.                     */
  k_mem_vmem_objs        = 1U,       /**< Registry slots.                              */
  k_mem_vmem_obj_bytes   = 1048576U, /**< Synthetic backing object size (1 MiB).       */
  k_mem_win_buf_bytes    = 1024U,    /**< Read scratch (>= the widest window).         */
  k_mem_win0_off         = 0U,       /**< Window 0 offset (within frame 0).            */
  k_mem_win0_len         = 100U,     /**< Window 0 length.                             */
  k_mem_win1_off         = 500U,     /**< Window 1 offset (crosses frame boundary).    */
  k_mem_win1_len         = 100U,     /**< Window 1 length.                             */
  k_mem_win2_off         = 1000U,    /**< Window 2 offset (spans several frames).      */
  k_mem_win2_len         = 1000U,    /**< Window 2 length.                             */
  k_mem_win_eof_back     = 50U,      /**< Bytes before EOF the near-EOF window starts. */
  k_mem_win_eof_len      = 100U,     /**< Near-EOF window length (clamps to 50).       */
  k_mem_win_past_len     = 10U,      /**< Past-EOF window length (reads 0).            */
} mem_vmem_cfg_t;

/** @enum mem_gen_t @brief Deterministic backing-byte generator constants. */
typedef enum : uint32_t {
  k_mem_gen_mul       = 0x9E3779B1U, /**< Odd multiplicative mixer. */
  k_mem_gen_add       = 0x00A5A5A5U, /**< Additive constant.        */
  k_mem_gen_shift_a   = 5U,          /**< First xor-fold shift.     */
  k_mem_gen_shift_b   = 13U,         /**< Second xor-fold shift.    */
  k_mem_gen_shift_out = 24U,         /**< Output byte select shift. */
} mem_gen_t;

/**
 * @struct mem_report_t
 * @brief Observables gathered from the four exercises.
 */
typedef struct {
  uint32_t slab_free;      /**< Free cells after the slab reset.       */
  uint32_t arena_free;     /**< Bytes remaining after the arena reset. */
  uint32_t tile_hits;      /**< Tile-cache hit count.                  */
  uint32_t tile_misses;    /**< Tile-cache miss count.                 */
  uint32_t tile_evictions; /**< Tile-cache eviction count.             */
  uint32_t vmem_crc;       /**< CRC-32 over the streamed byte windows. */
  uint32_t vmem_evictions; /**< Page-cache evictions (paging proof).   */
} mem_report_t;

/* --- Backing storage, carved once at file scope (NASA P10 Rule 3) --------- */

/** @brief Slab-exercise pool. */
[[gnu::aligned(8)]] static uint8_t s_slab_pool[k_mem_slab_pool_bytes];
/** @brief Arena-exercise region. */
[[gnu::aligned(8)]] static uint8_t s_arena_pool[k_mem_arena_pool_bytes];
/** @brief Tile-cache decoded-pixel cells. */
[[gnu::aligned(
  8)]] static uint8_t s_tile_cells[(size_t)k_mem_tile_cells * (size_t)k_mem_tile_cell_bytes];
/** @brief Tile-cache per-cell key storage. */
static ra8_tile_key_t s_tile_keys[k_mem_tile_cells];
/** @brief Tile-cache per-cell dimension descriptors. */
static ra8_tile_dims_t s_tile_dims[k_mem_tile_cells];
/** @brief Tile-cache per-cell link metadata. */
static ra8_keycache_cell_t s_tile_meta[k_mem_tile_cells];
/** @brief Tile-cache hash buckets. */
static int32_t s_tile_buckets[k_mem_tile_buckets];
/** @brief Page-cache frame storage (2 KiB resident). */
[[gnu::aligned(
  8)]] static uint8_t s_vmem_frames[(size_t)k_mem_vmem_frames * (size_t)k_mem_vmem_frame_bytes];
/** @brief Page-cache per-frame metadata. */
static ra8_vmem_frame_t s_vmem_fmeta[k_mem_vmem_frames];
/** @brief Page-cache per-frame key storage. */
static ra8_vmem_key_t s_vmem_fkeys[k_mem_vmem_frames];
/** @brief Page-cache hash buckets. */
static int32_t s_vmem_fbuckets[k_mem_vmem_buckets];
/** @brief Object-source registry slots. */
static ra8_vsource_obj_t s_vmem_objs[k_mem_vmem_objs];
/** @brief Stream read scratch (>= the widest window). */
static uint8_t s_win_buf[k_mem_win_buf_bytes];

static const uint8_t k_msg_boot[]       = "mem-subsystem: boot 2KiB-resident-1MiB-paged\r\n";
static const uint8_t k_msg_fail_init[]  = "mem: FAIL init\r\n";
static const uint8_t k_msg_fail_slab[]  = "mem: FAIL slab\r\n";
static const uint8_t k_msg_fail_arena[] = "mem: FAIL arena\r\n";
static const uint8_t k_msg_fail_tile[]  = "mem: FAIL tile\r\n";
static const uint8_t k_msg_fail_vmem[]  = "mem: FAIL vmem\r\n";
static const uint8_t k_msg_slab[]       = "mem: slab_free=";
static const uint8_t k_msg_arena[]      = " arena_free=";
static const uint8_t k_msg_tile[]       = " tile_evict=";
static const uint8_t k_msg_vmem[]       = " vmem_crc=";
static const uint8_t k_msg_ok[]         = " PASS\r\n";

/** @brief Emit a byte run on the board console. */
static void mem_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void mem_panic_halt(const uint8_t* msg, uint32_t len)
{
  mem_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Fold one byte into a running CRC-32 (reflected, poly 0xEDB88320). */
static uint32_t mem_crc32_byte(uint32_t crc, uint8_t b)
{
  crc ^= (uint32_t)b;
  for (uint32_t bit = 0U; bit < (uint32_t)k_mem_crc_bits; bit++) {
    const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
    crc                 = (crc >> 1U) ^ ((uint32_t)k_mem_crc_poly & mask);
  }
  return crc;
}

/** @brief Deterministic backing byte at absolute @p off (host/emulator/silicon-equal). */
static uint8_t mem_gen_byte(uint64_t off)
{
  const uint32_t x = (uint32_t)off;
  uint32_t       h = x ^ (x >> (uint32_t)k_mem_gen_shift_a) ^ (x >> (uint32_t)k_mem_gen_shift_b);
  h                = (h * (uint32_t)k_mem_gen_mul) + (uint32_t)k_mem_gen_add;
  return (uint8_t)(h >> (uint32_t)k_mem_gen_shift_out);
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void mem_print_hex(uint32_t value)
{
  uint8_t buf[k_mem_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_mem_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_mem_hex_nibbles - 1U - i) * (uint32_t)k_mem_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_mem_nibble_mask;
    buf[i] =
      (uint8_t)((nib < (uint32_t)k_mem_dec_base) ? ('0' + nib) : ('A' + (nib - k_mem_dec_base)));
  }
  mem_print(buf, (uint32_t)k_mem_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void mem_print_uint(uint32_t value)
{
  uint8_t  buf[k_mem_dec_base];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_mem_dec_base)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_mem_dec_base));
    n++;
    value /= (uint32_t)k_mem_dec_base;
  }
  for (uint32_t i = 0U; i < n; i++) {
    mem_print(&buf[n - 1U - i], 1U);
  }
}

/**
 * @brief Exercise 1: slab alloc-to-exhaustion, free, and reset-by-reinit.
 *
 * @param[out] r Report to update with the free-cell count after the reset.
 * @return true on the expected behaviour, false on any deviation.
 */
static bool mem_run_slab(mem_report_t* r)
{
  ra8_slab_t slab = {};
  if (ra8_slab_init(&slab,
                    s_slab_pool,
                    (uint32_t)sizeof(s_slab_pool),
                    (uint32_t)k_mem_slab_cell_bytes) != k_ra8_ok) {
    return false;
  }
  void* cells[k_mem_slab_cells] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_mem_slab_cells; i++) {
    if (ra8_slab_alloc(&slab, &cells[i]) != k_ra8_ok) {
      return false;
    }
  }
  void* extra = nullptr;
  if (ra8_slab_alloc(&slab, &extra) != k_ra8_err_no_mem) {
    return false;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_mem_slab_freed; i++) {
    if (ra8_slab_free(&slab, cells[i]) != k_ra8_ok) {
      return false;
    }
  }
  uint32_t free_before = 0U;
  if ((ra8_slab_stats(&slab, &free_before, nullptr) != k_ra8_ok) ||
      (free_before != (uint32_t)k_mem_slab_freed)) {
    return false;
  }
  /* Reset: re-init re-threads every cell back onto the freelist. */
  if (ra8_slab_init(&slab,
                    s_slab_pool,
                    (uint32_t)sizeof(s_slab_pool),
                    (uint32_t)k_mem_slab_cell_bytes) != k_ra8_ok) {
    return false;
  }
  uint32_t total = 0U;
  if (ra8_slab_stats(&slab, &r->slab_free, &total) != k_ra8_ok) {
    return false;
  }
  return (total == (uint32_t)k_mem_slab_cells) && (r->slab_free == (uint32_t)k_mem_slab_cells);
}

/**
 * @brief Exercise 2: arena bump carves, over-subscription, and reset-by-reinit.
 *
 * @param[out] r Report to update with the bytes remaining after the reset.
 * @return true on the expected behaviour, false on any deviation.
 */
static bool mem_run_arena(mem_report_t* r)
{
  ra8_arena_t arena = {};
  if (ra8_arena_init(&arena, s_arena_pool, (uint32_t)sizeof(s_arena_pool)) != k_ra8_ok) {
    return false;
  }
  void* b0 = nullptr;
  void* b1 = nullptr;
  if (ra8_arena_carve(&arena, (uint32_t)k_mem_arena_carve0, (uint32_t)k_mem_arena_align0, &b0) !=
      k_ra8_ok) {
    return false;
  }
  if (ra8_arena_carve(&arena, (uint32_t)k_mem_arena_carve1, (uint32_t)k_mem_arena_align1, &b1) !=
      k_ra8_ok) {
    return false;
  }
  void* big = nullptr;
  if (ra8_arena_carve(&arena, (uint32_t)k_mem_arena_big, (uint32_t)k_mem_arena_align0, &big) !=
      k_ra8_err_no_mem) {
    return false;
  }
  /* Reset: re-init drops the high-water mark back to zero. */
  if (ra8_arena_init(&arena, s_arena_pool, (uint32_t)sizeof(s_arena_pool)) != k_ra8_ok) {
    return false;
  }
  if (ra8_arena_remaining(&arena, &r->arena_free) != k_ra8_ok) {
    return false;
  }
  return r->arena_free == (uint32_t)k_mem_arena_pool_bytes;
}

/** @brief Synthetic tile decoder: stamps the key column into the cell. */
static ra8_err_t mem_tile_decode(void*                 ctx,
                                 const ra8_tile_key_t* key,
                                 uint8_t*              cell,
                                 uint32_t              cell_bytes,
                                 uint16_t*             out_w,
                                 uint16_t*             out_h)
{
  (void)ctx;
  (void)memset(cell, 0, (size_t)cell_bytes);
  cell[0] = (uint8_t)key->tile_x;
  *out_w  = (uint16_t)k_mem_tile_dim;
  *out_h  = (uint16_t)k_mem_tile_dim;
  return k_ra8_ok;
}

/** @brief Fetch (get + immediately put) tile column @p tx, so it is unpinned. */
static ra8_err_t mem_tile_touch(ra8_tile_cache_t* tc, uint16_t tx)
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

/**
 * @brief Exercise 3: overfill the tile cache to observe LRU eviction.
 *
 * @param[out] r Report to update with the hit / miss / eviction counters.
 * @return true on the expected counters, false on any deviation.
 */
static bool mem_run_tiles(mem_report_t* r)
{
  ra8_tile_cache_t           tc  = {};
  const ra8_tile_cache_cfg_t cfg = {
    .cell_mem     = s_tile_cells,
    .cell_bytes   = (uint32_t)k_mem_tile_cell_bytes,
    .cell_count   = (uint32_t)k_mem_tile_cells,
    .meta         = s_tile_meta,
    .keys         = s_tile_keys,
    .dims         = s_tile_dims,
    .buckets      = s_tile_buckets,
    .bucket_count = (uint32_t)k_mem_tile_buckets,
    .decode       = mem_tile_decode,
    .decode_ctx   = nullptr,
  };
  if (ra8_tile_cache_init(&tc, &cfg) != k_ra8_ok) {
    return false;
  }
  /* Overfill: more distinct tiles than cells forces LRU eviction. */
  for (uint32_t i = 0U; i < (uint32_t)k_mem_tile_distinct; i++) {
    if (mem_tile_touch(&tc, (uint16_t)i) != k_ra8_ok) {
      return false;
    }
  }
  /* The most-recent k_mem_tile_cells tiles are still resident: re-touch them
   * (MRU-first) so each is a hit and none evicts. */
  for (uint32_t i = 0U; i < (uint32_t)k_mem_tile_cells; i++) {
    const uint16_t tx = (uint16_t)((uint32_t)k_mem_tile_distinct - 1U - i);
    if (mem_tile_touch(&tc, tx) != k_ra8_ok) {
      return false;
    }
  }
  /* Re-touch the very first tile (long evicted): one more miss + eviction. */
  if (mem_tile_touch(&tc, 0U) != k_ra8_ok) {
    return false;
  }
  if (ra8_tile_cache_stats(&tc, &r->tile_hits, &r->tile_misses, &r->tile_evictions) != k_ra8_ok) {
    return false;
  }
  return (r->tile_hits == (uint32_t)k_mem_tile_exp_hits) &&
         (r->tile_misses == (uint32_t)k_mem_tile_exp_miss) &&
         (r->tile_evictions == (uint32_t)k_mem_tile_exp_evict);
}

/** @brief Backing read callback: fill @p buf with the deterministic generator. */
static ra8_err_t mem_backing_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if (buf == nullptr) {
    return k_ra8_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = mem_gen_byte(offset + (uint64_t)i);
  }
  return k_ra8_ok;
}

/**
 * @brief Read one byte window through the stream, verify it, and fold it in.
 *
 * @param[in]     st      Bound vmem stream.
 * @param[in]     off     Absolute object offset to read from.
 * @param[in]     len     Bytes requested.
 * @param[in,out] crc     Running CRC-32 the read bytes are folded into.
 * @param[out]    out_got Bytes the stream actually returned.
 * @return true if every returned byte matched the generator, false otherwise.
 */
static bool
mem_stream_window(ra8_vmem_stream_t* st, uint64_t off, uint32_t len, uint32_t* crc, size_t* out_got)
{
  if (len > (uint32_t)k_mem_win_buf_bytes) {
    return false;
  }
  const size_t got = ra8_vmem_stream_read(st, off, s_win_buf, (size_t)len);
  for (size_t i = 0U; i < got; i++) {
    if (s_win_buf[i] != mem_gen_byte(off + (uint64_t)i)) {
      return false;
    }
    *crc = mem_crc32_byte(*crc, s_win_buf[i]);
  }
  *out_got = got;
  return true;
}

/**
 * @brief Read every window and confirm the stream returned the expected lengths.
 *
 * @param[in]     st  Bound vmem stream.
 * @param[in,out] crc Running CRC-32 the read bytes are folded into.
 * @return true if every window read its expected (clamped) length correctly.
 */
static bool mem_vmem_read_windows(ra8_vmem_stream_t* st, uint32_t* crc)
{
  size_t got = 0U;
  if (!mem_stream_window(st, (uint64_t)k_mem_win0_off, (uint32_t)k_mem_win0_len, crc, &got) ||
      (got != (size_t)k_mem_win0_len)) {
    return false;
  }
  if (!mem_stream_window(st, (uint64_t)k_mem_win1_off, (uint32_t)k_mem_win1_len, crc, &got) ||
      (got != (size_t)k_mem_win1_len)) {
    return false;
  }
  if (!mem_stream_window(st, (uint64_t)k_mem_win2_off, (uint32_t)k_mem_win2_len, crc, &got) ||
      (got != (size_t)k_mem_win2_len)) {
    return false;
  }
  const uint64_t eof_off = (uint64_t)k_mem_vmem_obj_bytes - (uint64_t)k_mem_win_eof_back;
  if (!mem_stream_window(st, eof_off, (uint32_t)k_mem_win_eof_len, crc, &got) ||
      (got != (size_t)k_mem_win_eof_back)) {
    return false;
  }
  if (!mem_stream_window(st,
                         (uint64_t)k_mem_vmem_obj_bytes,
                         (uint32_t)k_mem_win_past_len,
                         crc,
                         &got) ||
      (got != 0U)) {
    return false;
  }
  return true;
}

/**
 * @brief Exercise 4: stream byte windows out of a 1 MiB object via a 2 KiB cache.
 *
 * @param[out] r Report to update with the window CRC + page-cache evictions.
 * @return true on the expected reads (and that paging occurred), false otherwise.
 */
static bool mem_run_vmem(mem_report_t* r)
{
  ra8_vsource_t vs = {};
  if (ra8_vsource_init(&vs, s_vmem_objs, (uint32_t)k_mem_vmem_objs) != k_ra8_ok) {
    return false;
  }
  uint32_t obj_id = 0U;
  if (ra8_vsource_add_paged(&vs,
                            mem_backing_read,
                            nullptr,
                            0U,
                            (uint64_t)k_mem_vmem_obj_bytes,
                            &obj_id) != k_ra8_ok) {
    return false;
  }
  ra8_vmem_t           vm   = {};
  const ra8_vmem_cfg_t vcfg = {
    .frame_mem    = s_vmem_frames,
    .frame_bytes  = (uint32_t)k_mem_vmem_frame_bytes,
    .frame_count  = (uint32_t)k_mem_vmem_frames,
    .meta         = s_vmem_fmeta,
    .keys         = s_vmem_fkeys,
    .buckets      = s_vmem_fbuckets,
    .bucket_count = (uint32_t)k_mem_vmem_buckets,
    .loader       = ra8_vsource_loader,
    .loader_ctx   = &vs,
  };
  if (ra8_vmem_init(&vm, &vcfg) != k_ra8_ok) {
    return false;
  }
  ra8_vmem_stream_t st = {};
  if (ra8_vmem_stream_init(&st, &vm, obj_id, (uint64_t)k_mem_vmem_obj_bytes) != k_ra8_ok) {
    return false;
  }
  uint32_t crc = (uint32_t)k_mem_crc_init;
  if (!mem_vmem_read_windows(&st, &crc)) {
    return false;
  }
  r->vmem_crc = crc ^ (uint32_t)k_mem_crc_xorout;
  if (ra8_vmem_stats(&vm, nullptr, nullptr, &r->vmem_evictions) != k_ra8_ok) {
    return false;
  }
  /* A 1 MiB object read through a 4-frame pool must have paged at least once. */
  return r->vmem_evictions >= 1U;
}

/** @brief Bring up clocks / MSTP / time + the board console; halt on failure. */
static void mem_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    mem_panic_halt(k_msg_fail_init, (uint32_t)sizeof(k_msg_fail_init) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    mem_panic_halt(k_msg_fail_init, (uint32_t)sizeof(k_msg_fail_init) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    mem_panic_halt(k_msg_fail_init, (uint32_t)sizeof(k_msg_fail_init) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_mem_uart_baud) != k_ra8_ok) {
    mem_panic_halt(k_msg_fail_init, (uint32_t)sizeof(k_msg_fail_init) - 1U);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: drive each ra8_mem layer, then print the observables banner.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The slab/arena/tile/vmem banner is emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  mem_setup_or_halt();
  ra8_isr_globals_enable();
  mem_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  mem_report_t r = {};
  if (!mem_run_slab(&r)) {
    mem_panic_halt(k_msg_fail_slab, (uint32_t)sizeof(k_msg_fail_slab) - 1U);
  }
  if (!mem_run_arena(&r)) {
    mem_panic_halt(k_msg_fail_arena, (uint32_t)sizeof(k_msg_fail_arena) - 1U);
  }
  if (!mem_run_tiles(&r)) {
    mem_panic_halt(k_msg_fail_tile, (uint32_t)sizeof(k_msg_fail_tile) - 1U);
  }
  if (!mem_run_vmem(&r)) {
    mem_panic_halt(k_msg_fail_vmem, (uint32_t)sizeof(k_msg_fail_vmem) - 1U);
  }

  mem_print(k_msg_slab, (uint32_t)sizeof(k_msg_slab) - 1U);
  mem_print_uint(r.slab_free);
  mem_print(k_msg_arena, (uint32_t)sizeof(k_msg_arena) - 1U);
  mem_print_uint(r.arena_free);
  mem_print(k_msg_tile, (uint32_t)sizeof(k_msg_tile) - 1U);
  mem_print_uint(r.tile_evictions);
  mem_print(k_msg_vmem, (uint32_t)sizeof(k_msg_vmem) - 1U);
  mem_print_hex(r.vmem_crc);
  mem_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
