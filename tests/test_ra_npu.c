/**
 * @file test_ra_npu.c
 * @brief Unit tests for ra_npu.c (Arm Ethos-U55 NPU command/queue foundation)
 *
 * @details
 * Compiled with `-DRA_DEVICE_RA8P1` (see tests/CMakeLists.txt) so the
 * device-gated `ra_npu.c` body is live and `ra_npu_regs.h` is includable. The
 * NPU register window (`0x40140000`) sits inside the host MMIO backing store's
 * peripheral region, so register writes land in RAM and the tests assert the
 * exact QBASE / QSIZE / BASEPn / CMD sequence the Ethos-U55 submission protocol
 * requires -- no silicon, no real inference.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_err.h"
#include "ra_npu.h"
#include "ra_npu_regs.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_npu_test_const_t
 * @brief Fixture sizes and seed values for the NPU driver tests.
 */
typedef enum : uint32_t {
  k_test_npu_cmd_bytes        = 256U, /**< Fake command-stream length.     */
  k_test_npu_arena_bytes      = 64U,  /**< Fake tensor-arena length.       */
  k_test_npu_region_count     = 3U,   /**< Regions programmed by the job.  */
  k_test_npu_too_many_regions = 9U,   /**< One past k_ra_npu_region_count. */
  k_test_npu_addr_hi_shift    = 32U,  /**< uint64 address -> high 32 bits. */
} ra_npu_test_const_t;

/** @brief Fake Vela command stream (contents irrelevant to the register test). */
static uint8_t s_cmd_stream[k_test_npu_cmd_bytes];
/** @brief Fake region 0 arena (weights by convention). */
static uint8_t s_weights[k_test_npu_arena_bytes];
/** @brief Fake region 1 arena (scratch). */
static uint8_t s_scratch[k_test_npu_arena_bytes];
/** @brief Fake region 2 arena (input/output). */
static uint8_t s_io_arena[k_test_npu_arena_bytes];

/**
 * @brief Populate a valid three-region job descriptor for the fixtures.
 * @param[out] job Job to fill.
 */
static void npu_fill_job(ra_npu_job_t* job)
{
  *job                                = (ra_npu_job_t){};
  job->cmd_stream                     = s_cmd_stream;
  job->cmd_stream_bytes               = k_test_npu_cmd_bytes;
  job->region_count                   = (uint8_t)k_test_npu_region_count;
  job->region_base[k_ra_npu_region_0] = (uint64_t)(uintptr_t)s_weights;
  job->region_base[k_ra_npu_region_1] = (uint64_t)(uintptr_t)s_scratch;
  job->region_base[k_ra_npu_region_2] = (uint64_t)(uintptr_t)s_io_arena;
}

/**
 * @brief Reset host MMIO and bring the NPU up cleanly (deinit any prior state).
 */
static void npu_prep(void)
{
  (void)ra_npu_deinit();
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_init());
}

/**
 * @brief Assert BASEPn (lo+hi) holds the expected 64-bit base for one region.
 * @param[in] idx  Region index.
 * @param[in] base Expected AXI base address.
 */
static void npu_assert_region(ra_npu_region_idx_t idx, uint64_t base)
{
  const uint32_t lo_off =
    (uint32_t)k_ra_npu_off_basep0_lo + ((uint32_t)idx * (uint32_t)k_ra_npu_basep_stride_bytes);
  const uint32_t hi_off = lo_off + (uint32_t)k_ra_npu_reg_hi_offset;
  TEST_ASSERT_EQ(base, *ra_npu_reg(lo_off));
  TEST_ASSERT_EQ((base >> k_test_npu_addr_hi_shift), *ra_npu_reg(hi_off));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the init-guard contract;
 * every rejection is a single-condition `RA_VALIDATE_INIT` in the code under
 * test, no `&&` / `||`)
 */
static void test_uninitialized_rejects(void)
{
  TEST_BEGIN("npu rejects ops before init");
  ra_sim_mmap_reset();

  ra_npu_job_t job = {};
  npu_fill_job(&job);
  uint32_t id   = 0U;
  bool     done = false;

  /* No ra_npu_init() has run yet in this binary. */
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_npu_run());
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_npu_read_id(&id));
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_npu_poll(&done));
  TEST_END("npu rejects ops before init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- init runs mstp-enable + reset, both
 * single-condition error checks in the code under test)
 */
static void test_init_ungates_and_reads_id(void)
{
  TEST_BEGIN("npu init ungates + id readable");
  npu_prep();

  uint32_t id = 0xFFFFFFFFU;
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_read_id(&id));
  /* After a host reset the ID register reads back the zeroed backing store. */
  TEST_ASSERT_EQ(0U, id);
  TEST_END("npu init ungates + id readable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- submit's happy path has no `&&`/`||`;
 * it programs QBASE/QSIZE/BASEPn and returns k_ra_ok)
 */
static void test_submit_programs_queue_and_regions(void)
{
  TEST_BEGIN("npu submit programs queue + regions");
  npu_prep();

  ra_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_submit(&job));

  const uint64_t cs = (uint64_t)(uintptr_t)s_cmd_stream;
  TEST_ASSERT_EQ(cs, *ra_npu_reg(k_ra_npu_off_qbase_lo));
  TEST_ASSERT_EQ((cs >> k_test_npu_addr_hi_shift), *ra_npu_reg(k_ra_npu_off_qbase_hi));
  TEST_ASSERT_EQ(k_test_npu_cmd_bytes, *ra_npu_reg(k_ra_npu_off_qsize));

  npu_assert_region(k_ra_npu_region_0, (uint64_t)(uintptr_t)s_weights);
  npu_assert_region(k_ra_npu_region_1, (uint64_t)(uintptr_t)s_scratch);
  npu_assert_region(k_ra_npu_region_2, (uint64_t)(uintptr_t)s_io_arena);
  TEST_END("npu submit programs queue + regions");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- each rejection path in submit is a
 * separate single-condition guard, exercised one at a time)
 */
