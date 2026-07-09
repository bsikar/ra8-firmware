/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_shelf/src/sh_sd.c
 * @brief Optional SD-card book source for the hybrid e-reader.
 *
 * @details
 * Brings up the Pmod2 microSD over SCI0 Simple-SPI -> ra_sdmmc_spi -> ra_fs
 * (mirroring the pagecache / epub_open path), scans the FAT root for *.RBK
 * files, and serves demand-paged byte-range reads out of the selected file.
 * Each .RBK is the same compressed RBKC container the baked books use, so the
 * rest of the app is source-agnostic. The opened book's file handle stays held
 * (::sh_sd_book_open) and ::sh_sd_book_read seeks + reads single compressed
 * chunks on demand -- the whole container is never resident. Mounting is
 * best-effort: with no card (board_sim run without `--sd`)
 * ra_sdmmc_spi_init() times out and the shelf stays baked-only.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 6 / App] {World: NS}
 *
 * @since 0.1.0
 */
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_epub.h"
#include "ra_epub_fs.h"
#include "ra_fs.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci_spi.h"
#include "ra_sdmmc_spi.h"
#include "ra_spi.h"
#include "sh_app.h"

/** @enum sh_sd_const_t @brief SD bus + name constants. */
typedef enum : uint32_t {
  k_sd_spi_chan = 0U, /**< Pmod2 / J25 SCI0 Simple-SPI channel. */
  k_sd_ext_len  = 4U, /**< Length of the ".RBK" extension.      */
} sh_sd_const_t;

static const ra_port_pin_t k_sd_pin_sck  = (ra_port_pin_t)k_ra_board_pmod2_spi_sck;
static const ra_port_pin_t k_sd_pin_cipo = (ra_port_pin_t)k_ra_board_pmod2_spi_cipo;
static const ra_port_pin_t k_sd_pin_copi = (ra_port_pin_t)k_ra_board_pmod2_spi_copi;
static const ra_port_pin_t k_sd_pin_cs   = (ra_port_pin_t)k_ra_board_pmod2_spi_cs;

static uint32_t        s_pclka_hz;         /**< PCLKA for the SPI clock divider.       */
static ra_fs_backend_t s_backend;          /**< SD block-device backend (mount-lived). */
static ra_fs_mount_t*  s_mount;            /**< Mounted FAT volume, or NULL.           */
static ra_fs_file_t*   s_book;             /**< Held-open .RBK backing paged reads.    */
static const char*     s_sd_tag = "sh_sd"; /**< Log tag for SD-read diagnostics.       */

/* cppcheck-suppress constParameterCallback
 * Reason: bound to ra_sdmmc_spi_transport_t::set_clock; the void* ctx signature
 * is fixed by the seam. */
static ra_err_t sh_sd_set_clock(void* ctx, uint32_t hz)
{
  const uint32_t pclka_hz = *(const uint32_t*)ctx;
  return ra_sci_spi_set_clock((uint8_t)k_sd_spi_chan, hz, pclka_hz);
}

/** @brief ra_sdmmc_spi_transport_t::cs over ra_gpio (CS active-low). */
static ra_err_t sh_sd_cs(void* ctx, bool asserted)
{
  (void)ctx;
  return ra_gpio_write(k_sd_pin_cs, asserted ? k_ra_level_low : k_ra_level_high);
}

/** @brief ra_sdmmc_spi_transport_t::xfer over ra_sci_spi_xfer. */
static ra_err_t sh_sd_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  return ra_sci_spi_xfer((uint8_t)k_sd_spi_chan, tx, rx, len);
}

/** @brief Route Pmod2 SPI pins + CS GPIO and init SCI0 SPI at the 400 kHz floor. */
[[nodiscard]] static ra_err_t sh_sd_bus_init(void)
{
  if ((ra_pfs_route_peripheral(k_sd_pin_sck, k_ra_psel_sci_async, "sd.sck") != k_ra_ok) ||
      (ra_pfs_route_peripheral(k_sd_pin_cipo, k_ra_psel_sci_async, "sd.cipo") != k_ra_ok) ||
      (ra_pfs_route_peripheral(k_sd_pin_copi, k_ra_psel_sci_async, "sd.copi") != k_ra_ok) ||
      (ra_gpio_output_init(k_sd_pin_cs, k_ra_level_high) != k_ra_ok)) {
    return k_ra_err_invalid_arg;
  }
  const ra_sci_spi_cfg_t cfg = {.baud_hz   = (uint32_t)k_ra_sdmmc_spi_clock_init_hz,
                                .pclk_hz   = s_pclka_hz,
                                .mode      = k_ra_spi_mode_0,
                                .lsb_first = false};
  return ra_sci_spi_init((uint8_t)k_sd_spi_chan, &cfg);
}

