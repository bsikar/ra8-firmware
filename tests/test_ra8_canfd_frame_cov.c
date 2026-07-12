/**
 * @file tests/test_ra8_canfd_frame_cov.c
 * @brief Coverage-gap tests for libs/ra8_hal/src/ra8_canfd_frame.c
 *
 * @details
 * The original test_ra8_canfd.c suite left the #ifdef RA8_SIMULATOR_MODE
 * function ra8_canfd_test_inject_frame() completely untested.  That
 * function is compiled into every host test build (RA8_SIMULATOR_MODE is
 * always defined in tests/CMakeLists.txt) but no test ever called it,
 * leaving lines 327-350 uncovered.
 *
 * This file exercises every path through that function:
 *
 *  - Bad channel -> k_ra8_err_null_ptr (null-check branch).
 *  - Valid channel, non-null data, data_len within the 64-byte cap ->
 *    full happy path including the DF[] copy loop with data != nullptr.
 *  - Valid channel, non-null data, data_len exceeding the 64-byte cap ->
 *    copy_len clamping arm.
 *  - Valid channel, null data, data_len > 0 ->
 *    inner ternary data==nullptr -> 0U arm.
 *
 * Additionally each inject call is followed by ra8_canfd_receive() to
 * confirm that the register side-effects are consistent with the
 * production receive path (CFDRFSTS[0] RFEMP cleared).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/* -------------------------------------------------------------------------
 * Local constants (tests are exempt from the magic-number gate but named
 * values document intent and keep clang-tidy -readability-magic-numbers
 * quiet on the host build).
 * -------------------------------------------------------------------------
 */

/**
 * @enum inj_channel_t
 * @brief Channel indices used throughout this test file.
 */
typedef enum : uint8_t {
  k_inj_ch0    = 0U, /**< Valid channel 0.                            */
  k_inj_ch1    = 1U, /**< Valid channel 1.                            */
  k_inj_ch_bad = 2U, /**< Out-of-range channel -- no register window. */
} inj_channel_t;

/**
 * @enum inj_word_t
 * @brief Raw register words and lengths used by the inject tests.
 */
typedef enum : uint32_t {
  k_inj_id_ext     = 0x1F0ABCDU,  /**< 29-bit extended ID, IDE bit clear.            */
  k_inj_ptr_dlc15  = 0xF0000000U, /**< DLC=15 in bits [31:28].                       */
  k_inj_fdsts_fd   = 0x00000003U, /**< FDF | BRS bits set.                           */
  k_inj_len_short  = 8U,          /**< data_len <= k_ra8_canfd_data_bytes_max.       */
  k_inj_len_over   = 200U,        /**< data_len > k_ra8_canfd_data_bytes_max: clamp. */
  k_inj_len_null   = 4U,          /**< data_len when data==nullptr.                  */
  k_inj_byte_seed  = 0xA5U,       /**< Seed byte for the data[] pattern.             */
  k_inj_std_id_val = 0x7FFU,      /**< Max 11-bit standard ID (no IDE bit).          */
  k_inj_ptr_dlc8   = 0x80000000U, /**< DLC=8 packed into bits [31:28]: 8 << 28.      */
  k_inj_fdsts_none = 0U,          /**< Classic CAN: no FD flags.                     */
} inj_word_t;

/* -------------------------------------------------------------------------
 * Helper
 * -------------------------------------------------------------------------
 */

/**
 * @brief Fill `buf[0..len-1]` with a deterministic byte sequence.
 *
 * @param[out] buf   Destination buffer.
 * @param[in]  len   Number of bytes to fill.
 * @pre  buf != nullptr.
 * @post buf[i] == (uint8_t)((k_inj_byte_seed + i) & 0xFFU) for all i < len.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void fill_pattern(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)(((uint32_t)k_inj_byte_seed + i) & 0xFFU);
  }
}

/* -------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------
 */

