/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/pagecache/main.c
 * @brief On-silicon HIL: ra8_reflow pagination-cache round-trip on SD (#117).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * #79 added the import-time pagination cache (`ra8_reflow_cache`): lay a chapter
 * out once, serialise the flattened glyph/page result to a keyed blob, persist
 * it, and on the next open skip the expensive layout by loading the blob. That
 * path was host-tested only; this app runs it on the M85 against a real microSD
 * volume through `ra8_fs` -- the exact storage the e-reader uses.
 *
 * Flow: bring up the Pmod2 microSD over `ra8_sdmmc_spi`, mount an `ra8_fs` volume
 * (formatting FAT32 if blank), self-provision `FONT.OTF` (a baked Latin-1 OTF),
 * then exercise the cache three ways:
 *   1. ROUND-TRIP: lay out a fixed chapter live, `ra8_reflow_cache_serialize` it,
 *      write the blob to SD, read it back, `ra8_reflow_cache_load` it into a fresh
 *      engine, re-serialise, and assert the reloaded blob is byte-for-byte equal
 *      to the live one (the serialised form is the per-page glyph/page data, so
 *      equal blobs prove the layout was restored exactly).
 *   2. INVALIDATE: load the same blob into an engine initialised at a DIFFERENT
 *      font size and assert `k_ra8_err_invalid_state` (the key mismatch is caught;
 *      no stale page is served).
 *   3. RESET-SURVIVAL: `::g_pc_hit` reflects whether the cache file already
 *      existed at boot, so a second boot against a persisted card reports a hit
 *      (served from the on-disk cache without a fresh layout).
 *
 * The HIL gate is memprobe (J-Link / board_sim `--dump-sym`), not the console:
 * an SD app drives the SCI0 Simple-SPI bus, and board_sim folds every SCI channel
 * into one console line, so a SCI8 banner is interleaved with SPI traffic there
 * (the same reason the sibling SD HIL apps gate on SWD globals). The success path
 * advances ::g_pc_heartbeat once per frame and only after every assertion passes;
 * any failure stamps ::g_pc_err and parks without bumping it.
 *
 * Required external hardware (on-bench): Digilent PMOD MicroSD (410-380) in Pmod2
 * (J25) with a microSD inserted. THIS APP MAY FORMAT THE CARD. Under board_sim
 * attach a blank card with `--sd-new 64:fat32` (round-trip + invalidate), and a
 * second run against a `--save-sd` image to prove the cache-hit reset-survival.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "literata_latin1.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_port_utils.h"
#include "ra8_reflow.h"
#include "ra8_reflow_cache.h"
#include "ra8_sci_spi.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_spi.h"
#include "ra8_time.h"

/** @enum pc_consts_t @brief Console / SPI / reflow knobs (no magic numbers). */
typedef enum : uint32_t {
  k_pc_uart_baud   = 115200U,     /**< Console baud.                        */
  k_pc_spi_chan    = 0U,          /**< Pmod2 / J25 SCI0 Simple-SPI.         */
  k_pc_font_cap    = 131072U,     /**< Font read buffer (OTF is ~57 KiB).   */
  k_pc_blob_cap    = 16384U,      /**< Cache-blob buffer capacity, bytes.   */
  k_pc_view_w      = 600U,        /**< Reflow viewport width, px.           */
  k_pc_view_h      = 800U,        /**< Reflow viewport height, px.          */
  k_pc_font_px     = 18U,         /**< Body font size, px (round-trip key). */
  k_pc_font_px_alt = 24U,         /**< Alt font size to force invalidation. */
  k_pc_ink_argb    = 0xFF101010U, /**< Body text colour.                    */
  k_pc_link_argb   = 0xFF2A52BEU, /**< Anchor text colour.                  */
  k_pc_crc_init    = 0xFFFFFFFFU, /**< CRC-32 initial value.                */
  k_pc_crc_poly    = 0xEDB88320U, /**< CRC-32 reflected polynomial.         */
  k_pc_crc_bits    = 8U,          /**< Bits folded per byte.                */
} pc_consts_t;

/** @brief Pmod2 SPI pins (J25) -- SCI0 Simple-SPI; CS held by GPIO. */
static const ra8_port_pin_t k_pc_pin_sck  = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck;
static const ra8_port_pin_t k_pc_pin_cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo;
static const ra8_port_pin_t k_pc_pin_copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi;
static const ra8_port_pin_t k_pc_pin_cs   = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs;

/** @brief Font + cache file paths on the SD volume (8.3, ra8_fs is root-only). */
static const char k_pc_font_path[]  = "FONT.OTF";
static const char k_pc_cache_path[] = "PCACHE.BIN";

/** @brief Fixed chapter laid out + cached (short: sim stb_truetype is slow). */
static const char k_pc_body[] = "<html><body><h1>Cache</h1>"
                                "<p>Pagination cache round-trip on silicon.</p>"
                                "<p>Second paragraph for the break engine.</p></body></html>";

/**
 * @enum pc_err_t
 * @brief Failure-stage codes stamped to ::g_pc_err for SWD / `--dump-sym`.
 */
typedef enum : uint32_t {
  k_pc_err_none     = 0U,  /**< No failure (success path).            */
  k_pc_err_init     = 1U,  /**< CGC / time / console / SPI bring-up.  */
  k_pc_err_card     = 2U,  /**< SD card SPI init.                     */
  k_pc_err_mount    = 3U,  /**< Mount (and format-if-blank).          */
  k_pc_err_font     = 4U,  /**< Provision / read FONT.OTF.            */
  k_pc_err_layout   = 5U,  /**< ra8_reflow_init / layout_chapter.     */
  k_pc_err_ser      = 6U,  /**< ra8_reflow_cache_serialize.           */
  k_pc_err_persist  = 7U,  /**< Write / read-back the blob on SD.     */
  k_pc_err_readback = 8U,  /**< Read-back blob != written blob.       */
  k_pc_err_load     = 9U,  /**< ra8_reflow_cache_load (expected hit). */
  k_pc_err_crc      = 10U, /**< Reloaded layout CRC != live CRC.      */
  k_pc_err_invalid  = 11U, /**< Alt-size load did not invalidate.     */
} pc_err_t;

/** @enum pc_pace_t @brief Idle-loop pacing for the success heartbeat. */
typedef enum : uint32_t {
  k_pc_frame_ms = 100U, /**< Heartbeat period in milliseconds. */
} pc_pace_t;

/* ---- J-Link / board_sim readable diagnostics (memprobe gate) ------------- */

/** @brief First failing stage (::pc_err_t), 0 on success. SWD / `--dump-sym`. */
volatile uint32_t g_pc_err = (uint32_t)k_pc_err_none;
/** @brief 1 if the cache file existed at boot (reset-survival hit). */
volatile uint32_t g_pc_hit = 0U;
/** @brief 1 if the reloaded-cache blob CRC equals the live-layout blob CRC. */
volatile uint32_t g_pc_crc_match = 0U;
/** @brief 1 if loading the blob at the alt font size returned invalid_state. */
volatile uint32_t g_pc_invalidate = 0U;
/** @brief Page count produced by the live layout. */
volatile uint32_t g_pc_pages = 0U;
/** @brief CRC-32 of the live-layout serialised blob. */
volatile uint32_t g_pc_live_crc = 0U;
/** @brief CRC-32 of the reloaded-then-reserialised blob. */
volatile uint32_t g_pc_cache_crc = 0U;
/** @brief Idle heartbeat; advances ONLY after every assertion passes. */
volatile uint32_t g_pc_heartbeat = 0U;

/* ---- Static storage (large pools off the stack) -------------------------- */

/** @brief Font blob read off the card. */
static uint8_t s_font_buf[k_pc_font_cap];
/** @brief Reflow engine (glyph / page / token / text pools live inside). */
static ra8_reflow_t s_engine;
/** @brief Serialised cache blob from the live layout. */
static uint8_t s_live_blob[k_pc_blob_cap];
/** @brief Blob read back off SD (compared to ::s_live_blob). */
static uint8_t s_read_blob[k_pc_blob_cap];
/** @brief Blob re-serialised from the reloaded engine (CRC vs live). */
static uint8_t s_reser_blob[k_pc_blob_cap];
/** @brief SD backend; file-scope so the mount handle may reference it. */
static ra8_fs_backend_t s_backend;

static const uint8_t k_msg_boot[] = "pagecache-hil: boot\r\n";
static const uint8_t k_msg_pass[] = "pagecache-hil: hit=";
static const uint8_t k_msg_ok[]   = " crc_match=1 invalidate=1 PASS\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void pc_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/**
 * @brief Stamp the failure stage and park the CPU (no heartbeat).
 * @param[in] err Failure-stage code (::pc_err_t).
 */
static void pc_fail(uint32_t err)
{
  g_pc_err = err;
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief CRC-32 (reflected, poly 0xEDB88320) over @p data. */
static uint32_t pc_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = (uint32_t)k_pc_crc_init;
  for (size_t i = 0U; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint32_t bit = 0U; bit < (uint32_t)k_pc_crc_bits; bit++) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc                 = (crc >> 1U) ^ ((uint32_t)k_pc_crc_poly & mask);
    }
  }
  return ~crc;
}

