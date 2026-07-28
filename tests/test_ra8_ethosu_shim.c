/**
 * @file test_ra8_ethosu_shim.c
 * @brief Unit tests for ra8_ethosu_shim.c (Arm ethos-u-core-driver -> ra8_npu adapter)
 *
 * @details
 * Compiled with `-DRA8_DEVICE_RA8P1` (see tests/CMakeLists.txt) so the
 * device-gated `ra8_ethosu_shim.c` and `ra8_npu.c` bodies are live and
 * `ra8_npu_regs.h` is includable. The NPU register window (`0x40140000`) sits
 * inside the host MMIO backing store's peripheral region, so the driver's
 * register writes land in RAM and these tests assert that the Arm API surface
 * -- ethosu_reserve_driver() + ethosu_invoke_v3() -- programs the exact
 * QBASE / QSIZE / BASEPn / CMD sequence the Ethos-U55 submission protocol
 * requires, and returns 0. No silicon, no real inference.
 *
 * ethosu_invoke_v3() blocks in ra8_npu_wait() until STATUS.cmd_end latches;
 * nothing reaches cmd_end on its own in a host test, so each test pre-latches
 * STATUS.cmd_end (as the ra8_emulator NPU model would after executing) before the
 * invoke, so the bounded wait returns immediately.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_ethosu_shim.h"
#include "ra8_npu.h"
#include "ra8_npu_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra8_ethosu_test_const_t
 * @brief Fixture sizes and address-splitting constants for the shim tests.
 */
typedef enum : uint32_t {
  k_test_shim_cmd_bytes     = 128U,        /**< Fake command-stream length.          */
  k_test_shim_arena_bytes   = 64U,         /**< Fake tensor-arena length.            */
  k_test_shim_region_count  = 3U,          /**< Regions in the happy-path job.       */
  k_test_shim_two_regions   = 2U,          /**< Regions in the 2-region job.         */
  k_test_shim_one_region    = 1U,          /**< Regions in the 1-region job.         */
  k_test_shim_zero_regions  = 0U,          /**< Region-less job (MC/DC A=F leg).     */
  k_test_shim_overflow_cnt  = 9U,          /**< One past k_ra8_npu_region_count (8). */
  k_test_shim_neg_cnt       = 0xFFFFFFFFU, /**< Stand-in for a negative int count.   */
  k_test_shim_addr_hi_shift = 32U,         /**< uint64 address -> high 32 bits.      */
  k_test_shim_addr_lo_mask  = 0xFFFFFFFFU, /**< Low word: addr & this == LO reg.     */
} ra8_ethosu_test_const_t;

/** @brief Fake Vela command stream (contents irrelevant to the register test). */
static uint8_t s_shim_cmd[k_test_shim_cmd_bytes];
/** @brief Fake region 0 arena (weights by convention). */
static uint8_t s_shim_weights[k_test_shim_arena_bytes];
/** @brief Fake region 1 arena (input tensor). */
static uint8_t s_shim_input[k_test_shim_arena_bytes];
/** @brief Fake region 2 arena (output tensor). */
static uint8_t s_shim_output[k_test_shim_arena_bytes];

/**
 * @brief Reset host MMIO, reserve the NPU, and pre-latch STATUS.cmd_end.
 * @return The reserved (non-NULL) driver handle.
 */
static struct ethosu_driver* shim_prep(void)
{
  ra8_sim_mmap_reset();                                /**< RA8 sim mmap reset.    */
  struct ethosu_driver* drv = ethosu_reserve_driver(); /**< Ethosu reserve driver. */
  TEST_ASSERT_NOT_NULL(drv);                           /**< Drv.                   */
  /* No real NPU reaches cmd_end here: latch it so the blocking ra8_npu_wait()
   * inside ethosu_invoke_v3() returns immediately (as the sim model would). */
  *ra8_npu_reg(k_ra8_npu_off_status) =
    ((uint32_t)1U << k_ra8_npu_status_cmd_end_bit); /**< RA8 npu status cmd end bit. */
  return drv;                                       /**< Drv.                        */
}

