/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ra8_io_sd_demo/main.c
 * @brief Prove the ra8_io fabric's swappable backend (#155/#156) over a microSD.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Companion to `ra8_io_demo`, which runs the `ra8_io` VFS over a RAM disk. This
 * app runs the IDENTICAL `ra8_io` VFS API over a real micro-SD card by swapping
 * only the block-device backend: instead of the RAM backend it binds the
 * `ra8_io` SD-over-SPI block device (`ra8_io_blockdev_sdspi_init`) on top of the
 * `ra8_sdmmc_spi` driver. The Pmod2 / SCI0 Simple-SPI bus comes up in one library
 * call -- `ra8_sdmmc_spi_transport_sci` routes the four pins, claims CS, brings up
 * SCI0 Simple-SPI, and fills the transport vtable -- matching the SDHI demo's
 * one-line `ra8_sdcard_init` ergonomics. The data path above the block device is
 * this app's self-contained `ra8_io_roundtrip.{h,c}` helper (the SDRAM / OSPI / SD
 * demos under hw_pending carry their own copy) -- this app differs only by the
 * SD-SPI transport factory call plus the ONE backend bind line and its own PASS banner.
 *
 * On a clean round-trip it prints exactly
 * `ra8_io_sd_demo: sd:/LOGS/A.TXT 512 bytes PASS`; any failed step prints
 * `ra8_io_sd_demo: FAIL` and parks the CPU. The HIL runner and the board_sim
 * smoke gate scrape for that PASS line.
 *
 * Required external hardware (on-bench): Digilent PMOD MicroSD (part 410-380)
 * in Pmod2 (J25) with any microSD card inserted. THIS APP ERASES THE CARD: it
 * self-formats a fresh FAT32 volume, so any blank card works (FAT32 handles the
 * multi-GB cards a real SD slot sees -- FAT16's 2 GB ceiling does not).
 * Under board_sim attach a blank card with `--sd-new 64:fat16` (the firmware
 * reformats it FAT32 regardless of the seed type; only the capacity matters).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io.h"
#include "ra8_io_roundtrip.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_port_utils.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_time.h"

/* =============================================================================
 * Tunables
 * =============================================================================
 */

/**
 * @enum sd_demo_config_t
 * @brief Compile-time settings for the ra8_io SD-backend demo.
 */
typedef enum : uint32_t {
  k_sd_demo_uart_baud     = 115200U, /**< J-Link OB CDC console baud.            */
  k_sd_demo_spi_channel   = 0U,      /**< Pmod2 / J25 SCI0 Simple-SPI.           */
  k_sd_demo_payload_bytes = 512U,    /**< One-sector deterministic test payload. */
} sd_demo_config_t;

/* =============================================================================
 * Pinout (Pmod2 SPI for SD card)
 * =============================================================================
 */

/** @brief Pmod2 SPI pins (J25) -- SCI0 Simple-SPI; CS held by GPIO. */
static const ra8_port_pin_t k_sd_demo_pin_sck  = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck;
static const ra8_port_pin_t k_sd_demo_pin_cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo;
static const ra8_port_pin_t k_sd_demo_pin_copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi;
static const ra8_port_pin_t k_sd_demo_pin_cs   = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs;

/* =============================================================================
 * Static message strings (ASCII-only per project policy)
 * =============================================================================
 */

static const uint8_t k_msg_boot[]      = "ra8_io_sd_demo: boot\r\n";
static const uint8_t k_msg_card_ok[]   = "ra8_io_sd_demo: card ready\r\n";
static const uint8_t k_msg_init_fail[] = "ra8_io_sd_demo: FAIL init\r\n";
static const uint8_t k_msg_pass[]      = "ra8_io_sd_demo: sd:/LOGS/A.TXT 512 bytes PASS\r\n";
static const uint8_t k_msg_fail[]      = "ra8_io_sd_demo: FAIL\r\n";

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_sd_demo";

/** @brief ra8_io block device + ra8_fs backend bridged onto the SD card. */
static ra8_io_blockdev_t s_bd;
static ra8_fs_backend_t  s_be;

/** @brief Shared round-trip parameters for this SD-backed FAT32 volume. */
static const ra8_io_roundtrip_params_t s_params = {
  .vfs_prefix   = "sd",
  .fat_type     = k_ra8_fs_type_fat32,
  .volume_label = "RAIOSD",
  .log_tag      = s_tag,
  .root_file    = "",
  .root_path    = "",
  .root_bytes   = 0U,
  .subdir_path  = "sd:/LOGS",
  .subdir_file  = "sd:/LOGS/A.TXT",
  .subdir_bytes = (uint32_t)k_sd_demo_payload_bytes,
};

/* =============================================================================
 * UART output helpers
 * =============================================================================
 */

/**
 * @brief Write a byte run on the SCI8 console.
 *
 * @details Thin wrapper over `ra8_board_uart_console_write` so the demo's print
 *          sites stay terse; the return value is intentionally discarded because
 *          a diagnostic-print failure has no useful recovery on a panic path.
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
  (void)ra8_board_uart_console_write(msg, (size_t)len);
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
 * Hardware bring-up
 * =============================================================================
 */

/**
 * @brief Bring up CGC + SysTick + console SCI; panic on fail.
 *
 * @details Initialises the clock generator, caches CPUCLK0 and PCLKA, starts the
 *          SysTick time base, and brings up the SCI8 J-Link console via the
 *          board-support package (`ra8_board_uart_console_init`), which routes
 *          PD02 TXD / PD03 RXD and configures SCI8 async at the given baud.
 *          The Pmod2 SD SPI bus is brought up later by the `ra8_sdmmc_spi` SCI
 *          transport factory, so this routine only owns the clocks + console.
 *          Any failing step panic-halts so a misconfigured console never reaches
 *          the SD bring-up.
 *
 * @param[out] out_pclka_hz Cached PCLKA rate (Hz) handed to the transport factory.
 *
 * @return Nothing (panic-halts on failure).
 *
 * @pre Reset_Handler initialised .data/.bss.
 * @pre @p out_pclka_hz is writable.
 * @post On success the console prints and `*out_pclka_hz` holds the PCLKA rate.
 * @post On any failure the CPU is parked in WFI.
 *
 * @note Not thread-safe; call once from the single-threaded init path.
 * @since 0.1.0
 */
static void sd_demo_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    sd_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    sd_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) {
    sd_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    sd_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_sd_demo_uart_baud) != k_ra8_ok) {
    sd_demo_panic_halt();
  }
  *out_pclka_hz = pclka_hz;
}

