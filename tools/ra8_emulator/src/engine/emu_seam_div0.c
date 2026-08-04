/**
 * @file emu_seam_div0.c
 * @brief Divide-by-zero UsageFault (CCR.DIV_0_TRP) CPU-model seam
 *
 * @details
 * Unicorn's Cortex-M core executes UDIV/SDIV with the Arm default
 * divide-by-zero result (quotient 0) and never raises the UsageFault real
 * silicon takes when CCR.DIV_0_TRP is set. This seam tracks every UDIV/SDIV
 * site at setup and -- only once the firmware opts in by setting DIV_0_TRP --
 * overwrites those sites with UDF so each divide traps through the
 * invalid-instruction hook, which either latches the decoded UsageFault
 * (zero divisor) or emulates the divide in software and continues. Moved
 * verbatim out of the ra8_emulator main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdio.h>
#include <string.h>

#include "emu_elf.h"
#include "emu_engine.h"
#include "emu_exc.h"
#include "emu_seams.h"

/** @brief Trapping div-0 latched (the run loop synthesises the UsageFault). */
static bool s_div0_fault;

/** @brief PC of the divide that trapped (stacked by the synthesised fault). */
static uint32_t s_div0_fault_pc;

/** @brief Count of divide-by-zero UsageFaults synthesised this run. */
static uint64_t s_div0_traps;

/*
 * =============================================================================
 * Divide-by-zero UsageFault (CCR.DIV_0_TRP) -- CPU-model seam.
 *
 * Unicorn's Cortex-M core executes UDIV/SDIV with the Arm default divide-by-zero
 * result (quotient 0) and never raises the UsageFault that real Armv7-M/Armv8-M
 * silicon takes when CCR.DIV_0_TRP is set. ra8_emulator closes that gap by scanning
 * the image for every UDIV/SDIV site and -- only after the firmware sets
 * CCR.DIV_0_TRP (watched via on_scb_ctrl_write) -- overwriting those sites with an
 * undefined instruction (UDF). Each divide then traps through the already-armed
 * ::on_invalid_insn hook into ::emulate_div0_patched, which either takes a decoded
 * UsageFault (divisor zero) or emulates the divide in software (::div0_quotient)
 * and continues. Patching -- rather than a per-site UC_HOOK_CODE -- is the point:
 * a code hook disables Unicorn's engine-wide block chaining and would roughly
 * quarter throughput for ANY busy firmware that arms the trap, whereas the invalid
 * hook has no such cost. A firmware that never opts in keeps its native divides
 * and pays nothing. Faithful in both directions: no faked trap, no masked one.
 * =============================================================================
 */

/**
 * @brief UDF.W #0 -- a permanently-undefined 32-bit Thumb-2 instruction (LE bytes).
 *
 * @details Armv8-M ``UDF.W #0`` is 0xF7F0A000; stored little-endian it is the
 * byte sequence written over an armed divide so its execution raises the
 * undefined-instruction trap that ::emulate_div0_patched services. Byte 2 is 0.
 */
static const uint8_t k_div0_udf[k_div0_insn_len] = {
  (uint8_t)k_div0_udf_b0,
  (uint8_t)k_div0_udf_b1,
  0U,
  (uint8_t)k_div0_udf_b3,
};

/**
 * @struct div0_site_t
 * @brief One tracked UDIV/SDIV site: its address and original encoding halfwords.
 * @details The original encoding is kept so ::emulate_div0_patched can recover the
 * operand registers after the site's bytes have been overwritten with UDF.
 */
typedef struct {
  uint32_t va;  /**< Divide-instruction virtual address. */
  uint16_t hw1; /**< Original first halfword.            */
  uint16_t hw2; /**< Original second halfword.           */
} div0_site_t;

/** @brief Div-0 seam sizing: max tracked divide sites (real counts tiny). */
enum : uint32_t {
  k_div0_sites_max = 4096U, /**< Div0 sites maximum. */
};
static div0_site_t s_div0_site[k_div0_sites_max]; /**< Tracked divide sites.        */
static uint32_t    s_div0_site_n;                 /**< Count of tracked sites.      */
static bool        s_div0_armed;                  /**< Armed sites overwritten UDF. */

