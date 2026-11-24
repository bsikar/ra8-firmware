/**
 * @file ra8_target_model.h
 * @brief Consumable single-source RA8 target model for firmware and host tools
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: ALL}
 *
 * @details
 * Umbrella target model publishing the SoC architecture constants that host
 * tools (`ra8_emulator` via Zig `translate-c`), off-target test harnesses, and
 * chip-level drivers consume:
 *
 *   - SoC identity, feature presence flags, and core memory window bases
 *     (`ra8_device.h`).
 *   - Canonical ELF, linker, DFU, UI debug, and RTOS seam symbol names
 *     (`ra8_seam_symbols.h`).
 *   - Complete peripheral register block bases, layouts, and bitfields
 *     (`ra8_regs.h` plus conditional `ra8_npu_regs.h` under `RA8_HAS_NPU`).
 *   - Dual-core CPU_CTRL window (`ra8_dual_core.h`) and early-boot SYSC LPM
 *     addresses (`ra8_lpm_safe_boot.h`).
 *   - Cortex-M85 Private Peripheral Bus (PPB), IDAU Non-Secure bus alias,
 *     CPSCU / PSCU / SAMON security controller bases, and SYSC register
 *     addresses derived directly from `k_ra8_system_base_addr` plus register
 *     offsets (`ra8_target_model_bus_t`, `ra8_target_model_sysc_addr_t`).
 *
 * Board-specific memory geometries (flash/SRAM/SDRAM/OSPI capacities and board
 * aliases) intentionally stay in the Ring-5 BSP headers
 * (`libs/ra8_board_ek_ra8d2/inc/ra8_board_memmap.h` and
 * `libs/ra8_board_ra8p1/inc/ra8_board_memmap.h`) so Ring 3 HAL stays board-
 * independent.
 *
 * ## Consuming from Zig (`translate-c`)
 *
 * The firmware headers use C23 typed enums, `static_assert`, and `nullptr`.
 * Translate this header (together with a board's `ra8_board_memmap.h` if
 * needed) with:
 *
 * @code{.text}
 * -DRA8_OFF_TARGET=1 -Dstatic_assert=_Static_assert -Dnullptr=((void*)0)
 * @endcode
 *
 * Pass `-DRA8_DEVICE_RA8P1` (or `-DRA8_HAS_NPU=1`) when extracting the RA8P1
 * Ethos-U55 NPU register window.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* `ra8_device.h` sorts first alphabetically and resolves RA8_HAS_NPU before NPU gating. */
#include "ra8_device.h"
#include "ra8_dual_core.h"
#include "ra8_lpm_safe_boot.h"
#include "ra8_regs.h"
#include "ra8_seam_symbols.h"

#ifdef RA8_HAS_NPU
#include "ra8_npu_regs.h"
#endif

/* =============================================================================
 * SoC Bus, Security-Attribution, and Cortex-M85 Core PPB Bases
 * =============================================================================
 */

/**
 * @enum ra8_target_model_bus_t
 * @brief Architectural bus windows, IDAU offset, and Cortex-M85 PPB block bases.
 *
 * @details
 * Sources:
 *   - Renesas RA8D2 HUM Ch 4.1 "Memory Map" (Peripheral space 0x4000_0000),
 *     Ch 51 "TrustZone" (`R_CPSCU` 0x4000_8000, `R_PSCU` 0x4020_4000,
 *     CMSAMON +0x30, SFSAMON +0x3C, IDAU bit[28]=1 NS offset +0x1000_0000).
 *   - Arm v8-M Architecture Reference Manual B3.2 (PPB 0xE000_0000, SCS
 *     0xE000_E000, SCB 0xE000_ED00), B6.2 (NS aliases 0xE002_E000 /
 *     0xE002_ED00), C1.6 (DCB 0xE000_EDF0), C1.7 (FPB 0xE000_2000), C1.8
 *     (DWT 0xE000_1000), and NVIC_ITNS0..15 (0xE000_E380).
 */
typedef enum : uintptr_t {
  k_ra8_periph_bus_base_addr    = 0x40000000UL, /**< Secure peripheral bus base.            */
  k_ra8_idau_ns_alias_offset    = 0x10000000UL, /**< IDAU bit[28] Non-Secure alias delta.   */
  k_ra8_periph_bus_ns_base_addr = 0x50000000UL, /**< Non-Secure peripheral bus alias.       */
  k_ra8_cpscu_base_addr         = 0x40008000UL, /**< Secure CPSCU controller base.          */
  k_ra8_pscu_base_addr          = 0x40204000UL, /**< Secure PSCU peripheral attr base.      */
  k_ra8_pscu_psarb_addr         = 0x40204004UL, /**< PSCU.PSARB (MSTPCRB attribution).      */
  k_ra8_pscu_psarc_addr         = 0x40204008UL, /**< PSCU.PSARC (MSTPCRC attribution).      */
  k_ra8_pscu_psard_addr         = 0x4020400CUL, /**< PSCU.PSARD (MSTPCRD attribution).      */
  k_ra8_pscu_psare_addr         = 0x40204010UL, /**< PSCU.PSARE (MSTPCRE attribution).      */
  k_ra8_samon_cms_addr          = 0x40204030UL, /**< PSCU.CMSAMON code MRAM split monitor.  */
  k_ra8_samon_sfs_addr          = 0x4020403CUL, /**< PSCU.SFSAMON SiP flash split monitor.  */
  k_ra8_core_ppb_base_addr      = 0xE0000000UL, /**< Armv8-M Private Peripheral Bus base.   */
  k_ra8_core_itm_base_addr      = 0xE0000000UL, /**< Instrumentation Trace Macrocell base.  */
  k_ra8_core_dwt_base_addr      = 0xE0001000UL, /**< Data Watchpoint and Trace base.        */
  k_ra8_core_fpb_base_addr      = 0xE0002000UL, /**< Flash Patch and Breakpoint base.       */
  k_ra8_core_scs_base_addr      = 0xE000E000UL, /**< System Control Space (Secure) base.    */
  k_ra8_core_itns_base_addr     = 0xE000E380UL, /**< NVIC_ITNS0..15 IRQ target array.       */
  k_ra8_core_scb_base_addr      = 0xE000ED00UL, /**< System Control Block (Secure) base.    */
  k_ra8_core_dcb_base_addr      = 0xE000EDF0UL, /**< Debug Control Block base.              */
  k_ra8_core_scs_ns_base_addr   = 0xE002E000UL, /**< System Control Space Non-Secure alias. */
  k_ra8_core_scb_ns_base_addr   = 0xE002ED00UL, /**< System Control Block Non-Secure alias. */
} ra8_target_model_bus_t;

