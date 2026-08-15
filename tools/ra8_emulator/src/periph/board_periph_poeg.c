/**
 * @file board_periph_poeg.c
 * @brief Port Output Enable for GPT (POEG) safe-shutoff register model
 *
 * @details
 * Models the RA8D2 POEG block (ra8_poeg_regs.h, ra8_poeg.c) at @c 0x40212000
 * so the safe-shutoff example (poeg_safe_shutoff) can assert an output-disable
 * request, read back that the GPT outputs are in the disabled (high-impedance)
 * state, then clear the request and read back that the outputs are re-enabled.
 * Without this block the POEGG window falls through to the sparse fallback,
 * where the derived output-disable STATE flag (POEGG.ST) never asserts, so the
 * app could not observe the shutoff it just requested and its liveness counter
 * would never advance -- a real modelling gap, reported as an EIL FAIL.
 *
 * Four POEG groups (POEG0..POEG3) live at @c 0x40212000 with a @c 0x100 stride;
 * each group exposes a single 32-bit @c POEGG register (offset 0) carrying the
 * trigger-request flags, the trigger enables, and the read-only output-disable
 * STATE flag.
 *
 * Register semantics modelled (HUM Ch 21.2.1 "POEGG : POEG Group n Setting
 * Register", p 872):
 *  - @c SSF (bit 3) is the software-stop request: writing 1 requests the
 *    GPT-output disable; writing 0 clears the request. @c PIDF / @c IOCF /
 *    @c OSTPF (bits 0..2) are the pin / output-short / oscillation-stop request
 *    flags -- write-0-to-clear latches a real trigger source would set.
 *  - @c ST (bit 16) is the OUTPUT-DISABLE STATE flag: it is read-only and
 *    reflects whether ANY request flag is currently set. The model recomputes
 *    it on every write and ignores an attempt to write it, exactly as the
 *    hardware ORs the active requests into ST. When ST is 1 the group's GPT
 *    outputs are forced high-impedance; clearing every request returns ST to 0
 *    and the outputs are driven again.
 *
 * The block is register-level only ("narrow"): it does not cross-wire ST into
 * the GPT counter model, because the safe-shutoff proof the EIL gate checks is
 * the register-observable ST transition (assert -> high-Z -> clear -> enabled),
 * which is precisely what ra8_emulator can verify with no physical pin to scope.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"
#include "emu_host_io_internal.h"

/** @brief POEG block geometry (ra8_poeg_regs.h, r_poeg_regs_t). */
typedef enum : uint64_t {
  k_poeg_base      = 0x40212000UL,  /**< POEG0 base (HUM Ch 21.2.1). */
  k_poeg_stride    = 0x100UL,       /**< Bytes per POEG group.       */
  k_poeg_count     = 4UL,           /**< POEG0..POEG3.               */
  k_poeg_span      = 0x100UL * 4UL, /**< Covers all four groups.     */
  k_poeg_off_poegg = 0x00UL,        /**< POEGG setting + status.     */
} poeg_geom_t;

/** @brief POEGG field masks (ra8_poeg.h enum mirror). */
typedef enum : uint32_t {
  k_poegg_pidf = 0x00000001UL, /**< Port-input request flag (bit 0).    */
  k_poegg_iocf = 0x00000002UL, /**< Output-short request flag (bit 1).  */
  k_poegg_ostp = 0x00000004UL, /**< Osc-stop request flag (bit 2).      */
  k_poegg_ssf  = 0x00000008UL, /**< Software-stop request flag (bit 3). */
  k_poegg_st   = 0x00010000UL, /**< Output-disable STATE flag (bit 16). */
  k_poegg_trig = k_poegg_pidf | k_poegg_iocf | k_poegg_ostp | k_poegg_ssf,
  /**< Any request flag: ST is the OR of these. */
} poegg_field_t;

/** @brief Per-tick order slot for the POEG block (report order only). */
typedef enum : uint32_t {
  k_poeg_block_order = 22U, /**< Just after the GPT/AGT timers (20). */
} poeg_order_t;

/** @brief POEG register state. */
typedef struct {
  uint32_t poegg[k_poeg_count]; /**< Per-group POEGG shadow (ST derived). */
  uint32_t asserts;             /**< 0->1 ST transitions (shutoffs).      */
  uint32_t clears;              /**< 1->0 ST transitions (re-enables).    */
} poeg_state_t;

static poeg_state_t s_poeg;

