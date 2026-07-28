/**
 * @file test_ra8_io_stream_blockdev_cov.c
 * @brief Coverage-boost tests for ra8_io_stream_blockdev.c.
 *
 * @details
 * Targets the eight source lines left uncovered after test_ra8_io_stream.c:
 *
 *   73  -- bdsink_commit_sector: error return when ra8_io_blockdev_write fails.
 *   187 -- bdsink_write error path: `out_written != nullptr` branch (true arm).
 *   188 -- bdsink_write error path: `*out_written = done` assignment.
 *   189 -- bdsink_write error path: closing brace of the out_written block.
 *   190 -- bdsink_write error path: `return e` statement.
 *   194 -- bdsink_write success path: `*out_written = len` assignment.
 *   195 -- bdsink_write success path: closing brace of the out_written block.
 *   227 -- bdsink_flush: early return when fill == 0 (nothing pending).
 *
 * Fault injection uses a minimal mock block-device backend assembled from the
 * internal vtable (ra8_io_blockdev_internal.h). The mock write callback returns
 * k_ra8_err_hw_error once the write-call index reaches the configured threshold,
 * exercising the full error-propagation path through bdsink_commit_sector and
 * bdsink_write without real hardware.
 *
 * This file is auto-discovered by the tests/CMakeLists.txt GLOB so no manual
 * list entry is required. gcovr merges its .gcda counters with sibling tests,
 * so newly executed lines accumulate toward the module coverage total.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_internal.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_blockdev.h"
#include "unity_minimal.h"

/* =========================================================================
 * Fixture constants
 * =========================================================================
 */

/**
 * @enum cov_bd_const_t
 * @brief Symbolic constants for the blockdev-stream coverage fixture.
 *
 * @details
 * Collects every numeric value used across this file so the magic-number
 * gate stays silent for first-party readers (tests are exempt from the gate
 * but named values still aid comprehension and maintenance).
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_bd_ram_blocks   = 4U,   /**< Block count for the RAM-backed fixture disk.  */
  k_cov_bd_stream_bytes = 600U, /**< 512-byte first sector + 88-byte tail payload. */
  k_cov_bd_one_sector   = 512U, /**< Exactly one logical block (write-fault test). */
  k_cov_bd_start_lba    = 0U,   /**< Starting logical block address for all tests. */
  k_cov_bd_fail_on_zero = 0U,   /**< writes_until_fail=0 means fail immediately.   */
} cov_bd_const_t;

/* =========================================================================
 * Fault-injecting mock block-device backend
 * =========================================================================
 */

/**
 * @struct mock_fault_state_t
 * @brief State for a fault-injecting block-device mock backend.
 *
 * @details
 * The mock write callback counts calls and returns k_ra8_err_hw_error for
 * every write whose zero-based index is >= `writes_until_fail`. Setting
 * `writes_until_fail = 0` rejects the very first write call.
 *
 * @invariant `write_count` grows monotonically while writes succeed.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t writes_until_fail; /**< First write index that returns an error. */
  uint32_t write_count;       /**< Number of write calls received so far.   */
} mock_fault_state_t;

/**
 * @brief Mock read -- always succeeds and zero-fills the output buffer.
 *
 * @param[in]  ctx   Unused (mock carries no stored blocks).
 * @param[in]  lba   Logical block address (ignored).
 * @param[in]  count Number of blocks to read (determines bytes zeroed).
 * @param[out] buf   Zeroed for `count * 512` bytes.
 *
 * @return ra8_err_t Always k_ra8_ok.
 * @retval k_ra8_ok Read completed successfully.
 *
 * @pre buf is writable for count * 512 bytes.
 * @pre ctx is not inspected.
 * @post buf[0..count*512) is zero-filled.
 * @post No mock state is mutated.
 *
 * @note Not thread-safe (single-threaded test context).
 * @since 0.1.0
 */
static ra8_err_t mock_fault_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)memset(buf, 0, (size_t)count * (size_t)k_ra8_io_block_size_bytes);
  return k_ra8_ok;
}

