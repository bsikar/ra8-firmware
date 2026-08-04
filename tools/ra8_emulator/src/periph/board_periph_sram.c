/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_periph_sram.c
 * @brief SRAM controller ECC self-test / error-status model (SRAMCRn / SRAMESR)
 *
 * @details
 * Models the RA8D2 SRAM controller window (ra8_sram_regs.h, ra8_sram.c) at
 * @c 0x40002000 so @c mem_ecc_fault_demo (issue #130) can prove the ECC
 * error-detection path headlessly. The sparse MMIO fallback shadows these
 * registers as plain read-back memory, which is enough for @c ecc_monitor_demo
 * (it only reads a clean @c SRAMESR) but NOT for a fault-injection demo: a real
 * decoder self-test latches @c SRAMESR when the corrupted syndrome is read
 * back, and nothing in the fallback does that.
 *
 * This block claims the control window and models the HUM Ch 58.3.4 ECC
 * decoder self-test (``ra8_sram_self_test``). That routine drives each bank's
 * @c SRAMCRn through write (@c 0x08) -> bypass (@c 0x80) -> verify (@c 0x1C);
 * the bank's data line is corrupted while in bypass mode and the verify read
 * latches the error. ra8_emulator cannot observe the syndrome data write (on-chip
 * SRAM is host-backed RAM, not an MMIO hook), so it cannot tell a 1-bit fault
 * from a 2-bit one -- it latches BOTH @c SRAMESR slots for the bank on the
 * bypass->verify transition. That is enough for ``ra8_sram_self_test`` to report
 * ``out_caught`` for either injection (it checks its own slot), proving the
 * detection + reporting + clear plumbing. The precise per-slot fidelity (and
 * 1-bit correction vs the 2-bit NMI) is silicon-only; the demo stays in
 * @c hw_pending. All other registers (SRAMPRCR / SRAMWTSC / SRAMCRn /
 * SRAMECCRGNn / SRAMEARnm) read back exactly as written, and @c SRAMESCLR
 * writes clear the matching @c SRAMESR / @c SRAMEAR slots.
 *
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"

/**
 * @enum sram_geom_t
 * @brief SRAM-controller window geometry + register offsets (ra8_sram_regs.h).
 */
typedef enum : uint64_t {
  k_sram_base      = 0x40002000ULL, /**< Secure SRAM-control window base.        */
  k_sram_span      = 0x100ULL,      /**< Covers SRAMPRCR..SRAMEARnm.             */
  k_sram_off_cr0   = 0x10ULL,       /**< SRAMCR0 (CRn at 0x10 + 4*bank), 8-bit.  */
  k_sram_off_cr3   = 0x1CULL,       /**< SRAMCR3 -- last per-bank control byte.  */
  k_sram_off_esr   = 0x40ULL,       /**< SRAMESR error status, 16-bit.           */
  k_sram_off_esclr = 0x48ULL,       /**< SRAMESCLR error-status clear, 16-bit.   */
  k_sram_off_ear0  = 0x50ULL,       /**< SRAMEAR[0][0]; [bank][slot] at +8b/+4s. */
} sram_geom_t;

/**
 * @enum sram_model_t
 * @brief Self-test phase CR values + ESR/bank layout (no magic numbers).
 */
typedef enum : uint32_t {
  k_sram_cr_bypass    = 0x80U, /**< Self-test bypass phase (TSTBYP=1).       */
  k_sram_cr_verify    = 0x1CU, /**< Self-test verify phase (ECC+check).      */
  k_sram_cr_mask      = 0xFFU, /**< SRAMCRn is one byte wide.                */
  k_sram_bank_count   = 4U,    /**< SRAM0..SRAM3.                            */
  k_sram_slots_bank   = 2U,    /**< ESR slots per bank: 0=1-bit, 1=2-bit.    */
  k_sram_ear_bank_str = 8U,    /**< EAR stride per bank (2 slots * 4 bytes). */
  k_sram_ear_slot_str = 4U,    /**< EAR stride per slot (32-bit register).   */
  k_sram_byte_bits    = 8U,    /**< Bits per byte for width-aware assembly.  */
} sram_model_t;

/** @brief Per-bank data-window offsets, for a plausible EAR fault address. */
static const uint32_t k_sram_bank_data_off[k_sram_bank_count] = {
  0x00000000U, /* SRAM0 @ 0x22000000 */
  0x00080000U, /* SRAM1 @ 0x22080000 */
  0x00100000U, /* SRAM2 @ 0x22100000 */
  0x00180000U, /* SRAM3 @ 0x22180000 */
};

/** @brief SRAM-controller model state (cleared each run / reboot). */
typedef struct {
  uint8_t  shadow[k_sram_span];        /**< Read-back store for all plain registers. */
  uint16_t esr;                        /**< Modeled SRAMESR (latch + clear).         */
  uint8_t  last_cr[k_sram_bank_count]; /**< Last SRAMCRn write per bank.             */
  uint32_t latch_count;                /**< Self-test latches this run (report).     */
} sram_state_t;

static sram_state_t s_sram;

/** @brief ESR bit for (bank, slot): slot 0 = 1-bit error, slot 1 = 2-bit. */
static uint16_t sram_esr_bit(uint32_t bank, uint32_t slot)
{
  const uint32_t pos = (k_sram_slots_bank * bank) + slot;
  return (uint16_t)((uint16_t)1U << (uint16_t)pos);
}

/** @brief Write @p value (size bytes) into the read-back shadow at @p off. */
static void sram_shadow_store(uint64_t off, unsigned size, uint64_t value)
{
  for (unsigned i = 0U; i < size; ++i) {
    const uint64_t at = off + (uint64_t)i;
    if (at < (uint64_t)k_sram_span) {
      s_sram.shadow[at] = (uint8_t)(value >> (k_sram_byte_bits * i));
    }
  }
}

/** @brief Read @p size bytes from the read-back shadow at @p off. */
static uint64_t sram_shadow_load(uint64_t off, unsigned size)
{
  uint64_t v = 0U;
  for (unsigned i = 0U; i < size; ++i) {
    const uint64_t at = off + (uint64_t)i;
    if (at < (uint64_t)k_sram_span) {
      v |= (uint64_t)s_sram.shadow[at] << (k_sram_byte_bits * i);
    }
  }
  return v;
}

/** @brief Latch both ESR slots + EAR for @p bank (self-test verify reached). */
static void sram_latch_bank(uint32_t bank)
{
  s_sram.esr          = (uint16_t)(s_sram.esr | sram_esr_bit(bank, 0U) | sram_esr_bit(bank, 1U));
  const uint64_t ear0 = k_sram_off_ear0 + ((uint64_t)bank * (uint64_t)k_sram_ear_bank_str);
  sram_shadow_store(ear0, sizeof(uint32_t), (uint64_t)k_sram_bank_data_off[bank]);
  sram_shadow_store(ear0 + (uint64_t)k_sram_ear_slot_str,
                    sizeof(uint32_t),
                    (uint64_t)k_sram_bank_data_off[bank]);
  s_sram.latch_count++;
}

/** @brief Detect the self-test bypass->verify CR transition for @p bank. */
static void sram_cr_write(uint32_t bank, uint8_t cr)
{
  bool was_bypass = (s_sram.last_cr[bank] == (uint8_t)k_sram_cr_bypass);
  bool is_verify  = (cr == (uint8_t)k_sram_cr_verify);
  if (was_bypass) {
    if (is_verify) {
      sram_latch_bank(bank);
    }
  }
  s_sram.last_cr[bank] = cr;
}

/** @brief Clear the ESR / EAR slots named by an SRAMESCLR write mask. */
static void sram_clear(uint16_t mask)
{
  s_sram.esr = (uint16_t)(s_sram.esr & (uint16_t)~mask);
  for (uint32_t bank = 0U; bank < k_sram_bank_count; ++bank) {
    for (uint32_t slot = 0U; slot < k_sram_slots_bank; ++slot) {
      if ((mask & sram_esr_bit(bank, slot)) != 0U) {
        const uint64_t ear = k_sram_off_ear0 + ((uint64_t)bank * (uint64_t)k_sram_ear_bank_str) +
                             ((uint64_t)slot * (uint64_t)k_sram_ear_slot_str);
        sram_shadow_store(ear, sizeof(uint32_t), 0U);
      }
    }
  }
}

/** @brief MMIO read inside the SRAM-control window (width-aware). */
static uint64_t sram_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  const uint64_t off = addr - (uint64_t)k_sram_base;
  if (off == (uint64_t)k_sram_off_esr) {
    return (uint64_t)s_sram.esr;
  }
  return sram_shadow_load(off, size);
}

