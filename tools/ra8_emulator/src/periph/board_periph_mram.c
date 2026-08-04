/**
 * @file board_periph_mram.c
 * @brief Extra-MRAM (data-flash) MACI program/erase model for ra8_emulator.
 *
 * @details
 * Models the RA8D2 MRMS controller (ra8_flash_regs.h, ra8_flash.c) just enough
 * to make the firmware's real extra-MRAM *program* sequence round-trip. The
 * firmware drives extra-MRAM writes through the MACI command sequencer rather
 * than by storing to the region; this model intercepts that sequence:
 *
 *   1. MENTRYR (0x4013E084) = 0xAA80  -> enter program/erase mode; the driver
 *      then spins on MENTRYR bit 0x0080 until set.
 *   2. MSADDR  (0x4013E030) = target  -> latch the extra-MRAM destination.
 *   3. MACI command area (0x40120000): the opener opcode, byte 0x08 (the N
 *      halfword count), eight 16-bit halfwords of data, then byte 0xD0 (the
 *      trailer that starts processing). The opener is 0xE8 (Program command, HUM
 *      Ch 59.7.4.5 p 3591) for the extra-MRAM DATA area; the 0x40 Configuration
 *      Set opener (HUM Ch 59.7.4.8 p 3594, used only for the OFS config area) is
 *      also accepted so an OFS config-set round-trips in the emulator too.
 *   4. MSTATR  (0x4013E080) read       -> driver spins on MRDY (0x8000) and then
 *      checks the error mask (0x00B85020).
 *   5. MENTRYR = 0xAA00                -> leave program mode.
 *
 * On the 0xD0 trailer this model writes the accumulated halfword payload to the
 * latched MSADDR via @c uc_mem_write. The option-setting / OTP window is
 * host-backed by emu_memmap (it is readable on silicon, and the boot-ROM option
 * words live there), so an accepted Program at an in-window MSADDR round-trips
 * on the firmware's read-back. The model is deliberately *faithful* about the
 * command shape: one command carries eight halfwords (16 bytes), so a caller
 * that programs more than 16 bytes per `ra8_flash_extra_mram_write` call issues
 * one command per unit. What it does NOT model is the one-time-programmable
 * semantics -- a real OTP cell cannot be erased and re-programmed, so an emulator pass
 * for an erase/rewrite demo is optimistic pending the #315 bench answer. Before
 * the extra-MRAM opcode fix, this model accepted ONLY the 0x40 opener, which is
 * why ra8_emulator round-tripped the firmware's (wrong) config-set write and masked
 * the on-silicon blank-MRAM ECC fault.
 *
 * @par There is no data-flash at 0x2700_0000 (#170)
 * The premise above -- that @c 0x27000000 is an "extra-MRAM data region" the
 * Program command can target -- is **false on this silicon**, and modelling it
 * as ordinary RAM is what kept @c ra8_io_mram_demo green here while the bench
 * returned @c Error=516. HUM Ch 5 Figure 5.2 p 237 labels the Extra MRAM
 * "(option-setting memory)"; the RA8D2 has no user EEPROM / data-flash array,
 * and @c 0x2700_0000 appears nowhere in the manual. HUM Ch 59.7.4.5 Table 59.15
 * p 3592 enumerates every legal Program target and they all lie in
 * @c 0x02E0_7600..0x02E1_79F0 (FSBL, measurement report, code certificate,
 * general-purpose OTP, PBPS, POFSPS, REVOKE, HUK-zeroize enable, anti-rollback
 * counter). ::maci_commit now refuses anything outside that window and latches
 * the command-locked state, matching a by-hand J-Link reproduction on an
 * EK-RA8D2 (MSTATR = 0x0080C000, MASTAT = 0x18).
 *
 * Two register windows are intercepted (the controller block at 0x4013C000 and
 * the MACI command area at 0x40120000); both share one module-static state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_periph_block.h"

/**
 * @brief MRMS program-mode register sub-window (ra8_flash_regs.h).
 *
 * @details Deliberately narrow: it covers ONLY the extra-MRAM program-mode
 * registers (the 0x2000 page = absolute 0x4013E0xx). The 0x4013C0xx config page
 * (MRCPFB / MRCFREQ / MREFREQ) is left to the sparse model because `ra8_cgc_init`
 * programs the MRAM wait-state latches there during clock setup with a
 * write-and-readback poll whose key-strip semantics this model does not mimic --
 * claiming that page would break every app's boot.
 */
