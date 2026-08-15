/**
 * @file board_usb_host.c
 * @brief USBHS HOST-mode controller model (self-loop peer of the USBFS device)
 *
 * @details
 * Implements the model declared in board_usb_host.h. The register window is
 * the shared "USB2_B" layout (ra8_usb_regs.h; HUM Ch 37 for the USBHS
 * instance) plus the HS-only PHY registers (PLLSTA / PHYSET / LPSTS) and the
 * DEVADDn slots. The polled first-party host engine drives this window with
 * plain MMIO -- no interrupts -- so the model services each access
 * synchronously and forwards the resulting bus transactions to the modelled
 * USBFS device over the loop-cable transport (board_usb.h):
 *
 *  - `DCPCTR.SUREQ` runs the SETUP transaction: the USBREQ..USBLENG mirrors
 *    are delivered to the device (its CTRT path) and SACK latches in INTSTS1
 *    -- a device SIE always ACKs a SETUP (USB 2.0 sec 8.5.3), so no wait is
 *    modelled. With nothing attached SIGN latches instead.
 *  - `BRDYSTS` composes live: the DCP bit reflects control-IN bytes the device
 *    committed (or its status-stage ZLP after the device's CCPL), and each
 *    receive pipe's bit reflects that endpoint's committed bulk-IN staging.
 *  - The `CFIFO` port drains those stagings on reads (32-bit accesses, the
 *    HS MBW) and accumulates OUT payloads on writes; `CFIFOCTR.BVAL` commits
 *    an OUT packet over the loop (control-OUT to the DCP, bulk-OUT to the
 *    addressed endpoint) and raises the matching BEMP "transmitted" status.
 *  - `DCPCTR.CCPL` runs the status stage: an OUT ZLP after a control read
 *    (advancing the device to its read-status stage), or -- after a control
 *    write / no-data request -- arming the wait for the device firmware's
 *    CCPL, whose observation becomes the host's status-ZLP BRDY.
 *  - `DVSTCTR0.USBRST` release delivers a bus reset to the device and latches
 *    RHST = full-speed (the looped USBFS device); `SYSSTS0.LNST` reflects the
 *    device's D+ pull-up.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "board_usb_host.h"

#include <stdio.h>
#include <string.h>

#include "board_usb.h"
#include "emu_host_io_internal.h"
#include "ra8_usb_regs.h"

/* =============================================================================
 * Window geometry + model sizing.
 * =============================================================================
 */

/** @brief USBHS register-window geometry and staging sizes. */
typedef enum : uint64_t {
  k_usbhs_base      = 0x40351000UL, /**< USBHS base (HUM Ch 37, p 2059).       */
  k_usbhs_span      = 0x104UL,      /**< Window: through LPSTS at +0x102.      */
  k_usbhs_reg_words = 0x104UL / 2U, /**< 16-bit register shadow word count.    */
  k_usbhs_stage_cap = 512UL,        /**< OUT staging (one HS bulk packet).     */
  k_usbhs_mps_fall  = 64UL,         /**< DTLN clamp when no MPS is programmed. */
} usbhs_geom_t;

/** @brief DVSTCTR0 fields the model owns (HUM Ch 37.2.5). */
typedef enum : uint16_t {
  k_usbhs_dvst_rhst_fs = 0x0002U, /**< RHST = 10b: full-speed link.  */
  k_usbhs_dvst_usbrst  = 0x0040U, /**< USBRST: bus-reset signalling. */
} usbhs_dvst_t;

/** @brief DCPCTR / PIPECTR write-pulse bits that never persist in a shadow. */
typedef enum : uint16_t {
  k_usbhs_dcpctr_sureq    = 0x4000U, /**< SUREQ: launch the SETUP (pulse).      */
  k_usbhs_dcpctr_sureqclr = 0x0800U, /**< SUREQCLR: abort a SETUP (pulse).      */
  k_usbhs_dcpctr_ccpl     = 0x0004U, /**< CCPL: run the status stage.           */
  k_usbhs_ctr_sqset       = 0x0080U, /**< SQSET: force DATA1 (pulse).           */
  k_usbhs_ctr_sqclr       = 0x0100U, /**< SQCLR: force DATA0 (pulse).           */
  k_usbhs_ctr_pulse_mask  = 0x4980U, /**< SUREQ | SQCLR | SQSET (never stored). */
} usbhs_pulse_t;

/** @brief INTSTS1 host-event bits the model latches (HUM Ch 37.2.15). */
typedef enum : uint16_t {
  k_usbhs_int1_sack = 0x0010U, /**< SACK: the device ACKed the SETUP.  */
  k_usbhs_int1_sign = 0x0020U, /**< SIGN: three SETUP attempts failed. */
} usbhs_int1_t;

/** @brief SETUP-word decode + status-bit constants for the host model. */
typedef enum : uint16_t {
  k_usbhs_setup_dir_in = 0x0080U, /**< bmRequestType bit 7: device-to-host. */
  k_usbhs_dcp_bit      = 0x0001U, /**< BRDYSTS / BEMPSTS bit 0 (the DCP).   */
  k_usbhs_lnst_j_state = 0x0001U, /**< SYSSTS0.LNST: J-state (FS idle).     */
  k_usbhs_byte_mask    = 0x00FFU, /**< One-byte mask.                       */
} usbhs_bits_t;

