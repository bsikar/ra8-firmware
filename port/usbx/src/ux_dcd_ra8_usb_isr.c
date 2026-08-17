/**
 * @file port/usbx/src/ux_dcd_ra8_usb_isr.c
 * @brief USBX device-controller-driver bridge to ra8_usb -- NVIC ISR trampolines.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * USBFS / USBHS NVIC interrupt entry points, the per-bit ISR counters,
 * the spurious-entry storm acknowledgement, and the controller-selection
 * helpers that route the polled / interrupt dispatch back into USBX.
 *
 * Split out of ``ux_dcd_ra8_usb.c`` to keep each translation unit under
 * the maintainability line cap; the cross-translation-unit contract
 * lives in ``ux_dcd_ra8_usb_internal.h``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#define UX_SOURCE_CODE

#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_elc_regs.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_usb_regs.h"
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_dcd_ra8_usb_internal.h"
#include "ux_device_stack.h"
#include "ux_system.h"
#include "ux_utility.h"

/**
 * @var g_isr_invocations
 * @brief Counter of NVIC ISR entries through ::internal_usbfs_isr or
 *        ::internal_usbhs_isr.
 *
 * @details
 * Reaches non-zero on the first USB IRQ delivered through
 * ra8_isr_register / NVIC. Stays at 0 if the IELSR slot was never
 * routed, the NVIC line was masked, or the vector trampoline did not
 * land in the substrate dispatcher (e.g. weak Default_Handler still
 * winning the link). Read via JLink to confirm interrupts fire.
 * HUM Ch 13 NVIC + Ch 14 ICU IELSR.
 *
 * @note Written only by the two ISR trampolines.
 * @since 0.1.0
 */
volatile uint32_t g_isr_invocations = 0U;

