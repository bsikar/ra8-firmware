/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_npu_regs.h
 * @brief Arm Ethos-U55 NPU register window on the Renesas RA8P1 (RA8P1-only)
 * @ingroup grp_hal_system
 *
 * @details
 * The RA8P1 (R7KA8P1KFLCAC) integrates an Arm Ethos-U55 micro-NPU that the
 * RA8D2 does not have. This header exposes the register window base, the command
 * / queue / base-pointer interface offsets, and the CMD / STATUS / RESET field
 * bits so `ra8_npu.c` can drive an inference job out of shared system SRAM.
 *
 * ## Sourcing (no invented registers)
 *
 * The window base, size, interrupt event, module-stop bit, and clock domain are
 * CONFIRMED constants cross-validated across the RA8P1 FSP CMSIS device header
 * `R7KA8P1KF_core0.h` and the Zephyr `r7ka8p1xf.dtsi`, which agree exactly. The
 * register OFFSETS and field-bit layout below are the Arm Ethos-U55
 * architectural APB interface, transcribed from the "Arm Ethos-U55 NPU Technical
 * Reference Manual" register map and cross-checked byte-for-byte against Arm's
 * open `ethos-u-core-driver` `ethosu_interface.h` (both REFERENCE-ONLY: the
 * offsets are hardware facts, the prose and enums here are hand-authored under
 * this project's style). This is deliberately NOT the RSIP-style invented-
 * register fiction the project has been burned by before -- every offset traces
 * to the published Ethos-U55 interface. On-silicon behaviour is UNPROVEN (there
 * is no RA8P1 board yet); the driver foundation is host-tested for the register
 * write sequence only, and a real inference is a follow-up.
 *
 * Confirmed integration facts:
 *  - Register window base `0x40140000`, size `0x1000` (4 KiB).
 *  - Peripheral event (ELC/ICU) `NPU_IRQ` = `0x067`.
 *  - Module-stop bit: `MSTPCRA` bit 16 (see `ra8_mstp_regs.h`).
 *  - Clock: dedicated `NPUCLK` domain, `SCKDIVCR2[11:8]` (already modelled in
 *    `ra8_cgc_regs.h`), up to 500 MHz.
 *  - Compute: 256 8x8 MAC units (~256 GOPS), 8/16-bit quantized CNN + RNN.
 *  - The NPU has no dedicated CPU-visible SRAM: it works out of the shared
 *    on-chip system SRAM (`k_ra8_mem_sram_base`) over AXI, plus a small internal
 *    SHRAM (8-48 KiB, not memory-mapped to the CPU).
 *
 * @note Compiled into RA8P1 targets only. It is guarded so an accidental
 *       include on an RA8D2 build fails loudly rather than pretending the NPU
 *       exists.
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_device.h"

#ifndef RA8_HAS_NPU
#error "ra8_npu_regs.h included on a device without an NPU (RA8P1-only)."
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_npu_addr_t
 * @brief Ethos-U55 register-window base and extent on the RA8P1.
 *
 * @details Address constants for the NPU peripheral. `uintptr_t` so the base
 *          is a correct pointer on both the 32-bit target and the 64-bit test
 *          host.
 *
 * @invariant `k_ra8_npu_base_addr` is 4 KiB aligned.
 * @see ra8_npu_reg
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_ra8_npu_base_addr = 0x40140000U, /**< R_NPU register window base (Ethos-U55). */
} ra8_npu_addr_t;

/**
 * @enum ra8_npu_layout_t
 * @brief Byte offsets of the Ethos-U55 APB registers within the window.
 *
 * @details Word-aligned byte offsets into the 4 KiB window, transcribed from the
 *          Ethos-U55 TRM register map (see the file header for sourcing). The
 *          command queue is the CPU->NPU submission channel: `QBASE`
 *          (lo/hi 64-bit) points at the Vela command stream in SRAM and `QSIZE`
 *          gives its byte length. The eight base-pointer regions (see
 *          `k_ra8_npu_off_basep0_lo` and `ra8_npu_region_idx_t`) supply the AXI
 *          base address of each tensor arena the command stream references.
 *
 * @invariant Every offset is < `k_ra8_npu_window_bytes` and 4-byte aligned.
 * @see ra8_npu_reg
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_window_bytes  = 0x1000U, /**< 4 KiB register window.                  */
  k_ra8_npu_off_id        = 0x0000U, /**< NPU_ID (Arm Ethos-U arch id/revision).  */
  k_ra8_npu_off_status    = 0x0004U, /**< NPU_STATUS (state + fault flags, RO).   */
  k_ra8_npu_off_cmd       = 0x0008U, /**< NPU_CMD (run / clear-irq / q-enables).  */
  k_ra8_npu_off_reset     = 0x000CU, /**< NPU_RESET (software reset request).     */
  k_ra8_npu_off_qbase_lo  = 0x0010U, /**< QBASE bits [31:0] (cmd-stream address). */
  k_ra8_npu_off_qbase_hi  = 0x0014U, /**< QBASE bits [63:32].                     */
  k_ra8_npu_off_qread     = 0x0018U, /**< QREAD (cmd-stream read offset, RO).     */
  k_ra8_npu_off_qconfig   = 0x001CU, /**< QCONFIG (cmd-stream AXI region config). */
  k_ra8_npu_off_qsize     = 0x0020U, /**< QSIZE (cmd-stream length in bytes).     */
  k_ra8_npu_off_prot      = 0x0024U, /**< NPU_PROT (privilege/security, RO).      */
  k_ra8_npu_off_config    = 0x0028U, /**< NPU_CONFIG (build-time macs/rev, RO).   */
  k_ra8_npu_off_regioncfg = 0x003CU, /**< REGIONCFG (per-region AXI attributes).  */
  k_ra8_npu_off_basep0_lo = 0x0080U, /**< BASEP0 bits [31:0] (region 0 base).     */
} ra8_npu_layout_t;

