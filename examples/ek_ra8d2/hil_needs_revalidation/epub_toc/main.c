/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/epub_toc/main.c
 * @brief On-silicon HIL: EPUB TOC navigation (NCX + nav.xhtml) from SD (#116).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * #74 added titled-TOC parsing to `ra8_epub` -- both the EPUB2 NCX (`<navMap>`)
 * and the EPUB3 `nav.xhtml` (`<nav epub:type="toc">`) forms -- but it has only
 * ever run on the x86 host. This app, building on `epub_open` (#114), runs
 * the TOC path on the M85 against real `.epub` files staged on a microSD card.
 *
 * It self-provisions three baked books onto the card (if absent) and parses each:
 *   - `TOCNCX.EPB`: EPUB2 NCX -> assert `toc_kind` == ncx, `toc_count` == 2, the
 *     CRC-32 of entry 0's label, and that entry 0 resolves to spine index 0.
 *   - `TOCNAV.EPB`: EPUB3 nav -> assert `toc_kind` == nav, `toc_count` == 3, the
 *     entry-0 label CRC, and entry-0 -> spine index 0 (the `#fragment` stripped).
 *   - `TOCBAD.EPB`: no TOC document -> assert graceful degradation: `toc_kind`
 *     none with the spine still readable (chapter count == 2), no HardFault.
 *
 * The HIL gate is memprobe (J-Link / ra8_emulator `--dump-sym`), not the console:
 * an SD app drives the SCI0 Simple-SPI bus, and ra8_emulator folds every SCI channel
 * into one console line, so the SCI8 banner is interleaved with SPI traffic there
 * (the same reason the sibling SD HIL apps -- `epub_open`, `sd_font_render`,
 * `fs_format_mount` -- gate on SWD globals). The success path advances
 * ::g_etoc_heartbeat once per frame and only after every assertion passes; any
 * failure stamps ::g_etoc_err and parks without bumping it. So a steadily
 * advancing heartbeat with a zero ::g_etoc_err proves both TOC code paths plus
 * the malformed-TOC fallback ran on real SD bytes. The console banner remains for
 * a real-bench scope.
 *
 * Required external hardware (on-bench): Digilent PMOD MicroSD (410-380) in Pmod2
 * (J25) with a microSD inserted. THIS APP MAY FORMAT THE CARD. Under ra8_emulator
 * attach a blank card with `--sd-new 64:fat32`.
 *
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "epub_toc_fixture.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_epub.h"
#include "ra8_epub_fs.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci_spi.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_spi.h"
#include "ra8_time.h"

/** @enum etoc_consts_t @brief Console / SPI / parse knobs (no magic numbers). */
typedef enum : uint32_t {
  k_etoc_uart_baud  = 115200U,     /**< Console baud.                        */
  k_etoc_spi_chan   = 0U,          /**< Pmod2 / J25 SCI0 Simple-SPI.         */
  k_etoc_crc_init   = 0xFFFFFFFFU, /**< CRC-32 initial value.                */
  k_etoc_crc_poly   = 0xEDB88320U, /**< CRC-32 reflected polynomial.         */
  k_etoc_crc_bits   = 8U,          /**< Bits folded per byte.                */
  k_etoc_title_max  = 256U,        /**< Bounded title scan (NASA Rule 2).    */
  k_etoc_no_chap    = 0xFFFFU,     /**< Sentinel: entry has no spine target. */
  k_etoc_kind_unset = 0xFFU,       /**< Sentinel: toc_kind not yet read.     */
} etoc_consts_t;

/** @enum etoc_expect_t @brief Byte-exact expected TOC results (host-verified). */
typedef enum : uint32_t {
  k_etoc_ncx_kind  = 1U,          /**< k_ra8_epub_toc_ncx.               */
  k_etoc_ncx_count = 2U,          /**< navMap navPoints.                 */
  k_etoc_ncx_crc   = 0xDBC4EA24U, /**< crc32("Intro").                   */
  k_etoc_nav_kind  = 2U,          /**< k_ra8_epub_toc_nav.               */
  k_etoc_nav_count = 3U,          /**< nav `<ol>` entries.               */
  k_etoc_nav_crc   = 0x4CC9A9C1U, /**< crc32("Cover").                   */
  k_etoc_bad_kind  = 0U,          /**< k_ra8_epub_toc_none (no TOC doc). */
  k_etoc_exp_chap  = 2U,          /**< Spine length in every fixture.    */
  k_etoc_entry0    = 0U,          /**< Entry-0 -> spine index 0.         */
} etoc_expect_t;

/** @brief Pmod2 SPI pins (J25) -- SCI0 Simple-SPI; CS held by GPIO. */
static const ra8_port_pin_t k_etoc_pin_sck  = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck;
static const ra8_port_pin_t k_etoc_pin_cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo;
static const ra8_port_pin_t k_etoc_pin_copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi;
static const ra8_port_pin_t k_etoc_pin_cs   = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs;

/** @brief SD paths for the three baked books (8.3 short names; ra8_fs root-only). */
static const char k_etoc_path_ncx[] = "TOCNCX.EPB";
static const char k_etoc_path_nav[] = "TOCNAV.EPB";
static const char k_etoc_path_bad[] = "TOCBAD.EPB";

/**
 * @enum etoc_err_t
 * @brief Failure-stage codes stamped to ::g_etoc_err for SWD / `--dump-sym`.
 */
typedef enum : uint32_t {
  k_etoc_err_none  = 0U, /**< No failure (success path).           */
  k_etoc_err_init  = 1U, /**< CGC / time / console / SPI bring-up. */
  k_etoc_err_card  = 2U, /**< SD card SPI init.                    */
  k_etoc_err_mount = 3U, /**< Mount (and format-if-blank).         */
  k_etoc_err_prov  = 4U, /**< Provision a .epub onto the card.     */
  k_etoc_err_ncx   = 5U, /**< NCX (EPUB2) TOC assertion.           */
  k_etoc_err_nav   = 6U, /**< nav (EPUB3) TOC assertion.           */
  k_etoc_err_bad   = 7U, /**< Malformed-TOC fallback assertion.    */
} etoc_err_t;

/** @enum etoc_pace_t @brief Idle-loop pacing for the success heartbeat. */
typedef enum : uint32_t {
  k_etoc_frame_ms = 100U, /**< Heartbeat period in milliseconds. */
} etoc_pace_t;

/**
 * @var g_etoc_err
 * @brief First failing stage (::etoc_err_t), 0 on success. SWD / `--dump-sym`.
 * @note Exported (non-static) so a memprobe gate can read it by symbol.
 */
volatile uint32_t g_etoc_err       = (uint32_t)k_etoc_err_none;
volatile uint32_t g_etoc_ncx_kind  = 0U; /**< NCX toc_kind on success.       */
volatile uint32_t g_etoc_ncx_n     = 0U; /**< NCX toc_count on success.      */
volatile uint32_t g_etoc_ncx_e0crc = 0U; /**< NCX entry-0 label CRC.         */
volatile uint32_t g_etoc_ncx_ch0   = 0U; /**< NCX entry-0 -> spine index.    */
volatile uint32_t g_etoc_nav_kind  = 0U; /**< nav toc_kind on success.       */
volatile uint32_t g_etoc_nav_n     = 0U; /**< nav toc_count on success.      */
volatile uint32_t g_etoc_nav_e0crc = 0U; /**< nav entry-0 label CRC.         */
volatile uint32_t g_etoc_nav_ch0   = 0U; /**< nav entry-0 -> spine index.    */
volatile uint32_t g_etoc_bad_kind  = 0U; /**< malformed-TOC kind (expect 0). */
volatile uint32_t g_etoc_bad_chap  = 0U; /**< malformed-TOC spine fallback.  */
/** @brief Idle heartbeat; advances ONLY after all asserts pass (the HIL gate). */
volatile uint32_t g_etoc_heartbeat = 0U;

/** @brief Opened book (large -- file-scope, not on the stack). */
static ra8_epub_book_t s_book;
/** @brief Streamed-open source-file context; must outlive @ref s_book (#230). */
static ra8_epub_stream_fs_ctx_t s_epub_io;
/** @brief SD backend; file-scope so the mount handle may reference it. */
static ra8_fs_backend_t s_backend;

static const uint8_t k_msg_boot[]   = "epub-toc-hil: boot\r\n";
static const uint8_t k_msg_cardok[] = "epub-toc-hil: card ready\r\n";
static const uint8_t k_msg_finit[]  = "toc-hil: FAIL init\r\n";
static const uint8_t k_msg_fcard[]  = "toc-hil: FAIL card\r\n";
static const uint8_t k_msg_fmount[] = "toc-hil: FAIL mount\r\n";
static const uint8_t k_msg_fprov[]  = "toc-hil: FAIL provision\r\n";
static const uint8_t k_msg_fncx[]   = "toc-hil: FAIL ncx\r\n";
static const uint8_t k_msg_fnav[]   = "toc-hil: FAIL nav\r\n";
static const uint8_t k_msg_fbad[]   = "toc-hil: FAIL bad\r\n";
static const uint8_t k_msg_ok[]     = "toc-hil: ncx+nav+fallback PASS\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void etoc_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/**
 * @brief Stamp the failure stage, print the FAIL banner, and park the CPU.
 *
 * @param[in] err Failure-stage code (::etoc_err_t).
 * @param[in] msg Console diagnostic bytes.
 * @param[in] len Length of @p msg.
 */
static void etoc_fail(uint32_t err, const uint8_t* msg, uint32_t len)
{
  g_etoc_err = err;
  etoc_print(msg, len);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief CRC-32 (reflected, poly 0xEDB88320) over @p data. */
static uint32_t etoc_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = (uint32_t)k_etoc_crc_init;
  for (size_t i = 0U; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint32_t bit = 0U; bit < (uint32_t)k_etoc_crc_bits; bit++) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc                 = (crc >> 1U) ^ ((uint32_t)k_etoc_crc_poly & mask);
    }
  }
  return ~crc;
}