typedef enum : uint64_t {
  k_mram_reg_base    = 0x4013E000UL, /**< MRMS program-mode sub-window base. */
  k_mram_reg_span    = 0x00000100UL, /**< Covers MSADDR / MSTATR / MENTRYR.  */
  k_mram_off_msaddr  = 0x0030UL,     /**< MSADDR  : MACI start address.      */
  k_mram_off_mstatr  = 0x0080UL,     /**< MSTATR  : extra-MRAM status.       */
  k_mram_off_mentryr = 0x0084UL,     /**< MENTRYR : program-mode entry.      */
} mram_reg_map_t;

/** @brief MACI command-issuing area window (ra8_flash_regs.h). */
typedef enum : uint64_t {
  k_maci_base = 0x40120000UL, /**< MACI command-issuing area base.   */
  k_maci_span = 0x00000010UL, /**< One command port (byte+halfword). */
} maci_map_t;

/**
 * @brief Code-MRAM program-control sub-window (R_MRMS 0x3000 page).
 *
 * @details The application-slot program path (ra8_flash.c, MRCPC0/1 gate + direct
 * STR into the mapped MRAM code window + MRCFLR flush) does NOT use the MACI
 * sequencer. It only opens the per-world program gate, stores the bytes -- which
 * Unicorn serves directly, the MRAM region is mapped read/write/exec -- and then
 * spins MRCPS for buffer-ready / commit-done before returning. Model just enough
 * of that window to report a controller that is always idle-and-ready so the
 * DFU code-MRAM program (dfu_selftest_*) completes instead of spinning MRCPS to
 * its timeout. The 0x3800 ECC-config and 0x0000 wait-state pages stay sparse.
 */
typedef enum : uint64_t {
  k_mrpgm_base      = 0x4013F000UL,       /**< 0x4013C000 + 0x3000 code-MRAM P/E page. */
  k_mrpgm_span      = 0x00000100UL,       /**< Covers MRCPC0/1 / MRCPS / MRCFLR.       */
  k_mrpgm_words     = 0x00000100UL / 4UL, /**< Shadow word count.                      */
  k_mrpgm_off_mrcps = 0x0010UL,           /**< MRCPS : Code MRAM Program Status.       */
} mrpgm_map_t;

/** @brief MRCPS ready snapshot: address buffer empty, not busy, not full, no errors. */
typedef enum : uint8_t {
  k_mrcps_ready = 0x20U, /**< ABUFEMP set; PRGBSYC / ABUFFULL / error bits clear. */
} mrpgm_status_t;

/** @brief Register bit values the driver polls / writes. */
typedef enum : uint32_t {
  k_mram_mentryr_pe_bit = 0x0080U,     /**< MENTRYR.MENTRY status bit.      */
  k_mram_mstatr_mrdy    = 0x00008000U, /**< MSTATR.MRDY (command complete). */
  k_mram_mstatr_ilglerr = 0x00004000U, /**< MSTATR.ILGLERR  (illegal).      */
  k_mram_mstatr_ilgcom  = 0x00800000U, /**< MSTATR.ILGCOMERR (illegal cmd). */
} mram_status_t;

/** @brief MASTAT bits latched when the sequencer rejects a command. */
typedef enum : uint32_t {
  k_mram_off_mastat   = 0x0010UL, /**< MASTAT : extra-MRAM access status. */
  k_mram_mastat_mreae = 0x08U,    /**< Extra MRAM access error.           */
  k_mram_mastat_cmdlk = 0x10U,    /**< Command-locked state latched.      */
} mram_mastat_t;