/**
 * @brief Decode a Thumb-2 halfword pair as UDIV/SDIV, recovering all operands.
 *
 * @details
 * Matches the T1 encodings ``1111 1011 1011 Rn : 1111 Rd 1111 Rm`` (UDIV) and
 * ``1111 1011 1001 Rn : 1111 Rd 1111 Rm`` (SDIV) from the Armv8-M Architecture
 * Reference Manual, recovering Rn (dividend), Rd (destination), Rm (divisor) and
 * whether the divide is signed. The two fixed nibbles in hw2 ([15:12] and [7:4]
 * == 1111) are verified so a scan false-positive on data cannot be taken as a
 * divide.
 *
 * The architecture makes UDIV/SDIV with d, n, or m == 13 (SP) or 15 (PC)
 * UNPREDICTABLE, so a real compiler never emits those encodings; a match with
 * any of Rn/Rd/Rm in {13, 15} is therefore a scan false-positive. This matters
 * because the sweep runs on 2-byte boundaries: the high nibbles of an adjacent
 * BL / B.W pair (each halfword 0xFxxx) read as "SDIV ..., ..., r15", and if that
 * window were armed, ::div0_patch_sites would overwrite the two straddled real
 * branches with UDF -- corrupting executed code, not an unreachable divide.
 * Rejecting the reserved register operands keeps the seam matching only the
 * encodings the core can actually take.
 *
 * @param[in]  hw1        First (low-address) instruction halfword.
 * @param[in]  hw2        Second instruction halfword.
 * @param[out] out_rn     Dividend register index [0, 15] on a match.
 * @param[out] out_rd     Destination register index [0, 15] on a match.
 * @param[out] out_rm     Divisor register index [0, 15] on a match.
 * @param[out] out_signed True for SDIV, false for UDIV, on a match.
 *
 * @return Whether @p hw1 / @p hw2 encode a UDIV or SDIV.
 * @retval true  A divide was decoded; all outputs are valid.
 * @retval false Not a divide; the outputs are untouched.
 *
 * @pre All output pointers are non-NULL.
 * @pre @p hw1 / @p hw2 are the two halfwords of one candidate site.
 * @post The outputs are written only when true is returned.
 * @post No engine or global state is mutated (pure).
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static bool udiv_sdiv_decode(uint16_t  hw1,
                             uint16_t  hw2,
                             uint32_t* out_rn,
                             uint32_t* out_rd,
                             uint32_t* out_rm,
                             bool*     out_signed)
{
  if ((out_rn == nullptr) || (out_rd == nullptr) || (out_rm == nullptr) ||
      (out_signed == nullptr)) {
    return false;
  }
  const bool is_udiv = ((uint32_t)hw1 & (uint32_t)k_div0_hw1_mask) == (uint32_t)k_div0_hw1_udiv;
  const bool is_sdiv = ((uint32_t)hw1 & (uint32_t)k_div0_hw1_mask) == (uint32_t)k_div0_hw1_sdiv;
  if (!is_udiv && !is_sdiv) {
    return false;
  }
  if (((uint32_t)hw2 & (uint32_t)k_div0_hw2_mask) != (uint32_t)k_div0_hw2_fixed) {
    return false;
  }
  const uint32_t rn = (uint32_t)hw1 & (uint32_t)k_div0_reg_mask;
  const uint32_t rd = ((uint32_t)hw2 >> (uint32_t)k_div0_rd_shift) & (uint32_t)k_div0_reg_mask;
  const uint32_t rm = (uint32_t)hw2 & (uint32_t)k_div0_reg_mask;
  /* d/n/m == SP or PC is UNPREDICTABLE for UDIV/SDIV: never a real divide, so
   * this window is a false positive straddling two instructions. Reject it so
   * the arming pass does not overwrite executed code with UDF. */
  if ((rn == (uint32_t)k_div0_reg_sp) || (rn == (uint32_t)k_div0_reg_pc) ||
      (rd == (uint32_t)k_div0_reg_sp) || (rd == (uint32_t)k_div0_reg_pc) ||
      (rm == (uint32_t)k_div0_reg_sp) || (rm == (uint32_t)k_div0_reg_pc)) {
    return false;
  }
  *out_rn     = rn;
  *out_rd     = rd;
  *out_rm     = rm;
  *out_signed = is_sdiv;
  return true;
}

