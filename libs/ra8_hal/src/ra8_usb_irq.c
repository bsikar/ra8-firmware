/**
 * @file ra8_usb_irq.c
 * @brief USB IRQ dispatch / event callbacks / power + host-mode bring-up
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Two cohesive groups split out of ``ra8_usb.c`` to keep every translation
 * unit under the 1000-line cap:
 *
 *  - The interrupt-delivery + power surface: the per-controller event
 *    callback table, ``ra8_usb_dispatch`` (W0C-correct INTSTS0 ack +
 *    handler fan-out), ``ra8_usb_intsts0_snapshot``, and the
 *    module-stop enter / exit helpers.
 *  - The host-mode lifecycle bring-up (``ra8_usb_host_init`` /
 *    ``ra8_usb_host_deinit`` / ``ra8_usb_host_bus_reset`` /
 *    ``ra8_usb_host_set_uact`` / ``ra8_usb_host_setup_request``), peer of
 *    the device-mode lifecycle in ``ra8_usb_device.c``.
 *
 * The shared register helpers and the USBHS PHY bring-up it calls are
 * declared in ``ra8_usb_internal.h``. Modelled on FSP
 * ``r_usb_hreg_access.c``; no FSP source ships in this tree.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_internal.h"
#include "ra8_usb_regs.h"

static const char* s_tag = "USB";

/* =============================================================================
 * IRQ delivery + power
 * =============================================================================
 */

/* Per-controller callbacks. Two slots so USBFS and USBHS can have
 * independent event handlers -- this is what lets one firmware image
 * run the DCD bridge on one controller and a different driver (e.g.
 * bare-CDC) on the other. */
typedef enum : uint8_t {
  k_ra8_usb_cb_slot_fs = 0U, /**< RA8 USB cb slot fs. */
  k_ra8_usb_cb_slot_hs = 1U, /**< RA8 USB cb slot hs. */
  k_ra8_usb_cb_slot_n  = 2U, /**< RA8 USB cb slot n.  */
} ra8_usb_cb_slot_t;

static ra8_usb_event_fn_t s_usb_fn[k_ra8_usb_cb_slot_n];
static void*              s_usb_ctx[k_ra8_usb_cb_slot_n];