/**
 * @brief Inject with an out-of-range channel; expect k_ra8_err_null_ptr.
 *
 * @details
 * Drives the `if (reg == nullptr) { return k_ra8_err_null_ptr; }` branch
 * (ra8_canfd_frame.c lines 335-336).  ra8_canfd(2) returns nullptr because
 * only channels 0 and 1 have a simulator MMIO window.
 *
 * @par MC/DC:
 * Decision: `reg == nullptr` (single condition; no compound decision).
 * - Vector 1: channel=0 -> reg != nullptr -> false (covered by other tests).
 * - Vector 2: channel=2 -> reg == nullptr -> true (this test).
 * Single-condition decisions satisfy MC/DC trivially.
 *
 * @pre ra8_sim_mmap_reset() has been called.
 * @post No simulator state is modified (early return before any write).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_inject_bad_channel(void)
{
  TEST_BEGIN("inject_frame: bad channel returns null_ptr");
  ra8_sim_mmap_reset();

  const ra8_err_t err = ra8_canfd_test_inject_frame((uint8_t)k_inj_ch_bad,
                                                    (uint32_t)k_inj_id_ext,
                                                    (uint32_t)k_inj_ptr_dlc15,
                                                    (uint32_t)k_inj_fdsts_fd,
                                                    nullptr,
                                                    (uint32_t)k_inj_len_short);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, err);

  TEST_END("inject_frame: bad channel returns null_ptr");
}

/**
 * @brief Inject a frame with a valid channel and non-null data; expect k_ra8_ok.
 *
 * @details
 * Drives the full happy path of ra8_canfd_test_inject_frame():
 *
 *  - Register writes to CFDRF[0].ID / PTR / FDSTS (lines 338-340).
 *  - The copy_len assignment with data_len <= cap (lines 341,343).
 *  - The DF[] copy loop with data != nullptr (lines 344-346).
 *  - CFDRFSTS[0] RFEMP cleared (line 348).
 *  - k_ra8_ok returned (line 349).
 *
 * The test then calls ra8_canfd_receive() to confirm the register state
 * matches what the production path expects (RFEMP=0, ID correct).
 *
 * @par MC/DC:
 * Decision 1: `data_len > k_ra8_canfd_data_bytes_max` (single condition).
 * - This test exercises the FALSE arm (data_len=8 <= 64).
 * Decision 2: `data != nullptr` (single condition).
 * - This test exercises the TRUE arm (data points to a real array).
 * Single-condition decisions satisfy MC/DC trivially.
 *
 * @pre ra8_sim_mmap_reset() has been called.
 * @post CFDRF[0].ID == k_inj_id_ext, PTR == k_inj_ptr_dlc15.
 * @post CFDRFSTS[0] RFEMP bit is clear after inject.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_inject_happy_path(void)
{
  TEST_BEGIN("inject_frame: valid channel + data writes registers");
  ra8_sim_mmap_reset();

  uint8_t data[k_ra8_canfd_data_bytes_max];
  fill_pattern(data, (uint32_t)k_inj_len_short);

  /* Arm RFEMP so we can verify it gets cleared. */
  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_inj_ch0);
  reg->CFDRFSTS[0]        = (uint32_t)k_ra8_rfsts_bit_empty;

  const ra8_err_t err = ra8_canfd_test_inject_frame((uint8_t)k_inj_ch0,
                                                    (uint32_t)k_inj_id_ext,
                                                    (uint32_t)k_inj_ptr_dlc15,
                                                    (uint32_t)k_inj_fdsts_fd,
                                                    data,
                                                    (uint32_t)k_inj_len_short);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  /* Verify register words were written. */
  TEST_ASSERT_EQ(k_inj_id_ext, reg->CFDRF[0].ID);
  TEST_ASSERT_EQ(k_inj_ptr_dlc15, reg->CFDRF[0].PTR);
  TEST_ASSERT_EQ(k_inj_fdsts_fd, reg->CFDRF[0].FDSTS);

  /* Verify RFEMP was cleared so ra8_canfd_receive sees a frame. */
  TEST_ASSERT((reg->CFDRFSTS[0] & (uint32_t)k_ra8_rfsts_bit_empty) == 0U);

  /* Verify payload bytes landed in DF[]. */
  TEST_ASSERT_EQ(data[0], reg->CFDRF[0].DF[0]);
  TEST_ASSERT_EQ(data[7], reg->CFDRF[0].DF[7]);

  /* Confirm the production receive path can consume the injected frame. */
  ra8_canfd_frame_t out = {};
  const ra8_err_t   rx  = ra8_canfd_receive((uint8_t)k_inj_ch0, &out);
  TEST_ASSERT_EQ(k_ra8_ok, rx);

  TEST_END("inject_frame: valid channel + data writes registers");
}

