/**
 * @file examples/ra8p1_foundation/npu_smoke/main.c
 * @brief RA8P1 Arm Ethos-U55 NPU foundation smoke -- exercises the driver surface
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Companion to ``examples/ra8p1_foundation/blink_ra8p1`` for the NPU. This app
 * drives the full ``ra_npu`` command/queue surface -- bring the Ethos-U55 out of
 * module-stop (``ra_npu_init``), read the ``NPU_ID`` probe, program a trivial
 * command stream + one tensor region out of SRAM (``ra_npu_submit``), kick the
 * job (``ra_npu_run``), and read back ``STATUS`` (``ra_npu_read_status``).
 *
 * It is a BUILD-FOUNDATION app, NOT a hardware-validated one: there is no RA8P1
 * board yet, and the "command stream" here is a zeroed placeholder rather than a
 * real Vela-compiled program, so a real inference would not complete on silicon.
 * Its purpose is to prove the NPU driver compiles and links for the
 * R7KA8P1KFLCAC and to document the intended call sequence. The observability
 * globals below let a J-Link memprobe read the ID / status the driver captured.
 * Building a real command stream (Vela) and running an inference are follow-ups.
 *
 * @par Architectural ring
 * See docs/RING_AND_WORLD.md for what `[Ring 6 / APP] {World: S}` means --
 * application-layer code that runs in the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-07-09
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_device.h"
#include "ra_err.h"
#include "ra_npu.h"

/*
 * Compile-time proof that the RA8P1 toolchain selection reached this TU: the NPU
 * driver only exists when RA_HAS_NPU is defined (RA_DEVICE_RA8P1). Building this
 * app with the RA8D2 toolchain fails loudly here instead of silently dropping
 * the NPU.
 */
#ifndef RA_HAS_NPU
#error "npu_smoke must be built with cmake/toolchain-ra8p1.cmake (RA_DEVICE_RA8P1)."
#endif

/**
 * @enum npu_smoke_size_t
 * @brief Placeholder command-stream / tensor-arena sizes for the smoke job.
 */
typedef enum : uint32_t {
  k_npu_smoke_cmd_bytes   = 256U,  /**< Placeholder command-stream length. */
  k_npu_smoke_arena_bytes = 1024U, /**< Placeholder tensor-arena length.   */
} npu_smoke_size_t;

/**
 * @enum npu_smoke_region_t
 * @brief Number of tensor regions the smoke job programs.
 */
typedef enum : uint8_t {
  k_npu_smoke_region_count = 1U, /**< One arena (region 0). */
} npu_smoke_region_t;

/**
 * @var s_npu_cmd_stream
 * @brief Placeholder Vela command stream in SRAM (zeroed; not a real program).
 * @details Only its address/length are exercised by the driver here.
 * @note Not a valid inference program -- see the file header.
 * @since 0.1.0
 */
static uint8_t s_npu_cmd_stream[k_npu_smoke_cmd_bytes];

/**
 * @var s_npu_arena
 * @brief Placeholder tensor arena in SRAM for region 0.
 * @details Stands in for the weight/scratch/IO tensors a real job would place.
 * @note Contents are irrelevant to the foundation smoke.
 * @since 0.1.0
 */
static uint8_t s_npu_arena[k_npu_smoke_arena_bytes];

/**
 * @var g_npu_smoke_id
 * @brief NPU_ID captured after init, for external (J-Link) inspection.
 * @details `volatile` + non-static so a debugger can read it; firmware writes
 *          it once.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_npu_smoke_id = 0U;

/**
 * @var g_npu_smoke_status
 * @brief Raw NPU_STATUS captured after the run, for external inspection.
 * @details `volatile` + non-static so a debugger can watch it.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_npu_smoke_status = 0U;

/**
 * @brief Park the CPU forever in WFI -- used as a panic stop.
 */
static void npu_smoke_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  if (ra_npu_init() != k_ra_ok) {
    npu_smoke_panic_halt();
  }

  uint32_t id = 0U;
  if (ra_npu_read_id(&id) != k_ra_ok) {
    npu_smoke_panic_halt();
  }
  g_npu_smoke_id = id;

  ra_npu_job_t job                   = {};
  job.cmd_stream                     = s_npu_cmd_stream;
  job.cmd_stream_bytes               = k_npu_smoke_cmd_bytes;
  job.region_count                   = (uint8_t)k_npu_smoke_region_count;
  job.region_base[k_ra_npu_region_0] = (uint64_t)(uintptr_t)s_npu_arena;

  if (ra_npu_submit(&job) != k_ra_ok) {
    npu_smoke_panic_halt();
  }
  if (ra_npu_run() != k_ra_ok) {
    npu_smoke_panic_halt();
  }

  ra_npu_status_t st = {};
  if (ra_npu_read_status(&st) == k_ra_ok) {
    g_npu_smoke_status = st.raw;
  }

  npu_smoke_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