/**
 * @brief Assert a 64-bit register pair (lo+hi words) holds @p expect.
 * @param[in] lo_off Byte offset of the low word.
 * @param[in] expect Expected 64-bit value.
 */
static void shim_assert_reg64(uint32_t lo_off, uint64_t expect)
{
  const uint32_t hi_off = lo_off + (uint32_t)k_ra8_npu_reg_hi_offset;
  TEST_ASSERT_EQ((expect & (uint64_t)k_test_shim_addr_lo_mask), *ra8_npu_reg(lo_off));
  TEST_ASSERT_EQ((expect >> k_test_shim_addr_hi_shift), *ra8_npu_reg(hi_off));
}

/**
 * @brief Assert BASEPn (lo+hi) holds @p base for one region.
 * @param[in] idx  Region index.
 * @param[in] base Expected AXI base address.
 */
static void shim_assert_region(ra8_npu_region_idx_t idx, uint64_t base)
{
  const uint32_t lo_off =
    (uint32_t)k_ra8_npu_off_basep0_lo + ((uint32_t)idx * (uint32_t)k_ra8_npu_basep_stride_bytes);
  shim_assert_reg64(lo_off, base);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- ethosu_invoke_v3's guards are each a
 * single condition, and the ra8_npu submit/run happy paths plus poll's
 * single-condition fault-mask + cmd_end tests carry no `&&` / `||`)
 */
static void test_reserve_and_invoke_programs_registers(void)
{
  TEST_BEGIN("ethosu reserve + invoke programs NPU registers");
  struct ethosu_driver* drv = shim_prep();

  uint64_t base_addrs[k_test_shim_region_count];
  size_t   sizes[k_test_shim_region_count];
  base_addrs[k_ra8_npu_region_0] = (uint64_t)(uintptr_t)s_shim_weights;
  base_addrs[k_ra8_npu_region_1] = (uint64_t)(uintptr_t)s_shim_input;
  base_addrs[k_ra8_npu_region_2] = (uint64_t)(uintptr_t)s_shim_output;
  sizes[k_ra8_npu_region_0]      = sizeof(s_shim_weights);
  sizes[k_ra8_npu_region_1]      = sizeof(s_shim_input);
  sizes[k_ra8_npu_region_2]      = sizeof(s_shim_output);

  const int rc = ethosu_invoke_v3(drv,
                                  s_shim_cmd,
                                  (int)sizeof(s_shim_cmd),
                                  base_addrs,
                                  sizes,
                                  (int)k_test_shim_region_count,
                                  nullptr);
  TEST_ASSERT_EQ(0, rc);

  /* QBASE/QSIZE point at the command stream the shim was handed. */
  shim_assert_reg64(k_ra8_npu_off_qbase_lo, (uint64_t)(uintptr_t)s_shim_cmd);
  TEST_ASSERT_EQ(sizeof(s_shim_cmd), *ra8_npu_reg(k_ra8_npu_off_qsize));

  /* BASEPn hold the region bases from base_addrs[]. */
  shim_assert_region(k_ra8_npu_region_0, base_addrs[k_ra8_npu_region_0]);
  shim_assert_region(k_ra8_npu_region_1, base_addrs[k_ra8_npu_region_1]);
  shim_assert_region(k_ra8_npu_region_2, base_addrs[k_ra8_npu_region_2]);

  /* ra8_npu_run() kicked the job: CMD.transition_to_running_state == bit 0. */
  TEST_ASSERT_EQ(((uint32_t)1U << k_ra8_npu_cmd_run_bit), *ra8_npu_reg(k_ra8_npu_off_cmd));
  TEST_END("ethosu reserve + invoke programs NPU registers");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- a 2-region invoke exercises the same
 * single-condition guards and the bounded region-copy loop with a smaller count)
 */
