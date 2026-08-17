/**
 * @file emu_exc_scs.c
 * @brief SCS write watchers + DWT time base (see emu_exc.h)
 *
 * @details
 * The system-control-space companions of the exception engine: the SCB
 * control-word watcher (AIRCR.SYSRESETREQ warm-reboot requests and the
 * CCR.DIV_0_TRP arming of the div-0 seam), the NVIC ISER/ICER W1S/W1C
 * folding into board_periph's enable shadow, and the DWT cycle-counter model
 * that keeps a masked-context ra8_delay_ms advancing. Moved verbatim out of
 * the ra8_emulator main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdio.h>

#include "board_periph.h"
#include "emu_console.h"
#include "emu_engine.h"
#include "emu_exc.h"
#include "emu_seams.h"

/** @brief AIRCR.SYSRESETREQ observed: the run loop performs a warm reboot. */
static bool s_reboot_request;

/**
 * @brief UC_HOOK_MEM_WRITE handler for the SCB control words AIRCR and CCR.
 *
 * @details
 * One write hook spans the SCB control block from AIRCR (0xE000ED0C) through CCR
 * (0xE000ED14) and dispatches strictly by word address, so the two nearby control
 * registers share a single hook rather than each adding its own -- this Unicorn
 * build consults every installed memory hook per access, so folding CCR in here
 * keeps a hot read loop paying the baseline hook count. Intervening words (VTOR,
 * SCR) fall through untouched.
 *
 * - **AIRCR**: writing SYSRESETREQ (with the mandatory 0x05FA key in the upper
 *   half-word) asks the chip to reset. The emulator honours it as a warm reboot:
 *   record the request and stop the chunk; the run loop's reboot wrapper re-runs
 *   the firmware from its reset vector (latching RSTSR1.SWRF). Without this,
 *   ra8_reset_software_reset would spin forever waiting for a reset that never came.
 * - **CCR**: writing DIV_0_TRP arms the divide-by-zero UsageFault by overwriting
 *   the tracked divide sites with UDF (::div0_patch_sites) the instant the firmware
 *   opts in, race-free even when the arming write and the divide share one chunk.
 *
 * @param[in,out] uc    Unicorn engine (stopped to end the chunk on a reset).
 * @param[in]     type  Unused memory-event type.
 * @param[in]     addr  Observed SCB address (dispatched by word).
 * @param[in]     size  Access width (unused).
 * @param[in]     value Value written.
 * @param[in]     user  Unused hook context.
 * @return Nothing.
 * @since 0.1.0
  * @pre Arguments satisfy the ranges documented for on SCB ctrl write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu exc scs model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 */
RA8_INTERNAL static void internal_on_scb_ctrl_write(uc_engine*  uc,
                                                    uc_mem_type type,
                                                    uint64_t    addr,
                                                    int         size,
                                                    int64_t     value,
                                                    void*       user)
{
  (void)type;
  (void)size;
  (void)user;
  const uint32_t word = (uint32_t)addr & ~3U;
  if (word == (uint32_t)k_scb_ccr) {
    if (((uint32_t)value & (uint32_t)k_ccr_div_0_trp) != 0U) {
      div0_patch_sites(uc); /* firmware opted in: overwrite the divides with UDF. */
    }
    return;
  }
  if (word != (uint32_t)k_scb_aircr) {
    return; /* an intervening SCB word (VTOR / SCR): not ours. */
  }
  if (((uint32_t)value & (1U << (uint32_t)k_aircr_sysresetreq)) == 0U) {
    return; /* a non-reset AIRCR write (e.g. priority grouping) */
  }
  s_reboot_request = true;
  (void)uc_emu_stop(uc); /* end the chunk; the run loop performs the reboot */
}

