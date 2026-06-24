/**
 * @file examples/ek_ra8d2/hw_pending/ra_io_sd_demo/main.c
 * @brief Prove the ra_io fabric's swappable backend (#155/#156) over a microSD.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Companion to `ra_io_demo`, which runs the `ra_io` VFS over a RAM disk. This
 * app runs the IDENTICAL `ra_io` VFS API over a real micro-SD card by swapping
 * only the block-device backend: instead of the RAM backend it binds the
 * `ra_io` SD-over-SPI block device (`ra_io_blockdev_sdspi_init`) on top of the
 * `ra_sdmmc_spi` driver. The same Pmod2 / SCI0 Simple-SPI transport adapter as
 * `fs_format_mount` brings the card up; the data path above the block device is
 * pure `ra_io`:
 *
 *   1. `ra_io_blockdev_sdspi_init` -- SD-SPI vtable over the card.
 *   2. `ra_io_blockdev_as_fs_backend` -- bridge the block device to `ra_fs`.
 *   3. `ra_fs_format` (FAT16) + `ra_fs_mount`, then `ra_io_vfs_mount("sd", ...)`.
 *   4. `ra_io_vfs_mkdir("sd:/LOGS")` -- exercises the VFS mkdir path.
 *   5. `ra_io_vfs_open("sd:/LOGS/A.TXT", write)` + `ra_fs_write` -- write payload.
 *   6. `ra_io_vfs_open("sd:/LOGS/A.TXT", read)` + `ra_fs_read` -- read it back.
 *   7. byte-compare the read-back against the deterministic payload.
 *
 * On a clean round-trip it prints exactly
 * `ra_io_sd_demo: sd:/LOGS/A.TXT 512 bytes PASS`; any failed step prints
 * `ra_io_sd_demo: FAIL` and parks the CPU. The HIL runner and the board_sim
 * smoke gate scrape for that PASS line.
 *
 * Required external hardware (on-bench): Digilent PMOD MicroSD (part 410-380)
 * in Pmod2 (J25) with any microSD card inserted. THIS APP ERASES THE CARD.
 * Under board_sim attach a blank card with `--sd-new 64:fat16`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_gpio_constants.h"
#include "ra_io.h"
#include "ra_isr.h"
#include "ra_log.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_sci_spi.h"
#include "ra_sdmmc_spi.h"
#include "ra_spi.h"
#include "ra_time.h"

/* =============================================================================
 * Tunables
 * =============================================================================
 */

/**
 * @enum sd_demo_config_t
 * @brief Compile-time settings for the ra_io SD-backend demo.
 */
typedef enum : uint32_t {
  k_sd_demo_uart_baud       = 115200U,      /**< J-Link OB CDC console baud.            */
  k_sd_demo_uart_channel    = 8U,           /**< SCI8 console (TXD8=PD02, RXD8=PD03).   */
  k_sd_demo_spi_channel     = 0U,           /**< Pmod2 / J25 SCI0 Simple-SPI.           */
  k_sd_demo_payload_bytes   = 512U,         /**< One-sector deterministic test payload. */
  k_sd_demo_prng_seed       = 0xA5F00DadUL, /**< Deterministic payload seed.            */
  k_sd_demo_prng_mul        = 1664525UL,    /**< Numerical Recipes LCG multiplier.      */
  k_sd_demo_prng_add        = 1013904223UL, /**< Numerical Recipes LCG increment.       */
  k_sd_demo_prng_byte_shift = 16U,          /**< Bit shift selecting the PRNG byte.     */
  k_sd_demo_byte_mask       = 0xFFU,        /**< Low-byte mask.                         */
} sd_demo_config_t;

/* =============================================================================
 * Pinout (SCI8 console + Pmod2 SPI for SD card)
 * =============================================================================
 */

/** @brief SCI8 console TXD = PD02 (UM Table 26 console pinmap). */
static const ra_port_pin_t k_sd_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
/** @brief SCI8 console RXD = PD03 (UM Table 26 console pinmap). */
static const ra_port_pin_t k_sd_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Pmod2 SPI pins (J25) -- SCI0 Simple-SPI; CS held by GPIO. */
static const ra_port_pin_t k_sd_demo_pin_sck  = (ra_port_pin_t)k_ra_board_pmod2_spi_sck;
static const ra_port_pin_t k_sd_demo_pin_cipo = (ra_port_pin_t)k_ra_board_pmod2_spi_cipo;
static const ra_port_pin_t k_sd_demo_pin_copi = (ra_port_pin_t)k_ra_board_pmod2_spi_copi;
static const ra_port_pin_t k_sd_demo_pin_cs   = (ra_port_pin_t)k_ra_board_pmod2_spi_cs;

