/**
 * @file test_ra8_flash_irq_cov.c
 * @brief Coverage booster for ra8_flash_irq.c.
 *
 * @details
 * Drives the branches in ra8_flash_irq.c left uncovered by the primary test
 * suite (test_ra8_flash.c + test_ra8_flash_edge_cases.c):
 *
 *   - MASTAT.MREAE delivery in ra8_flash_dispatch_isr (lines 301-303).
 *   - MASTAT.CMDLK delivery in ra8_flash_dispatch_isr (lines 305-307).
 *   - internal_validate_range: aligned address below code-MRAM (line 418).
 *   - internal_validate_range: success return (line 426).
 *   - internal_world_for_addr body (lines 377, 383-384).
 *   - ra8_flash_erase: happy-path loop and success return (lines 438,
 *     442-449, 450).
 *   - ra8_flash_erase: loop early-return on hw error (line 446).
 *   - ra8_flash_write: happy-path loop and success return (lines 462,
 *     466-472, 475-476).
 *   - ra8_flash_write: loop early-return on hw error (line 473).
 *   - ra8_flash_blank_check: in_extra RHS evaluation (line 504).
 *
 * Lines 169 and 171 (the default: arm of the set_irq_enable switch) carry
 * GCOVR_EXCL_LINE markers in the source because the src >=
 * k_ra8_flash_irq_count guard at the top of that function rejects every value
 * that could reach the arm, making it genuinely unreachable from any valid call
 * site.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_flash.h"
#include "ra8_flash_regs.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/**
 * @enum flash_irq_cov_fixture_t
 * @brief The physical quantities the configuration declares, plus the
 * recognizable values moved through the code under test.
 */
typedef enum : uint8_t {
  k_flash_core_clock_mhz   = 200U,  /**< Core clock the flash configuration declares, in MHz. */
  k_flash_periph_clock_mhz = 100U,  /**< Its peripheral clock, half the core clock.           */
  k_flash_erased_byte      = 0xFFU, /**< The erased state of MRAM: all ones.                  */
} flash_irq_cov_fixture_t;

/* ---------------------------------------------------------------------------
 * Address constants
 * ------------------------------------------------------------------------ */

/**
 * @enum ra8_flash_irq_cov_addr_t
 * @brief Per-test address constants for ra8_flash_irq_cov tests.
 *
 * @details
 * k_test_below_code_aligned is a 32-byte-aligned address that falls below
 * the code-MRAM window (0x02000000).  It is used to drive the
 * address-below-code-start early-exit in internal_validate_range without
 * triggering the alignment check that sits in front of it.
 *
 * @code
 * ra8_flash_erase((uintptr_t)k_test_below_code_aligned, 1U);
 * @endcode
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_test_below_code_aligned = 0x01FFE000UL, /**< 32-byte aligned, below 0x02000000. */
} ra8_flash_irq_cov_addr_t;

/* ---------------------------------------------------------------------------
 * Shared callback state
 * ------------------------------------------------------------------------ */

/** @var s_cov_cb_count
 * @brief Invocation counter for the coverage-test callback.
 * @details Incremented once per ra8_flash_dispatch_isr callback delivery.
 * @note Not thread-safe; tests are single-threaded.
 * @warning Reset to 0 at the start of each test that uses it.
 * @since 0.1.0 */
static uint32_t s_cov_cb_count = 0U;

/** @var s_cov_cb_last
 * @brief IRQ source seen on the most recent callback invocation.
 * @details Updated by cov_callback on each call.
 * @note Not thread-safe; tests are single-threaded.
 * @since 0.1.0 */
static ra8_flash_irq_src_t s_cov_cb_last = (ra8_flash_irq_src_t)0U;

/**
 * @brief Minimal flash ISR callback used by coverage tests.
 *
 * @details
 * Records the invocation count and the most-recent IRQ source so test
 * assertions can verify that the correct dispatcher branch fired.
 *
 * @param[in] ev Event descriptor from ra8_flash_dispatch_isr.
 *
 * @pre Called from ra8_flash_dispatch_isr with a valid event pointer.
 * @post s_cov_cb_count incremented; s_cov_cb_last updated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void cov_callback(const ra8_flash_isr_event_t* ev)
{
  s_cov_cb_count++;
  s_cov_cb_last = ev->src;
}

/* ---------------------------------------------------------------------------
 * Config helper
 * ------------------------------------------------------------------------ */

