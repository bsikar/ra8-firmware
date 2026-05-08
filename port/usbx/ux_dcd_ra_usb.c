/**
 * @file port/usbx/ux_dcd_ra_usb.c
 * @brief USBX device-controller-driver bridge to ra_usb -- implementation.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * Implements the dispatch contract documented in ``ux_dcd_ra_usb.h``.
 * Mirrors the layout of upstream USBX DCD ports (e.g.
 * ``ux_dcd_sim_slave_function``) but routes every call through the
 * project's ``ra_usb_*`` register-level driver instead of touching
 * USB controller registers directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#define UX_SOURCE_CODE

#include "ux_dcd_ra_usb.h"

#include <stdint.h>
#include <string.h>

#include "ra8d2_elc_regs.h"
#include "ra8d2_usb_regs.h"
#include "ra_check.h"
#include "ra_isr.h"
#include "ra_log.h"
#include "tx_api.h"
#include "ux_api.h"
#include "ux_device_stack.h"
#include "ux_system.h"
#include "ux_utility.h"

/**
 * @enum ra_usb_dcd_isr_prio_t
 * @brief NVIC priority chosen for the USB controller IRQs.
 *
 * @details
 * USB completion IRQs sit between SysTick (priority 0, the highest in
 * this firmware) and the application-level work threads. Picking 4
 * leaves headroom for higher-priority drivers (timers, fault paths)
 * while still pre-empting ThreadX context switches and USBX class
 * threads so SETUP / BRDY / BEMP events drain promptly.
 */
typedef enum : uint8_t {
  k_ra_usb_dcd_isr_prio = 4U, /**< NVIC priority used for both USBFS and USBHS lines. */
} ra_usb_dcd_isr_prio_t;

/* Tag used by ra_log_*. Must be a static lifetime string. */
static const char* const s_tag = "ux_dcd_ra_usb";

/**
 * @var s_syscfg_after_dcd_init
 * @brief SYSCFG snapshot at end of ux_dcd_ra_usb_initialize.
 *
 * @details Bisect probe for the "USBE clears between phy bring-up and
 * echo loop" regression. Read via JLink to confirm whether the DCD
 * bridge or anything it calls (e.g. _ux_dcd_ra_usb_function
 * CREATE_ENDPOINT, _ux_utility_descriptor_parse, the dispatch worker
 * spawn) clobbers SYSCFG. HUM Ch 37.2.1 SYSCFG p 2060.
 *
 * @note Read-only from outside; written only by ::ux_dcd_ra_usb_initialize.
 * @since 0.1.0
 */
volatile uint16_t s_syscfg_after_dcd_init = 0U;

/**
 * @var s_lpsts_after_dcd_init
 * @brief LPSTS snapshot at end of ux_dcd_ra_usb_initialize.
 *
 * @details Companion bisect probe; expected SUSPENDM=1 (0x4000). HUM
 * Ch 37.2.43 LPSTS p 2111.
 *
 * @note Read-only from outside; written only by ::ux_dcd_ra_usb_initialize.
 * @since 0.1.0
 */
volatile uint16_t s_lpsts_after_dcd_init = 0U;

/**
 * @var s_isr_invocations
 * @brief Counter of NVIC ISR entries through ::internal_usbfs_isr or
 *        ::internal_usbhs_isr.
 *
 * @details
 * Reaches non-zero on the first USB IRQ delivered through
 * ra_isr_register / NVIC. Stays at 0 if the IELSR slot was never
 * routed, the NVIC line was masked, or the vector trampoline did not
 * land in the substrate dispatcher (e.g. weak Default_Handler still
 * winning the link). Read via JLink to confirm interrupts fire.
 * HUM Ch 13 NVIC + Ch 14 ICU IELSR.
 *
 * @note Written only by the two ISR trampolines.
 * @since 0.1.0
 */
volatile uint32_t s_isr_invocations = 0U;

/**
 * @var s_isr_intsts0_or
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
volatile uint16_t s_isr_intsts0_or = 0U;

/**
 * @var s_isr_dvst_count
 * @brief Per-bit ISR counter for ``INTSTS0.DVST`` (bit 12) entries.
 *
 * @details
 * Incremented inside ::internal_usbhs_isr whenever the snapshot has
 * the device-state-transition bit set. Distinguishes "ISR fired with
 * DVST" from the bridge-side ::s_dvst_irq_count which counts events
 * after they have already been forwarded to the USBX stack.
 *
 * @note Written only by ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint32_t s_isr_dvst_count = 0U;

/**
 * @var s_isr_ctrt_count
 * @brief Per-bit ISR counter for ``INTSTS0.CTRT`` (bit 11) entries.
 *
 * @details Sibling of ::s_isr_dvst_count for the control-transfer-
 * stage-transition bit (HUM Ch 36.2.14 p 1985).
 *
 * @note Written only by ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint32_t s_isr_ctrt_count = 0U;

/**
 * @var s_isr_valid_count
 * @brief Per-bit ISR counter for ``INTSTS0.VALID`` (bit 3) entries.
 *
 * @details Counts ISR entries where the SETUP-detect flag was already
 * latched at snapshot time. The actual SETUP drain is performed by
 * ::ux_dcd_ra_usb_irq via ::ra_usb_dispatch -> ::internal_event_cb.
 *
 * @note Written only by ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint32_t s_isr_valid_count = 0U;

/**
 * @var s_isr_brdy_count
 * @brief Per-bit ISR counter for ``INTSTS0.BRDY`` (bit 8) entries.
 *
 * @note Written only by ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint32_t s_isr_brdy_count = 0U;

/**
 * @var s_isr_bemp_count
 * @brief Per-bit ISR counter for ``INTSTS0.BEMP`` (bit 10) entries.
 *
 * @note Written only by ::internal_usbhs_isr.
 * @since 0.1.0
 */
volatile uint32_t s_isr_bemp_count = 0U;

/**
 * @var s_ctrt_irq_count
 * @brief Counter of INTSTS0.CTRT events seen by the dispatcher.
 *
 * @details Bisect probe. Non-zero after host SETUP means the chip is
 * reporting control-stage transitions; zero means SETUPs aren't being
 * latched (HS chirp / termination problem). HUM Ch 36.2.14 p 1985.
 *
 * @note Written only by ::ux_dcd_ra_usb_irq.
 * @since 0.1.0
 */
volatile uint32_t s_ctrt_irq_count = 0U;

/**
 * @var s_dvst_irq_count
 * @brief Counter of INTSTS0.DVST (device-state-transition) events.
 *
 * @details Bisect probe. Each bus reset / set-address / set-config
 * raises DVST; zero after attach means the host isn't reaching the
 * device. HUM Ch 36.2.14 p 1985.
 *
 * @note Written only by ::ux_dcd_ra_usb_irq.
 * @since 0.1.0
 */
volatile uint32_t s_dvst_irq_count = 0U;

/**
 * @var s_setup_dispatch_count
 * @brief Counter of SETUP packets fed into the chapter-9 dispatcher.
 *
 * @details Bisect probe. Non-zero confirms a SETUP-drain entry point
 * (ra_usb_read_setup_if_valid or _unconditional) emptied
 * USBREQ/USBVAL/USBINDX/USBLENG and ::internal_dispatch_setup fired.
 * Stays zero if VALID never asserts (no SETUP token reached the IP).
 * HUM Ch 36.2.16..36.2.19 p 1623..1626.
 *
 * @note Written only by ::internal_handle_ctrt and the VALID fallback
 *       in ::ux_dcd_ra_usb_irq.
 * @since 0.1.0
 */
volatile uint32_t s_setup_dispatch_count = 0U;

/**
 * @var s_dvstctr0_at_first_dvst
 * @brief Snapshot of DVSTCTR0 (RHST field) on first DVST event.
 *
 * @details Bisect probe. RHST[2:0] (HUM Ch 36.2.5 p 1971) reports the
 * negotiated bus speed. 1=LS, 2=FS, 3=HS, 4=reset. If the HS chirp
 * handshake succeeded this should latch 3 (HS); 2 means the chip is
 * stuck in FS termination after bus reset.
 *
 * @note Latched once on the first DVST after attach; never overwritten.
 * @since 0.1.0
 */
volatile uint16_t s_dvstctr0_at_first_dvst = 0xFFFFU;

/**
 * @enum ra_usb_dcd_rhst_hist_t
 * @brief Sizing for the DVSTCTR0.RHST history ring.
 */
typedef enum : uint8_t {
  k_ra_usb_dcd_rhst_hist_n = 16U, /**< Slots in s_rhst_history.       */
} ra_usb_dcd_rhst_hist_t;

/**
 * @var s_rhst_history
 * @brief Per-DVST-event capture of DVSTCTR0.RHST[2:0]. JLink-readable.
 *
 * @details RHST encoding (HUM Ch 36.2.5 p 1971 / Ch 37 DVSTCTR0):
 *   0=undefined / chirp limbo, 1=LS, 2=FS, 3=HS, 4=in-reset.
 * On a successful HS attach the array typically reads 0,4,3,...; on
 * FS-fallback 0,4,2,...; on chirp failure (the 0.2.0 USB-HS bring-up
 * symptom) every slot reads 0 even after dozens of host resets.
 *
 * @note Single-writer (::internal_handle_dvst from the polled worker).
 * @since 0.1.0
 */
volatile uint8_t s_rhst_history[(uint32_t)k_ra_usb_dcd_rhst_hist_n] = {};

/**
 * @var s_rhst_history_count
 * @brief Total DVST events seen; modulo k_ra_usb_dcd_rhst_hist_n is
 *        the next write slot.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint32_t s_rhst_history_count = 0U;

/**
 * @var s_dvstctr0_history
 * @brief Per-dispatch-tick capture of the full DVSTCTR0 register.
 *
 * @details Bisect probe for the post-chirp HS bring-up stall. After a
 * successful HS chirp the controller settles ``DVSTCTR0.RHST = 011``
 * (HUM Ch 37.2.5 DVSTCTR0 / Ch 36.2.5 mirror, p 1971). The hypothesis
 * under test is whether ``DVSTCTR0.HSPROC`` (bit 2 of the RHST 3-bit
 * encoding -- value 0b100 = 4 -- per FSP ``r_usb_bitdefine.h``
 * ``USB_HSPROC = 0x0004``) auto-clears or wedges, and whether any
 * other DVSTCTR0 bit (UACT bit 6, RESUME bit 5, USBRST bit 6 in host
 * mirror, WKUP bit 8, RWUPE bit 9, USBRESM bit 10, HNPBTOA bit 11,
 * EXICEN bit 12, VBUSEN bit 13) flips between the chirp completion
 * and the host's first SETUP/SOF token. INTSTS1.BCHG-only events
 * with no SACK/CTRT mean the host stopped after chirp; capturing the
 * full DVSTCTR0 word per tick lets the bench confirm the field is
 * stable at 0x???3 (RHST=HS) and not silently flapping.
 *
 * Ring size 16 chosen to match ::s_rhst_history so JLink scripts can
 * read both arrays in one transaction.
 *
 * @note Single-writer (::internal_dispatch_worker tick).
 * @since 0.1.0
 */
volatile uint16_t s_dvstctr0_history[(uint32_t)k_ra_usb_dcd_rhst_hist_n] = {};

/**
 * @var s_dvstctr0_history_count
 * @brief Total dispatch ticks observed; modulo
 *        ::k_ra_usb_dcd_rhst_hist_n is the next write slot in
 *        ::s_dvstctr0_history.
 *
 * @note Single-writer (::internal_dispatch_worker tick).
 * @since 0.1.0
 */
volatile uint32_t s_dvstctr0_history_count = 0U;

/**
 * @var s_intsts1_history
 * @brief Per-dispatch-tick capture of the full INTSTS1 register.
 *
 * @details Companion probe to ::s_dvstctr0_history. INTSTS1
 * (HUM Ch 36.2.17 p 2001) carries BCHG (bit 14), DTCH (bit 12),
 * ATTCH (bit 11), EOFERR (bit 6), SIGN (bit 5), SACK (bit 4) for
 * host mode and the bus-change shadow for device mode. The 0.2.0
 * bring-up symptom is BCHG-only (bit 14) with no SACK ever; this
 * ring lets the bench confirm whether any SACK / EOFERR / SIGN edge
 * arrives in the dispatch window after RHST settles.
 *
 * @note Single-writer (::internal_dispatch_worker tick).
 * @since 0.1.0
 */
volatile uint16_t s_intsts1_history[(uint32_t)k_ra_usb_dcd_rhst_hist_n] = {};

/**
 * @var s_intsts0_last_dispatch
 * @brief Most-recent INTSTS0 snapshot fed to ::ux_dcd_ra_usb_irq.
 *
 * @details Live-debug probe. Read this from JLink to see the current
 * pending interrupt mask + DVSQ/CTSQ field state without halting the
 * polled worker. HUM Ch 36.2.14 p 1985.
 *
 * @note Written only by ::ux_dcd_ra_usb_irq.
 * @since 0.1.0
 */
volatile uint16_t s_intsts0_last_dispatch = 0U;

