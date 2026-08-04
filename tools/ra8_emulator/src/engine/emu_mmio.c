/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file emu_mmio.c
 * @brief Sparse MMIO model implementation (see emu_mmio.h)
 *
 * @details
 * The slot table, settle toggle, MRMS readback quirk, BG_BGC witness and the
 * run-end report -- moved verbatim out of the ra8_emulator main translation
 * unit. Modelled board_periph blocks always answer first; this model is the
 * fallback for addresses no block owns.
 *
 *
 * @since 0.1.0
 */

#include "emu_mmio.h"

#include <stdio.h>

#include "board_periph.h"
#include "emu_cpu1.h"

/** @brief Sparse-model sizing and settle thresholds. */
typedef enum : uint32_t {
  k_mmio_slots     = 2048U,       /**< Distinct MMIO addresses tracked.         */
  k_mmio_settle    = 8U,          /**< Same-addr reads before a poll "settles". */
  k_mmio_print_max = 256U,        /**< Max MMIO rows printed in the summary.    */
  k_u32_all_ones   = 0xFFFFFFFFU, /**< All bits set (MMIO read toggle).         */
} emu_mmio_cfg_t;

/* Renesas peripheral quirks that the generic sparse model cannot reproduce.
 *
 * MRMS frequency latches: the CGC driver (libs/ra8_hal/src/ra8_cgc.c,
 * internal_wait_mrm_freq) writes ``key | freq_mhz`` to MRCFREQ / MREFREQ and
 * spins until the register reads back == freq_mhz. Real silicon validates the
 * upper key byte then strips it, so the readback is the bare frequency. The
 * generic model reflects the full written word (key still in bits[31:24]), so
 * the readback never equals freq and the poll runs to its 0x40000 timeout ->
 * lcd_panic_halt. Model the hardware: on readback of these two registers,
 * return the stored value with the key byte masked off. */
typedef enum : uint64_t {
  k_mrms_mrcfreq   = 0x4013C004UL, /**< MRICLK freq latch (write key 0x1E). */
  k_mrms_mrefreq   = 0x4013C008UL, /**< MRPCLK freq latch (write key 0xE1). */
  k_mrms_freq_mask = 0x00FFFFFFUL, /**< Key byte (bits[31:24]) stripped.    */
} mrms_quirk_t;

/* Sparse model of the Renesas peripheral space. Each touched address gets a
 * slot: control writes are reflected back on read so "configure then verify"
 * works, but once the firmware spins reading one address (a "wait for
 * ready/idle" poll) past k_mmio_settle, reads alternate 0 / all-ones so a
 * single-bit poll for either edge (flag set OR flag clear) completes instead
 * of running to its timeout. */
static uint64_t s_mmio_addr[k_mmio_slots];
static uint32_t s_mmio_val[k_mmio_slots];
static bool     s_mmio_written[k_mmio_slots];
static uint32_t s_mmio_rcount[k_mmio_slots];
static uint32_t s_mmio_wcount[k_mmio_slots];
static uint32_t s_mmio_n;
static uint32_t s_mmio_reads;
static uint32_t s_mmio_writes;
static uint32_t s_mmio_toggle;

static int      s_mmio_cache    = -1; /**< 1-entry address->slot lookup cache. */
static int      s_mmio_run_slot = -1; /**< Slot of the current read run.       */
static uint32_t s_mmio_run;           /**< Consecutive reads of that slot.     */

/* BG_BGC colour-cycle witness: total writes and the distinct values seen. */
static uint32_t s_bgc_writes;
static uint32_t s_bgc_distinct[k_bgc_track_max];
static uint32_t s_bgc_distinct_n;

static uint32_t s_bgc_writes;
static uint32_t s_bgc_distinct[k_bgc_track_max];
static uint32_t s_bgc_distinct_n;