/** @brief Small integer constants (shifts / offsets) used by the model. */
typedef enum : uint32_t {
  k_usbhs_byte_bits = 8U, /**< Bits per byte (LE packing shifts). */
} usbhs_misc_t;

/** @brief Register field masks (HUM Ch 37.2) not exported by ra8_usb_regs.h. */
typedef enum : uint16_t {
  k_usbhs_dcpmaxp_mxps  = 0x007FU, /**< DCPMAXP.MXPS: DCP max packet field.   */
  k_usbhs_pipemaxp_mxps = 0x07FFU, /**< PIPEMAXP.MXPS: pipe max packet field. */
  k_usbhs_rhst_mask     = 0x0007U, /**< DVSTCTR0.RHST: connected-speed (RO).  */
} usbhs_field_t;

/* =============================================================================
 * Model state.
 * =============================================================================
 */

/**
 * @struct usbhs_state_t
 * @brief Aggregate USBHS host-model state.
 * @details The 16-bit @c reg shadow answers every register without special
 *          semantics; the per-pipe arrays are the PIPESEL-windowed
 *          configuration; @c stage accumulates CFIFO writes until BVAL
 *          commits them over the loop.
 * @invariant @c stage_len <= ::k_usbhs_stage_cap.
 */
typedef struct {
  uint16_t reg[k_usbhs_reg_words];         /**< Plain register shadow.         */
  uint16_t pipecfg[k_ra8_usb_pipe_count];  /**< Windowed PIPECFG per pipe.     */
  uint16_t pipebuf[k_ra8_usb_pipe_count];  /**< Windowed PIPEBUF per pipe.     */
  uint16_t pipemaxp[k_ra8_usb_pipe_count]; /**< Windowed PIPEMAXP per pipe.    */
  uint16_t pipeperi[k_ra8_usb_pipe_count]; /**< Windowed PIPEPERI per pipe.    */
  uint8_t  stage[k_usbhs_stage_cap];       /**< CFIFO OUT staging buffer.      */
  uint16_t stage_len;                      /**< Staged OUT bytes.              */
  uint16_t rhst;                           /**< DVSTCTR0.RHST after bus reset. */
  bool     engaged;                        /**< Model owns the window.         */
  bool     allowed;                        /**< main.c granted the loop.       */
  bool     last_setup_in;                  /**< Last SETUP was device-to-host. */
  bool     sie_status_done;                /**< SIE ran the status (SET_ADDR). */
  bool     ccpl_armed;                     /**< Awaiting the device's CCPL.    */
  bool     status_zlp;                     /**< Status ZLP ready for the host. */
  uint32_t setups;                         /**< SETUPs delivered (report).     */
  uint32_t bulk_out;                       /**< Bulk-OUT packets (report).     */
  uint32_t bulk_in;                        /**< Bulk-IN packets (report).      */
} usbhs_state_t;

/**
 * @var s_hs
 * @brief Single USBHS host-model instance.
 * @details One controller instance exists on the RA8D2, so one model instance
 *          suffices; both engines dispatch into it through the shared MMIO
 *          hooks.
 * @note Mutated only from the ra8_emulator MMIO hooks; not for direct use.
 * @warning Reset via ::board_usb_host_init only.
 * @since 0.1.0
 */
static usbhs_state_t s_hs;

/**
 * @var s_hs_trace
 * @brief When true, each loop-cable transaction is logged to injected error sink.
 * @details Latched from the --trace flag by ::board_usb_host_init.
 * @note Read-only outside init.
 * @warning Set via ::board_usb_host_init only.
 * @since 0.1.0
 */
static bool s_hs_trace;

/* =============================================================================
 * Small helpers.
 * =============================================================================
 */

/**
 * @brief Word index into the 16-bit register shadow for a window offset.
 * @details Word index into the 16-bit register shadow for a window offset; this step is contained within the board USB host model and uses bounded caller or module-owned storage.
 * @param[in] off Register or byte offset addressed by the operation.
 * @return The usbhs word result produced by the board USB host model.
 * @retval value The operation-specific usbhs word value.
 * @pre Arguments satisfy the ranges documented for usbhs word. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board USB host model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_usbhs_word(uint64_t off)
{
  return (uint32_t)((off & ~(uint64_t)1U) / 2U);
}

/**
 * @brief Currently-selected CFIFO pipe number (CFIFOSEL.CURPIPE[3:0]).
 * @details Currently-selected cfifo pipe number (cfifosel.curpipe[3:0]); this step is contained within the board USB host model and uses bounded caller or module-owned storage.
 * @return The usbhs cur pipe result produced by the board USB host model.
 * @retval value The operation-specific usbhs cur pipe value.
 * @pre Arguments satisfy the ranges documented for usbhs cur pipe. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board USB host model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_usbhs_cur_pipe(void)
{
  const uint16_t sel = s_hs.reg[internal_usbhs_word((uint64_t)k_ra8_usb_off_cfifosel)];
  return (uint8_t)(sel & (uint16_t)k_ra8_fifosel_curpipe);
}

/**
 * @brief True when CFIFOSEL selects the write (ISEL) side of the DCP port.
 * @details True when cfifosel selects the write (isel) side of the dcp port; this step is contained within the board USB host model and uses bounded caller or module-owned storage.
 * @return The usbhs cfifo isel result produced by the board USB host model.
 * @retval true The usbhs cfifo isel condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for usbhs cfifo isel. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board USB host model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_usbhs_cfifo_isel(void)
{
  const uint16_t sel = s_hs.reg[internal_usbhs_word((uint64_t)k_ra8_usb_off_cfifosel)];
  return (sel & (uint16_t)k_ra8_fifosel_isel) != 0U;
}

/**
 * @brief Device endpoint number a host pipe addresses (PIPECFG.EPNUM).
 * @details Device endpoint number a host pipe addresses (pipecfg.epnum); this step is contained within the board USB host model and uses bounded caller or module-owned storage.
 * @param[in] pipe USB pipe index selected by the transfer.
 * @return The usbhs pipe ep result produced by the board USB host model.
 * @retval value The operation-specific usbhs pipe ep value.
 * @pre Arguments satisfy the ranges documented for usbhs pipe ep. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board USB host model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_usbhs_pipe_ep(uint8_t pipe)
{
  const uint8_t ep =
    (uint8_t)(s_hs.pipecfg[pipe % k_ra8_usb_pipe_count] & (uint16_t)k_ra8_pipecfg_epnum_mask);
  return (ep != 0U) ? ep : pipe; /* unconfigured: fall back to pipe == EP. */
}