/**
 * @brief Map a USB speed enum to the per-controller callback slot index.
 *
 * @details Two slots exist: ``k_ra8_usb_cb_slot_fs`` for the USBFS
 * controller and ``k_ra8_usb_cb_slot_hs`` for the USBHS controller.
 * Every code path that indexes ``s_usb_fn[]`` or ``s_usb_ctx[]``
 * goes through this helper so we never confuse the two.
 *
 * @param[in] speed Controller selector (FS or HS).
 *
 * @return Slot index in ``s_usb_fn[]`` / ``s_usb_ctx[]``.
 * @retval k_ra8_usb_cb_slot_hs ``speed == k_ra8_usb_speed_hs``.
 * @retval k_ra8_usb_cb_slot_fs Any other speed value (FS default).
 *
 * @pre ``speed`` is a valid ``ra8_usb_speed_t`` enum.
 * @pre s_usb_fn / s_usb_ctx storage is in scope.
 * @post No global state is touched; the helper is pure.
 * @post Result is always < ``k_ra8_usb_cb_slot_n``.
 *
 * @note Pure / thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_cb_slot(ra8_usb_speed_t speed)
{
  return (speed == k_ra8_usb_speed_hs) ? (uint8_t)k_ra8_usb_cb_slot_hs
                                       : (uint8_t)k_ra8_usb_cb_slot_fs;
}

/**
 * @brief Implementation of `ra8_usb_attach_handler()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] fn See implementation.
 * @param[in] ctx See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_attach_handler(ra8_usb_speed_t speed, ra8_usb_event_fn_t fn, void* ctx)
{
  const uint8_t slot = internal_cb_slot(speed);
  s_usb_fn[slot]     = fn;
  s_usb_ctx[slot]    = ctx;
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_dispatch()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_ISR_SAFE
void ra8_usb_dispatch(ra8_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) { /* GCOVR_EXCL_BR_LINE -- speeds always valid */
    return;             /* GCOVR_EXCL_LINE                           */
  }
  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1986.
   *
   * INTSTS0 is W0C: writing 0 to a bit clears it, writing 1 has no
   * effect. The previous mask-and-back pattern
   *   INTSTS0 = mask & ~ack_bits
   * unconditionally writes 0 to every bit in ack_bits, even bits that
   * were 0 in the snapshot. That means a CTRT (or DVST) edge that
   * asserts in HW *between* the read of `mask` and the write of the
   * ack pattern is silently cleared before the dispatcher can act on
   * it. With a free-running polled worker (1.7M ticks/sec) and SETUP
   * tokens arriving at ~1 ms cadence, almost every CTRT edge falls
   * inside this race window, which is why s_ctrt_irq_count stayed at
   * 0 even though DVST was caught (DVST edges hold longer because the
   * device-state remains in default until the next reset).
   *
   * Correct W0C ack: write 0 ONLY to the bits we observed set, and 1
   * (no-op) everywhere else. ``~(mask & ack_bits)`` does exactly that:
   * for any bit not in ack_bits, we write 1 (preserve); for an
   * ack_bits position that was 0 in the snapshot, we write 1 (no-op,
   * does not clobber a freshly-set edge); for an ack_bits position
   * that was 1 in the snapshot, we write 0 (clear).
   *
   * VALID is intentionally NOT in ack_bits -- the SETUP handlers
   * (ra8_usb_read_setup_if_valid / _unconditional) drain
   * USBREQ/USBVAL/USBINDX/USBLENG and clear VALID themselves.
   * CTSQ/DVSQ are read-only fields. */
  const uint16_t mask = reg->INTSTS0;

  const uint16_t ack_bits = (uint16_t)((1U << k_ra8_int0_bit_ctrt) | (1U << k_ra8_int0_bit_dvst) |
                                       (1U << k_ra8_int0_bit_bemp) | (1U << k_ra8_int0_bit_brdy) |
                                       (1U << k_ra8_int0_bit_nrdy) | (1U << k_ra8_int0_bit_vbse) |
                                       (1U << k_ra8_int0_bit_rsme) | (1U << k_ra8_int0_bit_sofr));
  reg->INTSTS0            = (uint16_t)~(uint16_t)(mask & ack_bits);

  const uint8_t            slot = internal_cb_slot(speed);
  const ra8_usb_event_fn_t fn   = s_usb_fn[slot];
  void* const              ctx  = s_usb_ctx[slot];
  if (fn != nullptr) {
    fn(ctx, speed, mask);
  }
}

/**
 * @brief Implementation of `ra8_usb_intsts0_snapshot()`.
 * @details Pure MMIO read; no INTSTS0 bits are modified.
 * @param[in] speed See header.
 * @return INTSTS0 raw value.
 * @retval 0 Invalid speed or controller not powered.
 * @pre Module state is consistent.
 * @pre Controller register window is mapped.
 * @post No bits in INTSTS0 modified.
 * @post Caller-visible state matches the documented contract.
 * @note Safe to call from any context.
 * @since 0.1.0
 */
uint16_t ra8_usb_intsts0_snapshot(ra8_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return 0U;
  }
  return reg->INTSTS0;
}

/**
 * @brief Implementation of `ra8_usb_enter_stop()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_enter_stop(ra8_usb_speed_t speed)
{
  if ((speed != k_ra8_usb_speed_fs) && (speed != k_ra8_usb_speed_hs)) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_disable(internal_mstp(speed));
}

/**
 * @brief Implementation of `ra8_usb_exit_stop()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_exit_stop(ra8_usb_speed_t speed)
{
  if ((speed != k_ra8_usb_speed_fs) && (speed != k_ra8_usb_speed_hs)) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_enable(internal_mstp(speed));
}
/* =============================================================================
 * Host-mode bring-up (peer of the device-mode lifecycle above)
 *
 * Mirrors FSP's `r_usb_basic/src/hw/r_usb_hreg_access.c` host bits.
 * Device and host share the same register block; the only mode-bit
 * differences live inside SYSCFG (DCFM/DRPD vs DPRPU) and DVSTCTR0
 * (UACT / USBRST, host-only).
 * =============================================================================
 */

