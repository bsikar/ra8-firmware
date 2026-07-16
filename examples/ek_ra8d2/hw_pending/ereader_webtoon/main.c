/**
 * @file examples/ek_ra8d2/hw_pending/ereader_webtoon/main.c
 * @brief Headless deterministic gate for the continuous vertical-scroll
 *        (webtoon / manhwa) reading mode (#289).
 *
 * @details
 * Exercises the whole #289 scroll engine end to end, with no panel / SDRAM /
 * touch / SD dependency, so the result is a single deterministic UART banner
 * that is identical on the unit-test host, in board_sim and on silicon
 * (SIM == HIL). The app:
 *
 *   1. Builds a tall synthetic webtoon strip as a raw RTA1 band-tile atlas in
 *      internal SRAM (`ra8_tileatlas`, one full-width band column) -- each
 *      pixel encodes its absolute canvas coordinate so the render is
 *      verifiable and every band seam is visible if the geometry is wrong.
 *   2. Pages bands through a small `ra8_tile_cache` (fewer cells than bands, so
 *      the LRU must evict -- the bounded-memory property) via
 *      `ra8_webtoon_tile_decode`.
 *   3. Opens the strip with `ra8_webtoon_open`, then scrolls the ENTIRE strip
 *      top -> bottom in fixed steps, compositing each visible band into a fixed
 *      RGB565 framebuffer through `ra8_gfx_blit` and folding an FNV-1a-32 hash
 *      over every rendered frame.
 *   4. Sums the per-frame `skipped` band count across the whole traversal (the
 *      #289 0-skip contract: it must be zero) and prints:
 *
 *        `ereader-webtoon: bands=<N> steps=<M> skipped=0 crc=<8 hex>`
 *
 * The strip fits the resident SRAM budget while still requiring many bands and
 * eviction; on a real reader the atlas lives in SDRAM/OSPI and only the visible
 * bands page into the cache -- the same code path, unbounded strip height.
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

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_tile_cache.h"
#include "ra8_tileatlas.h"
#include "ra8_time.h"
#include "ra8_webtoon.h"

/* ===========================================================================
 * Sizing + protocol constants (no magic numbers)
 * ===========================================================================
 */

/** @enum ew_geom_t @brief Synthetic strip + viewport geometry. */
typedef enum : uint32_t {
  k_ew_strip_w     = 100U,            /**< Strip width, pixels.                */
  k_ew_band_h      = 50U,             /**< Band (RTA1 tile) height, pixels.    */
  k_ew_strip_h     = 1000U,           /**< Strip height, pixels (20 bands).    */
  k_ew_bands       = 20U,             /**< 1000 / 50 bands.                    */
  k_ew_bpp         = 3U,              /**< RGB888 strip pixels.                */
  k_ew_view_w      = 100U,            /**< Viewport width, pixels.             */
  k_ew_view_h      = 300U,            /**< Viewport height, pixels.            */
  k_ew_scroll_step = 25U,             /**< Scroll increment per frame, pixels. */
  k_ew_band_bytes  = 100U * 50U * 3U, /**< One band payload, bytes.            */
} ew_geom_t;

/** @enum ew_cache_t @brief Tile-cache sizing (fewer cells than bands). */
typedef enum : uint32_t {
  k_ew_cells   = 4U, /**< Cache cells (< 20 bands -> eviction). */
  k_ew_buckets = 8U, /**< Hash buckets.                         */
} ew_cache_t;

/** @enum ew_console_t @brief Console + hash knobs. */
typedef enum : uint32_t {
  k_ew_uart_baud   = 115200U,     /**< Console baud.                 */
  k_ew_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.       */
  k_ew_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.              */
  k_ew_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value. */
  k_ew_nibble_bits = 4U,          /**< Bits per hex nibble.          */
  k_ew_nibble_mask = 0x0FU,       /**< Low-nibble mask.              */
  k_ew_dec_ten     = 10U,         /**< Hex digit / decimal split.    */
} ew_console_t;

/** @enum ew_layout_t @brief RTA1 on-disk field offsets for the builder. */
typedef enum : uint32_t {
  k_ew_hdr_bytes     = 32U,   /**< Header length.              */
  k_ew_ftr_bytes     = 16U,   /**< Footer length.              */
  k_ew_ftr_magic_off = 12U,   /**< Footer magic "RTAE" offset. */
  k_ew_idx_entry     = 8U,    /**< Index-entry length.         */
  k_ew_byte_mask     = 0xFFU, /**< Low-byte mask.              */
  k_ew_shift_8       = 8U,    /**< One-byte shift.             */
  k_ew_shift_16      = 16U,   /**< Two-byte shift.             */
  k_ew_shift_24      = 24U,   /**< Three-byte shift.           */
} ew_layout_t;