/* =============================================================================
 * Static message strings (ASCII-only per project policy)
 * =============================================================================
 */

static const uint8_t k_msg_boot[]      = "ra_io_sd_demo: boot\r\n";
static const uint8_t k_msg_card_ok[]   = "ra_io_sd_demo: card ready\r\n";
static const uint8_t k_msg_init_fail[] = "ra_io_sd_demo: FAIL init\r\n";
static const uint8_t k_msg_pass[]      = "ra_io_sd_demo: sd:/LOGS/A.TXT 512 bytes PASS\r\n";
static const uint8_t k_msg_fail[]      = "ra_io_sd_demo: FAIL\r\n";

/** @brief VFS mount name and target path for the round-trip file. */
static const char k_mount_name[] = "sd";
static const char k_dir_path[]   = "sd:/LOGS";
static const char k_file_path[]  = "sd:/LOGS/A.TXT";

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_sd_demo";

/* =============================================================================
 * UART output helpers
 * =============================================================================
 */

/**
 * @brief Write a byte run on the SCI8 console.
 *
 * @details Thin wrapper over `ra_sci_write_polling` so the demo's print sites
 *          stay terse; the return value is intentionally discarded because a
 *          diagnostic-print failure has no useful recovery on a panic path.
 *
 * @param[in] msg Bytes to emit (non-NULL, length @p len).
 * @param[in] len Byte count to emit.
 *
 * @return Nothing.
 *
 * @pre SCI8 is initialised.
 * @pre @p msg points to at least @p len readable bytes.
 * @post The bytes are queued to the console.
 * @post No other state is modified.
 *
 * @note Not thread-safe; call from the single-threaded app context.
 * @since 0.1.0
 */
static void sd_demo_print(const uint8_t* msg, uint32_t len)
{
  (void)ra_sci_write_polling((uint8_t)k_sd_demo_uart_channel, msg, len);
}

/** @brief Emit a NUL-terminated literal (length via sizeof at the call site). */
#define SD_DEMO_PUTS(lit) sd_demo_print((lit), (uint32_t)sizeof(lit) - 1U)

/**
 * @brief Halt forever in WFI -- panic stop on irrecoverable failure.
 *
 * @details Used after a fatal bring-up or round-trip error has already been
 *          reported on the console; it parks the core so a debugger or the HIL
 *          runner can observe the final state.
 *
 * @return Never returns.
 *
 * @pre A fatal error has been reported to the console.
 * @pre The console output has been queued.
 * @post The CPU spins in WFI and never returns.
 * @post No further application code runs.
 *
 * @note Not thread-safe; this is a terminal panic path.
 * @since 0.1.0
 */
static void sd_demo_panic_halt(void)
{
  while (true) {
    __asm__ volatile("wfi");
  }
}

/* =============================================================================
 * SPI -> ra_sdmmc_spi transport adapter (mirror of fs_format_mount)
 * =============================================================================
 */

/* cppcheck-suppress constParameterCallback
 * Reason: bound to ra_sdmmc_spi_transport_t::set_clock, whose signature is
 * `ra_err_t (*)(void*, uint32_t)`; constifying ctx would break the binding. */
/**
 * @brief `ra_sdmmc_spi_transport_t::set_clock` shim over `ra_sci_spi_set_clock`.
 *
 * @details Recovers the cached PCLKA rate from the transport context and asks
 *          the SCI0 Simple-SPI driver to reprogram the bit-rate generator for
 *          the requested SCK frequency. Bound by value into the transport vtable
 *          the SD driver calls during init and speed-up.
 *
 * @param[in] ctx Transport context: pointer to the cached PCLKA rate in Hz.
 * @param[in] hz  Requested SCK frequency in Hz.
 *
 * @return ra_err_t from `ra_sci_spi_set_clock`.
 * @retval k_ra_ok    The bit-rate generator was reprogrammed.
 * @retval k_ra_err_* SCI clock-configuration failure.
 *
 * @pre @p ctx points to a live PCLKA-rate value.
 * @pre SCI0 Simple-SPI is initialised.
 * @post On success SCK runs at the closest achievable rate to @p hz.
 * @post No state beyond the SCI bit-rate registers is modified.
 *
 * @note Not thread-safe; called from the SD driver's single-threaded path.
 * @since 0.1.0
 */