/** @brief True if two byte runs of equal length are identical. */
static bool pc_blob_equal(const uint8_t* a, const uint8_t* b, size_t len)
{
  for (size_t i = 0U; i < len; i++) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

/* ---- SPI -> ra8_sdmmc_spi transport adapter (mirror of epub_open) ------ */

/* cppcheck-suppress constParameterCallback -- bound to ra8_sdmmc_spi_transport_t::set_clock, `ra8_err_t (*)(void*, uint32_t)`; constifying ctx would break the binding. */
static ra8_err_t pc_spi_set_clock(void* ctx, uint32_t hz)
{
  const uint32_t pclka_hz = *(const uint32_t*)ctx;
  return ra8_sci_spi_set_clock((uint8_t)k_pc_spi_chan, hz, pclka_hz);
}

/** @brief ra8_sdmmc_spi_transport_t::cs over ra8_gpio (CS active-low). */
static ra8_err_t pc_spi_cs(void* ctx, bool asserted)
{
  (void)ctx;
  return ra8_gpio_write(k_pc_pin_cs, asserted ? k_ra8_level_low : k_ra8_level_high);
}

/** @brief ra8_sdmmc_spi_transport_t::xfer over ra8_sci_spi_xfer. */
static ra8_err_t pc_spi_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  return ra8_sci_spi_xfer((uint8_t)k_pc_spi_chan, tx, rx, len);
}

