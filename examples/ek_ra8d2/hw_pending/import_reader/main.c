/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_pending/import_reader/main.c
 * @brief End-to-end on-import EPUB -> .rabook compile + cache + read (#151).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The on-silicon proof of the self-contained-appliance flow specified in issue
 * #151: drop a raw `.epub` on the SD card and the device "just works." This app
 * mounts a FAT volume on a micro-SD over `ra8_sdmmc_spi`, then drives the
 * `ra8_rabook_import` cache manager wired to its PRODUCTION compile adapter
 * (`ra8_rabook_import_compile_adapter`, which STREAMS the source through a
 * bounded `ra8_vmem` page cache -- no whole-file load buffer, #230 -- into
 * `ra8_rabook_compile_from_epub`):
 *
 *   1. First open of `BOOK.EPB` is a cache MISS -> the importer streams the
 *      source through CRC-32 to key the entry, compiles the EPUB into the flat
 *      `RABOOK1` body, and writes it crash-safely (temp + rename) to the
 *      root-level 8.3 cache name `XXXXXXXX.RBK` plus a freshness marker
 *      `XXXXXXXX.RBM`. The outcome reads back `compiled`.
 *   2. The app confirms the cache file now exists on the card (the `.rabook`
 *      appeared).
 *   3. A second open of the SAME source is a cache HIT -> the freshness marker
 *      matches, so the importer returns the cached path WITHOUT recompiling
 *      (outcome `hit`, the compiler seam is never invoked).
 *   4. The reader then reads the cached `.rabook` back off the card and walks
 *      it: `ra8_book_validate` accepts the bare (uncompressed) blob, and the app
 *      logs the book title, chapter count, and each chapter's plain-text length.
 *
 * On success it prints exactly
 * `import_reader: miss->compile->cache->hit->read PASS`; any failed step prints
 * `import_reader: FAIL <stage>` and parks the core. The ra8_emulator headless gate
 * (and a future HIL runner) scrape for that PASS line.
 *
 * @note The cache uses the library's current v1 ROOT-level 8.3 name layout. The
 *       dedicated `/RABOOK/` subdirectory layout from the issue is BLOCKED on
 *       FAT subdirectory write (`ra8_fs_mkdir`, tracked by #151/#165) and is the
 *       next increment -- do not attempt it here.
 * @note The compile working arenas live in external SDRAM (the issue's
 *       conversion-arena tenant of #147); they are sized for a small text book.
 *       A worst-case (image-heavy) book needs the larger ~24-32 MiB budget the
 *       issue specifies, also carved from SDRAM.
 *
 * Required external hardware (on-bench): Digilent PMOD MicroSD (part 410-380)
 * in Pmod2 (J25) with a microSD card carrying a `BOOK.EPB` text EPUB. Under
 * ra8_emulator attach an image with `--sd <image>` (build one with
 * `tools/mkfontimg <book.epub> card.img BOOK.EPB`).
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_book.h"
#include "ra8_cgc.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_rabook_compile.h"
#include "ra8_rabook_import.h"
#include "ra8_rabook_import_compiler.h"
#include "ra8_rabook_pipeline.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_time.h"
#include "ra8_vmem.h"

/* =============================================================================
 * Tunables
 * =============================================================================
 */

/**
 * @enum imp_config_t
 * @brief Compile-time settings for the on-import compile + cache + read demo.
 */
typedef enum : uint32_t {
  k_imp_uart_baud        = 115200U, /**< J-Link OB CDC console baud.          */
  k_imp_spi_channel      = 0U,      /**< Pmod2 / J25 SCI0 Simple-SPI.         */
  k_imp_format_version   = 1U,      /**< RABOOK1 on-disk format stamp.        */
  k_imp_importer_version = 1U,      /**< Importer/compiler version stamp.     */
  k_imp_scratch_cap      = 4096U,   /**< Source-CRC streaming chunk (bytes).  */
  k_imp_path_cap         = 16U,     /**< Cache-path buffer (>= name cap).     */
  k_imp_dec_base         = 10U,     /**< Radix for integer-to-ASCII.          */
  k_imp_hex_nibbles      = 8U,      /**< Hex digits in a 32-bit value.        */
  k_imp_nibble_bits      = 4U,      /**< Bits per hex nibble.                 */
  k_imp_nibble_mask      = 0x0FU,   /**< Low-nibble mask.                     */
  k_imp_str_max          = 256U,    /**< Bound on a logged C-string (Rule 2). */
} imp_config_t;

/**
 * @enum imp_buf_cap_t
 * @brief SDRAM working-arena capacities (sized for a small text book).
 */
typedef enum : uint32_t {
  k_imp_cache_frame_bytes = 4096U,              /**< Source page-cache frame size (bytes).     */
  k_imp_cache_frames      = 64U,                /**< Fixed frame budget: 64 x 4 KiB = 256 KiB. */
  k_imp_cache_buckets     = 128U,               /**< Page-cache hash buckets (~2x frames).     */
  k_imp_chapter_cap       = 32U,                /**< Max chapters.                             */
  k_imp_node_cap          = 2048U,              /**< Max DOM nodes.                            */
  k_imp_attr_cap          = 512U,               /**< Max attribute records.                    */
  k_imp_style_cap         = 16U,                /**< Max stylesheets.                          */
  k_imp_image_cap         = 32U,                /**< Max image descriptors.                    */
  k_imp_string_cap        = 64U * 1024U,        /**< String-pool capacity (bytes).             */
  k_imp_imgpool_cap       = 1024U * 1024U,      /**< Image-pool capacity (bytes).              */
  k_imp_out_cap           = 512U * 1024U,       /**< Output-blob capacity (bytes).             */
  k_imp_xhtml_cap         = 64U * 1024U,        /**< Chapter XHTML scratch (bytes).            */
  k_imp_css_cap           = 16U * 1024U,        /**< Stylesheet load scratch (bytes).          */
  k_imp_imgraw_cap        = 2U * 1024U * 1024U, /**< Raw cover/image scratch (bytes).          */
  k_imp_arena_cap         = 4U * 1024U * 1024U, /**< stb_image bump arena (bytes).             */
  k_imp_gray_cap          = 2U * 1024U * 1024U, /**< Gray downscale scratch (pixels).          */
  k_imp_readback_cap      = 512U * 1024U,       /**< Cached-.rabook read buffer.               */
} imp_buf_cap_t;

/* =============================================================================
 * Pinout (Pmod2 SPI for SD card)
 * =============================================================================
 */

/** @brief Pmod2 SPI pins (J25) -- SCI0 Simple-SPI; CS held by GPIO. */
static const ra8_port_pin_t k_imp_pin_sck  = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck;
static const ra8_port_pin_t k_imp_pin_cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo;
static const ra8_port_pin_t k_imp_pin_copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi;
static const ra8_port_pin_t k_imp_pin_cs   = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs;

/** @brief Root-level 8.3 source name the appliance imports. */
static const char k_imp_epub_path[] = "BOOK.EPB";

/* =============================================================================
 * SDRAM working storage (no heap; NASA Rule 3). All NOLOAD .sdram_data.
 * =============================================================================
 */

/** @brief Streaming CRC chunk for the source-key pass. */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_imp_scratch[k_imp_scratch_cap];

/** @brief Fixed frame pool the streamed source is paged through (#230). */
[[gnu::section(".sdram_data"),
  gnu::aligned(8)]] static uint8_t s_imp_cache_frames[k_imp_cache_frames * k_imp_cache_frame_bytes];

/** @brief Per-frame metadata for the source page cache. */
static ra8_vmem_frame_t s_imp_cache_meta[k_imp_cache_frames];

/** @brief Per-frame key storage for the source page cache. */
static ra8_vmem_key_t s_imp_cache_keys[k_imp_cache_frames];

/** @brief Hash-bucket heads for the source page cache. */
static int32_t s_imp_cache_buckets[k_imp_cache_buckets];

/** @brief Source page cache; re-initialised by the adapter per compile. */
static ra8_vmem_t s_imp_cache;

/** @brief Open-book storage owned across the compile. */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static ra8_epub_book_t s_imp_epub;

/** @brief RABOOK1 builder arenas (one per table + the two pools + output). */
[[gnu::section(".sdram_data"),
  gnu::aligned(8)]] static ra8_book_chapter_t s_imp_chapters[k_imp_chapter_cap];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static ra8_book_node_t s_imp_nodes[k_imp_node_cap];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static ra8_book_attr_t s_imp_attrs[k_imp_attr_cap];
[[gnu::section(".sdram_data"),
  gnu::aligned(8)]] static ra8_book_stylesheet_t                s_imp_styles[k_imp_style_cap];
[[gnu::section(".sdram_data"),
  gnu::aligned(8)]] static ra8_book_image_t                     s_imp_images[k_imp_image_cap];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static char    s_imp_strpool[k_imp_string_cap];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_imp_imgpool[k_imp_imgpool_cap];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_imp_out[k_imp_out_cap];

/** @brief Pipeline scratch (XHTML load + stylesheet load + image decode + gray downscale). */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_imp_xhtml[k_imp_xhtml_cap];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_imp_image_raw[k_imp_imgraw_cap];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_imp_img_scratch[k_imp_arena_cap];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_imp_gray[k_imp_gray_cap];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static char    s_imp_css[k_imp_css_cap];

/** @brief Cached-`.rabook` read-back buffer (4-byte aligned for the accessors). */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_imp_readback[k_imp_readback_cap];

/* =============================================================================
 * Static message strings (ASCII-only per project policy)
 * =============================================================================
 */

static const uint8_t k_msg_boot[]    = "import_reader: boot\r\n";
static const uint8_t k_msg_card_ok[] = "import_reader: card ready\r\n";
static const uint8_t k_msg_mounted[] = "import_reader: volume mounted\r\n";
static const uint8_t k_msg_miss[]    = "import_reader: first open MISS -> compiled, cache=";
static const uint8_t k_msg_hit[]     = "import_reader: second open HIT (no recompile), cache=";
static const uint8_t k_msg_cbytes[]  = "import_reader: cache bytes=";
static const uint8_t k_msg_title[]   = "import_reader: title=";
static const uint8_t k_msg_chaps[]   = " chapters=";
static const uint8_t k_msg_ch[]      = "import_reader: ch";
static const uint8_t k_msg_textlen[] = " textlen=";
static const uint8_t k_msg_eol[]     = "\r\n";
static const uint8_t k_msg_pass[]    = "import_reader: miss->compile->cache->hit->read PASS\r\n";
static const uint8_t k_msg_fail[]    = "import_reader: FAIL ";

/* =============================================================================
 * Console output helpers
 * =============================================================================
 */

/**
 * @brief Write a byte run on the J-Link OB console.
 * @param[in] msg Bytes to emit (non-NULL, length @p len).
 * @param[in] len Byte count to emit.
 * @pre The BSP console is initialised.
 * @pre @p msg points to at least @p len readable bytes.
 * @post The bytes are queued to the console.
 * @post No other state is modified.
 * @note Not thread-safe; single-threaded app context.
 * @since 0.1.0
 */
static void imp_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Emit a NUL-terminated literal (length via sizeof at the call site). */
#define IMP_PUTS(lit) imp_print((lit), (uint32_t)sizeof(lit) - 1U)

/**
 * @brief Emit a NUL-terminated C-string, bounded to @ref k_imp_str_max bytes.
 * @param[in] str NUL-terminated string to emit (non-NULL).
 * @pre The console is initialised.
 * @pre @p str is NUL-terminated.
 * @post At most @ref k_imp_str_max bytes are queued to the console.
 * @post No other state is modified.
 * @note Not thread-safe; single-threaded app context.
 * @since 0.1.0
 */
static void imp_print_cstr(const char* str)
{
  uint32_t n = 0U;
  while ((n < (uint32_t)k_imp_str_max) && (str[n] != '\0')) {
    n++;
  }
  imp_print((const uint8_t*)str, n);
}

/**
 * @brief Print a small unsigned integer in decimal.
 * @param[in] value Value to print (0 .. UINT32_MAX).
 * @pre The console is initialised.
 * @pre @p value fits a uint32_t (always true).
 * @post The decimal digits are queued to the console.
 * @post No other state is modified.
 * @note Not thread-safe; single-threaded app context.
 * @since 0.1.0
 */
static void imp_print_uint(uint32_t value)
{
  uint8_t  buf[k_imp_dec_base];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_imp_dec_base)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_imp_dec_base));
    n++;
    value /= (uint32_t)k_imp_dec_base;
  }
  for (uint32_t i = 0U; i < n; i++) {
    imp_print(&buf[n - 1U - i], 1U);
  }
}

