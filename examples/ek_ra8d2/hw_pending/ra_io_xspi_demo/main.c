/**
 * @file examples/ek_ra8d2/hw_pending/ra_io_xspi_demo/main.c
 * @brief ra_io fabric over OSPI NOR flash (epic #155, #156) on the EK-RA8D2.
 *
 * @details
 * The third storage tier for the ra_io fabric. Where `ra_io_demo` proves the
 * fabric over RAM and `ra_io_sd_demo` over an SD card, this app proves it over
 * the on-board Octo-SPI (OSPI / xSPI) NOR flash -- an *erase-before-write*
 * medium. NOR can only clear bits (1 -> 0) on a program, so a 4 KiB sector must
 * be erased back to all-ones before any byte in it is re-written. The xSPI
 * block-device backend hides this behind the block vtable: an arbitrary
 * 512-byte block write triggers a whole-4-KiB-sector read-modify-write inside
 * the backend. This exercises the block-device capabilities
 * (`must_erase_before_write`, whole-sector RMW) that the RAM and SD backends do
 * not have.
 *
 * The pipeline, with no external hardware:
 *   1. Bring up the OSPI controller in 1S-1S-1S link mode (`ra_xspi_init`).
 *   2. Build an xSPI block device over a small flash window (Phase 1, #156).
 *   3. Bridge it to ra_fs and format/mount a FAT12 volume, then register it in
 *      the VFS as `"xs"` (Phase 3, #158).
 *   4. mkdir `xs:/CFG`, write a payload to `xs:/CFG/SET.BIN`, read it back
 *      through the VFS name, and byte-compare.
 *   5. Report progress on the SCI8 console through a UART stream sink, with
 *      ra_log routed into the same stream (Phase 2, #157).
 *
 * The window and payload are deliberately small (512 blocks = 256 KiB volume,
 * 256-byte payload) because every 512-byte write drives a 4 KiB erase +
 * reprogram RMW that is slow in board_sim; a large volume would blow the run
 * budget. board_sim models the 2 MiB OSPI NOR array internally, so the run is
 * headless and observable: a successful run prints
 * `ra_io_xspi_demo: xs:/CFG/SET.BIN 256 bytes PASS`.
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
#include "ra_xspi.h"

/** @enum demo_const_t @brief Console + volume + OSPI knobs (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_chan   = 8U,      /**< SCI8 J-Link OB console.              */
  k_demo_uart_baud   = 115200U, /**< Console baud.                        */
  k_demo_xspi_inst   = 0U,      /**< xSPI controller instance index.      */
  k_demo_xspi_base   = 0U,      /**< Flash byte offset of logical blk 0.  */
  k_demo_disk_blocks = 512U,    /**< 256 KiB OSPI window (FAT12).         */
  k_demo_payload     = 256U,    /**< Bytes written + read back.           */
  k_demo_pin_shift   = 8U,      /**< Port byte position in ra_port_pin_t. */
  k_demo_seed_mul    = 5U,      /**< Test-pattern multiplier.             */
  k_demo_seed_add    = 1U,      /**< Test-pattern additive bias.          */
} demo_const_t;

/** @brief SCI8 console TXD = PD02. */
static const ra_port_pin_t k_demo_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra_port_pin_t k_demo_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra_pin_3);

/** @brief Block-device handle + its OSPI NOR backend state. */
static ra_io_blockdev_t            s_bd;
static ra_io_blockdev_xspi_state_t s_xstate;
/** @brief ra_fs backend bridged onto the OSPI block device. */
static ra_fs_backend_t s_be;
/** @brief Mount handle for the "xs" VFS volume. */
static ra_fs_mount_t* s_mnt = nullptr;
/** @brief UART output stream + its sink state. */
static ra_io_stream_t            s_uart;
static ra_io_stream_uart_state_t s_ust;

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_xspi_demo";

