/**
 * @file examples/ek_ra8d2/hw_pending/ra_io_sdram_demo/main.c
 * @brief ra_io fabric over the external SDRAM backend (epic #155) on EK-RA8D2.
 *
 * @details
 * The same fabric as `ra_io_demo`, but the block device is backed by the 64 MiB
 * external SDRAM (at 0x68000000) instead of an in-SRAM buffer -- the swappable
 * backend differs, the ra_fs + VFS layers above are identical. The SDRAM
 * backend brings the controller up with `ra_sdramc_init` and then presents the
 * window as 512-byte logical blocks. With no external hardware:
 *   1. Bind an SDRAM block device (`ra_io_blockdev_sdram_init`, Phase 1 #156).
 *   2. Bridge it to ra_fs, format/mount a FAT12 volume, and register it in the
 *      VFS as `"dr"` (Phase 3, #158).
 *   3. Write a file and read it back through the `"dr:/..."` name (VFS).
 *   4. mkdir `dr:/SUB` and round-trip a file two levels deep.
 *   5. Report progress on the SCI8 console through a UART stream sink, with
 *      ra_log routed into the same stream (Phase 2, #157).
 *
 * board_sim maps the SDRAM region and models the SDRAM controller bring-up, so
 * the PASS/FAIL line is observable headlessly: a successful run prints
 * `ra_io_sdram_demo: mkdir+nested dr:/SUB/NOTE.TXT PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_io.h"
#include "ra_log.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @enum demo_const_t @brief Console + volume knobs (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_chan   = 8U,      /**< SCI8 J-Link OB console.            */
  k_demo_uart_baud   = 115200U, /**< Console baud.                      */
  k_demo_disk_blocks = 512U,    /**< 256 KiB SDRAM window (FAT12).      */
  k_demo_payload     = 128U,    /**< Bytes written + read back.         */
  k_demo_pin_shift   = 8U,      /**< Port byte position in ra_port_pin_t.*/
  k_demo_seed_mul    = 7U,      /**< Test-pattern multiplier.           */
  k_demo_note_len    = 64U,     /**< Bytes round-tripped in the subdir.  */
  k_demo_note_mod    = 26U,     /**< Subdir-note alphabet size.          */
} demo_const_t;

/** @brief SCI8 console TXD = PD02. */
static const ra_port_pin_t k_demo_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra_port_pin_t k_demo_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra_pin_3);

/** @brief Block-device handle + its SDRAM backend state (storage is the SDRAM). */
static ra_io_blockdev_t           s_bd;
static ra_io_blockdev_ram_state_t s_bstate;
/** @brief ra_fs backend bridged onto the block device. */
static ra_fs_backend_t s_be;
/** @brief UART output stream + its sink state. */
static ra_io_stream_t            s_uart;
static ra_io_stream_uart_state_t s_ust;

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_sdram_demo";

/**
 * @brief Print a NUL-terminated string on the demo's UART stream.
 *
 * @details Thin wrapper over ::ra_io_stream_puts bound to ::s_uart, so call
 *          sites do not repeat the sink handle.
 *
 * @param[in] msg NUL-terminated ASCII string (CR/LF supplied by the caller).
 *
 * @return None.
 *
 * @pre ::s_uart was initialised by ::ra_io_stream_uart_init.
 * @pre @p msg is non-NULL and NUL-terminated.
 * @post The bytes of @p msg are queued on the UART stream.
 * @post No other state changes.
 *
 * @note Blocking polled TX; not interrupt-safe.
 * @since 0.1.0
 */
static void demo_print(const char* msg)
{
  (void)ra_io_stream_puts(&s_uart, msg);
}

/**
 * @brief Bring up CGC + SysTick + the SCI8 console; halt forever on any failure.
 *
 * @details Initialises clocks, reads CPU/PCLKA rates, starts the millisecond
 *          time base, routes the SCI8 console pins, and opens the SCI8 UART. Any
 *          failure spins forever so a debugger can inspect the halt (there is no
 *          console yet to report on).
 *
 * @return None.
 *
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @pre Runs single-threaded during early boot.
 * @post On success SCI8 is open at ::k_demo_uart_baud and the time base runs.
 * @post On any failure the function never returns (infinite halt loop).
 *
 * @note Not thread-safe; boot-context only.
 * @since 0.1.0
 */