/**
 * @brief Return a valid ra8_flash_cfg_t for use in tests requiring init.
 *
 * @return Populated configuration with sensible clock and ECC settings.
 *
 * @pre None.
 * @post Returned struct is valid for ra8_flash_init().
 *
 * @note Thread-safe (returns by value).
 * @since 0.1.0
 */
static ra8_flash_cfg_t cfg_make(void)
{
  return (ra8_flash_cfg_t){
    .mrcfreq_mhz        = k_flash_core_clock_mhz,
    .mrefreq_mhz        = k_flash_periph_clock_mhz,
    .prefetch_en        = true,
    .ecc_encoder_enable = true,
    .ecc_decoder_enable = true,
  };
}

/* ---------------------------------------------------------------------------
 * Test: MASTAT.MREAE delivery (lines 301-303)
 * ------------------------------------------------------------------------ */

/**
 * @brief Verify ra8_flash_dispatch_isr delivers the extra-MRAM access-error
 * event.
 *
 * @details
 * Sets MASTAT.MREAE to 1 and registers a callback, then calls
 * ra8_flash_dispatch_isr.  Asserts that exactly one callback was delivered
 * with src == k_ra8_flash_irq_extra_err, covering lines 301-303 in
 * ra8_flash_irq.c.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the if at line 300 is a single
 * condition: `(mastat & k_ra8_mastat_mask_mreae) != 0U`)
 *
 * @pre ra8_fake_mmap_reset() called.
 * @post s_cov_cb_count == 1, s_cov_cb_last == k_ra8_flash_irq_extra_err.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_dispatch_mastat_mreae(void)
{
  TEST_BEGIN("flash dispatch: MASTAT.MREAE fires extra_err callback");
  ra8_fake_mmap_reset();
  s_cov_cb_count = 0U;
  s_cov_cb_last  = (ra8_flash_irq_src_t)0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_callback_set(cov_callback, nullptr));
  /* HUM Ch 59 "MASTAT : Extra MRAM Access Status Register" p 3562 */
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mastat) = (uint8_t)k_ra8_mastat_mask_mreae;
  const uint32_t n                                = ra8_flash_dispatch_isr();
  TEST_ASSERT_EQ(1, n);
  TEST_ASSERT_EQ(1, s_cov_cb_count);
  TEST_ASSERT_EQ(k_ra8_flash_irq_extra_err, s_cov_cb_last);
  TEST_END("flash dispatch: MASTAT.MREAE fires extra_err callback");
}

/* ---------------------------------------------------------------------------
 * Test: MASTAT.CMDLK delivery (lines 305-307)
 * ------------------------------------------------------------------------ */

/**
 * @brief Verify ra8_flash_dispatch_isr delivers the command-lock event.
 *
 * @details
 * Sets MASTAT.CMDLK to 1 and registers a callback, then calls
 * ra8_flash_dispatch_isr.  Asserts that exactly one callback was delivered
 * with src == k_ra8_flash_irq_extra_cmdlk, covering lines 305-307.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the if at line 304 is a single
 * condition: `(mastat & k_ra8_mastat_mask_cmdlk) != 0U`)
 *
 * @pre ra8_fake_mmap_reset() called.
 * @post s_cov_cb_count == 1, s_cov_cb_last == k_ra8_flash_irq_extra_cmdlk.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_dispatch_mastat_cmdlk(void)
{
  TEST_BEGIN("flash dispatch: MASTAT.CMDLK fires extra_cmdlk callback");
  ra8_fake_mmap_reset();
  s_cov_cb_count = 0U;
  s_cov_cb_last  = (ra8_flash_irq_src_t)0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_callback_set(cov_callback, nullptr));
  /* HUM Ch 59 "MASTAT : Extra MRAM Access Status Register" p 3562 */
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mastat) = (uint8_t)k_ra8_mastat_mask_cmdlk;
  const uint32_t n                                = ra8_flash_dispatch_isr();
  TEST_ASSERT_EQ(1, n);
  TEST_ASSERT_EQ(1, s_cov_cb_count);
  TEST_ASSERT_EQ(k_ra8_flash_irq_extra_cmdlk, s_cov_cb_last);
  TEST_END("flash dispatch: MASTAT.CMDLK fires extra_cmdlk callback");
}

