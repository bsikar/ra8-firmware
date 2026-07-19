/**
 * @file board_periph_canfd.c
 * @brief CAN-FD controller peripheral-block model for the board emulator
 *
 * @details
 * Models the two RA8D2 CANFD instances (ra8_canfd_regs.h / ra8_canfd.c) at
 * 0x40380000 (CANFD0) and 0x40382000 (CANFD1), each a 0x1920-byte channel
 * window, so the internal-loopback demo round-trips a frame instead of timing
 * out in the sparse fallback. Two behaviours are modelled on top of a flat
 * read-back register store:
 *
 *  - **Mode handshakes.** The driver (built without RA8_SIMULATOR_MODE for the
 *    real cross-compiled .elf) polls the global / channel state machine after
 *    each mode write and treats a stuck poll as a timeout. So a write to
 *    CFDGCTR.GMDC is reflected into CFDGSTS (reset -> GRSTSTS, halt -> GHLTSTS,
 *    operation -> both clear) and a write to CFDC[0].CTR.CHMDC into CFDC[0].STS
 *    (reset -> CRSTSTS, halt -> CHLTSTS, operation -> both clear). CFDGSTS
 *    powers up with GRAMINIT already clear so the init RAM-ready poll completes.
 *
 *  - **Internal loopback.** RX FIFO 0 powers up empty (CFDRFSTS[0].RFEMP = 1).
 *    Asserting CFDTMC[0].TMTR (the transmit request) copies the TX message
 *    buffer CFDTM[0].{ID,PTR,FDCTR,DF[]} into RX FIFO slot CFDRF[0].{ID,PTR,
 *    FDSTS,DF[]}, clears RFEMP (and sets RFIF, the RX-available flag), and marks
 *    the TX done (CFDTMSTS[0].TMTRF = 10b). The firmware then polls RFEMP clear,
 *    reads the mirrored frame back, and pops it by writing CFDRFPCTR[0] = 0xFF,
 *    which re-arms RFEMP. The delivered frame is byte-identical to the
 *    transmitted one, so the demo's RX == TX check passes.
 *
 * Self-registers its descriptor with the board_periph core from a file-scope
 * constructor; the core keeps no central block list -- see board_periph_block.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_console.h"
#include "board_periph_block.h"

/**
 * @enum canfd_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_canfd_val_64 = 64,
} canfd_uint8_const_t;

/** @brief CANFD block geometry (ra8_canfd_regs.h, FSP R_CANFD_Type). */
typedef enum : uint64_t {
  k_canfd0_base       = 0x40380000UL,   /**< CANFD0 channel-window base.           */
  k_canfd1_base       = 0x40382000UL,   /**< CANFD1 channel-window base.           */
  k_canfd_span        = 0x1920UL,       /**< Full FSP R_CANFD_Type window size.    */
  k_canfd_count       = 2UL,            /**< CANFD0 / CANFD1.                      */
  k_canfd_words       = 0x1920UL / 4UL, /**< CANFD words.                          */
  k_canfd_off_cnctr   = 0x004UL,        /**< CFDC[0].CTR channel control.          */
  k_canfd_off_cnsts   = 0x008UL,        /**< CFDC[0].STS channel status.           */
  k_canfd_off_gctr    = 0x018UL,        /**< CFDGCTR global control.               */
  k_canfd_off_gsts    = 0x01CUL,        /**< CFDGSTS global status.                */
  k_canfd_off_rfsts0  = 0x044UL,        /**< CFDRFSTS[0] RX FIFO 0 status.         */
  k_canfd_off_rfpctr0 = 0x04CUL,        /**< CFDRFPCTR[0] RX FIFO 0 pointer ctrl.  */
  k_canfd_off_tmc0    = 0x070UL,        /**< CFDTMC[0] TX MB 0 control byte.       */
  k_canfd_off_tmsts0  = 0x074UL,        /**< CFDTMSTS[0] TX MB 0 status byte.      */
  k_canfd_off_rf0     = 0x520UL,        /**< CFDRF[0] RX FIFO 0 access window.     */
  k_canfd_off_tm0     = 0x604UL,        /**< CFDTM[0] TX MB 0 access window.       */
  k_canfd_frame_words = 0x4CUL / 4UL,   /**< ID+PTR+FDSTS+DF[64] = 19 words.       */
  k_canfd_off_afl     = 0x120UL,        /**< CFDGAFL[] acceptance-filter window.   */
  k_canfd_afl_stride  = 0x10UL,         /**< Bytes per CFDGAFL entry (ID/M/P0/P1). */
  k_canfd_afl_count   = 16UL,           /**< Entries in one CFDGAFL page window.   */
} canfd_map_t;