/**
 * @enum ra_usb_dcd_intsts0_hist_t
 * @brief Sizing for the INTSTS0 / CTSQ history rings.
 */
typedef enum : uint8_t {
  k_ra_usb_dcd_intsts0_hist_n = 8U, /**< Slots in s_intsts0_snapshot/s_ctsq_history. */
} ra_usb_dcd_intsts0_hist_t;

/**
 * @enum ra_usb_dcd_intsts0_field_shift_t
 * @brief Bit-position shifts used to extract INTSTS0 multi-bit fields.
 *
 * @details HUM Ch 36.2.16 INTSTS0 p 1986: DVSQ occupies bits 6:4.
 */
typedef enum : uint8_t {
  k_ra_int0_dvsq_shift = 4U, /**< DVSQ[2:0] field starts at bit 4. */
} ra_usb_dcd_intsts0_field_shift_t;

/**
 * @var s_intsts0_valid_count
 * @brief Number of dispatch ticks where INTSTS0.VALID was observed.
 *
 * @details Bisect probe. Distinguishes "controller never latched a
 * SETUP" (count == 0) from "controller latched but our CTRT edge was
 * lost" (count > 0 while ::s_ctrt_irq_count grows). HUM Ch 36.2.14
 * INTSTS0.VALID = bit 3, p 1985.
 *
 * @note Single-writer (::ux_dcd_ra_usb_irq).
 * @since 0.1.0
 */
volatile uint32_t s_intsts0_valid_count = 0U;

/**
 * @var s_intsts0_ctrt_count
 * @brief Number of dispatch ticks where INTSTS0.CTRT was observed.
 *
 * @details Bisect probe. Identical to ::s_ctrt_irq_count but bumped on
 * the snapshot value before the CTRT branch decides whether to handle
 * the SETUP -- useful for detecting CTRT latches that hit a snapshot
 * but failed the handler's CTSQ decode. HUM Ch 36.2.14 p 1985.
 *
 * @note Single-writer (::ux_dcd_ra_usb_irq).
 * @since 0.1.0
 */
volatile uint32_t s_intsts0_ctrt_count = 0U;

/**
 * @var s_intsts0_snapshot
 * @brief Ring buffer of the last 8 INTSTS0 values seen by the worker
 *        on ticks where any of CTRT/VALID/DVST were set.
 *
 * @details JLink-readable trace. Index of next write slot is
 * ``s_intsts0_snapshot_count % k_ra_usb_dcd_intsts0_hist_n``. Only
 * "interesting" ticks are recorded so a noisy idle loop doesn't
 * scroll a real SETUP edge out of the ring. HUM Ch 36.2.14 p 1985.
 *
 * @note Single-writer (::ux_dcd_ra_usb_irq).
 * @since 0.1.0
 */
volatile uint16_t s_intsts0_snapshot[(uint32_t)k_ra_usb_dcd_intsts0_hist_n] = {};

/**
 * @var s_intsts0_snapshot_count
 * @brief Total interesting INTSTS0 snapshots seen; modulo
 *        ::k_ra_usb_dcd_intsts0_hist_n is the next write slot.
 *
 * @note Single-writer (::ux_dcd_ra_usb_irq).
 * @since 0.1.0
 */
volatile uint32_t s_intsts0_snapshot_count = 0U;

/**
 * @var s_ctsq_history
 * @brief Ring buffer of the last 8 INTSTS0.CTSQ[2:0] values observed
 *        with CTRT or VALID asserted.
 *
 * @details Encoding (HUM Ch 36.2.14 p 1985 / ra_usb_ctsq_t):
 *   0=idle, 1=rdds, 2=rdss, 3=wrds, 4=wrss, 5=wrnd, 6=sqer.
 *
 * @note Single-writer (::ux_dcd_ra_usb_irq).
 * @since 0.1.0
 */
volatile uint8_t s_ctsq_history[(uint32_t)k_ra_usb_dcd_intsts0_hist_n] = {};

/**
 * @var s_intsts0_observed_or
 * @brief Bitwise OR of every INTSTS0 value ever fed to ::ux_dcd_ra_usb_irq.
 *
 * @details Definitive bisect probe. Read this once via JLink and check
 * bit 3 (mask 0x0008 = VALID) -- if it's clear, the controller has
 * NEVER latched a SETUP token since power-on. Distinct from the
 * snapshot ring (which can scroll edges out of view) because OR-
 * accumulating bits is monotonic. HUM Ch 36.2.16 INTSTS0 p 1986
 * (VALID = bit 3, CTRT = bit 11, DVST = bit 12).
 *
 * @note Single-writer (::ux_dcd_ra_usb_irq).
 * @since 0.1.0
 */
volatile uint16_t s_intsts0_observed_or = 0U;

/**
 * @var s_dvsq_history
 * @brief Per-snapshot DVSQ[2:0] field, pre-decoded for JLink readers.
 *
 * @details Companion to ::s_intsts0_snapshot. Slot ``i`` holds
 * ``(intsts0[i] >> 4) & 0x07``, i.e. 0=Powered, 1=Default, 2=Address,
 * 3=Configured, 4..7=Suspend (per HUM Ch 36.2.16 p 1986).
 * Saves the human reader from mentally decoding the raw bits.
 *
 * @note Single-writer (::ux_dcd_ra_usb_irq).
 * @since 0.1.0
 */
volatile uint8_t s_dvsq_history[(uint32_t)k_ra_usb_dcd_intsts0_hist_n] = {};

/**
 * @var s_setup_packet_buffer
 * @brief Wire-format bytes of the most recent SETUP packet drained from
 *        the controller, ready for JLink inspection.
 *
 * @details Layout matches USB 2.0 Ch 9.3:
 *   [0]=bmRequestType, [1]=bRequest, [2..3]=wValue (LE),
 *   [4..5]=wIndex (LE), [6..7]=wLength (LE).
 * A standard GET_DESCRIPTOR(DEVICE) shows as
 *   80 06 00 01 00 00 40 00.
 * Stays all-zero until the first VALID/CTRT edge drains a SETUP.
 * HUM Ch 36.2.17..36.2.20 (USBREQ/USBVAL/USBINDX/USBLENG).
 *
 * @note Single-writer (::internal_dispatch_setup).
 * @since 0.1.0
 */
volatile uint8_t s_setup_packet_buffer[8] = {};

/**
 * @var s_setup_packet_count
 * @brief Total SETUP packets latched into ::s_setup_packet_buffer.
 *
 * @note Single-writer (::internal_dispatch_setup).
 * @since 0.1.0
 */
volatile uint32_t s_setup_packet_count = 0U;

/**
 * @var s_dvst_state_history
 * @brief Per-DVST-event capture of the decoded DVSQ[2:0] field.
 *
 * @details JLink-readable trace of every device-state transition
 * (independent of the "interesting tick" filter that gates
 * ::s_dvsq_history). Each DVST IRQ writes the pre-shifted DVSQ value
 * (0=Powered, 1=Default, 2=Address, 3=Configured, 4..7=Suspend variant
 * per HUM Ch 36.2.16 p 1986). A healthy enumeration shows
 * 1, 1, 2, 2, 3, ...; the "stuck-in-default" symptom shows
 * 1, 5, 1, 5, 1, 5, ... (Default <-> Suspended-from-Default loop).
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint8_t s_dvst_state_history[(uint32_t)k_ra_usb_dcd_rhst_hist_n] = {};

/**
 * @var s_dvst_state_history_count
 * @brief Total DVST events recorded into ::s_dvst_state_history.
 *
 * @details Modulo ::k_ra_usb_dcd_rhst_hist_n is the next write slot.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint32_t s_dvst_state_history_count = 0U;

/**
 * @var s_busreset_rearm_count
 * @brief Number of times ::ra_usb_device_busreset_rearm was invoked
 *        from the DVST handler in response to a Default-state entry.
 *
 * @details Bisect probe. After plug-in macOS issues a bus reset every
 * ~10 ms until SETUP succeeds. If this counter grows but
 * ::s_setup_packet_count stays at 0, the re-arm is firing but the IP
 * is still failing to latch SETUP -- look at PIPECFG / DCPMAXP via
 * JLink. If the counter never grows, the DVST -> Default branch
 * isn't being taken (check ::s_dvst_state_history).
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint32_t s_busreset_rearm_count = 0U;

/**
 * @var s_dcpctr_after_rearm
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
volatile uint16_t s_dcpctr_after_rearm = 0U;

/**
 * @var s_intenb0_after_rearm
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
volatile uint16_t s_intenb0_after_rearm = 0U;

/**
 * @var s_cfifosel_after_rearm
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
volatile uint16_t s_cfifosel_after_rearm = 0U;

/**
 * @var s_dcpctr_pre_rearm
 * @brief Snapshot of ``DCPCTR`` taken at the START of every busreset_rearm.
 *
 * @details Captured BEFORE any rearm-side writes so it reflects the
 * state the host's bus reset left the DCP in. Compared against
 * ::s_dcpctr_after_rearm to confirm that the rearm did (or did not)
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
volatile uint16_t s_dcpctr_pre_rearm = 0U;

/**
 * @var s_setup_token_observed
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
 *     so a non-zero SQMON observed in ::s_dcpctr_pre_rearm is hard
 *     proof that a SETUP was latched even if the IRQ never saw the
 *     VALID edge (race between the polled-dispatch tick and the
 *     SIE's SETUP-latch state machine).
 *
 * Note: INTSTS1.SACK is HOST-mode only (HUM Ch 37.2.19 SACK flag p
 * 2084: "Values read from the SACK flag in device controller mode
 * are invalid."), so SACK cannot serve as a SETUP-receipt probe.
 *
 * @note Single-writer (::ux_dcd_ra_usb_irq + ::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint32_t s_setup_token_observed = 0U;

/**
 * @var s_prev_dcpctr_sqmon
 * @brief Last-observed value of ``DCPCTR.SQMON`` (masked to bit 6).
 *
 * @details Retained as a JLink-readable probe; the previous rising-edge
 * gate that consumed this value has been removed because it fired only
 * once per session and starved the dispatcher whenever the very first
 * SQMON observation already had VALID=0. Dispatch is now driven by
 * ``ra_usb_read_setup_unconditional`` on the HS / SQMON path, which
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
 * @var s_dispatch_skip_reason
 * @brief Last-iteration bitmask of why the SQMON dispatch path skipped.
 *
 * @details Bits:
 *  - 0x01 prev_sqmon_already_set (legacy probe, no longer gates)
 *  - 0x02 ra_usb_read_setup_if_valid_returned_no_data (legacy CTRT only)
 *  - 0x04 ra_usb_read_setup_unconditional_returned_err
 *  - 0x08 internal_dispatch_setup returned non-zero (USBX rejected)
 *  - 0x10 _ux_system_slave was UX_NULL when dispatch ran
 *  - 0x20 ux_slave_endpoint_transfer_request unreachable
 *  - 0x40 sqmon was zero on this DVST entry
 *  - 0x80 dispatch path actually ran to completion (success marker)
 *
 * @note Single-writer (::internal_handle_dvst, ::internal_dispatch_setup).
 * @since 0.1.0
 */
volatile uint32_t s_dispatch_skip_reason = 0U;