/**
 * @brief Print a NUL-terminated string on the UART stream.
 *
 * @details Thin wrapper over ::ra_io_stream_puts that fixes the demo's UART
 * stream sink, so callers do not repeat the sink handle.
 *
 * @param[in] msg NUL-terminated ASCII string (CR/LF included by the caller).
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
 * @brief Bring up CGC + SysTick + the SCI8 console; halt on any failure.
 *
 * @details Initialises the clock-generation circuit, reads the CPU and PCLKA
 * rates, starts the millisecond time base, routes the SCI8 console pins, and
 * opens the SCI8 UART. Any failure spins forever so a debugger can inspect the
 * halt; there is no console yet to report on.
 *
 * @return None.
 *
 * @pre SystemInit has configured VTOR / FPU / priority grouping.
 * @pre Runs single-threaded during early boot with no other driver active.
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
 * @brief Bring up the OSPI block device, bridge it to ra_fs, format + mount.
 *
 * @details Initialises the xSPI controller in 1S-1S-1S link mode, binds an
 * erase-before-write block device over a 256 KiB flash window starting at
 * ::k_demo_xspi_base, bridges it to ra_fs, formats it FAT12, mounts it, and
 * registers the mount in the VFS under `"xs"`. Each step's return value is
 * checked; the first failure short-circuits with that error code.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok The `"xs"` volume is formatted, mounted, and VFS-registered.
 * @retval (other) The first failing bring-up step's error code.
 *
 * @pre ::demo_setup_or_halt has run (clocks + console up).
 * @pre ::s_bd / ::s_xstate / ::s_be out-live every call through the device.
 * @post On success ::s_mnt names a mounted FAT12 volume reachable as `"xs:"`.
 * @post On any non-ok return ::s_mnt is left as nullptr.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra_err_t demo_mount(void)
{
  RA_RETURN_ON_ERROR(ra_xspi_init((uint8_t)k_demo_xspi_inst, k_ra_xspi_lio_1s1s1s),
                     s_tag,
                     "xspi init");
  RA_RETURN_ON_ERROR(ra_io_blockdev_xspi_init(&s_bd,
                                              &s_xstate,
                                              (uint8_t)k_demo_xspi_inst,
                                              (uint32_t)k_demo_xspi_base,
                                              (uint32_t)k_demo_disk_blocks,
                                              false),
                     s_tag,
                     "blockdev init");
  RA_RETURN_ON_ERROR(ra_io_blockdev_as_fs_backend(&s_bd, &s_be), s_tag, "fs bridge");
  ra_fs_format_opts_t opts = {};
  opts.type                = k_ra_fs_type_fat12;
  opts.label               = "RAIOXS";
  RA_RETURN_ON_ERROR(ra_fs_format(&s_be, &opts), s_tag, "format");
  RA_RETURN_ON_ERROR(ra_fs_mount(&s_be, &s_mnt), s_tag, "mount");
  RA_RETURN_ON_ERROR(ra_io_vfs_mount("xs", s_mnt), s_tag, "vfs mount");
  return k_ra_ok;
}

/**
 * @brief mkdir `xs:/CFG`, write a payload, read it back, and byte-compare.
 *
 * @details Builds a deterministic ::k_demo_payload-byte pattern, creates the
 * `CFG` directory through the VFS, writes the pattern to `xs:/CFG/SET.BIN` via
 * VFS open(write) + ra_fs_write + close (each 512-byte block driving a 4 KiB
 * erase-reprogram RMW in the NOR backend), then reads it back via VFS
 * open(read) + ra_fs_read + close and compares every byte.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                    The payload round-tripped through NOR.
 * @retval k_ra_err_invalid_size      The read-back length differed.
 * @retval k_ra_err_checksum_mismatch The read-back bytes differed.
 * @retval (other)                    The first failing VFS / ra_fs step's code.
 *
 * @pre ::demo_mount returned k_ra_ok (the `"xs"` volume is mounted).
 * @pre The VFS name `"xs"` resolves to the OSPI-backed FAT12 mount.
 * @post On success `xs:/CFG/SET.BIN` holds the verified payload.
 * @post No file handle is left open on any return path.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra_err_t demo_roundtrip(void)
{
  uint8_t data[(size_t)k_demo_payload];
  for (uint32_t i = 0; i < (uint32_t)k_demo_payload; ++i) {
    data[i] = (uint8_t)((i * (uint32_t)k_demo_seed_mul) + (uint32_t)k_demo_seed_add);
  }
  RA_RETURN_ON_ERROR(ra_io_vfs_mkdir("xs:/CFG"), s_tag, "mkdir");

  ra_fs_file_t* wf = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open("xs:/CFG/SET.BIN", k_ra_fs_mode_write, &wf), s_tag, "open w");
  RA_RETURN_ON_ERROR(ra_fs_write(wf, data, (uint32_t)k_demo_payload), s_tag, "write");
  RA_RETURN_ON_ERROR(ra_fs_close(wf), s_tag, "close w");

  ra_fs_file_t* rf = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open("xs:/CFG/SET.BIN", k_ra_fs_mode_read, &rf), s_tag, "open r");
  uint8_t  got[(size_t)k_demo_payload] = {};
  uint32_t got_len                     = 0;
  RA_RETURN_ON_ERROR(ra_fs_read(rf, got, (uint32_t)k_demo_payload, &got_len), s_tag, "read");
  RA_RETURN_ON_ERROR(ra_fs_close(rf), s_tag, "close r");
  if (got_len != (uint32_t)k_demo_payload) {
    return k_ra_err_invalid_size;
  }
  if (memcmp(got, data, sizeof(data)) != 0) {
    return k_ra_err_checksum_mismatch;
  }
  return k_ra_ok;
}

/**
 * @brief Firmware entry point.
 *
 * @details Initialises logging and the console, brings up the OSPI NOR volume,
 * runs the erase-before-write round-trip, and prints a single PASS/FAIL verdict
 * line over SCI8 before parking in an infinite loop.
 *
 * @return Never returns.
 * @retval (none) The function does not return (final `while (true)`).
 *
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @pre The OSPI NOR array is present (modelled in board_sim, real on silicon).
 * @post Exactly one PASS or FAIL verdict line has been queued on SCI8.
 * @post Control parks in an infinite loop; the function never returns.
 *
 * @note Single-threaded; runs to the park loop on the main stack.
 * @since 0.1.0
 */
int main(void)
{
  ra_log_init();
  demo_setup_or_halt();
  (void)ra_io_stream_uart_init(&s_uart, &s_ust, (uint8_t)k_demo_uart_chan);
  (void)ra_io_log_attach(&s_uart); /* route ra_log into the UART stream too */
  demo_print("ra_io_xspi_demo: boot\r\n");

  ra_err_t e = demo_mount();
  if (e == k_ra_ok) {
    e = demo_roundtrip();
  }
  if (e == k_ra_ok) {
    demo_print("ra_io_xspi_demo: xs:/CFG/SET.BIN 256 bytes PASS\r\n");
  } else {
    demo_print("ra_io_xspi_demo: FAIL\r\n");
  }
  (void)ra_sci_flush((uint8_t)k_demo_uart_chan);
  while (true) {
  }
}