/**
 * @brief True when a host pipe transmits (host-to-device; PIPECFG.DIR set).
 * @details True when a host pipe transmits (host-to-device; pipecfg.dir set); this step is contained within the board USB host model and uses bounded caller or module-owned storage.
 * @param[in] pipe USB pipe index selected by the transfer.
 * @return The usbhs pipe is tx result produced by the board USB host model.
 * @retval true The usbhs pipe is tx condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for usbhs pipe is tx. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board USB host model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_usbhs_pipe_is_tx(uint8_t pipe)
{
  return (s_hs.pipecfg[pipe % k_ra8_usb_pipe_count] & (uint16_t)k_ra8_pipecfg_dir_in) != 0U;
}

/**
 * @brief A pipe's effective max packet size (fallback when unprogrammed).
 * @details A pipe's effective max packet size (fallback when unprogrammed); this step is contained within the board USB host model and uses bounded caller or module-owned storage.
 * @param[in] pipe USB pipe index selected by the transfer.
 * @return The usbhs pipe mps result produced by the board USB host model.
 * @retval value The operation-specific usbhs pipe mps value.
 * @pre Arguments satisfy the ranges documented for usbhs pipe mps. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board USB host model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_usbhs_pipe_mps(uint8_t pipe)
{
  const uint16_t mps =
    (uint16_t)(s_hs.pipemaxp[pipe % k_ra8_usb_pipe_count] & (uint16_t)k_usbhs_pipemaxp_mxps);
  return (mps != 0U) ? mps : (uint16_t)k_usbhs_mps_fall;
}

/**
 * @brief The DCP's effective max packet size (fallback when unprogrammed).
 * @details The dcp's effective max packet size (fallback when unprogrammed); this step is contained within the board USB host model and uses bounded caller or module-owned storage.
 * @return The usbhs default control pipe mps result produced by the board USB host model.
 * @retval value The operation-specific usbhs default control pipe mps value.
 * @pre Arguments satisfy the ranges documented for usbhs default control pipe mps. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board USB host model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_usbhs_dcp_mps(void)
{
  const uint16_t mps = (uint16_t)(s_hs.reg[internal_usbhs_word((uint64_t)k_ra8_usb_off_dcpmaxp)] &
                                  (uint16_t)k_usbhs_dcpmaxp_mxps);
  return (mps != 0U) ? mps : (uint16_t)k_usbhs_mps_fall;
}

/* =============================================================================
 * Bus transactions (loop-cable forwarding).
 * =============================================================================
 */

