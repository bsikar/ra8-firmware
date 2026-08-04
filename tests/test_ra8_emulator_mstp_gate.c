/**
 * @file test_ra8_emulator_mstp_gate.c
 * @brief Unit tests for the ra8_emulator module-stop gate table (#405)
 *
 * @details
 * Compiles the engine-free half of the ra8_emulator module-stop model
 * (tools/ra8_emulator/src/periph/board_periph_mstp_model.c) directly on the host and
 * drives its public seam (board_periph_mstp_internal.h). The ra8_emulator core
 * consults ::board_mstp_addr_stopped before answering an MMIO access to an
 * owning peripheral block; these tests prove the gate table is correct so a
 * peripheral the firmware never ungated is inert in the emulator exactly as it
 * is on silicon:
 *
 *  - At reset every gated instance is module-stopped (reads 0 / writes dropped).
 *  - Clearing an instance's module-stop bit (what ``ra8_mstp_enable`` does)
 *    enables that instance and ONLY that instance -- the read-modify-write is
 *    driven through the same shadow the core reads back.
 *  - A shared bit (GPT4..GPT9 all gated by MSTPE27) enables every instance it
 *    covers at once.
 *  - An address with no module-stop control (GPIO, ICU) or one ra8_emulator does
 *    not gate is never reported stopped, so un-gated blocks answer as before.
 *
 * The (address -> module-stop bit) truth each vector asserts against is the
 * HAL's ``ra8_mstp_regs.h`` enum (whose per-bit HUM citations are canonical),
 * decoded independently of the model, so a wrong bit in the model's table
 * fails a vector rather than agreeing with a matching mistake.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "board_periph_mstp_internal.h"
#include "ra8_mstp_regs.h"
#include "unity_minimal.h"

/** @brief Representative peripheral-instance register addresses under test. */
typedef enum : uint64_t {
  k_t_sci0 = 0x40358000UL, /**< SCI0 window base.       */
  k_t_sci3 = 0x40358300UL, /**< SCI3 window base.       */
  k_t_spi1 = 0x4035C100UL, /**< SPI1 window base.       */
  k_t_gpt0 = 0x40322000UL, /**< GPT0 window base.       */
  k_t_gpt4 = 0x40322400UL, /**< GPT4 window base.       */
  k_t_gpt9 = 0x40322900UL, /**< GPT9 window base.       */
  k_t_cac  = 0x40202400UL, /**< CAC window base.        */
  k_t_drw  = 0x40444000UL, /**< DRW window base.        */
  k_t_adc  = 0x40338000UL, /**< ADC_B window base.      */
  k_t_gpio = 0x40080000UL, /**< PORT0 (no module-stop). */
  k_t_icu  = 0x40006000UL, /**< ICU (no module-stop).   */
} t_addr_t;

/** @brief Packing constants mirroring ra8_mstp_t (reg<<8 | bit). */
typedef enum : uint32_t {
  k_t_id_reg_shift = 8U,    /**< Register index in bits 15:8. */
  k_t_id_byte_mask = 0xFFU, /**< Bit index in bits 7:0.       */
  k_t_reg_bytes    = 4U,    /**< Bytes per MSTPCR register.   */
} t_pack_t;

/**
 * @brief Read-modify-write one module-stop bit through the model's shadow.
 *
 * @details Mirrors ``ra8_mstp_enable`` / ``_disable``: read the owning MSTPCRx
 * word, clear (ungate) or set (re-gate) the bit, write it back.
 *
 * @param[in] id   Packed ra8_mstp_t id (from ra8_mstp_regs.h).
 * @param[in] stop true to set the bit (stop), false to clear it (run).
 */
static void set_module_stop(uint16_t id, bool stop)
{
  const uint32_t reg = ((uint32_t)id >> (uint32_t)k_t_id_reg_shift) & (uint32_t)k_t_id_byte_mask;
  const uint32_t bit = (uint32_t)id & (uint32_t)k_t_id_byte_mask;
  const uint64_t off = (uint64_t)reg * (uint64_t)k_t_reg_bytes;
  const uint32_t cur = board_mstp_read_reg(off, (unsigned)k_t_reg_bytes);
  const uint32_t nxt = stop ? (cur | ((uint32_t)1U << bit)) : (cur & ~((uint32_t)1U << bit));
  board_mstp_apply_write(off, (unsigned)k_t_reg_bytes, nxt);
}

