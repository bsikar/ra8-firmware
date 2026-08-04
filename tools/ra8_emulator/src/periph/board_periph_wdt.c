/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_periph_wdt.c
 * @brief Window Watchdog Timer (WDT0 / WWDT) model for ra8_emulator
 *
 * @details
 * Models the RA8D2 WDT0 (ra8_wdt_regs.h, ra8_wdt.c) at @c 0x40202600 closely
 * enough that a watchdog reset actually fires when the firmware stops refreshing
 * the counter, instead of the timeout never happening. Against the sparse
 * fallback @c wdt_reset_recovery_demo armed the WWDT for reset and then stopped
 * refreshing, but the chip never reset, so the post-reset boot (and its
 * @c "wdt: reset_by=watchdog" banner) was unreachable. This block runs the
 * down-counter and, on underflow in reset mode, asks the run loop to warm-reboot
 * the firmware with the watchdog-reset cause latched, so the next boot reads
 * @c k_ra8_reset_cause_wdt0.
 *
 * Behaviour (HUM Ch 27.2):
 *  - @c WDTRR (0x00) is the refresh register: the @c 0x00 then @c 0xFF unlock
 *    sequence both ARMS the counter (the first refresh starts it) and RELOADS it
 *    to full. A stray write that breaks the sequence is ignored here (the model
 *    does not synthesise the refresh-error path).
 *  - @c WDTCR (0x02) selects the timeout period (TOPS); shadowed so a read back
 *    works. The board emulator has no real watchdog clock, so the timeout maps
 *    to a fixed number of emulation ticks (one tick == one run-loop chunk):
 *    large enough that a normally-refreshing app stays alive, small enough that
 *    a deliberate stop trips within the run budget.
 *  - @c WDTSR (0x04) reports the live down-counter in CNTVAL and the UNDFF
 *    underflow flag.
 *  - @c WDTRCR (0x06) RSTIRQS selects reset (1) vs NMI / IRQ (0) on underflow.
 *
 * The per-chunk @c tick hook decrements the counter once it is armed; when it
 * reaches zero with RSTIRQS set, it raises the reboot request and disarms (so it
 * fires exactly once). With RSTIRQS clear (NMI / IRQ mode), the underflow latches
 * UNDFF but does not reboot -- the board emulator does not route the WDT NMI.
 *
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph.h"
#include "board_periph_block.h"

/** @brief WDT0 block geometry (ra8_wdt_regs.h, r_wdt_regs_t). */
typedef enum : uint64_t {
  k_wdt_base       = 0x40202600UL, /**< WDT0 base (HUM Ch 27).       */
  k_wdt_span       = 0x10UL,       /**< Covers WDTRR..WDTRCR.        */
  k_wdt_off_wdtrr  = 0x00UL,       /**< WDTRR refresh register (8b). */
  k_wdt_off_wdtcr  = 0x02UL,       /**< WDTCR control (16b).         */
  k_wdt_off_wdtsr  = 0x04UL,       /**< WDTSR status (16b).          */
  k_wdt_off_wdtrcr = 0x06UL,       /**< WDTRCR reset control (8b).   */
} wdt_geom_t;

/** @brief WDT register field constants (ra8_wdt_regs.h). */
typedef enum : uint32_t {
  k_wdt_refresh_a   = 0x00U,   /**< First byte of the refresh sequence.     */
  k_wdt_refresh_b   = 0xFFU,   /**< Second byte of the refresh sequence.    */
  k_wdt_rcr_rstirqs = 0x80U,   /**< WDTRCR.RSTIRQS: 1 = reset on underflow. */
  k_wdt_sr_cntval   = 0x3FFFU, /**< WDTSR.CNTVAL[13:0] live counter.        */
  k_wdt_sr_undff    = 0x4000U, /**< WDTSR.UNDFF underflow flag (bit 14).    */
  /* Emulation timeout in run-loop chunks. The demo refreshes about every 50
   * chunks; this is comfortably longer (so a refreshing app never trips) yet
   * well within the run budget once refreshing stops. */
  k_wdt_timeout_ticks = 400U, /**< Wdt timeout ticks. */
} wdt_field_t;

/** @brief Per-tick order slot for the WDT block (relative order only). */
typedef enum : uint32_t {
  k_wdt_block_order = 180U, /**< After the reset block; report order. */
} wdt_order_t;