/**
 * @brief Run the SETUP transaction the firmware host launched with SUREQ.
 *
 * @details Reads the USBREQ..USBLENG mirrors from the shadow and delivers the
 * packet to the looped device. A device SIE always ACKs a SETUP token, so
 * SACK latches synchronously; with no device attached (D+ pull-up absent)
 * SIGN latches instead, exactly the three-strikes failure the engine expects.
 * SET_ADDRESS reports SIE-handled: the status stage needs no device CCPL.
 *
 * @param[in,out] uc Unicorn engine (loop delivery pends the device IRQ).
 * @return Nothing.
 * @pre The model is engaged (host mode + loop granted).
 * @pre The USBREQ..USBLENG shadows hold the request to deliver.
 * @post SACK or SIGN is latched in the INTSTS1 shadow.
 * @post The per-transfer stage flags are re-armed for the new transfer.
 * @note Called from the MMIO write hook; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usbhs_do_setup(uc_engine* uc)
{
  s_hs.last_setup_in   = false;
  s_hs.sie_status_done = false;
  s_hs.ccpl_armed      = false;
  s_hs.status_zlp      = false;
  const uint32_t w1    = internal_usbhs_word((uint64_t)k_ra8_usb_off_intsts1);
  if (!board_usb_loop_attached()) {
    s_hs.reg[w1] = (uint16_t)(s_hs.reg[w1] | (uint16_t)k_usbhs_int1_sign);
    return;
  }
  const uint16_t req   = s_hs.reg[internal_usbhs_word((uint64_t)k_ra8_usb_off_usbreq)];
  const uint16_t val   = s_hs.reg[internal_usbhs_word((uint64_t)k_ra8_usb_off_usbval)];
  const uint16_t indx  = s_hs.reg[internal_usbhs_word((uint64_t)k_ra8_usb_off_usbindx)];
  const uint16_t leng  = s_hs.reg[internal_usbhs_word((uint64_t)k_ra8_usb_off_usbleng)];
  s_hs.last_setup_in   = ((req & (uint16_t)k_usbhs_setup_dir_in) != 0U);
  s_hs.sie_status_done = board_usb_loop_setup(uc, req, val, indx, leng);
  s_hs.reg[w1]         = (uint16_t)(s_hs.reg[w1] | (uint16_t)k_usbhs_int1_sack);
  s_hs.setups++;
  if (s_hs_trace) {
    (void)priv_emu_io_errf("  USBHS host    : SETUP req=0x%04X wVal=0x%04X wLen=%u%s\n",
                           (unsigned)req,
                           (unsigned)val,
                           (unsigned)leng,
                           s_hs.sie_status_done ? " (SIE status)" : "");
  }
}

/**
 * @brief Run the status stage the firmware host launched with CCPL.
 *
 * @details After a control READ the host drives an OUT ZLP: advance the
 * device to its read-status stage (its firmware then completes with CCPL)
 * and latch the host's DCP BEMP -- the ZLP "transmitted" indication its
 * best-effort wait polls. After a control WRITE / no-data request the host
 * collects the DEVICE's status ZLP: when the SIE already ran it
 * (SET_ADDRESS) the ZLP is ready now, otherwise arm the CCPL watch that
 * ::internal_usbhs_brdysts_value polls.
 *
 * @param[in,out] uc Unicorn engine (loop delivery pends the device IRQ).
 * @return Nothing.
 * @pre A SETUP for the active transfer completed (SACK observed).
 * @pre The model is engaged.
 * @post Read status: device advanced to read-status; host DCP BEMP latched.
 * @post Write / no-data status: the status-ZLP source is armed or ready.
 * @note Called from the MMIO write hook; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usbhs_do_ccpl(uc_engine* uc)
{
  const uint32_t wb = internal_usbhs_word((uint64_t)k_ra8_usb_off_bempsts);
  if (s_hs.last_setup_in) {
    board_usb_loop_status_out_zlp(uc);
    s_hs.reg[wb] = (uint16_t)(s_hs.reg[wb] | (uint16_t)k_usbhs_dcp_bit);
    return;
  }
  if (s_hs.sie_status_done) {
    s_hs.status_zlp = true; /* SET_ADDRESS: the SIE completed the transfer. */
    return;
  }
  s_hs.ccpl_armed = true; /* status ZLP follows the device firmware's CCPL. */
}