/**
 * @brief Print the FAIL banner with a stage tag, then trap and park.
 * @param[in] stage NUL-terminated stage label (non-NULL).
 * @return Never returns.
 * @pre The console is initialised.
 * @pre @p stage is NUL-terminated.
 * @post The FAIL line is queued; the CPU traps then spins in WFI.
 * @post No further application code runs.
 * @note Not thread-safe; terminal panic path. ra8_emulator halts on the BKPT.
 * @since 0.1.0
 */
static void imp_panic(const char* stage)
{
  IMP_PUTS(k_msg_fail);
  imp_print_cstr(stage);
  imp_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);
  (void)ra8_board_uart_console_flush();
  __asm__ volatile("bkpt #0");
  while (true) {
    __asm__ volatile("wfi");
  }
}

/* =============================================================================
 * Hardware bring-up
 * =============================================================================
 */

/**
 * @brief Bring up CGC + SysTick + console SCI; panic on failure.
 * @param[out] out_pclka_hz Cached PCLKA rate (Hz) for the SD transport factory.
 * @return Nothing (panic-halts on failure).
 * @pre Reset_Handler initialised .data/.bss.
 * @pre @p out_pclka_hz is writable.
 * @post On success the console prints and `*out_pclka_hz` holds the PCLKA rate.
 * @post On any failure the CPU is parked after a FAIL diagnostic.
 * @note Not thread-safe; call once from the single-threaded init path.
 * @since 0.1.0
 */