/** @brief WDT model state. */
typedef struct {
  uint16_t wdtcr;     /**< WDTCR shadow (timeout / window select).   */
  uint8_t  wdtrcr;    /**< WDTRCR shadow (RSTIRQS).                  */
  uint8_t  last_rr;   /**< Last WDTRR byte (for the 0x00->0xFF seq). */
  bool     armed;     /**< Counter is running (first refresh seen).  */
  bool     fired;     /**< Reset already requested (one-shot).       */
  uint32_t counter;   /**< Down-counter, in ticks.                   */
  uint16_t undff;     /**< UNDFF status latch.                       */
  uint32_t refreshes; /**< Refreshes serviced (for the report).      */
} wdt_state_t;

static wdt_state_t s_wdt;

/** @brief Per-chunk advance: run the armed down-counter; reset on underflow. */
static void wdt_tick(uc_engine* uc)
{
  (void)uc;
  if (!s_wdt.armed || s_wdt.fired) {
    return;
  }
  if (s_wdt.counter > 0U) {
    s_wdt.counter--;
    return;
  }
  /* Underflow. Latch UNDFF; if configured for reset, request a warm reboot with
   * the watchdog-reset cause and disarm so it fires exactly once. */
  s_wdt.undff = (uint16_t)k_wdt_sr_undff;
  if ((s_wdt.wdtrcr & (uint8_t)k_wdt_rcr_rstirqs) != 0U) {
    s_wdt.fired = true;
    s_wdt.armed = false;
    board_periph_reset_request_reboot(true, false);
  }
}

/** @brief Reset the WDT model to power-on state. */
static void wdt_reset(void)
{
  s_wdt = (wdt_state_t){};
}

/** @brief MMIO read inside the WDT window. */
static uint64_t wdt_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_wdt_base;
  if (off == (uint64_t)k_wdt_off_wdtcr) {
    return s_wdt.wdtcr;
  }
  if (off == (uint64_t)k_wdt_off_wdtsr) {
    const uint16_t cnt = (uint16_t)(s_wdt.counter & (uint32_t)k_wdt_sr_cntval);
    return (uint64_t)(cnt | s_wdt.undff);
  }
  if (off == (uint64_t)k_wdt_off_wdtrcr) {
    return s_wdt.wdtrcr;
  }
  return 0U;
}

/** @brief MMIO write inside the WDT window. */
static void wdt_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_wdt_base;
  if (off == (uint64_t)k_wdt_off_wdtrr) {
    const uint8_t b = (uint8_t)value;
    /* The 0x00 then 0xFF sequence refreshes: arm (first time) and reload. */
    if ((s_wdt.last_rr == (uint8_t)k_wdt_refresh_a) && (b == (uint8_t)k_wdt_refresh_b)) {
      s_wdt.armed   = true;
      s_wdt.counter = (uint32_t)k_wdt_timeout_ticks;
      s_wdt.undff   = 0U;
      s_wdt.refreshes++;
    }
    s_wdt.last_rr = b;
  } else if (off == (uint64_t)k_wdt_off_wdtcr) {
    s_wdt.wdtcr = (uint16_t)value;
  } else if (off == (uint64_t)k_wdt_off_wdtrcr) {
    s_wdt.wdtrcr = (uint8_t)value;
  }
}

/** @brief End-of-run WDT section: refreshes and whether it fired a reset. */
static void wdt_report(void)
{
  if ((s_wdt.refreshes == 0U) && !s_wdt.fired) {
    return; /* Untouched: stay quiet. */
  }
  (void)fprintf(stderr,
                "  WDT0          : refreshes=%u armed=%u reset_fired=%u\n",
                s_wdt.refreshes,
                (unsigned)s_wdt.armed,
                (unsigned)s_wdt.fired);
}

/** @brief WDT0 block descriptor (self-registered with the core). */
static const board_periph_block_t k_wdt_block = {
  .base   = (uint64_t)k_wdt_base,
  .span   = (uint64_t)k_wdt_span,
  .order  = (uint32_t)k_wdt_block_order,
  .read   = wdt_read,
  .write  = wdt_write,
  .tick   = wdt_tick,
  .reset  = wdt_reset,
  .report = wdt_report,
  .name   = "WDT0",
};

/** @brief Register the WDT0 block before main (host constructor). */
[[gnu::constructor]] static void wdt_block_register(void)
{
  board_periph_register_block(&k_wdt_block);
}