/**
 * @var g_isr_intsts0_or
 * @brief Cumulative bitwise-OR of every ``INTSTS0`` value observed at
 *        the entry of ::internal_usbhs_isr.
 *
 * @details
 * Each ISR entry samples ``INTSTS0`` (HUM Ch 36.2.14 p 1985) and folds
 * it into this accumulator with ``|=``. JLink-readable: the set bits
 * tell which event sources have ever fired since boot
 * (BRDY/NRDY/BEMP/CTRT/DVST/SOFR/RSME/VBSE in bits 8..15, plus VALID
 * in bit 3 and the read-only DVSQ/VBSTS status fields). Used to
 * distinguish "ISR runs but no event bits set" (interrupt storm from
 * an un-acked source) from "ISR runs with real events".
 *
 * @note Written only by ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint16_t g_isr_intsts0_or = 0U;

/**
 * @var g_isr_dvst_count
 * @brief Per-bit ISR counter for ``INTSTS0.DVST`` (bit 12) entries.
 *
 * @details
 * Incremented inside ::internal_usbhs_isr whenever the snapshot has
 * the device-state-transition bit set. Distinguishes "ISR fired with
 * DVST" from the bridge-side ::g_dvst_irq_count which counts events
 * after they have already been forwarded to the USBX stack.
 *
 * @note Written only by ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint32_t g_isr_dvst_count = 0U;

/**
 * @var g_isr_ctrt_count
 * @brief Per-bit ISR counter for ``INTSTS0.CTRT`` (bit 11) entries.
 *
 * @details Sibling of ::g_isr_dvst_count for the control-transfer-
 * stage-transition bit (HUM Ch 36.2.14 p 1985).
 *
 * @note Written only by ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint32_t g_isr_ctrt_count = 0U;

/**
 * @var g_isr_valid_count
 * @brief Per-bit ISR counter for ``INTSTS0.VALID`` (bit 3) entries.
 *
 * @details Counts ISR entries where the SETUP-detect flag was already
 * latched at snapshot time. The actual SETUP drain is performed by
 * ::ux_dcd_ra8_usb_irq via ::ra8_usb_dispatch -> ::priv_event_cb.
 *
 * @note Written only by ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint32_t g_isr_valid_count = 0U;

/**
 * @var g_isr_brdy_count
 * @brief Per-bit ISR counter for ``INTSTS0.BRDY`` (bit 8) entries.
 *
 * @note Written only by ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint32_t g_isr_brdy_count = 0U;

/**
 * @var g_isr_bemp_count
 * @brief Per-bit ISR counter for ``INTSTS0.BEMP`` (bit 10) entries.
 *
 * @note Written only by ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint32_t g_isr_bemp_count = 0U;

/**
 * @var g_isr_nrdy_count
 * @brief Per-bit ISR counter for ``INTSTS0.NRDY`` (bit 9) entries.
 *
 * @details Storm-localisation probe: a value in the millions after a
 * failed MSC scan means a pipe is NAK'ing host tokens and the NRDY
 * status is re-asserting INTSTS0.NRDY faster than the dispatcher
 * clears it. Written by ::internal_usbfs_isr and ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint32_t g_isr_nrdy_count = 0U;

/**
 * @var g_isr_sofr_count
 * @brief Per-bit ISR counter for ``INTSTS0.SOFR`` (bit 13) entries.
 *
 * @details Storm-localisation probe. SOFR fires once per USB frame
 * (1 kHz on full-speed); a count far above ``1000 * uptime_s`` means
 * the ISR is re-entering on a bit other than SOFR while SOFR happens
 * to be co-asserted. Written by ::internal_usbfs_isr and ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint32_t g_isr_sofr_count = 0U;

volatile uint32_t g_dcd_irq_spurious_mask_count = 0U;

/* -------------------------------------------------------------------------- */
/* USBFS interrupt-storm guard */
/*                                                                            */
/* The USBFS controller re-asserts its NVIC line for RSME / SOFR / status- */
/* only conditions independent of INTENB0. While the host hammers a NAK'ing */
/* pipe (e.g. the MSC bulk-OUT before the storage class thread has armed the */
/* CBW receive) these event-less entries re-fire ~1e5/s and -- via Cortex-M */
/* exception tail-chaining -- consume 100% CPU, so RTOS thread mode never runs */
/* and the USBX class thread is permanently starved (GitHub issue #6). */
/*                                                                            */
/* internal_usbfs_isr counts consecutive event-less entries; once that run */
/* crosses k_ra8_usb_storm_mask_run a real storm is in progress, and */
/* priv_usbfs_irq_mask() disables the USB IRQ at the NVIC -- handing the */
/* CPU to thread mode. Recovery is the per-app 1 ms SysTick handler: it calls */
/* ux_dcd_ra8_usb_irq_reenable(), which zeroes the run counter and re-enables */
/* the line. The run counter is thus a per-millisecond rate gauge -- a genuine */
/* storm (~1e3 event-less entries/ms) trips it well within a tick; normal idle */
/* SOFR (~1/ms) never does, so the guard is behaviour-neutral for the working */
/* CDC / HID apps. SysTick is the recovery clock (not a ThreadX TX_TIMER) */
/* because it is an exception handler -- it keeps running even while the storm */
/* has thread mode, and the ThreadX timer subsystem, starved. */
/* -------------------------------------------------------------------------- */

/**
 * @enum ra8_usb_storm_cfg_t
 * @brief Tuning constant for the USBFS interrupt-storm guard.
 */
typedef enum : uint32_t {
  k_ra8_usb_storm_mask_run = 8U, /**< Consecutive event-less FS ISR entries that
                                     trip the NVIC mask. The 1 ms SysTick
                                     handler zeroes the run, so this is a
                                     per-ms rate gauge: the bench storm runs
                                     ~40 event-less entries/ms and trips this
                                     in ~0.2 ms; normal SOFR is ~1/ms and
                                     never reaches it. A spurious trip is
                                     benign -- SysTick re-enables within 1 ms,
                                     so a real event is delayed at most 1 ms. */
} ra8_usb_storm_cfg_t;

/* -------------------------------------------------------------------------- */
/* IRQ glue */
/* -------------------------------------------------------------------------- */

