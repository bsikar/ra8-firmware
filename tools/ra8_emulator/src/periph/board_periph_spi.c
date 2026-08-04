/**
 * @file board_periph_spi.c
 * @brief SPI_B (Type-B SPI) controller peripheral-block model for ra8_emulator
 *
 * @details
 * Models the RA8D2 SPI_B controller the ra8_spi_b.c polling driver drives, so
 * the spi_loopback example exercises the genuine ra8_spi_init -> ra8_spi_xfer8
 * code path instead of the sparse ready-bit fallback. Two channels (SPI0 /
 * SPI1) share one window at 0x4035C000 with a 0x100 stride; the register
 * offsets match the @c r_spi_regs_t struct in ra8_spi_regs.h.
 *
 * Loopback semantics: the example calls ra8_spi_init with @c cfg.loopback=true,
 * which sets SPCR2.SPLP2 (non-inverting internal tie, rx = tx) while SPE is
 * still 0; the silicon then feeds the controller's outgoing line straight back
 * into its receive shifter with no external wiring. The model reproduces that
 * by echoing each word written to SPDR back into the receive holding register
 * and driving the SPSR flags the driver polls:
 *
 *  - SPSR.SPTEF (transmit-buffer empty) stays asserted once SPE is set, so the
 *    driver's "wait for TX empty" poll falls through (the model drains TX
 *    immediately).
 *  - A write to SPDR latches the word, echoes it into the receive holding
 *    register when loopback is engaged (else an idle 0), and asserts
 *    SPSR.SPRF (receive-buffer full) so the driver's "wait for RX full" poll
 *    completes and the subsequent SPDR read returns the echoed word.
 *  - SPSRC (write-1-to-clear) clears the matching SPSR flags, mirroring the
 *    driver's SPTEFC / SPRFC clears between frames.
 *
 * Self-registers its descriptor (address range + read / write / reset /
 * report) with the board_periph core from a file-scope constructor.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_periph_block.h"
#include "board_periph_eink.h"
#include "board_periph_sd.h"

/**
 * @brief SPI_B block geometry (ra8_spi_regs.h, 32-bit register file).
 *
 * @details
 * The RA8D2 has two SPI_B channels at 0x4035C000 (SPI0) and 0x4035C100
 * (SPI1), each a 0x70-byte register block inside a 0x100 stride. The polling
 * driver (libs/ra8_hal/src/ra8_spi_b.c) only touches the registers named here;
 * everything else in the window reflects writes through the shadow. Offsets
 * match the @c r_spi_regs_t struct in ra8_spi_regs.h.
 */
typedef enum : uint64_t {
  k_spi_base      = 0x4035C000UL,  /**< SPI0 base.                         */
  k_spi_stride    = 0x100UL,       /**< Bytes per SPI channel.             */
  k_spi_count     = 2UL,           /**< SPI0 + SPI1.                       */
  k_spi_span      = 0x100UL * 2UL, /**< Both channel windows.              */
  k_spi_off_spdr  = 0x00UL,        /**< SPDR data register (FIFO front).   */
  k_spi_off_spcr  = 0x08UL,        /**< SPCR control 1 (SPE/MSTR).         */
  k_spi_off_spcr2 = 0x0CUL,        /**< SPCR2 control 2 (loopback bits).   */
  k_spi_off_spsr  = 0x50UL,        /**< SPSR status (SPTEF/SPRF/...).      */
  k_spi_off_spsrc = 0x68UL,        /**< SPSRC status clear (write-1).      */
  k_spi_reg_words = 0x70UL / 4UL,  /**< Shadow word count for one channel. */
} spi_map_t;

/** @brief SPCR (control 1) bits the model observes (ra8_spi_regs.h). */
typedef enum : uint32_t {
  k_spi_spcr_spe = 0x00000001U, /**< SPE: SPI function enable (bit 0). */
} spi_spcr_bit_t;

/** @brief SPCR2 (control 2) loopback bits (ra8_spi_regs.h). */
typedef enum : uint32_t {
  k_spi_spcr2_splp  = 0x00010000U, /**< SPLP: inverting loopback (rx=~tx). */
  k_spi_spcr2_splp2 = 0x00020000U, /**< SPLP2: non-inverting loopback.     */
} spi_spcr2_bit_t;

/** @brief SPSR (status) flags the driver polls (ra8_spi_regs.h). */
typedef enum : uint32_t {
  k_spi_spsr_spdrf = 0x00800000U, /**< SPDRF: receive data ready (bit 23). */
  k_spi_spsr_sptef = 0x20000000U, /**< SPTEF: transmit empty (bit 29).     */
  k_spi_spsr_cendf = 0x40000000U, /**< CENDF: communication end (bit 30).  */
  k_spi_spsr_sprf  = 0x80000000U, /**< SPRF: receive full (bit 31).        */
} spi_spsr_bit_t;