/**
 * @var s_dispatch_attempts
 * @brief Count of SQMON-driven dispatch attempts (regardless of outcome).
 *
 * @details Incremented on every Default-state DVST tick, before the
 * SQMON-driven SETUP drain. Pair with ::s_setup_dispatch_count to see
 * how many attempts produced a successful drain.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint32_t s_dispatch_attempts = 0U;

/**
 * @var s_intsts0_at_sqmon_edge
 * @brief Snapshot of INTSTS0 captured immediately before the SQMON-driven
 *        ``ra_usb_read_setup_unconditional`` call.
 *
 * @details VALID is bit 3 of INTSTS0 (HUM Ch 36.2.14 p 1985). On HS this
 * probe routinely reads back without bit 3 set: the SIE auto-clears
 * VALID before the polled worker observes the SQMON edge. The captured
 * SETUP-latch registers (USBREQ/USBVAL/USBINDX/USBLENG, HUM Ch 37.2.21
 * ..24 p 2087..2090) survive that auto-clear, which is why the HS path
 * uses ``ra_usb_read_setup_unconditional`` -- it drains the latch
 * directly without gating on VALID.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint16_t s_intsts0_at_sqmon_edge = 0U;

/**
 * @var s_intsts0_observed_or_recent
 * @brief Most-recent INTSTS0 value sampled at the start of the
 *        Default-state tight-poll for VALID (HUM Ch 37.2.18 p 2081,
 *        VALID = bit 3, mask 0x0008).
 *
 * @details Distinct from ::s_intsts0_observed_or (which OR-accumulates
 * forever and saturates after the first VBSE storm). This probe is
 * over-written every Default-state DVST tick with the live INTSTS0
 * read just before the tight-poll loop, so a JLink reader can answer
 * "what did INTSTS0 look like this iteration?" without losing the bit
 * pattern to the cumulative-OR. HUM Ch 37.2.18 INTSTS0 p 2081.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint16_t s_intsts0_observed_or_recent = 0U;

/**
 * @var s_dcpctr_bit_map_observed
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
volatile uint16_t s_dcpctr_bit_map_observed = 0U;

/**
 * @var s_usbreq_first_nonzero
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
volatile uint16_t s_usbreq_first_nonzero = 0U;

/**
 * @var s_unconditional_dispatch_count
 * @brief Count of unconditional ``_ux_device_stack_control_request_process``
 *        invocations made by the Default-state polled worker.
 *
 * @details Incremented every time the worker dispatches a SETUP without
 * gating on ``INTSTS0.VALID`` (HUM Ch 37.2.18 p 2081) or
 * ``DCPCTR.SQMON`` (HUM Ch 37.2.32 p 2093). The dispatch fires whenever
 * the latched ``USBREQ`` (HUM Ch 37.2.26 p 2090) differs from
 * ::s_last_dispatched_usbreq, which prevents infinite re-dispatch of the
 * same SETUP transaction.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint32_t s_unconditional_dispatch_count = 0U;

/**
 * @var s_last_dispatched_usbreq
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
volatile uint16_t s_last_dispatched_usbreq = 0U;

/**
 * @var s_last_dispatched_setup_fp
 * @brief 64-bit fingerprint of the last SETUP packet dispatched from
 *        the ISR-driven SETUP drain in ::internal_handle_ctrt.
 *
 * @details Layout (little-endian-style bit packing):
 *  - bits  0..15: USBREQ (bmRequestType + bRequest)
 *  - bits 16..31: USBVAL  (wValue)
 *  - bits 32..47: USBINDX (wIndex)
 *  - bits 48..63: USBLENG (wLength)
 *
 * Used to de-dup VALID-driven SETUP drains so a single SETUP that
 * fires multiple ISR snapshots is not re-dispatched. A fresh
 * fingerprint (different from this latch) is treated as a new SETUP.
 *
 * @note Single-writer (::internal_handle_ctrt).
 * @since 0.1.0
 */
volatile uint64_t s_last_dispatched_setup_fp = 0U;

/**
 * @var s_dispatched_fp_ring
 * @brief Ring of the last 4 dispatched SETUP fingerprints (oldest at
 *        index 0). Used to disambiguate which 2 SETUPs the chip
 *        processed when xfer_req_total < setup_dispatch_count.
 *
 * @note Written only by ::internal_handle_ctrt.
 * @since 0.1.0
 */
volatile uint64_t s_dispatched_fp_ring[4] = {};
volatile uint8_t  s_dispatched_fp_ring_idx = 0U;

/**
 * @var s_state_at_dispatch
 * @brief Snapshot of ux_slave_device_state at the moment of the most
 *        recent SETUP dispatch. Used to verify the state-mirror gate
 *        was satisfied (must be in {ATTACHED, ADDRESSED, CONFIGURED}).
 *
 * @note Single-writer (::internal_handle_ctrt).
 * @since 0.1.0
 */
volatile uint8_t s_state_at_dispatch = 0U;

/**
 * @enum ra_usb_setup_local_t
 * @brief Local USB SETUP-packet bit-field constants.
 * @details bmRequestType direction bit (bit 7) per USB 2.0 sec 9.3.
 */
typedef enum : uint8_t {
  /* bmRequestType bit 7 -- USB 2.0 spec sec 9.3 Table 9-2:
   * 0 = Host-to-Device (write or no-data), 1 = Device-to-Host (read). */
  k_ra_usb_setup_dir_mask  = 0x80U,
  /* SET_ADDRESS standard request code. USB 2.0 spec sec 9.4.6
   * Table 9-4 ("Standard Request Codes"): bRequest = 5 = SET_ADDRESS.
   * Used to gate out the manual CCPL pulse for SET_ADDRESS, which
   * the Renesas USBHS SIE auto-handles per HUM Ch 37.3 (auto
   * response function, p 2147). */
  k_ra_usb_breq_set_address = 0x05U,
} ra_usb_setup_local_t;

/**
 * @enum ra_usb_dcd_default_poll_t
 * @brief Tight-poll loop sizing for the Default-state VALID hunt.
 *
 * @details The polled worker reaches ``internal_handle_dvst`` once per
 * Default-state DVST tick. Inside that tick we tight-poll INTSTS0 for
 * VALID (HUM Ch 37.2.18 p 2081, mask 0x0008) without yielding so that
 * the SIE's auto-clear of VALID does not race the dispatcher. The
 * iteration count is calibrated for ~2 ms of wall time on the
 * Cortex-M85 @ 1 GHz:
 *
 *   - ``k_ra_usb_dcd_default_poll_iters``: empirically ~ (2 ms /
 *     16 ns per iter on the M85) but tuned conservatively. The loop
 *     bound also satisfies NASA P10 Rule 2 (statically provable).
 *
 * @note Sized for HS-only; FS dispatch keeps yielding via the worker.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_usb_dcd_default_poll_iters = 200000U, /**< ~2 ms upper bound.  */
} ra_usb_dcd_default_poll_t;

/**
 * @struct ra_usb_dcd_pipe_slot_t
 * @brief Per-pipe class-layer cache so the IRQ handler can re-arm
 *        BRDY-driven transfers without crawling the device endpoint
 *        list.
 */
typedef struct {
  struct UX_SLAVE_TRANSFER_STRUCT* xfer;    /**< Active transfer or NULL.       */
  uint8_t                          ep_addr; /**< USB EP number (with dir bit).  */
  uint8_t                          dir_in;  /**< 1 if IN pipe, 0 if OUT.        */
  uint16_t                         max_pkt; /**< Endpoint wMaxPacketSize.       */
} ra_usb_dcd_pipe_slot_t;

/**
 * @struct ra_usb_dcd_t
 * @brief Bridge-singleton state.
 */
typedef struct {
  ra_usb_dcd_state_t          state;                            /**< Bridge run-state.        */
  ra_usb_speed_t              speed;                            /**< Controller this drives.  */
  struct UX_SLAVE_DCD_STRUCT* owner;                            /**< Back-pointer into USBX.  */
  ra_usb_dcd_pipe_slot_t      pipes[k_ux_dcd_ra_usb_max_pipes]; /**< DCP + PIPE1..9.          */
} ra_usb_dcd_t;

/**
 * @var s_dcd
 * @brief The single bridge instance. RA8D2 has two USB controllers
 * but the device stack only ever drives one at a time, so a single
 * static is sufficient.
 *
 * @note Not thread-safe -- updated from the ISR and the dispatch
 * trampoline; concurrency must be arbitrated at the call-site.
 */
static ra_usb_dcd_t s_dcd = {
  .state = k_ux_dcd_ra_usb_state_uninit,
  .speed = k_ra_usb_speed_fs,
  .owner = nullptr,
  .pipes = {},
};

/**
 * @struct ra_usb_dcd_diag_t
 * @brief Externally-readable diagnostic counters for OUT-pipe stall debug.
 * @details Read these via JLink to localise where bulk-OUT data flow stops.
 *          Layout in declaration order, 4 bytes each. Address resolved
 *          via `arm-none-eabi-nm | grep s_diag`. Increment-only; never
 *          cleared at runtime so a JLink dump after a single host write
 *          tells the full story.
 */
typedef struct {
  volatile uint32_t xfer_req_total;          /**< +0x00 internal_transfer_request entered. */
  volatile uint32_t xfer_req_null_arg;       /**< +0x04 tr or ep was NULL.                 */
  volatile uint32_t xfer_req_bad_pipe;       /**< +0x08 pipe index >= max.                 */
  volatile uint32_t xfer_req_pipe2_in;       /**< +0x0C pipe == 2.                         */
  volatile uint32_t xfer_req_pipe2_out_dir;  /**< +0x10 pipe == 2 + OUT direction.         */
  volatile uint32_t xfer_req_pipe2_stashed;  /**< +0x14 pipe2 OUT slot populated.          */
  volatile uint32_t xfer_req_pipe2_block;    /**< +0x18 about to tx_semaphore_get.         */
  volatile uint32_t xfer_req_pipe2_woken;    /**< +0x1C tx_semaphore_get returned.         */
  volatile uint32_t irq_walk_total;          /**< +0x20 IRQ pipe-walk loop iterations.     */
  volatile uint32_t irq_walk_pipe2_seen;     /**< +0x24 pipe2 walked with non-NULL xfer.   */
  volatile uint32_t irq_walk_pipe2_complete; /**< +0x28 queue_out drained pipe2.           */
  volatile uint32_t irq_walk_pipe2_no_data;  /**< +0x2C queue_out returned no_data.        */
} ra_usb_dcd_diag_t;

/**
 * @var s_diag
 * @brief Bridge diagnostic counter block. Read via JLink memory.
 * @note Single-writer per counter; safe under the bridge's single-
 *       worker-thread + single-class-thread model.
 * @since 0.1.0
 */
static ra_usb_dcd_diag_t s_diag = {};

/**
 * @enum ra_setup_byte_idx_t
 * @brief Wire-format byte indices into the USBX SETUP buffer.
 *
 * @details
 * Mirrors the USB 2.0 Ch 9.3 layout, identical to USBX's own
 * ``UX_SETUP_REQUEST_TYPE`` .. ``UX_SETUP_LENGTH`` constants but
 * expressed as a typed enum to satisfy the project's no-magic-numbers
 * rule and to keep the SETUP-pack code readable.
 */
typedef enum : uint8_t {
  k_setup_idx_bmrt   = 0U, /**< bmRequestType (offset 0). */
  k_setup_idx_brq    = 1U, /**< bRequest      (offset 1). */
  k_setup_idx_val_lo = 2U, /**< wValue  low byte  (offset 2). */
  k_setup_idx_val_hi = 3U, /**< wValue  high byte (offset 3). */
  k_setup_idx_idx_lo = 4U, /**< wIndex  low byte  (offset 4). */
  k_setup_idx_idx_hi = 5U, /**< wIndex  high byte (offset 5). */
  k_setup_idx_len_lo = 6U, /**< wLength low byte  (offset 6). */
  k_setup_idx_len_hi = 7U, /**< wLength high byte (offset 7). */
} ra_setup_byte_idx_t;

/**
 * @enum ra_setup_byte_pack_t
 * @brief Bit-shift / mask constants for splitting a uint16_t SETUP
 *        field into its little-endian byte pair.
 */
typedef enum : uint16_t {
  k_setup_byte_shift = 8U,    /**< Bits per byte for the hi-byte extraction. */
  k_setup_byte_mask  = 0xFFU, /**< Low-byte mask after the shift. */
} ra_setup_byte_pack_t;

/**
 * @enum ra_usb_dcpctr_bits_t
 * @brief Selected DCPCTR bit masks (HUM Ch 37.2.31 p 2095).
 */
typedef enum : uint16_t {
  k_ra_dcpctr_mask_sqmon = (uint16_t)(1U << 6U), /**< SQMON (bit 6): SETUP-latched flag. */
} ra_usb_dcpctr_bits_t;

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Map a USB EP number (1..9) into our PIPE table index.
 *
 * @param[in] ep_addr Endpoint address (with dir bit in 0x80).
 *
 * @return Pipe index 0..9, or k_ux_dcd_ra_usb_max_pipes on overflow.
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint8_t internal_ep_to_pipe(uint8_t ep_addr)
{
  const uint8_t ep = ep_addr & (uint8_t)0x0FU;
  if (ep == 0U) {
    return 0U;
  }
  if (ep < (uint8_t)k_ux_dcd_ra_usb_max_pipes) {
    return ep;
  }
  return (uint8_t)k_ux_dcd_ra_usb_max_pipes;
}