/** @brief CRC-32 over a null-terminated title (bounded by k_etoc_title_max). */
static uint32_t etoc_title_crc(const char* title)
{
  size_t len = 0U;
  while ((len < (size_t)k_etoc_title_max) && (title[len] != '\0')) {
    len++;
  }
  return etoc_crc32((const uint8_t*)title, len);
}

/* ---- SPI -> ra8_sdmmc_spi transport adapter (mirror of epub_open) ---- */

/* cppcheck-suppress constParameterCallback -- bound to ra8_sdmmc_spi_transport_t::set_clock, `ra8_err_t (*)(void*, uint32_t)`; constifying ctx would break the binding. */
static ra8_err_t etoc_spi_set_clock(void* ctx, uint32_t hz)
{
  const uint32_t pclka_hz = *(const uint32_t*)ctx;
  return ra8_sci_spi_set_clock((uint8_t)k_etoc_spi_chan, hz, pclka_hz);
}

/** @brief ra8_sdmmc_spi_transport_t::cs over ra8_gpio (CS active-low). */
static ra8_err_t etoc_spi_cs(void* ctx, bool asserted)
{
  (void)ctx;
  return ra8_gpio_write(k_etoc_pin_cs, asserted ? k_ra8_level_low : k_ra8_level_high);
}

