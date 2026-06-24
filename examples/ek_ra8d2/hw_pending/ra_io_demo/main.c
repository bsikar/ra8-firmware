/**
 * @file examples/ek_ra8d2/hw_pending/ra_io_demo/main.c
 * @brief End-to-end demo of the ra_io fabric (epic #155) on the EK-RA8D2.
 *
 * @details
 * Exercises the whole fabric in one app, with no external hardware:
 *   1. Build a RAM block device over an in-SRAM buffer (Phase 1, #156).
 *   2. Bridge it to ra_fs and format/mount a FAT12 volume (the block-device
 *      bridge), then register it in the VFS as `"ram"` (Phase 3, #158).
 *   3. Write a file and read it back through the `"ram:/..."` name (VFS).
 *   4. Report progress on the SCI8 console through a UART stream sink, with
 *      ra_log routed into the same stream (Phase 2, #157).
 *
 * The board_sim emulator captures the SCI8 console, so the PASS/FAIL line and
 * the byte counts are observable headlessly: a successful run prints
 * `ra_io_demo: wrote/read 128 bytes ram:/HELLO.TXT PASS`.
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
  k_demo_disk_blocks = 512U,    /**< 256 KiB RAM-disk (FAT12).          */
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

/** @brief 256 KiB RAM-disk backing buffer (in SRAM .bss). */
static uint8_t s_disk[(size_t)k_demo_disk_blocks * (size_t)k_ra_io_block_size_bytes];
/** @brief Block-device handle + its RAM backend state. */
static ra_io_blockdev_t           s_bd;
static ra_io_blockdev_ram_state_t s_bstate;
/** @brief ra_fs backend bridged onto the block device. */
static ra_fs_backend_t s_be;
/** @brief UART output stream + its sink state. */
static ra_io_stream_t            s_uart;
static ra_io_stream_uart_state_t s_ust;

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_demo";

/** @brief Print a NUL-terminated string on the UART stream. */
static void demo_print(const char* msg)
{
  (void)ra_io_stream_puts(&s_uart, msg);
}

/** @brief Bring up CGC + SysTick + the SCI8 console; halt on any failure. */
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

/** @brief Run the fabric round-trip; returns k_ra_ok on a verified match. */
static ra_err_t demo_run(void)
{
  RA_RETURN_ON_ERROR(
    ra_io_blockdev_ram_init(&s_bd, &s_bstate, s_disk, (uint32_t)k_demo_disk_blocks, false),
    s_tag,
    "blockdev init");
  RA_RETURN_ON_ERROR(ra_io_blockdev_as_fs_backend(&s_bd, &s_be), s_tag, "fs bridge");
  ra_fs_format_opts_t opts = {};
  opts.type                = k_ra_fs_type_fat12;
  opts.label               = "RAIO";
  RA_RETURN_ON_ERROR(ra_fs_format(&s_be, &opts), s_tag, "format");
  ra_fs_mount_t* mnt = nullptr;
  RA_RETURN_ON_ERROR(ra_fs_mount(&s_be, &mnt), s_tag, "mount");
  RA_RETURN_ON_ERROR(ra_io_vfs_mount("ram", mnt), s_tag, "vfs mount");

  uint8_t data[(size_t)k_demo_payload];
  for (uint32_t i = 0; i < (uint32_t)k_demo_payload; ++i) {
    data[i] = (uint8_t)(i * (uint32_t)k_demo_seed_mul + 1u);
  }
  RA_RETURN_ON_ERROR(ra_fs_write_file(mnt, "HELLO.TXT", data, (uint32_t)k_demo_payload),
                     s_tag,
                     "write");

  ra_fs_file_t* f = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open("ram:/HELLO.TXT", k_ra_fs_mode_read, &f), s_tag, "open");
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

/** @brief mkdir a subdir via the VFS, then round-trip a file two levels deep. */
static ra_err_t demo_subdir(void)
{
  RA_RETURN_ON_ERROR(ra_io_vfs_mkdir("ram:/SUB"), s_tag, "mkdir");
  uint8_t note[(size_t)k_demo_note_len];
  for (uint32_t i = 0; i < (uint32_t)k_demo_note_len; ++i) {
    note[i] = (uint8_t)('A' + (i % (uint32_t)k_demo_note_mod));
  }
  ra_fs_file_t* wf = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open("ram:/SUB/NOTE.TXT", k_ra_fs_mode_write, &wf), s_tag, "open w");
  RA_RETURN_ON_ERROR(ra_fs_write(wf, note, (uint32_t)k_demo_note_len), s_tag, "write");
  RA_RETURN_ON_ERROR(ra_fs_close(wf), s_tag, "close w");

  ra_fs_file_t* rf = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open("ram:/SUB/NOTE.TXT", k_ra_fs_mode_read, &rf), s_tag, "open r");
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
  demo_print("ra_io_demo: boot\r\n");

  const ra_err_t e = demo_run();
  if (e == k_ra_ok) {
    demo_print("ra_io_demo: wrote/read 128 bytes ram:/HELLO.TXT PASS\r\n");
    if (demo_subdir() == k_ra_ok) {
      demo_print("ra_io_demo: mkdir+nested ram:/SUB/NOTE.TXT PASS\r\n");
    } else {
      demo_print("ra_io_demo: mkdir FAIL\r\n");
    }
  } else {
    demo_print("ra_io_demo: FAIL\r\n");
  }
  (void)ra_sci_flush((uint8_t)k_demo_uart_chan);
  while (true) {
  }
}