/** @brief One-byte / one-word masks used by the echo path. */
typedef enum : uint32_t {
  k_spi_byte_mask = 0xFFU, /**< Low data byte of an 8-bit frame. */
} spi_data_t;

/** @brief Sizing for the board-console SPI transfer-summary line. */
typedef enum : uint32_t {
  k_spi_console_line_cap = 64U, /**< Max chars in one SPI console summary line. */
} spi_console_dim_t;

/**
 * @brief One SPI_B channel: register shadow + a single-frame echo engine.
 *
 * @details
 * The model owns the registers the polling driver actually touches; the rest
 * of the window reflects writes through @c reg. @c enabled tracks SPCR.SPE so
 * the status flags only report ready once the channel is running, @c loopback
 * latches SPCR2.SPLP/SPLP2 (the internal tie the example arms), @c rx holds the
 * most recently echoed word an SPDR read returns, and @c spsr carries the
 * SPTEF / SPRF flags the driver waits on (cleared through SPSRC).
 */
typedef struct {
  uint32_t reg[k_spi_reg_words]; /**< Reflect-on-read register shadow.        */
  uint32_t spsr;                 /**< SPSR flags (SPTEF / SPRF / ...).        */
  uint32_t rx;                   /**< Receive holding register (echoed word). */
  bool     enabled;              /**< SPCR.SPE: channel is running.           */
  bool     loopback;             /**< SPCR2.SPLP/SPLP2: internal rx = tx tie. */
  uint32_t frames;               /**< Frames echoed through this channel.     */
} spi_state_t;

/** @brief The modelled SPI_B channels (SPI0 + SPI1). */
static spi_state_t s_spi[k_spi_count];

/* =============================================================================
 * SPI_B controller model -- the single-frame echo the ra8_spi_b.c polling
 * driver clocks through SPDR / SPSR / SPSRC under internal loopback.
 * =============================================================================
 */

/** @brief Index of the shadow word for @p off within a channel (range-checked). */
static uint32_t spi_word(uint64_t off)
{
  return (uint32_t)(off / 4U);
}

/** @brief Apply an SPCR write: SPE gates the channel and arms TX-empty. */
static void spi_spcr_write(spi_state_t* s, uint32_t value)
{
  s->enabled = ((value & (uint32_t)k_spi_spcr_spe) != 0U);
  if (s->enabled) {
    /* Running: the model drains TX instantly, so SPTEF is always ready for
     * the first frame; SPRF stays clear until a word is written. */
    s->spsr |= (uint32_t)k_spi_spsr_sptef;
    s->spsr &= ~(uint32_t)k_spi_spsr_sprf;
  } else {
    s->spsr = 0U;
  }
}

/** @brief Apply an SPCR2 write: latch whether internal loopback is engaged. */
static void spi_spcr2_write(spi_state_t* s, uint32_t value)
{
  s->loopback = ((value & ((uint32_t)k_spi_spcr2_splp | (uint32_t)k_spi_spcr2_splp2)) != 0U);
}

/** @brief Handle a write to SPDR: echo the frame and assert receive-full. */
static void spi_spdr_write(spi_state_t* s, uint32_t value)
{
  /* SPLP2 ties the outgoing line back to the receive shifter (rx = tx);
   * SPLP inverts it (rx = ~tx). Without loopback, if a `--sd` card is
   * attached it answers the exchange (the genuine ra8_sdmmc_spi path); an
   * `--eink` IT8951 controller answers the ra8_epaper path; else nothing
   * drives the line and the receive shifter clocks in an idle 0. */
  if ((s->reg[spi_word((uint64_t)k_spi_off_spcr2)] & (uint32_t)k_spi_spcr2_splp) != 0U) {
    s->rx = (~value) & (uint32_t)k_spi_byte_mask;
  } else if (s->loopback) {
    s->rx = value;
  } else if (board_sd_attached()) {
    s->rx = (uint32_t)board_sd_exchange((uint8_t)(value & (uint32_t)k_spi_byte_mask));
  } else if (board_eink_attached()) {
    s->rx = (uint32_t)board_eink_exchange((uint8_t)(value & (uint32_t)k_spi_byte_mask));
  } else {
    s->rx = 0U;
  }
  /* TX drains immediately (SPTEF stays ready) and the echoed word lands in
   * the receive holding register, so SPRF asserts for the driver's RX poll. */
  s->spsr |= ((uint32_t)k_spi_spsr_sptef | (uint32_t)k_spi_spsr_sprf | (uint32_t)k_spi_spsr_spdrf);
  s->frames++;
}

