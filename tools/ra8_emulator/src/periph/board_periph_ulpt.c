/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_periph_ulpt.c
 * @brief Ultra-Low-Power Timer (ULPT) peripheral-block model for ra8_emulator
 *
 * @details
 * Models the RA8D2 ULPT (ra8_ulpt_regs.h, ra8_ulpt.c): two channels of a
 * 32-bit reloading down-counter that clock from the LOCO-derived ULPTLCLK and
 * keep running through Software Standby. The ULPT is the canonical low-power
 * periodic-wake source, so this block is what lets the deep-idle examples run
 * their genuine wake path in the headless emulator:
 *
 *  - @c lpm_periodic_idle (hw_pending): each period it starts ULPT0, confirms
 *    @c ULPTCR.TCSTF = 1, drops to Software Standby (WFI), and relies on the
 *    ULPT0 underflow to cancel standby through the ICU -> NVIC path. Without a
 *    model the underflow event is never raised, so its registered ISR never
 *    runs and the wake counter stays 0; with this block the ULPT0_ULPTI event
 *    fires once per armed period, exactly as on silicon.
 *  - @c ulpt_demo / @c lpm_ulpt_standby (hw_validated): the same wake source,
 *    polled (TUNDF) and ISR-driven respectively.
 *
 * The ULPT control/status register (ULPTCR) shares the AGT's AGTCR bit layout
 * (HUM Ch 25.2.1 ULPTCR p 1190, mirror of HUM Ch 24.2.6 AGTCR p 1175): TSTART
 * is bit 0, the read-only count-status flag TCSTF is bit 1, the forced-stop
 * request TSTOP is bit 2, and the underflow flag TUNDF is bit 5. The model
 * follows that authoritative layout so a TCSTF poll (bit 1) and a TUNDF poll
 * (bit 5) both observe real timer state.
 *
 * Timing is instruction-counted, not wall-clock: while a channel is started the
 * counter steps down by ::k_ulpt_step_per_chunk each emulation chunk (the same
 * device-independent cadence the AGT model uses), so a multi-thousand-tick
 * period underflows within a handful of chunks and the periodic loops finish
 * inside the headless budget deterministically. The ULPT0 underflow event is
 * raised on the rising edge of TUNDF only (until firmware clears it by
 * stopping/re-arming the channel), so each armed period contributes exactly one
 * wake -- the wake count is reproducible run to run.
 *
 * Window: ULPT0 @ 0x40220000, ULPT1 @ 0x40220100 (stride 0x100), a single
 * 0x200-byte block (HUM Ch 25.1 "Ultra-Low-Power Timer (ULPT)" p 1187).
 *
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"

/**
 * @brief Per-tick order slot for the ULPT block.
 *
 * @details
 * Placed just above the RTC (60) and IPC (62) slots so the ULPT ticks with the
 * other low-frequency timer/event blocks. Only the relative order matters; the
 * absolute value is free because the block shares no state with another. A local
 * enum keeps the value typed without editing the shared header.
 */
typedef enum : uint32_t {
  k_ulpt_block_order = 64U, /**< ULPT tick / reset / report order slot. */
} ulpt_order_t;

/** @brief ULPT register window geometry (ra8_ulpt_regs.h). */
typedef enum : uint64_t {
  k_ulpt_base   = 0x40220000UL,  /**< ULPT0 base (FSP R_ULPT0_BASE).      */
  k_ulpt_stride = 0x100UL,       /**< Bytes per channel (ULPT0 -> ULPT1). */
  k_ulpt_count  = 2UL,           /**< ULPT0 and ULPT1.                    */
  k_ulpt_span   = 0x100UL * 2UL, /**< Whole-block window (both channels). */
} ulpt_geom_t;

/** @brief ULPT per-channel register byte offsets (ra8_ulpt_regs.h). */
typedef enum : uint64_t {
  k_ulpt_off_cnt = 0x00UL, /**< ULPTCNT counter / reload (32-bit). */
  k_ulpt_off_cma = 0x04UL, /**< ULPTCMA compare match A (32-bit).  */
  k_ulpt_off_cmb = 0x08UL, /**< ULPTCMB compare match B (32-bit).  */
  k_ulpt_off_cr  = 0x0CUL, /**< ULPTCR control / status (8-bit).   */
  k_ulpt_off_mr1 = 0x0DUL, /**< ULPTMR1 mode 1 (8-bit).            */
  k_ulpt_off_mr2 = 0x0EUL, /**< ULPTMR2 mode 2 (8-bit).            */
  k_ulpt_off_mr3 = 0x0FUL, /**< ULPTMR3 mode 3 (8-bit).            */
  k_ulpt_off_ioc = 0x10UL, /**< ULPTIOC I/O control (8-bit).       */
} ulpt_off_t;