/**
 * @brief Mock write -- succeeds for the first N calls, then returns hw-error.
 *
 * @details
 * Increments `write_count` on each successful call. Once `write_count >=
 * writes_until_fail` the call returns k_ra8_err_hw_error without modifying
 * the counter, causing the error to repeat on every subsequent call.
 *
 * @param[in,out] ctx   Pointer to a ::mock_fault_state_t.
 * @param[in]     lba   Logical block address (ignored by the mock).
 * @param[in]     count Number of blocks (ignored by the mock).
 * @param[in]     buf   Source data (ignored by the mock).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Call index is within the success budget.
 * @retval k_ra8_err_hw_error Call index reached the configured fail threshold.
 *
 * @pre ctx points to a populated ::mock_fault_state_t.
 * @pre lba, count, and buf are not inspected.
 * @post On success write_count is incremented by one.
 * @post On failure write_count is unchanged and k_ra8_err_hw_error is returned.
 *
 * @note Not thread-safe (single-threaded test context).
 * @since 0.1.0
 */
static ra8_err_t mock_fault_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)lba;
  (void)count;
  (void)buf;
  mock_fault_state_t* st = (mock_fault_state_t*)ctx;
  if (st->write_count >= st->writes_until_fail) {
    return k_ra8_err_hw_error;
  }
  st->write_count++;
  return k_ra8_ok;
}

/**
 * @brief Mock get_caps -- reports a writable 16-block RAM-like device.
 *
 * @param[in]  ctx Unused (no per-device state in the mock).
 * @param[out] out Populated with nominal capability values.
 *
 * @return ra8_err_t Always k_ra8_ok.
 * @retval k_ra8_ok Capabilities populated successfully.
 *
 * @pre out is writable.
 * @pre ctx is not inspected.
 * @post out->logical_block_bytes equals k_ra8_io_block_size_bytes.
 * @post out->read_only is false.
 *
 * @note Not thread-safe (single-threaded test context).
 * @since 0.1.0
 */
static ra8_err_t mock_fault_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  (void)ctx;
  out->block_count             = 16U;
  out->erase_unit_blocks       = 1U;
  out->program_size_bytes      = (uint32_t)k_ra8_io_block_size_bytes;
  out->logical_block_bytes     = (uint16_t)k_ra8_io_block_size_bytes;
  out->erase_value             = (uint8_t)k_ra8_io_erase_value_zero;
  out->must_erase_before_write = false;
  out->read_only               = false;
  return k_ra8_ok;
}

/** @brief Vtable for the fault-injecting mock block-device backend. */
static const ra8_io_blockdev_iface_t k_mock_fault_iface = {
  .read     = mock_fault_read,
  .write    = mock_fault_write,
  .erase    = nullptr,
  .get_caps = mock_fault_get_caps,
  .sync     = nullptr,
};

/**
 * @brief Bind the fault-injecting mock backend into a caller-owned handle.
 *
 * @details
 * Populates `bd->iface` and `bd->ctx` directly instead of going through a
 * backend-specific init helper. This is permissible from test code that
 * includes ra8_io_blockdev_internal.h for MC/DC vector access to error paths.
 *
 * @param[out] bd                Zero-initialised block-device handle to populate.
 * @param[out] st                Zero-initialised mock state to populate.
 * @param[in]  writes_until_fail Number of write calls to allow before failing.
 *
 * @pre bd and st are zero-initialised on entry.
 * @pre writes_until_fail = 0 causes the very first write to fail.
 * @post bd->iface points to k_mock_fault_iface.
 * @post bd->ctx points to st.
 *
 * @note Not thread-safe (single-threaded test context).
 * @since 0.1.0
 */
static void
fixture_fault_init(ra8_io_blockdev_t* bd, mock_fault_state_t* st, uint32_t writes_until_fail)
{
  st->writes_until_fail = writes_until_fail;
  st->write_count       = 0U;
  bd->iface             = &k_mock_fault_iface;
  bd->ctx               = st;
}

/* =========================================================================
 * Test functions
 * =========================================================================
 */

