/**
 * @file board_periph_mstp.c
 * @brief Module Stop Control (MSTPCRA..E) register block for ra8_emulator
 *
 * @details
 * The board_periph block that owns the R_MSTP window (@c 0x4020_3000, HUM
 * Ch 11.2.6..11.2.10 p 443-450): it answers firmware reads/writes of
 * @c MSTPCRA..MSTPCRE from the tracked shadow in @c board_periph_mstp_model.c,
 * so the mandated read-back after an ungate settles deterministically and the
 * shadow the address->bit gate table consults stays current.
 *
 * The gate ENFORCEMENT lives in the board_periph core: before dispatching an
 * MMIO access to any owning block, @c board_periph.c calls
 * ::board_mstp_addr_stopped and -- when the peripheral is module-stopped --
 * reads 0 / drops the write, matching the silicon (a stopped module is
 * unclocked and does not respond). This file is only the register window plus
 * the reset hook and the end-of-run "you touched an unclocked peripheral"
 * report; the model half is engine-free so it can be unit-tested on the host.
 *
 * Splitting the model out of this file is what lets the gate table be tested
 * without Unicorn (tests/test_ra8_emulator_mstp_gate.c); this glue is the thin
 * part that the core registry needs and that pulls in board_periph_block.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"
#include "board_periph_mstp_internal.h"
#include "ra8_attributes.h"

/** @brief Per-tick order slot for the MSTP block (just before SYSC-PRCR). */
typedef enum : uint32_t {
  k_mstp_block_order = 168U, /**< Groups with the PRCR / power-domain blocks. */
} mstp_order_t;

/** @brief MMIO read of MSTPCRA..E: answer from the tracked shadow. */
static uint64_t mstp_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  return (uint64_t)board_mstp_read_reg(addr - (uint64_t)k_board_mstp_win_base, size);
}

/** @brief MMIO write of MSTPCRA..E: fold into the shadow (ungate/re-gate). */
static void mstp_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  board_mstp_apply_write(addr - (uint64_t)k_board_mstp_win_base, size, (uint32_t)value);
}

/** @brief End-of-run section: make any access to an unclocked block LOUD. */
static void mstp_report(void)
{
  const uint32_t r = board_mstp_gated_read_count();
  const uint32_t w = board_mstp_gated_write_count();
  if ((r == 0U) && (w == 0U)) {
    return; /* Clean: every peripheral the firmware touched was clocked. */
  }
  /* Loud: firmware read/wrote a module-stopped peripheral. On silicon those
   * reads return 0 and writes vanish -- the exact masked-pass this block ends. */
  (void)fprintf(stderr,
                "  MSTP-GATING   : DROPPED %u read(s)+%u write(s) to module-stopped "
                "peripheral(s) (last: %s) -- firmware forgot to cancel module-stop\n",
                r,
                w,
                board_mstp_last_gated_name());
}

/** @brief MSTP block descriptor (owns MSTPCRA..E; self-registered). */
static const board_periph_block_t k_mstp_block = {
  .base   = (uint64_t)k_board_mstp_win_base,
  .span   = (uint64_t)k_board_mstp_win_span,
  .order  = (uint32_t)k_mstp_block_order,
  .read   = mstp_read,
  .write  = mstp_write,
  .tick   = nullptr,
  .reset  = board_mstp_reset,
  .report = mstp_report,
  .name   = "MSTP",
};

/** @brief Register the MSTP block before main (host constructor). */
[[gnu::constructor]] static void mstp_block_register(void)
{
  board_periph_register_block(&k_mstp_block);
  board_mstp_reset();
}