/** @brief MMIO write inside the SRAM-control window (width-aware). */
static void sram_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  const uint64_t off = addr - (uint64_t)k_sram_base;
  if (off == (uint64_t)k_sram_off_esclr) {
    sram_clear((uint16_t)value);
    return;
  }
  if (off == (uint64_t)k_sram_off_esr) {
    s_sram.esr = (uint16_t)value;
    return;
  }
  if ((off >= (uint64_t)k_sram_off_cr0) && (off <= (uint64_t)k_sram_off_cr3)) {
    const uint64_t rel = off - (uint64_t)k_sram_off_cr0;
    if ((rel % sizeof(uint32_t)) == 0U) {
      sram_cr_write((uint32_t)(rel / sizeof(uint32_t)),
                    (uint8_t)(value & (uint64_t)k_sram_cr_mask));
    }
  }
  sram_shadow_store(off, size, value);
}

/** @brief Clear all SRAM-controller model state on reset / process start. */
static void sram_reset(void)
{
  s_sram = (sram_state_t){};
}

/** @brief End-of-run SRAM-ECC section: self-test latches observed this run. */
static void sram_report(void)
{
  if (s_sram.latch_count == 0U) {
    return; /* No fault-injection ran: stay quiet. */
  }
  (void)fprintf(stderr,
                "  SRAM-ECC      : decoder self-test latches=%u (SRAMESR modeled)\n",
                s_sram.latch_count);
}

/** @brief Per-tick order slot for the SRAM-ECC block (relative order). */
typedef enum : uint32_t {
  k_sram_block_order = 176U, /**< After the VBATT-backup block; report order. */
} sram_order_t;

/** @brief SRAM-controller block descriptor (self-registered with the core). */
static const board_periph_block_t k_sram_block = {
  .base   = (uint64_t)k_sram_base,
  .span   = (uint64_t)k_sram_span,
  .order  = (uint32_t)k_sram_block_order,
  .read   = sram_read,
  .write  = sram_write,
  .tick   = nullptr,
  .reset  = sram_reset,
  .report = sram_report,
  .name   = "SRAM-ECC",
};

/** @brief Register the SRAM-controller block before main (host constructor). */
[[gnu::constructor]] static void sram_block_register(void)
{
  board_periph_register_block(&k_sram_block);
}
