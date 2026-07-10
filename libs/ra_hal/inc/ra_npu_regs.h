/**
 * @file ra_npu_regs.h
 * @brief Arm Ethos-U55 NPU register window on the Renesas RA8P1 (RA8P1-only)
 *
 * @details
 * The RA8P1 (R7KA8P1KFLCAC) integrates an Arm Ethos-U55 micro-NPU that the
 * RA8D2 does not have. This header exposes the CONFIRMED, primary-sourced facts
 * about that block so RA8P1 code can locate and gate it; the full Ethos-U55
 * command/queue interface is deliberately NOT hand-transcribed here (that would
 * be invented register fiction of the kind this project has been burned by
 * before -- see the RSIP history). The command interface is defined by the Arm
 * "Ethos-U55 NPU Technical Reference Manual" and the RA8P1 Hardware User's
 * Manual (R01UH1064EJ0130); wiring up a real driver is tracked as a follow-up.
 *
 * Confirmed facts (cross-validated across the RA8P1 FSP CMSIS device header
 * `R7KA8P1KF_core0.h` and the Zephyr `r7ka8p1xf.dtsi`, which agree exactly):
 *  - Register window base `0x40140000`, size `0x1000` (4 KiB).
 *  - Peripheral event (ELC/ICU) `NPU_IRQ` = `0x067`.
 *  - Module-stop bit: `MSTPCRA` bit 16 (see `ra8d2_mstp_regs.h`).
 *  - Clock: dedicated `NPUCLK` domain, `SCKDIVCR2[11:8]` (already modelled in
 *    `ra8d2_cgc_regs.h`), up to 500 MHz.
 *  - Compute: 256 8x8 MAC units (~256 GOPS), 8/16-bit quantized CNN + RNN.
 *  - The NPU has no dedicated CPU-visible SRAM: it works out of the shared
 *    on-chip system SRAM (`k_ra_mem_sram_base`) over AXI, plus a small internal
 *    SHRAM (8-48 KiB, not memory-mapped to the CPU).
 *
 * @note Compiled into RA8P1 targets only. It is guarded so an accidental
 *       include on an RA8D2 build fails loudly rather than pretending the NPU
 *       exists.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.2.0
 */

#pragma once

#include <stdint.h>

#include "ra_device.h"

#if !defined(RA_HAS_NPU)
#error "ra_npu_regs.h included on a device without an NPU (RA8P1-only)."
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra_npu_addr_t
 * @brief Ethos-U55 register-window base and extent on the RA8P1.
 *
 * @details Address constants for the NPU peripheral. `uintptr_t` so the base
 *          is a correct pointer on both the 32-bit target and the 64-bit test
 *          host.
 *
 * @invariant `k_ra_npu_base_addr` is 4 KiB aligned.
 * @since 0.2.0
 */
typedef enum : uintptr_t {
  k_ra_npu_base_addr = 0x40140000U, /**< R_NPU register window base (Ethos-U55). */
} ra_npu_addr_t;

/**
 * @enum ra_npu_layout_t
 * @brief Sizes and confirmed register offsets within the NPU window.
 *
 * @details Only the window extent and the Arm-architectural `NPU_ID` register
 *          (offset 0, present on every Ethos-U variant and safe to read for
 *          presence/revision detection once the block is clocked and out of
 *          module-stop) are named here. The command-stream, queue, and base-
 *          pointer registers are intentionally omitted until transcribed from
 *          the Ethos-U55 TRM + RA8P1 HUM.
 *
 * @invariant `k_ra_npu_window_bytes` covers the whole peripheral.
 * @since 0.2.0
 */
typedef enum : uint32_t {
  k_ra_npu_window_bytes = 0x1000U, /**< 4 KiB register window.                 */
  k_ra_npu_off_id = 0x0000U,       /**< NPU_ID (Arm Ethos-U arch id/revision). */
} ra_npu_layout_t;

/**
 * @enum ra_npu_event_t
 * @brief ICU/ELC event number for the NPU interrupt.
 *
 * @details Route this event to an NVIC line via the ICU IELSR registers the
 *          same way any other peripheral event is routed (see
 *          `ra8d2_icu_regs.h`). This number occupies a slot that is a reserved
 *          gap in the RA8D2 event space, so it does not collide with any
 *          shared event.
 *
 * @invariant Value is a valid `elc_event_t` index on the RA8P1.
 * @since 0.2.0
 */
typedef enum : uint16_t {
  k_ra_npu_event_irq = 0x067U, /**< NPU_IRQ event (RA8P1 elc_event_t). */
} ra_npu_event_t;

/**
 * @brief Return the base of the NPU register window as a volatile pointer.
 *
 * @details Provides the raw 32-bit register window so a future Ethos-U55 driver
 *          can index confirmed offsets (e.g. `k_ra_npu_off_id`) without a HUM
 *          citation living in this accessor (it performs no register access, it
 *          only computes the address). The caller must have released the NPU
 *          module-stop bit and enabled NPUCLK before dereferencing.
 *
 * @return Volatile pointer to word 0 of the 4 KiB NPU window.
 * @retval k_ra_npu_base_addr Always the fixed peripheral base (never NULL).
 *
 * @pre The build targets the RA8P1 (`RA_HAS_NPU` defined).
 * @pre NPU module-stop released and NPUCLK enabled before any dereference.
 * @post Returned pointer addresses the NPU window; no register was touched.
 * @post No side effects on hardware.
 *
 * @note Not thread-safe in the sense of hardware ownership: the caller owns
 *       serialisation of NPU access. The pointer computation itself is
 *       re-entrant.
 * @since 0.2.0
 */
static inline volatile uint32_t* ra_npu_window(void)
{
  return (volatile uint32_t*)k_ra_npu_base_addr;
}

#ifdef __cplusplus
}
#endif
