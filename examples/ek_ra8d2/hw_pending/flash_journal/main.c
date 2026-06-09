/**
 * @file examples/ek_ra8d2/flash_journal/main.c
 * @brief Octo-SPI flash journal demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Treats the on-board 64 MB Octo-SPI flash as a tiny append-only
 * journal of 16-byte records. Each boot:
 *   1. Erases the first 4 KiB sector (idempotent restart).
 *   2. Reads back the first record (or zeroes if blank).
 *   3. Programs an incrementing counter record.
 *   4. Reads back the new record and toggles LED1 if the round-trip
 *      matches what was written.
 *
 * No filesystem layer (LevelX, FileX) is used -- this exercises
 * ``ra_xspi_flash_*`` directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_time.h"
#include "ra_xspi.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_journal_period_ms     = 1000U,
  k_journal_record_bytes  = 16U,
  k_journal_record_addr   = 0x0U,
  k_journal_xspi_instance = 0U,
  k_journal_counter_bytes = 4U,
  k_journal_byte_mask     = 0xFFU,
  k_journal_byte_shift    = 8U,
} flash_journal_const_t;

/**
 * @var g_fj_match
 * @brief HIL liveness counter -- incremented on every successful
 *        erase -> program -> read-back -> compare round-trip.
 *
 * @details
 * Read externally by scripts/hil_jlink_memprobe.sh via SWD. The probe
 * asserts this counter advances by >= HIL_PROBE_MIN_ADVANCE over the
 * sample window, proving the Octo-SPI flash peripheral actually wrote
 * and read back matching bytes (the alive-mode check could only prove
 * the chip didn't crash, not that flash actually round-tripped data).
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_fj_match = 0U;

/**
 * @var g_fj_mismatch
 * @brief HIL failure counter -- incremented on any erase, program, or
 *        read failure, or when the read-back bytes don't match what
 *        was just written.
 *
 * @details
 * The memprobe asserts this stays at 0 (or below
 * HIL_PROBE_MAX_FAILURE). Catches the silent-failure mode where the
 * peripheral starts up but writes silently drop, reads return stale
 * data, or the compare miscompares -- previously invisible because
 * the chip kept iterating the main loop happily.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_fj_mismatch = 0U;

/**
 * @var g_fj_last_step
 * @brief Last round-trip step value (J-Link memprobe diagnostic).
 *
 * @details
 * Encodes which step of the erase / program / read / compare chain
 * the firmware completed last. 0 = pre-loop init, 1 = erase ok,
 * 2 = program ok, 3 = read ok, 4 = match, 5 = erase err, 6 = program
 * err, 7 = read err, 8 = compare mismatch (echoed != counter). Lets
 * a memprobe pinpoint which OSPI op fails without UART.
 *
 * @note File-scope volatile so the optimiser cannot elide updates;
 *       read only by J-Link.
 * @since 0.1.0
 */
/**
 * @brief Step codes for ::g_fj_last_step (erase/program/read/compare chain).
 * @details Low codes are success milestones, high codes are the matching
 *          failure points; read via J-Link memprobe.
 */
typedef enum : uint32_t {
  k_fj_step_init             = 0U, /**< No round-trip run yet. */
  k_fj_step_erase_ok         = 1U, /**< Sector erase succeeded. */
  k_fj_step_program_ok       = 2U, /**< Record program succeeded. */
  k_fj_step_read_ok          = 3U, /**< Record read-back succeeded. */
  k_fj_step_compare_ok       = 4U, /**< Read-back matched. */
  k_fj_step_erase_failed     = 5U, /**< Sector erase failed. */
  k_fj_step_program_failed   = 6U, /**< Record program failed. */
  k_fj_step_read_failed      = 7U, /**< Record read-back failed. */
  k_fj_step_compare_mismatch = 8U, /**< Read-back differed (data error). */
} fj_step_t;

volatile uint32_t g_fj_last_step = k_fj_step_init;