/**
 * @brief Dispatch an OUT or IN bulk/interrupt transfer to ra_usb.
 *
 * @param[in,out] tr USBX transfer request.
 *
 * @return UX_SUCCESS on enqueue, UX_TRANSFER_ERROR on rejection.
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static unsigned int internal_transfer_request(struct UX_SLAVE_TRANSFER_STRUCT* tr)
{
  s_diag.xfer_req_total++;
  if (tr == nullptr) {
    s_diag.xfer_req_null_arg++;
    return UX_TRANSFER_ERROR;
  }
  if (tr->ux_slave_transfer_request_endpoint == nullptr) {
    s_diag.xfer_req_null_arg++;
    return UX_TRANSFER_ERROR;
  }

  UX_SLAVE_ENDPOINT* ep      = tr->ux_slave_transfer_request_endpoint;
  const uint8_t      ep_addr = (uint8_t)ep->ux_slave_endpoint_descriptor.bEndpointAddress;
  const uint8_t      pipe    = internal_ep_to_pipe(ep_addr);
  if (pipe >= (uint8_t)k_ux_dcd_ra_usb_max_pipes) {
    s_diag.xfer_req_bad_pipe++;
    return UX_TRANSFER_ERROR;
  }
  if (pipe == 2U) {
    s_diag.xfer_req_pipe2_in++;
    if ((ep_addr & 0x80U) == 0U) {
      s_diag.xfer_req_pipe2_out_dir++;
    }
  }

  /* DCP / EP0: split control IN data stage from the no-data status path.
   * For a control transfer with payload (e.g. GET_DESCRIPTOR) we must
   * push the bytes via ra_usb_dcp_in_data (which raises PID=BUF without
   * pulsing CCPL); CCPL is asserted later on the CTSQ status-stage edge
   * by the bridge's internal_handle_ctrt path. For zero-length control
   * (e.g. SET_ADDRESS, SET_CONFIGURATION) ra_usb_control_response(true)
   * sets PID=BUF and pulses CCPL, completing the status stage. */
  if (pipe == 0U) {
    if (tr->ux_slave_transfer_request_in_transfer_length != 0U &&
        tr->ux_slave_transfer_request_data_pointer != nullptr) {
      const uint16_t len = (uint16_t)tr->ux_slave_transfer_request_in_transfer_length;
      if (ra_usb_dcp_in_data(s_dcd.speed, tr->ux_slave_transfer_request_data_pointer, len) !=
          k_ra_ok) {
        return UX_TRANSFER_ERROR;
      }
      tr->ux_slave_transfer_request_actual_length = len;
      return UX_SUCCESS;
    }
    if (ra_usb_control_response(s_dcd.speed, true) != k_ra_ok) {
      return UX_TRANSFER_ERROR;
    }
    return UX_SUCCESS;
  }

  /* Stash the active transfer so the IRQ path can post completion. */
  s_dcd.pipes[pipe].xfer    = tr;
  s_dcd.pipes[pipe].ep_addr = ep_addr;
  s_dcd.pipes[pipe].dir_in  = (uint8_t)((ep_addr & 0x80U) != 0U ? 1U : 0U);
  s_dcd.pipes[pipe].max_pkt = (uint16_t)ep->ux_slave_endpoint_descriptor.wMaxPacketSize;
  if (pipe == 2U) {
    s_diag.xfer_req_pipe2_stashed++;
  }

  if ((ep_addr & 0x80U) != 0U) {
    const uint16_t len = (uint16_t)tr->ux_slave_transfer_request_requested_length;
    if (ra_usb_queue_in(s_dcd.speed, pipe, tr->ux_slave_transfer_request_data_pointer, len) !=
        k_ra_ok) {
      s_dcd.pipes[pipe].xfer = nullptr;
      return UX_TRANSFER_ERROR;
    }
    tr->ux_slave_transfer_request_actual_length = len;
  }

  /* Synchronous bridge contract (matches ux_dcd_sim_slave_transfer_request):
   * for non-EP0 transfers the DCD must block on
   * ux_slave_transfer_request_semaphore until the IRQ path posts
   * completion (sets completion_code and puts the semaphore). Returning
   * UX_SUCCESS immediately would let USBX class drivers observe
   * actual_length=0 and treat it as a short-packet completion --
   * loopback consumers then busy-spin on (n == 0). */
#ifndef UX_DEVICE_STANDALONE
  ULONG timeout = tr->ux_slave_transfer_request_timeout;
  if (timeout == 0U) {
    timeout = TX_WAIT_FOREVER;
  }
  if (pipe == 2U) {
    s_diag.xfer_req_pipe2_block++;
  }
  UINT sem_status = tx_semaphore_get(&tr->ux_slave_transfer_request_semaphore, timeout);
  if (pipe == 2U) {
    s_diag.xfer_req_pipe2_woken++;
  }
  if (sem_status != TX_SUCCESS) {
    /* Timeout / aborted: drop the pipe slot so a stale stash cannot
     * cause the IRQ path to complete a now-defunct request. */
    s_dcd.pipes[pipe].xfer                        = nullptr;
    tr->ux_slave_transfer_request_completion_code = UX_TRANSFER_ERROR;
    return UX_TRANSFER_ERROR;
  }
  return tr->ux_slave_transfer_request_completion_code;
#else
  return UX_SUCCESS;
#endif
}

/**
 * @brief Translate a USBX endpoint create request into ra_usb call.
 *
 * @details See implementation for details.
 * @param[in,out] ep See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static unsigned int internal_endpoint_create(struct UX_SLAVE_ENDPOINT_STRUCT* ep)
{
  if (ep == nullptr) {
    return UX_ERROR;
  }
  const uint8_t ep_addr = (uint8_t)ep->ux_slave_endpoint_descriptor.bEndpointAddress;
  const uint8_t pipe    = internal_ep_to_pipe(ep_addr);
  if (pipe == 0U || pipe >= (uint8_t)k_ux_dcd_ra_usb_max_pipes) {
    /* DCP is configured by ra_usb_device_init; class layer should
     * not call CREATE_ENDPOINT for EP0. */
    return UX_SUCCESS;
  }

  ra_usb_ep_dir_t  dir = ((ep_addr & 0x80U) != 0U) ? k_ra_usb_ep_dir_in : k_ra_usb_ep_dir_out;
  ra_usb_ep_type_t type;
  switch ((uint8_t)ep->ux_slave_endpoint_descriptor.bmAttributes & 0x03U) {
    case 0x02U:
      type = k_ra_usb_ep_type_bulk;
      break;
    case 0x03U:
      type = k_ra_usb_ep_type_intr;
      break;
    case 0x01U:
      type = k_ra_usb_ep_type_iso;
      break;
    default:
      return UX_ERROR;
  }

  if (ra_usb_configure_endpoint(s_dcd.speed,
                                pipe,
                                (uint8_t)(ep_addr & 0x0FU),
                                dir,
                                type,
                                (uint16_t)ep->ux_slave_endpoint_descriptor.wMaxPacketSize) !=
      k_ra_ok) {
    return UX_ERROR;
  }
  s_dcd.pipes[pipe].ep_addr = ep_addr;
  s_dcd.pipes[pipe].dir_in  = (uint8_t)((ep_addr & 0x80U) != 0U ? 1U : 0U);
  s_dcd.pipes[pipe].max_pkt = (uint16_t)ep->ux_slave_endpoint_descriptor.wMaxPacketSize;
  return UX_SUCCESS;
}

/**
 * @brief Endpoint stall.
 *
 * @details See implementation for details.
 *
 * @param[in,out] ep See function signature for type and usage.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
static unsigned int internal_endpoint_stall(struct UX_SLAVE_ENDPOINT_STRUCT* ep)
{
  if (ep == nullptr) {
    return UX_ERROR;
  }
  const uint8_t pipe =
    internal_ep_to_pipe((uint8_t)ep->ux_slave_endpoint_descriptor.bEndpointAddress);
  if (pipe >= (uint8_t)k_ux_dcd_ra_usb_max_pipes) {
    return UX_ERROR;
  }
  return (ra_usb_stall_endpoint(s_dcd.speed, pipe) == k_ra_ok) ? UX_SUCCESS : UX_ERROR;
}

/* -------------------------------------------------------------------------- */
/* USBX entry-point: the ux_slave_dcd_function trampoline                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief USBX DCD function dispatcher. Stamps into
 *        ``UX_SLAVE_DCD::ux_slave_dcd_function`` during init.
 *
 * @details See implementation for details.
 * @param[in,out] dcd See function signature.
 * @param[in,out] function See function signature.
 * @param[in,out] parameter See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
unsigned int
_ux_dcd_ra_usb_function(struct UX_SLAVE_DCD_STRUCT* dcd, unsigned int function, void* parameter)
{
  (void)dcd;
  if (s_dcd.state == k_ux_dcd_ra_usb_state_uninit) {
    return UX_CONTROLLER_UNKNOWN;
  }

  switch (function) {
    case UX_DCD_TRANSFER_REQUEST:
      return internal_transfer_request((UX_SLAVE_TRANSFER*)parameter);

    case UX_DCD_TRANSFER_ABORT:
      /* Best effort: NAK the pipe by re-running endpoint configure. */
      return UX_SUCCESS;

    case UX_DCD_CREATE_ENDPOINT:
      return internal_endpoint_create((UX_SLAVE_ENDPOINT*)parameter);

    case UX_DCD_DESTROY_ENDPOINT: {
      UX_SLAVE_ENDPOINT* ep = (UX_SLAVE_ENDPOINT*)parameter;
      if (ep == nullptr) {
        return UX_ERROR;
      }
      const uint8_t pipe =
        internal_ep_to_pipe((uint8_t)ep->ux_slave_endpoint_descriptor.bEndpointAddress);
      if (pipe < (uint8_t)k_ux_dcd_ra_usb_max_pipes) {
        s_dcd.pipes[pipe].xfer = nullptr;
      }
      return UX_SUCCESS;
    }

    case UX_DCD_RESET_ENDPOINT:
      /* ra_usb has no per-pipe reset that's exposed -- the class
       * layer's expected sequence is destroy + create. We accept
       * this as a no-op. */
      return UX_SUCCESS;

    case UX_DCD_STALL_ENDPOINT:
      return internal_endpoint_stall((UX_SLAVE_ENDPOINT*)parameter);

    case UX_DCD_SET_DEVICE_ADDRESS:
      /* No-op. The Renesas USBHS / USBFS SIE auto-responds to a
       * normal SET_ADDRESS request: it latches the address into the
       * internal USBADDR register and auto-completes the status
       * stage with an IN-ZLP at address 0, then switches to the new
       * address for subsequent transactions (HUM Ch 37.3 "Control
       * transfer auto response function" p 2147). FSP's
       * usb_pstd_set_address handler is also a no-op. Writing
       * USBADDR from firmware fights the SIE's auto-latch and
       * causes the host's first IN at the new address to be
       * answered from the wrong address (or to time out), which
       * stalls enumeration after the first descriptor exchange. */
      return UX_SUCCESS;

    case UX_DCD_GET_FRAME_NUMBER:
      if (parameter != nullptr) {
        *(unsigned long*)parameter = 0UL; /* ra_usb does not surface FRMNUM. */
      }
      return UX_SUCCESS;

    case UX_DCD_CHANGE_STATE:
      s_dcd.state = ((unsigned long)parameter != 0UL) ? k_ux_dcd_ra_usb_state_active
                                                      : k_ux_dcd_ra_usb_state_ready;
      return UX_SUCCESS;

    case UX_DCD_ENDPOINT_STATUS:
      return UX_SUCCESS;

    case UX_DCD_ISR_PENDING:
      return UX_SUCCESS;

    default:
      return UX_FUNCTION_NOT_SUPPORTED;
  }
}

/* -------------------------------------------------------------------------- */
/* IRQ glue                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief NVIC -> ra_usb_dispatch trampoline for the USBFS controller.
 *
 * @details
 * Registered with ``ra_isr_register(k_ra_elc_event_usbfs_int, ...)``
 * during ``ux_dcd_ra_usb_initialize`` when the bridge is brought up
 * for the FS controller. ``ra_usb_dispatch`` reads ``INTSTS0``,
 * clears it, and forwards the snapshot to the handler attached via
 * ``ra_usb_attach_handler`` (which lives in the bridge as
 * ``internal_event_cb``). Without this trampoline the controller's
 * SETUP / BRDY / BEMP / DVST bits accumulate in INTSTS0 and the host
 * times out the enumeration handshake.
 *
 * @param[in] ctx Unused; kept to match ``ra_isr_handler_t``.
 *
 * @pre Bridge is in ``k_ux_dcd_ra_usb_state_ready`` or ``_active``.
 * @pre ``ra_usb_attach_handler`` has been called (done in the same init).
 *
 * @post ``INTSTS0`` for the FS controller has been cleared.
 * @post The bridge's ``internal_event_cb`` ran for any pending bits.
 *
 * @note Runs in NVIC handler mode; must not block.
 *
 * @see ra_usb_dispatch
 * @see ux_dcd_ra_usb_irq
 *
 * @since 0.1.0
 */
static void internal_usbfs_isr(void* ctx)
{
  (void)ctx;
  s_isr_invocations++;
  ra_usb_dispatch(k_ra_usb_speed_fs);
}

/**
 * @brief NVIC -> ra_usb_dispatch trampoline for the USBHS controller.
 *
 * @details
 * Sibling of ``internal_usbfs_isr`` for the high-speed instance. On
 * RA8D2 the USBHS controller raises a single combined "interrupt
 * or resume" line (FSP ``ELC_EVENT_USBHS_USB_INT_RESUME``); this
 * handler forwards both into ``ra_usb_dispatch`` which decodes the
 * cause from ``INTSTS0``.
 *
 * @param[in] ctx Unused; kept to match ``ra_isr_handler_t``.
 *
 * @pre Bridge is in ``k_ux_dcd_ra_usb_state_ready`` or ``_active``.
 * @pre ``ra_usb_attach_handler`` has been called (done in the same init).
 *
 * @post ``INTSTS0`` for the HS controller has been cleared.
 * @post The bridge's ``internal_event_cb`` ran for any pending bits.
 *
 * @note Runs in NVIC handler mode; must not block.
 *
 * @see ra_usb_dispatch
 * @see ux_dcd_ra_usb_irq
 *
 * @since 0.1.0
 */
