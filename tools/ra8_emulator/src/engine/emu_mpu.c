/**
 * @file emu_mpu.c
 * @brief Armv8-M MPU enforcement implementation (see emu_mpu.h)
 *
 * @details
 * Region capture at RLAR writes, RO-region write traps armed/disarmed on
 * CTRL.ENABLE edges, and the MemManage synthesis -- moved verbatim out of
 * the ra8_emulator main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "emu_mpu.h"

#include "emu_engine.h"
#include "emu_exc.h"

/**
 * @enum mpu_field_t
 * @brief Armv8-M MPU register field bits ra8_emulator enforces.
 *
 * @details
 * The firmware programs the MPU via ra8_mpu (RNR -> RBAR -> RLAR per region,
 * then CTRL). ra8_emulator watches those writes (no banked per-region storage
 * in the flat-RAM PPB, so each region is captured at its RLAR write) and,
 * while ``CTRL.ENABLE`` is set, faults a privileged store into any
 * read-only region with a synthesised MemManage -- the part Unicorn's core
 * does not model. Inert unless the firmware enables the MPU.
 */
typedef enum : uint32_t {
  k_mpu_ctrl_enable_bit  = 1U << 0U,    /**< MPU_CTRL.ENABLE.                  */
  k_mpu_rbar_ap_ro_bit   = 1U << 2U,    /**< RBAR.AP[2]=1 -> read-only.        */
  k_mpu_rlar_en_bit      = 1U << 0U,    /**< RLAR.EN region-enable.            */
  k_mpu_addr_mask        = 0xFFFFFFE0U, /**< BASE/LIMIT [31:5] (32-byte gran). */
  k_mpu_region_low_mask  = 0x1FU,       /**< Low 5 bits of an inclusive limit. */
  k_mpu_rnr_mask         = 0x7U,        /**< 8 regions -> 3-bit select.        */
  k_mpu_max_regions      = 8U,          /**< Modelled data regions (DREGION).  */
  k_cfsr_mmfsr_daccviol  = 1U << 1U,    /**< CFSR.MMFSR.DACCVIOL.              */
  k_cfsr_mmfsr_mmarvalid = 1U << 7U,    /**< CFSR.MMFSR.MMARVALID.             */
} mpu_field_t;

/**
 * @struct mpu_region_t
 * @brief One captured MPU data region (base/limit + permission/enable).
 */
typedef struct {
  uint32_t base;  /**< Inclusive region base (32-byte aligned).    */
  uint32_t limit; /**< Inclusive region top.                       */
  bool     ro;    /**< AP encodes read-only (no privileged write). */
  bool     en;    /**< RLAR.EN set.                                */
} mpu_region_t;

static mpu_region_t s_mpu_region[k_mpu_max_regions];  /**< Per-region shadow.    */
static bool         s_mpu_enabled;                    /**< CTRL.ENABLE active.   */
static uc_hook      s_mpu_ro_hook[k_mpu_max_regions]; /**< RO-range write hooks. */
static uint32_t     s_mpu_ro_hook_n;                  /**< Installed hook count. */
static bool         s_mpu_fault;                      /**< RO write trapped.     */
static uint32_t     s_mpu_fault_pc;                   /**< PC of faulting store. */
static uint32_t     s_mpu_fault_addr;                 /**< Address written.      */

/**
 * @brief UC_HOOK_MEM_WRITE handler for a write into an enforced RO MPU region.
 *
 * @details
 * Installed (only while the MPU is enabled) on the exact [base, limit] range
 * of each read-only region. A privileged store into that range is an MPU
 * permission violation; record the faulting PC + address and stop the chunk
 * so the run loop can synthesise a MemManage exception at this boundary --
 * Unicorn's core models no MPU, so this is the software stand-in.
 *
 * @param[in,out] uc    Unicorn engine.
 * @param[in]     type  Access type (write); unused.
 * @param[in]     addr  Address being written (the violating address).
 * @param[in]     size  Access width; unused.
 * @param[in]     value Value being written; unused.
 * @param[in]     user  Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre The MPU is enabled and @p addr lies in a read-only region.
 * @pre The PPB CFSR / MMFAR words are mapped as RAM.
 * @post ::s_mpu_fault is set with the PC / address latched; emulation stopped.
 * @post At most one pending MPU fault is tracked at a time.
 * @note PC is captured here (the store instruction) so the run loop stacks it.
 * @since 0.1.0
 */
static void
on_mpu_ro_write(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* user)
{
  (void)type;
  (void)size;
  (void)value;
  (void)user;
  if (s_mpu_fault) {
    return; /* one pending fault is enough; the run loop takes it next */
  }
  s_mpu_fault      = true;
  s_mpu_fault_addr = (uint32_t)addr;
  s_mpu_fault_pc   = reg_get(uc, UC_ARM_REG_PC);
  (void)uc_emu_stop(uc);
}