static ra_err_t sd_demo_spi_set_clock(void* ctx, uint32_t hz)
{
  const uint32_t pclka_hz = *(const uint32_t*)ctx;
  return ra_sci_spi_set_clock((uint8_t)k_sd_demo_spi_channel, hz, pclka_hz);
}

/**
 * @brief `ra_sdmmc_spi_transport_t::cs` shim that drives the CS GPIO (active-low).
 *
 * @details Translates the driver's logical "assert / deassert" request into the
 *          physical CS level: asserted maps to a low output (card selected),
 *          deasserted to high. The context is unused because the CS pin is a
 *          fixed compile-time constant.
 *
 * @param[in] ctx      Unused transport context.
 * @param[in] asserted true => select the card (drive CS low); false => deselect.
 *
 * @return ra_err_t from `ra_gpio_write`.
 * @retval k_ra_ok    CS was driven to the requested level.
 * @retval k_ra_err_* GPIO write failure.
 *
 * @pre The CS pin is configured as a GPIO output.
 * @pre IOPORT is reachable.
 * @post CS reflects @p asserted on success.
 * @post No other pin state is modified.
 *
 * @note Not thread-safe; called from the SD driver's single-threaded path.
 * @since 0.1.0
 */
static ra_err_t sd_demo_spi_cs(void* ctx, bool asserted)
{
  (void)ctx;
  return ra_gpio_write(k_sd_demo_pin_cs, asserted ? k_ra_level_low : k_ra_level_high);
}

/**
 * @brief `ra_sdmmc_spi_transport_t::xfer` shim over `ra_sci_spi_xfer`.
 *
 * @details Performs a full-duplex SPI transfer of @p len bytes on SCI0, shifting
 *          @p tx out while latching the received bytes into @p rx. Either buffer
 *          may be NULL when that direction is don't-care, per the SCI SPI driver
 *          contract. The context is unused (fixed channel).
 *
 * @param[in]  ctx Unused transport context.
 * @param[in]  tx  Bytes to shift out (or NULL to send idle bytes).
 * @param[out] rx  Buffer for received bytes (or NULL to discard).
 * @param[in]  len Transfer length in bytes.
 *
 * @return ra_err_t from `ra_sci_spi_xfer`.
 * @retval k_ra_ok    The transfer completed.
 * @retval k_ra_err_* SCI SPI transfer failure.
 *
 * @pre SCI0 Simple-SPI is initialised and the card is selected as required.
 * @pre @p tx / @p rx each reference @p len bytes when non-NULL.
 * @post On success @p rx holds the @p len received bytes when non-NULL.
 * @post No state beyond @p rx and the SCI data registers is modified.
 *
 * @note Not thread-safe; called from the SD driver's single-threaded path.
 * @since 0.1.0
 */
static ra_err_t sd_demo_spi_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  return ra_sci_spi_xfer((uint8_t)k_sd_demo_spi_channel, tx, rx, len);
}

/* =============================================================================
 * Hardware bring-up
 * =============================================================================
 */

