/**
 * @file port/usbx/src/ux_dcd_ra8_usb_dvst.c
 * @brief USBX device-controller-driver bridge to ra8_usb -- device-state (DVST) path.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * Device-state-change handling: DVSQ decode, the USBX device-state mirror
 * policy, the negotiated-speed framework re-aim, and the DVST history
 * ring.
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
 * @var g_dvst_irq_count
 * @brief Counter of INTSTS0.DVST (device-state-transition) events.
 *
 * @details Bisect probe. Each bus reset / set-address / set-config
 * raises DVST; zero after attach means the host isn't reaching the
 * device. HUM Ch 36.2.14 p 1985.
 *
 * @note Written only by ::ux_dcd_ra8_usb_irq.
 * @since 0.1.0
 */
volatile uint32_t g_dvst_irq_count = 0U;

/**
 * @enum ra8_usb_dcd_sentinel_t
 * @brief "Not yet captured" sentinel for 16-bit register snapshots.
 */
typedef enum : uint16_t {
  k_ra8_usb_u16_unset = 0xFFFFU, /**< Snapshot has not been latched yet. */
} ra8_usb_dcd_sentinel_t;

/**
 * @var g_dvstctr0_at_first_dvst
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
volatile uint16_t g_dvstctr0_at_first_dvst = (uint16_t)k_ra8_usb_u16_unset;

/**
 * @enum ra8_usb_dcd_rhst_hist_t
 * @brief Sizing for the DVSTCTR0.RHST history ring.
 */
typedef enum : uint8_t {
  k_ra8_usb_dcd_rhst_hist_n = 16U, /**< Slots in g_rhst_history. */
} ra8_usb_dcd_rhst_hist_t;

/**
 * @var g_rhst_history
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
volatile uint8_t g_rhst_history[(uint32_t)k_ra8_usb_dcd_rhst_hist_n] = {};

/**
 * @var g_rhst_history_count
 * @brief Total DVST events seen; modulo k_ra8_usb_dcd_rhst_hist_n is
 *        the next write slot.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint32_t g_rhst_history_count = 0U;

/**
 * @var g_dvstctr0_history
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
 * Ring size 16 chosen to match ::g_rhst_history so JLink scripts can
 * read both arrays in one transaction.
 *
 * @note Single-writer (::internal_dispatch_worker tick).
 * @since 0.1.0
 */
volatile uint16_t g_dvstctr0_history[(uint32_t)k_ra8_usb_dcd_rhst_hist_n] = {};

/**
 * @var g_dvstctr0_history_count
 * @brief Total dispatch ticks observed; modulo
 *        ::k_ra8_usb_dcd_rhst_hist_n is the next write slot in
 *        ::g_dvstctr0_history.
 *
 * @note Single-writer (::internal_dispatch_worker tick).
 * @since 0.1.0
 */
volatile uint32_t g_dvstctr0_history_count = 0U;

/**
 * @var g_intsts1_history
 * @brief Per-dispatch-tick capture of the full INTSTS1 register.
 *
 * @details Companion probe to ::g_dvstctr0_history. INTSTS1
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
volatile uint16_t g_intsts1_history[(uint32_t)k_ra8_usb_dcd_rhst_hist_n] = {};

/**
 * @var g_dvst_state_history
 * @brief Per-DVST-event capture of the decoded DVSQ[2:0] field.
 *
 * @details JLink-readable trace of every device-state transition
 * (independent of the "interesting tick" filter that gates
 * ::g_dvsq_history). Each DVST IRQ writes the pre-shifted DVSQ value
 * (0=Powered, 1=Default, 2=Address, 3=Configured, 4..7=Suspend variant
 * per HUM Ch 36.2.16 p 1986). A healthy enumeration shows
 * 1, 1, 2, 2, 3, ...; the "stuck-in-default" symptom shows
 * 1, 5, 1, 5, 1, 5, ... (Default <-> Suspended-from-Default loop).
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint8_t g_dvst_state_history[(uint32_t)k_ra8_usb_dcd_rhst_hist_n] = {};

/**
 * @var g_dvst_state_history_count
 * @brief Total DVST events recorded into ::g_dvst_state_history.
 *
 * @details Modulo ::k_ra8_usb_dcd_rhst_hist_n is the next write slot.
 *
 * @note Single-writer (::internal_handle_dvst).
 * @since 0.1.0
 */