/**
 * @brief Compose the live BRDYSTS value (and poll the device's CCPL).
 *
 * @details The DCP bit reflects control-IN bytes the device committed or a
 * ready status ZLP; while the CCPL watch is armed each read polls the looped
 * device's completion so the spin inside `internal_host_dcp_in_wait` observes
 * the status ZLP the moment the device firmware finishes the request. Each
 * configured receive pipe's bit reflects its endpoint's committed bulk-IN
 * staging.
 *
 * @param[in,out] uc Unicorn engine (the CCPL poll may pend the device IRQ).
 * @return The BRDYSTS read value.
 * @retval 0 No packet is ready on the DCP or any receive pipe.
 * @pre The model is engaged.
 * @pre The loop transport is initialised (board_usb model live).
 * @post A consumed device CCPL is folded into the status-ZLP flag.
 * @post No other model state changes (compose-only otherwise).
 * @note Called from the MMIO read hook; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_usbhs_brdysts_value(uc_engine* uc)
{
  if (s_hs.ccpl_armed && board_usb_loop_take_ccpl(uc)) {
    s_hs.ccpl_armed = false;
    s_hs.status_zlp = true;
  }
  uint16_t v = 0U;
  if (s_hs.status_zlp || (board_usb_loop_ctrl_in_avail() > 0U)) {
    v |= (uint16_t)k_usbhs_dcp_bit;
  }
  for (uint8_t pipe = 1U; pipe < (uint8_t)k_ra8_usb_pipe_count; pipe++) {
    if (s_hs.pipecfg[pipe] == 0U) {
      continue;
    }
    if (internal_usbhs_pipe_is_tx(pipe)) {
      continue;
    }
    if (board_usb_loop_bulk_in_avail(internal_usbhs_pipe_ep(pipe)) > 0U) {
      v = (uint16_t)(v | (uint16_t)(1U << pipe));
    }
  }
  return v;
}

/**
 * @brief CFIFOCTR read value: FRDY plus the receive-side DTLN.
 *
 * @details FRDY is always ready (the port never stalls in the model). DTLN
 * presents the next packet's length for the selected receive source: the
 * device's control-IN staging on the DCP read window, or the addressed
 * endpoint's bulk-IN staging on a receive pipe -- clamped to the max packet
 * size so a long response drains as MPS-sized packets ending in a short one,
 * exactly the framing the polled engine's short-packet logic expects.
 *
 * @return The composed CFIFOCTR value.
 * @retval k_ra8_fifoctr_frdy No receive bytes pending (DTLN = 0).
 * @pre The model is engaged.
 * @pre CFIFOSEL selects the pipe the caller is transferring on.
 * @post No model state changes (compose-only).
 * @post DTLN <= the selected pipe's max packet size.
 * @note Called from the MMIO read hook; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_usbhs_cfifoctr_value(void)
{
  const uint8_t pipe  = internal_usbhs_cur_pipe();
  uint16_t      avail = 0U;
  uint16_t      mps   = (uint16_t)k_usbhs_mps_fall;
  if (pipe == 0U) {
    if (!internal_usbhs_cfifo_isel()) {
      avail = board_usb_loop_ctrl_in_avail();
      mps   = internal_usbhs_dcp_mps();
    }
  } else if (!internal_usbhs_pipe_is_tx(pipe)) {
    avail = board_usb_loop_bulk_in_avail(internal_usbhs_pipe_ep(pipe));
    mps   = internal_usbhs_pipe_mps(pipe);
  } else {
    avail = 0U;
  }
  const uint16_t dtln = (avail < mps) ? avail : mps;
  return (uint16_t)((uint16_t)k_ra8_fifoctr_frdy | (dtln & (uint16_t)k_ra8_fifoctr_dtln));
}

/**
 * @brief Service a CFIFO data-port read: drain receive bytes, packed LE.
 *
 * @details Pops up to @p size bytes from the selected receive source (DCP
 * control-IN or a receive pipe's bulk-IN) and packs them little-endian --
 * the 32-bit MBW drain the HS engine performs, including its final
 * partial-word read where only the low DTLN-remainder bytes are meaningful.
 *
 * @param[in,out] uc   Unicorn engine (a drained pipe raises the device BEMP).
 * @param[in]     size Access width in bytes (1 / 2 / 4).
 * @return The packed read value.
 * @retval 0 Nothing staged for the selected source.
 * @pre The model is engaged and CFIFOSEL selects the transfer's pipe.
 * @pre The caller observed FRDY (always ready here).
 * @post Up to @p size receive bytes are consumed from the staging.
 * @post A fully-drained bulk staging raises the device's BEMP for the pipe.
 * @note Called from the MMIO read hook; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_usbhs_cfifo_read(uc_engine* uc, unsigned size)
{
  uint8_t        bytes[sizeof(uint32_t)] = {};
  const uint16_t cap  = (size < sizeof(bytes)) ? (uint16_t)size : (uint16_t)sizeof(bytes);
  const uint8_t  pipe = internal_usbhs_cur_pipe();
  uint16_t       got  = 0U;
  if (pipe == 0U) {
    got = board_usb_loop_ctrl_in_read(bytes, cap);
  } else if (!internal_usbhs_pipe_is_tx(pipe)) {
    got = board_usb_loop_bulk_in_read(uc, internal_usbhs_pipe_ep(pipe), bytes, cap);
    if (got > 0U) {
      s_hs.bulk_in++;
    }
  } else {
    got = 0U;
  }
  (void)got; /* short reads leave the upper packed bytes zero (don't-care). */
  uint64_t v = 0U;
  for (uint32_t i = 0U; i < (uint32_t)cap; i++) {
    v |= ((uint64_t)bytes[i]) << (i * (uint32_t)k_usbhs_byte_bits);
  }
  return v;
}

