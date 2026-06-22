/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_shelf/src/sh_sd.c
 * @brief Optional SD-card book source for the hybrid e-reader.
 *
 * @details
 * Brings up the Pmod2 microSD over SCI0 Simple-SPI -> ra_sdmmc_spi -> ra_fs
 * (mirroring the pagecache / epub_open path), scans the FAT root for *.RBK
 * files, and reads a selected file into a shared SDRAM buffer. Each .RBK is the
 * same compressed RBKZ container the baked books use, so the rest of the app is
 * source-agnostic. Mounting is best-effort: with no card (board_sim run without
 * `--sd`) ra_sdmmc_spi_init() times out and the shelf stays baked-only.
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
#include "ra_fs.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci_spi.h"
#include "ra_sdmmc_spi.h"
#include "ra_spi.h"
#include "sh_app.h"

/** @enum sh_sd_const_t @brief SD bus + buffer constants. */
typedef enum : uint32_t {
  k_sd_spi_chan = 0U,                 /**< Pmod2 / J25 SCI0 Simple-SPI channel. */
  k_sd_file_cap = 2U * 1024U * 1024U, /**< Max compressed .RBK readable (SDRAM). */
  k_sd_ext_len  = 4U,                 /**< Length of the ".RBK" extension.       */
} sh_sd_const_t;

static const ra_port_pin_t k_sd_pin_sck  = (ra_port_pin_t)k_ra_board_pmod2_spi_sck;
static const ra_port_pin_t k_sd_pin_cipo = (ra_port_pin_t)k_ra_board_pmod2_spi_cipo;
static const ra_port_pin_t k_sd_pin_copi = (ra_port_pin_t)k_ra_board_pmod2_spi_copi;
static const ra_port_pin_t k_sd_pin_cs   = (ra_port_pin_t)k_ra_board_pmod2_spi_cs;

static uint32_t        s_pclka_hz; /**< PCLKA for the SPI clock divider.       */
static ra_fs_backend_t s_backend;  /**< SD block-device backend (mount-lived). */
static ra_fs_mount_t*  s_mount;    /**< Mounted FAT volume, or NULL.           */
static uint8_t         s_filebuf[k_sd_file_cap] __attribute__((section(".sdram_data"), aligned(8)));

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

/** @brief Case-sensitive ".RBK" suffix test on an 8.3 name. */
static bool sh_sd_is_rbk(const char* name)
{
  const size_t n = strlen(name);
  return (n > (size_t)k_sd_ext_len) && (strcmp(&name[n - (size_t)k_sd_ext_len], ".RBK") == 0);
}

/** @brief ra_fs_listdir callback: append each root *.RBK as an SD shelf entry. */
static void sh_sd_listdir_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)ctx;
  const uint8_t skip = (uint8_t)k_ra_fs_attr_directory | (uint8_t)k_ra_fs_attr_volume_id;
  if (((attr & skip) != 0U) || !sh_sd_is_rbk(name) ||
      (g_sh.book_count >= (uint16_t)k_sh_max_books)) {
    return;
  }
  sh_entry_t* e = &g_sh.entry[g_sh.book_count];
  *e            = (sh_entry_t){};
  e->from_sd    = true;
  e->blob_len   = size;
  (void)strncpy(e->sd_name, name, sizeof e->sd_name - 1U);
  /* Placeholder until first open populates real title/author/cover from the
   * header (reading a whole book over SPI at boot is too slow). */
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

const uint8_t* sh_sd_read(const char* name, uint32_t* out_len)
{
  if (s_mount == nullptr) {
    return nullptr;
  }
  ra_fs_file_t* file = nullptr;
  if (ra_fs_open(s_mount, name, k_ra_fs_mode_read, &file) != k_ra_ok) {
    return nullptr;
  }
  uint32_t       got = 0U;
  const ra_err_t err = ra_fs_read(file, s_filebuf, (uint32_t)sizeof s_filebuf, &got);
  (void)ra_fs_close(file);
  if (err != k_ra_ok) {
    return nullptr;
  }
  *out_len = got;
  return s_filebuf;
}
