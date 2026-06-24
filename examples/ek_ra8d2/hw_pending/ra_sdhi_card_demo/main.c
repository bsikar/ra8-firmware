/**
 * @file examples/ek_ra8d2/hw_pending/ra_sdhi_card_demo/main.c
 * @brief Native 4-bit SDHI raw-block round-trip on the EK-RA8D2 microSD (#123).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The minimal #123 deliverable: bring up the on-board microSD through the
 * dedicated **SDHI** 4-bit host controller and prove a *raw* 512-byte block
 * round-trip straight against the `ra_sdcard` HAL -- no `ra_io`, no `ra_fs`,
 * no file system. The SD Physical Layer identification
 * (CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 -> CMD9 -> CMD7) and the polled
 * 512-byte block read / write over the SD_BUF0 FIFO live in `ra_sdcard` +
 * `ra_sdhi`; this app only routes the bus, runs init, then:
 *
 *   1. `ra_sdcard_get_capacity` -- confirm the card reports a non-zero size.
 *   2. fill a 512-byte buffer with a deterministic LCG pattern.
 *   3. `ra_sdcard_write_blocks(lba, payload, 1)` -- write one block.
 *   4. `ra_sdcard_read_blocks(lba, readback, 1)` -- read it back.
 *   5. byte-compare the read-back against the written payload.
 *
 * On a clean round-trip it prints exactly
 * `ra_sdhi_card_demo: native SDHI block round-trip PASS`; any failed step prints
 * `ra_sdhi_card_demo: FAIL` and parks the CPU.
 *
 * Under board_sim the native-SDHI host-controller model (`board_periph_sdhi.c`)
 * serves a card attached with `--sd-new 64:fat16`. On the bench a real microSD
 * is wired to the port-4 SDHI bus. THIS APP OVERWRITES ONE BLOCK of the card.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_log.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_sdcard.h"
#include "ra_sdhi.h"
#include "ra_time.h"

/* =============================================================================
 * Tunables
 * =============================================================================
 */

/**
 * @enum sdhi_card_config_t
 * @brief Compile-time settings for the native-SDHI raw-block round-trip.
 */
typedef enum : uint32_t {
  k_sdhi_card_uart_baud       = 115200U,      /**< J-Link OB CDC console baud.            */
  k_sdhi_card_uart_channel    = 8U,           /**< SCI8 console (TXD8=PD02, RXD8=PD03).   */
  k_sdhi_card_instance        = 0U,           /**< SDHI0 drives the on-board microSD.     */
  k_sdhi_card_pin_count       = 8U,           /**< CMD/CLK/DAT0..3/WP/CD on port 4.       */
  k_sdhi_card_block_bytes     = 512U,         /**< One SD block.                          */
  k_sdhi_card_test_lba        = 64U,          /**< Block to round-trip (clear of the BPB).*/
  k_sdhi_card_block_count     = 1U,           /**< Round-trip one block.                  */
  k_sdhi_card_prng_seed       = 0xC0FFEE11UL, /**< Deterministic payload seed.            */
  k_sdhi_card_prng_mul        = 1664525UL,    /**< Numerical Recipes LCG multiplier.      */
  k_sdhi_card_prng_add        = 1013904223UL, /**< Numerical Recipes LCG increment.       */
  k_sdhi_card_prng_byte_shift = 16U,          /**< Bit shift selecting the PRNG byte.     */
  k_sdhi_card_byte_mask       = 0xFFU,        /**< Low-byte mask.                         */
} sdhi_card_config_t;

/* The test block must sit above block 0 (the FAT BPB) so the round-trip never
 * clobbers the boot sector; promote the doc-only @pre to a compile-time check. */
static_assert((uint32_t)k_sdhi_card_test_lba > 0U,
              "ra_sdhi_card_demo test LBA must be above block 0 (the FAT BPB)");

/** @brief Port-pin packing shift: the high byte holds the port index. */
typedef enum : uint16_t {
  k_sdhi_card_port_shift = 8U, /**< ra_port_pin_t = (port << 8) | pin. */
} sdhi_card_pack_t;

/* =============================================================================
 * Pinout (SCI8 console + SDHI bus pins)
 * =============================================================================
 */

