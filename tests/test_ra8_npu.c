/**
 * @file test_ra8_npu.c
 * @brief Unit tests for ra8_npu.c (Arm Ethos-U55 NPU command/queue foundation)
 *
 * @details
 * Compiled with `-DRA8_DEVICE_RA8P1` (see tests/CMakeLists.txt) so the
 * device-gated `ra8_npu.c` body is live and `ra8_npu_regs.h` is includable. The
 * NPU register window (`0x40140000`) sits inside the host MMIO backing store's
 * peripheral region, so register writes land in RAM and the tests assert the
 * exact QBASE / QSIZE / BASEPn / CMD sequence the Ethos-U55 submission protocol
 * requires -- no silicon, no real inference.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_npu.h"
#include "ra8_npu_regs.h"
#include "ra8_npu_sim_cmd.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum t_npu_probe_t
 * @brief Job-id out-parameter seed.
 */
typedef enum : uint32_t {
  k_t_id_unset = 0xFFFFFFFFU, /**< Pre-set job id; a submit that fails must
                                   leave it rather than report a real job.      */
} t_npu_probe_t;

/**
 * @enum ra8_npu_test_const_t
 * @brief Fixture sizes and seed values for the NPU driver tests.
 */
typedef enum : uint32_t {
  k_test_npu_cmd_bytes        = 256U, /**< Fake command-stream length.      */
  k_test_npu_arena_bytes      = 64U,  /**< Fake tensor-arena length.        */
  k_test_npu_region_count     = 3U,   /**< Regions programmed by the job.   */
  k_test_npu_too_many_regions = 9U,   /**< One past k_ra8_npu_region_count. */
  k_test_npu_addr_hi_shift    = 32U,  /**< uint64 address -> high 32 bits.  */
  k_test_npu_addr_lo_mask =
    (1ULL << k_test_npu_addr_hi_shift) - 1U, /**< Low word: addr & this == LO reg. */
} ra8_npu_test_const_t;

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
static void npu_fill_job(ra8_npu_job_t* job)
{
  *job                                 = (ra8_npu_job_t){};
  job->cmd_stream                      = s_cmd_stream;
  job->cmd_stream_bytes                = k_test_npu_cmd_bytes;
  job->region_count                    = (uint8_t)k_test_npu_region_count;
  job->region_base[k_ra8_npu_region_0] = (uint64_t)(uintptr_t)s_weights;
  job->region_base[k_ra8_npu_region_1] = (uint64_t)(uintptr_t)s_scratch;
  job->region_base[k_ra8_npu_region_2] = (uint64_t)(uintptr_t)s_io_arena;
}

/**
 * @brief Reset host MMIO and bring the NPU up cleanly (deinit any prior state).
 */
static void npu_prep(void)
{
  (void)ra8_npu_deinit();
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_init());
}

/**
 * @brief Assert BASEPn (lo+hi) holds the expected 64-bit base for one region.
 * @param[in] idx  Region index.
 * @param[in] base Expected AXI base address.
 */
static void npu_assert_region(ra8_npu_region_idx_t idx, uint64_t base)
{
  const uint32_t lo_off =
    (uint32_t)k_ra8_npu_off_basep0_lo + ((uint32_t)idx * (uint32_t)k_ra8_npu_basep_stride_bytes);
  const uint32_t hi_off = lo_off + (uint32_t)k_ra8_npu_reg_hi_offset;
  TEST_ASSERT_EQ((base & k_test_npu_addr_lo_mask), *ra8_npu_reg(lo_off));
  TEST_ASSERT_EQ((base >> k_test_npu_addr_hi_shift), *ra8_npu_reg(hi_off));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the init-guard contract;
 * every rejection is a single-condition `RA8_VALIDATE_INIT` in the code under
 * test, no `&&` / `||`)
 */