/**
 * @brief Legal MSADDR window for the MACI Program command (HUM Table 59.15).
 *
 * @details
 * HUM Ch 59.7.4.5 p 3592, Table 59.15 "Address used by Program command"
 * enumerates every address the Program command accepts, and they are all
 * inside the Extra MRAM option-setting memory: FSBL setting
 * (@c 0x02E0_7600), start address of measurement report / code certificate,
 * general-purpose OTP, PBPS / PBPS_SEC, POFSPS, REVOKE, Zeroization HUK
 * Enable and the anti-rollback counter setting -- spanning
 * @c 0x02E0_7600 to @c 0x02E1_79F0.
 *
 * There is deliberately NO general-purpose data-flash target. HUM Ch 5
 * Figure 5.2 p 237 labels this region "Extra MRAM (option-setting memory)";
 * the RA8D2 has no user EEPROM / data-flash array, and the address
 * @c 0x2700_0000 that @c ra8_flash_extra_mram_write targets appears nowhere
 * in the manual. A Program aimed outside this window is rejected by the
 * sequencer as an illegal command -- bench-confirmed on an EK-RA8D2, where
 * the by-hand sequence @c MSADDR=0x27000000, 0xE8, 0x08, 8x0xFFFF, 0xD0
 * leaves @c MSTATR = 0x0080C000 (ILGCOMERR | ILGLERR | MRDY) and
 * @c MASTAT = 0x18 (CMDLK | MREAE).
 */
typedef enum : uint32_t {
  k_mram_pgm_lo = 0x02E07600UL, /**< First legal Program MSADDR. */
  k_mram_pgm_hi = 0x02E179F0UL, /**< Last legal Program MSADDR.  */
} mram_pgm_window_t;

/** @brief MACI command opcodes the extra-MRAM program / config-set stream uses. */
typedef enum : uint32_t {
  k_maci_cmd_program    = 0xE8U, /**< Program opener: extra-MRAM data (HUM 59.7.4.5). */
  k_maci_cmd_config_set = 0x40U, /**< Config-Set opener: OFS area (HUM 59.7.4.8).     */
  k_maci_cmd_word_n     = 0x08U, /**< N halfword count written after the opener.      */
  k_maci_cmd_final      = 0xD0U, /**< Trailer byte that starts command processing.    */
} maci_cmd_t;

/** @brief MACI collection state. */
typedef enum : uint8_t {
  k_maci_idle    = 0U, /**< No command in progress.                 */
  k_maci_opener1 = 1U, /**< Saw an opener (0xE8/0x40), expecting N. */
  k_maci_collect = 2U, /**< Opener complete, collecting words.      */
} maci_state_t;

/** @brief Sizing + report-order constants. */
typedef enum : uint32_t {
  k_mram_reg_words   = (uint32_t)(k_mram_reg_span / 4UL), /**< Shadow words.         */
  k_mram_payload_max = 32U,                               /**< Max config-set bytes. */
  k_mram_byte_mask   = 0xFFU,                             /**< One byte.             */
  k_mram_hi_shift    = 8U,                                /**< High-byte shift.      */
  k_mram_block_order = 92U,                               /**< Report order slot.    */
} mram_lit_t;

/** @brief MRAM model state: register shadow + MACI collection + counters. */
typedef struct {
  uint32_t regs[k_mram_reg_words];      /**< 0x4013E000 window shadow.         */
  uint32_t pgm_regs[k_mrpgm_words];     /**< 0x4013F000 code-MRAM P/E shadow.  */
  uint32_t msaddr;                      /**< Latched MACI target address.      */
  uint8_t  payload[k_mram_payload_max]; /**< Collected config-set bytes.       */
  uint32_t payload_len;                 /**< Bytes collected this command.     */
  uint8_t  maci_state;                  /**< ::maci_state_t collection state.  */
  uint8_t  pe_active;                   /**< 1 => program/erase mode entered.  */
  uint32_t programs;                    /**< Config-set program commands.      */
  uint32_t rejected;                    /**< Programs refused: MSADDR illegal. */
} mram_state_t;

static mram_state_t s_mram;

/** @brief MMIO read inside the MRMS controller window. */
static uint64_t mram_reg_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_mram_reg_base;
  if (off == (uint64_t)k_mram_off_mentryr) {
    return (s_mram.pe_active != 0U) ? (uint64_t)k_mram_mentryr_pe_bit : 0U;
  }
  if (off == (uint64_t)k_mram_off_mstatr) {
    return (uint64_t)k_mram_mstatr_mrdy; /* command complete, no error bits */
  }
  if ((off / 4U) >= (uint64_t)k_mram_reg_words) {
    return 0U;
  }
  return s_mram.regs[off / 4U];
}

