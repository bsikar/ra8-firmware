/**
 * @file port/usbx/src/ux_dcd_ra8_usb_dvst_default.c
 * @brief USBX device-controller-driver bridge to ra8_usb -- DVST Default-state mirror path.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * The DVSQ=Default branch of the DVST handler: persistent SETUP-mirror
 * capture, the unconditional Default-state SETUP dispatch, and the
 * bus-reset DCP re-arm probes.
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
 * @var g_busreset_rearm_count
 * @brief Number of times ::ra8_usb_device_busreset_rearm was invoked
 *        from the DVST handler in response to a Default-state entry.
 *
 * @details Bisect probe. After plug-in macOS issues a bus reset every
 * ~10 ms until SETUP succeeds. If this counter grows but
 * ::g_setup_packet_count stays at 0, the re-arm is firing but the IP
 * is still failing to latch SETUP -- look at PIPECFG / DCPMAXP via
 * JLink. If the counter never grows, the DVST -> Default branch
 * isn't being taken (check ::priv_dvst_state_history).
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint32_t g_busreset_rearm_count = 0U;

/**
 * @var g_dcpctr_after_rearm
 * @brief Snapshot of ``DCPCTR`` taken at the end of every busreset_rearm.
 *
 * @details Bisect probe for the "VALID never asserts after bus reset"
 * symptom. The rearm intentionally does NOT write DCPCTR (HUM Ch
 * 36.2.21 / 37.2.31 p 1991 / 2093 documents that a USB bus reset
 * auto-defaults PID to NAK and CCPL to 0), so this snapshot reflects
 * whatever the SIE left in DCPCTR after the bus reset and any
 * SETUP-receipt state machine that completed before the rearm fired.
 * Expected value with PID=NAK and CCPL/SUREQ low: 0x0000. A non-zero
 * value means the IP latched something (PBUSY=bit 5, SQMON=bit 6,
 * BSTS=bit 15); SQMON=1 is the strongest "SETUP arrived" signal in
 * device mode (HUM Ch 37.2.31 p 2095, SQMON flag description).
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint16_t g_dcpctr_after_rearm = 0U;

/**
 * @var g_intenb0_after_rearm
 * @brief Snapshot of ``INTENB0`` taken at the end of every busreset_rearm.
 *
 * @details Bisect probe; expected value matches the device-mode mask in
 * ``internal_usb_init_common`` --
 * ``BEMP|BRDY|NRDY|CTRT|DVST|SOFR|RSME|VBSE`` = 0xFD00 (bits 8..15 set
 * except bit 12 -- wait, bit 12 = DVST = set; bits 11..15 = CTRT/DVST/
 * SOFR/RSME/VBSE; bits 8..10 = BRDY/NRDY/BEMP; combined = 0xFF00).
 * If this reads back zero, INTENB0 was clobbered after rearm and the
 * next CTRT edge will not raise an IRQ. HUM Ch 36.2.10 INTENB0 p 1980.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint16_t g_intenb0_after_rearm = 0U;

/**
 * @var g_cfifosel_after_rearm
 * @brief Snapshot of ``CFIFOSEL`` taken at the end of every busreset_rearm.
 *
 * @details Bisect probe. The rearm no longer writes CFIFOSEL (probe
 * data showed the in-IRQ write of 0x0400 read back as 0x0000 on the
 * HS instance, and FSP usb_pstd_bus_reset never touches CFIFOSEL --
 * SETUP latching uses USBREQ/USBVAL/USBINDX/USBLENG, not the CFIFO
 * port). This snapshot now just records whatever value was already
 * programmed by the previous data-stage. HUM Ch 36.2.7 / 37.2.8
 * CFIFOSEL p 1976 / 2071.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint16_t g_cfifosel_after_rearm = 0U;

/**
 * @var g_dcpctr_pre_rearm
 * @brief Snapshot of ``DCPCTR`` taken at the START of every busreset_rearm.
 *
 * @details Captured BEFORE any rearm-side writes so it reflects the
 * state the host's bus reset left the DCP in. Compared against
 * ::g_dcpctr_after_rearm to confirm that the rearm did (or did not)
 * mutate DCP fields.
 *
 * Bit positions of interest (HUM Ch 36.2.21 / 37.2.31 DCPCTR p 1991 /
 * 2093, full bit table on p 2095..2096):
 * - bits [1:0] PID:    00=NAK, 01=BUF, 11=STALL. After bus reset HUM
 *   Ch 37.2.31 (p 2095) guarantees PID=NAK.
 * - bit 5 PBUSY:       1 when SIE has the DCP. Should be 0 by the time
 *   the rearm runs (DVST already fired).
 * - bit 6 SQMON:       sequence-toggle monitor. HUM Ch 37.2.31 p 2095:
 *   "the USBHS sets the SQMON bit to 1 ... on successful reception of
 *   the setup packet." If this reads 1 BEFORE rearm, the chip already
 *   latched a SETUP and INTSTS0.VALID should be 1 too.
 * - bit 14 SUREQ:      host-mode-only; should always read 0 in device.
 * - bit 15 BSTS:       buffer-status flag.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint16_t g_dcpctr_pre_rearm = 0U;

/**
 * @var g_setup_token_observed
 * @brief Counter incremented every time the IRQ snapshot proves the
 *        device-side controller latched a SETUP token from the host.
 *
 * @details Two independent evidence paths fold into this counter:
 *  1. ``INTSTS0.VALID`` (bit 3, HUM Ch 36.2.14 / 37.2.18 p 1985 / 2081)
 *     was set in the snapshot. This is the canonical SETUP-arrived
 *     signal in device mode.
 *  2. ``DCPCTR.SQMON`` (bit 6, HUM Ch 37.2.31 p 2095) was set after
 *     a Default-state DVST. Per the HUM, SQMON transitions 0->1 only
 *     "on successful reception of the setup packet" in device mode,
 *     so a non-zero SQMON observed in ::g_dcpctr_pre_rearm is hard
 *     proof that a SETUP was latched even if the IRQ never saw the
 *     VALID edge (race between the polled-dispatch tick and the
 *     SIE's SETUP-latch state machine).
 *
 * Note: INTSTS1.SACK is HOST-mode only (HUM Ch 37.2.19 SACK flag p
 * 2084: "Values read from the SACK flag in device controller mode
 * are invalid."), so SACK cannot serve as a SETUP-receipt probe.
 *
 * @note Single-writer (::ux_dcd_ra8_usb_irq + ::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint32_t g_setup_token_observed = 0U;

/**
 * @var s_prev_dcpctr_sqmon
 * @brief Last-observed value of ``DCPCTR.SQMON`` (masked to bit 6).
 *
 * @details Retained as a JLink-readable probe; the previous rising-edge
 * gate that consumed this value has been removed because it fired only
 * once per session and starved the dispatcher whenever the very first
 * SQMON observation already had VALID=0. Dispatch is now driven by
 * ``ra8_usb_read_setup_unconditional`` on the HS / SQMON path, which
 * skips the VALID gate entirely (the SIE auto-clears VALID before the
 * polled worker observes the SETUP edge), drains the SETUP-latch
 * mirrors directly, and W0C-clears VALID defensively on the way out.
 * HUM Ch 36.2.14 p 1985, Ch 37.2.31 p 2095.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
static volatile uint16_t s_prev_dcpctr_sqmon = 0U;

/**
 * @var g_dispatch_attempts
 * @brief Count of SQMON-driven dispatch attempts (regardless of outcome).
 *
 * @details Incremented on every Default-state DVST tick, before the
 * SQMON-driven SETUP drain. Pair with ::g_setup_dispatch_count to see
 * how many attempts produced a successful drain.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint32_t g_dispatch_attempts = 0U;

/**
 * @var g_intsts0_at_sqmon_edge
 * @brief Snapshot of INTSTS0 captured immediately before the SQMON-driven
 *        ``ra8_usb_read_setup_unconditional`` call.
 *
 * @details VALID is bit 3 of INTSTS0 (HUM Ch 36.2.14 p 1985). On HS this
 * probe routinely reads back without bit 3 set: the SIE auto-clears
 * VALID before the polled worker observes the SQMON edge. The captured
 * SETUP-latch registers (USBREQ/USBVAL/USBINDX/USBLENG, HUM Ch 37.2.21
 * ..24 p 2087..2090) survive that auto-clear, which is why the HS path
 * uses ``ra8_usb_read_setup_unconditional`` -- it drains the latch
 * directly without gating on VALID.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint16_t g_intsts0_at_sqmon_edge = 0U;

/**
 * @var g_intsts0_observed_or_recent
 * @brief Most-recent INTSTS0 value sampled at the start of the
 *        Default-state tight-poll for VALID (HUM Ch 37.2.18 p 2081,
 *        VALID = bit 3, mask 0x0008).
 *
 * @details Distinct from ::priv_intsts0_observed_or (which OR-accumulates
 * forever and saturates after the first VBSE storm). This probe is
 * over-written every Default-state DVST tick with the live INTSTS0
 * read just before the tight-poll loop, so a JLink reader can answer
 * "what did INTSTS0 look like this iteration?" without losing the bit
 * pattern to the cumulative-OR. HUM Ch 37.2.18 INTSTS0 p 2081.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint16_t g_intsts0_observed_or_recent = 0U;

/**
 * @var g_dcpctr_bit_map_observed
 * @brief Cumulative bitwise-OR of every ``DCPCTR`` value sampled at the
 *        Default-state DVST entry.
 *
 * @details Definitive bisect probe for "which DCPCTR bits has the SIE
 * ever raised?". Per HUM Ch 37.2.32 p 2093 the DCPCTR reset value is
 * ``0x0040`` (bit 6 SQMON defaults to 1 = DATA1-expected); a value
 * other than 0x0040 means the SIE has mutated PID, CCPL, PINGE,
 * PBUSY, SQSET, SQCLR, SUREQCLR, CSSTS, CSCLR, SUREQ, or BSTS at
 * least once since power-on. HUM Ch 37.2.32 p 2095..2096.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint16_t g_dcpctr_bit_map_observed = 0U;

/**
 * @var g_usbreq_first_nonzero
 * @brief First non-zero ``USBREQ`` value captured by the tight-poll
 *        Default-state loop (HUM Ch 37.2.26 p 2090).
 *
 * @details Pure latch: stays 0 until the polled worker reads back a
 * non-zero ``USBREQ`` (bRequest in bits 15..8, bmRequestType in bits
 * 7..0). For a standard ``GET_DESCRIPTOR(DEVICE)`` this latches as
 * ``0x0680`` (bRequest=0x06, bmRequestType=0x80). Stays 0 forever
 * means the SIE has never latched a SETUP transaction into the
 * USBREQ/USBVAL/USBINDX/USBLENG mirrors -- proof the host's SETUP
 * tokens are not reaching the device.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint16_t g_usbreq_first_nonzero = 0U;

/**
 * @var g_unconditional_dispatch_count
 * @brief Count of unconditional ``_ux_device_stack_control_request_process``
 *        invocations made by the Default-state polled worker.
 *
 * @details Incremented every time the worker dispatches a SETUP without
 * gating on ``INTSTS0.VALID`` (HUM Ch 37.2.18 p 2081) or
 * ``DCPCTR.SQMON`` (HUM Ch 37.2.32 p 2093). The dispatch fires whenever
 * the latched ``USBREQ`` (HUM Ch 37.2.26 p 2090) differs from
 * ::g_last_dispatched_usbreq, which prevents infinite re-dispatch of the
 * same SETUP transaction.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint32_t g_unconditional_dispatch_count = 0U;

/**
 * @var g_last_dispatched_usbreq
 * @brief Last ``USBREQ`` (HUM Ch 37.2.26 p 2090) value forwarded to the
 *        chapter-9 dispatcher by the unconditional Default-state worker.
 *
 * @details Used as the de-duplication key: the worker dispatches only
 * when the live ``USBREQ`` differs from this latch, so the same SETUP
 * cannot be re-fired across loop iterations within a single bus-state
 * dwell. Reset to 0 after a fresh dispatch records the new value.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint16_t g_last_dispatched_usbreq = 0U;

/**
 * @enum ra8_usb_dcd_default_poll_t
 * @brief Tight-poll loop sizing for the Default-state VALID hunt.
 *
 * @details The polled worker reaches ``internal_handle_dvst`` once per
 * Default-state DVST tick. Inside that tick we tight-poll INTSTS0 for
 * VALID (HUM Ch 37.2.18 p 2081, mask 0x0008) without yielding so that
 * the SIE's auto-clear of VALID does not race the dispatcher. The
 * iteration count is calibrated for ~2 ms of wall time on the
 * Cortex-M85 @ 1 GHz:
 *
 *   - ``k_ra8_usb_dcd_default_poll_iters``: empirically ~ (2 ms /
 *     16 ns per iter on the M85) but tuned conservatively. The loop
 *     bound also satisfies NASA P10 Rule 2 (statically provable).
 *
 * @note Sized for HS-only; FS dispatch keeps yielding via the worker.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_usb_dcd_default_poll_iters = 200000U, /**< ~2 ms upper bound. */
} ra8_usb_dcd_default_poll_t;