/* ---------------------------------------------------------------------------
 * Test: internal_validate_range address below code-MRAM (line 418)
 * ------------------------------------------------------------------------ */

/**
 * @brief Verify ra8_flash_erase rejects an aligned address below code MRAM.
 *
 * @details
 * Uses an address that satisfies the 32-byte alignment check (so that
 * check does not mask line 418) but falls below k_ra8_flash_code_start,
 * covering the early-exit at line 418 of internal_validate_range.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the if at line 417 is a single
 * condition: `address < k_ra8_flash_code_start`)
 *
 * @pre ra8_flash_init() succeeded.
 * @post ra8_flash_erase returns k_ra8_err_invalid_arg.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_validate_range_below_code_start_aligned(void)
{
  TEST_BEGIN("flash erase: aligned addr below code-MRAM rejected (line 418)");
  ra8_fake_mmap_reset();
  (void)ra8_flash_deinit();
  const ra8_flash_cfg_t cfg = cfg_make();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));

  /* 0x01FFE000 is 32-byte aligned (0x01FFE000 & 0x1F == 0) and below
   * k_ra8_flash_code_start (0x02000000), so the alignment check passes and
   * the address-below-code-start check (line 417-418) fires. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_flash_erase((uintptr_t)k_test_below_code_aligned, 1U));
  TEST_END("flash erase: aligned addr below code-MRAM rejected (line 418)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_flash_erase happy path (lines 377,383-384,426,438,442-450)
 * ------------------------------------------------------------------------ */

/**
 * @brief Verify the ra8_flash_erase happy path completes without error.
 *
 * @details
 * Initializes the driver, pre-stages MRCPS with ABUFEMP set (so that
 * internal_wait_buffer_ready and internal_wait_commit_done both return
 * k_ra8_ok on the first spin), then calls ra8_flash_erase with one block at
 * k_ra8_flash_code_start.  The call must pass internal_validate_range
 * (covering the success return at line 426), call internal_world_for_addr
 * (lines 377, 383-384), and execute the erase loop (lines 438, 442-449)
 * before returning k_ra8_ok (line 450).
 *
 * @par MC/DC:
 * (no compound decisions in this test -- every if in the exercised path is
 * a single condition)
 *
 * @pre ra8_flash_init() succeeded.
 * @pre MRCPS.ABUFEMP = 1 (staged before call).
 * @post ra8_flash_erase returns k_ra8_ok.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_erase_happy_path(void)
{
  TEST_BEGIN("flash erase: happy path covers loop and success return");
  ra8_fake_mmap_reset();
  (void)ra8_flash_deinit();
  const ra8_flash_cfg_t cfg = cfg_make();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));

  /* Stage MRCPS so that both spin-loops inside ra8_flash_write_block return
   * immediately.  ABUFEMP=1 satisfies internal_wait_commit_done; PRGBSYC=0
   * and ABUFFULL=0 (both clear in 0x20) satisfy internal_wait_buffer_ready;
   * no error bits are set so the post-write status check also passes. */
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3577 */
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps) = (uint8_t)k_ra8_mrcps_mask_abufemp;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_erase((uintptr_t)k_ra8_flash_code_start, 1U));
  TEST_END("flash erase: happy path covers loop and success return");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_flash_erase loop error return (line 446)
 * ------------------------------------------------------------------------ */