static void test_invoke_two_regions(void)
{
  TEST_BEGIN("ethosu invoke programs a 2-region job");
  struct ethosu_driver* drv = shim_prep();

  uint64_t base_addrs[k_test_shim_two_regions];
  size_t   sizes[k_test_shim_two_regions];
  base_addrs[k_ra8_npu_region_0] = (uint64_t)(uintptr_t)s_shim_weights;
  base_addrs[k_ra8_npu_region_1] = (uint64_t)(uintptr_t)s_shim_input;
  sizes[k_ra8_npu_region_0]      = sizeof(s_shim_weights);
  sizes[k_ra8_npu_region_1]      = sizeof(s_shim_input);

  const int rc = ethosu_invoke_v3(drv,
                                  s_shim_cmd,
                                  (int)sizeof(s_shim_cmd),
                                  base_addrs,
                                  sizes,
                                  (int)k_test_shim_two_regions,
                                  nullptr);
  TEST_ASSERT_EQ(0, rc);

  shim_assert_reg64(k_ra8_npu_off_qbase_lo, (uint64_t)(uintptr_t)s_shim_cmd);
  TEST_ASSERT_EQ(sizeof(s_shim_cmd), *ra8_npu_reg(k_ra8_npu_off_qsize));
  shim_assert_region(k_ra8_npu_region_0, base_addrs[k_ra8_npu_region_0]);
  shim_assert_region(k_ra8_npu_region_1, base_addrs[k_ra8_npu_region_1]);
  TEST_END("ethosu invoke programs a 2-region job");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- reserve's ready-flag guard and
 * release's null-check are each a single condition; the handle-identity and
 * post-release invoke assertions carry no `&&` / `||`)
 */
static void test_reserve_idempotent_release_noop(void)
{
  TEST_BEGIN("ethosu reserve idempotent + release no-op");
  ra8_sim_mmap_reset();

  struct ethosu_driver* a = ethosu_reserve_driver();
  struct ethosu_driver* b = ethosu_reserve_driver();
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT(a == b); /* single persistent NPU -> same handle every time. */

  ethosu_release_driver(a); /* no-op on the singular NPU. */

  struct ethosu_driver* c = ethosu_reserve_driver();
  TEST_ASSERT(c == a); /* same context handed back after release. */

  /* Invoke still succeeds after release + re-reserve. */
  *ra8_npu_reg(k_ra8_npu_off_status)           = ((uint32_t)1U << k_ra8_npu_status_cmd_end_bit);
  uint64_t  base_addrs[k_test_shim_one_region] = {(uint64_t)(uintptr_t)s_shim_input};
  size_t    sizes[k_test_shim_one_region]      = {sizeof(s_shim_input)};
  const int rc                                 = ethosu_invoke_v3(c,
                                                                  s_shim_cmd,
                                                                  (int)sizeof(s_shim_cmd),
                                                                  base_addrs,
                                                                  sizes,
                                                                  (int)k_test_shim_one_region,
                                                                  nullptr);
  TEST_ASSERT_EQ(0, rc);
  shim_assert_region(k_ra8_npu_region_0, base_addrs[k_ra8_npu_region_0]);
  TEST_END("ethosu reserve idempotent + release no-op");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- each rejection path in ethosu_invoke_v3
 * is a separate single-condition guard, exercised one at a time; the region-count
 * overflow guard is the single condition `num_base_addr > k_ra8_npu_region_count`)
 */