/**
 * @enum ra8_usb_dvstctr_bit_t
 * @brief DVSTCTR0 host-mode bit positions (HUM Ch 36.2.5 "DVSTCTR0").
 *
 * @details Sourced from CMSIS `R_USB_FS0_DVSTCTR0_*_Pos` in
 * `R7KA8D2KF_core0.h` (lines 71382-71389) and confirmed for the HS
 * instance (`R_USB_HS0_DVSTCTR0_*_Pos`, lines 75228-75235).
 */
typedef enum : uint8_t {
  k_ra8_dvstctr_bit_uact   = 4U, /**< Bus enable (host SOF generation). */
  k_ra8_dvstctr_bit_vbusen = 9U, /**< External VBUS-switch enable (HS). */
  k_ra8_dvstctr_bit_resume = 5U, /**< Resume signal output (host).      */
  k_ra8_dvstctr_bit_usbrst = 6U, /**< Bus reset signal (host).          */
  k_ra8_dvstctr_bit_rwupe  = 7U, /**< Remote-wake detect enable (host). */
} ra8_usb_dvstctr_bit_t;

/**
 * @brief Build the host-mode SYSCFG word.
 *
 * @details Sets SCKE | DCFM | DRPD | USBE; adds HSE for the HS
 * instance. DPRPU is intentionally not set (device-mode pull-up).
 *
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_host_syscfg_word(ra8_usb_speed_t speed)
{
  uint16_t syscfg = (uint16_t)(1U << k_ra8_syscfg_bit_scke);
  syscfg          = (uint16_t)(syscfg | (uint16_t)(1U << k_ra8_syscfg_bit_dcfm));
  syscfg          = (uint16_t)(syscfg | (uint16_t)(1U << k_ra8_syscfg_bit_drpd));
  syscfg          = (uint16_t)(syscfg | (uint16_t)(1U << k_ra8_syscfg_bit_usbe));
  if (speed == k_ra8_usb_speed_hs) {
    syscfg = (uint16_t)(syscfg | (uint16_t)(1U << k_ra8_syscfg_bit_hse));
  }
  return syscfg;
}

/**
 * @brief Program the host-mode FIFO / DCP / interrupt-enable defaults.
 *
 * @details Shared tail of host bring-up for both controller instances:
 * 16-bit FIFO port widths (the per-access width is re-selected by
 * ::internal_select_cfifo), DCP defaults with a 64-byte max packet,
 * USBADDR=0 (newly attached devices answer at the default address), and
 * the host interrupt-enable mask (per-pipe ENB registers stay clear;
 * the transfer engines arm exactly what they wait on).
 *
 * @param[in] reg Selected controller register block.
 * @pre SYSCFG already carries the host role (DCFM | DRPD | USBE).
 * @pre Single-threaded init context.
 * @post FIFO selects, DCP, USBADDR and INTENB0/1 hold the defaults.
 * @post BRDYENB / NRDYENB / BEMPENB are cleared.
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_host_init_defaults(volatile r_usb_regs_t* reg)
{
  /* HUM Ch 36.2.7 "CFIFOSEL : CFIFO Port Select Register", p 1976 */
  reg->CFIFOSEL  = k_ra8_fifosel_mbw_16;
  reg->D0FIFOSEL = k_ra8_fifosel_mbw_16;
  reg->D1FIFOSEL = k_ra8_fifosel_mbw_16;

  /* HUM Ch 36.2.20 "DCPMAXP : DCP Max Packet Size Register", p 1999 */
  reg->DCPCFG  = 0U;
  reg->DCPMAXP = k_ra8_usb_dcp_max_packet;
  reg->DCPCTR  = 0U;

  /* HUM Ch 36.2.16 "USBADDR : USB Address Register", p 1994 -- target
   * device address for the host's outgoing tokens. Default to 0
   * (newly-attached devices respond at address 0). */
  reg->USBADDR = 0U;

  /* HUM Ch 36.2.10 "INTENB0 : Interrupt Enable Register 0", p 1980 */
  reg->INTENB0 = (uint16_t)((1U << k_ra8_int0_bit_bemp) | (1U << k_ra8_int0_bit_brdy) |
                            (1U << k_ra8_int0_bit_nrdy) | (1U << k_ra8_int0_bit_ctrt) |
                            (1U << k_ra8_int0_bit_dvst) | (1U << k_ra8_int0_bit_vbse));
  reg->INTENB1 = 0U;
  reg->BRDYENB = 0U;
  reg->NRDYENB = 0U;
  reg->BEMPENB = 0U;
}