/**
 * @brief Append CFIFO data-port write bytes to the OUT staging buffer.
 *
 * @details Accumulates the little-endian bytes of each port write (32-bit
 * head words at CFIFO+0, the 16-bit CFIFOH / 8-bit CFIFOHH residual aliases
 * at +2 / +3) until BVAL commits the packet over the loop.
 *
 * @param[in] value The written value (little-endian byte order).
 * @param[in] size  Access width in bytes (1 / 2 / 4).
 * @return Nothing.
 * @pre The model is engaged.
 * @pre The staging has room (bounded by ::k_usbhs_stage_cap).
 * @post Up to @p size bytes are appended to the staging.
 * @post Overflow bytes beyond the staging capacity are dropped.
 * @note Called from the MMIO write hook; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usbhs_cfifo_write(uint64_t value, unsigned size)
{
  for (uint32_t i = 0U; (i < size) && (s_hs.stage_len < (uint16_t)k_usbhs_stage_cap); i++) {
    s_hs.stage[s_hs.stage_len] =
      (uint8_t)((value >> (i * (uint32_t)k_usbhs_byte_bits)) & (uint64_t)k_usbhs_byte_mask);
    s_hs.stage_len++;
  }
}

/**
 * @brief Apply a CFIFOCTR write: BCLR discards, BVAL commits over the loop.
 *
 * @details BCLR on a write window drops the staged OUT bytes; on a receive
 * window it releases the pending source (control-IN remainder plus the
 * status-ZLP flag on the DCP, the endpoint's bulk-IN staging on a pipe).
 * BVAL commits the staged bytes: a control-OUT data packet to the device's
 * DCP, or a bulk-OUT packet to the addressed endpoint -- each latching the
 * matching BEMP "transmitted" status the engine's post-commit wait polls.
 * A zero-length DCP BVAL (the read-status OUT ZLP) is inert here: the CCPL
 * pulse that follows runs that stage.
 *
 * @param[in,out] uc    Unicorn engine (loop delivery pends the device IRQ).
 * @param[in]     value The written CFIFOCTR value.
 * @return Nothing.
 * @pre The model is engaged and CFIFOSEL selects the transfer's pipe.
 * @pre Staged bytes (for BVAL) were pushed via the CFIFO port.
 * @post BCLR: the selected side's staging is empty.
 * @post BVAL: the packet is delivered and the staging reset.
 * @note Called from the MMIO write hook; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usbhs_cfifoctr_write(uc_engine* uc, uint16_t value)
{
  const uint8_t pipe = internal_usbhs_cur_pipe();
  if ((value & (uint16_t)k_ra8_fifoctr_bclr) != 0U) {
    if (pipe == 0U) {
      if (internal_usbhs_cfifo_isel()) {
        s_hs.stage_len = 0U;
      } else {
        board_usb_loop_ctrl_in_flush();
        s_hs.status_zlp = false;
      }
    } else if (internal_usbhs_pipe_is_tx(pipe)) {
      s_hs.stage_len = 0U;
    } else {
      board_usb_loop_bulk_in_flush(internal_usbhs_pipe_ep(pipe));
    }
  }
  if ((value & (uint16_t)k_ra8_fifoctr_bval) != 0U) {
    const uint32_t wb = internal_usbhs_word((uint64_t)k_ra8_usb_off_bempsts);
    if ((pipe == 0U) && internal_usbhs_cfifo_isel() && (s_hs.stage_len > 0U)) {
      board_usb_loop_ctrl_out(uc, s_hs.stage, s_hs.stage_len);
      s_hs.reg[wb]   = (uint16_t)(s_hs.reg[wb] | (uint16_t)k_usbhs_dcp_bit);
      s_hs.stage_len = 0U;
    } else if ((pipe != 0U) && (s_hs.stage_len > 0U)) {
      board_usb_loop_bulk_out(uc, internal_usbhs_pipe_ep(pipe), s_hs.stage, s_hs.stage_len);
      s_hs.reg[wb]   = (uint16_t)(s_hs.reg[wb] | (uint16_t)(1U << pipe));
      s_hs.stage_len = 0U;
      s_hs.bulk_out++;
    }
  }
}

/**
 * @brief Apply a DVSTCTR0 write: bus-reset delivery + RHST latch.
 *
 * @details The read-only RHST field is masked out of the stored value. On the
 * USBRST falling edge (reset released) with a device attached, the bus reset
 * is delivered to the looped device and RHST latches full-speed -- the looped
 * peer is the USBFS controller, so the link trains to FS.
 *
 * @param[in,out] uc    Unicorn engine (loop delivery pends the device IRQ).
 * @param[in]     value The written DVSTCTR0 value.
 * @return Nothing.
 * @pre The model is engaged.
 * @pre The stored shadow holds the previous DVSTCTR0 control bits.
 * @post The shadow holds @p value minus RHST; RHST reflects the reset result.
 * @post On the reset release with a device attached the device re-defaults.
 * @note Called from the MMIO write hook; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usbhs_dvstctr_write(uc_engine* uc, uint16_t value)
{
  const uint32_t w   = internal_usbhs_word((uint64_t)k_ra8_usb_off_dvstctr0);
  const uint16_t old = s_hs.reg[w];
  s_hs.reg[w]        = (uint16_t)(value & (uint16_t)~(uint16_t)k_usbhs_rhst_mask);
  const bool was_rst = (old & (uint16_t)k_usbhs_dvst_usbrst) != 0U;
  const bool is_rst  = (value & (uint16_t)k_usbhs_dvst_usbrst) != 0U;
  if (was_rst && !is_rst) {
    if (board_usb_loop_attached()) {
      s_hs.rhst = (uint16_t)k_usbhs_dvst_rhst_fs;
      board_usb_loop_bus_reset(uc);
    } else {
      s_hs.rhst = 0U;
    }
  }
}

/**
 * @brief Apply a DCPCTR write: PID persists, SUREQ / CCPL pulses execute.
 *
 * @details The write-pulse bits (SUREQ / SUREQCLR / SQSET / SQCLR) never
 * persist -- SUREQ launches the SETUP transaction and self-clears (a read
 * never observes it mid-flight; the model runs it synchronously). A CCPL
 * rising edge runs the status stage for the active transfer direction.
 *
 * @param[in,out] uc    Unicorn engine (transactions pend the device IRQ).
 * @param[in]     value The written DCPCTR value.
 * @return Nothing.
 * @pre The model is engaged.
 * @pre For SUREQ, the USBREQ..USBLENG mirrors hold the request.
 * @post The shadow holds the persistent bits (PID / CCPL) of @p value.
 * @post Any launched transaction's side effects are applied.
 * @note Called from the MMIO write hook; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usbhs_dcpctr_write(uc_engine* uc, uint16_t value)
{
  const uint32_t w   = internal_usbhs_word((uint64_t)k_ra8_usb_off_dcpctr);
  const uint16_t old = s_hs.reg[w];
  if (((value & (uint16_t)k_usbhs_dcpctr_sureq) != 0U) &&
      ((old & (uint16_t)k_usbhs_dcpctr_sureq) == 0U)) {
    internal_usbhs_do_setup(uc);
  }
  if (((value & (uint16_t)k_usbhs_dcpctr_ccpl) != 0U) &&
      ((old & (uint16_t)k_usbhs_dcpctr_ccpl) == 0U)) {
    internal_usbhs_do_ccpl(uc);
  }
  s_hs.reg[w] = (uint16_t)(value & (uint16_t)~(uint16_t)((uint16_t)k_usbhs_ctr_pulse_mask |
                                                         (uint16_t)k_usbhs_dcpctr_sureqclr));
}

/* =============================================================================
 * Engaged read / write dispatch.
 * =============================================================================
 */

