/**
 * @file examples/ek_ra8d2/hw_pending/ra8_io_sdhi_demo/src/main.c
 * @brief Prove the ra8_io fabric's swappable backend over native SDHI.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The native-SDHI sibling of `ra8_io_sd_demo`. That app runs the `ra8_io` VFS over
 * a micro-SD card clocked byte-by-byte through an SCI Simple-SPI bus; this app
 * runs the IDENTICAL `ra8_io` VFS API over the SAME card but reached through the
 * dedicated 4-bit **SDHI** controller. Only the block-device backend changes:
 * instead of `ra8_io_blockdev_sdspi_init` it binds the native-SDHI block device
 * (`ra8_io_blockdev_sdhi_init`) on top of the `ra8_sdcard` + `ra8_sdhi` HAL drivers.
 * The data path above the block device is pure `ra8_io`:
 *
 *   1. Route the six SDHI1_B pins (port 4, pins 0..5) via `ra8_board_sdhi_pins_init`.
 *   2. `ra8_sdcard_init({.instance = 1})` -- full SD identification + clock step-up.
 *   3. `sdhi_demo_run()` (implemented in `sdhi_demo.zig`):
 *      - `ra8_io_blockdev_sdhi_init` -- native-SDHI block-device vtable over the card.
 *      - `ra8_io_blockdev_as_fs_backend` -- bridge the block device to `ra8_fs`.
 *      - `ra8_fs_mount` (formatting FAT16 only when blank), then `ra8_io_vfs_mount("sd", ...)`.
 *      - `ra8_io_vfs_listdir("sd:/", ...)` -- print one line per root entry (name and size).
 *      - `ra8_io_vfs_mkdir("sd:/LOGS")` -- ensure the target directory exists.
 *      - `ra8_io_vfs_open("sd:/LOGS/A.TXT", write)` + `ra8_fs_write` -- write payload.
 *      - `ra8_io_vfs_open("sd:/LOGS/A.TXT", read)` + `ra8_fs_read` -- read it back.
 *      - byte-compare the read-back against the deterministic payload.
 *
 * On a clean round-trip it prints exactly
 * `ra8_io_sdhi_demo: sd:/LOGS/A.TXT 512 bytes PASS`; any failed step prints
 * `ra8_io_sdhi_demo: FAIL` and parks the CPU. The HIL runner and the ra8_emulator
 * smoke gate scrape for that PASS line.
 *
 * Required external hardware (on-bench): a microSD card in the on-board SDHI
 * slot. Existing FAT cards are mounted in place without reformatting; blank
 * cards are formatted FAT16 (`RAIOSD`). Under ra8_emulator attach a blank card
 * with `--sd-new 64:fat16` or a fixture folder with `--sd-dir DIR`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "fw_if_clock.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_clock_profile.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_sdcard.h"
#include "ra8_time.h"

/* =============================================================================
 * Tunables
 * =============================================================================
 */

/**
 * @enum sdhi_demo_config_t
 * @brief Compile-time settings for the ra8_io native-SDHI backend demo.
 */
typedef enum : uint32_t {
  k_sdhi_demo_uart_baud = 115200U, /**< J-Link OB CDC console baud.      */
  k_sdhi_demo_instance  = 1U,      /**< SDHI1_B drives the micro-SD bus. */
} sdhi_demo_config_t;

/* =============================================================================
 * Static message strings (ASCII-only per project policy)
 * =============================================================================
 */

static const uint8_t k_msg_boot[]      = "ra8_io_sdhi_demo: boot\r\n";
static const uint8_t k_msg_card_ok[]   = "ra8_io_sdhi_demo: card ready\r\n";
static const uint8_t k_msg_init_fail[] = "ra8_io_sdhi_demo: FAIL init\r\n";
static const uint8_t k_msg_pass[]      = "ra8_io_sdhi_demo: sd:/LOGS/A.TXT 512 bytes PASS\r\n";
static const uint8_t k_msg_fail[]      = "ra8_io_sdhi_demo: FAIL\r\n";

/* =============================================================================
 * UART output helpers
 * =============================================================================
 */

/**
 * @brief Write a byte run on the J-Link OB VCOM console.
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
 * @pre The board console is initialised.
 * @pre @p msg points to at least @p len readable bytes.
 * @post The bytes are queued to the console.
 * @post No other state is modified.
 *
 * @note Not thread-safe; call from the single-threaded app context.
 * @since 0.1.0
 */
