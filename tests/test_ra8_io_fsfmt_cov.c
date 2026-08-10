/**
 * @file test_ra8_io_fsfmt_cov.c
 * @brief Coverage supplement: uncovered branches in ra8_io_fsfmt.c.
 *
 * @details
 * Drives the branches that test_ra8_io_fsfmt.c does not reach:
 *   - Line 83:  internal_read_boot0 null read_block guard.
 *   - Line 139: fat_probe returns false when the block read fails.
 *   - Line 142: fat_probe returns false when the block carries the exFAT marker.
 *   - Line 148: fat_probe returns false when boot signature byte 1 (0xAA) is absent.
 *   - Line 177: exfat_probe returns false when the block read fails.
 *
 * Line 80 (internal_read_boot0 null-be guard) is annotated GCOVR_EXCL_LINE in
 * the source: ra8_io_fsfmt_probe validates backend non-null before dispatching to
 * any probe, so no host input can ever reach that branch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_fsfmt.h"
#include "unity_minimal.h"

/* ==========================================================================
 * Fixture constants
 * ========================================================================== */

/**
 * @enum cov_fsfmt_const_t
 * @brief Coverage-test fixture constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_small_blocks   = 8U,    /**< Block count for the small RAM fixture.       */
  k_cov_sig0_off       = 510U,  /**< Boot sector signature byte 0 offset.         */
  k_cov_sig1_off       = 511U,  /**< Boot sector signature byte 1 offset.         */
  k_cov_sig0_val       = 0x55U, /**< Correct boot-signature byte 0 value.         */
  k_cov_exfat_name_off = 3U,    /**< exFAT VBR filesystem-name field byte offset. */
  k_cov_exfat_name_len = 8U,    /**< Length of the "EXFAT   " name field.         */
} cov_fsfmt_const_t;

/** @brief Backing storage shared by the small RAM block device fixture. */
static uint8_t s_cov_disk[(size_t)k_cov_small_blocks * (size_t)k_ra8_io_block_size_bytes];

/* ==========================================================================
 * First-fail mock backend
 *
 * Used in test_fat_probe_rejects_exfat_marker to reach ra8_io_fsfmt.c
 * line 142. Call 1 (from exfat_probe) returns an error so that exfat_probe
 * reaches its read-failure return at line 177. Call 2 (from fat_probe)
 * returns a block bearing the exFAT filesystem name so that fat_probe
 * reaches the exFAT-rejection return at line 142.
 * ========================================================================== */

/** @brief Rolling call counter for mock_read_first_fail; cleared before use. */
static uint32_t s_cov_call;

/** @brief Block image returned by mock_read_first_fail on its second call. */
static uint8_t s_cov_exfat_blk[(size_t)k_ra8_io_block_size_bytes];

/**
 * @brief Mock read_block: returns an error on call 1, the exFAT block on call 2+.
 *
 * @details
 * Increments s_cov_call on every invocation. Call 1 returns k_ra8_err_hw_error to
 * make the exfat_probe's internal_read_boot0 fail (reaching line 177). Call 2
 * fills buf with s_cov_exfat_blk so that fat_probe finds the exFAT marker and
 * returns false at line 142.
 *
 * @param[in]  ctx   Unused; pass nullptr.
 * @param[in]  lba   Unused.
 * @param[in]  count Unused.
 * @param[out] buf   Receives s_cov_exfat_blk on the second and subsequent calls.
 *
 * @return ra8_err_t
 * @retval k_ra8_err_hw_error First call (s_cov_call == 1).
 * @retval k_ra8_ok           Second call and beyond; buf filled from s_cov_exfat_blk.
 *
 * @pre s_cov_call is 0 before the first call (reset by the test).
 * @pre s_cov_exfat_blk carries the desired block content before the second call.
 * @post s_cov_call is incremented by 1 on each call.
 * @post On the second call, buf[0..511] equals s_cov_exfat_blk.
 *
 * @note Not thread-safe; uses module-static state.
 *
 * @since 0.1.0
 */
static ra8_err_t mock_read_first_fail(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)count;
  s_cov_call++;
  if (s_cov_call == 1U) {
    return k_ra8_err_hw_error;
  }
  (void)memcpy(buf, s_cov_exfat_blk, sizeof(s_cov_exfat_blk));
  return k_ra8_ok;
}

/* ==========================================================================
 * Test functions
 * ========================================================================== */