static void imp_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    imp_panic("clocks");
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    imp_panic("clocks");
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) {
    imp_panic("clocks");
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    imp_panic("time");
  }
  if (ra8_board_uart_console_init((uint32_t)k_imp_uart_baud) != k_ra8_ok) {
    imp_panic("console");
  }
  *out_pclka_hz = pclka_hz;
}

/**
 * @brief Build the SCI-SPI transport, run SD identification; panic on failure.
 * @param[in] pclka_hz Live PCLKA rate (Hz) feeding the SCI baud divider.
 * @return Nothing (panic-halts on failure).
 * @pre `ra8_cgc_init` has run and the console SCI is up.
 * @pre @p pclka_hz is the live PCLKA rate.
 * @post On success the SD card is in SPI mode and `card ready` is printed.
 * @post On failure a diagnostic is printed and the CPU is parked.
 * @note Not thread-safe; call once after the clocks + console are up.
 * @since 0.1.0
 */
static void imp_init_card_or_halt(uint32_t pclka_hz)
{
  const ra8_sdmmc_spi_sci_pins_t pins = {
    .sck  = k_imp_pin_sck,
    .cipo = k_imp_pin_cipo,
    .copi = k_imp_pin_copi,
    .cs   = k_imp_pin_cs,
  };
  ra8_sdmmc_spi_transport_t transport = {};
  if (ra8_sdmmc_spi_transport_sci((uint8_t)k_imp_spi_channel, pclka_hz, &pins, &transport) !=
      k_ra8_ok) {
    imp_panic("sd transport");
  }
  if (ra8_sdmmc_spi_init(&transport) != k_ra8_ok) {
    imp_panic("sd init");
  }
  IMP_PUTS(k_msg_card_ok);
}

