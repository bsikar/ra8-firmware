/**
 * @file examples/ek_ra8d2/hw_pending/ra_io_sdram_demo/main.c
 * @brief ra_io fabric over the external SDRAM backend (epic #155) on EK-RA8D2.
 *
 * @details
 * The same fabric as `ra_io_demo`, but the block device is backed by the 64 MiB
 * external SDRAM (at 0x68000000) instead of an in-SRAM buffer -- the swappable
 * backend differs, the ra_fs + VFS layers above are identical and shared through
 * `common/ra_io_roundtrip.{h,c}`. This app differs only by the ONE backend bind
 * line (`ra_io_blockdev_sdram_init`, which brings the controller up with
 * `ra_sdramc_init` and presents the window as 512-byte logical blocks) plus its
 * own PASS banners.
 *
 * board_sim maps the SDRAM region and models the SDRAM controller bring-up, so
 * the PASS/FAIL line is observable headlessly: a successful run prints
 * `ra_io_sdram_demo: mkdir+nested dr:/SUB/NOTE.TXT PASS`.
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

/** @enum demo_const_t @brief Console + volume knobs (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_chan   = 8U,      /**< SCI8 J-Link OB console.            */
  k_demo_uart_baud   = 115200U, /**< Console baud.                      */
  k_demo_disk_blocks = 512U,    /**< 256 KiB SDRAM window (FAT12).      */
  k_demo_payload     = 128U,    /**< Bytes written + read back at root. */
  k_demo_pin_shift   = 8U,      /**< Port byte position in ra_port_pin_t.*/
  k_demo_note_len    = 64U,     /**< Bytes round-tripped in the subdir.  */
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

/** @brief Shared round-trip parameters for this SDRAM-backed FAT12 volume. */
static const ra_io_roundtrip_params_t s_params = {
  .vfs_prefix   = "dr",
  .fat_type     = k_ra_fs_type_fat12,
  .volume_label = "RAIOSDRAM",
  .log_tag      = s_tag,
  .root_file    = "HELLO.TXT",
  .root_path    = "dr:/HELLO.TXT",
  .root_bytes   = (uint32_t)k_demo_payload,
  .subdir_path  = "dr:/SUB",
  .subdir_file  = "dr:/SUB/NOTE.TXT",
  .subdir_bytes = (uint32_t)k_demo_note_len,
};

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
 * @brief Bind the SDRAM block device, then run the shared FAT round-trip.
 *
 * @details Binds the external-SDRAM backend over a 256 KiB window (the single
 *          per-demo bind line), mounts the FAT12 volume through the shared
 *          helper, runs the root-file round-trip, and -- on success -- returns
 *          the mount handle through @p out_mount for the subdir round-trip.
 *
 * @param[out] out_mount Receives the mount handle on success, nullptr on error.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok    The SDRAM volume mounted and the root file round-tripped.
 * @retval k_ra_err_* The first failing bind / mount / round-trip step's code.
 *
 * @pre ::demo_setup_or_halt has run (clocks + console up).
 * @pre @p out_mount is non-NULL and the SDRAM pins/clocks allow bring-up.
 * @post On success `dr:/HELLO.TXT` holds the verified payload.
 * @post On any non-ok return `*out_mount` is nullptr.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra_err_t demo_run(ra_fs_mount_t** out_mount)
{
  RA_RETURN_ON_ERROR(ra_io_blockdev_sdram_init(&s_bd, &s_bstate, (uint32_t)k_demo_disk_blocks),
                     s_tag,
                     "sdram blockdev init");
  RA_RETURN_ON_ERROR(ra_io_roundtrip_mount(&s_bd, &s_params, &s_be, out_mount), s_tag, "mount");
  return ra_io_roundtrip_root_file(*out_mount, &s_params);
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

  ra_fs_mount_t* mnt = nullptr;
  const ra_err_t e   = demo_run(&mnt);
  if (e == k_ra_ok) {
    demo_print("ra_io_sdram_demo: wrote/read 128 bytes dr:/HELLO.TXT PASS\r\n");
    if (ra_io_roundtrip_subdir_file(&s_params) == k_ra_ok) {
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