/**
 * @brief Verify ra8_flash_erase propagates an erase_block hw error immediately.
 *
 * @details
 * Pre-stages MRCPS with ABUFEMP (so the spin-loops complete) and PRGERRC
 * (so the post-write status check inside ra8_flash_write_block returns
 * k_ra8_err_hw_error).  The error propagates through ra8_flash_erase_block
 * back to the ra8_flash_erase loop, which returns it at line 446.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the if at line 445 is a single
 * condition: `err != k_ra8_ok`)
 *
 * @pre ra8_flash_init() succeeded.
 * @pre MRCPS = ABUFEMP | PRGERRC.
 * @post ra8_flash_erase returns k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_erase_loop_hw_error(void)
{
  TEST_BEGIN("flash erase: loop propagates erase_block hw error (line 446)");
  ra8_fake_mmap_reset();
  (void)ra8_flash_deinit();
  const ra8_flash_cfg_t cfg = cfg_make();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));

  /* ABUFEMP satisfies both spin-loops; PRGERRC is detected by the post-write
   * error check in ra8_flash_write_block -> k_ra8_err_hw_error propagates up.
   */
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3577 */
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps) =
    (uint8_t)(k_ra8_mrcps_mask_abufemp | k_ra8_mrcps_mask_prgerrc);

  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_flash_erase((uintptr_t)k_ra8_flash_code_start, 1U));
  TEST_END("flash erase: loop propagates erase_block hw error (line 446)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_flash_write happy path (lines 462,466-472,475-476)
 * ------------------------------------------------------------------------ */

/**
 * @brief Verify the ra8_flash_write happy path completes without error.
 *
 * @details
 * Calls ra8_flash_write with a 32-byte buffer aligned to
 * k_ra8_flash_code_start. MRCPS is pre-staged with ABUFEMP so both spin-loops
 * inside ra8_flash_write_block resolve immediately and no error bits are
 * detected. This drives internal_world_for_addr (line 462), the page-loop body
 * (lines 466-472), and the success return (lines 475-476).
 *
 * @par MC/DC:
 * (no compound decisions in this test -- every if in the exercised path is
 * a single condition)
 *
 * @pre ra8_flash_init() succeeded.
 * @pre MRCPS.ABUFEMP = 1.
 * @post ra8_flash_write returns k_ra8_ok.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_write_happy_path(void)
{
  TEST_BEGIN("flash write: happy path covers loop and success return");
  ra8_fake_mmap_reset();
  (void)ra8_flash_deinit();
  const ra8_flash_cfg_t cfg = cfg_make();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));

  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3577 */
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps) = (uint8_t)k_ra8_mrcps_mask_abufemp;

  const uint8_t buf[k_ra8_mram_write_size_bytes] = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_flash_write((uintptr_t)k_ra8_flash_code_start, buf, k_ra8_mram_write_size_bytes));
  TEST_END("flash write: happy path covers loop and success return");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_flash_write loop error return (line 473)
 * ------------------------------------------------------------------------ */

/**
 * @brief Verify ra8_flash_write propagates a write_block hw error immediately.
 *
 * @details
 * Pre-stages MRCPS with ABUFEMP | PRGERRC.  The spin-loops complete
 * normally but the post-write error check in ra8_flash_write_block returns
 * k_ra8_err_hw_error, which the ra8_flash_write loop propagates at line 473.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the if at line 472 is a single
 * condition: `err != k_ra8_ok`)
 *
 * @pre ra8_flash_init() succeeded.
 * @pre MRCPS = ABUFEMP | PRGERRC.
 * @post ra8_flash_write returns k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_write_loop_hw_error(void)
{
  TEST_BEGIN("flash write: loop propagates write_block hw error (line 473)");
  ra8_fake_mmap_reset();
  (void)ra8_flash_deinit();
  const ra8_flash_cfg_t cfg = cfg_make();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));

  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3577 */
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps) =
    (uint8_t)(k_ra8_mrcps_mask_abufemp | k_ra8_mrcps_mask_prgerrc);

  const uint8_t buf[k_ra8_mram_write_size_bytes] = {};
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    ra8_flash_write((uintptr_t)k_ra8_flash_code_start, buf, k_ra8_mram_write_size_bytes));
  TEST_END("flash write: loop propagates write_block hw error (line 473)");
}