static void test_invoke_rejects_bad_args(void)
{
  TEST_BEGIN("ethosu invoke rejects bad args");
  struct ethosu_driver* drv = shim_prep();

  uint64_t base_addrs[k_test_shim_region_count];
  size_t   sizes[k_test_shim_region_count];
  base_addrs[k_ra8_npu_region_0] = (uint64_t)(uintptr_t)s_shim_weights;
  base_addrs[k_ra8_npu_region_1] = (uint64_t)(uintptr_t)s_shim_input;
  base_addrs[k_ra8_npu_region_2] = (uint64_t)(uintptr_t)s_shim_output;
  sizes[k_ra8_npu_region_0]      = sizeof(s_shim_weights);
  sizes[k_ra8_npu_region_1]      = sizeof(s_shim_input);
  sizes[k_ra8_npu_region_2]      = sizeof(s_shim_output);
  const int good_cnt             = (int)k_test_shim_region_count;

  /* Null driver handle. */
  TEST_ASSERT(ethosu_invoke_v3(nullptr,
                               s_shim_cmd,
                               (int)sizeof(s_shim_cmd),
                               base_addrs,
                               sizes,
                               good_cnt,
                               nullptr) != 0);
  /* Null command stream. */
  TEST_ASSERT(
    ethosu_invoke_v3(drv, nullptr, (int)sizeof(s_shim_cmd), base_addrs, sizes, good_cnt, nullptr) !=
    0);
  /* Empty command stream. */
  TEST_ASSERT(ethosu_invoke_v3(drv, s_shim_cmd, 0, base_addrs, sizes, good_cnt, nullptr) != 0);
  /* Negative region count. */
  TEST_ASSERT(ethosu_invoke_v3(drv,
                               s_shim_cmd,
                               (int)sizeof(s_shim_cmd),
                               base_addrs,
                               sizes,
                               (int)k_test_shim_neg_cnt,
                               nullptr) != 0);
  /* Region count past ra8_npu's region_base[] capacity -- overflow rejected. */
  TEST_ASSERT(ethosu_invoke_v3(drv,
                               s_shim_cmd,
                               (int)sizeof(s_shim_cmd),
                               base_addrs,
                               sizes,
                               (int)k_test_shim_overflow_cnt,
                               nullptr) != 0);

  /* A rejected invoke must not have kicked the job: CMD stays clear. */
  TEST_ASSERT_EQ(0U, *ra8_npu_reg(k_ra8_npu_off_cmd));
  TEST_END("ethosu invoke rejects bad args");
}

/**
 * @brief Exercise the one compound guard in the shim: the null-region-array check.
 *
 * @details
 * `internal_invoke_args_ok()` rejects a job that claims regions but hands over a
 * NULL base-address array via `if ((num_base_addr > 0) && (base_addr == nullptr))`.
 * The three invokes below drive that decision through minimal MC/DC (reached via
 * the public ethosu_invoke_v3(); the accepting legs pre-latch STATUS.cmd_end so
 * the bounded wait returns).
 *
 * @par MC/DC:
 * Decision: `if ((num_base_addr > 0) && (base_addr == nullptr))` (2 conditions)
 * - Vector 1: num_base_addr=1, base_addr=NULL  -> true  (reject): both conditions true.
 * - Vector 2: num_base_addr=0, base_addr=NULL  -> false (accept): `num_base_addr > 0`
 *             is false, so `&&` short-circuits and base_addr is never evaluated.
 * - Vector 3: num_base_addr=1, base_addr=valid -> false (accept): only base_addr varies.
 * Vectors 1+2 prove `num_base_addr > 0` independently drives the outcome; vectors
 * 1+3 prove the same for `base_addr == nullptr`. N+1 = 3 vectors for N=2 conditions.
 */