static void test_uninitialized_rejects(void)
{
  TEST_BEGIN("npu rejects ops before init");
  ra8_sim_mmap_reset();

  ra8_npu_job_t job = {};
  npu_fill_job(&job);
  uint32_t id   = 0U;
  bool     done = false;

  /* No ra8_npu_init() has run yet in this binary. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_npu_run());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_npu_read_id(&id));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_npu_poll(&done));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_npu_irq_arm());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_npu_wait_irq());
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

  uint32_t id = k_t_id_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_read_id(&id));
  /* After a host reset the ID register reads back the zeroed backing store. */
  TEST_ASSERT_EQ(0U, id);
  TEST_END("npu init ungates + id readable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- submit's happy path has no `&&`/`||`;
 * it programs QBASE/QSIZE/BASEPn and returns k_ra8_ok)
 */
static void test_submit_programs_queue_and_regions(void)
{
  TEST_BEGIN("npu submit programs queue + regions");
  npu_prep();

  ra8_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_submit(&job));

  const uint64_t cs = (uint64_t)(uintptr_t)s_cmd_stream;
  TEST_ASSERT_EQ((cs & k_test_npu_addr_lo_mask), *ra8_npu_reg(k_ra8_npu_off_qbase_lo));
  TEST_ASSERT_EQ((cs >> k_test_npu_addr_hi_shift), *ra8_npu_reg(k_ra8_npu_off_qbase_hi));
  TEST_ASSERT_EQ(k_test_npu_cmd_bytes, *ra8_npu_reg(k_ra8_npu_off_qsize));

  npu_assert_region(k_ra8_npu_region_0, (uint64_t)(uintptr_t)s_weights);
  npu_assert_region(k_ra8_npu_region_1, (uint64_t)(uintptr_t)s_scratch);
  npu_assert_region(k_ra8_npu_region_2, (uint64_t)(uintptr_t)s_io_arena);
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

  ra8_npu_job_t job = {};
  npu_fill_job(&job);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_npu_submit(nullptr));

  ra8_npu_job_t no_stream = job;
  no_stream.cmd_stream    = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_npu_submit(&no_stream));

  ra8_npu_job_t empty    = job;
  empty.cmd_stream_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_npu_submit(&empty));

  ra8_npu_job_t too_many = job;
  too_many.region_count  = (uint8_t)k_test_npu_too_many_regions;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_npu_submit(&too_many));
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
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_npu_run());

  ra8_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_run());

  /* CMD.transition_to_running_state == bit 0. */
  TEST_ASSERT_EQ(((uint32_t)1U << k_ra8_npu_cmd_run_bit), *ra8_npu_reg(k_ra8_npu_off_cmd));
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

  ra8_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_run());

  bool done = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_poll(&done));
  TEST_ASSERT(!done); /* STATUS clear -> not done. */

  /* Seed STATUS.cmd_end -> poll reports done. */
  *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_cmd_end_bit);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_poll(&done));
  TEST_ASSERT(done);

  ra8_npu_status_t st = {};
  *ra8_npu_reg(k_ra8_npu_off_status) =
    ((uint32_t)1U << k_ra8_npu_status_state_bit) | ((uint32_t)1U << k_ra8_npu_status_cmd_end_bit);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_read_status(&st));
  TEST_ASSERT(st.running);
  TEST_ASSERT(st.cmd_end);
  TEST_ASSERT(!st.fault);

  /* Seed a bus-error fault -> poll surfaces k_ra8_err_hw_error. */
  *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_bus_error_bit);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_npu_poll(&done));
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

  ra8_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_run());

  /* Completion: STATUS.cmd_end set -> wait returns ok. */
  *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_cmd_end_bit);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_wait());

  /* Fault: STATUS ECC-fault set -> wait returns hw_error. */
  *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_ecc_fault_bit);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_npu_wait());

  /* Clear IRQ writes CMD.clear_irq == bit 1. */
  *ra8_npu_reg(k_ra8_npu_off_cmd) = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_clear_irq());
  TEST_ASSERT_EQ(((uint32_t)1U << k_ra8_npu_cmd_clear_irq_bit), *ra8_npu_reg(k_ra8_npu_off_cmd));
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

  ra8_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_run());

  /* STATUS stays clear (no cmd_end, no fault) -> bounded wait times out. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_npu_wait());
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

  ra8_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_submit(&job));

  /* Reset clears the submitted-job latch: run must be rejected again. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_reset());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_npu_run());

  /* Deinit gates the block; further ops are rejected until re-init. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_deinit());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_npu_reset());
  TEST_END("npu reset + deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- irq_arm's submit guard, wait_irq's
 * armed guard, and the handler's single-condition status branches are each one
 * condition; they are driven one path at a time)
 */