/**
 * @enum ra8_usb_dcpctr_bits_t
 * @brief Selected DCPCTR bit masks (HUM Ch 37.2.31 p 2095).
 */
typedef enum : uint16_t {
  k_ra8_dcpctr_mask_sqmon = (uint16_t)(1U << 6U), /**< SQMON (bit 6): SETUP-latched flag. */
} ra8_usb_dcpctr_bits_t;

/**
 * @brief Capture the persistent SETUP mirror into the JLink-readable probe.
 *
 * @details The HS SIE auto-clears INTSTS0.VALID (HUM Ch 37.2.18 p 2081)
 * and DCPCTR.SQMON (HUM Ch 37.2.32 p 2093) faster than the polled worker
 * can observe them, so flag-gated reads race the controller and miss
 * every SETUP. The four mirrors USBREQ/USBVAL/USBINDX/USBLENG
 * (HUM Ch 37.2.26..29 p 2090..2092) latch the wire-format SETUP and
 * PERSIST across the SIE's auto-clears. If any mirror is non-zero, copy
 * them into ``g_setup_packet_buffer`` in USB 2.0 Ch 9.3 wire byte order.
 *
 * @param[in] usbreq_live USBREQ snapshot.
 * @param[in] usbval_live USBVAL snapshot.
 * @param[in] usbindx_live USBINDX snapshot.
 * @param[in] usbleng_live USBLENG snapshot.
 *
 * @pre Caller is on the ISR callback path in DVSQ=Default.
 * @pre All four arguments hold the live mirror values.
 * @post ``g_setup_packet_buffer`` populated if any mirror is non-zero.
 * @post ``g_setup_packet_count`` incremented in that case.
 *
 * @note ISR-callback context; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_dvst_capture_setup_mirror(uint16_t usbreq_live,
                                                            uint16_t usbval_live,
                                                            uint16_t usbindx_live,
                                                            uint16_t usbleng_live)
{
  const bool any_nonzero = ((usbreq_live | usbval_live | usbindx_live | usbleng_live) != 0U);
  if (!any_nonzero) {
    return;
  }
  g_setup_packet_buffer[k_setup_idx_bmrt] = (uint8_t)(usbreq_live & k_setup_byte_mask);
  g_setup_packet_buffer[k_setup_idx_brq] =
    (uint8_t)((usbreq_live >> k_setup_byte_shift) & k_setup_byte_mask);
  g_setup_packet_buffer[k_setup_idx_val_lo] = (uint8_t)(usbval_live & k_setup_byte_mask);
  g_setup_packet_buffer[k_setup_idx_val_hi] =
    (uint8_t)((usbval_live >> k_setup_byte_shift) & k_setup_byte_mask);
  g_setup_packet_buffer[k_setup_idx_idx_lo] = (uint8_t)(usbindx_live & k_setup_byte_mask);
  g_setup_packet_buffer[k_setup_idx_idx_hi] =
    (uint8_t)((usbindx_live >> k_setup_byte_shift) & k_setup_byte_mask);
  g_setup_packet_buffer[k_setup_idx_len_lo] = (uint8_t)(usbleng_live & k_setup_byte_mask);
  g_setup_packet_buffer[k_setup_idx_len_hi] =
    (uint8_t)((usbleng_live >> k_setup_byte_shift) & k_setup_byte_mask);
  g_setup_packet_count++;
  if (g_usbreq_first_nonzero == 0U) {
    g_usbreq_first_nonzero = usbreq_live;
  }
}

/**
 * @brief Unconditionally dispatch a Default-state SETUP if USBREQ changed.
 *
 * @details Gate ONLY on USBREQ-change so the same SETUP cannot re-fire
 * across DVST entries within one Default dwell. A fresh USBREQ wire
 * value (different from ``g_last_dispatched_usbreq``) is treated as a
 * new SETUP and pushed into chapter-9, even when both VALID and SQMON
 * have already been auto-cleared by the SIE. After dispatch we W0C-ack
 * INTSTS0.VALID and pulse a benign DCPCTR write.
 *
 * @param[in] reg Controller register block for the current speed.
 * @param[in] usbreq_live USBREQ wire value just read.
 * @param[in] usbval_live USBVAL wire value just read.
 * @param[in] usbindx_live USBINDX wire value just read.
 * @param[in] usbleng_live USBLENG wire value just read.
 *
 * @pre Caller is in DVSQ=Default.
 * @pre ``reg`` is non-null.
 * @post ``g_last_dispatched_usbreq`` updated when dispatched.
 * @post INTSTS0.VALID W0C-cleared after dispatch.
 *
 * @note ISR-callback context; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_dvst_dispatch_if_new(volatile r_usb_regs_t* reg,
                                                       uint16_t               usbreq_live,
                                                       uint16_t               usbval_live,
                                                       uint16_t               usbindx_live,
                                                       uint16_t               usbleng_live)
{
  if (usbreq_live == g_last_dispatched_usbreq) {
    /* USBREQ unchanged since last dispatch: do not re-fire. */
    g_dispatch_skip_reason |= (uint32_t)k_ra8_usb_skip_usbreq_unchanged;
    return;
  }
  ra8_usb_setup_t setup = {};
  setup.bm_request_type = (uint8_t)(usbreq_live & k_setup_byte_mask);
  setup.b_request       = (uint8_t)((usbreq_live >> k_setup_byte_shift) & k_setup_byte_mask);
  setup.w_value         = usbval_live;
  setup.w_index         = usbindx_live;
  setup.w_length        = usbleng_live;

  g_setup_token_observed++;
  g_setup_dispatch_count++;
  g_unconditional_dispatch_count++;
  (void)priv_dispatch_setup(&setup);
  g_last_dispatched_usbreq = usbreq_live;

  /* Post-dispatch W0C-ack of INTSTS0.VALID (HUM Ch 37.2.18 p 2081). */
  reg->INTSTS0 = (uint16_t)(reg->INTSTS0 & (uint16_t)~(uint16_t)k_ra8_intsts0_mask_valid);
  /* Pulse DCPCTR write (HUM Ch 37.2.32 p 2093). Benign W0C confirm. */
  reg->DCPCTR = (uint16_t)(reg->DCPCTR & (uint16_t)~(uint16_t)k_ra8_dcpctr_mask_sqmon);
}

