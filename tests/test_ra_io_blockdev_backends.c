/**
 * @file test_ra_io_blockdev_backends.c
 * @brief Unit tests for the ra_io hardware block-device backends (issue #156).
 *
 * @details
 * Exercises the backends that are reachable under `RA_SIMULATOR_MODE`:
 *   - sdram: full read/write/erase/caps over the simulated SDRAM window.
 *   - xspi : geometry validation plus a NOR read-modify-write round-trip over
 *            the `ra_xspi` simulator's 4 KiB fake-flash, including the property
 *            that a write preserves untouched blocks in the same sector.
 *   - mram : the data-MRAM fence (reject mis-aligned / out-of-region windows),
 *            capabilities, and a memory-mapped read of poked data.
 *   - sdspi / sdhi: bind + NULL-guard (the data path needs a real card, so it
 *            is exercised by board_sim / HIL, not here).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_flash_regs.h"
#include "ra8d2_ospi_regs.h"
#include "ra_err.h"
#include "ra_io_blockdev.h"
#include "ra_io_blockdev_mram.h"
#include "ra_io_blockdev_ram.h"
#include "ra_io_blockdev_sdhi.h"
#include "ra_io_blockdev_sdram.h"
#include "ra_io_blockdev_sdspi.h"
#include "ra_io_blockdev_xspi.h"
#include "ra_xspi.h"
#include "unity_minimal.h"

/**
 * @enum t_backend_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_sdram_blocks   = 64,     /**< 32 KiB inside the simulated SDRAM window.   */
  k_t_xspi_blocks    = 8,      /**< 4 KiB == the sim fake-flash (one sector).   */
  k_t_mram_blocks    = 4,      /**< 2 KiB inside the 12 KiB extra-MRAM region.  */
  k_t_sdram_oversize = 200000, /**< > 64 MiB / 512 -- rejected by sdram init.  */
} t_backend_const_t;

/* =============================================================================
 * sdram
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- sdram init rejects NULL/zero/oversize via
 * independent single-condition guards, then a read/write/erase round-trip)
 */
static void test_sdram_backend(void)
{
  TEST_BEGIN("sdram backend");
  ra_io_blockdev_t           bd    = {};
  ra_io_blockdev_ram_state_t state = {};
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_io_blockdev_sdram_init(nullptr, &state, k_t_sdram_blocks));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_io_blockdev_sdram_init(&bd, nullptr, k_t_sdram_blocks));
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_io_blockdev_sdram_init(&bd, &state, 0));
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_io_blockdev_sdram_init(&bd, &state, k_t_sdram_oversize));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_sdram_init(&bd, &state, k_t_sdram_blocks));

  ra_io_blockdev_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_get_caps(&bd, &caps));
  TEST_ASSERT_EQ(k_t_sdram_blocks, caps.block_count);
  TEST_ASSERT_EQ(k_ra_io_erase_value_zero, caps.erase_value);

  uint8_t out[(size_t)k_ra_io_block_size_bytes];
  for (uint32_t i = 0; i < (uint32_t)k_ra_io_block_size_bytes; ++i) {
    out[i] = (uint8_t)((i * 5u + 1u) & 0xFFu);
  }
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_write(&bd, 3, 1, out));
  uint8_t in[(size_t)k_ra_io_block_size_bytes] = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_read(&bd, 3, 1, in));
  TEST_ASSERT(memcmp(in, out, sizeof(in)) == 0);

  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_erase(&bd, 3, 1));
  (void)memset(in, 0xAA, sizeof(in));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_read(&bd, 3, 1, in));
  TEST_ASSERT_EQ(0, in[0]);
  TEST_ASSERT_EQ(0, in[(size_t)k_ra_io_block_size_bytes - 1u]);
  TEST_END("sdram backend");
}

/* =============================================================================
 * xspi (OSPI NOR) -- read-modify-write over the sim fake-flash
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- xspi geometry init rejects each bad
 * argument via independent single-condition guards)
 */
static void test_xspi_geom(void)
{
  TEST_BEGIN("xspi geom validation");
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  /* instance out of range */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_io_blockdev_xspi_init(&bd, &state, 2, 0, 8, false));
  /* base offset not sector-aligned */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_io_blockdev_xspi_init(&bd, &state, 0, 1, 8, false));
  /* block_count not a whole number of sectors */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_io_blockdev_xspi_init(&bd, &state, 0, 0, 5, false));
  /* zero block_count */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_io_blockdev_xspi_init(&bd, &state, 0, 0, 0, false));
  /* NULL guards */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_io_blockdev_xspi_init(nullptr, &state, 0, 0, 8, false));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_io_blockdev_xspi_init(&bd, nullptr, 0, 0, 8, false));
  TEST_END("xspi geom validation");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- erase the sector, write one block, read
 * it back; write a different pair of blocks and confirm the first block is
 * preserved by the whole-sector read-modify-write)
 */