/**
 * @brief Install a write hook on every enabled, read-only MPU region.
 *
 * @details
 * Called when the firmware sets MPU_CTRL.ENABLE. Memory hooks are evaluated
 * at access time, so a violating store anywhere in a region's [base, limit]
 * range traps via ::on_mpu_ro_write.
 *
 * @param[in,out] uc Unicorn engine.
 * @return Nothing.
 *
 * @pre The per-region shadow ::s_mpu_region has been captured at RLAR writes.
 * @pre No RO hooks are currently installed.
 * @post ::s_mpu_ro_hook_n hooks are installed (one per RO region).
 * @post Non-RO / disabled regions are left unhooked (writable).
 * @note RW and background-region accesses are never hooked, so cost is nil
 *       for apps that do not lock down a region.
 * @since 0.1.0
 */
static void mpu_install_ro_hooks(uc_engine* uc)
{
  s_mpu_ro_hook_n = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_mpu_max_regions; i++) {
    if (!s_mpu_region[i].en || !s_mpu_region[i].ro) {
      continue;
    }
    (void)uc_hook_add(uc,
                      &s_mpu_ro_hook[s_mpu_ro_hook_n],
                      UC_HOOK_MEM_WRITE,
                      (void*)on_mpu_ro_write,
                      nullptr,
                      (uint64_t)s_mpu_region[i].base,
                      (uint64_t)s_mpu_region[i].limit);
    s_mpu_ro_hook_n++;
  }
}

/**
 * @brief Remove every installed RO-region write hook.
 *
 * @details Called when the firmware clears MPU_CTRL.ENABLE, restoring plain
 *          read/write access to the formerly protected ranges.
 *
 * @param[in,out] uc Unicorn engine.
 * @return Nothing.
 *
 * @pre ::s_mpu_ro_hook[0 .. s_mpu_ro_hook_n) hold live hook handles.
 * @pre The engine is not mid-callback for one of those hooks.
 * @post All RO hooks are deleted and ::s_mpu_ro_hook_n is 0.
 * @post Subsequent writes to the ranges no longer trap.
 * @note Idempotent when no hooks are installed.
 * @since 0.1.0
 */
static void mpu_remove_ro_hooks(uc_engine* uc)
{
  for (uint32_t i = 0U; i < s_mpu_ro_hook_n; i++) {
    (void)uc_hook_del(uc, s_mpu_ro_hook[i]);
  }
  s_mpu_ro_hook_n = 0U;
}

/**
 * @brief UC_HOOK_MEM_WRITE handler for MPU_RLAR -- capture one region.
 *
 * @details
 * ra8_mpu programs each region as RNR -> RBAR -> RLAR, so at the RLAR write the
 * region's RNR and RBAR already sit in the (flat-RAM) PPB; read them back and
 * record base / limit / permission / enable into the per-region shadow. The
 * flat PPB has no banked per-region storage, so this capture-at-RLAR is how
 * ra8_emulator reconstructs the table the real MPU keeps internally.
 *
 * @param[in,out] uc    Unicorn engine.
 * @param[in]     type  Access type (write); unused.
 * @param[in]     addr  Observed address (MPU_RLAR); unused.
 * @param[in]     size  Access width; unused.
 * @param[in]     value The RLAR value being written.
 * @param[in]     user  Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre RNR + RBAR for this region were written before this RLAR store.
 * @pre The PPB RNR / RBAR words are mapped as RAM.
 * @post ::s_mpu_region[RNR] mirrors the region (en=0 for an RLAR=0 disable).
 * @post No enforcement changes until MPU_CTRL.ENABLE is (re)written.
 * @note AP[2]=1 in RBAR marks the region read-only (no privileged store).
 * @since 0.1.0
 */
static void on_mpu_rlar_write(uc_engine*  uc,
                              uc_mem_type type,
                              uint64_t    addr,
                              int         size,
                              int64_t     value,
                              void*       user)
{
  (void)type;
  (void)addr;
  (void)size;
  (void)user;
  const uint32_t rnr      = rd32(uc, (uint64_t)k_mpu_rnr) & (uint32_t)k_mpu_rnr_mask;
  const uint32_t rbar     = rd32(uc, (uint64_t)k_mpu_rbar);
  const uint32_t rlar     = (uint32_t)value;
  s_mpu_region[rnr].base  = rbar & (uint32_t)k_mpu_addr_mask;
  s_mpu_region[rnr].limit = (rlar & (uint32_t)k_mpu_addr_mask) | (uint32_t)k_mpu_region_low_mask;
  s_mpu_region[rnr].ro    = (rbar & (uint32_t)k_mpu_rbar_ap_ro_bit) != 0U;
  s_mpu_region[rnr].en    = (rlar & (uint32_t)k_mpu_rlar_en_bit) != 0U;
}

