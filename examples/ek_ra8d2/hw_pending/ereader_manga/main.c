/**
 * @file examples/ek_ra8d2/hw_pending/ereader_manga/main.c
 * @brief Headless gate for the #231 full-resolution image path: import-time
 *        transcode of an in-EPUB PNG page into an RTA1 atlas, then a
 *        decode-on-demand tile render (board_sim / sim-only).
 *
 * @details
 * The end-to-end proof of the #231 producer chain on the real cross-compiled
 * target build:
 *
 *   1. Open a baked all-image EPUB through `ra8_epub_open_streamed` (the
 *      production open path; the archive is windowed, never copied whole).
 *   2. `ra8_epub_tile_binder_import("page1.png")`: stream the manifest
 *      image's encoded bytes through the bounded transcode producer
 *      (`ra8_tileatlas_produce`) into an SDRAM memstore -- the 512x512
 *      grayscale page (256 KiB decoded) is transcoded via one 64 KiB band
 *      at a time, never resident whole -- and register the finished
 *      deflate-coded atlas with the tile binder.
 *   3. Render a 160x120 centre crop by paging the four covering tiles
 *      through a deliberately tiny 2-cell tile cache (decode-on-demand with
 *      forced evictions), expanding gray8 to RGB565.
 *   4. Print the atlas geometry + byte count + an FNV-1a-32 framebuffer
 *      hash on the SCI8 J-Link OB console:
 *
 *        `ereader-manga-hil: import 512x512 tiles=16 atlas=<N> crc=<8hex> ok`
 *
 * Every stage is a deterministic integer pipeline (PNG inflate, tile
 * DEFLATE, gray->RGB565 pack), so the banner is identical on host tests,
 * board_sim, and silicon; the host twin (tests/test_app_ereader_manga.c)
 * asserts the same numbers.
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

#include "manga_hil_fixture.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_epub.h"
#include "ra8_epub_img_tiles.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_sdramc.h"
#include "ra8_tile_cache.h"
#include "ra8_tileatlas.h"
#include "ra8_tileatlas_produce.h"
#include "ra8_time.h"

/** @enum mg_consts_t @brief Framebuffer / import / cache / hash knobs. */
typedef enum : uint32_t {
  k_mg_fb_w        = 160U,          /**< Framebuffer width, pixels.       */
  k_mg_fb_h        = 120U,          /**< Framebuffer height, pixels.      */
  k_mg_uart_baud   = 115200U,       /**< Console baud.                    */
  k_mg_img_edge    = 512U,          /**< Fixture page edge, pixels.       */
  k_mg_tile_edge   = 128U,          /**< Import tile edge, pixels.        */
  k_mg_cap_edge    = 1024U,         /**< Import budget cap, pixels.       */
  k_mg_cell_bytes  = 16384U,        /**< Cache cell = one gray8 tile.     */
  k_mg_cells       = 2U,            /**< Deliberately tiny (evictions).   */
  k_mg_buckets     = 8U,            /**< Cache hash buckets.              */
  k_mg_scratch     = 18688U,        /**< stored_bound(16384) staging.     */
  k_mg_work_bytes  = 1024U * 1024U, /**< SDRAM producer work arena.       */
  k_mg_store_bytes = 512U * 1024U,  /**< SDRAM atlas memstore.            */
  k_mg_image_id    = 1U,            /**< Binder image id for the page.    */
  k_mg_view_x0     = 176U,          /**< Crop left edge (centre 160x120). */
  k_mg_view_y0     = 196U,          /**< Crop top edge (centre 160x120).  */
  k_mg_fnv_offset  = 2166136261U,   /**< FNV-1a-32 offset basis.          */
  k_mg_fnv_prime   = 16777619U,     /**< FNV-1a-32 prime.                 */
  k_mg_hex_nibbles = 8U,            /**< Hex digits in a 32-bit value.    */
  k_mg_nibble_bits = 4U,            /**< Bits per hex nibble.             */
  k_mg_nibble_mask = 0x0FU,         /**< Low-nibble mask.                 */
  k_mg_dec_ten     = 10U,           /**< Hex digit / decimal split.       */
  k_mg_rgb565_r_sh = 11U,           /**< RGB565 red field shift.          */
  k_mg_rgb565_g_sh = 5U,            /**< RGB565 green field shift.        */
  k_mg_gray_r_drop = 3U,            /**< gray8 -> 5-bit field shift.      */
  k_mg_gray_g_drop = 2U,            /**< gray8 -> 6-bit field shift.      */
} mg_consts_t;