/**
 * @brief Process the DVSQ=Default branch of ``internal_handle_dvst``.
 *
 * @details Captures DCPCTR / INTSTS0 probes, drains the persistent
 * SETUP mirrors, dispatches a fresh SETUP if USBREQ changed, then
 * re-arms DCP via ``ra8_usb_device_busreset_rearm`` so the IP can latch
 * the host's next SETUP token (FSP ``usb_pstd_busreset`` parity).
 * HUM Ch 36.2.7 / 36.2.10 / 36.2.21 (FS) and Ch 37 mirrors.
 *
 * @param[in] speed Which controller fired.
 *
 * @pre Caller confirmed ``dvsq == k_ra8_dvsq_default``.
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize``.
 * @post DCP re-armed via ``ra8_usb_device_busreset_rearm``.
 * @post ``g_busreset_rearm_count`` incremented.
 *
 * @note ISR-callback context; must not block.
 * @since 0.1.0
 */
void priv_dvst_default_state(ra8_usb_speed_t speed)
{
  /* HUM Ch 37.2.32 DCPCTR p 2093: capture DCPCTR BEFORE the rearm.
   * DCPCTR.SQMON resets to 1 = DATA1-expected, so "SQMON==1" is NOT
   * a SETUP-arrival signal -- VALID is the race-immune signal. */
  volatile r_usb_regs_t* const reg = (speed == k_ra8_usb_speed_hs) ? ra8_usb_hs() : ra8_usb_fs();
  if (reg != nullptr) {
    const uint16_t dcpctr_pre = reg->DCPCTR;
    g_dcpctr_pre_rearm        = dcpctr_pre;
    g_dcpctr_bit_map_observed = (uint16_t)(g_dcpctr_bit_map_observed | dcpctr_pre);

    /* SQMON probe retained for older bisect notes. */
    const uint16_t now_sqmon = (uint16_t)(dcpctr_pre & (uint16_t)k_ra8_dcpctr_mask_sqmon);

    g_dispatch_attempts++;
    const uint16_t intsts0_pre   = reg->INTSTS0;
    g_intsts0_at_sqmon_edge      = intsts0_pre;
    g_intsts0_observed_or_recent = intsts0_pre;

    /* Drain persistent SETUP mirrors every Default-state tick. */
    const uint16_t usbreq_live  = reg->USBREQ;
    const uint16_t usbval_live  = reg->USBVAL;
    const uint16_t usbindx_live = reg->USBINDX;
    const uint16_t usbleng_live = reg->USBLENG;

    internal_dvst_capture_setup_mirror(usbreq_live, usbval_live, usbindx_live, usbleng_live);
    internal_dvst_dispatch_if_new(reg, usbreq_live, usbval_live, usbindx_live, usbleng_live);
    s_prev_dcpctr_sqmon = now_sqmon;
  }
  (void)ra8_usb_device_busreset_rearm(speed);
  g_busreset_rearm_count++;
  if (reg != nullptr) {
    g_dcpctr_after_rearm   = reg->DCPCTR;
    g_intenb0_after_rearm  = reg->INTENB0;
    g_cfifosel_after_rearm = reg->CFIFOSEL;
  }
}