/** @brief Route Pmod2 SPI pins and claim CS as a GPIO output (idle high). */
[[nodiscard]] static ra8_err_t pc_spi_pins_init(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral(k_pc_pin_sck, k_ra8_psel_sci_async, "pc.sck");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_pc_pin_cipo, k_ra8_psel_sci_async, "pc.cipo");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_pc_pin_copi, k_ra8_psel_sci_async, "pc.copi");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_output_init(k_pc_pin_cs, k_ra8_level_high);
}

/** @brief Bring up CGC + SysTick + console SCI + SPI + CS GPIO; halt on fail. */
static void pc_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) ||
      (ra8_time_init(cpuclk0_hz) != k_ra8_ok)) {
    pc_fail((uint32_t)k_pc_err_init);
  }
  if (ra8_board_uart_console_init((uint32_t)k_pc_uart_baud) != k_ra8_ok) {
    pc_fail((uint32_t)k_pc_err_init);
  }
  if (pc_spi_pins_init() != k_ra8_ok) {
    pc_fail((uint32_t)k_pc_err_init);
  }
  const ra8_sci_spi_cfg_t spi_cfg = {.baud_hz   = (uint32_t)k_ra8_sdmmc_spi_clock_init_hz,
                                     .pclk_hz   = pclka_hz,
                                     .mode      = k_ra8_spi_mode_0,
                                     .lsb_first = false};
  if (ra8_sci_spi_init((uint8_t)k_pc_spi_chan, &spi_cfg) != k_ra8_ok) {
    pc_fail((uint32_t)k_pc_err_init);
  }
  *out_pclka_hz = pclka_hz;
}

/** @brief Init the SD card over SPI; halt with a diagnostic on failure. */
static void pc_init_card_or_halt(uint32_t* pclka_hz)
{
  const ra8_sdmmc_spi_transport_t transport = {.set_clock = pc_spi_set_clock,
                                               .cs        = pc_spi_cs,
                                               .xfer      = pc_spi_xfer,
                                               .ctx       = pclka_hz};
  if (ra8_sdmmc_spi_init(&transport) != k_ra8_ok) {
    pc_fail((uint32_t)k_pc_err_card);
  }
}