static void test_invoke_null_base_addr_guard_mcdc(void)
{
  TEST_BEGIN("ethosu invoke null-base_addr guard (MC/DC)");
  struct ethosu_driver* drv = shim_prep();

  uint64_t one_base[k_test_shim_one_region] = {(uint64_t)(uintptr_t)s_shim_input};
  size_t   one_size[k_test_shim_one_region] = {sizeof(s_shim_input)};

  /* Vector 1 {num_base_addr>0 = T, base_addr==NULL = T}: guard true -> rejected. */
  TEST_ASSERT(ethosu_invoke_v3(drv,
                               s_shim_cmd,
                               (int)sizeof(s_shim_cmd),
                               nullptr,
                               one_size,
                               (int)k_test_shim_one_region,
                               nullptr) != 0);

  /* Vector 2 {num_base_addr>0 = F}: guard false by short-circuit; a region-less
   * job is valid (ra8_npu_submit accepts region_count 0), so the NULL base array is
   * never dereferenced and the invoke succeeds. */
  *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_cmd_end_bit);
  TEST_ASSERT_EQ(0,
                 ethosu_invoke_v3(drv,
                                  s_shim_cmd,
                                  (int)sizeof(s_shim_cmd),
                                  nullptr,
                                  nullptr,
                                  (int)k_test_shim_zero_regions,
                                  nullptr));

  /* Vector 3 {num_base_addr>0 = T, base_addr==NULL = F}: guard false -> accepted. */
  *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_cmd_end_bit);
  TEST_ASSERT_EQ(0,
                 ethosu_invoke_v3(drv,
                                  s_shim_cmd,
                                  (int)sizeof(s_shim_cmd),
                                  one_base,
                                  one_size,
                                  (int)k_test_shim_one_region,
                                  nullptr));

  TEST_END("ethosu invoke null-base_addr guard (MC/DC)");
}

/**
 * @brief Force `ra8_npu_init()` to fail so ethosu_reserve_driver() returns NULL.
 *
 * @details
 * `internal_npu_apply_reset()` inside `ra8_npu_init()` writes NPU_RESET then busy-
 * spins on STATUS.reset until it clears. In the host MMIO model STATUS is plain
 * RAM, so pre-latching STATUS.reset high makes the reset poll exhaust its budget
 * and `ra8_npu_init()` returns `k_ra8_err_hw_timeout`; the shim then logs and hands
 * back NULL. This MUST run before any successful reserve: once the shim's ready
 * flag latches, later reserves skip `ra8_npu_init()` entirely and this arm is
 * unreachable.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the shim's `ra8_err_is_error(err)` guard on
 * the init result is a single condition; the reset-poll timeout it depends on is
 * driven here to its failing side)
 */
static void test_reserve_reports_init_failure(void)
{
  TEST_BEGIN("ethosu reserve returns NULL when npu_init fails");
  ra8_sim_mmap_reset();
  /* Latch STATUS.reset stuck-high: the reset poll in ra8_npu_init() can never see
   * it clear, so the bounded spin times out and init fails. */
  *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_reset_bit);

  struct ethosu_driver* drv = ethosu_reserve_driver();
  TEST_ASSERT_NULL(drv);

  /* Clear the stuck bit so the next test's reserve reset succeeds. */
  ra8_sim_mmap_reset();
  TEST_END("ethosu reserve returns NULL when npu_init fails");
}

/**
 * @brief A latched STATUS fault makes ra8_npu_wait() fail, so invoke returns nonzero.
 *
 * @details
 * With a valid job, `ethosu_invoke_v3()` submits + runs, then blocks in
 * `ra8_npu_wait()`, which polls STATUS. Pre-latching a hardware-fault bit (bus
 * error) instead of cmd_end makes `ra8_npu_poll()` report `k_ra8_err_hw_error` on
 * the first poll; the shim surfaces that as a non-zero (failure) return.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the shim's `ra8_err_is_error(werr)` guard
 * on the wait result is a single condition; the driver's fault check that feeds
 * it is a single OR-reduced mask test, not a source-level compound decision)
 */
static void test_invoke_reports_wait_fault(void)
{
  TEST_BEGIN("ethosu invoke fails when the NPU latches a fault");
  ra8_sim_mmap_reset();
  struct ethosu_driver* drv = ethosu_reserve_driver();
  TEST_ASSERT_NOT_NULL(drv);
  /* Latch a bus-error fault (and deliberately NOT cmd_end) so the wait poll
   * trips the fault path rather than completing. */
  *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_bus_error_bit);

  uint64_t  base_addrs[k_test_shim_one_region] = {(uint64_t)(uintptr_t)s_shim_input};
  size_t    sizes[k_test_shim_one_region]      = {sizeof(s_shim_input)};
  const int rc                                 = ethosu_invoke_v3(drv,
                                                                  s_shim_cmd,
                                                                  (int)sizeof(s_shim_cmd),
                                                                  base_addrs,
                                                                  sizes,
                                                                  (int)k_test_shim_one_region,
                                                                  nullptr);
  TEST_ASSERT(rc != 0);
  TEST_END("ethosu invoke fails when the NPU latches a fault");
}