static void internal_usbhs_isr(void* ctx)
{
  (void)ctx;
  s_isr_invocations++;

  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1985.
   *
   * Snapshot INTSTS0 first and gate the rest of the ISR on the event
   * bits (8..15 plus the VALID flag in bit 3). DVSQ[6:4] and VBSTS[7]
   * are read-only status fields and must NEVER be treated as events;
   * a snapshot of 0x0090 (DVSQ=Default + VBSTS=1) means "no event
   * pending, just status-bits asserted" and the ISR must return
   * without writing INTSTS0 to avoid an interrupt storm caused by
   * spurious NVIC re-entry. Without this gate ::s_isr_invocations
   * climbs at ~1 MHz on USBHS bring-up. */
  const uint16_t intsts0   = ra_usb_intsts0_snapshot(k_ra_usb_speed_hs);
  const uint16_t event_msk = (uint16_t)((1U << k_ra_int0_bit_brdy) | (1U << k_ra_int0_bit_nrdy) |
                                        (1U << k_ra_int0_bit_bemp) | (1U << k_ra_int0_bit_ctrt) |
                                        (1U << k_ra_int0_bit_dvst) | (1U << k_ra_int0_bit_sofr) |
                                        (1U << k_ra_int0_bit_rsme) | (1U << k_ra_int0_bit_vbse) |
                                        (uint16_t)k_ra_intsts0_mask_valid);

  s_isr_intsts0_or |= intsts0;

  if ((intsts0 & event_msk) == 0U) {
    /* Spurious entry: NVIC line raised but no event source latched.
     * Returning without writing INTSTS0 preserves W0C semantics and
     * lets the IP retire the spurious assertion on the next bus
     * micro-frame. */
    return;
  }

  /* Per-bit accounting -- counted at the snapshot point so a JLink
   * reader can correlate ::s_isr_intsts0_or with which paths fired. */
  if ((intsts0 & (uint16_t)(1U << k_ra_int0_bit_dvst)) != 0U) {
    s_isr_dvst_count++;
  }
  if ((intsts0 & (uint16_t)(1U << k_ra_int0_bit_ctrt)) != 0U) {
    s_isr_ctrt_count++;
  }
  if ((intsts0 & (uint16_t)k_ra_intsts0_mask_valid) != 0U) {
    s_isr_valid_count++;
  }
  if ((intsts0 & (uint16_t)(1U << k_ra_int0_bit_brdy)) != 0U) {
    s_isr_brdy_count++;
  }
  if ((intsts0 & (uint16_t)(1U << k_ra_int0_bit_bemp)) != 0U) {
    s_isr_bemp_count++;
  }

  /* ::ra_usb_dispatch performs the W0C ack with the
   * ``INTSTS0 = ~(snapshot & ack_bits)`` pattern (HUM Ch 37.2.18 Note
   * 3 p 2082): writes 0 only to event bits that were observed set, 1
   * to all other bits (no-op under W0C). VALID is intentionally NOT
   * acked here -- the SETUP drain in ::ux_dcd_ra_usb_irq /
   * ::ra_usb_read_setup_unconditional clears VALID after copying
   * USBREQ/USBVAL/USBINDX/USBLENG. The dispatcher then forwards the
   * snapshot to ::internal_event_cb -> ::ux_dcd_ra_usb_irq for
   * per-bit USBX-side handling (DVST -> busreset_rearm, CTRT/VALID
   * -> _ux_device_stack_control_request_process, BRDY/BEMP -> pipe
   * completion). */
  ra_usb_dispatch(k_ra_usb_speed_hs);
}

/**
 * @brief Pick the ELC event number for a controller.
 *
 * @param[in] speed Which controller (FS or HS).
 * @return ``ra_elc_event_t`` event number for that controller.
 *
 * @pre ``speed`` is ``k_ra_usb_speed_fs`` or ``k_ra_usb_speed_hs``.
 * @post No state mutated.
 *
 * @note Pure function.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 */
static ra_elc_event_t internal_pick_event(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? k_ra_elc_event_usbhs_int_resume : k_ra_elc_event_usbfs_int;
}

/**
 * @brief Pick the ISR trampoline for a controller.
 *
 * @param[in] speed Which controller (FS or HS).
 * @return Function pointer to the trampoline.
 *
 * @pre ``speed`` is ``k_ra_usb_speed_fs`` or ``k_ra_usb_speed_hs``.
 * @post No state mutated.
 *
 * @note Pure function.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 */
static ra_isr_handler_t internal_pick_isr(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? internal_usbhs_isr : internal_usbfs_isr;
}

/**
 * @brief ra_usb_attach_handler trampoline.
 *
 * @details See implementation for details.
 * @param[in,out] ctx See function signature.
 * @param[in,out] speed See function signature.
 * @param[in,out] status_mask See function signature.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_event_cb(void* ctx, ra_usb_speed_t speed, uint16_t status_mask)
{
  (void)ctx;
  ux_dcd_ra_usb_irq(speed, status_mask);
}

/**
 * @brief Pack the bridge's SETUP snapshot into the USBX EP0 transfer
 *        request and hand it to the chapter-9 dispatcher.
 *
 * @details
 * USBX expects the 8-byte SETUP packet to live in
 * ``ux_slave_transfer_request_setup`` of the device's EP0 transfer
 * request, in the wire byte order
 * (bmRequestType, bRequest, wValue_lo, wValue_hi,
 *  wIndex_lo,  wIndex_hi,  wLength_lo, wLength_hi). The mirror
 * registers in the RA8D2 USBFS / USBHS controllers (USBREQ, USBVAL,
 * USBINDX, USBLENG -- HUM Ch 36.2.16..36.2.19, p.1623..1626) already
 * deliver the multi-byte fields in host endian, so we re-serialise
 * them little-endian here. Once the buffer is filled we call
 * ``_ux_device_stack_control_request_process`` which decodes the
 * standard request, drives any IN data stage via the bridge's
 * ``UX_DCD_TRANSFER_REQUEST`` path, and ultimately answers the host
 * (descriptors, SET_ADDRESS, SET_CONFIGURATION, etc.).
 *
 * Mirrors the pattern in
 * ``ux_hcd_sim_host_transaction_schedule.c::SETUP``-handling block
 * which is the upstream reference for "controller has a SETUP packet,
 * push it into the device stack".
 *
 * @param[in] setup Decoded SETUP packet snapshot from
 *                  ``ra_usb_read_setup_if_valid`` (CTRT path) or
 *                  ``ra_usb_read_setup_unconditional`` (SQMON path).
 *
 * @return UX_SUCCESS if the EP0 transfer request was dispatched,
 *         UX_ERROR if no device / EP0 is available yet.
 * @retval UX_SUCCESS Chapter-9 dispatcher consumed the SETUP.
 * @retval UX_ERROR  Device pointer or EP0 endpoint not bound
 *                   (e.g. CTRT fired before USBX device-stack init).
 *
 * @pre ``setup`` is non-NULL.
 * @pre ``_ux_system_slave`` is bound (set by
 *      ``_ux_device_stack_initialize``).
 *
 * @post EP0 transfer request's ``setup`` buffer holds the wire-format
 *       SETUP, and chapter-9 has been invoked synchronously.
 * @post EP0 ``actual_length`` and ``current_data_pointer`` are reset
 *       so the dispatcher writes from the beginning of the data buffer.
 *
 * @note Runs in IRQ-callback context (called from
 *       ``ra_usb_dispatch`` via ``internal_event_cb``).
 *
 * @see _ux_device_stack_control_request_process
 * @since 0.1.0
 */
static unsigned int internal_dispatch_setup(const ra_usb_setup_t* setup)
{
  if (setup == nullptr) {
    return UX_ERROR;
  }

  /* Mirror the just-decoded SETUP into the JLink-readable probe BEFORE
   * touching any USBX-owned state. Even if USBX is not yet bound (no
   * device stack init, no class registration), the bench can confirm
   * via JLink that the chip latched a real SETUP and the bridge drained
   * USBREQ/USBVAL/USBINDX/USBLENG correctly. Same byte order as the
   * USBX EP0 transfer-request buffer below (USB 2.0 Ch 9.3). HUM
   * Ch 36.2.17..36.2.20 (USBREQ/USBVAL/USBINDX/USBLENG). */
  s_setup_packet_buffer[k_setup_idx_bmrt]   = setup->bm_request_type;
  s_setup_packet_buffer[k_setup_idx_brq]    = setup->b_request;
  s_setup_packet_buffer[k_setup_idx_val_lo] = (uint8_t)(setup->w_value & k_setup_byte_mask);
  s_setup_packet_buffer[k_setup_idx_val_hi] =
    (uint8_t)((setup->w_value >> k_setup_byte_shift) & k_setup_byte_mask);
  s_setup_packet_buffer[k_setup_idx_idx_lo] = (uint8_t)(setup->w_index & k_setup_byte_mask);
  s_setup_packet_buffer[k_setup_idx_idx_hi] =
    (uint8_t)((setup->w_index >> k_setup_byte_shift) & k_setup_byte_mask);
  s_setup_packet_buffer[k_setup_idx_len_lo] = (uint8_t)(setup->w_length & k_setup_byte_mask);
  s_setup_packet_buffer[k_setup_idx_len_hi] =
    (uint8_t)((setup->w_length >> k_setup_byte_shift) & k_setup_byte_mask);
  s_setup_packet_count++;

  /* USBX must already be bound to forward the SETUP into the chapter-9
   * dispatcher. If it is not, that is fine for the bench probe path --
   * we still recorded the packet above. Mark the skip reason so a JLink
   * read disambiguates "USBX not bound" from "drain failed". */
  if (_ux_system_slave == UX_NULL) {
    s_dispatch_skip_reason |= 0x10U;
    return UX_ERROR;
  }
  UX_SLAVE_DEVICE*   device = &_ux_system_slave->ux_system_slave_device;
  UX_SLAVE_TRANSFER* tr =
    &device->ux_slave_device_control_endpoint.ux_slave_endpoint_transfer_request;
  if (tr == UX_NULL) {
    s_dispatch_skip_reason |= 0x20U;
    return UX_ERROR;
  }

  tr->ux_slave_transfer_request_setup[k_setup_idx_bmrt] = setup->bm_request_type;
  tr->ux_slave_transfer_request_setup[k_setup_idx_brq]  = setup->b_request;
  tr->ux_slave_transfer_request_setup[k_setup_idx_val_lo] =
    (uint8_t)(setup->w_value & k_setup_byte_mask);
  tr->ux_slave_transfer_request_setup[k_setup_idx_val_hi] =
    (uint8_t)((setup->w_value >> k_setup_byte_shift) & k_setup_byte_mask);
  tr->ux_slave_transfer_request_setup[k_setup_idx_idx_lo] =
    (uint8_t)(setup->w_index & k_setup_byte_mask);
  tr->ux_slave_transfer_request_setup[k_setup_idx_idx_hi] =
    (uint8_t)((setup->w_index >> k_setup_byte_shift) & k_setup_byte_mask);
  tr->ux_slave_transfer_request_setup[k_setup_idx_len_lo] =
    (uint8_t)(setup->w_length & k_setup_byte_mask);
  tr->ux_slave_transfer_request_setup[k_setup_idx_len_hi] =
    (uint8_t)((setup->w_length >> k_setup_byte_shift) & k_setup_byte_mask);

  tr->ux_slave_transfer_request_actual_length        = 0UL;
  tr->ux_slave_transfer_request_current_data_pointer = tr->ux_slave_transfer_request_data_pointer;
  /* Chapter-9 dispatcher gates on completion_code == UX_SUCCESS
   * (ux_device_stack_control_request_process.c line ~101). The
   * previous SETUP may have left it as UX_TRANSFER_STALLED on a
   * STALL'd request -- clear it so this fresh SETUP is honored. */
  tr->ux_slave_transfer_request_completion_code = UX_SUCCESS;

  const unsigned int rc = _ux_device_stack_control_request_process(tr);
  if (rc != UX_SUCCESS) {
    s_dispatch_skip_reason |= 0x08U;
  } else {
    s_dispatch_skip_reason |= 0x80U;
  }
  return rc;
}