volatile uint32_t g_dvst_state_history_count = 0U;

/**
 * @enum ra8_usb_dcd_rhst_t
 * @brief DVSTCTR0.RHST[2:0] settled link-speed encodings.
 * @details HUM Ch 36.2.5 / Ch 37 DVSTCTR0 p 1971: the controller writes
 * this after the reset/chirp handshake. Only the two settled, addressable
 * speeds matter to the speed mirror; LS (1), in-reset (4) and undefined
 * (0) are transient and leave the mirror untouched.
 */
typedef enum : uint8_t {
  k_ra8_usb_rhst_fs = 2U, /**< Full speed settled. */
  k_ra8_usb_rhst_hs = 3U, /**< High speed settled. */
} ra8_usb_dcd_rhst_t;

/**
 * @brief Map a DVSQ-bits-4..6 value to the USBX device-state enum.
 *
 * @details DVSQ bit 6 set => Suspended-from-X variants. Non-suspend
 * Powered (0x00) is treated as ATTACHED for the chapter-9 gate.
 *
 * @param[in] dvsq DVSQ bits extracted from INTSTS0 (mask 0x70 plus
 *                 suspend bit).
 *
 * @return The USBX device state to mirror.
 * @retval UX_DEVICE_SUSPENDED Suspend variant observed.
 * @retval UX_DEVICE_ATTACHED Default or Powered.
 * @retval UX_DEVICE_ADDRESSED DVSQ=010 Address state.
 * @retval UX_DEVICE_CONFIGURED DVSQ=011 Configured state.
 *
 * @pre Caller masked dvsq to the DVSQ field.
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize``.
 * @post No state mutated.
 * @post Pure function.
 *
 * @note Pure; safe in IRQ context.
 * @since 0.1.0
 */
RA8_INTERNAL static unsigned long internal_dvst_map_dvsq_to_ux_state(uint16_t dvsq)
{
  if ((dvsq & (uint16_t)k_ra8_dvsq_suspend) != 0U) {
    return (unsigned long)UX_DEVICE_SUSPENDED;
  }
  switch (dvsq & (uint16_t)k_ra8_intsts0_mask_dvsq) {
    case (uint16_t)k_ra8_dvsq_default: /* Default -- bus reset complete */
      return (unsigned long)UX_DEVICE_ATTACHED;
    case (uint16_t)k_ra8_dvsq_address: /* Address state */
      return (unsigned long)UX_DEVICE_ADDRESSED;
    case (uint16_t)k_ra8_dvsq_configured: /* Configured state */
      return (unsigned long)UX_DEVICE_CONFIGURED;
    case (uint16_t)k_ra8_dvsq_powered: /* Powered -- pre-bus-reset */
    default:
      return (unsigned long)UX_DEVICE_ATTACHED;
  }
}