/**
 * @enum ra8_npu_region_idx_t
 * @brief Base-pointer region indices (Ethos-U55 has eight AXI region bases).
 *
 * @details The Vela command stream addresses tensors by region index; the CPU
 *          programs the actual AXI base of each region into the BASEPn register
 *          pair before running a job. `BASEPn_lo` sits at
 *          `k_ra8_npu_off_basep0_lo + n * k_ra8_npu_basep_stride_bytes`, and its
 *          high word `k_ra8_npu_reg_hi_offset` bytes above that. Region 0 is the
 *          read-only weight/bias arena by Vela convention; other regions hold
 *          the scratch, input, and output tensors.
 *
 * @invariant `k_ra8_npu_region_count` equals the number of BASEPn register pairs.
 * @see ra8_npu_geom_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_npu_region_0     = 0U, /**< AXI region 0 (weights/biases by convention). */
  k_ra8_npu_region_1     = 1U, /**< AXI region 1.                                */
  k_ra8_npu_region_2     = 2U, /**< AXI region 2.                                */
  k_ra8_npu_region_3     = 3U, /**< AXI region 3.                                */
  k_ra8_npu_region_4     = 4U, /**< AXI region 4.                                */
  k_ra8_npu_region_5     = 5U, /**< AXI region 5.                                */
  k_ra8_npu_region_6     = 6U, /**< AXI region 6.                                */
  k_ra8_npu_region_7     = 7U, /**< AXI region 7.                                */
  k_ra8_npu_region_count = 8U, /**< Number of BASEPn region base-pointer pairs.  */
} ra8_npu_region_idx_t;

/**
 * @enum ra8_npu_geom_t
 * @brief Geometry of the 64-bit register pairs (QBASE, BASEPn).
 *
 * @details Ethos-U55 addresses are 64-bit and split across two consecutive
 *          32-bit words: a low word then a high word `k_ra8_npu_reg_hi_offset`
 *          bytes above it. Consecutive BASEPn pairs are
 *          `k_ra8_npu_basep_stride_bytes` apart.
 *
 * @invariant `k_ra8_npu_basep_stride_bytes == 2 * k_ra8_npu_reg_hi_offset`.
 * @see ra8_npu_region_idx_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_reg_hi_offset      = 0x0004U, /**< Low->high word gap in a 64-bit reg. */
  k_ra8_npu_basep_stride_bytes = 0x0008U, /**< Bytes between BASEPn pairs (lo+hi). */
} ra8_npu_geom_t;

/**
 * @enum ra8_npu_cmd_bit_t
 * @brief Bit positions in the NPU_CMD register.
 *
 * @details Writing 1 to `k_ra8_npu_cmd_run_bit` transitions the NPU into the
 *          running state and it begins parsing the command queue. Writing 1 to
 *          `k_ra8_npu_cmd_clear_irq_bit` acknowledges (clears) a raised IRQ. The
 *          clock/power Q-enable bits gate the internal power/clock quiescence
 *          handshakes and are held enabled while a job runs.
 *
 * @invariant Each value is a valid 0-based bit index in a 32-bit register.
 * @see ra8_npu_status_bit_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_npu_cmd_run_bit       = 0U, /**< transition_to_running_state (start job). */
  k_ra8_npu_cmd_clear_irq_bit = 1U, /**< clear_irq (write 1 to acknowledge IRQ).  */
  k_ra8_npu_cmd_clock_q_bit   = 2U, /**< clock_q_enable.                          */
  k_ra8_npu_cmd_power_q_bit   = 3U, /**< power_q_enable.                          */
} ra8_npu_cmd_bit_t;