/** @brief SCI8 console TXD = PD02 (UM Table 26 console pinmap). */
static const ra_port_pin_t k_sdhi_card_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << (uint16_t)k_sdhi_card_port_shift) |
                  (uint16_t)k_ra_pin_2);
/** @brief SCI8 console RXD = PD03 (UM Table 26 console pinmap). */
static const ra_port_pin_t k_sdhi_card_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << (uint16_t)k_sdhi_card_port_shift) |
                  (uint16_t)k_ra_pin_3);

/** @brief SDHI bus pins (port 4, pins 0..7: CMD/CLK/DAT0..3/WP/CD). */
static const ra_port_pin_t k_sdhi_card_pins[k_sdhi_card_pin_count] = {
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << (uint16_t)k_sdhi_card_port_shift) | (uint16_t)k_ra_pin_0),
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << (uint16_t)k_sdhi_card_port_shift) | (uint16_t)k_ra_pin_1),
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << (uint16_t)k_sdhi_card_port_shift) | (uint16_t)k_ra_pin_2),
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << (uint16_t)k_sdhi_card_port_shift) | (uint16_t)k_ra_pin_3),
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << (uint16_t)k_sdhi_card_port_shift) | (uint16_t)k_ra_pin_4),
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << (uint16_t)k_sdhi_card_port_shift) | (uint16_t)k_ra_pin_5),
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << (uint16_t)k_sdhi_card_port_shift) | (uint16_t)k_ra_pin_6),
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << (uint16_t)k_sdhi_card_port_shift) | (uint16_t)k_ra_pin_7),
};

/* =============================================================================
 * Static message strings (ASCII-only per project policy)
 * =============================================================================
 */

static const uint8_t k_msg_boot[]      = "ra_sdhi_card_demo: boot\r\n";
static const uint8_t k_msg_card_ok[]   = "ra_sdhi_card_demo: card ready\r\n";
static const uint8_t k_msg_init_fail[] = "ra_sdhi_card_demo: FAIL init\r\n";
static const uint8_t k_msg_pass[]      = "ra_sdhi_card_demo: native SDHI block round-trip PASS\r\n";
static const uint8_t k_msg_fail[]      = "ra_sdhi_card_demo: FAIL\r\n";

/** @brief Module log tag. */
static const char* const s_tag = "ra_sdhi_card_demo";

/* =============================================================================
 * UART output helpers
 * =============================================================================
 */

/**
 * @brief Write a byte run on the SCI8 console.
 *
 * @details Thin wrapper over `ra_sci_write_polling`; the return value is
 *          intentionally discarded because a diagnostic-print failure has no
 *          useful recovery on a panic path.
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
static void sdhi_card_print(const uint8_t* msg, uint32_t len)
{
  (void)ra_sci_write_polling((uint8_t)k_sdhi_card_uart_channel, msg, len);
}

/** @brief Emit a NUL-terminated literal (length via sizeof at the call site). */
#define SDHI_CARD_PUTS(lit) sdhi_card_print((lit), (uint32_t)sizeof(lit) - 1U)