/** @brief MMIO write inside the MRMS controller window (latch MSADDR / P-E mode). */
static void mram_reg_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_mram_reg_base;
  if (off == (uint64_t)k_mram_off_mentryr) {
    s_mram.pe_active = (((uint32_t)value & (uint32_t)k_mram_mentryr_pe_bit) != 0U) ? 1U : 0U;
  }
  if (off == (uint64_t)k_mram_off_msaddr) {
    s_mram.msaddr = (uint32_t)value;
  }
  if ((off / 4U) < (uint64_t)k_mram_reg_words) {
    s_mram.regs[off / 4U] = (uint32_t)value;
  }
}

/**
 * @brief Latch the command-locked state the sequencer enters on rejection.
 *
 * @details
 * HUM Ch 59.7.4.4 p 3590: once the extra-MRAM sequencer is command-locked,
 * "MACI commands cannot be accepted" until a Status Clear or Forced Stop
 * releases it. Sets the same bits a real rejection leaves behind so firmware
 * reading MSTATR / MASTAT in the emulator sees the bench values.
 */
static void maci_reject(void)
{
  s_mram.regs[(uint32_t)k_mram_off_mstatr / 4U] |=
    (uint32_t)k_mram_mstatr_ilgcom | (uint32_t)k_mram_mstatr_ilglerr;
  s_mram.regs[(uint32_t)k_mram_off_mastat / 4U] |=
    (uint32_t)k_mram_mastat_cmdlk | (uint32_t)k_mram_mastat_mreae;
  s_mram.rejected++;
}

/** @brief Commit the collected command payload to the mapped MRAM region. */
static void maci_commit(uc_engine* uc)
{
  if (s_mram.payload_len == 0U) {
    return;
  }
  /* HUM Ch 59.7.4.5 Table 59.15 p 3592: the Program command only accepts the
   * option-setting / OTP addresses in [k_mram_pgm_lo, k_mram_pgm_hi]. Anything
   * else -- notably the 0x2700_0000 "extra-MRAM data area" that exists in no
   * RA8D2 memory map -- is an illegal command on silicon. This model used to
   * write it straight through to mapped RAM, which let ra8_io_mram_demo round
   * trip in the emulator while the bench returned Error=516 (#170). Reject it
   * here so the emulator reports the bench result. */
  if (s_mram.msaddr < (uint32_t)k_mram_pgm_lo) {
    maci_reject();
    return;
  }
  if (s_mram.msaddr > (uint32_t)k_mram_pgm_hi) {
    maci_reject();
    return;
  }
  (void)uc_mem_write(uc, (uint64_t)s_mram.msaddr, s_mram.payload, (size_t)s_mram.payload_len);
  s_mram.programs++;
}

/** @brief MMIO write inside the MACI command-issuing area (the command stream). */
static void maci_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)addr;
  if (size == 2U) {
    if (s_mram.maci_state == (uint8_t)k_maci_collect) {
      if ((s_mram.payload_len + 2U) <= (uint32_t)k_mram_payload_max) {
        s_mram.payload[s_mram.payload_len] = (uint8_t)(value & (uint64_t)k_mram_byte_mask);
        s_mram.payload[s_mram.payload_len + 1U] =
          (uint8_t)((value >> (uint64_t)k_mram_hi_shift) & (uint64_t)k_mram_byte_mask);
        s_mram.payload_len += 2U;
      }
    }
    return;
  }
  const uint32_t byte = (uint32_t)value & (uint32_t)k_mram_byte_mask;
  /* Either opener (Program 0xE8 or Config-Set 0x40) starts a command; kept as
   * two single-condition checks so the model carries no compound decision. */
  if (byte == (uint32_t)k_maci_cmd_program) {
    s_mram.maci_state = (uint8_t)k_maci_opener1;
    return;
  }
  if (byte == (uint32_t)k_maci_cmd_config_set) {
    s_mram.maci_state = (uint8_t)k_maci_opener1;
    return;
  }
  if ((byte == (uint32_t)k_maci_cmd_word_n) && (s_mram.maci_state == (uint8_t)k_maci_opener1)) {
    s_mram.maci_state  = (uint8_t)k_maci_collect;
    s_mram.payload_len = 0U;
    return;
  }
  if ((byte == (uint32_t)k_maci_cmd_final) && (s_mram.maci_state == (uint8_t)k_maci_collect)) {
    maci_commit(uc);
    s_mram.maci_state = (uint8_t)k_maci_idle;
    return;
  }
  s_mram.maci_state = (uint8_t)k_maci_idle; /* forced-stop / status-clear / unknown */
}