/**
 * @brief bdsink_flush returns k_ra8_ok immediately when fill == 0 (line 227).
 *
 * @details
 * Binds a block-device sink to a small RAM disk and calls ra8_io_stream_flush
 * without first writing any bytes. bdsink_flush reads fill == 0 and executes
 * the early return at line 227 without performing any device I/O.
 *
 * @par MC/DC:
 * Decision: `st->fill == 0U` in bdsink_flush (single condition)
 * - Vector 1 (this test): fill = 0 -> true -> k_ra8_ok, line 227 executed.
 * - Vector 2 (test_blockdev_sink in test_ra8_io_stream.c): fill = 88 -> false
 *   -> zero-pad and commit trailing sector.
 * Minimal MC/DC: N+1 = 2 vectors for N = 1 condition.
 *
 * @since 0.1.0
 */
static void test_bdsink_flush_no_pending_bytes(void)
{
  TEST_BEGIN("bdsink_flush fill==0 early return (line 227)");
  static uint8_t s_disk[(size_t)k_cov_bd_ram_blocks * (size_t)k_ra8_io_block_size_bytes];
  ra8_io_blockdev_ram_state_t bstate = {};
  ra8_io_blockdev_t           bd     = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &bstate, s_disk, k_cov_bd_ram_blocks, false));
  ra8_io_stream_blockdev_state_t sstate = {};
  ra8_io_stream_t                s      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_blockdev_init(&s, &sstate, &bd, k_cov_bd_start_lba));
  /* flush with nothing written: fill == 0 hits line 227 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_flush(&s));
  TEST_END("bdsink_flush fill==0 early return (line 227)");
}

/**
 * @brief bdsink_write success path assigns *out_written = len (lines 193-195).
 *
 * @details
 * Streams 600 bytes through a RAM-backed block-device sink using a non-NULL
 * out_written pointer. On the successful loop exit bdsink_write executes
 * `*out_written = len` (line 194) before returning k_ra8_ok. The test asserts
 * both the return code and the written count.
 *
 * @par MC/DC:
 * Decision: `out_written != nullptr` at success exit of bdsink_write (line 193)
 * - Vector 1 (this test): &written -> true -> line 194 (*out_written = len).
 * - Vector 2 (test_blockdev_sink in test_ra8_io_stream.c): nullptr -> false ->
 *   assignment skipped.
 * Minimal MC/DC: N+1 = 2 vectors for N = 1 condition.
 *
 * @since 0.1.0
 */
static void test_bdsink_write_out_written_nonnull_success(void)
{
  TEST_BEGIN("bdsink_write success *out_written = len (lines 193-195)");
  static uint8_t s_disk[(size_t)k_cov_bd_ram_blocks * (size_t)k_ra8_io_block_size_bytes];
  ra8_io_blockdev_ram_state_t bstate = {};
  ra8_io_blockdev_t           bd     = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &bstate, s_disk, k_cov_bd_ram_blocks, false));
  ra8_io_stream_blockdev_state_t sstate = {};
  ra8_io_stream_t                s      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_blockdev_init(&s, &sstate, &bd, k_cov_bd_start_lba));
  uint8_t  payload[(size_t)k_cov_bd_stream_bytes] = {};
  uint32_t written                                = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_write(&s, payload, k_cov_bd_stream_bytes, &written));
  TEST_ASSERT_EQ(k_cov_bd_stream_bytes, written);
  TEST_END("bdsink_write success *out_written = len (lines 193-195)");
}

/**
 * @brief bdsink_write error path sets *out_written and returns error (lines 73, 187-190).
 *
 * @details
 * Binds a fault-injecting mock with writes_until_fail = 0 so the first device
 * write fails. Streaming exactly 512 bytes causes bdsink_fill_chunk to fill the
 * sector completely (done = 512, fill = 512), then bdsink_commit_if_full calls
 * bdsink_commit_sector, which calls ra8_io_blockdev_write. The mock returns
 * k_ra8_err_hw_error; bdsink_commit_sector propagates it at line 73. Back in
 * bdsink_write, e != k_ra8_ok (line 186 true), out_written != nullptr (line 187
 * true), *out_written = done = 512 (line 188), return e (line 190).
 *
 * @par MC/DC:
 * Decision: `e != k_ra8_ok` in bdsink_write loop body (line 186, single condition)
 * - Vector 1 (this test): e = k_ra8_err_hw_error -> true -> error return path.
 * - Vector 2 (test_blockdev_sink in test_ra8_io_stream.c): e = k_ra8_ok -> false
 *   -> loop continues to next iteration.
 * Decision: `out_written != nullptr` in error path (line 187, single condition)
 * - Vector 1 (this test): &written -> true -> *out_written = done (line 188).
 * - Vector 2 (test_bdsink_write_error_propagates_no_out_written): nullptr -> false.
 * Both decisions have minimal MC/DC coverage: N+1 = 2 vectors for N = 1 condition.
 *
 * @since 0.1.0
 */
