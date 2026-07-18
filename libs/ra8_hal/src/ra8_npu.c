/**
 * @file ra8_npu.c
 * @brief Arm Ethos-U55 NPU command/queue driver foundation (RA8P1-only)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Implementation of the `ra8_npu.h` contract. The whole translation unit is
 * gated behind `RA8_HAS_NPU` so it compiles to nothing on the RA8D2 (which has
 * no NPU) and is only built for the RA8P1 (`-DRA8_DEVICE_RA8P1`). Every MMIO
 * access carries an Arm Ethos-U55 NPU TRM citation; the RA8P1 integration facts
 * (module-stop bit, clock, IRQ) are sourced in `ra8_npu_regs.h`.
 *
 * The command-stream model implemented here is the Ethos-U55 submission
 * protocol: program `QBASE`/`QSIZE` at the Vela command stream, program each
 * `BASEPn` tensor-arena base, then write `CMD.transition_to_running_state` to
 * run; completion and faults are read from `STATUS`. There is no RA8P1 board
 * yet, so this is host-tested for the register write sequence only -- a real
 * on-silicon inference is a follow-up.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_attributes.h"
#include "ra8_device.h"

#ifdef RA8_HAS_NPU

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_npu.h"
#include "ra8_npu_regs.h"

/**
 * @var s_tag
 * @brief Log component tag for this driver.
 * @details Passed to the `ra8_log_*` / `RA8_*` macros as the source tag.
 * @note Read-only literal; never modified.
 * @since 0.1.0
 */
static const char* s_tag = "NPU";

/**
 * @enum ra8_npu_mstp_enc_t
 * @brief Packing of the NPU module-stop identifier for `ra8_mstp_enable()`.
 *
 * @details `ra8_mstp_t` packs a peripheral as `(register_index << 8) | bit`. The
 *          RA8P1 NPU module-stop is MSTPCRA (`k_ra8_mstp_reg_a`) bit 16 -- a
 *          confirmed RA8P1 constant (see `ra8_npu_regs.h`). It is not a named
 *          member of the shared RA8D2 `ra8_mstp_t` enum (the RA8D2 has no NPU), so
 *          it is composed here from these fields and cast to `ra8_mstp_t`.
 *
 * @invariant `k_ra8_npu_mstp_bit` is a valid 0..31 bit index.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_npu_mstp_reg_shift = 8U,  /**< (register_index << 8) | bit packing. */
  k_ra8_npu_mstp_bit       = 16U, /**< NPU module-stop = MSTPCRA bit 16.    */
} ra8_npu_mstp_enc_t;

/**
 * @enum ra8_npu_budget_t
 * @brief Bounded busy-spin budgets (NASA Power of 10 Rule 2).
 *
 * @details Fixed upper bounds so every poll loop terminates. On silicon a soft
 *          reset settles in a handful of NPUCLK cycles and an inference in
 *          microseconds-to-milliseconds; these caps are generous ceilings that
 *          only bound the pathological no-response case.
 *
 * @invariant Both values are non-zero.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_reset_poll_iters = 100000U,  /**< STATUS.reset settle budget. */
  k_ra8_npu_job_poll_iters   = 2000000U, /**< Job-completion poll budget. */
} ra8_npu_budget_t;

/**
 * @enum ra8_npu_split_t
 * @brief Bit shift for splitting a 64-bit AXI address into lo/hi words.
 *
 * @details Ethos-U55 QBASE / BASEPn are 64-bit registers written as two 32-bit
 *          words; the high word is the address shifted right by this amount.
 *
 * @invariant Equal to the width of a 32-bit register in bits.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_npu_addr_hi_shift = 32U, /**< uint64 address -> high 32 bits. */
} ra8_npu_split_t;