static void test_irq_arm_gates(void)
{
  TEST_BEGIN("npu irq arm/wait gating");
  npu_prep();

  ra8_npu_job_t job = {};
  npu_fill_job(&job);

  /* Arm before submit is rejected; wait before arm is rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_npu_irq_arm());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_npu_wait_irq());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_irq_arm());
  TEST_END("npu irq arm/wait gating");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it drives the interrupt completion path:
 * arm -> run -> ISR latches STATUS.cmd_end -> wait_irq returns ok; each handler
 * branch is a single condition)
 */
static void test_irq_driven_completion(void)
{
  TEST_BEGIN("npu irq-driven completion");
  npu_prep();

  ra8_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_irq_arm());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_run());

  /* Spurious IRQ first: STATUS clear (no cmd_end, no fault) -> handler leaves the
   * latch armed and only acks the line. */
  *ra8_npu_reg(k_ra8_npu_off_status) = 0U;
  ra8_npu_irq_handler(nullptr);
  TEST_ASSERT_EQ(((uint32_t)1U << k_ra8_npu_cmd_clear_irq_bit), *ra8_npu_reg(k_ra8_npu_off_cmd));

  /* Real completion: STATUS.cmd_end latched -> handler advances to done, and the
   * bounded wait returns immediately. */
  *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_cmd_end_bit);
  ra8_npu_irq_handler(nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_wait_irq());
  TEST_END("npu irq-driven completion");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the handler's fault branch and
 * wait_irq's fault branch are each a single condition)
 */
static void test_irq_fault_and_timeout(void)
{
  TEST_BEGIN("npu irq fault + timeout paths");
  npu_prep();

  ra8_npu_job_t job = {};
  npu_fill_job(&job);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_irq_arm());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_run());

  /* Fault: STATUS bus-error latched -> handler advances to fault, wait reports it. */
  *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_bus_error_bit);
  ra8_npu_irq_handler(nullptr);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_npu_wait_irq());

  /* Timeout: re-arm, never latch a result -> the bounded wait exhausts its budget. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_irq_arm());
  *ra8_npu_reg(k_ra8_npu_off_status) = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_npu_wait_irq());

  /* Handler while de-initialised: read_status fails, so it latches nothing and
   * cannot fault (covers the status-read failure branch). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_deinit());
  ra8_npu_irq_handler(nullptr);
  TEST_END("npu irq fault + timeout paths");
}

/* --------------------------------------------------------------------------
 * Execution-model coverage (issue #222): drive the FULL submit -> run ->
 * poll -> read-output path through a host-side MOCK of the ra8_emulator NPU
 * execution model. The mock decodes the command stream via the SHARED
 * ra8_npu_sim_cmd.h convention (so it can never drift from
 * tools/ra8_emulator/src/periph/board_periph_npu.c) and applies the op to the tensor
 * arenas, then latches STATUS exactly as the sim does: cmd_end on success, a
 * cmd_parse fault on a stream it cannot interpret (honest model -- no faked
 * completion). The NPU register window is plain host RAM here, so the driver
 * reads back the QBASE / QSIZE / BASEPn the mock consumes.
 * -------------------------------------------------------------------------- */

/**
 * @enum ra8_npu_exec_const_t
 * @brief Fixture sizes + input pattern for the execution-model tests.
 */
typedef enum : uint32_t {
  k_test_exec_bytes     = 64U,   /**< Tensor-arena length (bytes).          */
  k_test_exec_addk      = 0x11U, /**< Constant the add-constant op applies. */
  k_test_exec_seed_mul  = 7U,    /**< input[i] = (i*mul + add) & mask.      */
  k_test_exec_seed_add  = 3U,    /**< Input pattern additive offset.        */
  k_test_exec_byte_mask = 0xFFU, /**< 8-bit element wrap (matches the op).  */
} ra8_npu_exec_const_t;

/** @brief Sim command stream (ra8_npu_sim_cmd.h layout) for the exec tests. */
static uint32_t s_exec_cmd[k_ra8_npu_sim_word_num];
/** @brief Region 0 weight arena (present by convention; unused by the op). */
static uint8_t s_exec_weights[k_test_exec_bytes];
/** @brief Region 1 input tensor (op source). */
static uint8_t s_exec_input[k_test_exec_bytes];
/** @brief Region 2 output tensor (op destination). */
static uint8_t s_exec_output[k_test_exec_bytes];