/** @brief ra8_sdmmc_spi_transport_t::xfer over ra8_sci_spi_xfer. */
static ra8_err_t etoc_spi_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  return ra8_sci_spi_xfer((uint8_t)k_etoc_spi_chan, tx, rx, len);
}

/** @brief Route Pmod2 SPI pins and claim CS as a GPIO output (idle high). */
[[nodiscard]] static ra8_err_t etoc_spi_pins_init(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral(k_etoc_pin_sck, k_ra8_psel_sci_async, "etoc.sck");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_etoc_pin_cipo, k_ra8_psel_sci_async, "etoc.cipo");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_etoc_pin_copi, k_ra8_psel_sci_async, "etoc.copi");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_output_init(k_etoc_pin_cs, k_ra8_level_high);
}

/** @brief Bring up CGC + SysTick + console SCI + SPI + CS GPIO; halt on fail. */
static void etoc_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) ||
      (ra8_time_init(cpuclk0_hz) != k_ra8_ok)) {
    etoc_fail((uint32_t)k_etoc_err_init, k_msg_finit, (uint32_t)sizeof(k_msg_finit) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_etoc_uart_baud) != k_ra8_ok) {
    etoc_fail((uint32_t)k_etoc_err_init, k_msg_finit, (uint32_t)sizeof(k_msg_finit) - 1U);
  }
  if (etoc_spi_pins_init() != k_ra8_ok) {
    etoc_fail((uint32_t)k_etoc_err_init, k_msg_finit, (uint32_t)sizeof(k_msg_finit) - 1U);
  }
  const ra8_sci_spi_cfg_t spi_cfg = {.baud_hz   = (uint32_t)k_ra8_sdmmc_spi_clock_init_hz,
                                     .pclk_hz   = pclka_hz,
                                     .mode      = k_ra8_spi_mode_0,
                                     .lsb_first = false};
  if (ra8_sci_spi_init((uint8_t)k_etoc_spi_chan, &spi_cfg) != k_ra8_ok) {
    etoc_fail((uint32_t)k_etoc_err_init, k_msg_finit, (uint32_t)sizeof(k_msg_finit) - 1U);
  }
  *out_pclka_hz = pclka_hz;
}