/**
 * @brief Build the SCI-SPI transport, init the SD driver, panic-halt on failure.
 *
 * @details The whole SD-SPI bring-up now collapses to two library calls: the
 *          `ra8_sdmmc_spi` SCI transport factory routes the four Pmod2 pins,
 *          claims CS, brings up SCI0 Simple-SPI, and fills the transport vtable;
 *          `ra8_sdmmc_spi_init` then runs the SD identification sequence. Prints
 *          `card ready` on success or a `FAIL init` diagnostic before parking.
 *
 * @param[in] pclka_hz Live PCLKA rate (Hz) feeding the SCI baud divider.
 *
 * @return Nothing (panic-halts on failure).
 *
 * @pre `ra8_cgc_init` has run and the console SCI is up.
 * @pre @p pclka_hz is the live PCLKA rate.
 * @post On success the SD card is in SPI mode at default speed.
 * @post On failure a diagnostic is printed and the CPU is parked.
 *
 * @note Not thread-safe; call once after the clocks + console are up.
 * @since 0.1.0
 */
static void sd_demo_init_card_or_halt(uint32_t pclka_hz)
{
  const ra8_sdmmc_spi_sci_pins_t pins = {
    .sck  = k_sd_demo_pin_sck,
    .cipo = k_sd_demo_pin_cipo,
    .copi = k_sd_demo_pin_copi,
    .cs   = k_sd_demo_pin_cs,
  };
  ra8_sdmmc_spi_transport_t transport = {};
  if (ra8_sdmmc_spi_transport_sci((uint8_t)k_sd_demo_spi_channel, pclka_hz, &pins, &transport) !=
      k_ra8_ok) {
    SD_DEMO_PUTS(k_msg_init_fail);
    sd_demo_panic_halt();
  }
  if (ra8_sdmmc_spi_init(&transport) != k_ra8_ok) {
    SD_DEMO_PUTS(k_msg_init_fail);
    sd_demo_panic_halt();
  }
  SD_DEMO_PUTS(k_msg_card_ok);
}

/* =============================================================================
 * ra8_io round-trip
 * =============================================================================
 */

/**
 * @brief Bind the SD-SPI block device and run the shared ra8_io round-trip.
 *
 * @details This is the "swappable backend" core: the only difference from the
 *          RAM / SDRAM / OSPI demos is that the block device bound here is the
 *          SD-over-SPI backend. After binding, the shared helper formats + mounts
 *          a FAT32 volume under `"sd"` and runs the mkdir + nested round-trip into
 *          `sd:/LOGS/A.TXT`. The VFS is initialised first because this app does
 *          not rely on an earlier mount having done so.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Backend bound, FAT32 formatted + mounted, round-trip passed.
 * @retval k_ra8_err_* The first failing fabric step's code.
 *
 * @pre The SD card is initialised (`ra8_sdmmc_spi_init` succeeded).
 * @pre The VFS mount table has a free slot named distinctly from `"sd"`.
 * @post On success `sd:/LOGS/A.TXT` holds the verified payload.
 * @post No file handle is left open on any return path.
 *
 * @note Not thread-safe; single-threaded init path only.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t sd_demo_roundtrip(void)
{
  RA8_RETURN_ON_ERROR(ra8_io_vfs_init(), s_tag, "vfs init");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_sdspi_init(&s_bd), s_tag, "sdspi bind");
  ra8_fs_mount_t* mnt = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_roundtrip_mount(&s_bd, &s_params, &s_be, &mnt), s_tag, "mount");
  return ra8_io_roundtrip_subdir_file(&s_params);
}

/* =============================================================================
 * Main
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: bring up the bus + card, run the ra8_io round-trip, print PASS.
 *
 * @details Brings up the clocks, console, SPI, and SD card, then runs the shared
 *          `ra8_io` VFS round-trip over the SD-over-SPI block device. On success
 *          it prints the exact PASS banner the HIL runner and board_sim smoke
 *          gate scrape for; on any failure it prints `FAIL` and parks the core.
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
  ra8_isr_globals_enable();
  ra8_log_init();
  SD_DEMO_PUTS(k_msg_boot);

  sd_demo_init_card_or_halt(pclka_hz);

  const ra8_err_t r = sd_demo_roundtrip();
  if (r != k_ra8_ok) {
    SD_DEMO_PUTS(k_msg_fail);
    sd_demo_panic_halt();
  }
  SD_DEMO_PUTS(k_msg_pass);
  (void)ra8_board_uart_console_flush();

  while (true) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