/**
 * @brief Bring the USBHS instance up in HOST role (PHY + SYSCFG bits).
 *
 * @details The HS PHY needs the full UTMI bring-up (CLKSEL, DIRPD and
 * PLLRESET release, SUSPENDM, PLLLOCK) before the module can operate;
 * reuse the device-mode sequence, which ends with USBE=1 and the
 * device-polarity DRPD=0. Then flip the role bits: HUM Ch 37.2.1
 * requires DCFM to be changed while USBE=0, so drop USBE, set DCFM
 * (host controller) + DRPD (host pull-downs) + CNEN, and re-enable.
 * CNEN (single-ended receiver enable) is required for the HS PHY to
 * report line state / attach at all -- without it LNST reads SE0
 * forever and no ATTCH ever latches (HUM Ch 37.2.1 p 2062).
 *
 * @param[in] reg HS register block (must be the USBHS instance).
 * @return Passthrough from the PHY bring-up.
 * @retval k_ra8_ok             PHY locked; SYSCFG carries the host role.
 * @retval k_ra8_err_hw_timeout The UTMI PLL never locked.
 * @pre MSTPB12 is ungated and USB60CLK (PLL2P/4) is running.
 * @pre Single-threaded init context.
 * @post SYSCFG = HSE | DCFM | DRPD | CNEN | USBE with the PHY powered.
 * @post The caller still programs DVSTCTR0 / FIFO / DCP defaults.
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_host_hs_bringup(volatile r_usb_regs_t* reg)
{
  const ra8_err_t phy_err = internal_usbhs_phy_bringup(reg);
  RA8_RETURN_ON_ERROR(phy_err, s_tag, "host_init: HS PHY bring-up"); /* GCOVR_EXCL_BR_LINE */
  internal_rmw16(&reg->SYSCFG, 0U, (uint16_t)(1U << k_ra8_syscfg_bit_usbe));
  internal_rmw16(&reg->SYSCFG,
                 (uint16_t)((uint16_t)(1U << k_ra8_syscfg_bit_dcfm) |
                            (uint16_t)((uint16_t)(1U << k_ra8_syscfg_bit_drpd) |
                                       (uint16_t)(1U << k_ra8_syscfg_bit_cnen))),
                 0U);
  internal_rmw16(&reg->SYSCFG, (uint16_t)(1U << k_ra8_syscfg_bit_usbe), 0U);
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_host_init()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_host_init(ra8_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  const ra8_err_t mst_err = ra8_mstp_enable(internal_mstp(speed));
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "host_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  if (speed == k_ra8_usb_speed_hs) {
    const ra8_err_t hs_err = internal_host_hs_bringup(reg);
    RA8_RETURN_ON_ERROR(hs_err, s_tag, "host_init: HS bring-up"); /* GCOVR_EXCL_BR_LINE */
  } else {
    /* HUM Ch 36.2.1 "SYSCFG : System Configuration Control Register", p 1967 */
    reg->SYSCFG = internal_host_syscfg_word(speed);
  }

  /* HUM Ch 36.2.5 "DVSTCTR0 : Device State Control Register 0", p 1971 */
  reg->DVSTCTR0 = 0U;
  if (speed == k_ra8_usb_speed_hs) {
    /* HUM Ch 37.2.5 DVSTCTR0.VBUSEN: the USBHS jack's external VBUS
     * switch is driven by this bit (FSP hw_usb_hmodule_init); without
     * it an attached device never powers and LNST stays SE0. */
    internal_rmw16(&reg->DVSTCTR0, (uint16_t)(1U << k_ra8_dvstctr_bit_vbusen), 0U);
  }

  internal_host_init_defaults(reg);

  ra8_log_info_val(s_tag, "usb host init speed", (uint32_t)speed);
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_host_deinit()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_host_deinit(ra8_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 36.2.5 "DVSTCTR0 : Device State Control Register 0", p 1971 */
  reg->DVSTCTR0 = 0U;
  reg->INTENB0  = 0U;
  reg->INTENB1  = 0U;
  reg->BRDYENB  = 0U;
  reg->NRDYENB  = 0U;
  reg->BEMPENB  = 0U;
  reg->SYSCFG   = 0U;
  return ra8_mstp_disable(internal_mstp(speed));
}