/** @brief Global status (CFDGSTS) bit masks the model drives. */
typedef enum : uint32_t {
  k_gsts_grststs  = 1UL << 0U, /**< Global reset status.         */
  k_gsts_ghltsts  = 1UL << 1U, /**< Global halt status.          */
  k_gsts_graminit = 1UL << 3U, /**< Global RAM-init in progress. */
} canfd_gsts_t;

/** @brief Channel status (CFDC[0].STS) bit masks the model drives. */
typedef enum : uint32_t {
  k_cnsts_crstst = 1UL << 0U, /**< Channel reset status. */
  k_cnsts_chltst = 1UL << 1U, /**< Channel halt status.  */
} canfd_cnsts_t;

/** @brief Message-ID field masks (CFDRF.ID / CFDTM.ID), for trace + report. */
typedef enum : uint32_t {
  k_canfd_id_std = 0x000007FFUL, /**< 11-bit standard ID. */
  k_canfd_id_ext = 0x1FFFFFFFUL, /**< 29-bit extended ID. */
} canfd_id_mask_t;

/** @brief CFDTM/CFDRF.PTR frame fields (PTR layout: [15:0] timestamp, [31:28] DLC). */
typedef enum : uint32_t {
  k_canfd_ptr_word      = 1UL,   /**< PTR is the 2nd word of a frame window. */
  k_canfd_ptr_dlc_shift = 28UL,  /**< DLC occupies PTR bits [31:28].         */
  k_canfd_ptr_dlc_mask  = 0xFUL, /**< 4-bit DLC code mask (0..15).           */
} canfd_ptr_t;

/** @brief Global / channel mode-control field (GMDC / CHMDC, bits [1:0]). */
typedef enum : uint32_t {
  k_mode_mask      = 0x3UL, /**< 2-bit mode field. */
  k_mode_operation = 0x0UL, /**< Operation mode.   */
  k_mode_reset     = 0x1UL, /**< Reset mode.       */
  k_mode_halt      = 0x2UL, /**< Halt mode.        */
} canfd_mode_t;

/** @brief RX FIFO status (CFDRFSTS[0]) bit masks. */
typedef enum : uint32_t {
  k_rfsts_empty = 1UL << 0U, /**< RFEMP: FIFO empty flag.  */
  k_rfsts_if    = 1UL << 3U, /**< RFIF: RX interrupt flag. */
} canfd_rfsts_t;

/** @brief TX MB control / status byte bits. */
typedef enum : uint8_t {
  k_tmc_txreq  = 1U << 0U, /**< CFDTMC.TMTR transmit request.  */
  k_tmsts_done = 0x04U,    /**< CFDTMSTS.TMTRF = 10b complete. */
} canfd_tmc_t;

/**
 * @brief CANFD0 RX-FIFO (RXF) ELC event the model pends on loopback delivery.
 *
 * @details
 * RA8D2 ELC event signal table (HUM Ch 19 Table 19.3) groups the CANFD0
 * interrupts together; the global RX-FIFO event (CAN0_RXF) is the RX-available
 * interrupt source. The canfd_loopback demo is purely POLLED -- it registers no
 * CAN ISR and links no IELSR slot to this event -- so pending it is a no-op for
 * that demo and is done only so an interrupt-driven receiver would also be
 * exercised. The numeric value is not verified against a local FSP bsp_elc.h
 * (FSP is not installed here); it is unused on the demo's success path, which
 * rests entirely on the RFEMP / RFIF status modelling above.
 */
typedef enum : uint16_t {
  k_event_can0_rxf = 0x33DU, /**< CANFD0 RXF RX-FIFO event (best-effort). */
} canfd_elc_event_t;

/** @brief One CANFD instance: flat register backing store. */
typedef struct {
  uint32_t reg[k_canfd_words]; /**< Word-addressed register backing. */
  uint32_t loopbacks;          /**< Frames delivered TX -> RX FIFO.  */
} canfd_inst_t;

static canfd_inst_t s_canfd[k_canfd_count];