/**
 * @brief Inject with data_len exceeding the 64-byte cap; verify clamping.
 *
 * @details
 * Drives the TRUE arm of:
 *   `(data_len > k_ra8_canfd_data_bytes_max) ? k_ra8_canfd_data_bytes_max : data_len`
 * (ra8_canfd_frame.c lines 341-343).  data_len=200 forces copy_len to be
 * clamped to 64 so the loop stays within the DF[64] array bounds.
 *
 * The test verifies that the first and last bytes of the 64-byte window
 * were written with the expected pattern values and that the function
 * returns k_ra8_ok.
 *
 * @par MC/DC:
 * Decision: `data_len > k_ra8_canfd_data_bytes_max` (single condition).
 * - This test exercises the TRUE arm (data_len=200 > 64).
 * - The FALSE arm is exercised by test_inject_happy_path (data_len=8).
 * Single-condition decisions satisfy MC/DC trivially.
 *
 * @pre ra8_sim_mmap_reset() has been called.
 * @post CFDRF[0].DF[0] and DF[63] hold the pattern bytes.
 * @post ra8_canfd_test_inject_frame() returns k_ra8_ok.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_inject_clamp_data_len(void)
{
  TEST_BEGIN("inject_frame: oversized data_len clamps to 64 bytes");
  ra8_sim_mmap_reset();

  /* Provide exactly max bytes of pattern data. */
  uint8_t data[k_ra8_canfd_data_bytes_max];
  fill_pattern(data, (uint32_t)k_ra8_canfd_data_bytes_max);

  const ra8_err_t err = ra8_canfd_test_inject_frame((uint8_t)k_inj_ch0,
                                                    (uint32_t)k_inj_id_ext,
                                                    (uint32_t)k_inj_ptr_dlc15,
                                                    (uint32_t)k_inj_fdsts_fd,
                                                    data,
                                                    (uint32_t)k_inj_len_over);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_inj_ch0);

  /* First and last bytes of the clamped window must match the pattern. */
  TEST_ASSERT_EQ(data[0], reg->CFDRF[0].DF[0]);
  TEST_ASSERT_EQ(data[k_ra8_canfd_data_bytes_max - 1U],
                 reg->CFDRF[0].DF[k_ra8_canfd_data_bytes_max - 1U]);

  TEST_END("inject_frame: oversized data_len clamps to 64 bytes");
}

/**
 * @brief Inject with data==nullptr; verify DF[] is filled with zeros.
 *
 * @details
 * Drives the FALSE arm of:
 *   `(data != nullptr) ? data[b] : 0U`
 * inside the DF[] copy loop (ra8_canfd_frame.c line 345).  When the
 * caller passes a null data pointer but a non-zero data_len the function
 * writes zero into every DF[] slot instead of dereferencing the pointer.
 *
 * @par MC/DC:
 * Decision: `data != nullptr` (single condition).
 * - This test exercises the FALSE arm (data == nullptr -> writes 0U).
 * - The TRUE arm is exercised by test_inject_happy_path.
 * Single-condition decisions satisfy MC/DC trivially.
 *
 * @pre ra8_sim_mmap_reset() has been called.
 * @post CFDRF[0].DF[0..k_inj_len_null-1] are zero.
 * @post ra8_canfd_test_inject_frame() returns k_ra8_ok.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_inject_null_data_zeroes_df(void)
{
  TEST_BEGIN("inject_frame: null data fills DF[] with zeros");
  ra8_sim_mmap_reset();

  /* Pre-fill DF[] with a non-zero sentinel so the zero-write is detectable. */
  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_inj_ch1);
  for (uint32_t i = 0U; i < (uint32_t)k_inj_len_null; i++) {
    reg->CFDRF[0].DF[i] = (uint8_t)k_inj_byte_seed;
  }

  const ra8_err_t err = ra8_canfd_test_inject_frame((uint8_t)k_inj_ch1,
                                                    (uint32_t)k_inj_id_ext,
                                                    (uint32_t)k_inj_ptr_dlc15,
                                                    (uint32_t)k_inj_fdsts_fd,
                                                    nullptr,
                                                    (uint32_t)k_inj_len_null);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  /* All DF[] slots covered by the loop must be zero. */
  for (uint32_t i = 0U; i < (uint32_t)k_inj_len_null; i++) {
    TEST_ASSERT_EQ(0, reg->CFDRF[0].DF[i]);
  }

  TEST_END("inject_frame: null data fills DF[] with zeros");
}

