/**
 * @file sim_exc.h
 * @brief Cortex-M exception model constants and interfaces for board_sim
 *
 * @details
 * The architectural constants of the Armv7E-M / Armv8-M exception model the
 * emulator reproduces by hand (Unicorn's Cortex-M33 core carries no NVIC /
 * exception unit): the system-control-space register addresses the models
 * poll/edit in flat PPB RAM, and the EXC_RETURN / stack-frame / instruction
 * encoding constants the entry/return and instruction-seam logic decodes.
 *
 * Split out of the board_sim main translation unit; behaviour unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cortex-M system control space (architectural, all cores) -- inside the PPB.
 * The PPB is mapped as plain RAM here (not callback MMIO), so SCB/NVIC writes
 * the firmware performs land in memory and read back -- the exception model
 * below polls/edits these words to model what real M-profile hardware would do
 * with the NVIC that Unicorn does not implement. */
typedef enum : uint64_t {
  k_syst_csr          = 0xE000E010UL, /**< SysTick control/status (SYST_CSR).     */
  k_syst_csr_run      = 0x3UL,        /**< ENABLE | TICKINT both set.             */
  k_scb_icsr          = 0xE000ED04UL, /**< Interrupt control/state (ICSR).        */
  k_scb_vtor          = 0xE000ED08UL, /**< Vector table offset register.          */
  k_scb_aircr         = 0xE000ED0CUL, /**< App interrupt/reset control (AIRCR).   */
  k_aircr_sysresetreq = 2UL,          /**< AIRCR.SYSRESETREQ bit (request reset). */
  k_scb_shpr2         = 0xE000ED1CUL, /**< System handler priority 2 (SVC=b3).    */
  k_scb_shpr3         = 0xE000ED20UL, /**< System handler priority 3 (PSV/SYT).   */
  k_mpu_type          = 0xE000ED90UL, /**< MPU_TYPE (DREGION in bits 15:8).       */
  k_mpu_type_seed     = 0x00000800UL, /**< 8 data regions (matches the M85 MPU).  */
  k_mpu_ctrl          = 0xE000ED94UL, /**< MPU_CTRL (ENABLE in bit 0).            */
  k_mpu_rnr           = 0xE000ED98UL, /**< MPU_RNR (region number select).        */
  k_mpu_rbar          = 0xE000ED9CUL, /**< MPU_RBAR (BASE[31:5]|SH|AP[2:1]|XN).   */
  k_mpu_rlar          = 0xE000EDA0UL, /**< MPU_RLAR (LIMIT[31:5]|AttrIdx|EN).     */
  k_scb_ccr           = 0xE000ED14UL, /**< Configuration and Control (CCR).       */
  k_ccr_div_0_trp     = 0x10UL,       /**< CCR.DIV_0_TRP bit4: divide-by-0 traps. */
  k_scb_cfsr          = 0xE000ED28UL, /**< Config Fault Status (MMFSR low byte).  */
  k_scb_mmfar         = 0xE000ED34UL, /**< MemManage Fault Address Register.      */
  k_icsr_pendsvset    = 28UL,         /**< ICSR.PENDSVSET bit (request PendSV).   */
  k_icsr_pendstset    = 26UL,         /**< ICSR.PENDSTSET bit (request SysTick).  */
  k_exc_usagefault    = 6UL,          /**< UsageFault exception / vector index.   */
  k_exc_memmanage     = 4UL,          /**< MemManage exception / vector index.    */
  k_exc_svcall        = 11UL,         /**< SVCall exception / vector index.       */
  k_exc_pendsv        = 14UL,         /**< PendSV exception / vector index.       */
  k_exc_systick       = 15UL,         /**< SysTick exception / vector index.      */
  k_nvic_ipr_base     = 0xE000E400UL, /**< NVIC IPR priority bytes (one per IRQ). */
  k_nvic_ispr_base    = 0xE000E200UL, /**< NVIC ISPR set-pending (per-IRQ bit).   */
  k_nvic_iser_base    = 0xE000E100UL, /**< NVIC ISER set-enable array base.       */
  k_nvic_icer_base    = 0xE000E180UL, /**< NVIC ICER clear-enable array base.     */
  k_nvic_en_words     = 8UL,          /**< ISER/ICER words modelled (256 lines).  */
  k_nvic_en_span      = 8UL * 4UL,    /**< Byte span of one set/clear array.      */
  k_exc_irq_vec0      = 16UL,         /**< Vector index of IRQ0 (16 + IRQn).      */
} cortexm_scs_t;

/* Armv7E-M / Armv8-M exception model constants (the part Unicorn's Cortex-M33
 * core leaves to software here -- it has no NVIC/exception unit). EXC_RETURN
 * magic values steer the unstack: bit2 picks the return stack (1 = PSP, 0 =
 * MSP), bit3 the return mode (1 = Thread, 0 = Handler), bit4 the frame type
 * (1 = basic 8-word frame, 0 = FP extended frame). On Armv8-M the value also
 * carries bit6 = S (1 = return to Secure, 0 = Non-Secure) and bit5 = DCRS, so a
 * Non-Secure thread return is 0xFFFFFFBC -- NOT one of the Armv7-M 0xFFFFFFF_
 * values. is_exc_return() therefore matches the whole bits[31:7]-set EXC_RETURN
 * prefix (0xFFFFFF80..FF), and exc_enter/exc_return branch on bit4 (FType) to
 * stack/unstack the S0-S15 + FPSCR words whenever an FP extended frame is in
 * play (ThreadX's PendSV mirrors this with its own TST LR,#0x10 on S16-S31). */