/**
 * @brief Read one USBHS register (engaged path).
 *
 * @details Registers with live semantics are composed (SYSSTS0 line state,
 * PLLSTA lock, DVSTCTR0 RHST, CFIFO / CFIFOCTR receive state, BRDYSTS);
 * everything else answers from the plain shadow -- including the
 * PIPESEL-windowed configuration, which reads from the selected pipe's slot.
 *
 * @param[in,out] uc   Unicorn engine (receive paths pend the device IRQ).
 * @param[in]     off  Byte offset into the USBHS window.
 * @param[in]     size Access width in bytes.
 * @return The register value -- 64-bit wide so the CFIFO port's 32-bit MBW
 *         drains are returned untruncated (every other register is 16-bit).
 * @retval 0 Unwritten plain registers (power-on shadow).
 * @pre The model is engaged.
 * @pre @p off < ::k_usbhs_span.
 * @post Receive-side reads consume staged bytes (CFIFO only).
 * @post All other reads leave the model unchanged.
 * @note Called from the MMIO read hook; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_usbhs_reg_read(uc_engine* uc, uint64_t off, unsigned size)
{
  const uint8_t sel = (uint8_t)(s_hs.reg[internal_usbhs_word((uint64_t)k_ra8_usb_off_pipesel)] &
                                (uint16_t)k_ra8_fifosel_curpipe);
  switch ((uint16_t)off) {
    case (uint16_t)k_ra8_usb_off_syssts0:
      return (board_usb_loop_attached()) ? (uint64_t)k_usbhs_lnst_j_state : 0U;
    case (uint16_t)k_ra8_usbhs_off_pllsta:
      return (uint16_t)k_ra8_pllsta_plllock;
    case (uint16_t)k_ra8_usb_off_dvstctr0:
      return (uint16_t)(s_hs.reg[internal_usbhs_word(off)] | s_hs.rhst);
    case (uint16_t)k_ra8_usb_off_cfifo:
      return internal_usbhs_cfifo_read(uc, size); /* full width: 32-bit MBW drains. */
    case (uint16_t)k_ra8_usb_off_cfifoctr:
      return internal_usbhs_cfifoctr_value();
    case (uint16_t)k_ra8_usb_off_brdysts:
      return internal_usbhs_brdysts_value(uc);
    case (uint16_t)k_ra8_usb_off_nrdysts:
      return 0U;
    case (uint16_t)k_ra8_usb_off_pipecfg:
      return s_hs.pipecfg[sel % k_ra8_usb_pipe_count];
    case (uint16_t)k_ra8_usb_off_pipebuf:
      return s_hs.pipebuf[sel % k_ra8_usb_pipe_count];
    case (uint16_t)k_ra8_usb_off_pipemaxp:
      return s_hs.pipemaxp[sel % k_ra8_usb_pipe_count];
    case (uint16_t)k_ra8_usb_off_pipeperi:
      return s_hs.pipeperi[sel % k_ra8_usb_pipe_count];
    default:
      return s_hs.reg[internal_usbhs_word(off)];
  }
}