/**
 * @brief Record one DVST event into the JLink-readable causal history.
 *
 * @details One byte per event: high nibble = the raw DVSQ field
 * (shifted), low nibble = ``ux_slave_device_state`` at IRQ entry
 * (0xF when USBX is not bound yet). Reading the ring alongside the
 * SETUP trace reconstructs who demoted/upgraded the chapter-9 state.
 *
 * @param[in] dvsq Masked INTSTS0.DVSQ field for this event.
 *
 * @pre Called from the DVST IRQ path only (single writer).
 * @pre ::g_dvst_state_history_count monotonicity is maintained.
 * @post One ring slot holds the packed event; count incremented.
 * @post No other state changes.
 *
 * @note Diagnostic only; never read by production code.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_dvst_record_history(uint16_t dvsq)
{
  const uint8_t dvst_slot =
    (uint8_t)(g_dvst_state_history_count % (uint32_t)k_ra8_usb_dcd_rhst_hist_n);
  uint8_t entry_state = (uint8_t)k_dcd_trace_nibble;
  if (_ux_system_slave != UX_NULL) {
    entry_state = (uint8_t)(_ux_system_slave->ux_system_slave_device.ux_slave_device_state &
                            (unsigned long)k_dcd_trace_nibble);
  }
  const uint8_t packed            = (uint8_t)((uint8_t)((dvsq >> (uint8_t)k_ra8_int0_dvsq_shift)
                                                        << (uint8_t)k_dcd_trace_nib_shift) |
                                              entry_state);
  g_dvst_state_history[dvst_slot] = packed;
  g_dvst_state_history_count++;
  priv_trace_event((uint8_t)k_dcd_trace_kind_dvst, packed, 0U);
}

/**
 * @brief DVSQ mirror policy: may ``new_state`` be written into USBX?
 *
 * @details The chapter-9 stack owns the ADDRESSED/CONFIGURED
 * transitions (it sets them while processing SET_ADDRESS /
 * SET_CONFIGURATION) and the mirror must never demote that state
 * asynchronously: USBX class threads (storage) gate on CONFIGURED and
 * suspend themselves permanently when they wake to anything else.
 * Policy: suspends are not propagated (traffic pauses, gates keep);
 * upgrades apply directly; a disconnect + apply happens ONLY on a
 * genuine Default-state entry (true bus reset). A generic "any
 * downgrade is a reset" rule does not work because the hardware DVSQ
 * LAGS the stack during configuration -- it sits in Address state
 * until SET_CONFIGURATION's status stage completes, and ISR INTSTS0
 * snapshots can be staler still -- so it tears the just-built
 * configuration down microseconds after activation (observed against
 * macOS at HS as a sub-millisecond configure/deactivate loop with the
 * storage thread never scheduled). The Default-entry disconnect is
 * required: macOS's MSC driver resets the device at start-of-probe,
 * and without the teardown the stack treats the re-issued
 * SET_CONFIGURATION as a same-value no-op (no interface mount, no
 * class re-activation) and the storage thread parks forever.
 *
 * @param[in,out] device    USBX device instance (state read; on Default
 *                          entry the configuration is torn down).
 * @param[in]     dvsq      Masked DVSQ field from the INTSTS0 snapshot.
 * @param[in]     new_state ``dvsq`` mapped to a UX_DEVICE_* state.
 *
 * @return Whether the caller may write ``new_state`` into the stack.
 * @retval true  Upgrade, or Default-state entry (post-disconnect).
 * @retval false Suspend or a stale/lagging downgrade; do not touch.
 *
 * @pre ``device`` is non-NULL (caller checked ``_ux_system_slave``).
 * @pre ``new_state`` is the ::internal_dvst_map_dvsq_to_ux_state
 *      mapping of ``dvsq`` (the pair must describe the same event).
 * @post On Default-state entry the stack configuration has been torn
 *       down via ``_ux_device_stack_disconnect`` (no-op if none).
 * @post ``device->ux_slave_device_state`` itself is NOT written here;
 *       that is the caller's job iff the return value is true.
 *
 * @note ISR-callback context; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_dvst_policy_apply(UX_SLAVE_DEVICE* device, uint16_t dvsq, unsigned long new_state)
{
  bool apply = false;
  if (new_state == (unsigned long)UX_DEVICE_SUSPENDED) {
    apply = false;
  } else if (new_state > device->ux_slave_device_state) {
    apply = true; /* upgrade */
  } else if (dvsq == (uint16_t)k_ra8_dvsq_default) {
    (void)_ux_device_stack_disconnect(); /* deactivates iff configured */
    apply = true;
  }
  return apply;
}