static void test_bdsink_write_error_propagates_with_out_written(void)
{
  TEST_BEGIN("bdsink_write error path with out_written (lines 73, 187-190)");
  ra8_io_blockdev_t  bd  = {};
  mock_fault_state_t mst = {};
  fixture_fault_init(&bd, &mst, k_cov_bd_fail_on_zero);
  ra8_io_stream_blockdev_state_t sstate = {};
  ra8_io_stream_t                s      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_blockdev_init(&s, &sstate, &bd, k_cov_bd_start_lba));
  uint8_t         payload[(size_t)k_cov_bd_one_sector] = {};
  uint32_t        written                              = 0U;
  const ra8_err_t e = ra8_io_stream_write(&s, payload, k_cov_bd_one_sector, &written);
  TEST_ASSERT(e != k_ra8_ok);
  TEST_ASSERT_EQ(k_cov_bd_one_sector, written);
  TEST_END("bdsink_write error path with out_written (lines 73, 187-190)");
}

/**
 * @brief bdsink_write error path skips *out_written when it is nullptr (line 190).
 *
 * @details
 * Same fault-injecting setup as test_bdsink_write_error_propagates_with_out_written
 * but passes nullptr for out_written. Line 187 (out_written != nullptr) evaluates
 * false, lines 188-189 are skipped, and line 190 (return e) executes directly.
 * This test supplies the second MC/DC vector for the line-187 decision.
 *
 * @par MC/DC:
 * Decision: `out_written != nullptr` in error path (line 187, single condition)
 * - Vector 1 (test_bdsink_write_error_propagates_with_out_written): true.
 * - Vector 2 (this test): nullptr -> false -> skip *out_written assignment.
 * Together they prove `out_written` independently influences the outcome.
 *
 * @since 0.1.0
 */
static void test_bdsink_write_error_propagates_no_out_written(void)
{
  TEST_BEGIN("bdsink_write error path nullptr out_written (line 190)");
  ra8_io_blockdev_t  bd  = {};
  mock_fault_state_t mst = {};
  fixture_fault_init(&bd, &mst, k_cov_bd_fail_on_zero);
  ra8_io_stream_blockdev_state_t sstate = {};
  ra8_io_stream_t                s      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_blockdev_init(&s, &sstate, &bd, k_cov_bd_start_lba));
  uint8_t         payload[(size_t)k_cov_bd_one_sector] = {};
  const ra8_err_t e = ra8_io_stream_write(&s, payload, k_cov_bd_one_sector, nullptr);
  TEST_ASSERT(e != k_ra8_ok);
  TEST_END("bdsink_write error path nullptr out_written (line 190)");
}

/* =========================================================================
 * Entry point
 * =========================================================================
 */

/**
 * @brief Run all coverage-boost tests and report overall pass.
 *
 * @return int32_t Always 0 on success; exit(1) on the first assertion failure.
 *
 * @pre RA8_OFF_TARGET is active (host build, no hardware access).
 * @pre The ra8_core_hal OBJECT library was built with --coverage instrumentation.
 * @post All eight uncovered lines in ra8_io_stream_blockdev.c have been executed.
 * @post Exit code 0 signals success to the CI coverage runner.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  test_bdsink_flush_no_pending_bytes();
  test_bdsink_write_out_written_nonnull_success();
  test_bdsink_write_error_propagates_with_out_written();
  test_bdsink_write_error_propagates_no_out_written();
  (void)fprintf(stderr, "[OK  ] test_ra8_io_stream_blockdev_cov.c\n");
  return 0;
}