/** @brief Init the SD card over SPI; halt with a diagnostic on failure. */
static void etoc_init_card_or_halt(uint32_t* pclka_hz)
{
  const ra8_sdmmc_spi_transport_t transport = {.set_clock = etoc_spi_set_clock,
                                               .cs        = etoc_spi_cs,
                                               .xfer      = etoc_spi_xfer,
                                               .ctx       = pclka_hz};
  if (ra8_sdmmc_spi_init(&transport) != k_ra8_ok) {
    etoc_fail((uint32_t)k_etoc_err_card, k_msg_fcard, (uint32_t)sizeof(k_msg_fcard) - 1U);
  }
  etoc_print(k_msg_cardok, (uint32_t)sizeof(k_msg_cardok) - 1U);
}

/** @brief Mount the card, formatting FAT32 first if it is blank/unmountable. */
static ra8_fs_mount_t* etoc_mount_or_halt(void)
{
  if (ra8_sdmmc_spi_bind_fs_backend(&s_backend) != k_ra8_ok) {
    etoc_fail((uint32_t)k_etoc_err_mount, k_msg_fmount, (uint32_t)sizeof(k_msg_fmount) - 1U);
  }
  ra8_fs_mount_t* mount = nullptr;
  if (ra8_fs_mount(&s_backend, &mount) != k_ra8_ok) {
    ra8_fs_format_opts_t opts = {};
    opts.type                 = k_ra8_fs_type_fat32;
    opts.label                = "RAEPUB";
    if ((ra8_fs_format(&s_backend, &opts) != k_ra8_ok) ||
        (ra8_fs_mount(&s_backend, &mount) != k_ra8_ok)) {
      etoc_fail((uint32_t)k_etoc_err_mount, k_msg_fmount, (uint32_t)sizeof(k_msg_fmount) - 1U);
    }
  }
  return mount;
}

/** @brief Write one baked book onto the volume if @p path is absent. */
static void
etoc_provision_one(ra8_fs_mount_t* mount, const char* path, const uint8_t* data, uint32_t len)
{
  ra8_fs_file_t* file = nullptr;
  if (ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file) == k_ra8_ok) {
    (void)ra8_fs_close(file); /* already present -- nothing to provision. */
    return;
  }
  if (ra8_fs_write_file(mount, path, data, len) != k_ra8_ok) {
    etoc_fail((uint32_t)k_etoc_err_prov, k_msg_fprov, (uint32_t)sizeof(k_msg_fprov) - 1U);
  }
}

