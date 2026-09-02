/**
 * @file examples/ek_ra8d2/hw_pending/ra8_io_sdhi_demo/src/main.c
 * @brief Prove the ra8_io fabric's swappable backend (#155/#156) over native SDHI.
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
 *   1. Route the eight SDHI pins (port 4, pins 0..7) via `ra8_board_sdhi_pins_init`.
 *   2. `ra8_sdcard_init({.instance = 0})` -- full SD identification + clock step-up.
 *   3. `ra8_io_blockdev_sdhi_init` -- native-SDHI block-device vtable over the card.
 *   4. `ra8_io_blockdev_as_fs_backend` -- bridge the block device to `ra8_fs`.
 *   5. `ra8_fs_format` (FAT16) + `ra8_fs_mount`, then `ra8_io_vfs_mount("sd", ...)`.
 *   6. `ra8_io_vfs_mkdir("sd:/LOGS")` -- exercises the VFS mkdir path.
 *   7. `ra8_io_vfs_open("sd:/LOGS/A.TXT", write)` + `ra8_fs_write` -- write payload.
 *   8. `ra8_io_vfs_open("sd:/LOGS/A.TXT", read)` + `ra8_fs_read` -- read it back.
 *   9. byte-compare the read-back against the deterministic payload.
 *
 * On a clean round-trip it prints exactly
 * `ra8_io_sdhi_demo: sd:/LOGS/A.TXT 512 bytes PASS`; any failed step prints
 * `ra8_io_sdhi_demo: FAIL` and parks the CPU. The HIL runner and the ra8_emulator
 * smoke gate scrape for that PASS line.
 *
 * Required external hardware (on-bench): a microSD card in the on-board SDHI
 * slot. THIS APP ERASES THE CARD. Under ra8_emulator attach a blank card with
 * `--sd-new 64:fat16`; the native-SDHI host-controller model serves it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_sdcard.h"
#include "ra8_sdhi.h"
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
  k_sdhi_demo_uart_baud       = 115200U,      /**< J-Link OB CDC console baud.            */
  k_sdhi_demo_instance        = 0U,           /**< SDHI0 drives the micro-SD bus.         */
  k_sdhi_demo_payload_bytes   = 512U,         /**< One-sector deterministic test payload. */
  k_sdhi_demo_prng_seed       = 0xA5F00DadUL, /**< Deterministic payload seed.            */
  k_sdhi_demo_prng_mul        = 1664525UL,    /**< Numerical Recipes LCG multiplier.      */
  k_sdhi_demo_prng_add        = 1013904223UL, /**< Numerical Recipes LCG increment.       */
  k_sdhi_demo_prng_byte_shift = 16U,          /**< Bit shift selecting the PRNG byte.     */
  k_sdhi_demo_byte_mask       = 0xFFU,        /**< Low-byte mask.                         */
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