/**
 * @brief Bump the per-bit ISR counters for an INTSTS0 snapshot.
 *
 * @details Each ``s_isr_*_count`` counter is a JLink-readable probe so
 * a bench reader can correlate ::g_isr_intsts0_or with which event bits
 * fired this tick. Shared by the FS and HS ISRs to keep each outer ISR
 * within the NASA P10 Rule 4 size cap.
 *
 * @param[in] intsts0 Snapshot of INTSTS0 read at the top of the ISR.
 *
 * @pre Snapshot has already been masked against ``event_msk``.
 * @pre Caller is in NVIC handler mode.
 * @post Per-bit counters bumped for every observed event bit.
 * @post No INTSTS0 W0C write performed.
 *
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_isr_bump_counts(uint16_t intsts0)
{
  if ((intsts0 & (uint16_t)(1U << k_ra8_int0_bit_dvst)) != 0U) {
    g_isr_dvst_count++;
  }
  if ((intsts0 & (uint16_t)(1U << k_ra8_int0_bit_ctrt)) != 0U) {
    g_isr_ctrt_count++;
  }
  if ((intsts0 & (uint16_t)k_ra8_intsts0_mask_valid) != 0U) {
    g_isr_valid_count++;
  }
  if ((intsts0 & (uint16_t)(1U << k_ra8_int0_bit_brdy)) != 0U) {
    g_isr_brdy_count++;
  }
  if ((intsts0 & (uint16_t)(1U << k_ra8_int0_bit_bemp)) != 0U) {
    g_isr_bemp_count++;
  }
  if ((intsts0 & (uint16_t)(1U << k_ra8_int0_bit_nrdy)) != 0U) {
    g_isr_nrdy_count++;
  }
  if ((intsts0 & (uint16_t)(1U << k_ra8_int0_bit_sofr)) != 0U) {
    g_isr_sofr_count++;
  }
}

/**
 * @brief NVIC ISR entry point for the USBFS controller.
 *
 * @details
 * Registered with ``ra8_isr_register(k_ra8_elc_event_usbfs_int, ...)``
 * during ``ux_dcd_ra8_usb_initialize``. Snapshots ``INTSTS0``, classifies
 * the entry, then forwards it to ``ra8_usb_dispatch`` -- the dispatch is
 * unconditional, identical to the pre-storm-guard behaviour, so this ISR
 * is behaviour-neutral for the working CDC / HID apps.
 *
 * The only added behaviour is the storm guard. An entry with no real
 * event bit (only RSME / SOFR / status bits -- these assert the NVIC
 * line independent of INTENB0) increments ::g_isr_spurious_run; a real
 * event resets it. ::ux_dcd_ra8_usb_irq_reenable (called from the per-app
 * 1 ms ``SysTick_Handler``) zeroes the counter, so it gauges the
 * event-less *rate*. Once it crosses ::k_ra8_usb_storm_mask_run within a
 * millisecond a genuine storm is in progress, and ::priv_usbfs_irq_mask
 * disables the USB IRQ so RTOS thread mode -- the otherwise-starved USBX
 * class thread (GitHub issue #6) -- gets the CPU back. The SysTick handler
 * re-enables the line within 1 ms. Normal idle SOFR (~1/ms) never trips
 * the guard.
 *
 * @param[in] ctx Unused; kept to match ``ra8_isr_handler_t``.
 *
 * @pre Bridge is in ``k_ux_dcd_ra8_usb_state_ready`` or ``_active``.
 * @pre ``ra8_usb_attach_handler`` has been called (done in the same init).
 *
 * @post ``g_isr_invocations`` is incremented and ``ra8_usb_dispatch`` ran.
 * @post On a sustained event-less run the USB IRQ is masked at the NVIC.
 *
 * @note Runs in NVIC handler mode; must not block.
 *
 * @see ra8_usb_dispatch
 * @see priv_usbfs_irq_mask
 * @see internal_usbhs_isr
 * @see ux_dcd_ra8_usb_irq
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usbfs_isr(void* ctx)
{
  (void)ctx;
  g_isr_invocations++;

  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1986.
   * event_msk is the real-event set: INTENB0 (BRDY/NRDY/BEMP/CTRT/
   * DVST/VBSE) plus VALID. RSME / SOFR / status bits are excluded --
   * they re-assert the NVIC line on their own and are what storms. */
  const uint16_t intsts0   = ra8_usb_intsts0_snapshot(k_ra8_usb_speed_fs);
  const uint16_t event_msk = (uint16_t)((1U << k_ra8_int0_bit_brdy) | (1U << k_ra8_int0_bit_nrdy) |
                                        (1U << k_ra8_int0_bit_bemp) | (1U << k_ra8_int0_bit_ctrt) |
                                        (1U << k_ra8_int0_bit_dvst) | (1U << k_ra8_int0_bit_vbse) |
                                        (uint16_t)k_ra8_intsts0_mask_valid);

  g_isr_intsts0_or |= intsts0;

  if ((intsts0 & event_msk) == 0U) {
    g_isr_spurious_run++;
    if (g_isr_spurious_run >= (uint32_t)k_ra8_usb_storm_mask_run) {
      priv_usbfs_irq_mask();
      g_dcd_irq_spurious_mask_count++;
    }
  } else {
    g_isr_spurious_run = 0U;
  }

  internal_isr_bump_counts(intsts0);
  ra8_usb_dispatch(k_ra8_usb_speed_fs);
}