/**
 * @enum ra8_npu_mask_t
 * @brief STATUS fault mask: any latched hardware-fault condition.
 *
 * @details OR of the bus-error, command-parse-error, weight-decoder-fault and
 *          internal-SRAM ECC-fault bits. A single mask keeps fault detection a
 *          one-condition test (no compound decision, hence no MC/DC obligation).
 *
 * @invariant Covers exactly the STATUS bits the driver treats as fatal.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_status_fault_mask =
    ((uint32_t)1U << k_ra8_npu_status_bus_error_bit) |
    ((uint32_t)1U << k_ra8_npu_status_cmd_parse_bit) |
    ((uint32_t)1U << k_ra8_npu_status_wd_fault_bit) |
    ((uint32_t)1U << k_ra8_npu_status_ecc_fault_bit), /**< RA8 npu status fault mask. */
} ra8_npu_mask_t;

/**
 * @var s_npu_mstp_id
 * @brief Module-stop identifier for the NPU, consumed by `ra8_mstp_enable()`.
 * @details Composed from `k_ra8_mstp_reg_a` (MSTPCRA) and `k_ra8_npu_mstp_bit`.
 * @note Compile-time constant; never modified.
 * @since 0.1.0
 */
static const ra8_mstp_t s_npu_mstp_id =
  (ra8_mstp_t)(((uint16_t)k_ra8_mstp_reg_a << k_ra8_npu_mstp_reg_shift) | k_ra8_npu_mstp_bit);

/**
 * @var s_npu_initialized
 * @brief True once `ra8_npu_init()` has ungated and reset the NPU.
 * @details Gates every operation that touches NPU registers.
 * @warning Do not modify directly; owned by `ra8_npu_init()`/`ra8_npu_deinit()`.
 * @since 0.1.0
 */
static bool s_npu_initialized = false;

/**
 * @var s_npu_job_submitted
 * @brief True between a successful `ra8_npu_submit()` and the next init/reset.
 * @details Guards `ra8_npu_run()` from kicking with no queue programmed.
 * @warning Do not modify directly; owned by submit / run-gating logic.
 * @since 0.1.0
 */
static bool s_npu_job_submitted = false;

/**
 * @enum ra8_npu_irq_state_t
 * @brief Interrupt-driven completion latch shared between the ISR and waiter.
 *
 * @details `ra8_npu_irq_arm()` sets `armed`; `ra8_npu_irq_handler()` advances it
 *          to `done` or `fault` from the observed STATUS; `ra8_npu_wait_irq()`
 *          polls it. `idle` means no IRQ-driven job is in flight.
 *
 * @invariant Only these four states are ever stored.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_npu_irq_idle  = 0U, /**< No interrupt-driven job armed.   */
  k_ra8_npu_irq_armed = 1U, /**< Armed and waiting for the IRQ.   */
  k_ra8_npu_irq_done  = 2U, /**< ISR observed STATUS.cmd_end.     */
  k_ra8_npu_irq_fault = 3U, /**< ISR observed a STATUS fault bit. */
} ra8_npu_irq_state_t;

/**
 * @var s_npu_irq_state
 * @brief Completion latch driven by `ra8_npu_irq_handler()` from the NPU ISR.
 * @details `volatile` because it crosses the ISR / thread boundary; owned by the
 *          arm / handler / wait logic.
 * @warning Do not modify directly outside the IRQ helpers.
 * @since 0.1.0
 */
static volatile ra8_npu_irq_state_t s_npu_irq_state = k_ra8_npu_irq_idle;