/** @brief Provision all three baked TOC fixtures onto the card. */
static void etoc_provision_or_halt(ra8_fs_mount_t* mount)
{
  etoc_provision_one(mount, k_etoc_path_ncx, k_etoc_ncx, (uint32_t)k_etoc_ncx_len);
  etoc_provision_one(mount, k_etoc_path_nav, k_etoc_nav, (uint32_t)k_etoc_nav_len);
  etoc_provision_one(mount, k_etoc_path_bad, k_etoc_bad, (uint32_t)k_etoc_bad_len);
}

/**
 * @brief Open one book off SD and read its TOC kind/count + entry-0 label CRC.
 *
 * @param[in]  mount   Mounted SD volume.
 * @param[in]  path    Book path on the volume.
 * @param[in]  stage   Failure-stage code for this book.
 * @param[in]  fmsg    FAIL banner for this book.
 * @param[in]  fmsglen Length of @p fmsg.
 * @param[out] kind    Receives `ra8_epub_get_toc_kind`.
 * @param[out] count   Receives `ra8_epub_get_toc_count`.
 * @param[out] e0crc   Receives the CRC-32 of entry 0's label (0 if no entries).
 * @param[out] ch0     Receives entry-0 -> spine index (0xFFFF if no entries).
 */
static void etoc_read_toc(ra8_fs_mount_t*    mount,
                          const char*        path,
                          uint32_t           stage,
                          const uint8_t*     fmsg,
                          uint32_t           fmsglen,
                          volatile uint32_t* kind,
                          volatile uint32_t* count,
                          volatile uint32_t* e0crc,
                          volatile uint32_t* ch0)
{
  if (ra8_epub_open_streamed_fs(mount, path, &s_epub_io, &s_book) != k_ra8_ok) {
    etoc_fail(stage, fmsg, fmsglen);
  }
  uint8_t  k = 0U;
  uint16_t n = 0U;
  if ((ra8_epub_get_toc_kind(&s_book, &k) != k_ra8_ok) ||
      (ra8_epub_get_toc_count(&s_book, &n) != k_ra8_ok)) {
    etoc_fail(stage, fmsg, fmsglen);
  }
  *kind  = (uint32_t)k;
  *count = (uint32_t)n;
  *e0crc = 0U;
  *ch0   = (uint32_t)k_etoc_no_chap;
  if (n > 0U) {
    ra8_epub_toc_entry_t entry = {};
    uint16_t             chap  = (uint16_t)k_etoc_no_chap;
    if ((ra8_epub_get_toc_entry(&s_book, 0U, &entry) != k_ra8_ok) ||
        (ra8_epub_toc_entry_to_chapter(&s_book, 0U, &chap) != k_ra8_ok)) {
      etoc_fail(stage, fmsg, fmsglen);
    }
    *e0crc = etoc_title_crc(entry.title);
    *ch0   = (uint32_t)chap;
  }
  (void)ra8_epub_close_streamed_fs(&s_epub_io, &s_book);
}

/** @brief Parse + assert the NCX (EPUB2) TOC; latch the result globals. */
static void etoc_check_ncx(ra8_fs_mount_t* mount)
{
  etoc_read_toc(mount,
                k_etoc_path_ncx,
                (uint32_t)k_etoc_err_ncx,
                k_msg_fncx,
                (uint32_t)sizeof(k_msg_fncx) - 1U,
                &g_etoc_ncx_kind,
                &g_etoc_ncx_n,
                &g_etoc_ncx_e0crc,
                &g_etoc_ncx_ch0);
  if ((g_etoc_ncx_kind != (uint32_t)k_etoc_ncx_kind) ||
      (g_etoc_ncx_n != (uint32_t)k_etoc_ncx_count) ||
      (g_etoc_ncx_e0crc != (uint32_t)k_etoc_ncx_crc) ||
      (g_etoc_ncx_ch0 != (uint32_t)k_etoc_entry0)) {
    etoc_fail((uint32_t)k_etoc_err_ncx, k_msg_fncx, (uint32_t)sizeof(k_msg_fncx) - 1U);
  }
}

