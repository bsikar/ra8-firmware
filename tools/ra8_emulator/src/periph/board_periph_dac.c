/**
 * @file board_periph_dac.c
 * @brief DAC_B (12-bit D/A) peripheral-block model for the board emulator
 *
 * @details
 * Models the two RA8D2 DAC_B instances (ra8_dac_b_regs.h / ra8_dac_b.c):
 * DAC_B0 at 0x40233000 and DAC_B1 at 0x40233100 (stride 0x100). Each instance
 * drives one 12-bit channel through a {DADR, DACR0, DACR1, DACR2} register set.
 *
 * The DAC has no conversion-result readback on silicon -- ``ra8_dac_b_write`` is
 * a single 16-bit store to DADR -- so this block simply accepts every register
 * write and reads each back as written. That read-back is what matters for the
 * driver: ``ra8_dac_b_init_configured`` / ``_set_output_enable`` do a
 * read-modify-write of DACR0 (DACEN / DAOUTDIS), and ``ra8_dac_b_get_status``
 * reads DACR0.DACEN, so the control state must survive. The model additionally
 * tracks each channel's last and peak DADR code (and a write count) purely for
 * the end-of-run report, so a ramp/triangle demo's output sweep is observable
 * headlessly even though there is no hardware read path.
 *
 * Self-registers its descriptor with the board_periph core from a file-scope
 * constructor; the core keeps no central block list -- see board_periph_block.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_periph_block.h"

/** @brief Console-tap sizing for DAC output summaries. */
typedef enum : uint32_t {
  k_dac_console_line_cap = 48U,  /**< Max chars in a "DAC.. code=.." line.    */
  k_dac_console_every    = 256U, /**< Push 1 line per N updates (anti-flood). */
} dac_console_t;

/** @brief DAC_B block geometry (ra8_dac_b_regs.h, FSP R_DAC_B0_Type). */
typedef enum : uint64_t {
  k_dac_base       = 0x40233000UL,  /**< DAC_B0 base.                        */
  k_dac_stride     = 0x100UL,       /**< Bytes between DAC_B0 and DAC_B1.    */
  k_dac_count      = 2UL,           /**< DAC_B0 / DAC_B1.                    */
  k_dac_span       = 0x200UL,       /**< Both instances (2 x 0x100 stride).  */
  k_dac_inst_words = 0x100UL / 4UL, /**< Backing words per instance.         */
  k_dac_off_dadr   = 0x00UL,        /**< DADR 12-bit data (16-bit reg).      */
  k_dac_off_dacr0  = 0x04UL,        /**< DACR0 control (DACEN/DAE/DAOUTDIS). */
  k_dac_off_dacr1  = 0x08UL,        /**< DACR1 control (DPSEL).              */
  k_dac_off_dacr2  = 0x0CUL,        /**< DACR2 control (OFSSEL).             */
} dac_map_t;

/** @brief DAC_B field masks. */
typedef enum : uint32_t {
  k_dac_dadr_mask  = 0x00000FFFUL, /**< 12-bit DADR data field.         */
  k_dac_dacen_mask = 0x00000001UL, /**< DACR0.DACEN channel-enable bit. */
} dac_field_t;

/** @brief One DAC_B instance: register backing store + write observability. */
typedef struct {
  uint32_t reg[k_dac_inst_words]; /**< Word-addressed register backing. */
  uint16_t last;                  /**< Last DADR code written.          */
  uint16_t peak;                  /**< Largest DADR code seen.          */
  uint32_t writes;                /**< DADR write count.                */
} dac_inst_t;

static dac_inst_t s_dac[k_dac_count];

/** @brief Word index into an instance's backing store for an in-instance offset. */
static uint32_t dac_word(uint64_t inst_off)
{
  return (uint32_t)(inst_off >> 2U);
}

/** @brief MMIO read inside the DAC window: read back the latched register. */
static uint64_t dac_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off  = addr - (uint64_t)k_dac_base;
  const uint32_t inst = (uint32_t)(off / (uint64_t)k_dac_stride);
  if (inst >= (uint32_t)k_dac_count) {
    return 0U;
  }
  const uint64_t inst_off = off % (uint64_t)k_dac_stride;
  if (inst_off >= (uint64_t)k_dac_stride) {
    return 0U;
  }
  return s_dac[inst].reg[dac_word(inst_off)];
}

/** @brief MMIO write inside the DAC window: latch the value; track DADR. */
static void dac_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off  = addr - (uint64_t)k_dac_base;
  const uint32_t inst = (uint32_t)(off / (uint64_t)k_dac_stride);
  if (inst >= (uint32_t)k_dac_count) {
    return;
  }
  const uint64_t inst_off = off % (uint64_t)k_dac_stride;
  if (inst_off >= (uint64_t)k_dac_stride) {
    return;
  }
  s_dac[inst].reg[dac_word(inst_off)] = (uint32_t)value;
  if (inst_off == (uint64_t)k_dac_off_dadr) {
    const uint16_t code = (uint16_t)((uint32_t)value & (uint32_t)k_dac_dadr_mask);
    s_dac[inst].last    = code;
    if (code > s_dac[inst].peak) {
      s_dac[inst].peak = code;
    }
    s_dac[inst].writes++;
    /* Console DAC tab: anti-flood -- the first update and then every Nth update
     * (a continuous waveform writes per-sample, so one line per N keeps the ring
     * readable while still showing the channel is active). */
    bool dac_report = (s_dac[inst].writes == 1U);
    if ((s_dac[inst].writes % (uint32_t)k_dac_console_every) == 0U) {
      dac_report = true;
    }
    if (dac_report) {
      char ln[k_dac_console_line_cap];
      (void)snprintf(ln,
                     sizeof(ln),
                     "DAC%u code=%u (n=%u)",
                     (unsigned)inst,
                     (unsigned)code,
                     (unsigned)s_dac[inst].writes);
      board_console_push(k_board_console_ch_dac, ln);
    }
    if (board_periph_trace()) {
      (void)fprintf(stderr, "  [trace] DAC_B%u <- code=%u\n", inst, code);
    }
  }
}

/** @brief Clear both DAC_B instances' registers and observability state. */
static void dac_reset(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dac_count; i++) {
    s_dac[i] = (dac_inst_t){};
  }
}

/** @brief Print one line per DAC_B instance that the firmware drove. */
static void dac_report(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dac_count; i++) {
    if (s_dac[i].writes == 0U) {
      continue;
    }
    (void)fprintf(stderr,
                  "  DAC_B%u        : writes=%u last=%u peak=%u enabled=%s\n",
                  i,
                  s_dac[i].writes,
                  s_dac[i].last,
                  s_dac[i].peak,
                  (s_dac[i].reg[dac_word((uint64_t)k_dac_off_dacr0)] & (uint32_t)k_dac_dacen_mask)
                    ? "yes"
                    : "no");
  }
}

/** @brief This block's descriptor (static lifetime; the core keeps the pointer). */
static const board_periph_block_t k_dac_block = {
  .base   = (uint64_t)k_dac_base,
  .span   = (uint64_t)k_dac_span,
  .order  = (uint32_t)k_block_order_i2c, /* After the timers/UART; no tick. */
  .read   = dac_read,
  .write  = dac_write,
  .tick   = nullptr,
  .reset  = dac_reset,
  .report = dac_report,
  .name   = "DAC_B",
};

/** @brief Self-register the DAC_B block before main runs (decentralized). */
[[gnu::constructor]] static void board_periph_dac_register(void)
{
  board_periph_register_block(&k_dac_block);
}