/**
 * @brief Bind the SD-over-SPI block device into `ra8_fs` and mount the volume.
 * @param[out] out_mount Receives the mounted-volume handle on success.
 * @return Nothing (panic-halts on failure).
 * @pre The SD card is initialised (`ra8_sdmmc_spi_init` succeeded).
 * @pre @p out_mount is writable.
 * @post On success `*out_mount` is a live mount of the card's existing FAT
 *       volume (the card is NOT reformatted -- the source `.epub` is preserved).
 * @post On failure a diagnostic is printed and the CPU is parked.
 * @note Not thread-safe; single-threaded init path only.
 * @since 0.1.0
 */
static void imp_mount_or_halt(ra8_fs_mount_t** out_mount)
{
  ra8_fs_backend_t backend = {};
  if (ra8_sdmmc_spi_bind_fs_backend(&backend) != k_ra8_ok) {
    imp_panic("sd backend");
  }
  if (ra8_fs_mount(&backend, out_mount) != k_ra8_ok) {
    imp_panic("mount");
  }
  IMP_PUTS(k_msg_mounted);
}

/* =============================================================================
 * Importer wiring (production compile adapter behind the DIP seam)
 * =============================================================================
 */

/** @brief Builder arenas, pipeline scratch, stb arena, and the compiler cookie. */
static ra8_rabook_buffers_t             s_imp_bufs;
static ra8_rabook_pipeline_scratch_t    s_imp_scr;
static ra8_img_arena_t                  s_imp_arena;
static ra8_rabook_import_compiler_ctx_t s_imp_cookie;

