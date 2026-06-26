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
 * the backend. The ra_fs + VFS layers above are identical to the other FAT
 * demos and shared through `common/ra_io_roundtrip.{h,c}`; this app differs only
 * by the OSPI controller bring-up (`ra_xspi_init`) plus the ONE backend bind
 * line (`ra_io_blockdev_xspi_init`) and its own PASS banner.
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

#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_io.h"
#include "ra_io_roundtrip.h"
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
/** @brief UART output stream + its sink state. */
static ra_io_stream_t            s_uart;
static ra_io_stream_uart_state_t s_ust;

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_xspi_demo";

/** @brief Shared round-trip parameters for this OSPI-backed FAT12 volume. */
static const ra_io_roundtrip_params_t s_params = {
  .vfs_prefix   = "xs",
  .fat_type     = k_ra_fs_type_fat12,
  .volume_label = "RAIOXS",
  .log_tag      = s_tag,
  .root_file    = "",
  .root_path    = "",
  .root_bytes   = 0U,
  .subdir_path  = "xs:/CFG",
  .subdir_file  = "xs:/CFG/SET.BIN",
  .subdir_bytes = (uint32_t)k_demo_payload,
};

/**
 * @brief Print a NUL-terminated string on the UART stream.
 *
 * @details Thin wrapper over ::ra_io_stream_puts that fixes the demo's UART
 *          stream sink, so callers do not repeat the sink handle.
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
 *          rates, starts the millisecond time base, routes the SCI8 console
 *          pins, and opens the SCI8 UART. Any failure spins forever so a
 *          debugger can inspect the halt; there is no console yet to report on.
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
 * @brief Bring up the OSPI block device, then run the shared FAT round-trip.
 *
 * @details Initialises the xSPI controller in 1S-1S-1S link mode, binds an
 *          erase-before-write block device over a 256 KiB flash window (the two
 *          per-demo bring-up lines), mounts the FAT12 volume through the shared
 *          helper, then runs the shared mkdir + nested round-trip into
 *          `xs:/CFG/SET.BIN`. The first failing step short-circuits with its code.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok    The `"xs"` volume mounted and the payload round-tripped.
 * @retval k_ra_err_* The first failing bring-up / mount / round-trip step's code.
 *
 * @pre ::demo_setup_or_halt has run (clocks + console up).
 * @pre ::s_bd / ::s_xstate / ::s_be out-live every call through the device.
 * @post On success `xs:/CFG/SET.BIN` holds the verified payload.
 * @post No file handle is left open on any return path.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra_err_t demo_run(void)
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
  ra_fs_mount_t* mnt = nullptr;
  RA_RETURN_ON_ERROR(ra_io_roundtrip_mount(&s_bd, &s_params, &s_be, &mnt), s_tag, "mount");
  return ra_io_roundtrip_subdir_file(&s_params);
}

/**
 * @brief Firmware entry point.
 *
 * @details Initialises logging and the console, brings up the OSPI NOR volume,
 *          runs the erase-before-write round-trip, and prints a single PASS/FAIL
 *          verdict line over SCI8 before parking in an infinite loop.
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

  const ra_err_t e = demo_run();
  if (e == k_ra_ok) {
    demo_print("ra_io_xspi_demo: xs:/CFG/SET.BIN 256 bytes PASS\r\n");
  } else {
    demo_print("ra_io_xspi_demo: FAIL\r\n");
  }
  (void)ra_sci_flush((uint8_t)k_demo_uart_chan);
  while (true) {
  }
}