static void demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra_cgc_init() != k_ra_ok) ||
      (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) ||
      (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) ||
      (ra_time_init(cpuclk0_hz) != k_ra_ok) ||
      (ra_pfs_route_peripheral(k_demo_txd, k_ra_psel_sci_async, "demo.txd") != k_ra_ok) ||
      (ra_pfs_route_peripheral(k_demo_rxd, k_ra_psel_sci_async, "demo.rxd") != k_ra_ok)) {
    while (true) {
    }
  }
  const ra_sci_cfg_t sci_cfg = {.baud      = (uint32_t)k_demo_uart_baud,
                                .data_bits = k_ra_sci_data_8,
                                .parity    = k_ra_sci_parity_none,
                                .stop_bits = k_ra_sci_stop_1,
                                .pclk_hz   = pclka_hz};
  if (ra_sci_init((uint8_t)k_demo_uart_chan, &sci_cfg) != k_ra_ok) {
    while (true) {
    }
  }
}

/**
 * @brief Bind the SDRAM block device, format/mount FAT12, and round-trip a file.
 *
 * @details Brings up the SDRAM block device over a 256 KiB window, bridges it to
 *          ra_fs, formats + mounts a FAT12 volume, registers it in the VFS as
 *          `"dr"`, writes a deterministic payload to `dr:/HELLO.TXT`, reads it
 *          back through the VFS name, and byte-compares.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                    The payload round-tripped intact.
 * @retval k_ra_err_invalid_size      The read-back length differed.
 * @retval k_ra_err_checksum_mismatch The read-back bytes differed.
 * @retval (other)                    The first failing fabric step's code.
 *
 * @pre ::demo_setup_or_halt has run (clocks + console up).
 * @pre The SDRAM pins/clocks allow `ra_sdramc_init` to succeed.
 * @post On success `dr:/HELLO.TXT` holds the verified payload.
 * @post No file handle is left open on any return path.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra_err_t demo_run(void)
{
  RA_RETURN_ON_ERROR(
    ra_io_blockdev_sdram_init(&s_bd, &s_bstate, (uint32_t)k_demo_disk_blocks),
    s_tag,
    "sdram blockdev init");
  RA_RETURN_ON_ERROR(ra_io_blockdev_as_fs_backend(&s_bd, &s_be), s_tag, "fs bridge");
  ra_fs_format_opts_t opts = {};
  opts.type                = k_ra_fs_type_fat12;
  opts.label               = "RAIOSDRAM";
  RA_RETURN_ON_ERROR(ra_fs_format(&s_be, &opts), s_tag, "format");
  ra_fs_mount_t* mnt = nullptr;
  RA_RETURN_ON_ERROR(ra_fs_mount(&s_be, &mnt), s_tag, "mount");
  RA_RETURN_ON_ERROR(ra_io_vfs_mount("dr", mnt), s_tag, "vfs mount");

  uint8_t data[(size_t)k_demo_payload];
  for (uint32_t i = 0; i < (uint32_t)k_demo_payload; ++i) {
    data[i] = (uint8_t)((i * (uint32_t)k_demo_seed_mul) + 1U);
  }
  RA_RETURN_ON_ERROR(ra_fs_write_file(mnt, "HELLO.TXT", data, (uint32_t)k_demo_payload),
                     s_tag,
                     "write");

  ra_fs_file_t* f = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open("dr:/HELLO.TXT", k_ra_fs_mode_read, &f), s_tag, "open");
  uint8_t  got[(size_t)k_demo_payload] = {};
  uint32_t got_len                     = 0;
  RA_RETURN_ON_ERROR(ra_fs_read(f, got, (uint32_t)k_demo_payload, &got_len), s_tag, "read");
  RA_RETURN_ON_ERROR(ra_fs_close(f), s_tag, "close");
  if (got_len != (uint32_t)k_demo_payload) {
    return k_ra_err_invalid_size;
  }
  if (memcmp(got, data, sizeof(data)) != 0) {
    return k_ra_err_checksum_mismatch;
  }
  return k_ra_ok;
}

/**
 * @brief mkdir a subdirectory via the VFS, then round-trip a file inside it.
 *
 * @details Creates `dr:/SUB` through the VFS, writes a deterministic note to
 *          `dr:/SUB/NOTE.TXT` (open/write/close), reads it back through the VFS
 *          name, and byte-compares -- exercising mkdir + nested-path resolution
 *          over the SDRAM-backed FAT volume.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                    The note round-tripped intact.
 * @retval k_ra_err_invalid_size      The read-back length differed.
 * @retval k_ra_err_checksum_mismatch The read-back bytes differed.
 * @retval (other)                    The first failing VFS / ra_fs step's code.
 *
 * @pre ::demo_run returned k_ra_ok (the `"dr"` volume is mounted).
 * @pre The VFS name `"dr"` resolves to the SDRAM-backed FAT12 mount.
 * @post On success `dr:/SUB/NOTE.TXT` holds the verified note.
 * @post No file handle is left open on any return path.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra_err_t demo_subdir(void)
{
  RA_RETURN_ON_ERROR(ra_io_vfs_mkdir("dr:/SUB"), s_tag, "mkdir");
  uint8_t note[(size_t)k_demo_note_len];
  for (uint32_t i = 0; i < (uint32_t)k_demo_note_len; ++i) {
    note[i] = (uint8_t)('A' + (i % (uint32_t)k_demo_note_mod));
  }
  ra_fs_file_t* wf = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open("dr:/SUB/NOTE.TXT", k_ra_fs_mode_write, &wf), s_tag, "open w");
  RA_RETURN_ON_ERROR(ra_fs_write(wf, note, (uint32_t)k_demo_note_len), s_tag, "write");
  RA_RETURN_ON_ERROR(ra_fs_close(wf), s_tag, "close w");

  ra_fs_file_t* rf = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open("dr:/SUB/NOTE.TXT", k_ra_fs_mode_read, &rf), s_tag, "open r");
  uint8_t  got[(size_t)k_demo_note_len] = {};
  uint32_t got_len                      = 0;
  RA_RETURN_ON_ERROR(ra_fs_read(rf, got, (uint32_t)k_demo_note_len, &got_len), s_tag, "read");
  RA_RETURN_ON_ERROR(ra_fs_close(rf), s_tag, "close r");
  if (got_len != (uint32_t)k_demo_note_len) {
    return k_ra_err_invalid_size;
  }
  if (memcmp(got, note, sizeof(note)) != 0) {
    return k_ra_err_checksum_mismatch;
  }
  return k_ra_ok;
}

/**
 * @brief Firmware entry point.
 *
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @return Never returns.
 */
int main(void)
{
  ra_log_init();
  demo_setup_or_halt();
  (void)ra_io_stream_uart_init(&s_uart, &s_ust, (uint8_t)k_demo_uart_chan);
  (void)ra_io_log_attach(&s_uart); /* route ra_log into the UART stream too */
  demo_print("ra_io_sdram_demo: boot\r\n");

  const ra_err_t e = demo_run();
  if (e == k_ra_ok) {
    demo_print("ra_io_sdram_demo: wrote/read 128 bytes dr:/HELLO.TXT PASS\r\n");
    if (demo_subdir() == k_ra_ok) {
      demo_print("ra_io_sdram_demo: mkdir+nested dr:/SUB/NOTE.TXT PASS\r\n");
    } else {
      demo_print("ra_io_sdram_demo: mkdir FAIL\r\n");
    }
  } else {
    demo_print("ra_io_sdram_demo: FAIL\r\n");
  }
  (void)ra_sci_flush((uint8_t)k_demo_uart_chan);
  while (true) {
  }
}