/**
 * @brief UC_HOOK_MEM_WRITE handler for the NVIC ISER / ICER arrays.
 *
 * @details
 * The NVIC set-enable (ISER) and clear-enable (ICER) registers are not normal
 * read/write words: a written 1 sets (ISER) or clears (ICER) that interrupt
 * line and a written 0 has no effect, so independent stores accumulate. The PPB
 * is mapped as plain RAM here, so the raw store would overwrite the whole word
 * and drop every other enabled line -- which breaks any firmware that enables
 * more than one line (e.g. the SCI RXI + TXI + TEI of the interrupt-driven UART
 * path, or several USB controller lines later). This hook decodes the written
 * bits and folds them into board_periph's authoritative enable shadow, which the
 * ICU model consults when deciding whether to pend a line. The raw RAM word is
 * left as-is (nothing reads ISER/ICER back on the modelled paths).
 *
 * @param[in,out] uc    Unicorn engine; unused (state lives in board_periph).
 * @param[in]     type  Memory access type (write); unused.
 * @param[in]     addr  The ISER/ICER word being written.
 * @param[in]     size  Access width in bytes; unused.
 * @param[in]     value The bit-mask the firmware is setting/clearing.
 * @param[in]     user  Hook user pointer; unused.
 * @return Nothing.
 * @since 0.1.0
  * @pre Arguments satisfy the ranges documented for on NVIC en write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu exc scs model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 */
RA8_INTERNAL static void internal_on_nvic_en_write(uc_engine*  uc,
                                                   uc_mem_type type,
                                                   uint64_t    addr,
                                                   int         size,
                                                   int64_t     value,
                                                   void*       user)
{
  (void)uc;
  (void)type;
  (void)size;
  (void)user;
  const bool     is_set = (addr >= (uint64_t)k_nvic_iser_base) &&
                          (addr < ((uint64_t)k_nvic_iser_base + (uint64_t)k_nvic_en_span));
  const uint64_t base   = is_set ? (uint64_t)k_nvic_iser_base : (uint64_t)k_nvic_icer_base;
  const uint32_t word   = (uint32_t)((addr - base) / 4U);
  const uint32_t bits   = (uint32_t)value;
  for (uint32_t b = 0U; b < 32U; b++) {
    if ((bits & (1U << b)) != 0U) {
      board_periph_nvic_set_enable((word * 32U) + b, is_set);
    }
  }
}

/**
 * @brief Data Watchpoint and Trace (DWT) cycle-counter register model.
 *
 * @details
 * The Armv8-M DWT unit exposes a free-running 32-bit cycle counter (CYCCNT)
 * that the firmware uses as a PRIMASK-immune busy-wait time base: `ra8_delay_ms`
 * (libs/ra8_core/src/ra8_time.c) spins on CYCCNT whenever interrupts are masked,
 * which is exactly the early bring-up window (SystemInit runs `cpsid i` and only
 * `ra8_isr_globals_enable` clears PRIMASK later). The PPB is mapped as plain RAM
 * here, so without a model CYCCNT reads a constant 0, the delta never reaches
 * the target, and any masked-context `ra8_delay_ms` spins the whole run budget --
 * which is what stalled `pdm_mic_demo` and `camera_capture` before their first
 * banner. These constants let ::dwt_cyccnt_advance model the counter's advance.
 */
typedef enum : uint64_t {
  k_dwt_ctrl_addr        = 0xE0001000UL, /**< DWT_CTRL (bit 0 CYCCNTENA).            */
  k_dwt_cyccnt_addr      = 0xE0001004UL, /**< DWT_CYCCNT free-running cycle counter. */
  k_dwt_ctrl_cyccntena   = 0x00000001UL, /**< DWT_CTRL.CYCCNTENA: counter enable.    */
  k_dwt_cyccnt_per_chunk = 500000UL,     /**< Cycles charged per outer chunk (==     */
                                         /**< k_run_chunk_insns: retired insns ~= */
                                         /**< elapsed cycles at ~1 IPC). */
} dwt_model_t;

