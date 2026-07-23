/**
 * @file examples/ek_ra8d2/hw_pending/ra8_io_mram_demo/main.c
 * @brief ra8_io fabric over on-chip extra-MRAM (data flash) backend (#155/#156).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Proves the ra8_io fabric over the RA8D2's on-chip extra MRAM at
 * ``k_ra8_flash_extra_start`` (0x02E07600, HUM Ch 59.7.4.5 Table 59.15 p 3592) --
 * programmed through the MACI command sequencer. Same block-device vtable as the
 * other ra8_io demos; only the backend differs (ra8_io_blockdev_mram instead of
 * RAM/SD/OSPI).
 *
 * @warning This window is one-time-programmable option-setting / OTP memory, not
 * a rewritable data-flash: the erase + re-program cycle this demo exercises does
 * NOT work on real silicon (there is no erase). board_sim maps the window and so
 * the round-trip passes here, but that is optimistic -- a real rewritable-medium
 * home for this demo (OSPI / SD) is tracked by #315.
 *
 * The demo exercises the backend directly through the ra8_io block-device layer,
 * which is the right level for this special-purpose non-volatile store (the FAT
 * layer is already proven over the RAM / SD / OSPI / SDRAM backends):
 *   1. ra8_flash_init() brings up the MRAM controller (Phase 1, #156).
 *   2. ra8_io_blockdev_mram_init() binds a block device over the fenced window.
 *   3. Erase a 512-byte logical block, program a deterministic pattern into it,
 *      read it back, and byte-compare -- the full erase + program + read path.
 *   4. Report on the SCI8 console through a ra8_io UART stream sink; ra8_log is
 *      routed into the same stream so any failing step is visible (Phase 2 #157).
 *
 * board_sim models the MACI program/erase sequence (board_periph_mram.c), so the
 * round-trip runs headless: a successful run prints
 * `ra8_io_mram_demo: 512-byte block erase/program/read on extra MRAM PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_flash.h"
#include "ra8_io.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_time.h"

/** @enum demo_const_t @brief Console + volume + MRAM knobs (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_chan   = 8U,      /**< SCI8 J-Link OB console.               */
  k_demo_uart_baud   = 115200U, /**< Console baud.                         */
  k_demo_mram_base   = (uint32_t)k_ra8_flash_extra_start, /**< Extra-MRAM OTP window base. */
  k_demo_disk_blocks = 24U,  /**< 12 KiB scratch inside the OTP window. */
  k_demo_test_lba    = 0U,   /**< Logical block under test.             */
  k_demo_block_count = 1U,   /**< Blocks erased/written/read.           */
  k_demo_payload     = 512U, /**< One 512-byte block round-tripped.     */
  k_demo_pin_shift   = 8U,   /**< Port byte position in ra8_port_pin_t. */
  k_demo_seed_mul    = 5U,   /**< Test-pattern multiplier.              */
  k_demo_seed_add    = 3U,   /**< Test-pattern additive bias.           */
  k_demo_mrcfreq_mhz = 200U, /**< Code-MRAM advertised clock (MHz).     */
  k_demo_mrefreq_mhz = 100U, /**< Extra-MRAM advertised clock (MHz).    */
} demo_const_t;

/** @brief SCI8 console TXD = PD02. */
static const ra8_port_pin_t k_demo_txd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra8_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra8_port_pin_t k_demo_rxd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra8_pin_3);

/** @brief Block-device handle + its MRAM backend state. */
static ra8_io_blockdev_t            s_bd;
static ra8_io_blockdev_mram_state_t s_mstate;
/** @brief UART output stream + its sink state. */
static ra8_io_stream_t            s_uart;
static ra8_io_stream_uart_state_t s_ust;

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_mram_demo";

/**
 * @brief Print a NUL-terminated string on the demo's UART stream.
 *
 * @details Thin wrapper over ::ra8_io_stream_puts bound to ::s_uart.
 *
 * @param[in] msg NUL-terminated ASCII string (CR/LF supplied by the caller).
 *
 * @return None.
 *
 * @pre ::s_uart was initialised by ::ra8_io_stream_uart_init.
 * @pre @p msg is non-NULL and NUL-terminated.
 * @post The bytes of @p msg are queued on the UART stream.
 * @post No other state changes.
 *
 * @note Blocking polled TX; not interrupt-safe.
 * @since 0.1.0
 */
static void demo_print(const char* msg)
{
  (void)ra8_io_stream_puts(&s_uart, msg);
}