/** @enum ew_colors_t @brief 0x00RRGGBB palette. */
typedef enum : uint32_t {
  k_ew_col_bg = 0x101018U, /**< Framebuffer clear colour (dark slate). */
} ew_colors_t;

/** @enum ew_atlas_t @brief Total raw-atlas byte length (header+bands+index+footer). */
typedef enum : uint32_t {
  k_ew_atlas_cap = (uint32_t)k_ew_hdr_bytes + ((uint32_t)k_ew_bands * (uint32_t)k_ew_band_bytes) +
                   ((uint32_t)k_ew_bands * (uint32_t)k_ew_idx_entry) +
                   (uint32_t)k_ew_ftr_bytes, /**< Backing buffer size, bytes. */
} ew_atlas_t;

/* ===========================================================================
 * Static fixtures (internal SRAM; no panel attached)
 * ===========================================================================
 */

/** @brief Backing store for the hand-built raw RTA1 strip. */
static uint8_t s_atlas[k_ew_atlas_cap];

/** @brief Fixed RGB565 viewport framebuffer in internal SRAM. */
static uint16_t s_framebuffer[(size_t)k_ew_view_h * (size_t)k_ew_view_w];

/** @brief Tile-cache cell storage. */
static uint8_t s_cells[(size_t)k_ew_cells * (size_t)k_ew_band_bytes];

static ra8_tile_key_t      s_keys[(size_t)k_ew_cells];
static ra8_tile_dims_t     s_dims[(size_t)k_ew_cells];
static ra8_keycache_cell_t s_meta[(size_t)k_ew_cells];
static int32_t             s_buckets[(size_t)k_ew_buckets];

static ra8_tileatlas_memstore_t s_store;
static ra8_webtoon_decode_ctx_t s_dctx;
static ra8_tile_cache_t         s_cache;
static ra8_webtoon_t            s_wt;

static const uint8_t k_msg_boot[]    = "ereader-webtoon: boot\r\n";
static const uint8_t k_msg_fail[]    = "ereader-webtoon: FAIL init\r\n";
static const uint8_t k_msg_ferr[]    = "ereader-webtoon: FAIL open\r\n";
static const uint8_t k_msg_pre[]     = "ereader-webtoon: bands=";
static const uint8_t k_msg_steps[]   = " steps=";
static const uint8_t k_msg_skipped[] = " skipped=";
static const uint8_t k_msg_crc[]     = " crc=";
static const uint8_t k_msg_eol[]     = "\r\n";

/* ===========================================================================
 * Console helpers
 * ===========================================================================
 */

/** @brief Emit a byte run on the SCI8 console. */
static void ew_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write((msg), (size_t)(len));
}

/** @brief Print the init-fail banner and spin (board_sim halts on the BKPT). */
static void ew_panic_halt(const uint8_t* msg, uint32_t len)
{
  ew_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void ew_print_hex(uint32_t value)
{
  uint8_t buf[k_ew_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_ew_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_ew_hex_nibbles - 1U - i) * (uint32_t)k_ew_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_ew_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_ew_dec_ten) ? ('0' + nib) : ('A' + (nib - k_ew_dec_ten)));
  }
  ew_print(buf, (uint32_t)k_ew_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void ew_print_uint(uint32_t value)
{
  uint8_t  buf[k_ew_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_ew_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_ew_dec_ten));
    n++;
    value /= (uint32_t)k_ew_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    ew_print(&buf[n - 1U - i], 1U);
  }
}

/* ===========================================================================
 * Synthetic strip builder + render helpers
 * ===========================================================================
 */

/** @brief Deterministic RGB888 pixel encoding its canvas coordinate. */
static void ew_pixel(uint32_t canvas_x, uint32_t canvas_y, uint8_t* out)
{
  out[0] = (uint8_t)(canvas_y & (uint32_t)k_ew_byte_mask);
  out[1] = (uint8_t)((canvas_y >> (uint32_t)k_ew_shift_8) & (uint32_t)k_ew_byte_mask);
  out[2] = (uint8_t)(canvas_x & (uint32_t)k_ew_byte_mask);
}

/** @brief Write a little-endian u16 at @p p. */
static void ew_put_u16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & (uint16_t)k_ew_byte_mask);
  p[1] = (uint8_t)((v >> (uint16_t)k_ew_shift_8) & (uint16_t)k_ew_byte_mask);
}