/**
 * @brief Halt forever in WFI -- panic stop on irrecoverable failure.
 *
 * @details Used after a fatal error has already been reported on the console; it
 *          parks the core so a debugger or the HIL runner can observe the state.
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
static void sdhi_card_panic_halt(void)
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
 * @brief Route SCI8 console pins to PD02 / PD03.
 *
 * @details Routes both console pins to the SCI async function via PFS; returns
 *          on the first routing failure so the caller can panic-halt.
 *
 * @return ra_err_t from the PFS routing calls.
 * @retval k_ra_ok    Both console pins routed.
 * @retval k_ra_err_* PFS routing failure.
 *
 * @pre IOPORT is reachable.
 * @pre The console SCI module clock is enabled.
 * @post PD02/PD03 are in SCI async mode on success.
 * @post On failure the pins are left in their prior state.
 *
 * @note Not thread-safe; call during single-threaded init.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sdhi_card_console_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_sdhi_card_pin_txd, k_ra_psel_sci_async, "sdhicard.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_sdhi_card_pin_rxd, k_ra_psel_sci_async, "sdhicard.rxd8");
}

/**
 * @brief Route the eight SDHI bus pins to `PSEL = k_ra_psel_sdhi`.
 *
 * @details Walks the port-4 pin list (CMD/CLK/DAT0..3/WP/CD), routing each to
 *          the SDHI peripheral function via PFS; returns on the first failure.
 *
 * @return ra_err_t from the routing calls.
 * @retval k_ra_ok    All eight SDHI pins routed.
 * @retval k_ra_err_* The first failing pin's routing code.
 *
 * @pre IOPORT is reachable.
 * @pre The SDHI module clock is enabled by `ra_sdhi_init` (later).
 * @post On success port-4 pins 0..7 are owned by the SDHI block.
 * @post On failure the affected pins are left in their prior state.
 *
 * @note Not thread-safe; call during single-threaded init.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sdhi_card_bus_pins_init(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_sdhi_card_pin_count; ++i) {
    const ra_err_t err =
      ra_pfs_route_peripheral(k_sdhi_card_pins[i], k_ra_psel_sdhi, "sdhicard.bus");
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Bring up CGC + SysTick + console SCI + SDHI bus pins; panic on fail.
 *
 * @details Initialises the clock generator, caches CPUCLK0 and PCLKA, starts the
 *          SysTick time base, routes the console pins, configures SCI8 async, and
 *          routes the eight SDHI bus pins. Any failing step panic-halts.
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
static void sdhi_card_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    sdhi_card_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    sdhi_card_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    sdhi_card_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    sdhi_card_panic_halt();
  }
  if (sdhi_card_console_pins_init() != k_ra_ok) {
    sdhi_card_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = (uint32_t)k_sdhi_card_uart_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_sdhi_card_uart_channel, &sci_cfg) != k_ra_ok) {
    sdhi_card_panic_halt();
  }
  if (sdhi_card_bus_pins_init() != k_ra_ok) {
    sdhi_card_panic_halt();
  }
}

/**
 * @brief Init the SD card over native SDHI, panic-halt on failure.
 *
 * @details Runs the full SD Physical Layer identification through `ra_sdcard_init`
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
static void sdhi_card_init_or_halt(void)
{
  const ra_sdcard_cfg_t cfg = {.instance = (uint8_t)k_sdhi_card_instance};
  if (ra_sdcard_init(&cfg) != k_ra_ok) {
    SDHI_CARD_PUTS(k_msg_init_fail);
    sdhi_card_panic_halt();
  }
  SDHI_CARD_PUTS(k_msg_card_ok);
}

/* =============================================================================
 * Payload + raw-block round-trip
 * =============================================================================
 */

/** @brief Static payload + read-back buffers (no heap; NASA Rule 3). */
static uint8_t s_payload[k_sdhi_card_block_bytes];
static uint8_t s_readback[k_sdhi_card_block_bytes];