/**
 * @brief Every gated instance is module-stopped straight out of reset.
 *
 * @par MC/DC:
 * No compound decision -- confirms the reset baseline the gate depends on: with
 * no ungate yet, ::board_mstp_addr_stopped is true for a representative address
 * in several families.
 */
static void test_reset_all_stopped(void)
{
  TEST_BEGIN("mstp_gate: reset stops every gated peripheral");
  board_mstp_reset();
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_sci0));
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_sci3));
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_spi1));
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_gpt0));
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_cac));
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_drw));
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_adc));
  TEST_END("mstp_gate: reset stops every gated peripheral");
}

/**
 * @brief Clearing a module-stop bit enables that instance and only that one.
 *
 * @par MC/DC:
 * No compound decision -- exercises the enable/disable contract the gate
 * enforces: ungating SCI3 releases 0x40358300 while SCI0 stays stopped, and
 * re-gating SCI3 stops it again.
 */
static void test_ungate_regate_one(void)
{
  TEST_BEGIN("mstp_gate: ungate/re-gate a single instance");
  board_mstp_reset();

  set_module_stop((uint16_t)k_ra8_mstp_sci3, false); /* ra8_mstp_enable(SCI3) */
  TEST_ASSERT(!board_mstp_addr_stopped((uint64_t)k_t_sci3));
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_sci0)); /* neighbour untouched */

  set_module_stop((uint16_t)k_ra8_mstp_sci3, true); /* ra8_mstp_disable(SCI3) */
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_sci3));
  TEST_END("mstp_gate: ungate/re-gate a single instance");
}

/**
 * @brief A shared module-stop bit releases every instance it covers at once.
 *
 * @par MC/DC:
 * No compound decision -- GPT4..GPT9 all map to MSTPE27, so ungating that one
 * bit must release both GPT4 and GPT9 while GPT0 (MSTPE31) stays stopped.
 */
static void test_shared_bit_gpt(void)
{
  TEST_BEGIN("mstp_gate: shared GPT4..9 bit releases both");
  board_mstp_reset();

  set_module_stop((uint16_t)k_ra8_mstp_gpt4_9, false);
  TEST_ASSERT(!board_mstp_addr_stopped((uint64_t)k_t_gpt4));
  TEST_ASSERT(!board_mstp_addr_stopped((uint64_t)k_t_gpt9));
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_gpt0)); /* distinct bit */

  set_module_stop((uint16_t)k_ra8_mstp_gpt4_9, true);
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_gpt4));
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_gpt9));
  TEST_END("mstp_gate: shared GPT4..9 bit releases both");
}

/**
 * @brief An address with no module-stop control is never reported stopped.
 *
 * @par MC/DC:
 * No compound decision -- GPIO and ICU own no gate entry, so
 * ::board_mstp_addr_stopped is false for them even at reset (when every gated
 * block IS stopped), proving un-gated blocks answer unchanged.
 */
static void test_ungated_addr_never_stopped(void)
{
  TEST_BEGIN("mstp_gate: ungated peripherals never gated");
  board_mstp_reset();
  TEST_ASSERT(!board_mstp_addr_stopped((uint64_t)k_t_gpio));
  TEST_ASSERT(!board_mstp_addr_stopped((uint64_t)k_t_icu));
  TEST_END("mstp_gate: ungated peripherals never gated");
}