static void sdhi_demo_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Emit a NUL-terminated literal (length via sizeof at the call site). */
#define SDHI_DEMO_PUTS(lit) sdhi_demo_print((lit), (uint32_t)sizeof(lit) - 1U)

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
static void sdhi_demo_panic_halt(void)
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
 * @brief Bring up CGC + SysTick + the board console + SDHI bus pins; panic on fail.
 *
 * @details Initialises the clock generator, caches CPUCLK0, starts the SysTick
 *          time base, brings the J-Link OB VCOM console up at 115200 8N1 via the
 *          BSP, then routes the six SDHI1_B bus pins via
 *          ``ra8_board_sdhi_pins_init``. Any failing step panic-halts so a
 *          misconfigured bus never reaches the SD bring-up.
 *
 * @return Nothing (panic-halts on failure).
 *
 * @pre Reset_Handler initialised .data/.bss.
 * @pre No other consumer owns the port-4 SDHI pins.
 * @post On success the console prints and the SDHI pins are routed.
 * @post On any failing step the core is parked in WFI.
 *
 * @note Not thread-safe; call once from the single-threaded init path.
 * @since 0.1.0
 */
static void sdhi_demo_setup_or_halt(void)
{
  const ra8_board_bringup_cfg_t bringup_cfg = {
    .console_baud = (uint32_t)k_sdhi_demo_uart_baud,
  };
  ra8_board_bringup_out_t bringup_out = {};
  if (ra8_board_bringup(&bringup_cfg, &bringup_out) != k_ra8_ok) {
    sdhi_demo_panic_halt();
  }
  if (ra8_board_sdhi_pins_init() != k_ra8_ok) {
    sdhi_demo_panic_halt();
  }
}

/**
 * @brief Init the SD card over native SDHI, panic-halt on failure.
 *
 * @details Runs the full SD Physical Layer identification through `ra8_sdcard_init`
 *          on SDHI1 (which itself brings up the SDHI block), printing `card ready`
 *          on success or a `FAIL init` diagnostic before parking on failure.
 *
 * @return Nothing (panic-halts on failure).
 *
 * @pre The six SDHI1_B bus pins are routed.
 * @pre A card is present in the SDHI slot.
 * @post On success the card is in TRAN state at default speed.
 * @post On failure a diagnostic is printed and the CPU is parked.
 *
 * @note Not thread-safe; call once after the SDHI pins are routed.
 * @since 0.1.0
 */
static void sdhi_demo_init_card_or_halt(void)
{
  const ra8_sdcard_cfg_t cfg = {.instance = (uint8_t)k_sdhi_demo_instance};
  if (ra8_sdcard_init(&cfg) != k_ra8_ok) {
    SDHI_DEMO_PUTS(k_msg_init_fail);
    sdhi_demo_panic_halt();
  }
  SDHI_DEMO_PUTS(k_msg_card_ok);
}

/* =============================================================================
 * Zig storage + VFS round-trip (src/sdhi_demo.zig)
 * =============================================================================
 */

/**
 * @brief Mount (formatting FAT16 only when blank), list `sd:/`, and round-trip
 *        `sd:/LOGS/A.TXT` via `ra8_io`.
 *
 * @return ra8_err_t Error code (`k_ra8_ok` on success).
 */
[[nodiscard]] extern ra8_err_t sdhi_demo_run(void);

/* =============================================================================
 * Main
 * =============================================================================
 */

/**
 * @brief App entry: bring up the bus + card, run the ra8_io round-trip, print PASS.
 *
 * @details Brings up the clocks, console, and SDHI bus pins, runs the native SD
 *          card identification, then calls `sdhi_demo_run()` (implemented in
 *          `sdhi_demo.zig`) to mount or format, list root entries, and verify the
 *          `sd:/LOGS/A.TXT` round-trip. On success it prints the exact PASS
 *          banner the HIL runner and ra8_emulator smoke gate scrape for; on any
 *          failure it prints `FAIL` and parks the core.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On a clean run the CPU loops forever after the PASS banner.
 * @post On any failure the function prints `FAIL` and halts in WFI.
 *
 * @note Not thread-safe; this is the single-threaded app entry.
 * @since 0.1.0
 */
void main(void)
{
  sdhi_demo_setup_or_halt();
  ra8_isr_globals_enable();
  ra8_log_init();
  SDHI_DEMO_PUTS(k_msg_boot);

  sdhi_demo_init_card_or_halt();

  const ra8_err_t r = sdhi_demo_run();
  if (r != k_ra8_ok) {
    SDHI_DEMO_PUTS(k_msg_fail);
    sdhi_demo_panic_halt();
  }
  SDHI_DEMO_PUTS(k_msg_pass);
  (void)ra8_board_uart_console_flush();

  while (true) {
    __asm__ volatile("wfi");
  }
}