/**
 * @brief Null read_block pointer causes both probes to fail (lines 83/139/177).
 *
 * @details
 * Constructs a ra8_fs_backend_t that is zero-initialised so its read_block field
 * is nullptr. ra8_io_fsfmt_probe validates that the backend pointer itself is not
 * null (it is a stack variable and therefore non-null), then dispatches to the
 * registered probes. Each probe calls internal_read_boot0, which finds
 * be->read_block == nullptr and returns false at line 83. exfat_probe reaches its
 * read-failure return at line 177; fat_probe reaches its read-failure return at
 * line 139. The overall probe therefore finds no matching format.
 *
 * @par MC/DC:
 * No compound decisions under test; each branch is a single condition. No
 * additional MC/DC vectors are required.
 *
 * @pre ra8_io_fsfmt_init() succeeds and registers the built-in formats.
 * @pre A zero-initialised ra8_fs_backend_t has read_block == nullptr.
 * @post ra8_io_fsfmt_probe returns k_ra8_err_not_found.
 * @post out remains nullptr.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static void test_null_read_block_fails_probes(void)
{
  TEST_BEGIN("fsfmt null read_block -> probe fails (lines 83/139/177)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());

  /* Zero-initialised: read_block is nullptr; all other fields are nullptr. */
  ra8_fs_backend_t      be  = {};
  const ra8_io_fsfmt_t* out = nullptr;

  /* exfat_probe -> internal_read_boot0 -> line 83 -> false -> line 177.
   * fat_probe   -> internal_read_boot0 -> line 83 -> false -> line 139. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_fsfmt_probe(&be, &out));
  TEST_ASSERT(out == nullptr);
  TEST_END("fsfmt null read_block -> probe fails (lines 83/139/177)");
}

/**
 * @brief fat_probe returns false when block 0 bears the exFAT marker (line 142).
 *
 * @details
 * After ra8_io_fsfmt_init the registry order is [exfat, fat]. The mock backend
 * mock_read_first_fail fails on call 1 (exfat_probe's read), causing exfat_probe
 * to reach its read-failure return at line 177 and return false. On call 2
 * (fat_probe's read) the mock returns a block whose bytes 3..10 spell "EXFAT   ".
 * internal_read_boot0 succeeds; internal_is_exfat returns true; fat_probe then
 * reaches the exFAT-rejection return at line 142. The overall probe returns
 * k_ra8_err_not_found because both built-in probes returned false.
 *
 * @par MC/DC:
 * No compound decisions under test; each branch is a single condition. No
 * additional MC/DC vectors are required.
 *
 * @pre ra8_io_fsfmt_init() succeeds.
 * @pre s_cov_exfat_blk is populated with the "EXFAT   " name at offset 3.
 * @post ra8_io_fsfmt_probe returns k_ra8_err_not_found.
 * @post s_cov_call equals 2 (exactly one read per probe).
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static void test_fat_probe_rejects_exfat_marker(void)
{
  TEST_BEGIN("fsfmt fat_probe rejects exFAT marker (lines 142/177)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());

  /* Prepare the exFAT block that the mock returns on the second call. */
  (void)memset(s_cov_exfat_blk, 0, sizeof(s_cov_exfat_blk));
  (void)memcpy(&s_cov_exfat_blk[(size_t)k_cov_exfat_name_off],
               "EXFAT   ",
               (size_t)k_cov_exfat_name_len);

  /* Reset the call counter and attach the mock. */
  s_cov_call          = 0U;
  ra8_fs_backend_t be = {};
  be.read_block       = mock_read_first_fail;

  const ra8_io_fsfmt_t* out = nullptr;
  /* Call 1 (exfat_probe): mock -> error -> line 177 -> false.
   * Call 2 (fat_probe):   mock -> exFAT block -> line 142 -> false. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_fsfmt_probe(&be, &out));
  TEST_ASSERT(out == nullptr);
  TEST_ASSERT_EQ(2U, s_cov_call);
  TEST_END("fsfmt fat_probe rejects exFAT marker (lines 142/177)");
}

/**
 * @brief fat_probe returns false when boot signature byte 1 (0xAA) is absent (line 148).
 *
 * @details
 * Block 0 of a small RAM disk is written with 0x55 at offset 510 (sig0 correct) and
 * 0x00 at offset 511 (sig1 wrong; the expected value is 0xAA). The block contains no
 * exFAT filesystem-name field. exfat_probe reads the block, finds no exFAT marker,
 * and returns false. fat_probe reads the block: the exFAT check passes (no marker),
 * the sig0 check passes (0x55 present), but the sig1 check fails (0x00 != 0xAA) and
 * fat_probe returns false at line 148. The overall probe returns k_ra8_err_not_found.
 *
 * @par MC/DC:
 * No compound decisions under test; each branch is a single condition. No
 * additional MC/DC vectors are required.
 *
 * @pre ra8_io_fsfmt_init() succeeds.
 * @pre Block 0 of the fixture has 0x55 at offset 510 and 0x00 at offset 511.
 * @post ra8_io_fsfmt_probe returns k_ra8_err_not_found.
 * @post out remains nullptr.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static void test_fat_probe_missing_sig1(void)
{
  TEST_BEGIN("fsfmt fat_probe missing sig1 (line 148)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());

  ra8_io_blockdev_ram_state_t state = {};
  ra8_io_blockdev_t           bd    = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &state, s_cov_disk, k_cov_small_blocks, false));

  /* Block 0: no exFAT marker; sig0=0x55 at offset 510; sig1=0x00 at offset 511. */
  uint8_t blk[(size_t)k_ra8_io_block_size_bytes] = {};
  blk[(size_t)k_cov_sig0_off]                    = (uint8_t)k_cov_sig0_val;
  /* blk[511] is already 0x00 from zero-init -- the absent 0xAA. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 0U, 1U, blk));

  ra8_fs_backend_t be = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&bd, &be));
  const ra8_io_fsfmt_t* out = nullptr;

  /* exfat_probe: no exFAT marker -> false.
   * fat_probe:   sig0=0x55 (passes line 144), sig1=0x00 != 0xAA -> line 148. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_fsfmt_probe(&be, &out));
  TEST_ASSERT(out == nullptr);
  TEST_END("fsfmt fat_probe missing sig1 (line 148)");
}

int32_t main(void)
{
  test_null_read_block_fails_probes();
  test_fat_probe_rejects_exfat_marker();
  test_fat_probe_missing_sig1();
  (void)fprintf(stderr, "[OK  ] test_ra8_io_fsfmt_cov.c\n");
  return 0;
}