/** @brief Parse + assert the nav (EPUB3) TOC; latch the result globals. */
static void etoc_check_nav(ra8_fs_mount_t* mount)
{
  etoc_read_toc(mount,
                k_etoc_path_nav,
                (uint32_t)k_etoc_err_nav,
                k_msg_fnav,
                (uint32_t)sizeof(k_msg_fnav) - 1U,
                &g_etoc_nav_kind,
                &g_etoc_nav_n,
                &g_etoc_nav_e0crc,
                &g_etoc_nav_ch0);
  if ((g_etoc_nav_kind != (uint32_t)k_etoc_nav_kind) ||
      (g_etoc_nav_n != (uint32_t)k_etoc_nav_count) ||
      (g_etoc_nav_e0crc != (uint32_t)k_etoc_nav_crc) ||
      (g_etoc_nav_ch0 != (uint32_t)k_etoc_entry0)) {
    etoc_fail((uint32_t)k_etoc_err_nav, k_msg_fnav, (uint32_t)sizeof(k_msg_fnav) - 1U);
  }
}

/**
 * @brief Assert the malformed-TOC fallback: no TOC, spine still readable.
 *
 * @details Opens the no-TOC book, requires `toc_kind` none, then confirms the
 * spine is still usable (`ra8_epub_get_chapter_count` == 2) so a renderer degrades
 * to the spine list rather than faulting.
 */
static void etoc_check_bad(ra8_fs_mount_t* mount)
{
  if (ra8_epub_open_streamed_fs(mount, k_etoc_path_bad, &s_epub_io, &s_book) != k_ra8_ok) {
    etoc_fail((uint32_t)k_etoc_err_bad, k_msg_fbad, (uint32_t)sizeof(k_msg_fbad) - 1U);
  }
  uint8_t  kind = (uint8_t)k_etoc_kind_unset;
  uint16_t chap = 0U;
  if ((ra8_epub_get_toc_kind(&s_book, &kind) != k_ra8_ok) ||
      (ra8_epub_get_chapter_count(&s_book, &chap) != k_ra8_ok)) {
    etoc_fail((uint32_t)k_etoc_err_bad, k_msg_fbad, (uint32_t)sizeof(k_msg_fbad) - 1U);
  }
  g_etoc_bad_kind = (uint32_t)kind;
  g_etoc_bad_chap = (uint32_t)chap;
  if ((kind != (uint8_t)k_etoc_bad_kind) || (chap != (uint16_t)k_etoc_exp_chap)) {
    etoc_fail((uint32_t)k_etoc_err_bad, k_msg_fbad, (uint32_t)sizeof(k_msg_fbad) - 1U);
  }
  (void)ra8_epub_close_streamed_fs(&s_epub_io, &s_book);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: SD bring-up -> provision -> TOC asserts -> heartbeat idle.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post On success the g_etoc_* result globals hold the parsed TOC values, the
 *       banner is emitted, and ::g_etoc_heartbeat advances once per frame.
 * @post On any failure ::g_etoc_err is non-zero and the CPU parks (no heartbeat).
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t pclka_hz = 0U;
  etoc_setup_or_halt(&pclka_hz);
  ra8_isr_globals_enable();
  ra8_log_init();
  etoc_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  etoc_init_card_or_halt(&pclka_hz);
  ra8_fs_mount_t* const mount = etoc_mount_or_halt();
  etoc_provision_or_halt(mount);

  etoc_check_ncx(mount);
  etoc_check_nav(mount);
  etoc_check_bad(mount);

  etoc_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  /* Idle with a heartbeat that advances ONLY here -- every failure path parks in
   * WFI without bumping it -- so a memprobe gate proves both TOC code paths plus
   * the malformed-TOC fallback ran on real SD bytes. */
  while (1) {
    ra8_delay_ms(k_etoc_frame_ms);
    g_etoc_heartbeat++;
  }
}
#pragma GCC diagnostic pop