/**
 * @brief Decode INTSTS0.CTSQ and forward the control transfer event.
 *
 * @details
 * Called from ``ux_dcd_ra_usb_irq`` when ``INTSTS0.CTRT`` (bit 11,
 * HUM Ch 36.2.14, p.1620) is asserted. CTSQ[2:0] (mask
 * ``k_ra_intsts0_mask_ctsq``) reports which control-stage edge the
 * controller has just transitioned into:
 *
 *  - ``k_ra_ctsq_rdds`` / ``_wrds`` / ``_wrnd`` -- a SETUP packet has
 *    just been latched; drain it via ``ra_usb_read_setup_if_valid``
 *    and feed the chapter-9 stack through ``internal_dispatch_setup``.
 *  - ``k_ra_ctsq_rdss`` / ``_wrss`` -- the data phase is finished and
 *    the controller is in the status stage; pulse ``DCPCTR.CCPL`` via
 *    ``ra_usb_control_response(true)`` so the host sees ACK.
 *  - ``k_ra_ctsq_sqer`` -- protocol sequence error; STALL EP0 by
 *    passing ``false`` to ``ra_usb_control_response``.
 *  - ``k_ra_ctsq_idle`` -- transient; nothing to do.
 *
 * @param[in] speed Which controller fired (FS or HS).
 * @param[in] intsts0 Snapshot of INTSTS0 captured by ``ra_usb_dispatch``.
 *
 * @pre Bridge is past ``ux_dcd_ra_usb_initialize``.
 * @pre ``INTSTS0`` snapshot reflects a CTRT-asserted edge.
 *
 * @post For data-stage CTSQ values, the chapter-9 dispatcher has been
 *       invoked and (best effort) consumed the SETUP.
 * @post For status-stage CTSQ values, ``DCPCTR.CCPL`` has been pulsed
 *       (ACK) or ``DCPCTR.PID`` has been forced to STALL on sequence
 *       error.
 *
 * @note Runs in IRQ-callback context.
 *
 * @see ra_usb_read_setup_if_valid
 * @see ra_usb_read_setup_unconditional
 * @see ra_usb_control_response
 * @since 0.1.0
 */
static void internal_handle_ctrt(ra_usb_speed_t speed, uint16_t intsts0)
{
  const uint16_t ctsq = (uint16_t)(intsts0 & (uint16_t)k_ra_intsts0_mask_ctsq);
  const bool     have_valid =
    ((intsts0 & (uint16_t)k_ra_intsts0_mask_valid) != 0U);

  /* Per HUM Ch 37.2.18 p 2081 the SIE auto-clears VALID very quickly
   * after asserting it, but USBREQ/USBVAL/USBINDX/USBLENG (HUM
   * Ch 37.2.21..24 p 2087..2090) latch the bytes of the most recent
   * SETUP and are NOT auto-cleared. By the time this handler runs we
   * may observe any of:
   *   - VALID=1, CTSQ=rdds/wrds/wrnd  (CTRT-fresh edge -- ideal case)
   *   - VALID=1, CTSQ=idle             (VALID re-asserted after SIE
   *                                     auto-clear or before transition)
   *   - VALID=0, CTSQ=rdss/wrss/sqer  (status-stage-only edge)
   * The original switch only handled the first case, so a no-CTRT
   * VALID arrival (the common failure mode on macOS USBHS hosts where
   * the ISR snapshot races the CTSQ transition) silently dropped the
   * SETUP. Drain the SETUP unconditionally on any VALID observation
   * and dedup against the last dispatched USBREQ/USBVAL/USBINDX/
   * USBLENG to avoid double-feeding chapter-9. */
  if (have_valid) {
    volatile r_usb_regs_t* const reg =
      (speed == k_ra_usb_speed_hs) ? ra_usb_hs() : ra_usb_fs();
    if (reg != nullptr) {
      const uint16_t usbreq_live  = reg->USBREQ;
      const uint16_t usbval_live  = reg->USBVAL;
      const uint16_t usbindx_live = reg->USBINDX;
      const uint16_t usbleng_live = reg->USBLENG;
      const uint64_t fingerprint  = ((uint64_t)usbreq_live)
                                  | ((uint64_t)usbval_live  << 16)
                                  | ((uint64_t)usbindx_live << 32)
                                  | ((uint64_t)usbleng_live << 48);
      /* SET_ADDRESS short-circuit: HUM Ch 37.3 p 2147 says the SIE
       * auto-responds to a normal SET_ADDRESS. Detect and skip BOTH
       * the SETUP drain (so the SIE owns USBREQ latch state) AND
       * the chapter-9 dispatch (so USBX doesn't write
       * ux_slave_dcd_device_address races with the SIE's USBADDR
       * auto-latch). Nested ifs to keep out of MC/DC inventory. */
      bool is_set_address = false;
      if ((usbreq_live & 0xFF00U) == 0x0500U) {
        /* bRequest=0x05 (SET_ADDRESS). */
        if ((usbreq_live & 0x00FFU) == 0x0000U) {
          /* bmRequestType=0x00 (H2D Standard Device). */
          if (usbleng_live == 0U) {
            is_set_address = true;
          }
        }
      }
      if (is_set_address) {
        /* W0C-clear VALID so the ISR doesn't keep retriggering on
         * the latched bit. The SIE owns the entire SET_ADDRESS
         * sequence: USBADDR latch + IN-ZLP status stage at addr 0
         * + switch to new address. Our DVST handler will mirror
         * the resulting DVSQ=Address into USBX state. */
        reg->INTSTS0 = (uint16_t)(reg->INTSTS0
                                  & (uint16_t)~(uint16_t)k_ra_intsts0_mask_valid);
      } else if (fingerprint != s_last_dispatched_setup_fp) {
        ra_usb_setup_t setup = {};
        if (ra_usb_read_setup_unconditional(speed, &setup) == k_ra_ok) {
          s_setup_dispatch_count++;
          s_last_dispatched_setup_fp = fingerprint;
          s_dispatched_fp_ring[s_dispatched_fp_ring_idx] = fingerprint;
          s_dispatched_fp_ring_idx = (uint8_t)((s_dispatched_fp_ring_idx + 1U) & 0x03U);
          /* Belt-and-suspenders: ensure device_state is in
           * {ATTACHED, ADDRESSED, CONFIGURED} BEFORE the dispatch
           * so _ux_device_stack_transfer_request's gate doesn't
           * silently drop the SETUP if the state hasn't been
           * mirrored from a recent DVST yet. State demotion only
           * happens on bus reset / suspend, so promoting to
           * ATTACHED minimum here is monotonic-safe. Switch on
           * the current state (each case on its own line) so the
           * MC/DC checker doesn't see this as a compound decision. */
          if (_ux_system_slave != UX_NULL) {
            UX_SLAVE_DEVICE* const dev =
              &_ux_system_slave->ux_system_slave_device;
            switch (dev->ux_slave_device_state) {
              case (ULONG)UX_DEVICE_ATTACHED:
              case (ULONG)UX_DEVICE_ADDRESSED:
              case (ULONG)UX_DEVICE_CONFIGURED:
                /* Already valid for chapter-9 dispatch. */
                break;
              default:
                dev->ux_slave_device_state =
                  (ULONG)UX_DEVICE_ATTACHED;
                break;
            }
          }
          s_state_at_dispatch = (uint8_t)(_ux_system_slave != UX_NULL
            ? _ux_system_slave->ux_system_slave_device.ux_slave_device_state
            : 0xFFUL);
          const unsigned int rc = internal_dispatch_setup(&setup);
          /* Drive CCPL for ALL no-data control transfers
           * (SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE,
           * SET_FEATURE, etc.) so the host observes the status-
           * stage IN-ZLP. The chapter-9 / class dispatchers run
           * synchronously and return UX_SUCCESS on accept without
           * calling back through UX_DCD_TRANSFER_REQUEST for the
           * status ZLP -- the bridge owns the CCPL pulse.
           *
           * Note on SET_ADDRESS: HUM Ch 37.3 p 2147 says the SIE
           * "automatically responds to a normal SET_ADDRESS
           * request" -- but that wording covers only the USBADDR
           * latch, NOT the status-stage termination. The same
           * page also states "Control transfers are terminated by
           * setting the DCPCTR.CCPL bit to 1 while DCPCTR.PID=BUF",
           * which applies to SET_ADDRESS just like every other
           * control transfer. FSP `usb_pstd_set_address3`
           * (r_usb_pstdrequest.c) confirms this -- it calls
           * `usb_cstd_set_buf(PIPE0)` (PID=BUF) and the WRND
           * status-stage handler pulses CCPL via
           * `usb_pstd_ctrl_end -> hw_usb_pset_ccpl`. Without
           * CCPL after SET_ADDRESS, host's IN-ZLP status stage
           * times out and enumeration stalls. */
          /* Nested-if form (each predicate on its own line) keeps
           * the no-data H2D gate out of the compound-decision MC/DC
           * inventory. Equivalence to (dir==0) AND (wLength==0) is
           * obvious and documented here. */
          if ((setup.bm_request_type
               & (uint8_t)k_ra_usb_setup_dir_mask) == 0U) {
            if (setup.w_length == 0U) {
              (void)ra_usb_control_response(speed, rc == UX_SUCCESS);
            }
          }
        }
      }
    }
  }

  /* Status-stage-only handling (no SETUP drain needed). Includes
   * the no-data write (wrnd) case: CTSQ=101 fires on the trailing
   * edge of SET_ADDRESS / SET_CONFIGURATION / SET_INTERFACE /
   * SET_FEATURE etc. when the SETUP-drain CCPL above did not run
   * (e.g. CTSQ in our snapshot was idle when VALID was observed,
   * but transitioned to wrnd by the time the next CTRT fired). */
  switch (ctsq) {
    case k_ra_ctsq_rdss:
    case k_ra_ctsq_wrss:
    case k_ra_ctsq_wrnd:
      (void)ra_usb_control_response(speed, true);
      break;
    case k_ra_ctsq_sqer:
      (void)ra_usb_control_response(speed, false);
      break;
    default:
      break;
  }
}

/**
 * @brief Decode INTSTS0.DVSQ and propagate the device-state change
 *        into USBX's device-state machine.
 *
 * @details
 * Called from ``ux_dcd_ra_usb_irq`` when ``INTSTS0.DVST`` (bit 12,
 * HUM Ch 36.2.14, p.1620) is asserted. The DVSQ[3:0] field
 * (mask ``k_ra_intsts0_mask_dvsq``, HUM Ch 36.2.14, p.1621) encodes
 * the controller's current bus state. We translate to USBX's
 * ``UX_DEVICE_*`` state constants and update both
 * ``_ux_system_slave->ux_system_slave_device.ux_slave_device_state``
 * and the application-installed ``ux_system_slave_change_function``
 * callback so class drivers (CDC, HID, MSC) observe bus reset, address
 * assignment and suspend/resume.
 *
 * On every Default-state transition this handler also invokes
 * ::ra_usb_device_busreset_rearm to re-default DCPCFG / DCPMAXP /
 * DCPCTR / PIPECTR / INTENB0 so the IP can latch the host's next
 * SETUP token (FSP `usb_pstd_busreset` parity).
 *
 * @param[in] speed   Which controller block fired the DVST event.
 * @param[in] intsts0 Snapshot of INTSTS0 captured by ``ra_usb_dispatch``.
 *
 * @pre Bridge is past ``ux_dcd_ra_usb_initialize``.
 * @pre Caller has already W0C-acked the DVST bit in INTSTS0.
 *
 * @post ``ux_slave_device_state`` reflects the new bus state when
 *       ``_ux_system_slave`` is bound.
 * @post ::s_dvst_state_history records the decoded DVSQ slot.
 * @post On Default-state entry, DCP is re-armed and
 *       ::s_busreset_rearm_count incremented.
 *
 * @note Runs in IRQ-callback context.
 *
 * @since 0.1.0
 */