/**
 * @brief Write one USBHS register (engaged path).
 *
 * @details Dispatches the registers with live semantics (CFIFO staging,
 * CFIFOCTR commits, DCPCTR pulses, DVSTCTR0 reset edges, W0C statuses,
 * the PIPESEL-windowed configuration and the PIPECTR pulse bits); everything
 * else stores to the plain shadow.
 *
 * @param[in,out] uc    Unicorn engine (transactions pend the device IRQ).
 * @param[in]     off   Byte offset into the USBHS window.
 * @param[in]     size  Access width in bytes.
 * @param[in]     value64 The written value -- 64-bit wide so the CFIFO port's
 *                      32-bit MBW fills arrive untruncated (every other
 *                      register consumes its low 16 bits).
 * @return Nothing.
 * @pre The model is engaged.
 * @pre @p off < ::k_usbhs_span.
 * @post The write's semantics are applied (shadow / staging / transaction).
 * @post Pulse bits never persist in the stored shadows.
 * @note Called from the MMIO write hook; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_usbhs_reg_write(uc_engine* uc, uint64_t off, unsigned size, uint64_t value64)
{
  const uint16_t value = (uint16_t)value64;
  const uint8_t  sel   = (uint8_t)(s_hs.reg[internal_usbhs_word((uint64_t)k_ra8_usb_off_pipesel)] &
                                   (uint16_t)k_ra8_fifosel_curpipe);
  /* The CFIFO port spans +0x14..+0x17: 32-bit head words at +0x14, the
   * CFIFOH (+0x16) / CFIFOHH (+0x17) residual aliases for the 2- / 1-byte
   * tails. All append to the same OUT staging. */
  if ((off >= (uint64_t)k_ra8_usb_off_cfifo) && (off < (uint64_t)(k_ra8_usb_off_cfifo + 4U))) {
    internal_usbhs_cfifo_write(value64, size); /* full width: 32-bit MBW fills. */
    return;
  }
  switch ((uint16_t)off) {
    case (uint16_t)k_ra8_usb_off_cfifoctr:
      internal_usbhs_cfifoctr_write(uc, value);
      return;
    case (uint16_t)k_ra8_usb_off_dcpctr:
      internal_usbhs_dcpctr_write(uc, value);
      return;
    case (uint16_t)k_ra8_usb_off_dvstctr0:
      internal_usbhs_dvstctr_write(uc, value);
      return;
    case (uint16_t)k_ra8_usb_off_intsts1:
    case (uint16_t)k_ra8_usb_off_bempsts:
      /* W0C: a written 0 clears, a written 1 preserves. */
      s_hs.reg[internal_usbhs_word(off)] &= value;
      return;
    case (uint16_t)k_ra8_usb_off_brdysts:
      /* Live bits are composed; only the sticky status-ZLP flag is W0C. */
      if ((value & (uint16_t)k_usbhs_dcp_bit) == 0U) {
        s_hs.status_zlp = false;
      }
      return;
    case (uint16_t)k_ra8_usb_off_nrdysts:
      return; /* never latched -- nothing to clear. */
    case (uint16_t)k_ra8_usb_off_pipecfg:
      s_hs.pipecfg[sel % k_ra8_usb_pipe_count] = value;
      return;
    case (uint16_t)k_ra8_usb_off_pipebuf:
      s_hs.pipebuf[sel % k_ra8_usb_pipe_count] = value;
      return;
    case (uint16_t)k_ra8_usb_off_pipemaxp:
      s_hs.pipemaxp[sel % k_ra8_usb_pipe_count] = value;
      return;
    case (uint16_t)k_ra8_usb_off_pipeperi:
      s_hs.pipeperi[sel % k_ra8_usb_pipe_count] = value;
      return;
    default:
      break;
  }
  if ((off >= (uint64_t)k_ra8_usb_off_pipectr) &&
      (off < (uint64_t)(k_ra8_usb_off_pipectr +
                        ((uint64_t)k_ra8_usb_pipectr_count * sizeof(uint16_t))))) {
    /* PIPECTR: PID persists; the SQSET / SQCLR pulses never store. */
    s_hs.reg[internal_usbhs_word(off)] =
      (uint16_t)(value & (uint16_t)~(uint16_t)k_usbhs_ctr_pulse_mask);
    return;
  }
  s_hs.reg[internal_usbhs_word(off)] = value;
}

/* =============================================================================
 * Public lifecycle + MMIO entry points.
 * =============================================================================
 */

void board_usb_host_init(bool trace)
{
  s_hs       = (usbhs_state_t){};
  s_hs_trace = trace;
}

void board_usb_host_set_allowed(bool allowed)
{
  s_hs.allowed = allowed;
}

uint64_t board_usb_host_read(uc_engine* uc, uint64_t addr, unsigned size, bool* handled)
{
  if (!s_hs.engaged || (addr < (uint64_t)k_usbhs_base) ||
      (addr >= ((uint64_t)k_usbhs_base + (uint64_t)k_usbhs_span))) {
    *handled = false;
    return 0U;
  }
  *handled = true;
  return internal_usbhs_reg_read(uc, addr - (uint64_t)k_usbhs_base, size);
}

void board_usb_host_write(uc_engine* uc,
                          uint64_t   addr,
                          unsigned   size,
                          uint64_t   value,
                          bool*      handled)
{
  if ((addr < (uint64_t)k_usbhs_base) ||
      (addr >= ((uint64_t)k_usbhs_base + (uint64_t)k_usbhs_span))) {
    *handled = false;
    return;
  }
  const uint64_t off = addr - (uint64_t)k_usbhs_base;
  if (!s_hs.engaged) {
    /* Dormant: watch only the SYSCFG host-role select. The write still
     * reports unhandled so the sparse fallback records it and every app that
     * never engages keeps its exact current behaviour. */
    *handled = false;
    if (s_hs.allowed && (off == (uint64_t)k_ra8_usb_off_syscfg) &&
        (((uint16_t)value & (uint16_t)(1U << k_ra8_syscfg_bit_dcfm)) != 0U)) {
      s_hs.engaged                                                  = true;
      s_hs.reg[internal_usbhs_word((uint64_t)k_ra8_usb_off_syscfg)] = (uint16_t)value;
      board_usb_loop_latch();
      (void)priv_emu_io_errf(
        "  USBHS host    : host mode engaged (SYSCFG.DCFM) -- self-loop to USBFS\n");
      *handled = true;
    }
    return;
  }
  *handled = true;
  internal_usbhs_reg_write(uc, off, size, value);
}

void board_usb_host_report(void)
{
  if (!s_hs.engaged) {
    return;
  }
  (void)priv_emu_io_errf(
    "  USBHS host    : %u SETUP(s), %u bulk-OUT commit(s), %u bulk-IN drain(s)\n",
    s_hs.setups,
    s_hs.bulk_out,
    s_hs.bulk_in);
}