/**
 * @brief Route SCI8 console pins to PD02 / PD03.
 *
 * @details Routes both console pins to the SCI async function via PFS; returns
 *          on the first routing failure so the caller can panic-halt.
 *
 * @return ra_err_t from the PFS routing calls.
 * @retval k_ra_ok    Both console pins routed.
 * @retval k_ra_err_* PFS routing failure.
 *
 * @pre IOPORT is reachable.
 * @pre The console SCI module clock is enabled.
 * @post PD02/PD03 are in SCI async mode on success.
 * @post On failure the pins are left in their prior state.
 *
 * @note Not thread-safe; call during single-threaded init.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sd_demo_console_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_sd_demo_pin_txd, k_ra_psel_sci_async, "sddemo.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_sd_demo_pin_rxd, k_ra_psel_sci_async, "sddemo.rxd8");
}

/**
 * @brief Route Pmod2 SPI pins and claim CS as a GPIO output (idle high).
 *
 * @details Routes SCK/CIPO/COPI to the SCI0 Simple-SPI function and drives the
 *          CS pin high as a GPIO output; returns on the first failure.
 *
 * @return ra_err_t from the routing calls.
 * @retval k_ra_ok    SPI pins routed, CS driven high.
 * @retval k_ra_err_* Routing failure.
 *
 * @pre IOPORT is reachable.
 * @pre The SCI0 module clock is enabled.
 * @post P601/P602/P603 are SCI0 Simple-SPI; P604 is a GPIO output high.
 * @post On failure the affected pins are left in their prior state.
 *
 * @note Not thread-safe; call during single-threaded init.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sd_demo_spi_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_sd_demo_pin_sck, k_ra_psel_sci_async, "sddemo.sck");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_sd_demo_pin_cipo, k_ra_psel_sci_async, "sddemo.cipo");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_sd_demo_pin_copi, k_ra_psel_sci_async, "sddemo.copi");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_gpio_output_init(k_sd_demo_pin_cs, k_ra_level_high);
}

/**
 * @brief Bring up CGC + SysTick + console SCI + SPI + CS GPIO; panic on fail.
 *
 * @details Initialises the clock generator, caches CPUCLK0 and PCLKA, starts the
 *          SysTick time base, routes the console pins, configures SCI8 async, and
 *          configures SCI0 Simple-SPI at the SD init clock. Any failing step
 *          panic-halts so a misconfigured bus never reaches the SD bring-up.
 *
 * @param[out] out_pclka_hz Cached PCLKA rate (Hz) for the SPI clock shim.
 *
 * @return Nothing (panic-halts on failure).
 *
 * @pre Reset_Handler initialised .data/.bss.
 * @pre @p out_pclka_hz is writable.
 * @post On success the console prints and SCI0 Simple-SPI is configured at the
 *       SD init clock, CS deasserted.
 * @post `*out_pclka_hz` holds the PCLKA rate on success.
 *
 * @note Not thread-safe; call once from the single-threaded init path.
 * @since 0.1.0
 */
static void sd_demo_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    sd_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    sd_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    sd_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    sd_demo_panic_halt();
  }
  if (sd_demo_console_pins_init() != k_ra_ok) {
    sd_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = (uint32_t)k_sd_demo_uart_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_sd_demo_uart_channel, &sci_cfg) != k_ra_ok) {
    sd_demo_panic_halt();
  }
  if (sd_demo_spi_pins_init() != k_ra_ok) {
    sd_demo_panic_halt();
  }
  const ra_sci_spi_cfg_t spi_cfg = {
    .baud_hz   = (uint32_t)k_ra_sdmmc_spi_clock_init_hz,
    .pclk_hz   = pclka_hz,
    .mode      = k_ra_spi_mode_0,
    .lsb_first = false,
  };
  if (ra_sci_spi_init((uint8_t)k_sd_demo_spi_channel, &spi_cfg) != k_ra_ok) {
    sd_demo_panic_halt();
  }
  *out_pclka_hz = pclka_hz;
}

/**
 * @brief Init the SD driver, panic-halt with a diagnostic on failure.
 *
 * @details Builds the transport vtable over the SPI adapters, runs the SD SPI
 *          bring-up (`ra_sdmmc_spi_init`), and prints `card ready` on success or
 *          a `FAIL init` diagnostic before parking on failure.
 *
 * @param[in] pclka_hz Cached PCLKA (Hz) bound into the transport ctx.
 *
 * @return Nothing (panic-halts on failure).
 *
 * @pre SCI0 Simple-SPI + CS GPIO are configured.
 * @pre @p pclka_hz points to the live PCLKA rate.
 * @post On success the SD card is in SPI mode at default speed.
 * @post On failure a diagnostic is printed and the CPU is parked.
 *
 * @note Not thread-safe; call once after the SPI bus is up.
 * @since 0.1.0
 */
static void sd_demo_init_card_or_halt(uint32_t* pclka_hz)
{
  const ra_sdmmc_spi_transport_t transport = {
    .set_clock = sd_demo_spi_set_clock,
    .cs        = sd_demo_spi_cs,
    .xfer      = sd_demo_spi_xfer,
    .ctx       = pclka_hz,
  };
  if (ra_sdmmc_spi_init(&transport) != k_ra_ok) {
    SD_DEMO_PUTS(k_msg_init_fail);
    sd_demo_panic_halt();
  }
  SD_DEMO_PUTS(k_msg_card_ok);
}

/* =============================================================================
 * Payload + ra_io round-trip
 * =============================================================================
 */

/** @brief Static payload + read-back buffers (no heap; NASA Rule 3). */
static uint8_t s_payload[k_sd_demo_payload_bytes];
static uint8_t s_readback[k_sd_demo_payload_bytes];