/** @brief MMIO read inside the MACI command area (driver never reads it). */
static uint64_t maci_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)addr;
  (void)size;
  return 0U;
}

/** @brief MMIO read inside the code-MRAM program-control window (MRCPS ready). */
static uint64_t mrpgm_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_mrpgm_base;
  if (off == (uint64_t)k_mrpgm_off_mrcps) {
    return (uint64_t)k_mrcps_ready; /* always idle-and-ready: no MACI sequencing here. */
  }
  if ((off / 4U) >= (uint64_t)k_mrpgm_words) {
    return 0U;
  }
  return s_mram.pgm_regs[off / 4U]; /* MRCPC0/1 / MRCFLR read back their written value. */
}

/** @brief MMIO write inside the code-MRAM program-control window (shadow the gate). */
static void mrpgm_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_mrpgm_base;
  /* MRCPC0/1 (program gate), MRCFLR (flush), MRCPS (W1C error clear): the mapped
   * MRAM window already accepts the direct STR programming, so these only need
   * to shadow so a gate read-back sees the key that was written. */
  if ((off / 4U) < (uint64_t)k_mrpgm_words) {
    s_mram.pgm_regs[off / 4U] = (uint32_t)value;
  }
}

/** @brief Reset the MRAM model: clear the shadow + collection state. */
static void mram_reset(void)
{
  (void)memset(&s_mram, 0, sizeof(s_mram));
}

/** @brief End-of-run MRAM section: MACI program-command count (only if used). */
static void mram_report(void)
{
  if (s_mram.rejected != 0U) {
    /* Loud: a Program aimed outside HUM Table 59.15's option-setting window
     * is an illegal command on silicon (command-locked, MSTATR.ILGCOMERR). */
    (void)fprintf(stderr,
                  "  Extra-MRAM    : %u MACI program commands, %u REJECTED "
                  "(MSADDR outside HUM Table 59.15 option-setting window)\n",
                  s_mram.programs,
                  s_mram.rejected);
    return;
  }
  if (s_mram.programs == 0U) {
    return;
  }
  (void)fprintf(stderr, "  Extra-MRAM    : %u MACI program commands\n", s_mram.programs);
}

/** @brief MRMS controller-register block descriptor (self-registered). */
static const board_periph_block_t k_mram_reg_block = {
  .base   = (uint32_t)k_mram_reg_base,
  .span   = (uint32_t)k_mram_reg_span,
  .order  = (uint32_t)k_mram_block_order,
  .read   = mram_reg_read,
  .write  = mram_reg_write,
  .tick   = nullptr,
  .reset  = mram_reset,
  .report = mram_report,
  .name   = "MRAM",
};

/** @brief MACI command-area block descriptor (self-registered). */
static const board_periph_block_t k_maci_block = {
  .base   = (uint32_t)k_maci_base,
  .span   = (uint32_t)k_maci_span,
  .order  = (uint32_t)k_mram_block_order,
  .read   = maci_read,
  .write  = maci_write,
  .tick   = nullptr,
  .reset  = nullptr,
  .report = nullptr,
  .name   = "MACI",
};

/** @brief Code-MRAM program-control block descriptor (self-registered). */
static const board_periph_block_t k_mrpgm_block = {
  .base   = (uint32_t)k_mrpgm_base,
  .span   = (uint32_t)k_mrpgm_span,
  .order  = (uint32_t)k_mram_block_order,
  .read   = mrpgm_read,
  .write  = mrpgm_write,
  .tick   = nullptr,
  .reset  = nullptr,
  .report = nullptr,
  .name   = "MRAM-pgm",
};

/** @brief Register the MRAM + MACI blocks before main (host constructor). */
[[gnu::constructor]] static void mram_block_register(void)
{
  board_periph_register_block(&k_mram_reg_block);
  board_periph_register_block(&k_maci_block);
  board_periph_register_block(&k_mrpgm_block);
}