/**
 * @brief ethosu_release_driver() tolerates a NULL handle without dereferencing it.
 *
 * @details
 * Exercises the release guard: a NULL handle is logged and ignored, and a valid
 * handle clears its reserved flag. No NPU state is touched.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the release null-check is a single
 * condition, driven to both sides by one NULL and one valid call)
 */
static void test_release_tolerates_null(void)
{
  TEST_BEGIN("ethosu release tolerates a NULL handle");
  ethosu_release_driver(nullptr); /* must not crash: null-guarded, returns early. */

  ra8_sim_mmap_reset();
  struct ethosu_driver* drv = ethosu_reserve_driver();
  TEST_ASSERT_NOT_NULL(drv);
  ethosu_release_driver(drv); /* valid handle: clears reserved, no-op otherwise. */
  TEST_END("ethosu release tolerates a NULL handle");
}

/**
 * @brief Tearing the NPU down under a stale handle makes invoke's submit fail.
 *
 * @details
 * A caller can `ra8_npu_deinit()` the block while still holding a driver handle
 * the shim previously handed out. The next `ethosu_invoke_v3()` passes the shim's
 * own argument guards but `ra8_npu_submit()` rejects the de-initialised NPU, and
 * the shim propagates that as a non-zero return. This drives the submit-failure
 * arm the shim's pre-checks otherwise pre-empt. It runs LAST because it leaves
 * the NPU de-initialised.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the shim's `ra8_err_is_error(serr)` guard
 * on the submit result is a single condition, driven to its failing side by the
 * de-initialised NPU state)
 */
static void test_invoke_rejected_after_deinit(void)
{
  TEST_BEGIN("ethosu invoke fails after the NPU is de-initialised");
  ra8_sim_mmap_reset();
  struct ethosu_driver* drv = ethosu_reserve_driver();
  TEST_ASSERT_NOT_NULL(drv);
  /* Tear the NPU down under the still-held handle (deinit requires it up, so a
   * k_ra8_ok return also confirms the precondition held). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_deinit());

  uint64_t  base_addrs[k_test_shim_one_region] = {(uint64_t)(uintptr_t)s_shim_input};
  size_t    sizes[k_test_shim_one_region]      = {sizeof(s_shim_input)};
  const int rc                                 = ethosu_invoke_v3(drv,
                                                                  s_shim_cmd,
                                                                  (int)sizeof(s_shim_cmd),
                                                                  base_addrs,
                                                                  sizes,
                                                                  (int)k_test_shim_one_region,
                                                                  nullptr);
  TEST_ASSERT(rc != 0);
  TEST_END("ethosu invoke fails after the NPU is de-initialised");
}

int32_t main(void)
{
  /* test_reserve_reports_init_failure MUST run first: it needs the shim's
   * ready flag still clear so ethosu_reserve_driver() actually calls
   * ra8_npu_init(). test_invoke_rejected_after_deinit MUST run last: it leaves
   * the NPU de-initialised. */
  test_reserve_reports_init_failure();
  test_reserve_and_invoke_programs_registers();
  test_invoke_two_regions();
  test_reserve_idempotent_release_noop();
  test_invoke_rejects_bad_args();
  test_invoke_null_base_addr_guard_mcdc();
  test_invoke_reports_wait_fault();
  test_release_tolerates_null();
  test_invoke_rejected_after_deinit();
  (void)fprintf(stderr, "[OK ] test_ra8_ethosu_shim.c\n");
  return 0;
}