/** @brief Record a BG_BGC write; remember the value if it is a new colour. */
static void bgc_track(uint32_t value)
{
  s_bgc_writes++;
  for (uint32_t i = 0U; i < s_bgc_distinct_n; i++) {
    if (s_bgc_distinct[i] == value) {
      return;
    }
  }
  if (s_bgc_distinct_n < (uint32_t)k_bgc_track_max) {
    s_bgc_distinct[s_bgc_distinct_n++] = value;
  }
}

/** @brief Find (or add) a slot for a distinct MMIO address; -1 if table full. */
static int mmio_index(uint64_t addr)
{
  if ((s_mmio_cache >= 0) && (s_mmio_addr[s_mmio_cache] == addr)) {
    return s_mmio_cache;
  }
  for (uint32_t i = 0U; i < s_mmio_n; i++) {
    if (s_mmio_addr[i] == addr) {
      s_mmio_cache = (int)i;
      return (int)i;
    }
  }
  if (s_mmio_n < (uint32_t)k_mmio_slots) {
    s_mmio_addr[s_mmio_n] = addr;
    s_mmio_cache          = (int)s_mmio_n;
    return (int)(s_mmio_n++);
  }
  return -1;
}

uint64_t mmio_read(uc_engine* uc, uint64_t offset, unsigned size, void* user)
{
  (void)user;
  s_mmio_reads++;
  /* A modelled peripheral block answers first; the sparse fallback below is
   * only reached for addresses no block in board_periph owns. */
  bool           handled = false;
  const uint64_t modeled = board_periph_read(uc, (uint64_t)k_periph_base + offset, size, &handled);
  if (handled) {
    return modeled;
  }
  const int idx = mmio_index((uint64_t)k_periph_base + offset);
  if (idx >= 0) {
    s_mmio_rcount[idx]++;
    if (idx == s_mmio_run_slot) {
      s_mmio_run++;
    } else {
      s_mmio_run_slot = idx;
      s_mmio_run      = 1U;
    }
    /* Reflect a written control value until a spin-poll forces it to settle. */
    if (s_mmio_written[idx] && (s_mmio_run <= (uint32_t)k_mmio_settle)) {
      const uint64_t addr = (uint64_t)k_periph_base + offset;
      /* MRMS frequency latches strip the write key byte on readback so the
       * driver's "wait until reg == freq" poll completes (see mrms_quirk_t). */
      if ((addr == (uint64_t)k_mrms_mrcfreq) || (addr == (uint64_t)k_mrms_mrefreq)) {
        return (uint64_t)(s_mmio_val[idx] & (uint32_t)k_mrms_freq_mask);
      }
      return (uint64_t)s_mmio_val[idx];
    }
  }
  s_mmio_toggle ^= (uint32_t)k_u32_all_ones;
  return (uint64_t)s_mmio_toggle;
}

/**
 * @brief Side-effect-free read of the last value written to a peripheral reg.
 *
 * @details
 * Returns the value last written to @p addr, or 0 if it was never written --
 * searching the MMIO shadow WITHOUT allocating a slot and WITHOUT advancing the
 * spin-settle toggle that ::mmio_read uses. ra8_emulator's own introspection (e.g.
 * ::build_frame reading GLCDC registers to compose the panel) must see stable
 * state: a firmware that never programs the GLCDC (blink, USB, UART demos) would
 * otherwise read the status-poll fallthrough (an alternating 0/0xFFFFFFFF), which
 * made the panel strobe black<->white every frame. A real read of an unwritten
 * register reset-defaults to 0 here, so the panel is a steady background.
 */
uint32_t mmio_peek(uint64_t addr)
{
  for (uint32_t i = 0U; i < s_mmio_n; i++) {
    if (s_mmio_addr[i] == addr) {
      return s_mmio_written[i] ? s_mmio_val[i] : 0U;
    }
  }
  return 0U;
}