/**
 * @brief UC_HOOK_MEM_WRITE handler for MPU_CTRL -- arm / disarm enforcement.
 *
 * @details
 * On the ENABLE edge, install write hooks over every read-only region; on the
 * disable edge, remove them. Edge-tracked via ::s_mpu_enabled so repeated
 * writes of the same state are no-ops.
 *
 * @param[in,out] uc    Unicorn engine.
 * @param[in]     type  Access type (write); unused.
 * @param[in]     addr  Observed address (MPU_CTRL); unused.
 * @param[in]     size  Access width; unused.
 * @param[in]     value The CTRL value being written.
 * @param[in]     user  Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre The per-region shadow has been captured for the regions in use.
 * @pre The engine permits uc_hook_add / uc_hook_del from a callback.
 * @post Enforcement hooks match the new ENABLE state.
 * @post ::s_mpu_enabled tracks the latest ENABLE bit.
 * @note Apps that never enable the MPU install no hooks (zero overhead).
 * @since 0.1.0
 */
static void on_mpu_ctrl_write(uc_engine*  uc,
                              uc_mem_type type,
                              uint64_t    addr,
                              int         size,
                              int64_t     value,
                              void*       user)
{
  (void)type;
  (void)addr;
  (void)size;
  (void)user;
  const bool enable = ((uint32_t)value & (uint32_t)k_mpu_ctrl_enable_bit) != 0U;
  if (enable && !s_mpu_enabled) {
    mpu_install_ro_hooks(uc);
    s_mpu_enabled = true;
  } else if (!enable && s_mpu_enabled) {
    mpu_remove_ro_hooks(uc);
    s_mpu_enabled = false;
  }
}

/**
 * @brief Synthesise a MemManage (#4) fault for a trapped RO-region write.
 *
 * @details
 * Called by the run loop after ::on_mpu_ro_write latched a violation. Latches
 * CFSR.MMFSR.DACCVIOL + MMARVALID and MMFAR (so a fault handler -- and the HIL
 * alive probe -- see the architectural status), forces PC back to the faulting
 * store so exc_enter stacks *that* address (a recovering handler skips exactly
 * one store), and vectors into the application's MemManage_Handler. If no
 * handler is installed the violation is dropped (no escalation modelled).
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base if VTOR reads as 0.
 * @return Nothing.
 *
 * @pre ::s_mpu_fault_pc / ::s_mpu_fault_addr hold the trapped store.
 * @pre The PPB CFSR / MMFAR words and the vector table are mapped as RAM.
 * @post On a valid vector, the core is in the MemManage handler with the
 *       basic frame stacked (stacked PC == the faulting store).
 * @post CFSR.MMFSR and MMFAR reflect a data-access violation.
 * @note Faithful to the recovering-handler contract of mpu_partition_simple.
 * @since 0.1.0
 */
void mpu_synth_memmanage(uc_engine* uc, uint32_t vtor_base)
{
  const uint32_t cfsr = rd32(uc, (uint64_t)k_scb_cfsr) | (uint32_t)k_cfsr_mmfsr_daccviol |
                        (uint32_t)k_cfsr_mmfsr_mmarvalid;
  wr32(uc, (uint64_t)k_scb_cfsr, cfsr);
  wr32(uc, (uint64_t)k_scb_mmfar, s_mpu_fault_addr);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &s_mpu_fault_pc);
  const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_memmanage);
  if (handler != 0U) {
    exc_enter(uc, (uint32_t)k_exc_memmanage, handler);
  }
}

/** @brief Implementation of `emu_mpu_install()` -- RLAR capture + CTRL edges. */
void emu_mpu_install(uc_engine* uc)
{
  /* Armv8-M MPU enforcement: capture each region at its RLAR write, then arm /
   * disarm read-only-region write traps on MPU_CTRL.ENABLE edges. Inert unless
   * the firmware programs and enables the MPU (mpu_partition_simple); a trapped
   * store synthesises MemManage in the run loop (see on_mpu_ro_write). */
  static uc_hook s_h_mpu_rlar;
  static uc_hook s_h_mpu_ctrl;
  (void)uc_hook_add(uc,
                    &s_h_mpu_rlar,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_mpu_rlar_write,
                    nullptr,
                    (uint64_t)k_mpu_rlar,
                    (uint64_t)k_mpu_rlar + 3U);
  (void)uc_hook_add(uc,
                    &s_h_mpu_ctrl,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_mpu_ctrl_write,
                    nullptr,
                    (uint64_t)k_mpu_ctrl,
                    (uint64_t)k_mpu_ctrl + 3U);
}

/** @brief Implementation of `emu_mpu_fault_pending()` -- plain flag read. */
bool emu_mpu_fault_pending(void)
{
  return s_mpu_fault;
}

/** @brief Implementation of `emu_mpu_clear_fault()` -- plain flag clear. */
void emu_mpu_clear_fault(void)
{
  s_mpu_fault = false;
}