/**
 * @enum ra8_npu_status_bit_t
 * @brief Bit positions in the NPU_STATUS register.
 *
 * @details `state` reads 1 while the NPU is running and 0 when it has stopped.
 *          `cmd_end` asserts when the command stream is fully consumed. The
 *          remaining bits latch fault conditions (bus error, command-stream
 *          parse error, weight-decoder fault, internal-SRAM ECC fault) that the
 *          driver surfaces as `k_ra8_err_hw_error`.
 *
 * @invariant Each value is a valid 0-based bit index in a 32-bit register.
 * @see ra8_npu_cmd_bit_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_npu_status_state_bit      = 0U, /**< 1 = running, 0 = stopped.      */
  k_ra8_npu_status_irq_raised_bit = 1U, /**< IRQ asserted by the NPU.       */
  k_ra8_npu_status_bus_error_bit  = 2U, /**< AXI bus error latched.         */
  k_ra8_npu_status_reset_bit      = 3U, /**< Reset in progress.             */
  k_ra8_npu_status_cmd_parse_bit  = 4U, /**< Command-stream parse error.    */
  k_ra8_npu_status_cmd_end_bit    = 5U, /**< Command stream fully consumed. */
  k_ra8_npu_status_pmu_irq_bit    = 6U, /**< Performance-counter IRQ.       */
  k_ra8_npu_status_wd_fault_bit   = 7U, /**< Weight-decoder fault.          */
  k_ra8_npu_status_ecc_fault_bit  = 8U, /**< Internal SRAM ECC fault.       */
} ra8_npu_status_bit_t;

/**
 * @enum ra8_npu_reset_bit_t
 * @brief Bit positions in the NPU_RESET register.
 *
 * @details A write to NPU_RESET requests a soft reset; the target privilege and
 *          security levels the NPU adopts on the next run are encoded in these
 *          bits. The driver clears both (privileged, secure) and then polls
 *          `k_ra8_npu_status_reset_bit` until the reset completes.
 *
 * @invariant Each value is a valid 0-based bit index in a 32-bit register.
 * @see ra8_npu_status_bit_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_npu_reset_pending_cpl_bit = 0U, /**< Privilege level adopted after reset. */
  k_ra8_npu_reset_pending_csl_bit = 1U, /**< Security level adopted after reset.  */
} ra8_npu_reset_bit_t;

/**
 * @enum ra8_npu_event_t
 * @brief ICU/ELC event number for the NPU interrupt.
 *
 * @details Route this event to an NVIC line via the ICU IELSR registers the
 *          same way any other peripheral event is routed (see
 *          `ra8_icu_regs.h` / `ra8_isr_register`). This number occupies a slot
 *          that is a reserved gap in the RA8D2 event space, so it does not
 *          collide with any shared event.
 *
 * @invariant Value is a valid `elc_event_t` index on the RA8P1.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_npu_event_irq = 0x067U, /**< NPU_IRQ event (RA8P1 elc_event_t). */
} ra8_npu_event_t;

/**
 * @brief Compute a volatile pointer to an NPU register by byte offset.
 *
 * @details Returns `k_ra8_npu_base_addr + byte_off` as a `volatile uint32_t*` so
 *          `ra8_npu.c` can read/write a confirmed offset (e.g. `k_ra8_npu_off_cmd`)
 *          with a single word access. This accessor performs NO register access
 *          -- it only computes an address -- so no register citation lives here;
 *          the citation sits at each dereference site in the driver. The caller
 *          must have released the NPU module-stop bit and enabled NPUCLK before
 *          dereferencing the result.
 *
 * @param[in] byte_off Register byte offset from `ra8_npu_layout_t`
 *                     (0 .. `k_ra8_npu_window_bytes` - 4, 4-byte aligned).
 *
 * @return Volatile pointer to the addressed 32-bit NPU register.
 * @retval (base+off) Always the fixed peripheral base plus `byte_off` (non-NULL).
 *
 * @pre The build targets the RA8P1 (`RA8_HAS_NPU` defined).
 * @pre NPU module-stop released and NPUCLK enabled before any dereference.
 * @post Returned pointer addresses the requested register; nothing was touched.
 * @post No side effects on hardware.
 *
 * @note Not thread-safe in the sense of hardware ownership: the caller owns
 *       serialisation of NPU access. The pointer computation itself is
 *       re-entrant.
 * @see ra8_npu_layout_t
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_npu_reg(uint32_t byte_off)
{
  return (volatile uint32_t*)(k_ra8_npu_base_addr + (uintptr_t)byte_off);
}

#ifdef __cplusplus
}
#endif