/**
 * @brief W0C-ack RSME/SOFR on a spurious USB ISR re-entry.
 *
 * @details The RA8 USB controller asserts its NVIC line for RSME
 * (resume detect) and SOFR (start-of-frame) independent of INTENB0 --
 * on USBHS via the PHY's USBR signal, and on USBFS the same applies
 * (confirmed on the bench: SOFR/RSME entries occur with INTENB0.SOFR
 * and INTENB0.RSME both clear). While the host hammers a NAK'ing
 * pipe the FS resume detector re-asserts RSME continuously, which --
 * without this short-circuit -- drives the full dispatch + pipe-walk
 * ~10^5 times/second and starves RTOS thread mode. When the snapshot
 * has no real event bit we W0C-clear the stuck RSME/SOFR flags, bump
 * the spurious counter, and return cheaply.
 *
 * @param[in] speed   Which controller the spurious entry came from.
 * @param[in] intsts0 The just-snapshotted INTSTS0 value.
 *
 * @pre Caller already determined ``(intsts0 & event_msk) == 0``.
 * @pre USB module for ``speed`` is initialized (``ra8_usb_device_init`` ran).
 * @post ``g_dcd_irq_spurious_mask_count`` is incremented.
 * @post RSME/SOFR bits that were set are cleared via ::ra8_usb_clear_status.
 *
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_ack_spurious(ra8_usb_speed_t speed, uint16_t intsts0)
{
  const uint16_t stuck_mask = (uint16_t)((1U << k_ra8_int0_bit_rsme) | (1U << k_ra8_int0_bit_sofr));
  if ((intsts0 & stuck_mask) != 0U) {
    (void)ra8_usb_clear_status(speed, (uint16_t)(intsts0 & stuck_mask));
  }
  /* The HS controller latches bus-change/attach/detach in INTSTS1 even
   * with INTENB1 fully masked; a held latch keeps the shared NVIC line
   * asserted with INTSTS0 clean (observed live: 440k spurious ISR
   * entries/s with INTSTS1.BCHG stuck). W0C-ack those bits here. */
  if (speed == k_ra8_usb_speed_hs) {
    volatile r_usb_regs_t* const reg = ra8_usb_hs();
    if (reg != nullptr) {
      const uint16_t int1_mask =
        (uint16_t)((1U << k_ra8_int1_bit_bchg) | (1U << k_ra8_int1_bit_dtch) |
                   (1U << k_ra8_int1_bit_attch));
      reg->INTSTS1 = (uint16_t)~int1_mask;
    }
  }
  g_dcd_irq_spurious_mask_count++;
}