/**
 * @var g_fj_last_counter
 * @brief Last counter the firmware tried to write.
 *
 * @details
 * Lets a memprobe compare against g_fj_last_echoed when
 * g_fj_last_step lands at 8 (compare mismatch).
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_fj_last_counter = 0U;

/**
 * @var g_fj_last_echoed
 * @brief Last counter value read back from flash.
 *
 * @details
 * Holds the result of flash_journal_unpack on the read-back record.
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_fj_last_echoed = 0U;

/**
 * @var g_fj_jedec_id
 * @brief One-shot JEDEC ID read at boot (memprobe diagnostic).
 *
 * @details
 * Stamped once by main right after ra_xspi_init returns. Lets a
 * memprobe confirm the OSPI controller actually clocks the bus by
 * reading the on-board flash's JEDEC ID. The EK-RA8D2 ships a Macronix
 * MX25UM25645G (manufacturer 0xC2), not the ISSI part the older comments
 * assumed. A value of 0x000000 means no ID returned; 0xFFFFFF means the
 * bus floated (no CIPO drive) -- on this board that means SW4 is not set
 * to all-OFF (see README.md / issue #44), NOT a dead chip.
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_fj_jedec_id = 0U;

/**
 * @var g_fj_expander_err
 * @brief Return code from the best-effort U15 Octo-SPI-active override.
 *
 * @details
 * 0 (k_ra_ok) means the U15 I/O expander ACKed the SW4 override write.
 * Note this does NOT mean the flash is connected: bench readback proved
 * the physical SW4 switches overpower the expander, so the physical DIP
 * still governs reachability. Non-zero means the expander did not respond
 * on RIIC1. Read externally by J-Link.
 *
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
/** @brief Sentinel for ::g_fj_expander_err meaning "no error captured yet". */
typedef enum : uint32_t {
  k_fj_err_none = 0xFFFFFFFFU,
} fj_err_sentinel_t;

volatile uint32_t g_fj_expander_err = k_fj_err_none;

/** @brief Park the CPU after a fatal init failure. */
static void flash_journal_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Pack a counter into a 16-byte record (little-endian + padding).
 *
 * @param[in]  counter Value to encode.
 * @param[out] rec     16-byte buffer to populate.
 */
static void flash_journal_pack(uint32_t counter, uint8_t* rec)
{
  for (uint8_t i = 0U; i < (uint8_t)k_journal_counter_bytes; i++) {
    rec[i] =
      (uint8_t)((counter >> ((uint32_t)k_journal_byte_shift * i)) & (uint32_t)k_journal_byte_mask);
  }
  for (uint8_t i = (uint8_t)k_journal_counter_bytes; i < (uint8_t)k_journal_record_bytes; i++) {
    rec[i] = i;
  }
}

/**
 * @brief Decode the counter from a 16-byte record.
 *
 * @param[in] rec 16-byte buffer.
 * @return Decoded little-endian uint32 from the first four bytes.
 */
static uint32_t flash_journal_unpack(const uint8_t* rec)
{
  uint32_t v = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_journal_counter_bytes; i++) {
    v |= ((uint32_t)rec[i]) << ((uint32_t)k_journal_byte_shift * i);
  }
  return v;
}