/** @enum mg_colors_t @brief 0x00RRGGBB palette. */
typedef enum : uint32_t {
  k_mg_col_bg = 0x202028U, /**< Framebuffer clear colour (dark slate). */
} mg_colors_t;

/** @brief Fixed RGB565 framebuffer in internal SRAM (no panel attached). */
static uint16_t s_framebuffer[(size_t)k_mg_fb_h * (size_t)k_mg_fb_w];
/** @brief The open streamed book (large -- file-scope, not on the stack). */
static ra8_epub_book_t s_book;
/** @brief The tile binder (cache + sources). */
static ra8_epub_tile_binder_t s_binder;
/** @brief Tile-cache cell storage (two gray8 128x128 tiles). */
static uint8_t s_cell_mem[(size_t)k_mg_cells * (size_t)k_mg_cell_bytes];
/** @brief Tile-cache key storage. */
static ra8_tile_key_t s_keys[k_mg_cells];
/** @brief Tile-cache dim descriptors. */
static ra8_tile_dims_t s_dims[k_mg_cells];
/** @brief Tile-cache link metadata. */
static ra8_keycache_cell_t s_meta[k_mg_cells];
/** @brief Tile-cache hash buckets. */
static int32_t s_buckets[k_mg_buckets];
/** @brief Stored-tile staging for the deflate atlas decode-on-miss. */
static uint8_t s_scratch[k_mg_scratch];
/** @brief Producer work arena (SDRAM: the transcode working set). */
[[gnu::section(".sdram_data")]] static uint8_t s_work[k_mg_work_bytes];
/** @brief Atlas memstore backing (SDRAM: where the produced atlas lives). */
[[gnu::section(".sdram_data")]] static uint8_t s_store_buf[k_mg_store_bytes];
/** @brief The atlas memstore (sink + pread pair over `s_store_buf`). */
static ra8_tileatlas_memstore_t s_store;

static const uint8_t k_msg_boot[]  = "ereader-manga-hil: boot\r\n";
static const uint8_t k_msg_fail[]  = "ereader-manga-hil: FAIL init\r\n";
static const uint8_t k_msg_book[]  = "ereader-manga-hil: FAIL book\r\n";
static const uint8_t k_msg_imp[]   = "ereader-manga-hil: FAIL import\r\n";
static const uint8_t k_msg_tile[]  = "ereader-manga-hil: FAIL tiles\r\n";
static const uint8_t k_msg_pre[]   = "ereader-manga-hil: import ";
static const uint8_t k_msg_x[]     = "x";
static const uint8_t k_msg_tiles[] = " tiles=";
static const uint8_t k_msg_atlas[] = " atlas=";
static const uint8_t k_msg_crc[]   = " crc=";
static const uint8_t k_msg_ok[]    = " ok\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void mg_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (board_sim halts on the BKPT). */
static void mg_panic_halt(const uint8_t* msg, uint32_t len)
{
  mg_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void mg_print_hex(uint32_t value)
{
  uint8_t buf[k_mg_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_mg_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_mg_hex_nibbles - 1U - i) * (uint32_t)k_mg_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_mg_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_mg_dec_ten) ? ('0' + nib) : ('A' + (nib - k_mg_dec_ten)));
  }
  mg_print(buf, (uint32_t)k_mg_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void mg_print_uint(uint32_t value)
{
  uint8_t  buf[k_mg_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_mg_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_mg_dec_ten));
    n++;
    value /= (uint32_t)k_mg_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    mg_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief Seek+read callback over the baked resident EPUB fixture. */
static size_t mg_media_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  (void)ctx;
  if (offset >= (uint64_t)k_manga_epub_len) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)k_manga_epub_len - offset;
  const size_t   take  = (len > (size_t)avail) ? (size_t)avail : len;
  (void)memcpy(buf, &k_manga_epub[(size_t)offset], take);
  return take;
}

