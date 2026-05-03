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
  flash_journal_pack(counter, rec);
  if (ra_xspi_flash_erase_sector((uint8_t)k_journal_xspi_instance,
                                 (uint32_t)k_journal_record_addr) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  if (ra_xspi_flash_program((uint8_t)k_journal_xspi_instance,
                            (uint32_t)k_journal_record_addr,
                            rec,
                            (uint32_t)k_journal_record_bytes) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  if (ra_xspi_flash_read((uint8_t)k_journal_xspi_instance,
                         (uint32_t)k_journal_record_addr,
                         back,
                         (uint32_t)k_journal_record_bytes) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  *echoed = flash_journal_unpack(back);
  return k_ra_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
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
  if (ra_xspi_init((uint8_t)k_journal_xspi_instance, k_ra_xspi_lio_1s1s1s) != k_ra_ok) {
    flash_journal_panic_halt();
  }
  ra_isr_globals_enable();

  uint32_t counter = 0U;
  while (1) {
    uint32_t       echoed = 0U;
    const ra_err_t err    = flash_journal_round_trip(counter, &echoed);
    if (err == k_ra_ok && echoed == counter) {
      (void)ra_board_led_toggle(k_ra_board_led1);
    } else {
      (void)ra_board_led_toggle(k_ra_board_led2);
    }
    counter++;
    ra_delay_ms(k_journal_period_ms);
  }
  flash_journal_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