/** @brief ra_io block device + ra_fs backend bridged onto the SD card. */
static ra_io_blockdev_t s_bd;
static ra_fs_backend_t  s_be;

/**
 * @brief Fill the payload buffer with a deterministic LCG byte sequence.
 *
 * @details Runs a Numerical-Recipes LCG over `s_payload`, selecting one byte per
 *          step, so the written content is reproducible and the read-back compare
 *          is a strong end-to-end check of the whole stack.
 *
 * @return Nothing.
 *
 * @pre `s_payload` is allocated (file-scope, always true).
 * @pre The LCG constants are non-zero.
 * @post `s_payload` holds a reproducible byte pattern.
 * @post No other state is modified.
 *
 * @note Not thread-safe; mutates the shared payload buffer.
 * @since 0.1.0
 */
static void sd_demo_fill_payload(void)
{
  uint32_t state = (uint32_t)k_sd_demo_prng_seed;
  for (uint32_t i = 0U; i < (uint32_t)k_sd_demo_payload_bytes; i++) {
    state        = (state * (uint32_t)k_sd_demo_prng_mul) + (uint32_t)k_sd_demo_prng_add;
    s_payload[i] = (uint8_t)((state >> k_sd_demo_prng_byte_shift) & k_sd_demo_byte_mask);
  }
}

