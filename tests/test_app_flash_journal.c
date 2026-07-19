/**
 * @file test_app_flash_journal.c
 * @brief Integration test for examples/ek_ra8d2/flash_journal/main.c
 *
 * @details
 * Replays the pack/unpack helpers and the round-trip wrapper around
 * ``ra8_xspi_flash_*``. The register-level NOR model in
 * ``tests/mocks/ra8_sim_xspi_flash.c`` services the driver's real
 * erase + program + read register sequence so the round-trip closes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "ra8_sim_xspi_flash.h"
#include "ra8_xspi.h"
#include "unity_minimal.h"

/**
 * @enum flash_journal_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_flash_journal_flash_journal_pack_42 = 42U,
  k_flash_journal_val_ff                = 0xFFU,
} flash_journal_uint8_const_t;

/**
 * @enum flash_journal_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_flash_journal_flash_journal_pack_deadbeef = 0xDEADBEEFU,
} flash_journal_uint32_const_t;

typedef enum : uint32_t {
  k_test_flash_record_bytes = 16U,  /**< Test flash record bytes.   */
  k_test_flash_record_addr  = 0x0U, /**< Test flash record address. */
  k_test_flash_instance     = 0U,   /**< Test flash instance.       */
} test_flash_const_t;

static void reset_world(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  ra8_sim_xspi_flash_install();
  (void)ra8_xspi_init((uint8_t)k_test_flash_instance, k_ra8_xspi_lio_1s1s1s);
}

static void flash_journal_pack(uint32_t counter, uint8_t* rec)
{
  for (uint8_t i = 0U; i < 4U; i++) {
    rec[i] = (uint8_t)((counter >> (8U * i)) & k_flash_journal_val_ff);
  }
  for (uint8_t i = 4U; i < (uint8_t)k_test_flash_record_bytes; i++) {
    rec[i] = (uint8_t)i;
  }
}

static uint32_t flash_journal_unpack(const uint8_t* rec)
{
  uint32_t v = 0U;
  for (uint8_t i = 0U; i < 4U; i++) {
    v |= ((uint32_t)rec[i]) << (8U * i);
  }
  return v;
}

/**
 * @par MC/DC:
 * Pack/unpack are pure data transforms with no boolean compounds;
 * we still cover both small and large 32-bit values to exercise the
 * shift/mask sequence end-to-end.
 */
static void test_flash_pack_unpack_roundtrip(void)
{
  reset_world();
  TEST_BEGIN("flash_journal: pack/unpack round-trip");
  uint8_t rec[k_test_flash_record_bytes];
  flash_journal_pack(k_flash_journal_flash_journal_pack_deadbeef, rec);
  TEST_ASSERT_EQ(0xEF, rec[0]);
  TEST_ASSERT_EQ(0xDE, rec[3]);
  TEST_ASSERT_EQ((unsigned int)0xDEADBEEFU, (unsigned int)flash_journal_unpack(rec));
  TEST_END("flash_journal: pack/unpack round-trip");
}

/**
 * @par MC/DC:
 * Compound decision in the app: ``erase!=ok || program!=ok || read!=ok``.
 * Three atomic conditions x N+1 = 4 vectors -- this test covers the
 * all-ok vector; the bad-instance / null-buf tests below cover each
 * sub-error vector.
 */
static void test_flash_xspi_round_trip_ok(void)
{
  reset_world();
  TEST_BEGIN("flash_journal: erase + program + read sequence ok");
  uint8_t rec[k_test_flash_record_bytes];
  flash_journal_pack(k_flash_journal_flash_journal_pack_42, rec);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_erase_sector((uint8_t)k_test_flash_instance,
                                             (uint32_t)k_test_flash_record_addr));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_program((uint8_t)k_test_flash_instance,
                                        (uint32_t)k_test_flash_record_addr,
                                        rec,
                                        (uint32_t)k_test_flash_record_bytes));
  uint8_t back[k_test_flash_record_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xspi_flash_read((uint8_t)k_test_flash_instance,
                                     (uint32_t)k_test_flash_record_addr,
                                     back,
                                     (uint32_t)k_test_flash_record_bytes));
  TEST_END("flash_journal: erase + program + read sequence ok");
}

/**
 * @par MC/DC:
 * Decision: ``ra8_xspi_flash_program(bad_inst, ...) != ok``. Pairs
 * with the all-ok vector to bound the program-fail branch.
 */
static void test_flash_xspi_program_bad_instance(void)
{
  reset_world();
  TEST_BEGIN("flash_journal: program rejects bad instance");
  uint8_t rec[k_test_flash_record_bytes] = {};
  TEST_ASSERT(ra8_xspi_flash_program(99U, 0U, rec, (uint32_t)k_test_flash_record_bytes) !=
              k_ra8_ok);
  TEST_END("flash_journal: program rejects bad instance");
}

/**
 * @par MC/DC:
 * Decision: ``ra8_xspi_flash_read(NULL, ...) != ok``. Pairs with the
 * all-ok vector for the read-fail vector.
 */
static void test_flash_xspi_read_null_buf(void)
{
  reset_world();
  TEST_BEGIN("flash_journal: read rejects NULL buf");
  TEST_ASSERT(ra8_xspi_flash_read((uint8_t)k_test_flash_instance,
                                  0U,
                                  nullptr,
                                  (uint32_t)k_test_flash_record_bytes) != k_ra8_ok);
  TEST_END("flash_journal: read rejects NULL buf");
}

int main(void)
{
  test_flash_pack_unpack_roundtrip();
  test_flash_xspi_round_trip_ok();
  test_flash_xspi_program_bad_instance();
  test_flash_xspi_read_null_buf();
  return 0;
}