/** @brief FNV-1a-32 over the rendered framebuffer bytes. */
static uint32_t mg_framebuffer_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_framebuffer;
  const size_t   n   = sizeof(s_framebuffer);
  uint32_t       hsh = (uint32_t)k_mg_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_mg_fnv_prime;
  }
  return hsh;
}

/**
 * @brief Blit one pinned gray8 tile's viewport overlap into the framebuffer.
 * @param[in] tile The pinned tile view.
 * @param[in] tx   Tile column of @p tile.
 * @param[in] ty   Tile row of @p tile.
 * @pre @p tile was returned pinned by the binder.
 * @pre The framebuffer geometry matches the k_mg_view crop.
 * @post The overlapping pixels are packed RGB565 into the framebuffer.
 * @post No tile-cache state changes (the caller unpins).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void mg_blit_tile(const ra8_tile_t* tile, uint16_t tx, uint16_t ty)
{
  const uint32_t px0 = (uint32_t)tx * (uint32_t)k_mg_tile_edge;
  const uint32_t py0 = (uint32_t)ty * (uint32_t)k_mg_tile_edge;
  for (uint32_t r = 0U; r < (uint32_t)tile->height; r++) {
    const uint32_t iy = py0 + r;
    if ((iy < (uint32_t)k_mg_view_y0) || (iy >= ((uint32_t)k_mg_view_y0 + (uint32_t)k_mg_fb_h))) {
      continue;
    }
    for (uint32_t c = 0U; c < (uint32_t)tile->width; c++) {
      const uint32_t ix = px0 + c;
      if ((ix < (uint32_t)k_mg_view_x0) || (ix >= ((uint32_t)k_mg_view_x0 + (uint32_t)k_mg_fb_w))) {
        continue;
      }
      const uint8_t  g   = tile->pixels[(r * (uint32_t)tile->width) + c];
      const uint16_t pix = (uint16_t)(((uint16_t)(g >> k_mg_gray_r_drop) << k_mg_rgb565_r_sh) |
                                      ((uint16_t)(g >> k_mg_gray_g_drop) << k_mg_rgb565_g_sh) |
                                      (uint16_t)(g >> k_mg_gray_r_drop));
      s_framebuffer[((iy - (uint32_t)k_mg_view_y0) * (uint32_t)k_mg_fb_w) +
                    (ix - (uint32_t)k_mg_view_x0)] = pix;
    }
  }
}

/**
 * @brief Page every tile covering the viewport through the binder and blit.
 * @return Whether every covering tile decoded and blitted.
 * @retval true  The crop is fully rendered.
 * @retval false A tile fetch failed.
 * @pre The binder holds the imported page under ::k_mg_image_id.
 * @pre The framebuffer is bound and cleared.
 * @post On true the viewport pixels are in the framebuffer.
 * @post Every pinned tile was released.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool mg_render_viewport(void)
{
  const uint16_t tx0 = (uint16_t)(k_mg_view_x0 / k_mg_tile_edge);
  const uint16_t ty0 = (uint16_t)(k_mg_view_y0 / k_mg_tile_edge);
  const uint16_t tx1 = (uint16_t)((k_mg_view_x0 + k_mg_fb_w - 1U) / k_mg_tile_edge);
  const uint16_t ty1 = (uint16_t)((k_mg_view_y0 + k_mg_fb_h - 1U) / k_mg_tile_edge);
  for (uint16_t ty = ty0; ty <= ty1; ty++) {
    for (uint16_t tx = tx0; tx <= tx1; tx++) {
      ra8_tile_t tile = {};
      if (ra8_epub_tile_binder_get(&s_binder, (uint32_t)k_mg_image_id, tx, ty, &tile) != k_ra8_ok) {
        return false;
      }
      mg_blit_tile(&tile, tx, ty);
      if (ra8_epub_tile_binder_put(&s_binder, tile.pixels) != k_ra8_ok) {
        return false;
      }
    }
  }
  return true;
}

/**
 * @brief Import the manifest page through the transcode producer.
 * @param[out] out_info Receives the produced atlas geometry.
 * @return Whether the import registered the page.
 * @retval true  The page's tiles are servable.
 * @retval false Open/import failed.
 * @pre The book is open and the binder initialised.
 * @pre The SDRAM work arena + memstore are mapped.
 * @post On true `out_info` describes the atlas in the memstore.
 * @post On false the binder is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool mg_import_page(ra8_tileatlas_info_t* out_info)
{
  s_store = (ra8_tileatlas_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_epub_atlas_import_cfg_t cfg = {
    .tile_w     = (uint16_t)k_mg_tile_edge,
    .tile_h     = (uint16_t)k_mg_tile_edge,
    .codec      = (uint8_t)k_ra8_tileatlas_codec_deflate,
    .max_width  = (uint16_t)k_mg_cap_edge,
    .max_height = (uint16_t)k_mg_cap_edge,
    .work       = s_work,
    .work_cap   = sizeof(s_work),
    .store      = {.sink      = ra8_tileatlas_memstore_sink,
                   .sink_ctx  = &s_store,
                   .pread     = ra8_tileatlas_memstore_pread,
                   .pread_ctx = &s_store},
  };
  if (ra8_epub_tile_binder_import(&s_binder, &s_book, "page1.png", (uint32_t)k_mg_image_id, &cfg) !=
      k_ra8_ok) {
    return false;
  }
  return ra8_epub_tile_binder_info(&s_binder, (uint32_t)k_mg_image_id, out_info) == k_ra8_ok;
}

/** @brief Bring up clocks/MSTP/time/SDRAM + the SCI8 console; halt on failure. */
static void mg_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_mg_uart_baud) != k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_sdramc_init() != k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: streamed open -> import-transcode -> tile render -> CRC.
 * @return Never returns.
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The import-geometry + render-hash banner is emitted; loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  mg_setup_or_halt();
  ra8_isr_globals_enable();
  mg_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  if (ra8_gfx_init(s_framebuffer,
                   (uint16_t)k_mg_fb_w,
                   (uint16_t)k_mg_fb_h,
                   k_ra8_gfx_format_rgb565) != k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  (void)ra8_gfx_clear((uint32_t)k_mg_col_bg);

  const ra8_epub_stream_media_t media = {.read = mg_media_read,
                                         .ctx  = nullptr,
                                         .size = (uint64_t)k_manga_epub_len};
  if (ra8_epub_open_streamed(&media, "manga.epub", &s_book) != k_ra8_ok) {
    mg_panic_halt(k_msg_book, (uint32_t)sizeof(k_msg_book) - 1U);
  }

  const ra8_tile_cache_cfg_t storage = {.cell_mem     = s_cell_mem,
                                        .cell_bytes   = (uint32_t)k_mg_cell_bytes,
                                        .cell_count   = (uint32_t)k_mg_cells,
                                        .meta         = s_meta,
                                        .keys         = s_keys,
                                        .dims         = s_dims,
                                        .buckets      = s_buckets,
                                        .bucket_count = (uint32_t)k_mg_buckets};
  if (ra8_epub_tile_binder_init(&s_binder, &storage, s_scratch, (uint32_t)sizeof(s_scratch)) !=
      k_ra8_ok) {
    mg_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }

  ra8_tileatlas_info_t info = {};
  if (!mg_import_page(&info)) {
    mg_panic_halt(k_msg_imp, (uint32_t)sizeof(k_msg_imp) - 1U);
  }
  if (!mg_render_viewport()) {
    mg_panic_halt(k_msg_tile, (uint32_t)sizeof(k_msg_tile) - 1U);
  }

  mg_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  mg_print_uint((uint32_t)info.width);
  mg_print(k_msg_x, (uint32_t)sizeof(k_msg_x) - 1U);
  mg_print_uint((uint32_t)info.height);
  mg_print(k_msg_tiles, (uint32_t)sizeof(k_msg_tiles) - 1U);
  mg_print_uint(info.tile_count);
  mg_print(k_msg_atlas, (uint32_t)sizeof(k_msg_atlas) - 1U);
  mg_print_uint(info.total_size);
  mg_print(k_msg_crc, (uint32_t)sizeof(k_msg_crc) - 1U);
  mg_print_hex(mg_framebuffer_hash());
  mg_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