/** @brief One decoded sim command (mirrors board_periph_npu.c npu_cmd_t). */
typedef struct {
  uint32_t op;    /**< Opcode (COPY / add-constant).      */
  uint32_t src;   /**< Source region index.               */
  uint32_t dst;   /**< Destination region index.          */
  uint32_t count; /**< Byte count to process.             */
  uint32_t konst; /**< Constant addend (add-constant op). */
} npu_exec_cmd_t;

/** @brief Read BASEPn back from the NPU registers as a host arena pointer. */
static uint8_t* npu_exec_region(ra8_npu_region_idx_t idx)
{
  const uint32_t lo_off =
    (uint32_t)k_ra8_npu_off_basep0_lo + ((uint32_t)idx * (uint32_t)k_ra8_npu_basep_stride_bytes);
  const uint32_t hi_off = lo_off + (uint32_t)k_ra8_npu_reg_hi_offset;
  const uint64_t base =
    (uint64_t)*ra8_npu_reg(lo_off) | ((uint64_t)*ra8_npu_reg(hi_off) << k_test_npu_addr_hi_shift);
  return (uint8_t*)(uintptr_t)base;
}

/**
 * @brief Decode the submitted stream at QBASE per the sim convention.
 * @param[out] out Filled with the command fields on success.
 * @return true when the stream is a valid sim program, else false.
 */
static bool npu_exec_decode(npu_exec_cmd_t* out)
{
  const uint64_t qbase =
    (uint64_t)*ra8_npu_reg(k_ra8_npu_off_qbase_lo) |
    ((uint64_t)*ra8_npu_reg(k_ra8_npu_off_qbase_hi) << k_test_npu_addr_hi_shift);
  const uint32_t qsize = *ra8_npu_reg(k_ra8_npu_off_qsize);
  if (qsize < (uint32_t)k_ra8_npu_sim_header_bytes) {
    return false;
  }
  const uint32_t* w = (const uint32_t*)(uintptr_t)qbase;
  if ((w[k_ra8_npu_sim_word_op] & (uint32_t)k_ra8_npu_sim_magic_mask) !=
      (uint32_t)k_ra8_npu_sim_magic) {
    return false;
  }
  out->op    = w[k_ra8_npu_sim_word_op] & (uint32_t)k_ra8_npu_sim_op_mask;
  out->src   = w[k_ra8_npu_sim_word_src];
  out->dst   = w[k_ra8_npu_sim_word_dst];
  out->count = w[k_ra8_npu_sim_word_count];
  out->konst = w[k_ra8_npu_sim_word_const];
  if (out->src >= (uint32_t)k_ra8_npu_region_count) {
    return false;
  }
  if (out->dst >= (uint32_t)k_ra8_npu_region_count) {
    return false;
  }
  if (out->count == 0U) {
    return false;
  }
  if (out->count > (uint32_t)k_ra8_npu_sim_max_bytes) {
    return false;
  }
  return true;
}

/** @brief Mirror the ra8_emulator NPU model: decode, apply, latch STATUS. */
static void npu_exec_mock(void)
{
  npu_exec_cmd_t c = {};
  if (!npu_exec_decode(&c)) {
    /* Honest model: a stream we cannot interpret faults, never fakes done. */
    *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_cmd_parse_bit);
    return;
  }
  uint8_t* s = npu_exec_region((ra8_npu_region_idx_t)c.src);
  uint8_t* d = npu_exec_region((ra8_npu_region_idx_t)c.dst);
  for (uint32_t i = 0U; i < c.count; i++) {
    d[i] = (c.op == (uint32_t)k_ra8_npu_sim_op_addk)
             ? (uint8_t)((s[i] + c.konst) & (uint32_t)k_ra8_npu_sim_byte_mask)
             : s[i];
  }
  *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_cmd_end_bit) |
                                       ((uint32_t)1U << k_ra8_npu_status_irq_raised_bit);
}

/** @brief Program s_exec_cmd with an add-constant of region 1 -> region 2. */
static void npu_exec_build_stream(void)
{
  s_exec_cmd[k_ra8_npu_sim_word_op] =
    (uint32_t)k_ra8_npu_sim_magic | (uint32_t)k_ra8_npu_sim_op_addk;
  s_exec_cmd[k_ra8_npu_sim_word_src]   = (uint32_t)k_ra8_npu_region_1;
  s_exec_cmd[k_ra8_npu_sim_word_dst]   = (uint32_t)k_ra8_npu_region_2;
  s_exec_cmd[k_ra8_npu_sim_word_count] = (uint32_t)k_test_exec_bytes;
  s_exec_cmd[k_ra8_npu_sim_word_const] = (uint32_t)k_test_exec_addk;
}

