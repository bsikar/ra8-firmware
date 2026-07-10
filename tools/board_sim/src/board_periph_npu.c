/**
 * @file board_periph_npu.c
 * @brief Arm Ethos-U55 NPU register-window model (RA8P1-only) for board_sim
 *
 * @details
 * The RA8P1 (R7KA8P1KFLCAC) integrates an Arm Ethos-U55 micro-NPU that the
 * RA8D2 does not have; it occupies a 4 KiB register window at 0x40140000 -- the
 * confirmed base + extent from libs/ra_hal/inc/ra_npu_regs.h (cross-sourced
 * there against the RA8P1 FSP CMSIS header and the Zephyr device tree). This
 * block gives that window an HONEST presence in the emulator: it is tagged
 * ::k_board_block_dev_ra8p1 so it is dispatched ONLY when board_sim runs the
 * RA8P1 profile (``--device ra8p1``); on the RA8D2 the window falls through to
 * the sparse fallback exactly as any reserved region would, so the RA8D2 run is
 * unchanged.
 *
 * ## Honest, deliberately-unimplemented model
 *
 * The Ethos-U55 command/queue interface (command-stream, base-pointer, and
 * status registers) is defined by the Arm "Ethos-U55 NPU Technical Reference
 * Manual" and the RA8P1 Hardware User's Manual (R01UH1064EJ0130); neither is
 * transcribed here. Per this project's honest-model rule -- an unmodelled
 * peripheral must fault or cleanly no-op and MUST NEVER fake success (the
 * ra_rsip "invented register" history is the cautionary tale) -- this block:
 *
 *  - maps the whole 4 KiB window so NPU-touching firmware does not read an
 *    unmodelled address as a spinning ready-bit toggle, but
 *  - returns a stable 0 for every read (a clear no-op: it never fabricates a
 *    ready/done bit a poll could "pass" by luck, and it does NOT invent an
 *    NPU_ID value -- that would require the Ethos-U55 TRM), and
 *  - tallies read/write counts and reports them at end-of-run, so a real NPU
 *    workload immediately sees "the NPU window was touched but is unmodelled"
 *    instead of a silent fake.
 *
 * A real command-stream model (enough to no-op an inference and assert the
 * NPU_IRQ completion event) is a board_sim follow-up on issue #222. The NPU
 * interrupt is ELC/ICU event 0x067 (::k_npu_event_irq); routing is already
 * generic in the core (board_periph_icu_raise_event accepts any event number),
 * so no faked completion is asserted here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.2.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"

/**
 * @brief Ethos-U55 NPU register-window geometry (mirrors ra_npu_regs.h).
 *
 * @details Base, 4 KiB extent, and the NPU_ID offset are the confirmed,
 *          primary-sourced constants from libs/ra_hal/inc/ra_npu_regs.h
 *          (``k_ra_npu_base_addr`` / ``k_ra_npu_window_bytes`` /
 *          ``k_ra_npu_off_id``). They are restated here rather than #included
 *          because ra_npu_regs.h is guarded to RA8P1 firmware builds (it
 *          ``#error``s without ``RA_HAS_NPU``) and board_sim is a host build.
 *
 * @invariant ::k_npu_span covers the whole peripheral window.
 * @since 0.2.0
 */
typedef enum : uint64_t {
  k_npu_base   = 0x40140000UL, /**< R_NPU register-window base (Ethos-U55). */
  k_npu_span   = 0x1000UL,     /**< 4 KiB register window.                  */
  k_npu_off_id = 0x0000UL,     /**< NPU_ID (Arm arch id/revision) offset.   */
} npu_map_t;

/**
 * @brief NPU interrupt event number and this block's tick/report order slot.
 *
 * @details ::k_npu_event_irq mirrors ra_npu_regs.h ``k_ra_npu_event_irq``; the
 *          core's ICU already routes any event number, so it is reported (not
 *          asserted) here. ::k_npu_block_order sits between GPIO (10) and the
 *          timers (20) for a stable report position.
 * @since 0.2.0
 */
typedef enum : uint32_t {
  k_npu_event_irq   = 0x067U, /**< NPU_IRQ ELC/ICU event (RA8P1 elc_event_t). */
  k_npu_block_order = 15U,    /**< Report/tick order slot (relative only).    */
} npu_misc_t;

/**
 * @brief NPU window access tally -- the honest "touched but unmodelled" record.
 *
 * @invariant Both counters are monotonic within a run; ::npu_reset zeroes them.
 * @since 0.2.0
 */
typedef struct {
  uint32_t reads;  /**< Reads observed inside the NPU window.  */
  uint32_t writes; /**< Writes observed inside the NPU window. */
} npu_state_t;

static npu_state_t s_npu;

/** @brief MMIO read inside the NPU window -- stable 0 (unmodelled, never faked). */
static uint64_t npu_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)addr; /* Every offset, NPU_ID (k_npu_off_id) included, reads 0. */
  (void)size;
  s_npu.reads++;
  return 0U;
}

/** @brief MMIO write inside the NPU window -- recorded, no modelled side effect. */
static void npu_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)addr;
  (void)size;
  (void)value;
  s_npu.writes++;
}

/** @brief Reset the NPU access tally to power-on state. */
static void npu_reset(void)
{
  s_npu.reads  = 0U;
  s_npu.writes = 0U;
}

/** @brief End-of-run NPU section: access tally + unmodelled-window notice. */
static void npu_report(void)
{
  if ((s_npu.reads == 0U) && (s_npu.writes == 0U)) {
    return; /* Untouched (e.g. blink_ra8p1): stay quiet. */
  }
  (void)fprintf(stderr,
                "  NPU(Ethos-U55): window touched r=%u w=%u -- MAPPED BUT UNMODELLED"
                " (no inference; IRQ event 0x%03X unrouted)\n",
                s_npu.reads,
                s_npu.writes,
                (unsigned)k_npu_event_irq);
}

/** @brief NPU block descriptor (RA8P1-only; self-registered with the core). */
static const board_periph_block_t k_npu_block = {
  .base   = (uint64_t)k_npu_base,
  .span   = (uint64_t)k_npu_span,
  .order  = (uint32_t)k_npu_block_order,
  .read   = npu_read,
  .write  = npu_write,
  .tick   = nullptr,
  .reset  = npu_reset,
  .report = npu_report,
  .name   = "NPU",
  .device = k_board_block_dev_ra8p1,
};

/** @brief Register the NPU block before main (host constructor). */
[[gnu::constructor]] static void npu_block_register(void)
{
  board_periph_register_block(&k_npu_block);
}