/** @brief Mount the card, formatting FAT32 first if blank. Never returns NULL. */
static ra8_fs_mount_t* pc_mount_or_halt(void)
{
  if (ra8_sdmmc_spi_bind_fs_backend(&s_backend) != k_ra8_ok) {
    pc_fail((uint32_t)k_pc_err_mount);
  }
  ra8_fs_mount_t* mount = nullptr;
  if (ra8_fs_mount(&s_backend, &mount) != k_ra8_ok) {
    ra8_fs_format_opts_t opts = {};
    opts.type                 = k_ra8_fs_type_fat32;
    opts.label                = "RAPCACHE";
    if ((ra8_fs_format(&s_backend, &opts) != k_ra8_ok) ||
        (ra8_fs_mount(&s_backend, &mount) != k_ra8_ok)) {
      pc_fail((uint32_t)k_pc_err_mount);
    }
  }
  return mount;
}

/**
 * @brief Provision FONT.OTF if absent, then read it into ::s_font_buf.
 * @param[in]  mount    Mounted SD volume.
 * @param[out] out_len  Receives the font length in bytes.
 */
static void pc_font_or_halt(ra8_fs_mount_t* mount, uint32_t* out_len)
{
  ra8_fs_file_t* file = nullptr;
  if (ra8_fs_open(mount, k_pc_font_path, k_ra8_fs_mode_read, &file) != k_ra8_ok) {
    if (ra8_fs_write_file(mount,
                          k_pc_font_path,
                          g_ra8_font_literata_latin1,
                          (uint32_t)g_ra8_font_literata_latin1_len) != k_ra8_ok) {
      pc_fail((uint32_t)k_pc_err_font);
    }
    if (ra8_fs_open(mount, k_pc_font_path, k_ra8_fs_mode_read, &file) != k_ra8_ok) {
      pc_fail((uint32_t)k_pc_err_font);
    }
  }
  uint32_t got = 0U;
  if ((ra8_fs_read(file, s_font_buf, (uint32_t)k_pc_font_cap, &got) != k_ra8_ok) || (got == 0U)) {
    (void)ra8_fs_close(file);
    pc_fail((uint32_t)k_pc_err_font);
  }
  (void)ra8_fs_close(file);
  *out_len = got;
}

/** @brief Read the cache file off SD into @p buf; return its length (0 if none). */
static uint32_t pc_read_cache(ra8_fs_mount_t* mount, uint8_t* buf, uint32_t cap)
{
  ra8_fs_file_t* file = nullptr;
  if (ra8_fs_open(mount, k_pc_cache_path, k_ra8_fs_mode_read, &file) != k_ra8_ok) {
    return 0U;
  }
  uint32_t got = 0U;
  if (ra8_fs_read(file, buf, cap, &got) != k_ra8_ok) {
    got = 0U;
  }
  (void)ra8_fs_close(file);
  return got;
}

/**
 * @brief Initialise the engine at @p font_px over the SD font.
 * @return true on success, false on init failure.
 */
static bool pc_engine_init(uint16_t font_px, uint32_t font_len)
{
  return ra8_reflow_init((uint16_t)k_pc_view_w,
                         (uint16_t)k_pc_view_h,
                         s_font_buf,
                         (size_t)font_len,
                         font_px,
                         (uint32_t)k_pc_ink_argb,
                         (uint32_t)k_pc_link_argb,
                         &s_engine) == k_ra8_ok;
}

/**
 * @brief Lay out the chapter live and serialise it into ::s_live_blob.
 * @param[in]  font_len Font length, bytes.
 * @param[out] out_n    Receives the live blob length, bytes.
 */
static void pc_live_layout_or_halt(uint32_t font_len, size_t* out_n)
{
  if (!pc_engine_init((uint16_t)k_pc_font_px, font_len)) {
    pc_fail((uint32_t)k_pc_err_layout);
  }
  uint32_t pages = 0U;
  if (ra8_reflow_layout_chapter(&s_engine,
                                (const uint8_t*)k_pc_body,
                                (size_t)(sizeof(k_pc_body) - 1U),
                                &pages) != k_ra8_ok) {
    pc_fail((uint32_t)k_pc_err_layout);
  }
  g_pc_pages = pages;
  size_t n   = 0U;
  if (ra8_reflow_cache_serialize(&s_engine,
                                 (const uint8_t*)k_pc_body,
                                 (size_t)(sizeof(k_pc_body) - 1U),
                                 s_live_blob,
                                 (size_t)k_pc_blob_cap,
                                 &n) != k_ra8_ok) {
    pc_fail((uint32_t)k_pc_err_ser);
  }
  g_pc_live_crc = pc_crc32(s_live_blob, n);
  *out_n        = n;
}