/** @brief Write a little-endian u32 at @p p. */
static void ew_put_u32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & (uint32_t)k_ew_byte_mask);
  p[1] = (uint8_t)((v >> (uint32_t)k_ew_shift_8) & (uint32_t)k_ew_byte_mask);
  p[2] = (uint8_t)((v >> (uint32_t)k_ew_shift_16) & (uint32_t)k_ew_byte_mask);
  p[3] = (uint8_t)((v >> (uint32_t)k_ew_shift_24) & (uint32_t)k_ew_byte_mask);
}

/** @brief Build the raw (codec 0) RTA1 band-tile atlas; return byte length. */
static uint32_t ew_build_strip(void)
{
  const uint32_t width  = (uint32_t)k_ew_strip_w;
  const uint32_t band_h = (uint32_t)k_ew_band_h;
  const uint32_t height = (uint32_t)k_ew_strip_h;
  const uint32_t bands  = (height + band_h - 1U) / band_h;
  for (uint32_t i = 0U; i < (uint32_t)k_ew_hdr_bytes; i++) {
    s_atlas[i] = 0U;
  }
  s_atlas[0] = 'R';
  s_atlas[1] = 'T';
  s_atlas[2] = 'A';
  s_atlas[3] = '1';
  ew_put_u16(&s_atlas[4], (uint16_t)width);
  ew_put_u16(&s_atlas[6], (uint16_t)height);
  ew_put_u16(&s_atlas[8], (uint16_t)width); /* tile_w == width (band column) */
  ew_put_u16(&s_atlas[10], (uint16_t)band_h);
  s_atlas[12] = (uint8_t)k_ew_bpp;
  s_atlas[13] = (uint8_t)k_ra8_tileatlas_codec_raw;
  ew_put_u32(&s_atlas[16], bands);

  /* Pixel region ends exactly where the index begins (sum of clamped band
   * heights == height), so the index can be written in place. */
  const uint32_t index_off = (uint32_t)k_ew_hdr_bytes + (width * height * (uint32_t)k_ew_bpp);
  uint32_t       off       = (uint32_t)k_ew_hdr_bytes;
  for (uint32_t n = 0U; n < bands; n++) {
    const uint32_t y0 = n * band_h;
    const uint32_t th = ((height - y0) < band_h) ? (height - y0) : band_h;
    ew_put_u32(&s_atlas[index_off + (n * (uint32_t)k_ew_idx_entry)], off);
    ew_put_u32(&s_atlas[index_off + (n * (uint32_t)k_ew_idx_entry) + 4U],
               width * th * (uint32_t)k_ew_bpp);
    for (uint32_t r = 0U; r < th; r++) {
      for (uint32_t x = 0U; x < width; x++) {
        ew_pixel(x, y0 + r, &s_atlas[off]);
        off += (uint32_t)k_ew_bpp;
      }
    }
  }
  off                  = index_off + (bands * (uint32_t)k_ew_idx_entry);
  const uint32_t total = off + (uint32_t)k_ew_ftr_bytes;
  ew_put_u32(&s_atlas[off], index_off);
  ew_put_u32(&s_atlas[off + 4U], bands);
  ew_put_u32(&s_atlas[off + 8U], total);
  const uint32_t mag = off + (uint32_t)k_ew_ftr_magic_off;
  s_atlas[mag + 0U]  = 'R';
  s_atlas[mag + 1U]  = 'T';
  s_atlas[mag + 2U]  = 'A';
  s_atlas[mag + 3U]  = 'E';
  return total;
}

/** @brief Webtoon blit sink -> ra8_gfx software blit (RGB888 -> RGB565). */
static ra8_err_t ew_blit(void*          ctx,
                         const uint8_t* pixels,
                         uint16_t       src_w,
                         uint16_t       src_h,
                         uint8_t        bpp,
                         int32_t        dst_x,
                         int32_t        dst_y)
{
  (void)ctx;
  (void)bpp; /* strip is RGB888 by construction */
  return ra8_gfx_blit(pixels, src_w, src_h, k_ra8_gfx_format_rgb888, dst_x, dst_y);
}

/** @brief FNV-1a-32 fold of the whole framebuffer into a running hash. */
static uint32_t ew_hash_frame(uint32_t hsh)
{
  const uint8_t* p = (const uint8_t*)s_framebuffer;
  const size_t   n = sizeof(s_framebuffer);
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_ew_fnv_prime;
  }
  return hsh;
}