/**
 * @brief Bind the SD-SPI block device, bridge it to ra_fs, format + mount FAT16,
 *        and register it in the VFS under `"sd"`.
 *
 * @details This is the "swappable backend" core: the only difference from
 *          `ra_io_demo` is that the block device here is the SD-over-SPI backend
 *          rather than the RAM backend. Everything above (`ra_fs` + VFS) is the
 *          identical API. The mount produced by `ra_fs_mount` is handed to the
 *          VFS, which keeps the pointer, so the local is intentionally not
 *          returned.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok    Backend bound, FAT16 formatted + mounted + VFS-registered.
 * @retval k_ra_err_* The first failing fabric step's code.
 *
 * @pre The SD card is initialised (`ra_sdmmc_spi_init` succeeded).
 * @pre The VFS mount table has a free slot named distinctly from `"sd"`.
 * @post On success `"sd:/..."` paths resolve to the SD volume.
 * @post On any non-ok return the partial state is abandoned (caller halts).
 *
 * @note Not thread-safe; single-threaded init path only.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sd_demo_mount_via_io(void)
{
  RA_RETURN_ON_ERROR(ra_io_vfs_init(), s_tag, "vfs init");
  RA_RETURN_ON_ERROR(ra_io_blockdev_sdspi_init(&s_bd), s_tag, "sdspi bind");
  RA_RETURN_ON_ERROR(ra_io_blockdev_as_fs_backend(&s_bd, &s_be), s_tag, "fs bridge");
  ra_fs_format_opts_t opts = {};
  opts.type                = k_ra_fs_type_fat16;
  opts.label               = "RAIOSD";
  RA_RETURN_ON_ERROR(ra_fs_format(&s_be, &opts), s_tag, "format");
  ra_fs_mount_t* mnt = nullptr;
  RA_RETURN_ON_ERROR(ra_fs_mount(&s_be, &mnt), s_tag, "mount");
  RA_RETURN_ON_ERROR(ra_io_vfs_mount(k_mount_name, mnt), s_tag, "vfs mount");
  return k_ra_ok;
}

/**
 * @brief Write `s_payload` to the VFS path through `ra_io_vfs_open` + `ra_fs_write`.
 *
 * @details Opens the file for writing via the named VFS path, streams the full
 *          payload, and closes it. The whole-file content is the deterministic
 *          LCG pattern, so a later read-back compare validates the path.
 *
 * @param[in] path `"sd:/..."` VFS file path to create.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok    Payload written and the file closed.
 * @retval k_ra_err_* ra_io open / write / close failure.
 *
 * @pre The `"sd"` volume is mounted and `s_payload` is filled.
 * @pre @p path is a valid `"name:/path"` string.
 * @post On success @p path holds `s_payload`.
 * @post The file handle is released on every path.
 *
 * @note Not thread-safe; mutates the shared SD volume.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sd_demo_write_payload(const char* path)
{
  ra_fs_file_t* wf = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open(path, k_ra_fs_mode_write, &wf), s_tag, "open w");
  ra_err_t err = ra_fs_write(wf, s_payload, (uint32_t)k_sd_demo_payload_bytes);
  if (err != k_ra_ok) {
    (void)ra_fs_close(wf);
    return err;
  }
  return ra_fs_close(wf);
}

/**
 * @brief Read the VFS file back and byte-compare against `s_payload`.
 *
 * @details Opens the file for reading via the named VFS path, reads up to the
 *          payload length, closes it, then verifies both the returned length and
 *          the full byte content match the written payload.
 *
 * @param[in] path `"sd:/..."` VFS file path to read.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                    Read-back length and content both matched.
 * @retval k_ra_err_invalid_size      The returned length differed.
 * @retval k_ra_err_checksum_mismatch The byte content differed.
 * @retval k_ra_err_*                 ra_io open / read / close failure.
 *
 * @pre The `"sd"` volume is mounted and @p path was written with `s_payload`.
 * @pre @p path is a valid `"name:/path"` string.
 * @post `s_readback` holds the bytes read from @p path.
 * @post The file handle is released on every path.
 *
 * @note Not thread-safe; mutates the shared read-back buffer.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sd_demo_read_and_verify(const char* path)
{
  ra_fs_file_t* rf = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open(path, k_ra_fs_mode_read, &rf), s_tag, "open r");
  memset(s_readback, 0, sizeof(s_readback));
  uint32_t got = 0U;
  ra_err_t err = ra_fs_read(rf, s_readback, (uint32_t)k_sd_demo_payload_bytes, &got);
  (void)ra_fs_close(rf);
  if (err != k_ra_ok) {
    return err;
  }
  if (got != (uint32_t)k_sd_demo_payload_bytes) {
    return k_ra_err_invalid_size;
  }
  if (memcmp(s_payload, s_readback, (size_t)k_sd_demo_payload_bytes) != 0) {
    return k_ra_err_checksum_mismatch;
  }
  return k_ra_ok;
}

/**
 * @brief Run the full ra_io round-trip: mount, mkdir, write, read-back, verify.
 *
 * @details Composes the helpers in order so the success path is a single line
 *          per step and the first failing step short-circuits with its code. The
 *          VFS mkdir exercises the new directory-creation path on the SD volume.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok    Mount + mkdir + write + read-back + verify all passed.
 * @retval k_ra_err_* The first failing step's code.
 *
 * @pre The SD card is initialised and `s_payload` is filled.
 * @pre The VFS mount table is empty or has a free slot.
 * @post On success the SD card holds the verified file under `sd:/LOGS`.
 * @post On any non-ok return the partial state is abandoned (caller halts).
 *
 * @note Not thread-safe; single-threaded round-trip.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sd_demo_roundtrip(void)
{
  RA_RETURN_ON_ERROR(sd_demo_mount_via_io(), s_tag, "mount via io");
  RA_RETURN_ON_ERROR(ra_io_vfs_mkdir(k_dir_path), s_tag, "mkdir");
  RA_RETURN_ON_ERROR(sd_demo_write_payload(k_file_path), s_tag, "write payload");
  RA_RETURN_ON_ERROR(sd_demo_read_and_verify(k_file_path), s_tag, "read verify");
  return k_ra_ok;
}

/* =============================================================================
 * Main
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: bring up the bus + card, run the ra_io round-trip, print PASS.
 *
 * @details Brings up the clocks, console, SPI, and SD card, fills the payload,
 *          then runs the full `ra_io` VFS round-trip over the SD-over-SPI block
 *          device. On success it prints the exact PASS banner the HIL runner and
 *          board_sim smoke gate scrape for; on any failure it prints `FAIL` and
 *          parks the core.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On a clean run the CPU loops forever after the PASS banner.
 * @post On any failure the function prints `FAIL` and halts in WFI.
 *
 * @note Not thread-safe; this is the single-threaded app entry.
 * @since 0.1.0
 */
int main(void)
{
  uint32_t pclka_hz = 0U;
  sd_demo_setup_or_halt(&pclka_hz);
  ra_isr_globals_enable();
  ra_log_init();
  SD_DEMO_PUTS(k_msg_boot);

  sd_demo_init_card_or_halt(&pclka_hz);
  sd_demo_fill_payload();

  const ra_err_t r = sd_demo_roundtrip();
  if (r != k_ra_ok) {
    SD_DEMO_PUTS(k_msg_fail);
    sd_demo_panic_halt();
  }
  SD_DEMO_PUTS(k_msg_pass);
  (void)ra_sci_flush((uint8_t)k_sd_demo_uart_channel);

  while (true) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