/**
 * @brief Implementation of `ra8_usb_host_bus_reset()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] assert_reset See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_host_bus_reset(ra8_usb_speed_t speed, bool assert_reset)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 36.2.5 "DVSTCTR0 : Device State Control Register 0", p 1971 */
  const uint16_t rst_bit  = (uint16_t)(1U << k_ra8_dvstctr_bit_usbrst);
  const uint16_t uact_bit = (uint16_t)(1U << k_ra8_dvstctr_bit_uact);
  if (assert_reset) {
    /* USBRST=1 forces UACT low; FSP atomically sets RST + clears UACT. */
    internal_rmw16(&reg->DVSTCTR0, rst_bit, uact_bit);
  } else {
    internal_rmw16(&reg->DVSTCTR0, 0U, rst_bit);
  }
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_host_set_uact()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] enable See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_host_set_uact(ra8_usb_speed_t speed, bool enable)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 36.2.5 "DVSTCTR0 : Device State Control Register 0", p 1971 */
  const uint16_t uact_bit = (uint16_t)(1U << k_ra8_dvstctr_bit_uact);
  if (enable) {
    internal_rmw16(&reg->DVSTCTR0, uact_bit, 0U);
  } else {
    internal_rmw16(&reg->DVSTCTR0, 0U, uact_bit);
  }
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_host_setup_request()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] setup See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_host_setup_request(ra8_usb_speed_t speed, const ra8_usb_setup_t* setup)
{
  RA8_CHECK_NULL_PTR(setup, s_tag, "host_setup_request: setup");
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1999 -- guard
   * against a still-pending request. */
  const uint16_t sureq_bit = (uint16_t)(1U << k_ra8_dcpctr_bit_sureq);
  if ((reg->DCPCTR & sureq_bit) != 0U) {
    return k_ra8_err_busy;
  }

  /* HUM Ch 36.2.17 "USBREQ : USB Request Type Register", p 1995 */
  const uint16_t req = (uint16_t)((uint16_t)setup->bm_request_type |
                                  (uint16_t)((uint16_t)setup->b_request << k_ra8_usb_byte_bits));
  reg->USBREQ        = req;
  reg->USBVAL        = setup->w_value;
  reg->USBINDX       = setup->w_index;
  reg->USBLENG       = setup->w_length;

  /* SUREQ tells the SIE to issue the SETUP token on the next frame. */
  internal_rmw16(&reg->DCPCTR, sureq_bit, 0U);
  return k_ra8_ok;
}