/* ===========================================================================
 * Boot + main
 * ===========================================================================
 */

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void ew_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    ew_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    ew_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    ew_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_ew_uart_baud) != k_ra8_ok) {
    ew_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/** @brief Wire the memstore/decoder/cache and open the webtoon strip. */
static void ew_open_or_halt(uint32_t total)
{
  s_store = (ra8_tileatlas_memstore_t){.buf = s_atlas, .cap = (size_t)k_ew_atlas_cap, .len = total};
  s_dctx  = (ra8_webtoon_decode_ctx_t){.pread       = ra8_tileatlas_memstore_pread,
                                       .pread_ctx   = &s_store,
                                       .info        = {},
                                       .scratch     = nullptr,
                                       .scratch_cap = 0U};
  if (ra8_tileatlas_parse(ra8_tileatlas_memstore_pread, &s_store, total, &s_dctx.info) !=
      k_ra8_ok) {
    ew_panic_halt(k_msg_ferr, (uint32_t)sizeof(k_msg_ferr) - 1U);
  }
  const ra8_tile_cache_cfg_t ccfg = {.cell_mem     = s_cells,
                                     .cell_bytes   = (uint32_t)k_ew_band_bytes,
                                     .cell_count   = (uint32_t)k_ew_cells,
                                     .meta         = s_meta,
                                     .keys         = s_keys,
                                     .dims         = s_dims,
                                     .buckets      = s_buckets,
                                     .bucket_count = (uint32_t)k_ew_buckets,
                                     .decode       = ra8_webtoon_tile_decode,
                                     .decode_ctx   = &s_dctx};
  if (ra8_tile_cache_init(&s_cache, &ccfg) != k_ra8_ok) {
    ew_panic_halt(k_msg_ferr, (uint32_t)sizeof(k_msg_ferr) - 1U);
  }
  const ra8_webtoon_cfg_t wcfg = {.pread      = ra8_tileatlas_memstore_pread,
                                  .pread_ctx  = &s_store,
                                  .atlas_size = total,
                                  .cache      = &s_cache,
                                  .image_id   = 1U,
                                  .viewport_w = (uint16_t)k_ew_view_w,
                                  .viewport_h = (uint16_t)k_ew_view_h,
                                  .blit       = ew_blit,
                                  .blit_ctx   = nullptr};
  if (ra8_webtoon_open(&s_wt, &wcfg) != k_ra8_ok) {
    ew_panic_halt(k_msg_ferr, (uint32_t)sizeof(k_msg_ferr) - 1U);
  }
}

/**
 * @brief Scroll the whole strip; return the folded frame hash + step/skip sums.
 *
 * @param[out] out_steps   Frames rendered.
 * @param[out] out_skipped Total skipped bands across the traversal (must be 0).
 * @return Running FNV-1a-32 over every rendered frame.
 */
static uint32_t ew_scroll_all(uint32_t* out_steps, uint32_t* out_skipped)
{
  uint32_t hsh     = (uint32_t)k_ew_fnv_offset;
  uint32_t steps   = 0U;
  uint32_t skipped = 0U;
  bool     done    = false;
  for (int32_t target = 0; !done; target += (int32_t)k_ew_scroll_step) {
    int32_t t = (target >= s_wt.max_scroll) ? s_wt.max_scroll : target;
    if (t == s_wt.max_scroll) {
      done = true;
    }
    (void)ra8_webtoon_scroll_by(&s_wt, t - s_wt.scroll_y);
    (void)ra8_gfx_clear((uint32_t)k_ew_col_bg);
    ra8_webtoon_render_stats_t st = {};
    (void)ra8_webtoon_render(&s_wt, &st);
    skipped += (uint32_t)st.skipped;
    hsh = ew_hash_frame(hsh);
    steps++;
  }
  *out_steps   = steps;
  *out_skipped = skipped;
  return hsh;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: build a tall webtoon strip, scroll it all, hash + print.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The webtoon scroll banner is emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  ew_setup_or_halt();
  ra8_isr_globals_enable();
  ew_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  if (ra8_gfx_init(s_framebuffer,
                   (uint16_t)k_ew_view_w,
                   (uint16_t)k_ew_view_h,
                   k_ra8_gfx_format_rgb565) != k_ra8_ok) {
    ew_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }

  const uint32_t total = ew_build_strip();
  ew_open_or_halt(total);

  uint32_t       steps   = 0U;
  uint32_t       skipped = 0U;
  const uint32_t crc     = ew_scroll_all(&steps, &skipped);

  ew_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  ew_print_uint((uint32_t)s_wt.band_count);
  ew_print(k_msg_steps, (uint32_t)sizeof(k_msg_steps) - 1U);
  ew_print_uint(steps);
  ew_print(k_msg_skipped, (uint32_t)sizeof(k_msg_skipped) - 1U);
  ew_print_uint(skipped);
  ew_print(k_msg_crc, (uint32_t)sizeof(k_msg_crc) - 1U);
  ew_print_hex(crc);
  ew_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