/** @brief Handle an SPSRC write (write-1-to-clear the matching SPSR flags). */
static void spi_spsrc_write(spi_state_t* s, uint32_t value)
{
  /* Every SPSRC clear bit sits at the same position as its SPSR flag, so a
   * written 1 clears that flag; SPTEF is re-asserted because the model's TX
   * buffer is always empty once the channel runs. */
  s->spsr &= ~value;
  if (s->enabled) {
    s->spsr |= (uint32_t)k_spi_spsr_sptef;
  }
}

/** @brief Read a register from one modelled SPI_B channel. */
static uint64_t spi_reg_read(spi_state_t* s, uint64_t off)
{
  if (off == (uint64_t)k_spi_off_spsr) {
    return s->spsr;
  }
  if (off == (uint64_t)k_spi_off_spdr) {
    return s->rx; /* receive holding register: the echoed frame */
  }
  return s->reg[spi_word(off)]; /* reflect every other register */
}

/** @brief Write a register on one modelled SPI_B channel. */
static void spi_reg_write(spi_state_t* s, uint64_t off, uint32_t value)
{
  s->reg[spi_word(off)] = value; /* shadow keeps "configure then verify" working */
  if (off == (uint64_t)k_spi_off_spcr) {
    spi_spcr_write(s, value);
  } else if (off == (uint64_t)k_spi_off_spcr2) {
    spi_spcr2_write(s, value);
  } else if (off == (uint64_t)k_spi_off_spdr) {
    spi_spdr_write(s, value);
  } else if (off == (uint64_t)k_spi_off_spsrc) {
    spi_spsrc_write(s, value);
  }
}

/** @brief MMIO read inside the SPI_B window: route to the addressed channel. */
static uint64_t spi_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint32_t ch  = (uint32_t)((addr - (uint64_t)k_spi_base) / (uint64_t)k_spi_stride);
  const uint64_t off = (addr - (uint64_t)k_spi_base) % (uint64_t)k_spi_stride;
  return spi_reg_read(&s_spi[ch], off);
}

/** @brief MMIO write inside the SPI_B window: route to the addressed channel. */
static void spi_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint32_t ch  = (uint32_t)((addr - (uint64_t)k_spi_base) / (uint64_t)k_spi_stride);
  const uint64_t off = (addr - (uint64_t)k_spi_base) % (uint64_t)k_spi_stride;
  spi_reg_write(&s_spi[ch], off, (uint32_t)value);
}

/** @brief Clear all SPI_B channel state to power-on. */
static void spi_reset(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_spi_count; i++) {
    s_spi[i] = (spi_state_t){};
  }
  board_sd_reset();   /* clear any attached SD card's command framing   */
  board_eink_reset(); /* clear any attached IT8951 controller's framing */
}

/**
 * @brief Print one line per SPI channel that echoed any frame.
 *
 * @details Also mirrors each active channel's transfer summary into the board
 * view's SPI console tab (::k_board_console_ch_spi). This single end-of-run
 * push is the model's coalesced transfer-completion point: the polling driver
 * clocks one 8-bit frame per SPDR write inside an unbounded loop and the model
 * exposes no per-transaction / CS-deassert boundary, so a per-frame push would
 * flood the 64-deep ring. The push is in-memory only and never touches stdout,
 * so the smoke / golden gates stay byte-identical.
 */
static void spi_report(void)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_spi_count; ch++) {
    if (s_spi[ch].frames > 0U) {
      (void)fprintf(stderr,
                    "  SPI%u          : %u frame(s) echoed (loopback=%s)\n",
                    ch,
                    s_spi[ch].frames,
                    s_spi[ch].loopback ? "yes" : "no");
      char ln[k_spi_console_line_cap];
      (void)snprintf(ln,
                     sizeof(ln),
                     "SPI%u: %u frames lb=%s",
                     ch,
                     s_spi[ch].frames,
                     s_spi[ch].loopback ? "on" : "off");
      board_console_push(k_board_console_ch_spi, ln);
    }
  }
  board_eink_report(); /* an attached IT8951 controller prints its own line */
}

/** @brief This block's descriptor (static lifetime; the core keeps the pointer). */
static const board_periph_block_t k_spi_block = {
  .base   = (uint64_t)k_spi_base,
  .span   = (uint64_t)k_spi_span,
  .order  = (uint32_t)k_block_order_sci,
  .read   = spi_read,
  .write  = spi_write,
  .tick   = nullptr,
  .reset  = spi_reset,
  .report = spi_report,
  .name   = "SPI_B",
};

/** @brief Self-register the SPI_B block before main runs (decentralized). */
[[gnu::constructor]] static void board_periph_spi_register(void)
{
  board_periph_register_block(&k_spi_block);
}