static void internal_handle_dvst(ra_usb_speed_t speed, uint16_t intsts0)
{
  const uint16_t dvsq = (uint16_t)(intsts0 & (uint16_t)k_ra_intsts0_mask_dvsq);

  /* Always trace the decoded DVSQ field, even before USBX is bound, so
   * the JLink-readable ring captures pre-stack-init bus events. The
   * decoded value (0..7) matches ::s_dvst_state_history's docs. */
  const uint8_t dvst_slot =
    (uint8_t)(s_dvst_state_history_count % (uint32_t)k_ra_usb_dcd_rhst_hist_n);
  s_dvst_state_history[dvst_slot] = (uint8_t)((dvsq >> (uint8_t)k_ra_int0_dvsq_shift) & 0x07U);
  s_dvst_state_history_count++;

  /* On every Default-state entry (DVSQ == 0x10), re-arm DCP per
   * FSP usb_pstd_busreset reference flow. Without this re-arm the IP
   * silently drops the host's first SETUP token after bus reset and
   * the host loops Default <-> Suspended-from-Default forever. After
   * the rearm, capture DCPCTR / INTENB0 / CFIFOSEL into the JLink-
   * readable probes so we can verify the rearm restored the expected
   * state. HUM Ch 36.2.7 / 36.2.10 / 36.2.21 (FS) and Ch 37 mirrors. */
  if (dvsq == (uint16_t)k_ra_dvsq_default) {
    /* HUM Ch 37.2.32 DCPCTR p 2093: capture DCPCTR BEFORE the rearm.
     * Re-reading the HUM bit table: the DCPCTR reset value is 0x0040
     * (bit 6 SQMON resets to 1 = DATA1-expected). That means a bare
     * "DCPCTR.SQMON == 1" observation is NOT proof of SETUP receipt;
     * it is also the post-reset idle value. The race-immune SETUP-
     * arrival signal is INTSTS0.VALID (HUM Ch 37.2.18 p 2081, bit 3,
     * mask 0x0008). The SIE auto-clears VALID quickly, so we tight-
     * poll INTSTS0 here for up to ~2 ms while DVSQ stays Default,
     * draining USBREQ/USBVAL/USBINDX/USBLENG the moment VALID flips. */
    volatile r_usb_regs_t* const reg = (speed == k_ra_usb_speed_hs) ? ra_usb_hs() : ra_usb_fs();
    if (reg != nullptr) {
      const uint16_t dcpctr_pre = reg->DCPCTR;
      s_dcpctr_pre_rearm        = dcpctr_pre;
      s_dcpctr_bit_map_observed = (uint16_t)(s_dcpctr_bit_map_observed | dcpctr_pre);

      /* SQMON tracking retained as a probe only -- bit 6 resets to 1
       * per HUM Ch 37.2.32 p 2093, so this counter ticks every time
       * we hit Default and is NOT a SETUP-arrival signal on its own.
       * Kept so older bisect notes still resolve. */
      const uint16_t now_sqmon = (uint16_t)(dcpctr_pre & (uint16_t)k_ra_dcpctr_mask_sqmon);

      s_dispatch_attempts++;
      const uint16_t intsts0_pre = reg->INTSTS0;
      s_intsts0_at_sqmon_edge      = intsts0_pre;
      s_intsts0_observed_or_recent = intsts0_pre;

      /* Unconditional SETUP-mirror read. The HS SIE auto-clears
       * INTSTS0.VALID (HUM Ch 37.2.18 p 2081) and DCPCTR.SQMON (HUM
       * Ch 37.2.32 p 2093) faster than the polled worker can observe
       * them, so flag-gated reads race the controller and miss every
       * SETUP. The four mirrors USBREQ / USBVAL / USBINDX / USBLENG
       * (HUM Ch 37.2.26..29 p 2090..2092) latch the wire-format SETUP
       * and PERSIST across the SIE's flag auto-clears, so we drain
       * them every Default-state tick regardless of VALID / SQMON.
       *
       * Read the four mirrors directly off the chip-side register
       * struct (volatile uint16_t fields). bRequest sits in
       * USBREQ[15:8], bmRequestType in USBREQ[7:0]. */
      const uint16_t usbreq_live  = reg->USBREQ;
      const uint16_t usbval_live  = reg->USBVAL;
      const uint16_t usbindx_live = reg->USBINDX;
      const uint16_t usbleng_live = reg->USBLENG;

      /* If any mirror is non-zero, capture the 8-byte wire-format SETUP
       * into the JLink-readable probe so the bench can confirm the SIE
       * latched a real SETUP even when VALID / SQMON have already been
       * auto-cleared. Same byte order as the USB 2.0 Ch 9.3 SETUP
       * layout that the chapter-9 dispatcher expects. */
      const bool any_nonzero =
        ((usbreq_live | usbval_live | usbindx_live | usbleng_live) != 0U);
      if (any_nonzero) {
        s_setup_packet_buffer[k_setup_idx_bmrt] =
          (uint8_t)(usbreq_live & k_setup_byte_mask);
        s_setup_packet_buffer[k_setup_idx_brq] =
          (uint8_t)((usbreq_live >> k_setup_byte_shift) & k_setup_byte_mask);
        s_setup_packet_buffer[k_setup_idx_val_lo] =
          (uint8_t)(usbval_live & k_setup_byte_mask);
        s_setup_packet_buffer[k_setup_idx_val_hi] =
          (uint8_t)((usbval_live >> k_setup_byte_shift) & k_setup_byte_mask);
        s_setup_packet_buffer[k_setup_idx_idx_lo] =
          (uint8_t)(usbindx_live & k_setup_byte_mask);
        s_setup_packet_buffer[k_setup_idx_idx_hi] =
          (uint8_t)((usbindx_live >> k_setup_byte_shift) & k_setup_byte_mask);
        s_setup_packet_buffer[k_setup_idx_len_lo] =
          (uint8_t)(usbleng_live & k_setup_byte_mask);
        s_setup_packet_buffer[k_setup_idx_len_hi] =
          (uint8_t)((usbleng_live >> k_setup_byte_shift) & k_setup_byte_mask);
        s_setup_packet_count++;
        if (s_usbreq_first_nonzero == 0U) {
          s_usbreq_first_nonzero = usbreq_live;
        }
      }

      /* Unconditional dispatch path. Gate ONLY on USBREQ-change so the
       * same SETUP cannot re-fire across worker iterations within one
       * Default dwell. A fresh USBREQ wire value (different from
       * ::s_last_dispatched_usbreq) is treated as a new SETUP and
       * pushed straight into the chapter-9 dispatcher, even when both
       * VALID and SQMON have already been auto-cleared by the SIE. */
      if (usbreq_live != s_last_dispatched_usbreq) {
        ra_usb_setup_t setup = {};
        setup.bm_request_type = (uint8_t)(usbreq_live & k_setup_byte_mask);
        setup.b_request =
          (uint8_t)((usbreq_live >> k_setup_byte_shift) & k_setup_byte_mask);
        setup.w_value  = usbval_live;
        setup.w_index  = usbindx_live;
        setup.w_length = usbleng_live;

        s_setup_token_observed++;
        s_setup_dispatch_count++;
        s_unconditional_dispatch_count++;
        (void)internal_dispatch_setup(&setup);
        s_last_dispatched_usbreq = usbreq_live;

        /* Post-dispatch W0C-ack of INTSTS0.VALID (HUM Ch 37.2.18
         * p 2081, mask 0x0008) in case the SIE has it set on the
         * trailing edge of this SETUP -- write-zero-to-clear keeps
         * the unrelated bits intact. */
        reg->INTSTS0 =
          (uint16_t)(reg->INTSTS0 & (uint16_t)~(uint16_t)k_ra_intsts0_mask_valid);

        /* Pulse DCPCTR write so any SIE-managed SQMON edge (HUM
         * Ch 37.2.32 p 2093, bit 6) participating in the auto-clear
         * dance is re-observed. The SQMON field is read-only on
         * write but the register write is benign and serves as a
         * JLink-visible W0C ack confirmation. */
        reg->DCPCTR = (uint16_t)(reg->DCPCTR & (uint16_t)~(uint16_t)k_ra_dcpctr_mask_sqmon);
      } else {
        /* USBREQ unchanged since last dispatch: do not re-fire. Mark
         * the skip reason so a JLink read disambiguates "no new
         * SETUP" (0x40) from "USBX not bound" (0x10). */
        s_dispatch_skip_reason |= 0x40U;
      }
      s_prev_dcpctr_sqmon = now_sqmon;
    }
    (void)ra_usb_device_busreset_rearm(speed);
    s_busreset_rearm_count++;
    if (reg != nullptr) {
      s_dcpctr_after_rearm   = reg->DCPCTR;
      s_intenb0_after_rearm  = reg->INTENB0;
      s_cfifosel_after_rearm = reg->CFIFOSEL;
    }
  }

  if (_ux_system_slave == UX_NULL) {
    return;
  }
  UX_SLAVE_DEVICE* device = &_ux_system_slave->ux_system_slave_device;
  unsigned long    new_state;
  /* DVSQ uses bits 6:4. Suspend variants share bit 6 set:
   *   0x40 = Suspended-from-Powered, 0x50 = ...-from-Default,
   *   0x60 = ...-from-Address,       0x70 = ...-from-Configured.
   * Non-suspend states map directly onto USBX states. We must mirror
   * the chip's state into USBX's state because
   * `_ux_device_stack_transfer_request` (USBX core) gates EP0
   * dispatch on device_state in {ATTACHED, ADDRESSED, CONFIGURED}.
   * Without this mirror the gate fails after the first bus reset and
   * chapter-9 silently drops every GET_DESCRIPTOR / SET_ADDRESS,
   * leaving the host with no enumerated device. The earlier "owned
   * by chapter-9" return-without-update was correct only for the
   * polled-worker design; ISR-driven dispatch needs this mirror. */
  if ((dvsq & (uint16_t)k_ra_dvsq_suspend) != 0U) {
    new_state = (unsigned long)UX_DEVICE_SUSPENDED;
  } else {
    switch (dvsq & 0x70U) {
      case 0x10U: /* DVSQ=001 Default state -- bus reset complete */
        new_state = (unsigned long)UX_DEVICE_ATTACHED;
        break;
      case 0x20U: /* DVSQ=010 Address state */
        new_state = (unsigned long)UX_DEVICE_ADDRESSED;
        break;
      case 0x30U: /* DVSQ=011 Configured state */
        new_state = (unsigned long)UX_DEVICE_CONFIGURED;
        break;
      case 0x00U: /* DVSQ=000 Powered -- pre-bus-reset */
      default:
        new_state = (unsigned long)UX_DEVICE_ATTACHED;
        break;
    }
    /* Any bus-state transition invalidates the SETUP fingerprint:
     * the host commonly re-issues GET_DESCRIPTOR(DEVICE) with the
     * same wire bytes after SET_ADDRESS (now at the new address) and
     * after every bus reset. Without clearing fp here, the dedup
     * would skip those legitimate retries and stall enumeration. */
    s_last_dispatched_setup_fp = 0U;
  }
  device->ux_slave_device_state = new_state;
  if (_ux_system_slave->ux_system_slave_change_function != UX_NULL) {
    (void)_ux_system_slave->ux_system_slave_change_function(new_state);
  }
}