/* =============================================================================
 * Derived SYSC / Clock / Reset Register Addresses
 * =============================================================================
 */

/**
 * @enum ra8_target_model_sysc_addr_t
 * @brief SYSC register addresses derived strictly from base + header offsets.
 */
typedef enum : uintptr_t {
  k_ra8_sys_vscr_addr = (uintptr_t)k_ra8_system_base_addr +
                        (uintptr_t)k_ra8_sys_off_vscr, /**< VSCR voltage scaling register. */
  k_ra8_sys_sckdivcr_addr =
    (uintptr_t)k_ra8_system_base_addr +
    (uintptr_t)k_ra8_sys_off_sckdivcr, /**< SCKDIVCR system clock divider. */
  k_ra8_sys_pllcr_addr =
    (uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_pllcr, /**< PLLCR PLL1 control. */
  k_ra8_sys_oscsf_addr   = (uintptr_t)k_ra8_system_base_addr +
                           (uintptr_t)k_ra8_sys_off_oscsf, /**< OSCSF oscillator stab flags. */
  k_ra8_sys_pllccr2_addr = (uintptr_t)k_ra8_system_base_addr +
                           (uintptr_t)k_ra8_sys_off_pllccr2, /**< PLLCCR2 PLL1 output dividers. */
  k_ra8_sys_scickdivcr_addr =
    (uintptr_t)k_ra8_system_base_addr +
    (uintptr_t)k_ra8_sys_off_scickdivcr, /**< SCICKDIVCR SCI clock divider. */
  k_ra8_sys_octackcr_addr =
    (uintptr_t)k_ra8_system_base_addr +
    (uintptr_t)k_ra8_sys_off_octackcr, /**< OCTACKCR OSPI clock source select. */
  k_ra8_sys_moscwtcr_addr =
    (uintptr_t)k_ra8_system_base_addr +
    (uintptr_t)k_ra8_sys_off_moscwtcr, /**< MOSCWTCR main osc wait cycles. */
  k_ra8_sys_pllccr_addr = (uintptr_t)k_ra8_system_base_addr +
                          (uintptr_t)k_ra8_sys_off_pllccr, /**< PLLCCR PLL1 input/multiplier. */
  k_ra8_sys_rstsr1_addr = (uintptr_t)k_ra8_system_base_addr +
                          (uintptr_t)k_ra8_sys_off_rstsr1, /**< RSTSR1 reset status 1. */
  k_ra8_sys_prcr_addr   = (uintptr_t)k_ra8_system_base_addr +
                          (uintptr_t)k_ra8_sys_off_prcr, /**< PRCR write-protection key register. */
  k_ra8_sys_lococr_addr = (uintptr_t)k_ra8_system_base_addr +
                          (uintptr_t)k_ra8_sys_off_lococr, /**< LOCOCR LOCO oscillator control. */
  k_ra8_sys_rstsr0_addr = (uintptr_t)k_ra8_system_base_addr +
                          (uintptr_t)k_ra8_sys_off_rstsr0, /**< RSTSR0 reset status 0. */
  k_ra8_sys_syrstmsk0_addr =
    (uintptr_t)k_ra8_system_base_addr +
    (uintptr_t)k_ra8_reset_off_syrstmsk0, /**< SYRSTMSK0 reset mask base. */
  k_ra8_sys_sosccr_addr = (uintptr_t)k_ra8_system_base_addr +
                          (uintptr_t)k_ra8_sys_off_sosccr, /**< SOSCCR sub-clock control. */
} ra8_target_model_sysc_addr_t;

/* =============================================================================
 * Architectural Cross-Checks
 * =============================================================================
 */

static_assert((uintptr_t)k_ra8_periph_bus_base_addr + (uintptr_t)k_ra8_idau_ns_alias_offset ==
                (uintptr_t)k_ra8_periph_bus_ns_base_addr,
              "Non-Secure peripheral bus must track IDAU bit[28] alias offset");
static_assert((uintptr_t)k_ra8_sram_data_base_addr == (uintptr_t)k_ra8_mem_sram_base,
              "SRAM reg header data base must match Core device map");
static_assert((uintptr_t)k_ra8_sdram_base_addr == (uintptr_t)k_ra8_mem_sdram_base,
              "SDRAM controller data base must match Core device map");
static_assert((uintptr_t)k_ra8_sys_prcr_addr == (uintptr_t)k_ra8_lpm_safe_boot_prcr_addr,
              "SYSC PRCR address must match safe-boot helper");
static_assert((uintptr_t)k_ra8_cpscu_base_addr == (uintptr_t)k_ra8_sram_cpscu_base_addr,
              "CPSCU target-model base must match SRAM CPSCU window");

#ifdef __cplusplus
}
#endif