/**
 * @brief Handle the DVST (device-state-changed) interrupt branch.
 *
 * @details Extracts the DVSQ field from ``intsts0``, records it in the
 * ring-buffer history for JLink readout, mirrors the decoded state into
 * the USBX device-state field via ``internal_dvst_map_dvsq_to_ux_state``
 * under the ::internal_dvst_policy_apply policy, and re-arms the DCP
 * after bus reset (Default-state entry) per the FSP
 * ``usb_pstd_busreset`` reference flow. Without the rearm the IP silently
 * drops the host's first SETUP token after bus reset.
 *
 * @param[in] speed Which controller fired (``k_ra8_usb_speed_fs`` or
 *                  ``k_ra8_usb_speed_hs``).
 * @param[in] intsts0 INTSTS0 snapshot captured at the top of the ISR.
 *
 * @pre Caller has masked ``intsts0`` against the event mask.
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize``.
 * @post ``g_dvst_state_history`` ring buffer captures the new DVSQ.
 * @post USBX device state mirrored to match the controller's DVSQ.
 *
 * @note ISR-callback context; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_handle_dvst(ra8_usb_speed_t speed, uint16_t intsts0)
{
  /* DVSQ field bits 6:4 on BOTH controllers (suspend = bit 6). USBHS
   * additionally reports VBUS status in bit 7, which must be stripped
   * -- keeping it made every event decode as "suspended" and the
   * mirror went inert (bus resets were ignored, see below). */
  const uint16_t dvsq = (uint16_t)(intsts0 & (uint16_t)k_ra8_intsts0_mask_dvsq);

  internal_dvst_record_history(dvsq);

  /* On Default-state entry (DVSQ == 0x10), re-arm DCP per FSP
   * usb_pstd_busreset reference flow. Without rearm the IP silently
   * drops the host's first SETUP token after bus reset. */
  if (dvsq == (uint16_t)k_ra8_dvsq_default) {
    priv_dvst_default_state(speed);
  }
  /* Suspended sub-states must not be treated as bus transitions. */

  if (_ux_system_slave == UX_NULL) {
    return;
  }
  UX_SLAVE_DEVICE*    device    = &_ux_system_slave->ux_system_slave_device;
  const unsigned long new_state = internal_dvst_map_dvsq_to_ux_state(dvsq);
  if ((dvsq & (uint16_t)k_ra8_dvsq_suspend) == 0U) {
    /* Any non-suspend bus-state transition invalidates the SETUP
     * de-dup fingerprint. */
    g_last_dispatched_setup_fp = 0U;
  }
  /* Upgrade-only mirror; disconnect + apply on true bus reset only --
   * rationale in internal_dvst_policy_apply's header. */
  if (internal_dvst_policy_apply(device, dvsq, new_state)) {
    device->ux_slave_device_state = new_state;
    if (_ux_system_slave->ux_system_slave_change_function != UX_NULL) {
      (void)_ux_system_slave->ux_system_slave_change_function(new_state);
    }
  }
}