/** @brief Seed s_exec_input with a deterministic pattern; zero s_exec_output. */
static void npu_exec_seed(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_test_exec_bytes; i++) {
    s_exec_input[i] =
      (uint8_t)(((i * (uint32_t)k_test_exec_seed_mul) + (uint32_t)k_test_exec_seed_add) &
                (uint32_t)k_test_exec_byte_mask);
    s_exec_output[i] = 0U;
  }
}

/** @brief Fill @p job with the 3-region exec descriptor (weights/input/output). */
static void npu_exec_fill_job(ra8_npu_job_t* job)
{
  *job                                 = (ra8_npu_job_t){};
  job->cmd_stream                      = s_exec_cmd;
  job->cmd_stream_bytes                = (uint32_t)sizeof(s_exec_cmd);
  job->region_count                    = (uint8_t)k_test_npu_region_count;
  job->region_base[k_ra8_npu_region_0] = (uint64_t)(uintptr_t)s_exec_weights;
  job->region_base[k_ra8_npu_region_1] = (uint64_t)(uintptr_t)s_exec_input;
  job->region_base[k_ra8_npu_region_2] = (uint64_t)(uintptr_t)s_exec_output;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the mock's guards are each a single
 * condition, and the driver path exercised here -- submit/run happy paths and
 * poll's single-condition fault-mask + cmd_end tests -- has no `&&`/`||`)
 */
static void test_exec_runs_addk_job(void)
{
  TEST_BEGIN("npu exec add-const job end-to-end");
  npu_prep();
  npu_exec_build_stream();
  npu_exec_seed();

  ra8_npu_job_t job = {};
  npu_exec_fill_job(&job);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_run());

  /* Emulator-mirror execution: applies the op + latches STATUS.cmd_end. */
  npu_exec_mock();

  bool done = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_poll(&done));
  TEST_ASSERT(done);

  ra8_npu_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_read_status(&st));
  TEST_ASSERT(st.cmd_end);
  TEST_ASSERT(!st.fault);

  /* The output arena must hold input+K byte-for-byte (deterministic). */
  for (uint32_t i = 0U; i < (uint32_t)k_test_exec_bytes; i++) {
    const uint8_t expected =
      (uint8_t)((s_exec_input[i] + (uint32_t)k_test_exec_addk) & (uint32_t)k_test_exec_byte_mask);
    TEST_ASSERT_EQ(expected, s_exec_output[i]);
  }
  TEST_END("npu exec add-const job end-to-end");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it drives the honest-model reject: a
 * stream with no sim magic makes the mock latch cmd_parse, and poll's
 * single-condition fault-mask test then surfaces k_ra8_err_hw_error)
 */
static void test_exec_rejects_non_sim_stream(void)
{
  TEST_BEGIN("npu exec rejects non-sim stream");
  npu_prep();

  /* A zeroed stream carries no SE55 magic: a real Vela program the model
   * cannot interpret. The driver accepts the submit (non-null, bytes > 0). */
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_npu_sim_word_num; i++) {
    s_exec_cmd[i] = 0U;
  }
  npu_exec_seed();

  ra8_npu_job_t job = {};
  npu_exec_fill_job(&job);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_run());

  /* Emulator-mirror execution latches a parse fault (no faked completion). */
  npu_exec_mock();

  bool done = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_npu_poll(&done));

  /* The output arena is untouched (all zero): nothing was executed. */
  for (uint32_t i = 0U; i < (uint32_t)k_test_exec_bytes; i++) {
    TEST_ASSERT_EQ(0U, s_exec_output[i]);
  }
  TEST_END("npu exec rejects non-sim stream");
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
  test_irq_arm_gates();
  test_irq_driven_completion();
  test_irq_fault_and_timeout();
  test_exec_runs_addk_job();
  test_exec_rejects_non_sim_stream();
  (void)fprintf(stderr, "[OK ] test_ra8_npu.c\n");
  return 0;
}