/**
 * @brief ULPTCR control / status bit masks.
 *
 * @details
 * Identical to the AGT AGTCR layout (HUM Ch 25.2.1 ULPTCR p 1190, mirror of
 * HUM Ch 24.2.6 AGTCR p 1175). TSTART/TSTOP are control, TCSTF/TUNDF are status.
 */
typedef enum : uint8_t {
  k_ulpt_cr_tstart = 0x01U, /**< TSTART start request (bit 0, RW).            */
  k_ulpt_cr_tcstf  = 0x02U, /**< TCSTF count-status flag (bit 1, RO).         */
  k_ulpt_cr_tstop  = 0x04U, /**< TSTOP forced-stop request (bit 2, W).        */
  k_ulpt_cr_tundf  = 0x20U, /**< TUNDF underflow flag (bit 5, write-0-clear). */
} ulpt_cr_bit_t;

/**
 * @brief Down-count step applied per emulation chunk while a channel runs.
 *
 * @details
 * Sized above the largest period the deep-idle examples load (ULPTCNT = 0x8000
 * in ulpt_demo, 0x4000 in lpm_periodic_idle) so an armed channel underflows on
 * its first idle tick. That matters because ra8_emulator fast-forwards a Software
 * Standby WFI one tick (one SysTick period) at a time: the ULPT underflow has to
 * be raised within that single idle tick to be the source that cancels standby
 * (and to pend the ULPT0_ULPTI IRQ before the firmware stops the channel),
 * mirroring how the LOCO-clocked ULPT wakes the CPU on silicon. A period larger
 * than the step still counts down faithfully across several chunks for a polled
 * (non-standby) reader.
 */
typedef enum : uint32_t {
  k_ulpt_step_per_chunk =
    0x40000U, /**< 256 Ki ticks/chunk: example periods underflow on the first idle tick. */
} ulpt_rate_t;

/**
 * @brief ULPT0 underflow ELC event the model emits (HUM Ch 19 ELC).
 *
 * @details
 * The RA8D2 ELC signal table places the ULPT0 underflow interrupt (ULPT0_ULPTI)
 * at 0x080, matching @c k_ra8_elc_event_ulpt0_ulpti. A firmware that routes it
 * through @c ra8_isr_register writes 0x080 into an IELSR slot, so the core's ICU
 * links the raised event to that slot. ULPT1 has no distinct event in this
 * codebase, so only channel 0 raises one (no example drives ULPT1).
 */
typedef enum : uint16_t {
  k_ulpt_event_ch0_underflow = 0x080U, /**< ULPT0_ULPTI underflow event. */
} ulpt_event_t;

/** @brief Sub-word access constants for the byte-granular MMIO path. */
typedef enum : uint32_t {
  k_ulpt_byte_mask = 0xFFU, /**< Low 8 bits of a register lane. */
  k_ulpt_byte_bits = 8U,    /**< Bits per byte lane.            */
} ulpt_byte_t;

/** @brief One modelled ULPT channel: a 32-bit reloading down-counter + status. */
typedef struct {
  uint32_t counter;    /**< Live ULPTCNT count.             */
  uint32_t reload;     /**< Value last written to ULPTCNT.  */
  uint32_t cmpa;       /**< ULPTCMA compare match A.        */
  uint32_t cmpb;       /**< ULPTCMB compare match B.        */
  uint8_t  cr;         /**< ULPTCR control / status.        */
  uint8_t  mr1;        /**< ULPTMR1 mode 1.                 */
  uint8_t  mr2;        /**< ULPTMR2 mode 2.                 */
  uint8_t  mr3;        /**< ULPTMR3 mode 3.                 */
  uint8_t  ioc;        /**< ULPTIOC I/O control.            */
  uint32_t underflows; /**< Underflow event count (report). */
} ulpt_channel_t;

static ulpt_channel_t s_ulpt[k_ulpt_count];

/* =============================================================================
 * Register-cell access (route a byte access to the addressed channel field).
 * =============================================================================
 */

/** @brief Current 32-bit value of the register cell that contains @p off. */
static uint32_t ulpt_cell_value(const ulpt_channel_t* c, uint64_t off)
{
  /* HUM Ch 25.2.6 "ULPT : ULPT Counter Register" p 1198 */
  if (off < (uint64_t)k_ulpt_off_cma) {
    return c->counter;
  }
  /* HUM Ch 25 "Ultra-Low-Power Timer (ULPT)" p 1187 -- ULPTCMA */
  if (off < (uint64_t)k_ulpt_off_cmb) {
    return c->cmpa;
  }
  /* HUM Ch 25 "Ultra-Low-Power Timer (ULPT)" p 1187 -- ULPTCMB */
  if (off < (uint64_t)k_ulpt_off_cr) {
    return c->cmpb;
  }
  /* HUM Ch 25.2.1 "ULPTCR : ULPT Control Register" p 1190 */
  if (off == (uint64_t)k_ulpt_off_cr) {
    return c->cr;
  }
  /* HUM Ch 25.2.2 "ULPTMR1 : ULPT Mode Register 1" p 1192 */
  if (off == (uint64_t)k_ulpt_off_mr1) {
    return c->mr1;
  }
  /* HUM Ch 25.2.3 "ULPTMR2 : ULPT Mode Register 2" p 1192 */
  if (off == (uint64_t)k_ulpt_off_mr2) {
    return c->mr2;
  }
  /* HUM Ch 25.2.4 "ULPTMR3 : ULPT Mode Register 3" p 1195 */
  if (off == (uint64_t)k_ulpt_off_mr3) {
    return c->mr3;
  }
  /* HUM Ch 25.2.5 "ULPTIOC : ULPT I/O Control Register" p 1195 */
  return c->ioc;
}

