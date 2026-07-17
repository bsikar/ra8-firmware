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
#include <unicorn/unicorn.h>

#include "ra8_attributes.h"

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
  k_thumb_hw_bytes    = 2U,          /**< Bytes per Thumb halfword.              */
  k_byte_mask         = 0xFFU,       /**< Low 8 bits of a value (one byte).      */
} cortexm_exc_t;

/**
 * @brief Read the handler address for an exception from the vector table.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base used when VTOR reads as 0.
 * @param[in]     exc_num   Exception/vector index to look up.
 * @return Handler entry address with the Thumb bit cleared.
 * @retval 0 when no usable handler is installed at that vector slot.
 * @pre @p uc has the vector table mapped at VTOR (or @p vtor_base).
 * @pre @p exc_num is a valid vector index (< table length).
 * @post No engine state is modified (read-only).
 * @note VTOR lives in PPB RAM here, written by SystemInit at boot.
 * @since 0.1.0
 */
RA8_PRIV uint32_t exc_vector(uc_engine* uc, uint32_t vtor_base, uint32_t exc_num);

/**
 * @brief Enter a Cortex-M exception: stack the basic frame and vector in.
 *
 * @details Reproduces Armv7E-M / Armv8-M exception entry that Unicorn's core
 * does not model: stack selection (PSP in Thread mode with CONTROL.SPSEL,
 * else MSP), the 8-word basic frame push with 8-byte realignment recorded in
 * the stacked xPSR, the FP extended frame when CONTROL.FPCA is set, the
 * EXC_RETURN load, the IPSR update and the vector fetch. The handler's
 * priority is pushed on the active-exception stack so nesting respects
 * priority.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     exc_num Exception number to take.
 * @param[in]     handler Handler entry address (Thumb bit ignored).
 * @return Nothing.
 * @pre Taking @p exc_num is permitted now (priority/PRIMASK already checked).
 * @pre The target stack is mapped.
 * @post The core is in Handler mode (IPSR == @p exc_num) running on MSP.
 * @post LR holds a valid EXC_RETURN and the outgoing frame is stacked.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see exc_return()  The inverse operation.
 * @since 0.1.0
 */
RA8_PRIV void exc_enter(uc_engine* uc, uint32_t exc_num, uint32_t handler);

/**
 * @brief Perform a Cortex-M exception return for an observed EXC_RETURN branch.
 *
 * @details The inverse of exc_enter(): pops the basic (and, when FType is
 * clear, FP extended) frame from the stack EXC_RETURN selects, undoes the
 * recorded realignment, restores CONTROL.SPSEL / xPSR / the banked SP, and
 * pops the active-exception stack.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     exc_ret The EXC_RETURN value (prefix bits[31:7] set).
 * @return Nothing.
 * @pre @p uc is in Handler mode with a valid frame on the indicated stack.
 * @pre @p exc_ret carries the EXC_RETURN prefix.
 * @post The core has resumed the unstacked context (PC/SP/xPSR restored).
 * @post The active-exception nesting depth decreased by one (if non-zero).
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see exc_enter()  The inverse operation.
 * @since 0.1.0
 */
RA8_PRIV void exc_return(uc_engine* uc, uint32_t exc_ret);

/**
 * @brief Take the highest-priority pending exception, if one may activate now.
 *
 * @details The software replacement for the NVIC's activation rule, called at
 * every instruction boundary AND after each exception return so a pend
 * tail-chains as hardware would. Models SysTick (periodic, armed once per
 * outer chunk), PendSV (level-pending via ICSR.PENDSVSET, cleared on
 * activation) and the ICU-queued peripheral IRQs, honouring PRIMASK and the
 * active-priority stack.
 *
 * @param[in,out] uc            Unicorn engine.
 * @param[in]     vtor_base     Fallback vector base if VTOR reads as 0.
 * @param[in]     allow_systick When false, the armed SysTick is left pending
 *                              so modelled time does NOT advance (used on
 *                              zero-time context-switch boundaries).
 * @return true if an exception was taken (PC now points at a handler).
 * @retval false Nothing pended, masked, or outprioritised.
 * @pre @p uc has stopped at an instruction boundary or just returned.
 * @pre The PPB (SYST_CSR / ICSR / SHPRn / VTOR) is mapped as RAM.
 * @post At most one exception is taken per call.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV bool exc_take_pending(uc_engine* uc, uint32_t vtor_base, bool allow_systick);

/**
 * @brief True if @p pc sits in a wait-for-interrupt spin (the core is idle).
 *
 * @details Recognises a halt instruction at @p pc (`b .` / `wfi`) or an
 * enclosing tight poll loop whose back-edge brackets @p pc and whose body
 * holds a `wfi` or `cpsie i` -- genuine idle, where the next thing that can
 * happen is the periodic SysTick. The run loop caps the idle chunk budget so
 * the spin returns at once instead of burning wall time; tick COUNT is
 * unchanged.
 *
 * @param[in,out] uc Unicorn engine (instructions are read from its memory).
 * @param[in]     pc Program counter to inspect (Thumb bit ignored).
 * @return true if @p pc is on, or enclosed by, a wait-for-interrupt idle loop.
 * @retval false Busy/straight-line code (never truncated).
 * @pre @p uc has the code region containing @p pc mapped.
 * @pre @p pc is halfword-aligned once the Thumb bit is cleared.
 * @post @p uc is unchanged (a read-only probe).
 * @note Detection only; advancing time stays the run loop's job.
 * @since 0.1.0
 */
RA8_PRIV bool idle_spin_at(uc_engine* uc, uint32_t pc);

