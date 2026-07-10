/**
 * @file ra_npu.h
 * @brief Arm Ethos-U55 NPU command/queue driver foundation (RA8P1-only)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Public API of the Ethos-U55 micro-NPU driver on the Renesas RA8P1
 * (R7KA8P1KFLCAC). This is the DRIVER FOUNDATION: it brings the NPU out of
 * module-stop, soft-resets the block, and models the Ethos-U55 command-stream
 * submission protocol -- program the command-queue base/size (`QBASE`/`QSIZE`),
 * program the per-tensor AXI region bases (`BASEPn`), kick the job
 * (`CMD.transition_to_running_state`), then poll or take the IRQ for completion
 * and read `STATUS` for faults.
 *
 * It is deliberately NOT a Vela compiler and NOT a TFLite-micro runtime: it
 * consumes a pre-compiled command stream (produced offline by Vela) plus the
 * input/output/scratch tensors already resident in shared system SRAM, and runs
 * one inference job. Building the command stream and the tensor-arena layout is
 * the caller's (or a future runtime's) responsibility -- see the follow-ups
 * filed against the RA8P1 epic.
 *
 * ## Sourcing / status
 *
 * The register interface is the Arm Ethos-U55 architectural APB map (see
 * `ra_npu_regs.h` for the primary-source transcription). There is no RA8P1
 * board yet, so the command/queue construction is host-tested for the exact
 * register write sequence only; a real on-silicon inference is a follow-up.
 *
 * ## Threading
 *
 * Not thread-safe. The NPU is a single-job engine; the caller owns
 * serialisation. Call from a single-threaded context or with the NPU IRQ
 * masked.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra_device.h"
#include "ra_err.h"
#include "ra_npu_regs.h"

#ifndef RA_HAS_NPU
#error "ra_npu.h included on a device without an NPU (RA8P1-only)."
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct ra_npu_job_t
 * @brief One Ethos-U55 inference job: a command stream plus its tensor arenas.
 *
 * @details
 * `cmd_stream` / `cmd_stream_bytes` describe the Vela-produced command stream
 * in SRAM that `ra_npu_submit()` points `QBASE`/`QSIZE` at. `region_base[i]` is
 * the AXI base address programmed into `BASEPi`, for the first `region_count`
 * regions the command stream references (region 0 is the weight/bias arena by
 * Vela convention). Unused entries are ignored.
 *
 * @invariant `cmd_stream != nullptr` and `cmd_stream_bytes > 0`.
 * @invariant `region_count <= k_ra_npu_region_count`.
 *
 * @par Example:
 * @code
 * ra_npu_job_t job = {};
 * job.cmd_stream       = g_vela_cmd_stream;
 * job.cmd_stream_bytes = sizeof(g_vela_cmd_stream);
 * job.region_count     = 3U;
 * job.region_base[k_ra_npu_region_0] = (uint64_t)(uintptr_t)g_weights;
 * job.region_base[k_ra_npu_region_1] = (uint64_t)(uintptr_t)g_scratch;
 * job.region_base[k_ra_npu_region_2] = (uint64_t)(uintptr_t)g_io_arena;
 * @endcode
 *
 * @see ra_npu_submit
 * @since 0.1.0
 */
typedef struct {
  const void* cmd_stream;                         /**< Vela command stream base in SRAM.         */
  uint32_t    cmd_stream_bytes;                   /**< Command stream length in bytes (> 0).     */
  uint8_t     region_count;                       /**< Number of `region_base[]` entries in use. */
  uint64_t    region_base[k_ra_npu_region_count]; /**< AXI base of each BASEPn arena.            */
} ra_npu_job_t;

/**
 * @struct ra_npu_status_t
 * @brief Decoded snapshot of the NPU_STATUS register.
 *
 * @details A friendly view of the raw STATUS word returned by
 *          `ra_npu_read_status()`. `raw` preserves the exact register value for
 *          logging; the booleans decode the bits the foundation cares about.
 *
 * @invariant `raw` is the verbatim STATUS register value at the read instant.
 * @see ra_npu_read_status
 * @since 0.1.0
 */
typedef struct {
  uint32_t raw;        /**< Verbatim NPU_STATUS register value.               */
  bool     running;    /**< STATUS.state: NPU is executing a command stream.  */
  bool     irq_raised; /**< STATUS.irq_raised: an interrupt is pending.       */
  bool     cmd_end;    /**< STATUS.cmd_end: command stream fully consumed.    */
  bool     fault;      /**< Any bus / parse / weight-decoder / ECC fault bit. */
} ra_npu_status_t;