/** @brief Byte offset of the start of the register cell that contains @p off. */
static uint64_t ulpt_cell_base(uint64_t off)
{
  if (off < (uint64_t)k_ulpt_off_cma) {
    return (uint64_t)k_ulpt_off_cnt;
  }
  if (off < (uint64_t)k_ulpt_off_cmb) {
    return (uint64_t)k_ulpt_off_cma;
  }
  if (off < (uint64_t)k_ulpt_off_cr) {
    return (uint64_t)k_ulpt_off_cmb;
  }
  return off; /* the 8-bit control / mode / io cells are one byte each */
}

/* =============================================================================
 * MMIO read / write.
 * =============================================================================
 */

/** @brief Read @p size bytes little-endian from the addressed ULPT channel. */
static uint64_t ulpt_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  const uint64_t rel = addr - (uint64_t)k_ulpt_base;
  const uint32_t ch  = (uint32_t)(rel / (uint64_t)k_ulpt_stride);
  if (ch >= (uint32_t)k_ulpt_count) {
    return 0U;
  }
  const uint64_t off  = rel % (uint64_t)k_ulpt_stride;
  const uint32_t cell = ulpt_cell_value(&s_ulpt[ch], off);
  const uint32_t lane = (uint32_t)(off - ulpt_cell_base(off));
  uint64_t       v    = 0U;
  for (unsigned i = 0U; i < size; i++) {
    const uint32_t b = lane + (uint32_t)i;
    if (b < (uint32_t)sizeof(uint32_t)) {
      const uint32_t lane_byte =
        (cell >> ((uint32_t)k_ulpt_byte_bits * b)) & (uint32_t)k_ulpt_byte_mask;
      v |= (uint64_t)lane_byte << ((uint32_t)k_ulpt_byte_bits * (uint32_t)i);
    }
  }
  return v;
}

/** @brief Apply a write to ULPTCR: TSTART arms, TCSTF tracks it, TUNDF clears on 0. */
static void ulpt_write_cr(ulpt_channel_t* c, uint8_t value)
{
  /* HUM Ch 25.2.1 "ULPTCR : ULPT Control Register" p 1190 -- TUNDF is
   * write-0-to-clear (ra8_ulpt_stop clears it by writing 0); TSTART is RW and
   * TCSTF tracks TSTART. Mirrors the AGT AGTCR model. */
  const uint8_t status_keep = (uint8_t)(c->cr & value & (uint8_t)k_ulpt_cr_tundf);
  const uint8_t start       = (uint8_t)(value & (uint8_t)k_ulpt_cr_tstart);
  c->cr = (uint8_t)(start | (start != 0U ? (uint8_t)k_ulpt_cr_tcstf : 0U) | status_keep);
}

