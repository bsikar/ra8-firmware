/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ra8_io_demo/src/main.c
 * @brief End-to-end demo of the ra8_io fabric (epic #155) over a RAM block device.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the whole fabric in one app, with no external hardware. The fabric
 * round-trip (bridge -> format -> mount -> VFS -> write/read/verify -> mkdir +
 * nested round-trip) lives in this app's self-contained
 * `ra8_io_roundtrip.{h,c}` (the SDRAM, OSPI/xSPI, and SD demos carry their own
 * copy under the hw_pending tree); this app differs only by the ONE backend bind
 * line -- here `ra8_io_blockdev_ram_init` over an in-SRAM buffer (Phase 1, #156)
 * -- plus its own PASS banners.
 *
 * The ra8_emulator captures the SCI8 console, so the PASS/FAIL line and
 * the byte counts are observable headlessly: a successful run prints
 * `ra8_io_demo: wrote/read 128 bytes ram:/HELLO.TXT PASS` followed by
 * `ra8_io_demo: mkdir+nested ram:/SUB/NOTE.TXT PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_console_stream.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io.h"
#include "ra8_io_roundtrip.h"
#include "ra8_log.h"
#include "ra8_time.h"

/** @enum demo_const_t @brief Console + volume knobs (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_baud   = 115200U, /**< Console baud.                      */
  k_demo_disk_blocks = 512U,    /**< 256 KiB RAM-disk (FAT12).          */
  k_demo_payload     = 128U,    /**< Bytes written + read back at root. */
  k_demo_note_len    = 64U,     /**< Bytes round-tripped in the subdir. */
} demo_const_t;

/** @brief 256 KiB RAM-disk backing buffer (in SRAM .bss). */
static uint8_t s_disk[(size_t)k_demo_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief Block-device handle + its RAM backend state. */
static ra8_io_blockdev_t           s_bd;
static ra8_io_blockdev_ram_state_t s_bstate;
/** @brief ra8_fs backend bridged onto the block device. */
static ra8_fs_backend_t s_be;
/** @brief Console output stream; the board owns the sink behind it. */
static ra8_io_stream_t s_uart;

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_demo";

/** @brief Shared round-trip parameters for this RAM-backed FAT12 volume. */
static const ra8_io_roundtrip_params_t s_params = {
  .vfs_prefix   = "ram",
  .fat_type     = k_ra8_fs_type_fat12,
  .volume_label = "RAIO",
  .log_tag      = s_tag,
  .root_file    = "HELLO.TXT",
  .root_path    = "ram:/HELLO.TXT",
  .root_bytes   = (uint32_t)k_demo_payload,
  .subdir_path  = "ram:/SUB",
  .subdir_file  = "ram:/SUB/NOTE.TXT",
  .subdir_bytes = (uint32_t)k_demo_note_len,
};

/**
 * @brief Print a NUL-terminated string on the demo's UART stream.
 *
 * @details Thin wrapper over ::ra8_io_stream_puts bound to ::s_uart, so call
 *          sites do not repeat the sink handle.
 *
 * @param[in] msg NUL-terminated ASCII string (CR/LF supplied by the caller).
 *
 * @return None.
 *
 * @pre ::s_uart was bound by ::ra8_board_console_stream.
 * @pre @p msg is non-NULL and NUL-terminated.
 * @post The bytes of @p msg are queued on the UART stream.
 * @post No other state changes.
 *
 * @note Blocking polled TX; not interrupt-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_print(const char* msg)
{
  (void)ra8_io_stream_puts(&s_uart, msg);
}

/**
 * @brief Bring up CGC + SysTick + the board console; halt forever on failure.
 *
 * @details Initialises clocks, reads the CPU rate, starts the millisecond time
 *          base, and hands the console over to the BSP -- which owns the
 *          channel, the PD02 / PD03 routing and the live-PCLKA bit-rate solve.
 *          Any failure spins forever so a debugger can inspect the halt.
 *
 * @return None.
 *
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @pre Runs single-threaded during early boot.
 * @post On success the console is open at ::k_demo_uart_baud and the time base
 *       runs.
 * @post On any failure the function never returns (infinite halt loop).
 *
 * @note Not thread-safe; boot-context only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_time_init(cpuclk0_hz) != k_ra8_ok) ||
      (ra8_board_uart_console_init((uint32_t)k_demo_uart_baud) != k_ra8_ok) ||
      (ra8_board_console_stream(&s_uart) != k_ra8_ok)) {
    while (true) {
    }
  }
}

/**
 * @brief Bind the RAM block device, then run the shared FAT round-trip.
 *
 * @details Binds the in-SRAM RAM backend (the single per-demo bind line), mounts
 *          the FAT12 volume through the shared helper, runs the root-file
 *          round-trip, and -- on success -- reports the result through @p mnt for
 *          the subsequent subdir round-trip in `main`.
 *
 * @param[out] out_mount Receives the mount handle on success, nullptr on error.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    The RAM volume mounted and the root file round-tripped.
 * @retval k_ra8_err_* The first failing bind / mount / round-trip step's code.
 *
 * @pre ::internal_demo_setup_or_halt has run (clocks + console up).
 * @pre @p out_mount is non-NULL.
 * @post On success `ram:/HELLO.TXT` holds the verified payload.
 * @post On any non-ok return `*out_mount` is nullptr.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_run(ra8_fs_mount_t** out_mount)
{
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_ram_init(&s_bd, &s_bstate, s_disk, (uint32_t)k_demo_disk_blocks, false),
    s_tag,
    "blockdev init");
  RA8_RETURN_ON_ERROR(ra8_io_roundtrip_mount(&s_bd, &s_params, &s_be, out_mount), s_tag, "mount");
  return ra8_io_roundtrip_root_file(*out_mount, &s_params);
}

/**
 * @brief Firmware entry point.
 *
 * @pre SystemInit set VTOR / FPU / priority grouping.
 */
void main(void)
{
  ra8_log_init();
  internal_demo_setup_or_halt();
  (void)ra8_io_log_attach(&s_uart); /* route ra8_log into the console stream too */
  internal_demo_print("ra8_io_demo: boot\r\n");

  ra8_fs_mount_t* mnt = nullptr;
  const ra8_err_t e   = internal_demo_run(&mnt);
  if (e == k_ra8_ok) {
    internal_demo_print("ra8_io_demo: wrote/read 128 bytes ram:/HELLO.TXT PASS\r\n");
    if (ra8_io_roundtrip_subdir_file(&s_params) == k_ra8_ok) {
      internal_demo_print("ra8_io_demo: mkdir+nested ram:/SUB/NOTE.TXT PASS\r\n");
    } else {
      internal_demo_print("ra8_io_demo: mkdir FAIL\r\n");
    }
  } else {
    internal_demo_print("ra8_io_demo: FAIL\r\n");
  }
  (void)ra8_board_uart_console_flush();
  while (true) {
  }
}