/**
 * @brief Bring the NPU out of module-stop and soft-reset it.
 *
 * @details
 * Ungates the NPU via the ref-counted `ra_mstp` substrate (MSTPCRA bit 16 on the
 * RA8P1), then issues a soft reset and polls `STATUS.reset` until the block
 * reports ready or a bounded budget expires. NPUCLK is supplied by the CGC clock
 * tree configured at system-clock init; this driver assumes it is running.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok NPU clocked, reset complete, ready for a job.
 * @retval k_ra_err_hw_timeout Reset did not complete within the spin budget.
 * @retval k_ra_err_hw_error An MSTP read-back or NPU fault bit was observed.
 *
 * @pre The build targets the RA8P1 (`RA_HAS_NPU` defined).
 * @pre NPUCLK is running (CGC clock tree already configured).
 * @post On success the NPU is out of module-stop and idle (STATUS.state == 0).
 * @post On success the driver's internal initialized flag is set.
 *
 * @note Not thread-safe. Call from single-threaded init or with IRQs masked.
 * @see ra_npu_deinit
 * @see ra_npu_reset
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: 2 preconditions, 2 postconditions
 * - Rule 7: returns ra_err_t, marked [[nodiscard]]
 */
[[nodiscard]] ra_err_t ra_npu_init(void);

/**
 * @brief Gate the NPU back into module-stop.
 *
 * @details Clears the driver's initialized flag and releases the NPU MSTP
 *          reference. Any in-flight job should be stopped first; the foundation
 *          does not force-stop a running stream.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok NPU gated (or still referenced by another user).
 * @retval k_ra_err_not_initialized `ra_npu_init()` had not run.
 * @retval k_ra_err_hw_timeout MSTP bit did not read back as set within budget.
 *
 * @pre `ra_npu_init()` previously succeeded.
 * @pre No inference job is currently running.
 * @post On success the NPU MSTP reference is released.
 * @post The driver's initialized flag is clear.
 *
 * @note Not thread-safe.
 * @see ra_npu_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_npu_deinit(void);

/**
 * @brief Soft-reset an already-initialized NPU.
 *
 * @details Issues an NPU_RESET and polls `STATUS.reset` until the block is
 *          ready, returning the engine to the idle/stopped state. Use between
 *          jobs to clear a latched fault.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Reset complete; NPU idle.
 * @retval k_ra_err_not_initialized `ra_npu_init()` had not run.
 * @retval k_ra_err_hw_timeout Reset did not complete within the spin budget.
 *
 * @pre `ra_npu_init()` previously succeeded.
 * @pre No inference job result is still needed (reset discards NPU state).
 * @post On success STATUS.reset reads 0 (reset finished).
 * @post On success the NPU is stopped (STATUS.state == 0).
 *
 * @note Not thread-safe.
 * @see ra_npu_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_npu_reset(void);

/**
 * @brief Read the Ethos-U55 NPU_ID (architecture / product revision) register.
 *
 * @details Presence/revision probe. The value encodes the Arm architecture and
 *          product revision fields; a driver typically reads it once after
 *          `ra_npu_init()` to confirm the block responds.
 *
 * @param[out] out_id On success, the raw 32-bit NPU_ID value.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok `*out_id` holds the NPU_ID value.
 * @retval k_ra_err_null_ptr `out_id` was nullptr.
 * @retval k_ra_err_not_initialized `ra_npu_init()` had not run.
 *
 * @pre `out_id` is non-NULL.
 * @pre `ra_npu_init()` previously succeeded.
 * @post On success `*out_id` holds the register value.
 * @post No hardware state is modified.
 *
 * @note Not thread-safe.
 * @see ra_npu_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_npu_read_id(uint32_t* out_id);

/**
 * @brief Program the command queue and tensor region base pointers for a job.
 *
 * @details
 * Writes `QBASE` (64-bit, split lo/hi) to the job's command-stream address and
 * `QSIZE` to its byte length, then writes each in-use `BASEPn` pair to the
 * corresponding tensor-arena AXI base. Does NOT start the job -- call
 * `ra_npu_run()` afterwards. Programming is idempotent: re-submitting overwrites
 * the previous descriptor.
 *
 * @param[in] job Job descriptor (command stream + region bases). Not retained.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Queue and region pointers programmed.
 * @retval k_ra_err_null_ptr `job` or `job->cmd_stream` was nullptr.
 * @retval k_ra_err_invalid_size `job->cmd_stream_bytes` was 0.
 * @retval k_ra_err_invalid_arg `job->region_count` exceeds `k_ra_npu_region_count`.
 * @retval k_ra_err_not_initialized `ra_npu_init()` had not run.
 *
 * @pre `ra_npu_init()` previously succeeded.
 * @pre `job` describes a valid command stream resident in NPU-visible SRAM.
 * @post On success QBASE/QSIZE and the used BASEPn registers hold `job`'s values.
 * @post The NPU is armed but not yet running (no CMD write performed).
 *
 * @note Not thread-safe.
 * @see ra_npu_run
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: 2 preconditions, 2 postconditions
 * - Rule 7: returns ra_err_t, marked [[nodiscard]]
 */