/**
 * @brief Recompute the read-only ST bit from the active request flags.
 *
 * @details ST reflects whether any request flag is set, so the model ORs it in
 * when @c k_poegg_trig is non-zero and clears it otherwise -- the hardware's
 * output-disable STATE latch behaviour with no physical pin to scope.
  * @param[in] writable Writable input used by the operation.
 * @return The poeg with st result produced by the board periph poeg model.
 * @retval value The operation-specific poeg with st value.
 * @pre Arguments satisfy the ranges documented for poeg with st. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph poeg model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_poeg_with_st(uint32_t writable)
{
  if ((writable & (uint32_t)k_poegg_trig) != 0U) {
    return writable | (uint32_t)k_poegg_st;
  }
  return writable & ~(uint32_t)k_poegg_st;
}

/**
 * @brief Reset the POEG model to power-on state.
 * @details Reset the poeg model to power-on state; this step is contained within the board periph poeg model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for poeg reset. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph poeg model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_poeg_reset(void)
{
  s_poeg = (poeg_state_t){};
}

/**
 * @brief MMIO read inside the POEG window.
 * @details MMIO read inside the poeg window; this step is contained within the board periph poeg model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @return The poeg read result produced by the board periph poeg model.
 * @retval value The operation-specific poeg read value.
 * @pre Arguments satisfy the ranges documented for poeg read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph poeg model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_poeg_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off   = addr - (uint64_t)k_poeg_base;
  const uint64_t group = off / (uint64_t)k_poeg_stride;
  const uint64_t reg   = off % (uint64_t)k_poeg_stride;
  if (group >= (uint64_t)k_poeg_count) {
    return 0U;
  }
  if (reg == (uint64_t)k_poeg_off_poegg) {
    return s_poeg.poegg[group];
  }
  return 0U;
}

/**
 * @brief MMIO write inside the POEG window.
 * @details MMIO write inside the poeg window; this step is contained within the board periph poeg model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for poeg write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph poeg model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_poeg_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off   = addr - (uint64_t)k_poeg_base;
  const uint64_t group = off / (uint64_t)k_poeg_stride;
  const uint64_t reg   = off % (uint64_t)k_poeg_stride;
  if (group >= (uint64_t)k_poeg_count) {
    return;
  }
  if (reg != (uint64_t)k_poeg_off_poegg) {
    return;
  }
  /* ST is read-only: strip any incoming ST, then re-derive it from the
   * request flags so a software-stop assert / clear moves ST as hardware would. */
  const uint32_t writable = (uint32_t)value & ~(uint32_t)k_poegg_st;
  const uint32_t prev     = s_poeg.poegg[group];
  const uint32_t next     = internal_poeg_with_st(writable);
  s_poeg.poegg[group]     = next;
  if (((prev & (uint32_t)k_poegg_st) == 0U) && ((next & (uint32_t)k_poegg_st) != 0U)) {
    s_poeg.asserts++;
  }
  if (((prev & (uint32_t)k_poegg_st) != 0U) && ((next & (uint32_t)k_poegg_st) == 0U)) {
    s_poeg.clears++;
  }
}

/**
 * @brief End-of-run POEG section: shutoff assert / re-enable counts.
 * @details End-of-run poeg section: shutoff assert / re-enable counts; this step is contained within the board periph poeg model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for poeg report. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph poeg model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_poeg_report(void)
{
  if ((s_poeg.asserts == 0U) && (s_poeg.clears == 0U)) {
    return; /* Untouched: stay quiet. */
  }
  (void)priv_emu_io_errf("  POEG          : shutoffs=%u re-enables=%u\n",
                         s_poeg.asserts,
                         s_poeg.clears);
}

/** @brief POEG block descriptor (self-registered with the core). */
static const board_periph_block_t s_k_poeg_block = {
  .base   = (uint64_t)k_poeg_base,
  .span   = (uint64_t)k_poeg_span,
  .order  = (uint32_t)k_poeg_block_order,
  .read   = internal_poeg_read,
  .write  = internal_poeg_write,
  .tick   = nullptr,
  .reset  = internal_poeg_reset,
  .report = internal_poeg_report,
  .name   = "POEG",
};

/** @brief Register the POEG block before main (host constructor). */
[[gnu::constructor]] RA8_INTERNAL static void internal_poeg_block_register(void)
{
  board_periph_register_block(&s_k_poeg_block);
}