void mmio_write(uc_engine* uc, uint64_t offset, unsigned size, uint64_t value, void* user)
{
  (void)user;
  s_mmio_writes++;
  const uint64_t mmio_abs = (uint64_t)k_periph_base + offset;
  /* Dual-core release: the firmware stages cpu1's vector table in
   * CPU1INITVTOR, then asserts CPU1ACTCSR.ACTREQ to start cpu1. The watcher
   * captures both so the run loop can boot the second engine (see emu_cpu1). */
  emu_cpu1_notify_mmio_write(mmio_abs, value);
  if (mmio_abs == (uint64_t)k_glcdc_bg_bgc) {
    bgc_track((uint32_t)value);
  }
  /* A modelled peripheral block consumes the write first (so GPIO latches,
   * timer control, and ICU event links take real effect); the sparse fallback
   * still records the write for the MMIO table and unmodelled blocks. */
  bool handled = false;
  board_periph_write(uc, (uint64_t)k_periph_base + offset, size, value, &handled);
  if (handled) {
    return;
  }
  const int idx = mmio_index((uint64_t)k_periph_base + offset);
  if (idx >= 0) {
    s_mmio_wcount[idx]++;
    s_mmio_val[idx]     = (uint32_t)value;
    s_mmio_written[idx] = true;
    if (idx == s_mmio_run_slot) {
      s_mmio_run++; /* same-addr read-modify-write spin accumulates toward settle */
    } else {
      s_mmio_run_slot = idx; /* new register -> following reads should see its value */
      s_mmio_run      = 1U;
    }
  }
}

/** @brief Implementation of `emu_mmio_reads()` -- plain counter read. */
uint32_t emu_mmio_reads(void)
{
  return s_mmio_reads;
}

/** @brief Implementation of `emu_mmio_writes()` -- plain counter read. */
uint32_t emu_mmio_writes(void)
{
  return s_mmio_writes;
}

/** @brief Implementation of `emu_mmio_print_counts()` -- run-end report line. */
void emu_mmio_print_counts(void)
{
  (void)fprintf(stderr,
                "  MMIO reads    : %u   writes: %u   distinct addrs: %u\n",
                s_mmio_reads,
                s_mmio_writes,
                s_mmio_n);
}

/** @brief Implementation of `emu_mmio_print_bgc_and_table()` -- run-end report. */
void emu_mmio_print_bgc_and_table(void)
{
  /* GLCDC colour-cycle witness: BG_BGC write count + the distinct colours. */
  (void)fprintf(stderr,
                "  BG_BGC writes : %u   distinct colours: %u   [",
                s_bgc_writes,
                s_bgc_distinct_n);
  for (uint32_t i = 0U; i < s_bgc_distinct_n; i++) {
    (void)fprintf(stderr, "%s0x%06X", (i == 0U) ? "" : " ", s_bgc_distinct[i]);
  }
  (void)fprintf(stderr, "]\n");
  (void)fprintf(stderr, "    %-12s %10s %10s %12s\n", "addr", "reads", "writes", "last-write");
  const bool     truncated = (s_mmio_n > (uint32_t)k_mmio_print_max);
  const uint32_t shown     = truncated ? (uint32_t)k_mmio_print_max : s_mmio_n;
  for (uint32_t i = 0U; i < shown; i++) {
    if (s_mmio_written[i]) {
      (void)fprintf(stderr,
                    "    0x%08llX %10u %10u   0x%08X\n",
                    (unsigned long long)s_mmio_addr[i],
                    s_mmio_rcount[i],
                    s_mmio_wcount[i],
                    s_mmio_val[i]);
    } else {
      (void)fprintf(stderr,
                    "    0x%08llX %10u %10u %12s\n",
                    (unsigned long long)s_mmio_addr[i],
                    s_mmio_rcount[i],
                    s_mmio_wcount[i],
                    "-");
    }
  }
  if (truncated) {
    (void)fprintf(stderr, "    ... (%u more)\n", s_mmio_n - shown);
  }
}