/**
 * @brief Populate the builder/scratch views + the production compile cookie.
 * @details Binds every caller-owned SDRAM arena into the views the real
 *          compiler needs and the cookie the import adapter forwards --
 *          including the fixed source page-cache storage the adapter streams
 *          the `.epub` through (#230). No buffer is aliased: @p image_raw is
 *          distinct from the @p img_arena / @p gray source the transcode stage
 *          reads (a contract @warning of the scratch struct).
 * @return Nothing.
 * @pre The file-scope SDRAM arenas are defined (always true at TU scope).
 * @pre This is called once before the first import.
 * @post `s_imp_bufs`, `s_imp_scr`, `s_imp_arena` and `s_imp_cookie` reference
 *       the static SDRAM storage.
 * @post No state beyond those four file-scope views is mutated.
 * @note Not thread-safe; single-owner views over shared file-scope arenas.
 * @since 0.1.0
 */
static void imp_build_cookie(void)
{
  s_imp_bufs = (ra8_rabook_buffers_t){
    .chapters       = s_imp_chapters,
    .chapter_cap    = (uint32_t)k_imp_chapter_cap,
    .nodes          = s_imp_nodes,
    .node_cap       = (uint32_t)k_imp_node_cap,
    .attrs          = s_imp_attrs,
    .attr_cap       = (uint32_t)k_imp_attr_cap,
    .stylesheets    = s_imp_styles,
    .stylesheet_cap = (uint32_t)k_imp_style_cap,
    .images         = s_imp_images,
    .image_cap      = (uint32_t)k_imp_image_cap,
    .string_pool    = s_imp_strpool,
    .string_cap     = (uint32_t)k_imp_string_cap,
    .image_pool     = s_imp_imgpool,
    .image_pool_cap = (uint32_t)k_imp_imgpool_cap,
    .out            = s_imp_out,
    .out_cap        = (uint32_t)k_imp_out_cap,
  };
  s_imp_arena = (ra8_img_arena_t){s_imp_img_scratch, sizeof(s_imp_img_scratch), 0U, 0U};
  s_imp_scr   = (ra8_rabook_pipeline_scratch_t){
    .xhtml     = s_imp_xhtml,
    .xhtml_cap = sizeof(s_imp_xhtml),
    .image_raw = s_imp_image_raw,
    .image_cap = sizeof(s_imp_image_raw),
    .img_arena = &s_imp_arena,
    .gray      = s_imp_gray,
    .gray_cap  = (uint32_t)k_imp_gray_cap,
    .css       = s_imp_css,
    .css_cap   = sizeof(s_imp_css),
  };
  s_imp_cookie = (ra8_rabook_import_compiler_ctx_t){
    .epub               = &s_imp_epub,
    .cache              = &s_imp_cache,
    .cache_frames       = s_imp_cache_frames,
    .cache_frame_bytes  = (uint32_t)k_imp_cache_frame_bytes,
    .cache_frame_count  = (uint32_t)k_imp_cache_frames,
    .cache_meta         = s_imp_cache_meta,
    .cache_keys         = s_imp_cache_keys,
    .cache_buckets      = s_imp_cache_buckets,
    .cache_bucket_count = (uint32_t)k_imp_cache_buckets,
    .bufs               = &s_imp_bufs,
    .scr                = &s_imp_scr,
  };
}