/** @brief Resolve an absolute address to its instance index (0/1) or count. */
static uint32_t canfd_instance(uint64_t addr)
{
  if ((addr >= (uint64_t)k_canfd0_base) &&
      (addr < (uint64_t)k_canfd0_base + (uint64_t)k_canfd_span)) {
    return 0U;
  }
  if ((addr >= (uint64_t)k_canfd1_base) &&
      (addr < (uint64_t)k_canfd1_base + (uint64_t)k_canfd_span)) {
    return 1U;
  }
  return (uint32_t)k_canfd_count;
}

/** @brief In-window byte offset of @p addr for instance @p inst. */
static uint64_t canfd_offset(uint32_t inst, uint64_t addr)
{
  const uint64_t base = (inst == 0U) ? (uint64_t)k_canfd0_base : (uint64_t)k_canfd1_base;
  return addr - base;
}

/** @brief Word index into the backing store for an in-window offset. */
static uint32_t canfd_word(uint64_t off)
{
  return (uint32_t)(off >> 2U);
}

/** @brief Apply a mode-control write to its matching status register bits. */
static void canfd_apply_global_mode(canfd_inst_t* c, uint32_t gctr)
{
  uint32_t gsts = c->reg[canfd_word((uint64_t)k_canfd_off_gsts)];
  gsts &= ~(uint32_t)(k_gsts_grststs | k_gsts_ghltsts | k_gsts_graminit);
  const uint32_t mode = gctr & (uint32_t)k_mode_mask;
  if (mode == (uint32_t)k_mode_reset) {
    gsts |= (uint32_t)k_gsts_grststs;
  } else if (mode == (uint32_t)k_mode_halt) {
    gsts |= (uint32_t)k_gsts_ghltsts;
  }
  c->reg[canfd_word((uint64_t)k_canfd_off_gsts)] = gsts;
}

/** @brief Apply a channel mode-control write to its matching status bits. */
static void canfd_apply_channel_mode(canfd_inst_t* c, uint32_t ctr)
{
  uint32_t sts = c->reg[canfd_word((uint64_t)k_canfd_off_cnsts)];
  sts &= ~(uint32_t)(k_cnsts_crstst | k_cnsts_chltst);
  const uint32_t mode = ctr & (uint32_t)k_mode_mask;
  if (mode == (uint32_t)k_mode_reset) {
    sts |= (uint32_t)k_cnsts_crstst;
  } else if (mode == (uint32_t)k_mode_halt) {
    sts |= (uint32_t)k_cnsts_chltst;
  }
  c->reg[canfd_word((uint64_t)k_canfd_off_cnsts)] = sts;
}

/**
 * @brief Decide whether @p tx_id passes the programmed acceptance filter.
 *
 * @details
 * Scans the CFDGAFL page-0 entry window (the demos use filter ids < one page,
 * so paging does not apply). An entry is active when its mask (CFDGAFL.M) is
 * non-zero; a frame is accepted if it matches any active entry,
 * @c (tx_id & mask) == (accept_id & mask). With no active entry the filter is
 * open -- internal loopback that programs no filters (canfd_loopback) keeps
 * delivering every frame, while canfd_filter_demo's programmed slots drop the
 * non-matching id.
 *
 * @param[in] c     Channel instance whose CFDGAFL window to read.
 * @param[in] tx_id Transmitted frame id (29-bit field, IDE/RTR masked off).
 * @return true if the frame is accepted (deliver), false if filtered out.
 */
static bool canfd_frame_accepted(const canfd_inst_t* c, uint32_t tx_id)
{
  bool any_active = false;
  for (uint32_t slot = 0U; slot < (uint32_t)k_canfd_afl_count; slot++) {
    const uint64_t ent =
      (uint64_t)k_canfd_off_afl + ((uint64_t)slot * (uint64_t)k_canfd_afl_stride);
    const uint32_t mask = c->reg[canfd_word(ent + 4UL)] & (uint32_t)k_canfd_id_ext;
    if (mask == 0U) {
      continue; /* unprogrammed slot */
    }
    any_active               = true;
    const uint32_t accept_id = c->reg[canfd_word(ent)] & (uint32_t)k_canfd_id_ext;
    if ((tx_id & mask) == (accept_id & mask)) {
      return true;
    }
  }
  return !any_active;
}