/**
 * @brief Sleep the core until the next interrupt (target), or nothing (host).
 *
 * @details Emits a `WFI` on an Arm target so `ra8_npu_wait_irq()` idles the CPU
 *          between latch checks. On the x86_64 unit-test host (`__ARM_ARCH`
 *          undefined) it compiles to nothing and the test drives the ISR by hand.
 *
 * @return None (void).
 * @retval None This function returns no value.
 *
 * @pre None (safe to call in any state).
 * @pre The caller is in a bounded wait loop.
 * @post On the target the core has slept until an interrupt (or returned at once).
 * @post No state is modified.
 *
 * @note ISR-safe / re-entrant; touches no shared state.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline void internal_npu_wait_for_irq(void)
{
#ifdef __ARM_ARCH
  __asm__ volatile("wfi");
#endif
}

/**
 * @brief Issue a soft reset and poll STATUS.reset until the NPU is ready.
 *
 * @details Writes NPU_RESET requesting the privileged+secure level (both pending
 *          bits clear) then busy-spins on STATUS.reset for a bounded budget.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Reset completed (STATUS.reset read back 0).
 * @retval k_ra8_err_hw_timeout Reset did not clear within the spin budget.
 *
 * @pre The NPU is out of module-stop (MSTP released) and NPUCLK is running.
 * @pre Caller serialises NPU access (single-threaded / IRQ-masked).
 * @post On success STATUS.reset reads 0.
 * @post No other NPU register is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_npu_apply_reset(void)
{
  /* Ethos-U55 NPU TRM "NPU_RESET" reg @ 0x0C -- request reset, privileged +
   * secure pending level (both pending-level bits written 0). */
  *ra8_npu_reg(k_ra8_npu_off_reset) = 0U;
  for (uint32_t i = 0U; i < k_ra8_npu_reset_poll_iters; i++) { /* GCOVR_EXCL_BR_LINE */
    /* Ethos-U55 NPU TRM "NPU_STATUS" reg @ 0x04 -- reset bit clears when done. */
    const uint32_t status = *ra8_npu_reg(k_ra8_npu_off_status);
    if ((status & ((uint32_t)1U << k_ra8_npu_status_reset_bit)) == 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Write one BASEPn tensor-region base-pointer pair (lo + hi words).
 *
 * @details Computes the region's lo/hi register offsets from the region index
 *          and stride, then writes the 64-bit AXI base as two 32-bit words.
 *
 * @param[in] idx  Region index (0 .. `k_ra8_npu_region_count` - 1).
 * @param[in] base AXI base address of the tensor arena for this region.
 *
 * @return None (void).
 * @retval None This function returns no value.
 *
 * @pre `idx` is a valid region index (caller-validated).
 * @pre The NPU is out of module-stop and NPUCLK is running.
 * @post BASEP`idx` (lo+hi) holds `base`.
 * @post No other NPU register is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_npu_write_region(ra8_npu_region_idx_t idx, uint64_t base)
{
  const uint32_t lo_off =
    (uint32_t)k_ra8_npu_off_basep0_lo + ((uint32_t)idx * (uint32_t)k_ra8_npu_basep_stride_bytes);
  const uint32_t hi_off = lo_off + (uint32_t)k_ra8_npu_reg_hi_offset;
  /* Ethos-U55 NPU TRM "BASEPn" regs @ 0x80 + n*8 -- tensor arena AXI base. */
  *ra8_npu_reg(lo_off) = (uint32_t)base;
  *ra8_npu_reg(hi_off) = (uint32_t)(base >> k_ra8_npu_addr_hi_shift);
}

ra8_err_t ra8_npu_init(void)
{
  const ra8_err_t mst = ra8_mstp_enable(s_npu_mstp_id);
  RA8_RETURN_ON_ERROR(mst, s_tag, "npu_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */
  const ra8_err_t rst = internal_npu_apply_reset();
  RA8_RETURN_ON_ERROR(rst, s_tag, "npu_init: reset"); /* GCOVR_EXCL_BR_LINE */
  s_npu_initialized   = true;
  s_npu_job_submitted = false;
  s_npu_irq_state     = k_ra8_npu_irq_idle;
  ra8_log_info(s_tag, "npu_init");
  return k_ra8_ok;
}

ra8_err_t ra8_npu_deinit(void)
{
  RA8_VALIDATE_INIT(s_npu_initialized, s_tag, "npu_deinit: not initialized");
  const ra8_err_t dis = ra8_mstp_disable(s_npu_mstp_id);
  RA8_RETURN_ON_ERROR(dis, s_tag, "npu_deinit: mstp disable"); /* GCOVR_EXCL_BR_LINE */
  s_npu_initialized   = false;
  s_npu_job_submitted = false;
  s_npu_irq_state     = k_ra8_npu_irq_idle;
  return k_ra8_ok;
}

ra8_err_t ra8_npu_reset(void)
{
  RA8_VALIDATE_INIT(s_npu_initialized, s_tag, "npu_reset: not initialized");
  const ra8_err_t rst = internal_npu_apply_reset();
  RA8_RETURN_ON_ERROR(rst, s_tag, "npu_reset"); /* GCOVR_EXCL_BR_LINE */
  s_npu_job_submitted = false;
  s_npu_irq_state     = k_ra8_npu_irq_idle;
  return k_ra8_ok;
}

ra8_err_t ra8_npu_read_id(uint32_t* out_id)
{
  RA8_CHECK_NULL_PTR(out_id, s_tag, "out_id must not be nullptr");
  RA8_VALIDATE_INIT(s_npu_initialized, s_tag, "npu_read_id: not initialized");
  /* Ethos-U55 NPU TRM "NPU_ID" reg @ 0x00 -- architecture / product id. */
  *out_id = *ra8_npu_reg(k_ra8_npu_off_id);
  return k_ra8_ok;
}

ra8_err_t ra8_npu_submit(const ra8_npu_job_t* job)
{
  RA8_CHECK_NULL_PTR(job, s_tag, "job must not be nullptr");
  RA8_CHECK_NULL_PTR(job->cmd_stream, s_tag, "job->cmd_stream must not be nullptr");
  RA8_VALIDATE_INIT(s_npu_initialized, s_tag, "npu_submit: not initialized");
  if (job->cmd_stream_bytes == 0U) {
    ra8_log_error(s_tag, "npu_submit: empty command stream");
    return k_ra8_err_invalid_size;
  }
  if (job->region_count > (uint8_t)k_ra8_npu_region_count) {
    ra8_log_error(s_tag, "npu_submit: too many regions");
    return k_ra8_err_invalid_arg;
  }
  const uint64_t qbase = (uint64_t)(uintptr_t)job->cmd_stream;
  /* Ethos-U55 NPU TRM "QBASE" regs @ 0x10 / 0x14 -- command-stream address. */
  *ra8_npu_reg(k_ra8_npu_off_qbase_lo) = (uint32_t)qbase;
  *ra8_npu_reg(k_ra8_npu_off_qbase_hi) = (uint32_t)(qbase >> k_ra8_npu_addr_hi_shift);
  /* Ethos-U55 NPU TRM "QSIZE" reg @ 0x20 -- command-stream length in bytes. */
  *ra8_npu_reg(k_ra8_npu_off_qsize) = job->cmd_stream_bytes;
  for (uint8_t r = 0U; r < job->region_count; r++) {
    internal_npu_write_region((ra8_npu_region_idx_t)r, job->region_base[r]);
  }
  s_npu_job_submitted = true;
  return k_ra8_ok;
}

ra8_err_t ra8_npu_run(void)
{
  RA8_VALIDATE_INIT(s_npu_initialized, s_tag, "npu_run: not initialized");
  if (!s_npu_job_submitted) {
    ra8_log_error(s_tag, "npu_run: no job submitted");
    return k_ra8_err_invalid_state;
  }
  /* Ethos-U55 NPU TRM "NPU_CMD" reg @ 0x08 -- transition_to_running_state. */
  *ra8_npu_reg(k_ra8_npu_off_cmd) = ((uint32_t)1U << k_ra8_npu_cmd_run_bit);
  return k_ra8_ok;
}

ra8_err_t ra8_npu_read_status(ra8_npu_status_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA8_VALIDATE_INIT(s_npu_initialized, s_tag, "npu_read_status: not initialized");
  /* Ethos-U55 NPU TRM "NPU_STATUS" reg @ 0x04 -- state + fault flags. */
  const uint32_t raw = *ra8_npu_reg(k_ra8_npu_off_status);
  out->raw           = raw;
  out->running       = (raw & ((uint32_t)1U << k_ra8_npu_status_state_bit)) != 0U;
  out->irq_raised    = (raw & ((uint32_t)1U << k_ra8_npu_status_irq_raised_bit)) != 0U;
  out->cmd_end       = (raw & ((uint32_t)1U << k_ra8_npu_status_cmd_end_bit)) != 0U;
  out->fault         = (raw & (uint32_t)k_ra8_npu_status_fault_mask) != 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_npu_poll(bool* out_done)
{
  RA8_CHECK_NULL_PTR(out_done, s_tag, "out_done must not be nullptr");
  RA8_VALIDATE_INIT(s_npu_initialized, s_tag, "npu_poll: not initialized");
  /* Ethos-U55 NPU TRM "NPU_STATUS" reg @ 0x04 -- fault + cmd-end flags. */
  const uint32_t raw = *ra8_npu_reg(k_ra8_npu_off_status);
  if ((raw & (uint32_t)k_ra8_npu_status_fault_mask) != 0U) {
    ra8_log_error(s_tag, "npu_poll: fault latched");
    return k_ra8_err_hw_error;
  }
  *out_done = (raw & ((uint32_t)1U << k_ra8_npu_status_cmd_end_bit)) != 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_npu_wait(void)
{
  RA8_VALIDATE_INIT(s_npu_initialized, s_tag, "npu_wait: not initialized");
  for (uint32_t i = 0U; i < k_ra8_npu_job_poll_iters; i++) { /* GCOVR_EXCL_BR_LINE */
    bool            done = false;
    const ra8_err_t err  = ra8_npu_poll(&done);
    RA8_RETURN_ON_ERROR(err, s_tag, "npu_wait: poll"); /* GCOVR_EXCL_BR_LINE */
    if (done) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

ra8_err_t ra8_npu_clear_irq(void)
{
  RA8_VALIDATE_INIT(s_npu_initialized, s_tag, "npu_clear_irq: not initialized");
  /* Ethos-U55 NPU TRM "NPU_CMD" reg @ 0x08 -- write 1 to clear_irq. */
  *ra8_npu_reg(k_ra8_npu_off_cmd) = ((uint32_t)1U << k_ra8_npu_cmd_clear_irq_bit);
  return k_ra8_ok;
}

ra8_err_t ra8_npu_irq_arm(void)
{
  RA8_VALIDATE_INIT(s_npu_initialized, s_tag, "npu_irq_arm: not initialized");
  if (!s_npu_job_submitted) {
    ra8_log_error(s_tag, "npu_irq_arm: no job submitted");
    return k_ra8_err_invalid_state;
  }
  s_npu_irq_state = k_ra8_npu_irq_armed;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_npu_irq_handler(void* ctx)
{
  (void)ctx;
  /* Delegate register access to the already-cited status/clear accessors so the
   * ISR itself adds no new MMIO citation and stays a two-op hot path. */
  ra8_npu_status_t st = {};
  if (ra8_npu_read_status(&st) == k_ra8_ok) {
    if (st.fault) {
      s_npu_irq_state = k_ra8_npu_irq_fault;
    } else if (st.cmd_end) {
      s_npu_irq_state = k_ra8_npu_irq_done;
    } else {
      /* Spurious / early IRQ with neither terminal bit set: leave the latch
       * armed and re-wait; the clear below still de-asserts the line. */
    }
  }
  (void)ra8_npu_clear_irq();
}

ra8_err_t ra8_npu_wait_irq(void)
{
  RA8_VALIDATE_INIT(s_npu_initialized, s_tag, "npu_wait_irq: not initialized");
  if (s_npu_irq_state == k_ra8_npu_irq_idle) {
    ra8_log_error(s_tag, "npu_wait_irq: not armed");
    return k_ra8_err_invalid_state;
  }
  for (uint32_t i = 0U; i < k_ra8_npu_job_poll_iters; i++) { /* GCOVR_EXCL_BR_LINE */
    const ra8_npu_irq_state_t state = s_npu_irq_state;
    if (state == k_ra8_npu_irq_done) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
    if (state == k_ra8_npu_irq_fault) { /* GCOVR_EXCL_BR_LINE */
      ra8_log_error(s_tag, "npu_wait_irq: fault latched");
      return k_ra8_err_hw_error;
    }
    internal_npu_wait_for_irq();
  }
  return k_ra8_err_hw_timeout;
}

#else
/* RA8D2 (no NPU): this translation unit is intentionally empty. */
typedef int ra8_npu_not_on_this_device_t;
#endif /* RA8_HAS_NPU */