/**
 * @brief Build the import config that injects the production compile adapter.
 * @param[in] mount Mounted volume holding the source and the cache.
 * @return A fully-populated config (compile seam = the production adapter).
 * @pre @p mount is a live mount.
 * @pre @ref imp_build_cookie has populated `s_imp_cookie`.
 * @post The returned config's `compile` is the real compiler adapter and its
 *       version stamps gate cache reuse for this firmware build.
 * @post No global state is mutated.
 * @note Not thread-safe; single-threaded init path only.
 * @since 0.1.0
 */
static ra8_rabook_import_cfg_t imp_make_cfg(ra8_fs_mount_t* mount)
{
  ra8_rabook_import_cfg_t cfg = {};
  cfg.mount                   = mount;
  cfg.compile                 = ra8_rabook_import_compile_adapter;
  cfg.compile_ctx             = &s_imp_cookie;
  cfg.scratch                 = s_imp_scratch;
  cfg.scratch_cap             = (uint32_t)k_imp_scratch_cap;
  cfg.format_version          = (uint32_t)k_imp_format_version;
  cfg.importer_version        = (uint32_t)k_imp_importer_version;
  return cfg;
}

/* =============================================================================
 * Cache read-back + book walk
 * =============================================================================
 */

/**
 * @brief Read the whole cached `.rabook` at @p path into @ref s_imp_readback.
 * @param[in]  mount   Mounted volume.
 * @param[in]  path    Root-level 8.3 cache path (non-NULL).
 * @param[out] out_len Receives the byte length read.
 * @return Error code (first failing `ra8_fs` step's code, or k_ra8_ok).
 * @retval k_ra8_ok           The cache was read fully into the buffer.
 * @retval k_ra8_err_no_mem   The cache is larger than the read-back buffer.
 * @retval k_ra8_err_*        An `ra8_fs` open/size/read failure.
 * @pre @p mount is mounted and @p path names a present cache file.
 * @pre @p out_len is writable.
 * @post On k_ra8_ok, `s_imp_readback[0..*out_len)` holds the bare RABOOK1 blob.
 * @post The `ra8_fs` file handle is closed on every return path.
 * @note Not thread-safe; single-threaded app context.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t
imp_read_cache(ra8_fs_mount_t* mount, const char* path, uint32_t* out_len)
{
  ra8_fs_file_t* file = nullptr;
  ra8_err_t      err  = ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t size = 0U;
  err           = ra8_fs_size(file, &size);
  if (err != k_ra8_ok) {
    (void)ra8_fs_close(file);
    return err;
  }
  if (size > (uint32_t)sizeof(s_imp_readback)) {
    (void)ra8_fs_close(file);
    return k_ra8_err_no_mem;
  }
  uint32_t got = 0U;
  err          = ra8_fs_read(file, s_imp_readback, (uint32_t)sizeof(s_imp_readback), &got);
  (void)ra8_fs_close(file);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_len = got;
  return k_ra8_ok;
}

/**
 * @brief Validate the cached blob and log its title, chapter count, and the
 *        plain-text length of every chapter.
 * @param[in] len Byte length of the blob in @ref s_imp_readback.
 * @return Error code.
 * @retval k_ra8_ok           Blob validated and walked.
 * @retval k_ra8_err_*        `ra8_book_validate` / per-chapter walk error.
 * @pre `s_imp_readback[0..len)` was filled by @ref imp_read_cache.
 * @pre @p len is the true readable length.
 * @post On k_ra8_ok the book's title + chapter summary are logged.
 * @post The immutable blob is never modified.
 * @note Not thread-safe; single-threaded app context.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t imp_walk_book(uint32_t len)
{
  const void* base = (const void*)s_imp_readback;
  ra8_err_t   err  = ra8_book_validate(base, (size_t)len);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_book_header_t* hdr = ra8_book_header(base);
  IMP_PUTS(k_msg_title);
  imp_print_cstr(ra8_book_string(base, hdr->title_off));
  IMP_PUTS(k_msg_chaps);
  imp_print_uint(hdr->chapter_count);
  imp_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  for (uint32_t ci = 0U; ci < hdr->chapter_count; ci++) {
    size_t    tlen = 0U;
    ra8_err_t cerr =
      ra8_book_chapter_text(base, ci, (char*)s_imp_xhtml, sizeof(s_imp_xhtml), &tlen);
    if (cerr != k_ra8_ok) {
      return cerr;
    }
    IMP_PUTS(k_msg_ch);
    imp_print_uint(ci);
    IMP_PUTS(k_msg_textlen);
    imp_print_uint((uint32_t)tlen);
    imp_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);
  }
  return k_ra8_ok;
}

/* =============================================================================
 * End-to-end flow
 * =============================================================================
 */