/**
 * @brief Erase + program + read-back round-trip of one record.
 *
 * @par MC/DC:
 * Compound decision: ``erase != ok || program != ok || read != ok``.
 * Three atomic conditions x N+1 = 4 vectors -- exercised by the
 * companion host test (each of erase/program/read fail independently).
 *
 * @param[in]  counter Sequence number to encode in the record.
 * @param[out] echoed  Counter actually read back from flash.
 *
 * @retval k_ra_ok           Round-trip succeeded.
 * @retval k_ra_err_hw_error Any of the three flash ops failed.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t flash_journal_round_trip(uint32_t counter, uint32_t* echoed)
{
  uint8_t rec[k_journal_record_bytes];
  uint8_t back[k_journal_record_bytes];
  g_fj_last_counter = counter;
  flash_journal_pack(counter, rec);
  if (ra_xspi_flash_erase_sector((uint8_t)k_journal_xspi_instance,
                                 (uint32_t)k_journal_record_addr) != k_ra_ok) {
    g_fj_last_step = k_fj_step_erase_failed;
    return k_ra_err_hw_error;
  }
  g_fj_last_step = k_fj_step_erase_ok;
  if (ra_xspi_flash_program((uint8_t)k_journal_xspi_instance,
                            (uint32_t)k_journal_record_addr,
                            rec,
                            (uint32_t)k_journal_record_bytes) != k_ra_ok) {
    g_fj_last_step = k_fj_step_program_failed;
    return k_ra_err_hw_error;
  }
  g_fj_last_step = k_fj_step_program_ok;
  if (ra_xspi_flash_read((uint8_t)k_journal_xspi_instance,
                         (uint32_t)k_journal_record_addr,
                         back,
                         (uint32_t)k_journal_record_bytes) != k_ra_ok) {
    g_fj_last_step = k_fj_step_read_failed;
    return k_ra_err_hw_error;
  }
  g_fj_last_step   = k_fj_step_read_ok;
  *echoed          = flash_journal_unpack(back);
  g_fj_last_echoed = *echoed;
  return k_ra_ok;
}

/**
 * @brief Run all init-time bring-up that must complete before the loop.
 *
 * @details
 * Brings CGC, SysTick, LEDs, OCTA pins, and OSPI controller up, then
 * does a one-shot JEDEC ID read so a memprobe can confirm the bus is
 * electrically alive. Panics on any failure other than the JEDEC read
 * (which is best-effort -- a floating bus returns 0x00FFFFFF and the
 * loop will surface the same fact via g_fj_last_step = 5).
 *
 * @pre Reset_Handler ran C-runtime init.
 * @post All HAL prereqs for the loop are live.
 * @since 0.1.0
 */
static void flash_journal_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    flash_journal_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    flash_journal_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    flash_journal_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    flash_journal_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    flash_journal_panic_halt();
  }
  /* Request the Octo-SPI SW4 layout via the U15 I/O expander. The
   * on-board Octo-SPI flash (U16) is connected only when SW4-1..8 are
   * physically OFF (Renesas FSP EK-RA8D2 ospi_b requirement). This call
   * drives the U15 outputs to that pattern, BUT bench readback proved the
   * physical SW4 switches overpower the expander (U15 latches 0xFF while
   * the actual pin levels read 0x00) -- so this is best-effort only. If
   * the JEDEC read below still returns 0x00FFFFFF, the physical SW4
   * switches are not all OFF. See README.md + issue #44. The result is
   * stamped to g_fj_expander_err for the memprobe. */
  const ra_err_t exp_err = ra_board_io_expander_set_octospi_active();
  g_fj_expander_err      = (uint32_t)exp_err;
  /* OCTA pins come out of reset as GPIO; ra_board_xspi_pins_init
   * routes them to PSEL=0x1C and pulses RESET_L on P106 so the
   * IS25LX512M lands in standard-SPI mode with deterministic state.
   * HUM Ch 20.6 "Multiplexed Pin Function Selector" p 871. */
  if (ra_board_xspi_pins_init() != k_ra_ok) {
    flash_journal_panic_halt();
  }
  if (ra_xspi_init((uint8_t)k_journal_xspi_instance, k_ra_xspi_lio_1s1s1s) != k_ra_ok) {
    flash_journal_panic_halt();
  }
  uint32_t jedec_id = 0U;
  (void)ra_xspi_flash_read_id((uint8_t)k_journal_xspi_instance, &jedec_id);
  g_fj_jedec_id = jedec_id;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  flash_journal_setup_or_halt();
  ra_isr_globals_enable();

  uint32_t counter = 0U;
  while (1) {
    uint32_t       echoed = 0U;
    const ra_err_t err    = flash_journal_round_trip(counter, &echoed);
    if (err == k_ra_ok && echoed == counter) {
      (void)ra_board_led_toggle(k_ra_board_led1);
      g_fj_match += 1U;
      g_fj_last_step = k_fj_step_compare_ok;
    } else {
      (void)ra_board_led_toggle(k_ra_board_led2);
      g_fj_mismatch += 1U;
      if (err == k_ra_ok) {
        g_fj_last_step = k_fj_step_compare_mismatch;
      }
    }
    counter++;
    ra_delay_ms(k_journal_period_ms);
  }
  flash_journal_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