/**
 * @brief MC/DC over the in-range decision inside the family lookup.
 *
 * @par MC/DC:
 * Decision: an address hits a family iff ``(addr >= base) && (addr < base+span)``
 * (2 conditions), evaluated against the CAC single-instance window
 * [0x40202400, 0x40202410).
 * - Vector 1: addr = base            -> true  && true  -> gated (both true)
 * - Vector 2: addr = base - 4        -> false && (--)  -> not gated (C1 varies)
 * - Vector 3: addr = base + span     -> true  && false -> not gated (C2 varies)
 * Vectors 1+2 prove the lower bound independently affects the outcome; 1+3 prove
 * the same for the upper bound. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_range_decision_mcdc(void)
{
  TEST_BEGIN("mstp_gate: in-range decision MC/DC");
  board_mstp_reset(); /* CAC starts stopped, so an in-range hit reports true. */
  /* base */
  TEST_ASSERT(board_mstp_addr_stopped((uint64_t)k_t_cac));
  /* below base */
  TEST_ASSERT(!board_mstp_addr_stopped((uint64_t)k_t_cac - 4U));
  /* base+span */
  TEST_ASSERT(!board_mstp_addr_stopped((uint64_t)k_t_cac + 0x10U));
  TEST_END("mstp_gate: in-range decision MC/DC");
}

/** @brief Fixed operands for the read-back / sub-word merge test. */
typedef enum : uint32_t {
  k_t_mstpcrb_off = 4U,          /**< MSTPCRB byte offset within the window. */
  k_t_word_bytes  = 4U,          /**< Full 32-bit access width.              */
  k_t_byte_bytes  = 1U,          /**< Single-byte access width.              */
  k_t_pat_word    = 0x12345678U, /**< Arbitrary full-word test pattern.      */
  k_t_pat_byte    = 0xABU,       /**< Byte written to the low lane.          */
  k_t_pat_merged  = 0x123456ABU, /**< pat_word with its low byte replaced.   */
} t_readback_t;

/**
 * @brief The shadow read-back returns what was written, at any access width.
 *
 * @par MC/DC:
 * No compound decision -- confirms the deterministic read-back path: a full
 * 32-bit write reads back intact, and a single-byte write merges into just its
 * byte lane without disturbing the rest of the register.
 */
static void test_readback_and_subword(void)
{
  TEST_BEGIN("mstp_gate: shadow read-back + sub-word merge");
  board_mstp_reset();

  /* MSTPCRB: full-word write reads back intact. */
  board_mstp_apply_write((uint64_t)k_t_mstpcrb_off,
                         (unsigned)k_t_word_bytes,
                         (uint32_t)k_t_pat_word);
  TEST_ASSERT_EQ(k_t_pat_word,
                 board_mstp_read_reg((uint64_t)k_t_mstpcrb_off, (unsigned)k_t_word_bytes));

  /* Byte write to the low lane of MSTPCRB merges without touching the rest. */
  board_mstp_apply_write((uint64_t)k_t_mstpcrb_off,
                         (unsigned)k_t_byte_bytes,
                         (uint32_t)k_t_pat_byte);
  TEST_ASSERT_EQ(k_t_pat_merged,
                 board_mstp_read_reg((uint64_t)k_t_mstpcrb_off, (unsigned)k_t_word_bytes));
  TEST_END("mstp_gate: shadow read-back + sub-word merge");
}

/**
 * @brief The dropped-access counters make a masked bug observable.
 *
 * @par MC/DC:
 * No compound decision -- reset zeroes the counters, a noted read/write bumps
 * the matching counter and records the offending peripheral's label.
 */
static void test_gated_access_counters(void)
{
  TEST_BEGIN("mstp_gate: dropped-access observability");
  board_mstp_reset();
  TEST_ASSERT_EQ(0U, board_mstp_gated_read_count());
  TEST_ASSERT_EQ(0U, board_mstp_gated_write_count());

  board_mstp_note_gated_access((uint64_t)k_t_sci3, false);
  board_mstp_note_gated_access((uint64_t)k_t_sci3, true);
  TEST_ASSERT_EQ(1U, board_mstp_gated_read_count());
  TEST_ASSERT_EQ(1U, board_mstp_gated_write_count());
  TEST_ASSERT(board_mstp_last_gated_name()[0] != '\0');
  TEST_END("mstp_gate: dropped-access observability");
}

/**
 * @brief Run every ra8_emulator MSTP-gate test.
 *
 * @return 0 on success; a failing assertion exits non-zero from the macro.
 */
int main(void)
{
  test_reset_all_stopped();
  test_ungate_regate_one();
  test_shared_bit_gpt();
  test_ungated_addr_never_stopped();
  test_range_decision_mcdc();
  test_readback_and_subword();
  test_gated_access_counters();
  return 0;
}