/**
 * @brief Run the import twice (miss then hit), confirm the cache, read + walk.
 * @param[in] mount Mounted volume carrying the source `.epub`.
 * @return Nothing (panic-halts on any failed stage).
 * @pre @p mount is mounted and carries @ref k_imp_epub_path.
 * @pre @ref imp_build_cookie and the SD bring-up have completed.
 * @post On success the PASS banner is printed; on failure the CPU is parked.
 * @post The source `.epub` is never modified; the cache `.rabook` is present.
 * @note Not thread-safe; single-threaded app context.
 * @since 0.1.0
 */
static void imp_run(ra8_fs_mount_t* mount)
{
  ra8_rabook_import_cfg_t     cfg                   = imp_make_cfg(mount);
  ra8_rabook_import_outcome_t out                   = k_ra8_rabook_import_hit;
  char                        path1[k_imp_path_cap] = {};
  char                        path2[k_imp_path_cap] = {};

  /* First open: cache miss -> compile + write-through. */
  if (ra8_rabook_import_open(&cfg, k_imp_epub_path, path1, (uint32_t)sizeof(path1), &out) !=
      k_ra8_ok) {
    imp_panic("import compile");
  }
  if (out != k_ra8_rabook_import_compiled) {
    imp_panic("expected miss");
  }
  IMP_PUTS(k_msg_miss);
  imp_print_cstr(path1);
  imp_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  /* The .rabook now exists on the card. */
  uint32_t cache_len = 0U;
  if (imp_read_cache(mount, path1, &cache_len) != k_ra8_ok) {
    imp_panic("cache read");
  }
  IMP_PUTS(k_msg_cbytes);
  imp_print_uint(cache_len);
  imp_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  /* Second open: fresh cache -> hit, the compiler seam is NOT invoked. */
  if (ra8_rabook_import_open(&cfg, k_imp_epub_path, path2, (uint32_t)sizeof(path2), &out) !=
      k_ra8_ok) {
    imp_panic("import reopen");
  }
  if (out != k_ra8_rabook_import_hit) {
    imp_panic("expected hit");
  }
  IMP_PUTS(k_msg_hit);
  imp_print_cstr(path2);
  imp_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  /* Read the cached book back and walk it. */
  if (imp_read_cache(mount, path2, &cache_len) != k_ra8_ok) {
    imp_panic("cache reread");
  }
  if (imp_walk_book(cache_len) != k_ra8_ok) {
    imp_panic("book walk");
  }
}

/* =============================================================================
 * Main
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: bring up the SD card, mount, import + cache + read, PASS.
 * @return Never returns.
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On a clean run the PASS banner is printed and the CPU loops in WFI.
 * @post On any failure the function prints a FAIL line and halts.
 * @note Not thread-safe; single-threaded app entry.
 * @since 0.1.0
 */
int main(void)
{
  uint32_t pclka_hz = 0U;
  imp_setup_or_halt(&pclka_hz);
  ra8_isr_globals_enable();
  ra8_log_init();
  IMP_PUTS(k_msg_boot);

  imp_init_card_or_halt(pclka_hz);

  ra8_fs_mount_t* mount = nullptr;
  imp_mount_or_halt(&mount);

  imp_build_cookie();
  imp_run(mount);

  IMP_PUTS(k_msg_pass);
  (void)ra8_board_uart_console_flush();

  while (true) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