/**
 * @brief Advance the DWT cycle counter by one outer chunk's worth of cycles.
 *
 * @details Models DWT_CYCCNT as the free-running counter Armv8-M specifies:
 * it counts only while DEMCR.TRCENA and DWT_CTRL.CYCCNTENA are both set, so
 * an app that never enables it is untouched. Keeps a masked-context
 * ra8_delay_ms (which spins on CYCCNT while PRIMASK is set) making progress.
 *
 * @param[in,out] uc Active Unicorn engine (CYCCNT lives in PPB RAM).
 * @return Nothing.
 * @pre @p uc has stopped at an instruction boundary (outer-chunk cadence).
 * @pre The PPB (DEMCR / DWT_CTRL / DWT_CYCCNT) is mapped as RAM.
 * @post CYCCNT advanced iff the trace subsystem and counter are enabled.
 * @note Not thread-safe; the run loop is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void dwt_cyccnt_advance(uc_engine* uc);

/**
 * @brief Arm the core exception hooks (unmapped / INTR / ICSR watch).
 *
 * @return Nothing.
 * @pre @p uc is initialised (setup phase).
 * @pre Called once, at the same setup position the hooks were always added.
 * @post The unmapped-access, interrupt and ICSR write hooks are live.
 * @note Not thread-safe; call once during single-threaded setup.
 * @see sim_exc_install_scb_nvic()  The second hook batch.
 * @since 0.1.0
 */
RA8_PRIV void sim_exc_install_core(uc_engine* uc);

/**
 * @brief Arm the SCB control-word and NVIC ISER/ICER write watchers.
 *
 * @return Nothing.
 * @pre sim_exc_install_core() ran (hook order is install order).
 * @pre Called once, at the same setup position the hooks were always added.
 * @post The AIRCR..CCR and ISER/ICER write hooks are live.
 * @note Not thread-safe; call once during single-threaded setup.
 * @since 0.1.0
 */
RA8_PRIV void sim_exc_install_scb_nvic(uc_engine* uc);

/**
 * @brief Pend the periodic SysTick for this outer chunk.
 *
 * @return Nothing.
 * @pre The run loop is at an outer-chunk boundary (one tick per chunk).
 * @pre None otherwise.
 * @post The tick is armed; exc_take_pending() may take it when permitted.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void sim_exc_arm_systick(void);

/**
 * @brief Consume a latched EXC_RETURN branch (read + clear).
 *
 * @param[out] out_pc Receives the EXC_RETURN magic value on true.
 * @return true when an EXC_RETURN branch was captured since the last take.
 * @retval false No exception return is pending (@p out_pc untouched).
 * @pre @p out_pc is non-null.
 * @pre The engine just stopped.
 * @post The latch is clear.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV bool sim_exc_take_exc_return(uint64_t* out_pc);

/**
 * @brief Clear the PendSV context-switch stop marker (per relaunch).
 *
 * @return Nothing.
 * @pre The inner run loop is about to (re)launch the engine.
 * @pre None otherwise.
 * @post The marker is clear until the next PENDSVSET-triggered stop.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void sim_exc_clear_pendsv_stop(void);

/**
 * @brief Whether the last engine stop was a PENDSVSET context-switch stop.
 *
 * @return true when the chunk ended on a PENDSVSET write.
 * @retval false The stop had another cause.
 * @pre The engine just stopped.
 * @pre None otherwise.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV bool sim_exc_pendsv_stop(void);

/**
 * @brief Whether the firmware executed a BKPT (deliberate trap / give-up).
 *
 * @return true once a BKPT halted the run.
 * @retval false No BKPT was executed.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV bool sim_exc_bkpt_hit(void);

/**
 * @brief PC of the BKPT that halted the run.
 *
 * @return The trapping BKPT's address.
 * @retval 0 No BKPT was executed this run.
 * @pre sim_exc_bkpt_hit() returned true (else the value is stale/zero).
 * @pre None otherwise.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV uint32_t sim_exc_bkpt_pc(void);

/**
 * @brief Whether AIRCR.SYSRESETREQ requested a warm reboot.
 *
 * @return true while a reset request is latched.
 * @retval false No reset was requested.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV bool sim_exc_reboot_requested(void);

/**
 * @brief Clear the latched warm-reboot request (after performing it).
 *
 * @return Nothing.
 * @pre The run loop just performed the warm reboot.
 * @pre None otherwise.
 * @post No reset request is pending.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void sim_exc_clear_reboot_request(void);

/**
 * @brief SysTick exceptions taken this run (report telemetry).
 *
 * @return The SysTick activation count.
 * @retval 0 No tick has fired yet.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV uint32_t sim_exc_systick_fires(void);

/**
 * @brief PendSV exceptions taken this run (report + idle signature).
 *
 * @return The PendSV activation count.
 * @retval 0 No context switch has happened yet.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV uint32_t sim_exc_pendsv_takes(void);

/**
 * @brief SVCall exceptions taken this run (report + idle signature).
 *
 * @return The SVCall activation count.
 * @retval 0 No SVC has been taken yet.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV uint32_t sim_exc_svc_takes(void);

/**
 * @brief Reset the exception bookkeeping for a warm reboot.
 *
 * @details Clears the active-handler stack, re-arms the periodic SysTick,
 * drops the BKPT / EXC_RETURN / PendSV-stop latches and zeroes the activation
 * counters -- exactly the state a fresh boot starts with. The reboot request
 * latch is owned by the SCB watcher and cleared separately.
 *
 * @return Nothing.
 * @pre A warm reboot just re-loaded the image.
 * @pre None otherwise.
 * @post The exception engine is in its boot state.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void sim_exc_reset(void);

#ifdef __cplusplus
}
#endif