bool sh_sd_mount(void)
{
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &s_pclka_hz) != k_ra_ok) {
    return false;
  }
  if (sh_sd_bus_init() != k_ra_ok) {
    return false;
  }
  const ra_sdmmc_spi_transport_t transport = {.set_clock = sh_sd_set_clock,
                                              .cs        = sh_sd_cs,
                                              .xfer      = sh_sd_xfer,
                                              .ctx       = &s_pclka_hz};
  if (ra_sdmmc_spi_init(&transport) != k_ra_ok) {
    return false; /* no card / timeout: caller falls back to baked-only */
  }
  if (ra_sdmmc_spi_bind_fs_backend(&s_backend) != k_ra_ok) {
    return false;
  }
  return ra_fs_mount(&s_backend, &s_mount) == k_ra_ok;
}

/** @brief Case-sensitive @p ext (e.g. ".RBK") suffix test on an 8.3 name. */
static bool sh_sd_has_ext(const char* name, const char* ext)
{
  const size_t n = strlen(name);
  return (n > (size_t)k_sd_ext_len) && (strcmp(&name[n - (size_t)k_sd_ext_len], ext) == 0);
}

/** @brief Classify an 8.3 name into a book format; true if it is a book. */
static bool sh_sd_classify(const char* name, sh_book_fmt_t* out_fmt)
{
  if (sh_sd_has_ext(name, ".RBK")) {
    *out_fmt = k_sh_fmt_rabook;
    return true;
  }
  if (sh_sd_has_ext(name, ".EPB")) { /* .epub truncates to the 3-char 8.3 ext .EPB */
    *out_fmt = k_sh_fmt_epub;
    return true;
  }
  return false;
}

/** @brief ra_fs_listdir callback: append each root *.RBK / *.EPB as an SD entry. */
static void sh_sd_listdir_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)ctx;
  const uint8_t skip = (uint8_t)k_ra_fs_attr_directory | (uint8_t)k_ra_fs_attr_volume_id;
  sh_book_fmt_t fmt  = k_sh_fmt_rabook;
  if (((attr & skip) != 0U) || !sh_sd_classify(name, &fmt) ||
      (g_sh.book_count >= (uint16_t)k_sh_max_books)) {
    return;
  }
  sh_entry_t* e = &g_sh.entry[g_sh.book_count];
  *e            = (sh_entry_t){};
  e->from_sd    = true;
  e->fmt        = fmt;
  e->blob_len   = size;
  (void)strncpy(e->sd_name, name, sizeof e->sd_name - 1U);
  /* Placeholder until first open populates real title/author/cover (reading +
   * parsing a whole book over SPI at boot is too slow). */
  (void)strncpy(e->title, name, sizeof e->title - 1U);
  (void)strncpy(e->author, "SD card -- tap to load", sizeof e->author - 1U);
  g_sh.book_count++;
}

void sh_sd_scan(void)
{
  if (s_mount != nullptr) {
    (void)ra_fs_listdir(s_mount, "/", sh_sd_listdir_cb, nullptr);
  }
}

bool sh_sd_book_open(const char* name, uint32_t* out_len)
{
  sh_sd_book_close();
  if (s_mount == nullptr) {
    return false;
  }
  if ((name == nullptr) || (out_len == nullptr)) {
    return false;
  }
  if (ra_fs_open(s_mount, name, k_ra_fs_mode_read, &s_book) != k_ra_ok) {
    s_book = nullptr;
    return false;
  }
  uint32_t size = 0U;
  if (ra_fs_size(s_book, &size) != k_ra_ok) {
    sh_sd_book_close();
    return false;
  }
  if (size == 0U) {
    sh_sd_book_close();
    return false;
  }
  *out_len = size;
  return true;
}

ra_err_t sh_sd_book_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx; /* the held file handle is module state; no per-call context */
  RA_CHECK_NULL_PTR(buf, s_sd_tag, "book_read: null buf");
  if (s_book == nullptr) {
    return k_ra_err_invalid_state;
  }
  if (offset > (uint64_t)UINT32_MAX) {
    return k_ra_err_out_of_range;
  }
  const ra_err_t err = ra_fs_seek(s_book, (uint32_t)offset);
  if (err != k_ra_ok) {
    return err;
  }
  uint32_t       got  = 0U;
  const ra_err_t rerr = ra_fs_read(s_book, buf, len, &got);
  if (rerr != k_ra_ok) {
    return rerr;
  }
  /* ra_fs_seek clamps to the file size, so a read past EOF surfaces here as a
   * short read rather than a seek error -- reject it as out-of-range. */
  if (got != len) {
    return k_ra_err_out_of_range;
  }
  return k_ra_ok;
}

void sh_sd_book_close(void)
{
  if (s_book != nullptr) {
    (void)ra_fs_close(s_book);
    s_book = nullptr;
  }
}

bool sh_sd_open_epub(const char* name, void* out_book, uint8_t* filebuf, size_t cap)
{
  if (s_mount == nullptr) {
    return false;
  }
  return ra_epub_open_fs(s_mount, name, filebuf, cap, (ra_epub_book_t*)out_book) == k_ra_ok;
}