/** @brief VFS mount name and target path for the round-trip file. */
static const char k_mount_name[] = "sd";
static const char k_dir_path[]   = "sd:/LOGS";
static const char k_file_path[]  = "sd:/LOGS/A.TXT";

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_sdhi_demo";

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
 *          BSP, then routes the eight SDHI0 bus pins via
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
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    sdhi_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    sdhi_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    sdhi_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_sdhi_demo_uart_baud) != k_ra8_ok) {
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
 *          on SDHI0 (which itself brings up the SDHI block), printing `card ready`
 *          on success or a `FAIL init` diagnostic before parking on failure.
 *
 * @return Nothing (panic-halts on failure).
 *
 * @pre The eight SDHI bus pins are routed.
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
 * Payload + ra8_io round-trip
 * =============================================================================
 */

/** @brief Static payload + read-back buffers (no heap; NASA Rule 3). */
static uint8_t s_payload[k_sdhi_demo_payload_bytes];
static uint8_t s_readback[k_sdhi_demo_payload_bytes];

/** @brief ra8_io block device + ra8_fs backend bridged onto the SD card. */
static ra8_io_blockdev_t s_bd;
static ra8_fs_backend_t  s_be;

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
static void sdhi_demo_fill_payload(void)
{
  uint32_t state = (uint32_t)k_sdhi_demo_prng_seed;
  for (uint32_t i = 0U; i < (uint32_t)k_sdhi_demo_payload_bytes; i++) {
    state        = (state * (uint32_t)k_sdhi_demo_prng_mul) + (uint32_t)k_sdhi_demo_prng_add;
    s_payload[i] = (uint8_t)((state >> k_sdhi_demo_prng_byte_shift) & k_sdhi_demo_byte_mask);
  }
}

/**
 * @brief Bind the native-SDHI block device, bridge it to ra8_fs, format + mount
 *        FAT16, and register it in the VFS under `"sd"`.
 *
 * @details This is the "swappable backend" core: the only difference from
 *          `ra8_io_sd_demo` is that the block device here is the native-SDHI
 *          backend rather than the SD-over-SPI backend. Everything above
 *          (`ra8_fs` + VFS) is the identical API. The mount produced by
 *          `ra8_fs_mount` is handed to the VFS, which keeps the pointer, so the
 *          local is intentionally not returned.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Backend bound, FAT16 formatted + mounted + VFS-registered.
 * @retval k_ra8_err_* The first failing fabric step's code.
 *
 * @pre The SD card is initialised (`ra8_sdcard_init` succeeded).
 * @pre The VFS mount table has a free slot named distinctly from `"sd"`.
 * @post On success `"sd:/..."` paths resolve to the SD volume.
 * @post On any non-ok return the partial state is abandoned (caller halts).
 *
 * @note Not thread-safe; single-threaded init path only.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t sdhi_demo_mount_via_io(void)
{
  RA8_RETURN_ON_ERROR(ra8_io_vfs_init(), s_tag, "vfs init");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_sdhi_init(&s_bd), s_tag, "sdhi bind");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_as_fs_backend(&s_bd, &s_be), s_tag, "fs bridge");
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "RAIOSD";
  RA8_RETURN_ON_ERROR(ra8_fs_format(&s_be, &opts), s_tag, "format");
  ra8_fs_mount_t* mnt = nullptr;
  RA8_RETURN_ON_ERROR(ra8_fs_mount(&s_be, &mnt), s_tag, "mount");
  RA8_RETURN_ON_ERROR(ra8_io_vfs_mount(k_mount_name, mnt), s_tag, "vfs mount");
  return k_ra8_ok;
}

/**
 * @brief Write `s_payload` to the VFS path through `ra8_io_vfs_open` + `ra8_fs_write`.
 *
 * @details Opens the file for writing via the named VFS path, streams the full
 *          payload, and closes it. The whole-file content is the deterministic
 *          LCG pattern, so a later read-back compare validates the path.
 *
 * @param[in] path `"sd:/..."` VFS file path to create.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Payload written and the file closed.
 * @retval k_ra8_err_* ra8_io open / write / close failure.
 *
 * @pre The `"sd"` volume is mounted and `s_payload` is filled.
 * @pre @p path is a valid `"name:/path"` string.
 * @post On success @p path holds `s_payload`.
 * @post The file handle is released on every path.
 *
 * @note Not thread-safe; mutates the shared SD volume.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t sdhi_demo_write_payload(const char* path)
{
  ra8_fs_file_t* wf = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_open(path, k_ra8_fs_mode_write, &wf), s_tag, "open w");
  ra8_err_t err = ra8_fs_write(wf, s_payload, (uint32_t)k_sdhi_demo_payload_bytes);
  if (err != k_ra8_ok) {
    (void)ra8_fs_close(wf);
    return err;
  }
  return ra8_fs_close(wf);
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
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Read-back length and content both matched.
 * @retval k_ra8_err_invalid_size      The returned length differed.
 * @retval k_ra8_err_checksum_mismatch The byte content differed.
 * @retval k_ra8_err_*                 ra8_io open / read / close failure.
 *
 * @pre The `"sd"` volume is mounted and @p path was written with `s_payload`.
 * @pre @p path is a valid `"name:/path"` string.
 * @post `s_readback` holds the bytes read from @p path.
 * @post The file handle is released on every path.
 *
 * @note Not thread-safe; mutates the shared read-back buffer.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t sdhi_demo_read_and_verify(const char* path)
{
  ra8_fs_file_t* rf = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_open(path, k_ra8_fs_mode_read, &rf), s_tag, "open r");
  memset(s_readback, 0, sizeof(s_readback));
  uint32_t  got = 0U;
  ra8_err_t err = ra8_fs_read(rf, s_readback, (uint32_t)k_sdhi_demo_payload_bytes, &got);
  (void)ra8_fs_close(rf);
  if (err != k_ra8_ok) {
    return err;
  }
  if (got != (uint32_t)k_sdhi_demo_payload_bytes) {
    return k_ra8_err_invalid_size;
  }
  if (memcmp(s_payload, s_readback, (size_t)k_sdhi_demo_payload_bytes) != 0) {
    return k_ra8_err_checksum_mismatch;
  }
  return k_ra8_ok;
}

/**
 * @brief Run the full ra8_io round-trip: mount, mkdir, write, read-back, verify.
 *
 * @details Composes the helpers in order so the success path is a single line
 *          per step and the first failing step short-circuits with its code. The
 *          VFS mkdir exercises the new directory-creation path on the SD volume.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Mount + mkdir + write + read-back + verify all passed.
 * @retval k_ra8_err_* The first failing step's code.
 *
 * @pre The SD card is initialised and `s_payload` is filled.
 * @pre The VFS mount table is empty or has a free slot.
 * @post On success the SD card holds the verified file under `sd:/LOGS`.
 * @post On any non-ok return the partial state is abandoned (caller halts).
 *
 * @note Not thread-safe; single-threaded round-trip.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t sdhi_demo_roundtrip(void)
{
  RA8_RETURN_ON_ERROR(sdhi_demo_mount_via_io(), s_tag, "mount via io");
  RA8_RETURN_ON_ERROR(ra8_io_vfs_mkdir(k_dir_path), s_tag, "mkdir");
  RA8_RETURN_ON_ERROR(sdhi_demo_write_payload(k_file_path), s_tag, "write payload");
  RA8_RETURN_ON_ERROR(sdhi_demo_read_and_verify(k_file_path), s_tag, "read verify");
  return k_ra8_ok;
}

/* =============================================================================
 * Main
 * =============================================================================
 */

/**
 * @brief App entry: bring up the bus + card, run the ra8_io round-trip, print PASS.
 *
 * @details Brings up the clocks, console, and SDHI bus pins, runs the native SD
 *          card identification, fills the payload, then runs the full `ra8_io` VFS
 *          round-trip over the native-SDHI block device. On success it prints the
 *          exact PASS banner the HIL runner and ra8_emulator smoke gate scrape for;
 *          on any failure it prints `FAIL` and parks the core.
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
  sdhi_demo_fill_payload();

  const ra8_err_t r = sdhi_demo_roundtrip();
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