/**
 * @brief NVIC ISR entry point for the USBHS controller.
 *
 * @details Snapshot ``INTSTS0`` and gate the rest of the ISR on the
 * event bits (8..15 plus the VALID flag). Spurious / USBR-driven
 * re-entry is short-circuited via ::internal_ack_spurious.
 * Real events are forwarded to ::ra8_usb_dispatch, which performs the
 * canonical W0C ack and invokes the USBX-side handler.
 *
 * @param[in] ctx Opaque ISR context (unused; kept for the ICU callback ABI).
 *
 * @pre USBHS module is past ``ra8_usb_device_init``.
 * @pre Caller is the NVIC USBHS_INT vector.
 *
 * @post ``g_isr_invocations`` is incremented.
 * @post On a real event ``ra8_usb_dispatch`` ran; on a spurious one
 *       RSME/SOFR were W0C-cleared and ``g_dcd_irq_spurious_mask_count``
 *       is incremented.
 *
 * @note ISR-only; must not block. Not re-entrant within a single
 *       controller.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usbhs_isr(void* ctx)
{
  (void)ctx;
  g_isr_invocations++;

  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1986.
   *
   * Snapshot INTSTS0 first and gate on event bits (8..15 + VALID in
   * bit 3). DVSQ[6:4] and VBSTS[7] are status-only and must NEVER be
   * treated as events; a snapshot of 0x0090 (DVSQ=Default + VBSTS=1)
   * means "no event pending, just status-bits asserted" and the ISR
   * must return without touching INTSTS0 to avoid an interrupt storm.
   * The event_msk below matches the bits enabled in INTENB0; RSME /
   * SOFR are intentionally excluded because the USBR resume-detect
   * signal can re-assert the NVIC line independent of INTENB0. */
  const uint16_t intsts0   = ra8_usb_intsts0_snapshot(k_ra8_usb_speed_hs);
  const uint16_t event_msk = (uint16_t)((1U << k_ra8_int0_bit_brdy) | (1U << k_ra8_int0_bit_nrdy) |
                                        (1U << k_ra8_int0_bit_bemp) | (1U << k_ra8_int0_bit_ctrt) |
                                        (1U << k_ra8_int0_bit_dvst) | (1U << k_ra8_int0_bit_vbse) |
                                        (uint16_t)k_ra8_intsts0_mask_valid);

  g_isr_intsts0_or |= intsts0;

  if ((intsts0 & event_msk) == 0U) {
    internal_ack_spurious(k_ra8_usb_speed_hs, intsts0);
    /* Same storm guard as the FS ISR: a sustained run of event-less
     * entries means a level condition the acks above cannot clear is
     * holding the NVIC line. Mask it; the per-app 1 ms SysTick handler
     * re-enables via ux_dcd_ra8_usb_irq_reenable, so real USB events
     * resume within one tick. Without this the HS line stormed at
     * ~440k entries/s and starved thread mode completely (the USBX
     * storage thread never got a single timeslice). */
    g_isr_spurious_run++;
    if (g_isr_spurious_run >= (uint32_t)k_ra8_usb_storm_mask_run) {
      priv_usbfs_irq_mask();
    }
    return;
  }
  g_isr_spurious_run = 0U;

  internal_isr_bump_counts(intsts0);

  /* ::ra8_usb_dispatch performs the W0C ack with the
   * ``INTSTS0 = ~(snapshot & ack_bits)`` pattern (HUM Ch 37.2.18 Note
   * 3 p 2082): writes 0 only to event bits that were observed set, 1
   * to all other bits (no-op under W0C). VALID is intentionally NOT
   * acked here -- the SETUP drain in ::ux_dcd_ra8_usb_irq /
   * ::ra8_usb_read_setup_unconditional clears VALID after copying
   * USBREQ/USBVAL/USBINDX/USBLENG. */
  ra8_usb_dispatch(k_ra8_usb_speed_hs);
}

/**
 * @brief Pick the ELC event number for a controller.
 *
 * @param[in] speed Which controller (FS or HS).
 * @return ``ra8_elc_event_t`` event number for that controller.
 *
 * @pre ``speed`` is ``k_ra8_usb_speed_fs`` or ``k_ra8_usb_speed_hs``.
 * @post No state mutated.
 *
 * @note Pure function.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
ra8_elc_event_t priv_pick_event(ra8_usb_speed_t speed)
{
  return (speed == k_ra8_usb_speed_hs) ? k_ra8_elc_event_usbhs_int_resume
                                       : k_ra8_elc_event_usbfs_int;
}

/**
 * @brief Pick the ISR trampoline for a controller.
 *
 * @param[in] speed Which controller (FS or HS).
 * @return Function pointer to the trampoline.
 *
 * @pre ``speed`` is ``k_ra8_usb_speed_fs`` or ``k_ra8_usb_speed_hs``.
 * @post No state mutated.
 *
 * @note Pure function.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
ra8_isr_handler_t priv_pick_isr(ra8_usb_speed_t speed)
{
  return (speed == k_ra8_usb_speed_hs) ? internal_usbhs_isr : internal_usbfs_isr;
}

/**
 * @brief ra8_usb_attach_handler trampoline.
 *
 * @details See implementation for details.
 * @param[in,out] ctx See function signature.
 * @param[in,out] speed See function signature.
 * @param[in,out] status_mask See function signature.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void priv_event_cb(void* ctx, ra8_usb_speed_t speed, uint16_t status_mask)
{
  (void)ctx;
  ux_dcd_ra8_usb_irq(speed, status_mask);
}