/** @brief Persist ::s_live_blob to SD, read it back, and verify it matches. */
static void pc_persist_and_verify_or_halt(ra8_fs_mount_t* mount, size_t n)
{
  if (ra8_fs_write_file(mount, k_pc_cache_path, s_live_blob, (uint32_t)n) != k_ra8_ok) {
    pc_fail((uint32_t)k_pc_err_persist);
  }
  const uint32_t got = pc_read_cache(mount, s_read_blob, (uint32_t)k_pc_blob_cap);
  if (got != (uint32_t)n) {
    pc_fail((uint32_t)k_pc_err_persist);
  }
  if (!pc_blob_equal(s_live_blob, s_read_blob, n)) {
    pc_fail((uint32_t)k_pc_err_readback);
  }
}

/**
 * @brief Load the read-back blob into a fresh same-size engine and CRC it.
 * @param[in] font_len Font length, bytes.
 * @param[in] n        Blob length, bytes.
 */
static void pc_reload_check_or_halt(uint32_t font_len, size_t n)
{
  if (!pc_engine_init((uint16_t)k_pc_font_px, font_len)) {
    pc_fail((uint32_t)k_pc_err_layout);
  }
  if (ra8_reflow_cache_load(&s_engine,
                            (const uint8_t*)k_pc_body,
                            (size_t)(sizeof(k_pc_body) - 1U),
                            s_read_blob,
                            n) != k_ra8_ok) {
    pc_fail((uint32_t)k_pc_err_load);
  }
  size_t n2 = 0U;
  if (ra8_reflow_cache_serialize(&s_engine,
                                 (const uint8_t*)k_pc_body,
                                 (size_t)(sizeof(k_pc_body) - 1U),
                                 s_reser_blob,
                                 (size_t)k_pc_blob_cap,
                                 &n2) != k_ra8_ok) {
    pc_fail((uint32_t)k_pc_err_ser);
  }
  g_pc_cache_crc = pc_crc32(s_reser_blob, n2);
  if ((n2 != n) || (g_pc_cache_crc != g_pc_live_crc)) {
    pc_fail((uint32_t)k_pc_err_crc);
  }
  g_pc_crc_match = 1U;
}

/** @brief Load the blob at the ALT font size and assert it invalidates. */
static void pc_invalidate_check_or_halt(uint32_t font_len, size_t n)
{
  if (!pc_engine_init((uint16_t)k_pc_font_px_alt, font_len)) {
    pc_fail((uint32_t)k_pc_err_layout);
  }
  if (ra8_reflow_cache_load(&s_engine,
                            (const uint8_t*)k_pc_body,
                            (size_t)(sizeof(k_pc_body) - 1U),
                            s_read_blob,
                            n) != k_ra8_err_invalid_state) {
    pc_fail((uint32_t)k_pc_err_invalid);
  }
  g_pc_invalidate = 1U;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: SD bring-up -> font -> cache round-trip -> heartbeat idle.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post On success the cache globals are latched and ::g_pc_heartbeat advances.
 * @post On any failure ::g_pc_err is non-zero and the CPU parks (no heartbeat).
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t pclka_hz = 0U;
  pc_setup_or_halt(&pclka_hz);
  ra8_isr_globals_enable();
  ra8_log_init();
  pc_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  pc_init_card_or_halt(&pclka_hz);
  ra8_fs_mount_t* const mount = pc_mount_or_halt();

  /* Reset-survival: a prior boot's persisted cache file is the hit signal. */
  g_pc_hit = (pc_read_cache(mount, s_read_blob, (uint32_t)k_pc_blob_cap) != 0U) ? 1U : 0U;

  uint32_t font_len = 0U;
  pc_font_or_halt(mount, &font_len);

  size_t n = 0U;
  pc_live_layout_or_halt(font_len, &n);
  pc_persist_and_verify_or_halt(mount, n);
  pc_reload_check_or_halt(font_len, n);
  pc_invalidate_check_or_halt(font_len, n);

  pc_print(k_msg_pass, (uint32_t)sizeof(k_msg_pass) - 1U);
  pc_print((const uint8_t*)(g_pc_hit != 0U ? "1" : "0"), 1U);
  pc_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  /* Idle with a heartbeat that advances ONLY here -- every failure path parks
   * in WFI without bumping it -- so a memprobe gate proves the full SD -> ra8_fs
   * -> ra8_reflow_cache round-trip + invalidation ran clean. */
  while (1) {
    ra8_delay_ms(k_pc_frame_ms);
    g_pc_heartbeat++;
  }
}
#pragma GCC diagnostic pop