/**
 * @brief Push a one-line CAN-FD frame summary to the board console CAN lane.
 *
 * @details
 * Formats a short, human-readable record of a single transmitted CAN-FD frame
 * -- the originating controller instance, the identifier in hex, the 4-bit DLC
 * code, and the @p outcome note -- into a bounded stack buffer and appends it to
 * ::k_board_console_ch_can via ::board_console_push. This is the console-tab tap
 * only: it performs no register access and writes nothing to stdout/stderr, so
 * the smoke/golden output stays byte-identical. It is invoked once per transmit
 * request (one logical CAN frame), never per byte, so the ring does not flood.
 *
 * @param[in] inst    CANFD instance index (0 = CANFD0, 1 = CANFD1).
 * @param[in] tx_id   Transmitted identifier, already masked to 29 bits.
 * @param[in] dlc     4-bit data-length code from CFDTM.PTR[31:28] (0..15).
 * @param[in] outcome Borrowed NUL-terminated delivery note (e.g. "-> RXF0").
 * @return Nothing.
 * @pre @p outcome is a valid NUL-terminated string.
 * @pre @p inst is a valid CANFD instance index (< k_canfd_count).
 * @post Exactly one line is appended to the CAN console lane.
 * @post No register, stdout, or stderr state is modified.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
static void canfd_console_frame(uint32_t inst, uint32_t tx_id, uint32_t dlc, const char* outcome)
{
  char ln[k_canfd_val_64];
  (void)snprintf(ln, sizeof(ln), "CANFD%u TX id=0x%X dlc=%u %s", inst, tx_id, dlc, outcome);
  board_console_push(k_board_console_ch_can, ln);
}

/** @brief Copy TX MB 0 into RX FIFO 0 and flag the frame available. */
static void canfd_loopback_deliver(uc_engine* uc, uint32_t inst)
{
  canfd_inst_t*  c     = &s_canfd[inst];
  const uint32_t tm    = canfd_word((uint64_t)k_canfd_off_tm0);
  const uint32_t rf    = canfd_word((uint64_t)k_canfd_off_rf0);
  const uint32_t tx_id = c->reg[tm] & (uint32_t)k_canfd_id_ext;
  const uint32_t ptr   = c->reg[tm + (uint32_t)k_canfd_ptr_word];
  const uint32_t dlc   = (ptr >> (uint32_t)k_canfd_ptr_dlc_shift) & (uint32_t)k_canfd_ptr_dlc_mask;
  /* The frame is transmitted regardless of the filter; mark TX complete.
   * CFDTMSTS[0] is a byte at a word-aligned offset, so its low byte carries
   * TMTRF = 10b ("transmission complete"). */
  c->reg[canfd_word((uint64_t)k_canfd_off_tmsts0)] = (uint32_t)k_tmsts_done;
  if (!canfd_frame_accepted(c, tx_id)) {
    if (board_periph_trace()) {
      (void)fprintf(stderr,
                    "  [trace] CANFD%u loopback: TX id=0x%X filtered (no AFL match)\n",
                    inst,
                    tx_id);
    }
    canfd_console_frame(inst, tx_id, dlc, "filtered");
    return; /* filtered out: transmitted but not received */
  }
  for (uint32_t w = 0U; w < (uint32_t)k_canfd_frame_words; w++) {
    c->reg[rf + w] = c->reg[tm + w];
  }
  /* RX FIFO 0 now holds one frame: clear empty, raise the RX flag. */
  uint32_t rfsts = c->reg[canfd_word((uint64_t)k_canfd_off_rfsts0)];
  rfsts &= ~(uint32_t)k_rfsts_empty;
  rfsts |= (uint32_t)k_rfsts_if;
  c->reg[canfd_word((uint64_t)k_canfd_off_rfsts0)] = rfsts;
  c->loopbacks++;
  if (inst == 0U) {
    board_periph_icu_raise_event(uc, (uint16_t)k_event_can0_rxf);
  }
  if (board_periph_trace()) {
    (void)fprintf(stderr,
                  "  [trace] CANFD%u loopback: TX MB0 -> RX FIFO0 (id=0x%X)\n",
                  inst,
                  c->reg[rf] & (uint32_t)k_canfd_id_ext);
  }
  canfd_console_frame(inst, tx_id, dlc, "-> RXF0");
}