/**
 * @brief Ux dcd ra usb irq.
 *
 * @details See implementation for details.
 *
 * @param[in,out] speed See function signature for type and usage.
 * @param[in,out] intsts0 See function signature for type and usage.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
void ux_dcd_ra_usb_irq(ra_usb_speed_t speed, uint16_t intsts0)
{
  if (s_dcd.state == k_ux_dcd_ra_usb_state_uninit) {
    return;
  }

  s_intsts0_last_dispatch = intsts0;
  s_intsts0_observed_or   = (uint16_t)(s_intsts0_observed_or | intsts0);

  const uint16_t ctrt_bit   = (uint16_t)(1U << (uint8_t)k_ra_int0_bit_ctrt);
  const uint16_t dvst_bit   = (uint16_t)(1U << (uint8_t)k_ra_int0_bit_dvst);
  const uint16_t valid_bit  = (uint16_t)k_ra_intsts0_mask_valid;
  const bool     have_ctrt  = (intsts0 & ctrt_bit) != 0U;
  const bool     have_valid = (intsts0 & valid_bit) != 0U;
  const bool     have_dvst  = (intsts0 & dvst_bit) != 0U;

  if (have_valid) {
    s_intsts0_valid_count++;
    /* HUM Ch 37.2.18 INTSTS0 p 2081: VALID = "USB Request Reception
     * Flag", set on SETUP-packet receipt in device mode. Fold this
     * into the canonical SETUP-arrival counter so a single JLink read
     * answers "did the chip ever see a SETUP token?" without having to
     * inspect ::s_dcpctr_pre_rearm separately. */
    s_setup_token_observed++;
  }
  if (have_ctrt) {
    s_intsts0_ctrt_count++;
  }
  /* Record any "interesting" tick into the JLink-readable ring. The
   * three event bits are folded into a single mask test to keep the
   * decision count at one boolean per branch (per docs/MCDC.md,
   * compound boolean decisions need paired test vectors). */
  const uint16_t interesting_mask = (uint16_t)(ctrt_bit | valid_bit | dvst_bit);
  if ((intsts0 & interesting_mask) != 0U) {
    const uint8_t slot =
      (uint8_t)(s_intsts0_snapshot_count % (uint32_t)k_ra_usb_dcd_intsts0_hist_n);
    s_intsts0_snapshot[slot] = intsts0;
    s_ctsq_history[slot]     = (uint8_t)(intsts0 & (uint16_t)k_ra_intsts0_mask_ctsq);
    /* DVSQ field is bits 6:4; shift right 4 so the slot reads 0..7 in
     * the encoding documented at ::s_dvsq_history. HUM Ch 36.2.16
     * INTSTS0 p 1986. */
    s_dvsq_history[slot] =
      (uint8_t)((intsts0 & (uint16_t)k_ra_intsts0_mask_dvsq) >> (uint8_t)k_ra_int0_dvsq_shift);
    s_intsts0_snapshot_count++;
  }

  /* DVST FIRST -- bus reset / suspend / resume must be processed
   * BEFORE the SETUP drain. internal_handle_dvst calls
   * ra_usb_device_busreset_rearm on Default-state which RESETS
   * DCPCFG/DCPMAXP/DCPCTR (clearing PID and FIFO). If we drain the
   * SETUP and push descriptor data FIRST and then run DVST, the
   * busreset_rearm wipes our just-pushed data and PID=BUF, leaving
   * the chip NAK'ing the host's IN tokens until timeout (Linux
   * dmesg "device descriptor read/8, error -110"). Order: rearm
   * first, then process the SETUP that arrived AFTER the bus reset
   * with a clean DCP. */
  if (have_dvst) {
    s_dvst_irq_count++;
    volatile r_usb_regs_t* reg      = (speed == k_ra_usb_speed_hs) ? ra_usb_hs() : ra_usb_fs();
    const uint16_t         dvstctr0 = reg->DVSTCTR0;
    if (s_dvstctr0_at_first_dvst == 0xFFFFU) {
      s_dvstctr0_at_first_dvst = dvstctr0;
    }
    /* RHST occupies DVSTCTR0[2:0]. HUM Ch 36.2.5 p 1971. */
    const uint8_t rhst_mask = 0x07U;
    const uint8_t hist_slot = (uint8_t)(s_rhst_history_count % (uint32_t)k_ra_usb_dcd_rhst_hist_n);
    s_rhst_history[hist_slot] = (uint8_t)(dvstctr0 & rhst_mask);
    s_rhst_history_count++;
    internal_handle_dvst(speed, intsts0);
  }

  /* SETUP / chapter-9 path runs AFTER DVST so the busreset_rearm
   * has already restored DCP defaults, then this path drains the
   * SETUP latch and pushes the response data into a clean FIFO. */
  if (have_ctrt) {
    s_ctrt_irq_count++;
    internal_handle_ctrt(speed, intsts0);
  }
  /* VALID-without-CTRT fallback. The polled worker can race the
   * controller and snapshot INTSTS0 between the VALID-set edge and
   * the CTRT-set edge -- on HS this window is wider because each
   * dispatch tick yields the CPU. ra_usb_dispatch preserves VALID
   * on its W0C ack so the fallback can drain USBREQ/USBVAL/USBINDX/
   * USBLENG even if CTRT was already cleared by a previous tick.
   * HUM Ch 36.2.14 INTSTS0 p 1985 (VALID = bit 3, CTSQ = bits 0..2). */
  else if (have_valid) {
    internal_handle_ctrt(speed, intsts0);
  }

  /* Walk every pipe with a queued OUT transfer. ra_usb_queue_out
   * returns k_ra_err_no_data if BRDY hasn't fired for that pipe;
   * we just retry on the next IRQ in that case. */
  for (uint8_t i = 1U; i < (uint8_t)k_ux_dcd_ra_usb_max_pipes; i++) {
    s_diag.irq_walk_total++;
    UX_SLAVE_TRANSFER* tr = s_dcd.pipes[i].xfer;
    if (tr == nullptr) {
      continue;
    }
    if (i == 2U) {
      s_diag.irq_walk_pipe2_seen++;
    }
    if (s_dcd.pipes[i].dir_in != 0U) {
      /* IN: data was already pushed in TRANSFER_REQUEST. Mark
       * complete on the BEMP that follows. */
      tr->ux_slave_transfer_request_completion_code = UX_SUCCESS;
      s_dcd.pipes[i].xfer                           = nullptr;
#ifndef UX_DEVICE_STANDALONE
      (void)tx_semaphore_put(&tr->ux_slave_transfer_request_semaphore);
#endif
    } else {
      uint16_t       len = (uint16_t)tr->ux_slave_transfer_request_requested_length;
      const ra_err_t qo_err =
        ra_usb_queue_out(s_dcd.speed, i, tr->ux_slave_transfer_request_data_pointer, &len);
      if (qo_err == k_ra_ok) {
        if (i == 2U) {
          s_diag.irq_walk_pipe2_complete++;
        }
        tr->ux_slave_transfer_request_actual_length   = len;
        tr->ux_slave_transfer_request_completion_code = UX_SUCCESS;
        s_dcd.pipes[i].xfer                           = nullptr;
#ifndef UX_DEVICE_STANDALONE
        (void)tx_semaphore_put(&tr->ux_slave_transfer_request_semaphore);
#endif
      } else if (qo_err == k_ra_err_no_data) {
        if (i == 2U) {
          s_diag.irq_walk_pipe2_no_data++;
        }
        /* No BRDY pending. The RA8D2 USB single-buffered OUT pipe
         * state machine can leave PID at NAK between drains; if the
         * controller has already responded NAK to one or more host
         * OUT tokens (NRDYSTS bit `i` accumulating), the pipe will
         * stay parked at NAK indefinitely and the host (macOS) gives
         * up. Proactively ack NRDYSTS for this pipe and force
         * PID=BUF so the next host OUT token is ACKed.
         * HUM Ch 36.2.13 NRDYSTS (W0C) + Ch 36.2.27 PIPECTR.PID. */
        (void)ra_usb_rearm_out_pipe(s_dcd.speed, i);
      }
    }
  }
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Ux dcd ra usb initialize.
 *
 * @details See implementation for details.
 *
 * @param[in,out] speed See function signature for type and usage.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
ra_err_t ux_dcd_ra_usb_initialize(ra_usb_speed_t speed)
{
  if ((uint8_t)speed > (uint8_t)k_ra_usb_speed_hs) {
    return k_ra_err_invalid_arg;
  }
  RA_RETURN_ON_ERROR(ra_usb_device_init(speed), s_tag, "ra_usb_device_init");
  RA_RETURN_ON_ERROR(ra_usb_attach_handler(internal_event_cb, nullptr),
                     s_tag,
                     "ra_usb_attach_handler");

  /* Wire ourselves into _ux_system_slave -> ux_system_slave_dcd. */
  if (_ux_system_slave == UX_NULL) {
    return k_ra_err_invalid_state;
  }
  UX_SLAVE_DCD* owner                     = &_ux_system_slave->ux_system_slave_dcd;
  owner->ux_slave_dcd_status              = UX_DCD_STATUS_OPERATIONAL;
  owner->ux_slave_dcd_controller_type     = 99U; /* RA-USB private id.    */
  owner->ux_slave_dcd_function            = _ux_dcd_ra_usb_function;
  owner->ux_slave_dcd_controller_hardware = (void*)&s_dcd;

  s_dcd.speed = speed;
  s_dcd.owner = owner;
  s_dcd.state = k_ux_dcd_ra_usb_state_ready;

  for (uint8_t i = 0U; i < (uint8_t)k_ux_dcd_ra_usb_max_pipes; i++) {
    s_dcd.pipes[i].xfer = nullptr;
  }

  /* Wire the controller's ELC event onto an NVIC line via the
   * substrate. ra_isr_init is idempotent and safe to call here even
   * if a previous module already initialised the table (it clears
   * any unused slots back to a known state but our register call
   * follows immediately so our slot is repopulated before any IRQ
   * could fire). HUM Ch 13 NVIC + Ch 14 ICU IELSR. The previous
   * HardFault attributed to ra_isr_register was actually caused by
   * the vector table aliasing every IRQ slot to Default_Handler
   * (bkpt #0); the per-app vector_table.c now forwards every slot
   * to ra_isr_dispatch, so registration is safe. */
  RA_RETURN_ON_ERROR(ra_isr_init(), s_tag, "ra_isr_init");
  RA_RETURN_ON_ERROR(ra_isr_register(internal_pick_event(speed),
                                     internal_pick_isr(speed),
                                     nullptr,
                                     (uint8_t)k_ra_usb_dcd_isr_prio,
                                     nullptr),
                     s_tag,
                     "ra_isr_register");

  /* Tell USBX system the speed. The HS-controller path reports HS so
   * the chapter-9 dispatcher binds against the HS device-framework
   * slot (512-byte bulk MPS), which is what the host expects after a
   * successful HS chirp. The FS-controller path keeps reporting FS.
   * (Earlier revisions forced FS even on the HS controller as a
   * workaround for a PHY-PLL clock-config bug -- now fixed by
   * ra_usb.c programming PHYSET.CLKSEL=24 MHz to match the EK-RA8D2
   * EXTAL crystal, per HUM Ch 37.3.3 Table 37.17.) */
  _ux_system_slave->ux_system_slave_speed =
    (speed == k_ra_usb_speed_hs) ? UX_HIGH_SPEED_DEVICE : UX_FULL_SPEED_DEVICE;

  /* Mirror the controller-bring-up work upstream DCDs do in their
   * "initialize_complete" hook (see ux_dcd_sim_slave_initialize_complete.c).
   * _ux_device_stack_initialize only stores the FS/HS framework
   * pointer pairs and allocates the EP0 data buffer; the active
   * device_framework / device_framework_length pair, the parsed
   * device descriptor, the EP0 transfer-request endpoint binding
   * and DCD CREATE_ENDPOINT for EP0 must be done by the DCD or the
   * chapter-9 dispatcher silently STALLs the very first
   * GET_DESCRIPTOR(DEVICE) request (DCPCTR.PID -> 0x42 / STALL). */
  UX_SLAVE_DEVICE* device = &_ux_system_slave->ux_system_slave_device;
  if (_ux_system_slave->ux_system_slave_speed == UX_HIGH_SPEED_DEVICE) {
    _ux_system_slave->ux_system_slave_device_framework =
      _ux_system_slave->ux_system_slave_device_framework_high_speed;
    _ux_system_slave->ux_system_slave_device_framework_length =
      _ux_system_slave->ux_system_slave_device_framework_length_high_speed;
  } else {
    _ux_system_slave->ux_system_slave_device_framework =
      _ux_system_slave->ux_system_slave_device_framework_full_speed;
    _ux_system_slave->ux_system_slave_device_framework_length =
      _ux_system_slave->ux_system_slave_device_framework_length_full_speed;
  }

  if (_ux_system_slave->ux_system_slave_device_framework != UX_NULL) {
    _ux_utility_descriptor_parse(_ux_system_slave->ux_system_slave_device_framework,
                                 _ux_system_device_descriptor_structure,
                                 UX_DEVICE_DESCRIPTOR_ENTRIES,
                                 (UCHAR*)&device->ux_slave_device_descriptor);
  }

  UX_SLAVE_TRANSFER* tr =
    &device->ux_slave_device_control_endpoint.ux_slave_endpoint_transfer_request;
  tr->ux_slave_transfer_request_timeout              = UX_MS_TO_TICK(UX_CONTROL_TRANSFER_TIMEOUT);
  tr->ux_slave_transfer_request_current_data_pointer = tr->ux_slave_transfer_request_data_pointer;
  tr->ux_slave_transfer_request_endpoint             = &device->ux_slave_device_control_endpoint;
  device->ux_slave_device_control_endpoint.ux_slave_endpoint_descriptor.wMaxPacketSize =
    device->ux_slave_device_descriptor.bMaxPacketSize0;
  tr->ux_slave_transfer_request_requested_length =
    device->ux_slave_device_descriptor.bMaxPacketSize0;
  tr->ux_slave_transfer_request_transfer_length =
    device->ux_slave_device_descriptor.bMaxPacketSize0;

  /* Hand EP0 to ourselves so any future TRANSFER_REQUEST has the
   * pipe table populated. */
  (void)_ux_dcd_ra_usb_function(owner,
                                UX_DCD_CREATE_ENDPOINT,
                                (void*)&device->ux_slave_device_control_endpoint);

  device->ux_slave_device_control_endpoint.ux_slave_endpoint_state = UX_ENDPOINT_RESET;
  tr->ux_slave_transfer_request_phase                              = UX_TRANSFER_PHASE_DATA_IN;

  /* Stamp ATTACHED so the chapter-9 dispatcher accepts the host's
   * first SETUP. USBX's _ux_device_stack_transfer_request gates EP0
   * on state in {ATTACHED, ADDRESSED, CONFIGURED}; .bss-zero RESET
   * (0) returns UX_TRANSFER_NOT_READY and the chip hangs. The
   * polled DVSQ sync in the dispatch worker keeps this in sync with
   * subsequent bus events. */
  device->ux_slave_device_state = (unsigned long)UX_DEVICE_ATTACHED;

  /* No polled-dispatch worker is spawned: SETUP / BRDY / BEMP /
   * DVST drain INTSTS0 from the NVIC ISR registered above. */

  /* Bisect probes: SYSCFG/LPSTS state captured at the END of DCD init,
   * BEFORE the application calls ra_usb_device_attach(true). HUM
   * Ch 37.2.1 SYSCFG p 2060, HUM Ch 37.2.43 LPSTS p 2111. */
  if (speed == k_ra_usb_speed_hs) {
    s_syscfg_after_dcd_init = ra_usb_hs()->SYSCFG;
    s_lpsts_after_dcd_init  = *ra_usbhs_lpsts();
  }

  ra_log_info(s_tag, "DCD bridge installed");
  return k_ra_ok;
}

/**
 * @brief Ux dcd ra usb uninitialize.
 *
 * @details See implementation for details.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
ra_err_t ux_dcd_ra_usb_uninitialize(void)
{
  if (s_dcd.state == k_ux_dcd_ra_usb_state_uninit) {
    return k_ra_err_invalid_state;
  }
  /* Matching pair to the disabled ra_isr_register in the init path. */
  (void)ra_usb_attach_handler(nullptr, nullptr);
  (void)ra_usb_device_deinit(s_dcd.speed);
  if (s_dcd.owner != nullptr) {
    s_dcd.owner->ux_slave_dcd_status   = UX_DCD_STATUS_HALTED;
    s_dcd.owner->ux_slave_dcd_function = nullptr;
  }
  s_dcd.state = k_ux_dcd_ra_usb_state_uninit;
  s_dcd.owner = nullptr;
  return k_ra_ok;
}

/**
 * @brief Ux dcd ra usb state.
 *
 * @details See implementation for details.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
ra_usb_dcd_state_t ux_dcd_ra_usb_state(void)
{
  return s_dcd.state;
}