/**
 * @brief Compute a UDIV/SDIV quotient with the Arm div-by-zero + overflow rules.
 *
 * @param[in] vn        Dividend value.
 * @param[in] vm        Divisor value (already known non-trapping: non-zero, or
 *                      DIV_0_TRP clear so a zero divisor yields the Arm default 0).
 * @param[in] is_signed True for SDIV, false for UDIV.
 * @return The 32-bit quotient the core would have produced.
 * @retval 0 When @p vm is zero (Arm default divide-by-zero result).
 *
 * @pre None.
 * @pre None.
 * @post No state is mutated (pure).
 * @post Signed INT32_MIN / -1 saturates to INT32_MIN (Arm SDIV overflow rule).
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static uint32_t div0_quotient(uint32_t vn, uint32_t vm, bool is_signed)
{
  if (vm == 0U) {
    return 0U; /* Arm default when DIV_0_TRP is clear. */
  }
  if (!is_signed) {
    return vn / vm;
  }
  if ((vn == (uint32_t)k_div0_int32_min) && ((int32_t)vm == -1)) {
    return (uint32_t)k_div0_int32_min; /* Arm SDIV overflow: INT32_MIN / -1 = INT32_MIN. */
  }
  return (uint32_t)((int32_t)vn / (int32_t)vm);
}

/**
 * @brief Service an undefined-instruction trap that landed on an armed divide.
 *
 * @details
 * Called from ::on_invalid_insn. Divide-by-zero trapping is modelled by
 * overwriting each divide with UDF once the firmware sets CCR.DIV_0_TRP -- unlike
 * a per-site UC_HOOK_CODE this does NOT disable Unicorn's translation-block
 * chaining, so a firmware that arms the trap runs its steady state at the baseline
 * rate. When @p pc is one of those patched sites this recovers the original
 * encoding, reads the operands, and either (a) latches ::s_div0_fault when the
 * divisor is zero and DIV_0_TRP is set -- so the run loop synthesises the decoded
 * UsageFault with @p pc stacked -- or (b) computes the quotient in software
 * (::div0_quotient), writes Rd and steps PC past the 4-byte instruction. A trap at
 * any other address is not ours.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the trapping instruction.
 * @param[in]     code The 4 bytes at @p pc (the UDF, unused -- the original
 *                     encoding comes from ::s_div0_site).
 * @return Whether the trap was an armed divide this handler serviced.
 * @retval true  @p pc was a patched divide; register/PC state (or the fault latch)
 *               was updated and the caller should stop + resume.
 * @retval false @p pc is not a patched divide; try the next handler.
 *
 * @pre ::s_div0_site[0 .. s_div0_site_n) hold the armed sites.
 * @pre The PPB CCR word is mapped as RAM.
 * @post On a trapping div-0 ::s_div0_fault is set with @p pc; else Rd and PC are
 *       advanced.
 * @note Not thread-safe (single engine).
 * @since 0.1.0
 */
bool emulate_div0_patched(uc_engine* uc, uint32_t pc, const uint8_t code[4])
{
  (void)code;
  const div0_site_t* site = nullptr;
  for (uint32_t i = 0U; i < s_div0_site_n; i++) {
    if (s_div0_site[i].va == pc) {
      site = &s_div0_site[i];
      break;
    }
  }
  if (site == nullptr) {
    return false; /* not an armed divide -- let the other invalid-insn handlers try. */
  }
  uint32_t rn   = 0U;
  uint32_t rd   = 0U;
  uint32_t rm   = 0U;
  bool     sign = false;
  if (!udiv_sdiv_decode(site->hw1, site->hw2, &rn, &rd, &rm, &sign)) {
    return false; /* a tracked non-divide (scan false-positive): not ours to service. */
  }
  uint32_t vn = 0U;
  uint32_t vm = 0U;
  (void)uc_reg_read(uc, k_arm_reg_id[rn], &vn);
  (void)uc_reg_read(uc, k_arm_reg_id[rm], &vm);
  const uint32_t ccr = rd32(uc, (uint64_t)k_scb_ccr);
  if ((vm == 0U) && ((ccr & (uint32_t)k_ccr_div_0_trp) != 0U)) {
    s_div0_fault    = true;
    s_div0_fault_pc = pc; /* stacked PC == the divide, as real hardware does.          */
    return true;          /* PC left at the site; the run loop synthesises UsageFault. */
  }
  const uint32_t q = div0_quotient(vn, vm, sign);
  (void)uc_reg_write(uc, k_arm_reg_id[rd], &q);
  uint32_t next = pc + (uint32_t)k_div0_insn_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  /* Emulating one divide consumes no modelled time: mark the stop a zero-time
   * seam relaunch so the inner run loop re-enters at `next` WITHOUT advancing
   * SysTick or charging an outer chunk (like on_sdmmc_read_block). Without this,
   * a divide in a hot loop (e.g. ra8_keycache_put's modulo hashing) charges one
   * of the 40000 chunks per execution, exhausting the budget mid-render. The
   * zero-divisor trap path above returns earlier and is handled by the
   * s_div0_fault branch, which is already zero-time. */
  emu_seam_request_relaunch();
  return true;
}