/* ---------------------------------------------------------------------------
 * Test: ra8_flash_blank_check in_extra RHS (line 504)
 * ------------------------------------------------------------------------ */

/**
 * @brief Verify ra8_flash_blank_check evaluates the in_extra upper-bound
 *        condition when the address is inside the extra-MRAM window.
 *
 * @details
 * Calls ra8_flash_blank_check with an address inside the extra-MRAM region
 * (k_ra8_flash_extra_start = 0x02E07600).  The logical-AND at lines 503-504
 * short-circuits on the first condition when the address is below
 * k_ra8_flash_extra_start; to reach line 504 the first condition must be
 * true, which happens here because the address equals k_ra8_flash_extra_start.
 * The memory is pre-staged with 0xFF so blank_check reports blank=true.
 *
 * @par MC/DC:
 * (no compound decisions exercised by this test; the mcdc-deactivated
 * comment on lines 502-504 of the source documents why the in_extra && is
 * exempt from MC/DC measurement)
 *
 * @pre Extra-MRAM fake region at k_ra8_flash_extra_start is mmap-backed.
 * @post ra8_flash_blank_check returns k_ra8_ok with out_blank = true.
 *
 * @note No ra8_flash_init required; blank_check has no init guard.
 * @since 0.1.0
 */
static void test_blank_check_extra_mram_path(void)
{
  TEST_BEGIN("flash blank_check: in_extra RHS evaluated for extra-MRAM addr");
  ra8_fake_mmap_reset();

  /* Stage erased-state (0xFF) bytes in the fake-backed extra-MRAM window so
   * blank_check can complete a real read without faulting. */
  volatile uint8_t* p = (volatile uint8_t*)(uintptr_t)k_ra8_flash_extra_start;
  for (uint32_t i = 0U; i < 16U; ++i) {
    p[i] = k_flash_erased_byte;
  }

  bool blank = false;
  /* address >= k_ra8_flash_extra_start forces evaluation of line 503 (true)
   * and therefore also line 504 (the upper-bound check). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_blank_check((uintptr_t)k_ra8_flash_extra_start, 16U, &blank));
  TEST_ASSERT_EQ(1, blank);
  TEST_END("flash blank_check: in_extra RHS evaluated for extra-MRAM addr");
}

/**
 * @brief Discard production log bytes during host error-path tests.
 * @details Redirects the logger away from host-unmapped ITM while leaving the
 * production error paths and their sanitizer instrumentation intact.
 * @param[in] context Unused caller context.
 * @param[in] byte Unused log byte.
 * @pre Installed before any vector that intentionally triggers an error log.
 * @pre The host test runs in one process and does not replace the sink concurrently.
 * @post The supplied byte is discarded without I/O or MMIO access.
 * @post No test fixture or production state is mutated.
 * @note This host-only callback does not suppress sanitizer diagnostics.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_host_log_sink(void* context, uint8_t byte)
{
  (void)context;
  (void)byte;
}

/* ---------------------------------------------------------------------------
 * Test runner
 * ------------------------------------------------------------------------ */

/**
 * @brief Run all ra8_flash_irq_cov coverage tests.
 *
 * @return 0 on success (individual test failures exit via TEST_FAIL_FMT).
 *
 * @pre Host test binary linked against ra8_core_hal with RA8_OFF_TARGET.
 * @post All uncovered branches in ra8_flash_irq.c are exercised.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int main(void)
{
  ra8_log_set_byte_sink(internal_host_log_sink, nullptr);
  test_dispatch_mastat_mreae();
  test_dispatch_mastat_cmdlk();
  test_validate_range_below_code_start_aligned();
  test_erase_happy_path();
  test_erase_loop_hw_error();
  test_write_happy_path();
  test_write_loop_hw_error();
  test_blank_check_extra_mram_path();
  (void)internal_test_output_fd_text(STDERR_FILENO,
                                     "[DONE] test_ra8_flash_irq_cov: all tests passed\n");
  ra8_log_set_byte_sink(nullptr, nullptr);
  return 0;
}