/** @brief Write @p size bytes little-endian into the addressed ULPT channel. */
static void ulpt_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  const uint64_t rel = addr - (uint64_t)k_ulpt_base;
  const uint32_t ch  = (uint32_t)(rel / (uint64_t)k_ulpt_stride);
  if (ch >= (uint32_t)k_ulpt_count) {
    return;
  }
  ulpt_channel_t* c   = &s_ulpt[ch];
  const uint64_t  off = rel % (uint64_t)k_ulpt_stride;
  /* The ra8_ulpt driver only ever issues aligned, full-width stores: a 32-bit
   * write to ULPTCNT/CMA/CMB and an 8-bit write to ULPTCR/MRn/IOC. */
  if (off < (uint64_t)k_ulpt_off_cma) {
    /* HUM Ch 25.2.6 "ULPT : ULPT Counter Register" p 1198 -- a write seeds both
     * the live counter and the reload value (ra8_ulpt_start writes the period). */
    c->counter = (uint32_t)value;
    c->reload  = (uint32_t)value;
  } else if (off < (uint64_t)k_ulpt_off_cmb) {
    /* HUM Ch 25 "Ultra-Low-Power Timer (ULPT)" p 1187 -- ULPTCMA */
    c->cmpa = (uint32_t)value;
  } else if (off < (uint64_t)k_ulpt_off_cr) {
    /* HUM Ch 25 "Ultra-Low-Power Timer (ULPT)" p 1187 -- ULPTCMB */
    c->cmpb = (uint32_t)value;
  } else if (off == (uint64_t)k_ulpt_off_cr) {
    ulpt_write_cr(c, (uint8_t)value);
  } else if (off == (uint64_t)k_ulpt_off_mr1) {
    /* HUM Ch 25.2.2 "ULPTMR1 : ULPT Mode Register 1" p 1192 */
    c->mr1 = (uint8_t)value;
  } else if (off == (uint64_t)k_ulpt_off_mr2) {
    /* HUM Ch 25.2.3 "ULPTMR2 : ULPT Mode Register 2" p 1192 */
    c->mr2 = (uint8_t)value;
  } else if (off == (uint64_t)k_ulpt_off_mr3) {
    /* HUM Ch 25.2.4 "ULPTMR3 : ULPT Mode Register 3" p 1195 */
    c->mr3 = (uint8_t)value;
  } else if (off == (uint64_t)k_ulpt_off_ioc) {
    /* HUM Ch 25.2.5 "ULPTIOC : ULPT I/O Control Register" p 1195 */
    c->ioc = (uint8_t)value;
  }
  (void)size;
}

/* =============================================================================
 * Per-tick down-count + edge-triggered underflow event.
 * =============================================================================
 */

/** @brief Advance one running ULPT channel by one chunk; raise its underflow event. */
static void ulpt_tick_channel(uc_engine* uc, uint32_t ch)
{
  ulpt_channel_t* c = &s_ulpt[ch];
  /* HUM Ch 25.2.1 "ULPTCR : ULPT Control Register" p 1190 -- TSTART gate */
  if ((c->cr & (uint8_t)k_ulpt_cr_tstart) == 0U) {
    return; /* stopped: counter holds */
  }
  const uint32_t step = (uint32_t)k_ulpt_step_per_chunk;
  if (c->counter > step) {
    c->counter -= step;
    return;
  }
  /* Underflow: reload from the latched period and continue (periodic). */
  const uint32_t span    = c->reload + 1U;
  const uint32_t deficit = step - c->counter;
  c->counter             = c->reload - ((deficit - 1U) % span);
  /* Raise only on the rising edge of TUNDF -- one wake per armed period, until
   * the firmware clears it by stopping/re-arming the channel. */
  if ((c->cr & (uint8_t)k_ulpt_cr_tundf) == 0U) {
    /* HUM Ch 25.2.1 "ULPTCR : ULPT Control Register" p 1190 -- TUNDF set */
    c->cr |= (uint8_t)k_ulpt_cr_tundf;
    c->underflows++;
    if (ch == 0U) {
      board_periph_icu_raise_event(uc, (uint16_t)k_ulpt_event_ch0_underflow);
    }
    if (board_periph_trace()) {
      (void)fprintf(stderr, "  ULPT%u         : underflow (reload=0x%08X)\n", ch, c->reload);
    }
  }
}

/** @brief Advance every running ULPT channel one chunk. */
static void ulpt_tick(uc_engine* uc)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_ulpt_count; ch++) {
    ulpt_tick_channel(uc, ch);
  }
}

/* =============================================================================
 * Reset / report.
 * =============================================================================
 */

/** @brief Clear every ULPT channel to its power-on state. */
static void ulpt_reset(void)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_ulpt_count; ch++) {
    s_ulpt[ch] = (ulpt_channel_t){};
  }
}

/** @brief Print each ULPT channel that underflowed at least once. */
static void ulpt_report(void)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_ulpt_count; ch++) {
    const ulpt_channel_t* c = &s_ulpt[ch];
    if (c->underflows == 0U) {
      continue;
    }
    (void)fprintf(stderr,
                  "  ULPT%u         : counter=0x%08X underflows=%u (running=%s)\n",
                  ch,
                  c->counter,
                  c->underflows,
                  (c->cr & (uint8_t)k_ulpt_cr_tstart) ? "yes" : "no");
  }
}

/** @brief ULPT register window + the down-count tick / reset / report. */
static const board_periph_block_t k_ulpt_block = {
  .base   = (uint64_t)k_ulpt_base,
  .span   = (uint64_t)k_ulpt_span,
  .order  = (uint32_t)k_ulpt_block_order,
  .read   = ulpt_read,
  .write  = ulpt_write,
  .tick   = ulpt_tick,
  .reset  = ulpt_reset,
  .report = ulpt_report,
  .name   = "ULPT",
};

/** @brief Self-register the ULPT window before main runs (decentralized). */
[[gnu::constructor]] static void board_periph_ulpt_register(void)
{
  board_periph_register_block(&k_ulpt_block);
}