/**
 * @brief Overwrite every tracked divide with UDF so divide-by-zero can trap.
 *
 * @details
 * Called from ::on_scb_ctrl_write the first time the firmware sets CCR.DIV_0_TRP.
 * The patch is deferred to opt-in so a firmware that never arms the trap keeps its
 * original divides (native, quotient-0 semantics) and pays nothing. Patching to
 * UDF -- rather than installing a UC_HOOK_CODE at each site -- is deliberate:
 * UC_HOOK_CODE disables Unicorn's engine-wide block chaining and would roughly
 * quarter the throughput of any busy loop in a DIV_0_TRP firmware, whereas the
 * already-armed undefined-instruction hook (::on_invalid_insn) has no such cost.
 * Idempotent via ::s_div0_armed. Re-applied after a warm reboot re-loads the image
 * (see the run loop's reboot paths, which clear ::s_div0_armed).
 *
 * @param[in,out] uc Unicorn engine whose memory is patched.
 * @return Nothing.
 *
 * @pre ::s_div0_site[0 .. s_div0_site_n) hold valid divide-site addresses.
 * @pre @p uc permits uc_mem_write to the (host-side) code image.
 * @post ::s_div0_armed is true and each site holds ::k_div0_udf.
 * @post A no-op when already armed.
 * @note Not thread-safe (single engine).
 * @since 0.1.0
 */
void div0_patch_sites(uc_engine* uc)
{
  if (s_div0_armed) {
    return;
  }
  for (uint32_t i = 0U; i < s_div0_site_n; i++) {
    (void)uc_mem_write(uc, (uint64_t)s_div0_site[i].va, k_div0_udf, sizeof(k_div0_udf));
  }
  s_div0_armed = true;
  (void)fprintf(stderr,
                "  div-0 seam: CCR.DIV_0_TRP set -- patched %u UDIV/SDIV site(s)\n",
                (unsigned)s_div0_site_n);
}

/**
 * @brief Record every UDIV/SDIV site in one executable segment.
 *
 * @details
 * Steps the segment a halfword at a time, since Thumb-2 instructions are
 * halfword-aligned and a 32-bit encoding may start at any even offset. Sites
 * are only recorded here; arming (the UDF overwrite) happens when the firmware
 * sets CCR.DIV_0_TRP.
 *
 * @param[in] seg Segment to scan.
 * @param[in] ctx Unused; the site table is file-scope state.
 *
 * @return True to continue with the next segment, false once the site cap is
 *         reached (there is no point scanning further).
 *
 * @pre @p seg is non-NULL.
 * @pre `seg->bytes .. seg->bytes + seg->filesz` lies inside the image.
 * @post `s_div0_site_n` is bounded by @ref k_div0_sites_max.
 * @post Each recorded site carries its virtual address and both halfwords.
 *
 * @note Not thread-safe; the emulator is single-threaded host-side.
 */
static bool div0_scan_segment(const elf_exec_segment_t* seg, void* ctx)
{
  (void)ctx;
  for (uint32_t off = 0U; (off + (uint32_t)k_div0_insn_len) <= seg->filesz; off += 2U) {
    const uint8_t* p    = seg->bytes + off;
    const uint16_t hw1  = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    const uint16_t hw2  = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
    uint32_t       rn   = 0U;
    uint32_t       rd   = 0U;
    uint32_t       rm   = 0U;
    bool           sign = false;
    if (!udiv_sdiv_decode(hw1, hw2, &rn, &rd, &rm, &sign)) {
      continue;
    }
    if (s_div0_site_n >= (uint32_t)k_div0_sites_max) {
      (void)fprintf(stderr, "  div-0 seam: site cap %u reached\n", (unsigned)k_div0_sites_max);
      return false;
    }
    s_div0_site[s_div0_site_n] =
      (div0_site_t){.va = (uint32_t)seg->vaddr + off, .hw1 = hw1, .hw2 = hw2};
    s_div0_site_n++;
  }
  return true;
}