/**
 * @brief Bring up CGC + SysTick + the SCI8 console; halt forever on any failure.
 *
 * @details Initialises clocks, reads CPU/PCLKA rates, starts the time base,
 *          routes the SCI8 console pins, and opens the SCI8 UART.
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
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) ||
      (ra8_time_init(cpuclk0_hz) != k_ra8_ok) ||
      (ra8_pfs_route_peripheral(k_demo_txd, k_ra8_psel_sci_async, "demo.txd") != k_ra8_ok) ||
      (ra8_pfs_route_peripheral(k_demo_rxd, k_ra8_psel_sci_async, "demo.rxd") != k_ra8_ok)) {
    while (true) {
    }
  }
  const ra8_sci_cfg_t sci_cfg = {.baud      = (uint32_t)k_demo_uart_baud,
                                 .data_bits = k_ra8_sci_data_8,
                                 .parity    = k_ra8_sci_parity_none,
                                 .stop_bits = k_ra8_sci_stop_1,
                                 .pclk_hz   = pclka_hz};
  if (ra8_sci_init((uint8_t)k_demo_uart_chan, &sci_cfg) != k_ra8_ok) {
    while (true) {
    }
  }
}

/**
 * @brief Bring up the MRAM controller and bind the MRAM block device.
 *
 * @details Initialises the MRAM controller (`ra8_flash_init`) and binds an
 *          erase-before-write block device over the fenced 12 KiB window.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The MRAM block device ::s_bd is ready.
 * @retval (other) The first failing bring-up step's error code.
 *
 * @pre ::demo_setup_or_halt has run (clocks + console up).
 * @pre The extra-MRAM region is present and writable.
 * @post On success ::s_bd reads/writes/erases the fenced MRAM window.
 * @post On any non-ok return ::s_bd is left unbound.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t demo_open(void)
{
  const ra8_flash_cfg_t fcfg = {.mrcfreq_mhz        = (uint16_t)k_demo_mrcfreq_mhz,
                                .mrefreq_mhz        = (uint8_t)k_demo_mrefreq_mhz,
                                .prefetch_en        = true,
                                .ecc_encoder_enable = true,
                                .ecc_decoder_enable = true};
  RA8_RETURN_ON_ERROR(ra8_flash_init(&fcfg), s_tag, "flash init");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_mram_init(&s_bd,
                                                &s_mstate,
                                                (uintptr_t)k_demo_mram_base,
                                                (uint32_t)k_demo_disk_blocks,
                                                false),
                      s_tag,
                      "mram blockdev init");
  return k_ra8_ok;
}

/**
 * @brief Erase a block, program a pattern into it, read it back, and compare.
 *
 * @details Drives the ra8_io block-device layer directly: erase one 512-byte
 *          logical block to 0xFF, program a deterministic pattern, read it back,
 *          and verify byte-for-byte -- the full MRAM erase + program + read path.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    The block round-tripped intact.
 * @retval k_ra8_err_checksum_mismatch The read-back bytes differed.
 * @retval (other)                    The first failing block-device call's code.
 *
 * @pre ::demo_open returned k_ra8_ok; ::s_bd is bound.
 * @pre ::k_demo_test_lba + ::k_demo_block_count is within the window.
 * @post On success block ::k_demo_test_lba holds the verified pattern.
 * @post No resource is leaked on any return path.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t demo_roundtrip(void)
{
  uint8_t data[(size_t)k_demo_payload];
  for (uint32_t i = 0; i < (uint32_t)k_demo_payload; ++i) {
    data[i] = (uint8_t)((i * (uint32_t)k_demo_seed_mul) + (uint32_t)k_demo_seed_add);
  }
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_erase(&s_bd, (uint32_t)k_demo_test_lba, (uint32_t)k_demo_block_count),
    s_tag,
    "erase");
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_write(&s_bd, (uint32_t)k_demo_test_lba, (uint32_t)k_demo_block_count, data),
    s_tag,
    "write");
  uint8_t got[(size_t)k_demo_payload] = {};
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_read(&s_bd, (uint32_t)k_demo_test_lba, (uint32_t)k_demo_block_count, got),
    s_tag,
    "read");
  if (memcmp(got, data, sizeof(data)) != 0) {
    return k_ra8_err_checksum_mismatch;
  }
  return k_ra8_ok;
}

/**
 * @brief Firmware entry point.
 *
 * @details Initialises logging + console, brings up the MRAM volume, runs the
 *          program/erase round-trip, and prints a single PASS/FAIL verdict.
 *
 * @return Never returns.
 * @retval (none) The function does not return (final `while (true)`).
 *
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @pre The extra-MRAM region is present (modelled in board_sim, real on silicon).
 * @post Exactly one PASS or FAIL verdict line has been queued on SCI8.
 * @post Control parks in an infinite loop; the function never returns.
 *
 * @note Single-threaded; runs to the park loop on the main stack.
 * @since 0.1.0
 */
int main(void)
{
  ra8_log_init();
  demo_setup_or_halt();
  (void)ra8_io_stream_uart_init(&s_uart, &s_ust, (uint8_t)k_demo_uart_chan);
  (void)ra8_io_log_attach(&s_uart); /* route ra8_log into the UART stream too */
  demo_print("ra8_io_mram_demo: boot\r\n");

  ra8_err_t e = demo_open();
  if (e == k_ra8_ok) {
    e = demo_roundtrip();
  }
  if (e == k_ra8_ok) {
    demo_print("ra8_io_mram_demo: 512-byte block erase/program/read on extra MRAM PASS\r\n");
  } else {
    demo_print("ra8_io_mram_demo: FAIL\r\n");
  }
  (void)ra8_sci_flush((uint8_t)k_demo_uart_chan);
  while (true) {
  }
}