static void test_submit_rejects_bad_args(void)
{
  TEST_BEGIN("npu submit rejects bad args");
  npu_prep();

  ra_npu_job_t job = {};
  npu_fill_job(&job);

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_npu_submit(nullptr));

  ra_npu_job_t no_stream = job;
  no_stream.cmd_stream   = nullptr;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_npu_submit(&no_stream));

  ra_npu_job_t empty     = job;
  empty.cmd_stream_bytes = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_npu_submit(&empty));

  ra_npu_job_t too_many = job;
  too_many.region_count = (uint8_t)k_test_npu_too_many_regions;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_npu_submit(&too_many));
  TEST_END("npu submit rejects bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- run's job guard is a single-condition
 * `if (!s_npu_job_submitted)`; the kick is an unconditional register write)
 */
static void test_run_requires_submit_then_kicks(void)
{
  TEST_BEGIN("npu run needs submit then kicks");
  npu_prep();

  /* Fresh init: no job submitted yet. */
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_npu_run());

  ra_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_run());

  /* CMD.transition_to_running_state == bit 0. */
  TEST_ASSERT_EQ(((uint32_t)1U << k_ra_npu_cmd_run_bit), *ra_npu_reg(k_ra_npu_off_cmd));
  TEST_END("npu run needs submit then kicks");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- poll's fault test and cmd-end test are
 * each a single mask comparison; read_status decodes bits independently)
 */
static void test_poll_and_status_decode(void)
{
  TEST_BEGIN("npu poll + status decode");
  npu_prep();

  ra_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_run());

  bool done = true;
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_poll(&done));
  TEST_ASSERT(!done); /* STATUS clear -> not done. */

  /* Seed STATUS.cmd_end -> poll reports done. */
  *ra_npu_reg(k_ra_npu_off_status) = ((uint32_t)1U << k_ra_npu_status_cmd_end_bit);
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_poll(&done));
  TEST_ASSERT(done);

  ra_npu_status_t st = {};
  *ra_npu_reg(k_ra_npu_off_status) =
    ((uint32_t)1U << k_ra_npu_status_state_bit) | ((uint32_t)1U << k_ra_npu_status_cmd_end_bit);
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_read_status(&st));
  TEST_ASSERT(st.running);
  TEST_ASSERT(st.cmd_end);
  TEST_ASSERT(!st.fault);

  /* Seed a bus-error fault -> poll surfaces k_ra_err_hw_error. */
  *ra_npu_reg(k_ra_npu_off_status) = ((uint32_t)1U << k_ra_npu_status_bus_error_bit);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_npu_poll(&done));
  TEST_END("npu poll + status decode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- wait spins single-condition poll until
 * done/fault/timeout; clear_irq is one register write)
 */
static void test_wait_paths_and_clear_irq(void)
{
  TEST_BEGIN("npu wait paths + clear irq");
  npu_prep();

  ra_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_run());

  /* Completion: STATUS.cmd_end set -> wait returns ok. */
  *ra_npu_reg(k_ra_npu_off_status) = ((uint32_t)1U << k_ra_npu_status_cmd_end_bit);
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_wait());

  /* Fault: STATUS ECC-fault set -> wait returns hw_error. */
  *ra_npu_reg(k_ra_npu_off_status) = ((uint32_t)1U << k_ra_npu_status_ecc_fault_bit);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_npu_wait());

  /* Clear IRQ writes CMD.clear_irq == bit 1. */
  *ra_npu_reg(k_ra_npu_off_cmd) = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_clear_irq());
  TEST_ASSERT_EQ(((uint32_t)1U << k_ra_npu_cmd_clear_irq_bit), *ra_npu_reg(k_ra_npu_off_cmd));
  TEST_END("npu wait paths + clear irq");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- wait's timeout path spins the fixed
 * bounded budget with STATUS never signalling done, then returns hw_timeout)
 */
static void test_wait_times_out(void)
{
  TEST_BEGIN("npu wait times out");
  npu_prep();

  ra_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_run());

  /* STATUS stays clear (no cmd_end, no fault) -> bounded wait times out. */
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, ra_npu_wait());
  TEST_END("npu wait times out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- reset + deinit are single-condition
 * init guards; after deinit the submit guard rejects again)
 */
static void test_reset_and_deinit(void)
{
  TEST_BEGIN("npu reset + deinit");
  npu_prep();

  ra_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_submit(&job));

  /* Reset clears the submitted-job latch: run must be rejected again. */
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_reset());
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_npu_run());

  /* Deinit gates the block; further ops are rejected until re-init. */
  TEST_ASSERT_EQ(k_ra_ok, ra_npu_deinit());
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_npu_reset());
  TEST_END("npu reset + deinit");
}

int32_t main(void)
{
  test_uninitialized_rejects();
  test_init_ungates_and_reads_id();
  test_submit_programs_queue_and_regions();
  test_submit_rejects_bad_args();
  test_run_requires_submit_then_kicks();
  test_poll_and_status_decode();
  test_wait_paths_and_clear_irq();
  test_wait_times_out();
  test_reset_and_deinit();
  (void)fprintf(stderr, "[OK ] test_ra_npu.c\n");
  return 0;
}