/**
 * @brief Scan the image for UDIV/SDIV sites so the div-0 trap can arm later.
 *
 * @details
 * Walks the ELF32 PT_LOAD executable segments on 2-byte boundaries for the
 * UDIV/SDIV encoding (::udiv_sdiv_decode), recording each site's VMA (p_vaddr
 * based, so a ramfunc is tracked at its execution address) and its original
 * halfwords. Nothing is patched here; the always-on SCB control-write watcher
 * (::on_scb_ctrl_write) overwrites the sites with UDF via ::div0_patch_sites only
 * if the firmware sets CCR.DIV_0_TRP. Tracked for every core (UDIV/SDIV exist on
 * the M85 and the M33 alike). A scan false-positive is harmless: the site is only
 * ever patched after opt-in, and ::emulate_div0_patched re-decodes before acting.
 *
 * @param[in] elf In-memory ELF image (still alive at call time).
 * @param[in] len Length of @p elf in bytes.
 * @return Nothing.
 *
 * @pre @p elf is a 32-bit ARM ELF (already validated by load_elf).
 * @pre @p len is the true byte length of @p elf.
 * @post ::s_div0_site holds up to ::k_div0_sites_max tracked divide sites.
 * @post No site is patched yet (armed later by on_scb_ctrl_write).
 * @note Not thread-safe; call once during setup before the run loop.
 * @since 0.1.0
 */
void div0_seam_install(const uint8_t* elf, long len)
{
  s_div0_site_n = 0U;
  s_div0_armed  = false;
  (void)elf_foreach_exec_segment(elf, len, div0_scan_segment, nullptr);
  /* The CCR write that arms these sites is caught by on_scb_ctrl_write; arming
   * overwrites each with UDF so its execution traps through on_invalid_insn. */
  if (s_div0_site_n > 0U) {
    (void)fprintf(stderr,
                  "  div-0 seam: %u UDIV/SDIV site(s) tracked; armed on CCR.DIV_0_TRP\n",
                  (unsigned)s_div0_site_n);
  }
}

/**
 * @brief Synthesise a UsageFault (#6) for a trapped divide-by-zero.
 *
 * @details
 * Called by the run loop after ::emulate_div0_patched latched a trapping div-0.
 * Latches CFSR.UFSR.DIVBYZERO (so a fault handler -- and the HIL alive probe -- see
 * the
 * architectural status: ``cfsr == 0x02000000``), forces PC back to the faulting
 * divide so ::exc_enter stacks *that* address (a real div-0 UsageFault stacks the
 * divide), and vectors into the application's UsageFault_Handler. If no handler
 * is installed the trap is dropped (no HardFault escalation is modelled -- the
 * firmware that arms DIV_0_TRP always installs the handler).
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base if VTOR reads as 0.
 * @return Nothing.
 *
 * @pre ::s_div0_fault_pc holds the trapping divide's address.
 * @pre The PPB CFSR word and the vector table are mapped as RAM.
 * @post On a valid vector, the core is in the UsageFault handler with the basic
 *       frame stacked (stacked PC == the faulting divide) and IPSR == 6.
 * @post CFSR.UFSR.DIVBYZERO reads set and ::s_div0_traps is incremented.
 * @note Faithful to Armv8-M CCR.DIV_0_TRP semantics; no time advances (a fault
 *       is synchronous).
 * @since 0.1.0
 */
void div0_synth_usagefault(uc_engine* uc, uint32_t vtor_base)
{
  const uint32_t cfsr = rd32(uc, (uint64_t)k_scb_cfsr) | (uint32_t)k_div0_cfsr_divzero;
  wr32(uc, (uint64_t)k_scb_cfsr, cfsr);
  const uint32_t fault_pc = emu_div0_fault_pc();
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &fault_pc);
  const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_usagefault);
  if (handler != 0U) {
    emu_div0_count_trap();
    exc_enter(uc, (uint32_t)k_exc_usagefault, handler);
  }
}

/** @brief Implementation of `emu_div0_fault_pending()` -- plain flag read. */
bool emu_div0_fault_pending(void)
{
  return s_div0_fault;
}

/** @brief Implementation of `emu_div0_clear_fault()` -- plain flag clear. */
void emu_div0_clear_fault(void)
{
  s_div0_fault = false;
}

/** @brief Implementation of `emu_div0_fault_pc()` -- plain state read. */
uint32_t emu_div0_fault_pc(void)
{
  return s_div0_fault_pc;
}

/** @brief Implementation of `emu_div0_count_trap()` -- run-end telemetry bump. */
void emu_div0_count_trap(void)
{
  s_div0_traps++;
}

/** @brief Implementation of `emu_div0_disarm()` -- warm reboot un-patches sites. */
void emu_div0_disarm(void)
{
  s_div0_armed = false;
}