/**
 * @brief Inject then receive on channel 1 to confirm round-trip decode.
 *
 * @details
 * Uses ra8_canfd_test_inject_frame() to pre-load a standard 11-bit frame
 * and then calls ra8_canfd_receive() to confirm:
 *
 *  - The ID is decoded correctly (no IDE bit -> standard frame).
 *  - The DLC is extracted from the PTR word.
 *  - RFEMP is cleared by inject so receive returns k_ra8_ok.
 *  - CFDRFPCTR[0] is written with 0xFF to advance the pointer.
 *
 * This indirectly validates that the CFDRFSTS manipulation (line 348)
 * is consistent with what ra8_canfd_receive() expects.
 *
 * @par MC/DC:
 * (No compound decisions exercised here -- single-condition checks only.)
 *
 * @pre ra8_sim_mmap_reset() has been called.
 * @post out.id == k_inj_std_id_val, out.is_extended == 0, out.dlc == 8.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_inject_then_receive_roundtrip(void)
{
  TEST_BEGIN("inject_frame: round-trip inject -> receive on channel 1");
  ra8_sim_mmap_reset();

  /* Build a standard-ID PTR word for DLC=8 (uses file-scope inj_word_t). */
  uint8_t data[8U];
  for (uint8_t i = 0U; i < 8U; i++) {
    data[i] = (uint8_t)(i + 1U);
  }

  /* Arm RFEMP to confirm inject clears it. */
  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_inj_ch1);
  reg->CFDRFSTS[0]        = (uint32_t)k_ra8_rfsts_bit_empty;

  const ra8_err_t inj =
    ra8_canfd_test_inject_frame((uint8_t)k_inj_ch1,
                                (uint32_t)k_inj_std_id_val, /* no IDE bit -> standard ID */
                                (uint32_t)k_inj_ptr_dlc8,
                                (uint32_t)k_inj_fdsts_none,
                                data,
                                8U);
  TEST_ASSERT_EQ(k_ra8_ok, inj);

  /* RFEMP must be clear now. */
  TEST_ASSERT((reg->CFDRFSTS[0] & (uint32_t)k_ra8_rfsts_bit_empty) == 0U);

  ra8_canfd_frame_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_receive((uint8_t)k_inj_ch1, &out));
  TEST_ASSERT_EQ(k_inj_std_id_val, out.id);
  TEST_ASSERT_EQ(0, out.is_extended);
  TEST_ASSERT_EQ(8, out.dlc);
  TEST_ASSERT_EQ(0, out.is_fd);
  TEST_ASSERT_EQ(0, out.is_brs);
  TEST_ASSERT_EQ(1, out.data[0]);
  TEST_ASSERT_EQ(8, out.data[7]);

  TEST_END("inject_frame: round-trip inject -> receive on channel 1");
}

/* -------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------
 */

/**
 * @brief Test executable entry point.
 * @return 0 on success; calls exit(1) on first assertion failure.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_inject_bad_channel();
  test_inject_happy_path();
  test_inject_clamp_data_len();
  test_inject_null_data_zeroes_df();
  test_inject_then_receive_roundtrip();
  (void)fprintf(stderr, "[OK ] test_ra8_canfd_frame_cov.c\n");
  return 0;
}