static void test_xspi_rmw_roundtrip(void)
{
  TEST_BEGIN("xspi RMW round-trip");
  TEST_ASSERT_EQ(k_ra_ok, ra_xspi_init(0, k_ra_xspi_lio_1s1s1s));
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_xspi_state_t state = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_xspi_init(&bd, &state, 0, 0, k_t_xspi_blocks, false));

  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_erase(&bd, 0, k_t_xspi_blocks));

  uint8_t a[(size_t)k_ra_io_block_size_bytes];
  for (uint32_t i = 0; i < (uint32_t)k_ra_io_block_size_bytes; ++i) {
    a[i] = (uint8_t)((i ^ 0x5Au) & 0xFFu);
  }
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_write(&bd, 2, 1, a));

  uint8_t got[(size_t)k_ra_io_block_size_bytes] = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_read(&bd, 2, 1, got));
  TEST_ASSERT(memcmp(got, a, sizeof(got)) == 0);

  /* Write blocks 5..6 (same 4 KiB sector); block 2 must survive the RMW. */
  uint8_t b[(size_t)k_ra_io_block_size_bytes * 2u];
  (void)memset(b, 0x3C, sizeof(b));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_write(&bd, 5, 2, b));

  (void)memset(got, 0, sizeof(got));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_read(&bd, 2, 1, got));
  TEST_ASSERT(memcmp(got, a, sizeof(got)) == 0);
  uint8_t got2[(size_t)k_ra_io_block_size_bytes * 2u] = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_read(&bd, 5, 2, got2));
  TEST_ASSERT(memcmp(got2, b, sizeof(got2)) == 0);
  TEST_END("xspi RMW round-trip");
}

/* =============================================================================
 * mram -- fence + caps + memory-mapped read
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- the fence is a chain of single-condition
 * guards; each rejection and the accept path is hit)
 */
static void test_mram_fence(void)
{
  TEST_BEGIN("mram fence");
  ra_io_blockdev_t            bd    = {};
  ra_io_blockdev_mram_state_t state = {};
  const uintptr_t             base  = (uintptr_t)k_ra_flash_extra_start;

  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_io_blockdev_mram_init(nullptr, &state, base, k_t_mram_blocks, false));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_io_blockdev_mram_init(&bd, nullptr, base, k_t_mram_blocks, false));
  /* misaligned base (not 32-byte aligned) */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_io_blockdev_mram_init(&bd, &state, base + 1u, k_t_mram_blocks, false));
  /* base in the forbidden code-MRAM region */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_io_blockdev_mram_init(&bd,
                                          &state,
                                          (uintptr_t)k_ra_flash_code_start,
                                          k_t_mram_blocks,
                                          false));
  /* window runs past the 12 KiB extra region */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_io_blockdev_mram_init(&bd, &state, base, 64, false));
  /* valid */
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_mram_init(&bd, &state, base, k_t_mram_blocks, false));

  ra_io_blockdev_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_get_caps(&bd, &caps));
  TEST_ASSERT_EQ(k_t_mram_blocks, caps.block_count);
  TEST_ASSERT_EQ(k_ra_io_erase_value_ones, caps.erase_value);
  TEST_ASSERT(caps.must_erase_before_write);

  /* memory-mapped read: poke known bytes into the window, read them back */
  volatile uint8_t* win = (volatile uint8_t*)base;
  for (uint32_t i = 0; i < (uint32_t)k_ra_io_block_size_bytes; ++i) {
    win[i] = (uint8_t)((i + 9u) & 0xFFu);
  }
  uint8_t got[(size_t)k_ra_io_block_size_bytes] = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_read(&bd, 0, 1, got));
  for (uint32_t i = 0; i < (uint32_t)k_ra_io_block_size_bytes; ++i) {
    TEST_ASSERT_EQ((uint8_t)((i + 9u) & 0xFFu), got[i]);
  }
  TEST_END("mram fence");
}

/* =============================================================================
 * sdspi / sdhi -- bind + NULL guard (data path is board_sim / HIL)
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- each init rejects NULL and binds a valid
 * handle)
 */
static void test_sdspi_sdhi_bind(void)
{
  TEST_BEGIN("sdspi/sdhi bind");
  ra_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_io_blockdev_sdspi_init(nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_sdspi_init(&bd));
  ra_io_blockdev_t bd2 = {};
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_io_blockdev_sdhi_init(nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_blockdev_sdhi_init(&bd2));
  TEST_END("sdspi/sdhi bind");
}

int32_t main(void)
{
  test_sdram_backend();
  test_xspi_geom();
  test_xspi_rmw_roundtrip();
  test_mram_fence();
  test_sdspi_sdhi_bind();
  (void)fprintf(stderr, "[OK  ] test_ra_io_blockdev_backends.c\n");
  return 0;
}