/**
 * @brief Fill the payload buffer with a deterministic LCG byte sequence.
 *
 * @details Runs a Numerical-Recipes LCG over `s_payload`, selecting one byte per
 *          step, so the written content is reproducible and the read-back compare
 *          is a strong end-to-end check of the SDHI block path.
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
static void sdhi_card_fill_payload(void)
{
  uint32_t state = (uint32_t)k_sdhi_card_prng_seed;
  for (uint32_t i = 0U; i < (uint32_t)k_sdhi_card_block_bytes; i++) {
    state        = (state * (uint32_t)k_sdhi_card_prng_mul) + (uint32_t)k_sdhi_card_prng_add;
    s_payload[i] = (uint8_t)((state >> k_sdhi_card_prng_byte_shift) & k_sdhi_card_byte_mask);
  }
}

/**
 * @brief Confirm the card capacity covers the test block, then check it is sane.
 *
 * @details Queries `ra_sdcard_get_capacity` and verifies the reported block
 *          count is non-zero and leaves room for the single test block at
 *          ::k_sdhi_card_test_lba, so the write / read below stay in range.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Capacity covers the test block.
 * @retval k_ra_err_out_of_range  The card is too small for the test block.
 * @retval k_ra_err_*             `ra_sdcard_get_capacity` failure.
 *
 * @pre `ra_sdcard_init` returned k_ra_ok.
 * @pre The test LBA is a compile-time constant.
 * @post Card state is unchanged (pure query).
 * @post On k_ra_ok the test LBA is within the card.
 *
 * @note Not thread-safe; single-threaded path.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sdhi_card_check_capacity(void)
{
  uint32_t blocks = 0U;
  RA_RETURN_ON_ERROR(ra_sdcard_get_capacity(&blocks), s_tag, "capacity");
  if (blocks <= (uint32_t)k_sdhi_card_test_lba) {
    return k_ra_err_out_of_range;
  }
  return k_ra_ok;
}

/**
 * @brief Write the payload, read it back, and byte-compare (raw SDHI blocks).
 *
 * @details Calls `ra_sdcard_write_blocks` then `ra_sdcard_read_blocks` for one
 *          512-byte block at ::k_sdhi_card_test_lba, then verifies the read-back
 *          equals the written payload. No file system is involved -- this is the
 *          native-SDHI block primitive end-to-end.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                    Read-back matched the written payload.
 * @retval k_ra_err_checksum_mismatch The read-back differed.
 * @retval k_ra_err_*                 `ra_sdcard` read / write failure.
 *
 * @pre `ra_sdcard_init` returned k_ra_ok and `s_payload` is filled.
 * @pre The test LBA is within the card capacity.
 * @post On k_ra_ok the block at the test LBA holds `s_payload`.
 * @post `s_readback` holds the bytes read back from the card.
 *
 * @note Not thread-safe; mutates one card block + the read-back buffer.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sdhi_card_block_roundtrip(void)
{
  RA_RETURN_ON_ERROR(ra_sdcard_write_blocks((uint32_t)k_sdhi_card_test_lba, s_payload,
                                            (uint32_t)k_sdhi_card_block_count),
                     s_tag, "write");
  memset(s_readback, 0, sizeof(s_readback));
  RA_RETURN_ON_ERROR(ra_sdcard_read_blocks((uint32_t)k_sdhi_card_test_lba, s_readback,
                                           (uint32_t)k_sdhi_card_block_count),
                     s_tag, "read");
  if (memcmp(s_payload, s_readback, (size_t)k_sdhi_card_block_bytes) != 0) {
    return k_ra_err_checksum_mismatch;
  }
  return k_ra_ok;
}

/**
 * @brief Run the capacity check + raw-block round-trip in order.
 *
 * @details Composes the two helpers so the first failing step short-circuits
 *          with its code; the success path is a single line per step.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok    Capacity ok and the block round-trip matched.
 * @retval k_ra_err_* The first failing step's code.
 *
 * @pre `ra_sdcard_init` returned k_ra_ok and `s_payload` is filled.
 * @pre The test LBA is a compile-time constant.
 * @post On k_ra_ok the card block at the test LBA holds the verified payload.
 * @post On any non-ok return the partial state is abandoned (caller halts).
 *
 * @note Not thread-safe; single-threaded round-trip.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sdhi_card_roundtrip(void)
{
  RA_RETURN_ON_ERROR(sdhi_card_check_capacity(), s_tag, "capacity check");
  RA_RETURN_ON_ERROR(sdhi_card_block_roundtrip(), s_tag, "block roundtrip");
  return k_ra_ok;
}

/* =============================================================================
 * Main
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: bring up the bus + card, raw-block round-trip, print PASS.
 *
 * @details Brings up the clocks, console, and SDHI bus pins, runs the native SD
 *          card identification, fills the payload, then writes + reads + compares
 *          one raw 512-byte block straight against `ra_sdcard`. On success it
 *          prints the exact PASS banner; on any failure it prints `FAIL` and parks
 *          the core.
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
  sdhi_card_setup_or_halt();
  ra_isr_globals_enable();
  ra_log_init();
  SDHI_CARD_PUTS(k_msg_boot);

  sdhi_card_init_or_halt();
  sdhi_card_fill_payload();

  const ra_err_t r = sdhi_card_roundtrip();
  if (r != k_ra_ok) {
    SDHI_CARD_PUTS(k_msg_fail);
    sdhi_card_panic_halt();
  }
  SDHI_CARD_PUTS(k_msg_pass);
  (void)ra_sci_flush((uint8_t)k_sdhi_card_uart_channel);

  while (true) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
