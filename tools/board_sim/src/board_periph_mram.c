/**
 * @file board_periph_mram.c
 * @brief Extra-MRAM (data-flash) MACI program/erase model for board_sim.
 *
 * @details
 * Models the RA8D2 MRMS controller (ra8d2_flash_regs.h, ra_flash.c) just enough
 * to make the firmware's real extra-MRAM *program* sequence round-trip. The
 * extra-MRAM data region at @c 0x27000000 is a normal mapped RAM region in
 * board_sim, so READS of it are served directly by Unicorn; what was missing is
 * the WRITE path, which the firmware drives through the MACI command sequencer
 * rather than by storing to the region:
 *
 *   1. MENTRYR (0x4013E084) = 0xAA80  -> enter program/erase mode; the driver
 *      then spins on MENTRYR bit 0x0080 until set.
 *   2. MSADDR  (0x4013E030) = target  -> latch the extra-MRAM destination.
 *   3. MACI command area (0x40120000): byte 0x40, byte 0x08 (config-set opener),
 *      then eight 16-bit halfwords of data, then byte 0xD0 (trailer).
 *   4. MSTATR  (0x4013E080) read       -> driver spins on MRDY (0x8000) and then
 *      checks the error mask (0x00B85020).
 *   5. MENTRYR = 0xAA00                -> leave program mode.
 *
 * On the 0xD0 trailer this model writes the accumulated halfword payload into the
 * mapped data region at the latched MSADDR via @c uc_mem_write, so the firmware's
 * subsequent direct reads see exactly what each config-set programmed. The model
 * is deliberately *faithful*: a config-set carries eight halfwords (16 bytes), so
 * a caller that programs more than 16 bytes per `ra_flash_extra_mram_write` call
 * will see only the first 16 land -- the model reflects the hardware, it does not
 * paper over it.
 *
 * Two register windows are intercepted (the controller block at 0x4013C000 and
 * the MACI command area at 0x40120000); both share one module-static state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_periph_block.h"

/**
 * @brief MRMS program-mode register sub-window (ra8d2_flash_regs.h).
 *
 * @details Deliberately narrow: it covers ONLY the extra-MRAM program-mode
 * registers (the 0x2000 page = absolute 0x4013E0xx). The 0x4013C0xx config page
 * (MRCPFB / MRCFREQ / MREFREQ) is left to the sparse model because `ra_cgc_init`
 * programs the MRAM wait-state latches there during clock setup with a
 * write-and-readback poll whose key-strip semantics this model does not mimic --
 * claiming that page would break every app's boot.
 */
typedef enum : uint64_t {
  k_mram_reg_base    = 0x4013E000UL, /**< MRMS program-mode sub-window base.  */
  k_mram_reg_span    = 0x00000100UL, /**< Covers MSADDR / MSTATR / MENTRYR.   */
  k_mram_off_msaddr  = 0x0030UL,     /**< MSADDR  : MACI start address.       */
  k_mram_off_mstatr  = 0x0080UL,     /**< MSTATR  : extra-MRAM status.        */
  k_mram_off_mentryr = 0x0084UL,     /**< MENTRYR : program-mode entry.       */
} mram_reg_map_t;

/** @brief MACI command-issuing area window (ra8d2_flash_regs.h). */
typedef enum : uint64_t {
  k_maci_base = 0x40120000UL, /**< MACI command-issuing area base. */
  k_maci_span = 0x00000010UL, /**< One command port (byte+halfword). */
} maci_map_t;

/** @brief Register bit values the driver polls / writes. */
typedef enum : uint32_t {
  k_mram_mentryr_pe_bit = 0x0080U,     /**< MENTRYR.MENTRY status bit.       */
  k_mram_mstatr_mrdy    = 0x00008000U, /**< MSTATR.MRDY (command complete).  */
} mram_status_t;

/** @brief MACI command opcodes the config-set sequence uses. */
typedef enum : uint32_t {
  k_maci_cmd_opener1 = 0x40U, /**< Config-set opener byte 1. */
  k_maci_cmd_opener2 = 0x08U, /**< Config-set opener byte 2. */
  k_maci_cmd_final   = 0xD0U, /**< Config-set trailer.       */
} maci_cmd_t;

/** @brief MACI collection state. */
typedef enum : uint8_t {
  k_maci_idle    = 0U, /**< No command in progress.            */
  k_maci_opener1 = 1U, /**< Saw 0x40, expecting 0x08.          */
  k_maci_collect = 2U, /**< Opener complete, collecting words. */
} maci_state_t;

/** @brief Sizing + report-order constants. */
typedef enum : uint32_t {
  k_mram_reg_words   = (uint32_t)(k_mram_reg_span / 4UL), /**< Shadow words.       */
  k_mram_payload_max = 32U,                               /**< Max config-set bytes.*/
  k_mram_byte_mask   = 0xFFU,                             /**< One byte.           */
  k_mram_hi_shift    = 8U,                                /**< High-byte shift.    */
  k_mram_block_order = 92U,                               /**< Report order slot.  */
} mram_lit_t;

/** @brief MRAM model state: register shadow + MACI collection + counters. */
typedef struct {
  uint32_t regs[k_mram_reg_words];      /**< 0x4013C000 window shadow.        */
  uint32_t msaddr;                      /**< Latched MACI target address.     */
  uint8_t  payload[k_mram_payload_max]; /**< Collected config-set bytes.     */
  uint32_t payload_len;                 /**< Bytes collected this command.    */
  uint8_t  maci_state;                  /**< ::maci_state_t collection state. */
  uint8_t  pe_active;                   /**< 1 => program/erase mode entered. */
  uint32_t programs;                    /**< Config-set program commands.     */
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

/** @brief Commit the collected config-set payload to the mapped MRAM region. */
static void maci_commit(uc_engine* uc)
{
  if (s_mram.payload_len == 0U) {
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
  if (byte == (uint32_t)k_maci_cmd_opener1) {
    s_mram.maci_state = (uint8_t)k_maci_opener1;
    return;
  }
  if ((byte == (uint32_t)k_maci_cmd_opener2) && (s_mram.maci_state == (uint8_t)k_maci_opener1)) {
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

/** @brief Reset the MRAM model: clear the shadow + collection state. */
static void mram_reset(void)
{
  (void)memset(&s_mram, 0, sizeof(s_mram));
}

/** @brief End-of-run MRAM section: config-set program count (only if used). */
static void mram_report(void)
{
  if (s_mram.programs == 0U) {
    return;
  }
  (void)fprintf(stderr, "  Extra-MRAM    : %u config-set programs\n", s_mram.programs);
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

/** @brief Register the MRAM + MACI blocks before main (host constructor). */
__attribute__((constructor)) static void mram_block_register(void)
{
  board_periph_register_block(&k_mram_reg_block);
  board_periph_register_block(&k_maci_block);
}