/**
 * @brief Advance the DWT cycle counter by one outer chunk's worth of cycles.
 *
 * @details
 * Models DWT_CYCCNT as the free-running cycle counter the Armv8-M architecture
 * (DDI0553 D1.2.1) specifies: it counts only while `DEMCR.TRCENA` and
 * `DWT_CTRL.CYCCNTENA` are both set. `ra8_time_init` arms both bits, so once the
 * firmware has initialised its time base the counter advances; an app that never
 * enables the cycle counter sees CYCCNT stay at its firmware-written value (zero
 * by default), so this model is inert for every such app and cannot regress it.
 *
 * The per-chunk increment (::k_dwt_cyccnt_per_chunk) equals the busy chunk's
 * instruction budget, i.e. one chunk of execution is charged one chunk of
 * cycles (~1 instruction per cycle on the M85). A read-modify-write is used so a
 * firmware CYCCNT reset (a `DWT->CYCCNT = 0` at init) is honoured and the count
 * simply resumes from there. Called once per outer chunk, in lockstep with the
 * SysTick period the run loop already advances, so time bases stay consistent.
 *
 * @param[in,out] uc Active Unicorn engine (CYCCNT and its enables live in PPB
 *                   RAM, read/written through @p uc).
 * @return Nothing.
 *
 * @pre @p uc has stopped at an instruction boundary (outer-chunk cadence).
 * @pre The PPB (DEMCR / DWT_CTRL / DWT_CYCCNT) is mapped as RAM.
 * @post CYCCNT is advanced iff the trace subsystem and cycle counter are enabled.
 * @post No PPB word other than DWT_CYCCNT is modified.
 * @note Not thread-safe; the run loop is single-threaded host-side.
 * @since 0.1.0
 */
void dwt_cyccnt_advance(uc_engine* uc)
{
  const uint32_t demcr = rd32(uc, (uint64_t)k_scb_demcr_addr);
  if ((demcr & (uint32_t)k_scb_demcr_trcena) == 0U) {
    return; /* Trace subsystem off: CYCCNT does not count. */
  }
  const uint32_t ctrl = rd32(uc, (uint64_t)k_dwt_ctrl_addr);
  if ((ctrl & (uint32_t)k_dwt_ctrl_cyccntena) == 0U) {
    return; /* Cycle counter disabled: leave CYCCNT untouched. */
  }
  const uint32_t next = rd32(uc, (uint64_t)k_dwt_cyccnt_addr) + (uint32_t)k_dwt_cyccnt_per_chunk;
  wr32(uc, (uint64_t)k_dwt_cyccnt_addr, next);
}

/** @brief Implementation of `emu_exc_install_scb_nvic()` -- SCB ctrl + NVIC hooks. */
void emu_exc_install_scb_nvic(uc_engine* uc)
{
  /* Watch the SCB control words AIRCR..CCR with ONE hook (internal_on_scb_ctrl_write): a
   * SYSRESETREQ store triggers a warm reboot and a CCR.DIV_0_TRP store arms the
   * div-0 trap (which then patches divides -- no per-access cost). Both are PPB
   * RAM, so a write-hook is the only way to observe them. */
  static uc_hook s_h_scb;
  (void)uc_hook_add(uc,
                    &s_h_scb,
                    UC_HOOK_MEM_WRITE,
                    (void*)internal_on_scb_ctrl_write,
                    nullptr,
                    (uint64_t)k_scb_aircr,
                    (uint64_t)k_scb_ccr + 3U);
  /* NVIC ISER / ICER are set-enable / clear-enable: fold each written bit into
   * board_periph's enable shadow so enabling several lines does not clobber the
   * earlier ones (see internal_on_nvic_en_write). The PPB is RAM, so this hook is the
   * only place the W1S/W1C semantics can be applied. */
  static uc_hook s_h_nvic_iser;
  static uc_hook s_h_nvic_icer;
  (void)uc_hook_add(uc,
                    &s_h_nvic_iser,
                    UC_HOOK_MEM_WRITE,
                    (void*)internal_on_nvic_en_write,
                    nullptr,
                    (uint64_t)k_nvic_iser_base,
                    (uint64_t)k_nvic_iser_base + (uint64_t)k_nvic_en_span - 1U);
  (void)uc_hook_add(uc,
                    &s_h_nvic_icer,
                    UC_HOOK_MEM_WRITE,
                    (void*)internal_on_nvic_en_write,
                    nullptr,
                    (uint64_t)k_nvic_icer_base,
                    (uint64_t)k_nvic_icer_base + (uint64_t)k_nvic_en_span - 1U);
}

/** @brief Implementation of `emu_exc_reboot_requested()` -- plain flag read. */
bool emu_exc_reboot_requested(void)
{
  return s_reboot_request;
}

/** @brief Implementation of `emu_exc_clear_reboot_request()` -- plain clear. */
void emu_exc_clear_reboot_request(void)
{
  s_reboot_request = false;
}