/**
 * @brief Mirror the negotiated link speed from DVSTCTR0.RHST into USBX.
 *
 * @details The HS controller does not know its link speed until the
 * host's reset/chirp handshake settles: connected to an HS host it
 * runs at high speed, to an FS host it falls back to full speed. USBX
 * serves descriptors from the CURRENT framework pointer
 * (``ux_system_slave_device_framework``), which ``ux_dcd_ra8_usb_init``
 * seeds from the HS slot for the HS controller. If the link settles at
 * FS, that pointer must be re-aimed at the full-speed framework
 * (64-byte bulk MPS), otherwise the host reads a 512-byte-MPS bulk
 * descriptor on a full-speed link and the bulk pipes never carry a CBW
 * (observed in the chip-to-chip self-loop: an FS host drives the HS
 * device, device sticks at ADDRESSED, storage class thread never runs
 * media_read). This mirrors both ``ux_system_slave_speed`` and the
 * current framework to the settled RHST. Only settled FS/HS values
 * act; transient RHST (LS / in-reset / undefined) leaves both alone.
 *
 * @param[in] rhst Masked DVSTCTR0.RHST[2:0] from the DVST snapshot.
 *
 * @return Nothing.
 * @note Speed + current framework reflect a settled FS/HS link.
 *
 * @pre ``_ux_system_slave`` is bound (init has run).
 * @pre @p rhst is the 3-bit RHST field (already masked).
 * @post ``ux_system_slave_speed`` and ``..._device_framework`` match the
 *       settled link speed, or are unchanged for a transient RHST value.
 * @post No register or pipe state is touched; USBX bookkeeping only.
 *
 * @note ISR-callback context; a few field writes, never blocks.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_dvst_track_speed(uint8_t rhst)
{
  if (_ux_system_slave == UX_NULL) {
    return;
  }
  if (rhst == (uint8_t)k_ra8_usb_rhst_fs) {
    _ux_system_slave->ux_system_slave_speed = UX_FULL_SPEED_DEVICE;
    _ux_system_slave->ux_system_slave_device_framework =
      _ux_system_slave->ux_system_slave_device_framework_full_speed;
    _ux_system_slave->ux_system_slave_device_framework_length =
      _ux_system_slave->ux_system_slave_device_framework_length_full_speed;
  } else if (rhst == (uint8_t)k_ra8_usb_rhst_hs) {
    _ux_system_slave->ux_system_slave_speed = UX_HIGH_SPEED_DEVICE;
    _ux_system_slave->ux_system_slave_device_framework =
      _ux_system_slave->ux_system_slave_device_framework_high_speed;
    _ux_system_slave->ux_system_slave_device_framework_length =
      _ux_system_slave->ux_system_slave_device_framework_length_high_speed;
  } else {
    /* Transient RHST (LS / in-reset / undefined): leave the mirror. */
  }
}

/**
 * @brief Capture DVSTCTR0 / RHST history, mirror speed, dispatch DVST.
 *
 * @details Pulled out of ``ux_dcd_ra8_usb_irq`` so the outer ISR fits in
 * one page. Reads DVSTCTR0 (HUM Ch 36.2.5 p 1971) for RHST history,
 * latches the first-observed value into ``g_dvstctr0_at_first_dvst``,
 * mirrors the settled link speed via ``internal_dvst_track_speed``,
 * then calls ``internal_handle_dvst`` for the state-machine update.
 *
 * @param[in] speed Which controller fired (FS or HS).
 * @param[in] intsts0 INTSTS0 snapshot (forwarded to internal_handle_dvst).
 *
 * @pre Caller has already verified the DVST bit is set in ``intsts0``.
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize``.
 * @post ``g_dvst_irq_count`` and ``g_rhst_history_count`` incremented.
 * @post Speed mirror updated and ``internal_handle_dvst`` has run.
 *
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
void priv_irq_dvst_prelude(ra8_usb_speed_t speed, uint16_t intsts0)
{
  g_dvst_irq_count++;
  volatile r_usb_regs_t* reg      = (speed == k_ra8_usb_speed_hs) ? ra8_usb_hs() : ra8_usb_fs();
  const uint16_t         dvstctr0 = reg->DVSTCTR0;
  if (g_dvstctr0_at_first_dvst == (uint16_t)k_ra8_usb_u16_unset) {
    g_dvstctr0_at_first_dvst = dvstctr0;
  }
  /* RHST occupies DVSTCTR0[2:0]. HUM Ch 36.2.5 p 1971. */
  const uint8_t rhst_mask   = (uint8_t)k_ra8_usb_dvsq_field_mask;
  const uint8_t rhst        = (uint8_t)(dvstctr0 & rhst_mask);
  const uint8_t hist_slot   = (uint8_t)(g_rhst_history_count % (uint32_t)k_ra8_usb_dcd_rhst_hist_n);
  g_rhst_history[hist_slot] = rhst;
  g_rhst_history_count++;
  internal_dvst_track_speed(rhst);
  internal_handle_dvst(speed, intsts0);
}