/** @brief MMIO read inside a CANFD window: read back the latched register. */
static uint64_t canfd_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint32_t inst = canfd_instance(addr);
  if (inst >= (uint32_t)k_canfd_count) {
    return 0U;
  }
  const uint64_t off = canfd_offset(inst, addr);
  if (off >= (uint64_t)k_canfd_span) {
    return 0U;
  }
  return s_canfd[inst].reg[canfd_word(off)];
}

/** @brief Handle a write to a CANFD register with side effects, else latch it. */
static void canfd_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)size;
  const uint32_t inst = canfd_instance(addr);
  if (inst >= (uint32_t)k_canfd_count) {
    return;
  }
  const uint64_t off = canfd_offset(inst, addr);
  if (off >= (uint64_t)k_canfd_span) {
    return;
  }
  canfd_inst_t* c = &s_canfd[inst];
  /* CFDTMC[0] is a byte register; a TMTR request triggers the loopback. */
  if (off == (uint64_t)k_canfd_off_tmc0) {
    c->reg[canfd_word(off)] = (uint32_t)value;
    if (((uint8_t)value & (uint8_t)k_tmc_txreq) != 0U) {
      canfd_loopback_deliver(uc, inst);
    }
    return;
  }
  /* Popping RX FIFO 0 (CFDRFPCTR write) returns it to the empty state. */
  if (off == (uint64_t)k_canfd_off_rfpctr0) {
    c->reg[canfd_word(off)] = (uint32_t)value;
    uint32_t rfsts          = c->reg[canfd_word((uint64_t)k_canfd_off_rfsts0)];
    rfsts |= (uint32_t)k_rfsts_empty;
    rfsts &= ~(uint32_t)k_rfsts_if;
    c->reg[canfd_word((uint64_t)k_canfd_off_rfsts0)] = rfsts;
    return;
  }
  c->reg[canfd_word(off)] = (uint32_t)value;
  if (off == (uint64_t)k_canfd_off_gctr) {
    canfd_apply_global_mode(c, (uint32_t)value);
  } else if (off == (uint64_t)k_canfd_off_cnctr) {
    canfd_apply_channel_mode(c, (uint32_t)value);
  }
}

/** @brief Reset both CANFD instances to a coherent power-on state. */
static void canfd_reset(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_canfd_count; i++) {
    (void)memset(&s_canfd[i], 0, sizeof(s_canfd[i]));
    /* RX FIFO 0 powers up empty; GRAMINIT powers up already-clear (0). */
    s_canfd[i].reg[canfd_word((uint64_t)k_canfd_off_rfsts0)] = (uint32_t)k_rfsts_empty;
  }
}

/** @brief Print one line per CANFD instance that looped a frame back. */
static void canfd_report(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_canfd_count; i++) {
    if (s_canfd[i].loopbacks == 0U) {
      continue;
    }
    const uint32_t rf = canfd_word((uint64_t)k_canfd_off_rf0);
    (void)fprintf(stderr,
                  "  CANFD%u        : loopback frames=%u last_rx_id=0x%03X\n",
                  i,
                  s_canfd[i].loopbacks,
                  s_canfd[i].reg[rf] & (uint32_t)k_canfd_id_std);
  }
}

/** @brief CANFD0 register window + the shared loopback reset / report. */
static const board_periph_block_t k_canfd0_block = {
  .base   = (uint64_t)k_canfd0_base,
  .span   = (uint64_t)k_canfd_span,
  .order  = (uint32_t)k_block_order_i2c, /* After the timers/UART; no tick. */
  .read   = canfd_read,
  .write  = canfd_write,
  .tick   = nullptr,
  .reset  = canfd_reset,
  .report = canfd_report,
  .name   = "CANFD0",
};

/** @brief CANFD1 register window (reset / report handled by the CANFD0 block). */
static const board_periph_block_t k_canfd1_block = {
  .base   = (uint64_t)k_canfd1_base,
  .span   = (uint64_t)k_canfd_span,
  .order  = (uint32_t)k_block_order_i2c,
  .read   = canfd_read,
  .write  = canfd_write,
  .tick   = nullptr,
  .reset  = nullptr,
  .report = nullptr,
  .name   = "CANFD1",
};

/** @brief Self-register both CANFD windows before main runs (decentralized). */
[[gnu::constructor]] static void board_periph_canfd_register(void)
{
  board_periph_register_block(&k_canfd0_block);
  board_periph_register_block(&k_canfd1_block);
}