typedef enum : uint32_t {
  k_exc_frame_words   = 8U,          /**< {R0-R3,R12,LR,PC,xPSR} basic frame.    */
  k_exc_frame_bytes   = 32U,         /**< 8 words * 4 bytes.                     */
  k_exc_ret_v8_mask   = 0xFFFFFF80U, /**< Armv8-M EXC_RETURN prefix: bits[31:7]. */
  k_exc_ret_ftype     = 0x10U,       /**< EXC_RETURN bit4: 1 = basic, 0 = FP.    */
  k_exc_ret_handler   = 0xFFFFFFF1U, /**< Return to Handler mode, MSP.           */
  k_exc_ret_msp       = 0xFFFFFFF9U, /**< Return to Thread mode, MSP.            */
  k_exc_ret_psp       = 0xFFFFFFFDU, /**< Return to Thread mode, PSP.            */
  k_exc_ret_spsel     = 0x4U,        /**< EXC_RETURN bit2: return stack = PSP.   */
  k_exc_ret_mode      = 0x8U,        /**< EXC_RETURN bit3: return to Thread.     */
  k_control_spsel     = 0x2U,        /**< CONTROL.SPSEL: thread SP = PSP.        */
  k_control_fpca      = 0x4U,        /**< CONTROL.FPCA: FP context is active.    */
  k_xpsr_t_bit        = 0x01000000U, /**< xPSR.T (Thumb) -- must stay set.       */
  k_xpsr_align9       = 0x00000200U, /**< xPSR bit9: stack-frame realignment.    */
  k_xpsr_ipsr_mask    = 0x000001FFU, /**< xPSR[8:0] = IPSR (active exception).   */
  k_exc_prio_none     = 0x100U,      /**< Sentinel "no handler active" prio.     */
  k_exc_prio_max      = 0xFFU,       /**< Lowest configurable priority value.    */
  k_exc_nest_max      = 4U,          /**< Tracked active-exception nesting cap.  */
  k_byte_bits         = 8U,          /**< Bits per byte (SHPR field width).      */
  k_frame_off_r3      = 12U,         /**< Basic exception-frame offset of R3.    */
  k_frame_off_lr      = 20U,         /**< Basic exception-frame offset of LR.    */
  k_frame_off_pc      = 24U,         /**< Basic exception-frame offset of PC.    */
  k_frame_off_xpsr    = 28U,         /**< Basic exception-frame offset of xPSR.  */
  k_fp_frame_extra    = 72U,         /**< FP ext frame above basic: S0-15+FPSCR. */
  k_fp_s_words        = 16U,         /**< S0-S15 saved in the FP extended frame. */
  k_frame_off_s0      = 32U,         /**< FP-frame offset of S0 (above basic).   */
  k_frame_off_fpscr   = 96U,         /**< FP-frame offset of FPSCR (32 + 16*4).  */
  k_vector_erased     = 0xFFFFFFFEU, /**< Erased-flash / invalid vector word.    */
  k_nvic_prio_shift   = 4U,          /**< Implemented priority is the 4 MSBs.    */
  k_lo4_mask          = 0xFU,        /**< Low nibble (register / cond field).    */
  k_armv8m_sg_opcode  = 0xE97FE97FU, /**< Armv8-M `SG` secure-gateway opcode.    */
  k_thumb2_insn_bytes = 4U,          /**< 32-bit Thumb-2 instruction width.      */
  k_fpcxtns_push      = 0xCF81ED6DU, /**< `VSTR FPCXTNS,[sp,#-4]!` (LE word).    */
  k_fpcxtns_pop       = 0xCF81ECFDU, /**< `VLDR FPCXTNS,[sp],#4` (LE word).      */
  k_word_bytes        = 4U,          /**< One stacked word.                      */
  k_clrm_hw0          = 0xE89FU,     /**< `CLRM {regs}` first halfword.          */
  k_vscclrm_hw0_s     = 0xEC9FU,     /**< `VSCCLRM {s..,VPR}` first halfword.    */
  k_vscclrm_hw0_d     = 0xECDFU,     /**< `VSCCLRM {d..,VPR}` first halfword.    */
  k_lo16_mask         = 0xFFFFU,     /**< Low halfword of a 32-bit fetch.        */
  k_bkpt_hw_base      = 0xBE00U,     /**< `BKPT #imm8` halfword (imm free).      */
  k_bkpt_hw_mask      = 0xFF00U,     /**< Mask isolating the BKPT opcode.        */
  k_thumb_bx_lr       = 0x4770U,     /**< `BX LR` (stub a function to return).   */
} cortexm_exc_t;

#ifdef __cplusplus
}
#endif