[[nodiscard]] ra_err_t ra_npu_submit(const ra_npu_job_t* job);

/**
 * @brief Kick the currently-submitted job (transition the NPU to running).
 *
 * @details Sets `CMD.transition_to_running_state`, after which the NPU parses
 *          the command queue programmed by `ra_npu_submit()`. Completion is
 *          observed via `ra_npu_poll()` / `ra_npu_wait()` or the NPU IRQ.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Run request issued.
 * @retval k_ra_err_not_initialized `ra_npu_init()` had not run.
 * @retval k_ra_err_invalid_state No job has been submitted since init/reset.
 *
 * @pre `ra_npu_submit()` programmed a job since the last init/reset.
 * @pre `ra_npu_init()` previously succeeded.
 * @post On success CMD.transition_to_running_state has been written.
 * @post The NPU begins (or is about to begin) consuming the command queue.
 *
 * @note Not thread-safe.
 * @see ra_npu_submit
 * @see ra_npu_wait
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_npu_run(void);

/**
 * @brief Non-blocking completion / fault poll of a running job.
 *
 * @details Reads STATUS once. Reports completion when the command stream end has
 *          been reached, and reports a hardware fault when any bus / parse /
 *          weight-decoder / ECC fault bit is latched.
 *
 * @param[out] out_done On k_ra_ok, `true` when the job has finished, else `false`.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Poll succeeded; `*out_done` set accordingly.
 * @retval k_ra_err_null_ptr `out_done` was nullptr.
 * @retval k_ra_err_not_initialized `ra_npu_init()` had not run.
 * @retval k_ra_err_hw_error A STATUS fault bit is set.
 *
 * @pre `out_done` is non-NULL.
 * @pre `ra_npu_run()` has been issued for the job being polled.
 * @post On k_ra_ok `*out_done` reflects command-stream completion.
 * @post No hardware state is modified.
 *
 * @note Not thread-safe.
 * @see ra_npu_wait
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_npu_poll(bool* out_done);

/**
 * @brief Bounded busy-wait until the running job completes or faults.
 *
 * @details Spins `ra_npu_poll()` up to a fixed iteration budget (NASA Rule 2).
 *          Returns as soon as the job completes, a fault is latched, or the
 *          budget is exhausted.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Job completed (command stream fully consumed).
 * @retval k_ra_err_not_initialized `ra_npu_init()` had not run.
 * @retval k_ra_err_hw_error A STATUS fault bit was observed.
 * @retval k_ra_err_hw_timeout Job did not complete within the spin budget.
 *
 * @pre `ra_npu_run()` has been issued for the job being awaited.
 * @pre `ra_npu_init()` previously succeeded.
 * @post On k_ra_ok the command stream has been fully consumed.
 * @post No hardware state is modified beyond the NPU's own progress.
 *
 * @note Not thread-safe. Prefer the IRQ path for long-running inferences.
 * @see ra_npu_poll
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_npu_wait(void);

/**
 * @brief Read and decode the NPU_STATUS register.
 *
 * @param[out] out On success, the decoded status snapshot.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok `*out` holds the decoded status.
 * @retval k_ra_err_null_ptr `out` was nullptr.
 * @retval k_ra_err_not_initialized `ra_npu_init()` had not run.
 *
 * @pre `out` is non-NULL.
 * @pre `ra_npu_init()` previously succeeded.
 * @post On success `*out` reflects the live STATUS register.
 * @post No hardware state is modified.
 *
 * @note Not thread-safe.
 * @see ra_npu_status_t
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_npu_read_status(ra_npu_status_t* out);

/**
 * @brief Acknowledge (clear) a raised NPU interrupt.
 *
 * @details Writes `CMD.clear_irq`. Call from the NPU ISR (or after an IRQ-driven
 *          completion) so the line de-asserts and the next job can raise it.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Clear-IRQ request issued.
 * @retval k_ra_err_not_initialized `ra_npu_init()` had not run.
 *
 * @pre `ra_npu_init()` previously succeeded.
 * @pre An NPU interrupt was raised (or this is a defensive pre-clear).
 * @post On success CMD.clear_irq has been written.
 * @post STATUS.irq_raised de-asserts once the write takes effect.
 *
 * @note ISR-safe: performs a single register write, no logging on the hot path.
 * @see ra_npu_poll
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_npu_clear_irq(void);

#ifdef __cplusplus
}
#endif
