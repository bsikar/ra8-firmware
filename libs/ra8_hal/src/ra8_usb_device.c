/**
 * @file ra8_usb_device.c
 * @brief USB device-mode lifecycle, status, and endpoint configuration
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Device-mode half of the native USB driver: module init / deinit /
 * attach, status + device-state queries, USB-address programming, the
 * bus-reset DCP re-arm, and the non-control endpoint (pipe) configuration
 * and stall entry points (HUM Ch 36 USBFS / Ch 37 USBHS). Split out of
 * ``ra8_usb.c`` so every translation unit stays under the 1000-line cap;
 * the shared register helpers it calls live in ``ra8_usb.c`` and the PHY /
 * common bring-up it dispatches to lives in ``ra8_usb_phy.c`` -- both are
 * declared in ``ra8_usb_internal.h``. Modelled on FSP ``r_usb_pdriver.c``
 * / ``r_usb_preg_abs.c``; no FSP source ships in this tree.
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
 * Device-mode bring-up diagnostic probes (read via JLink)
 * =============================================================================
 */

/**
 * @var g_syscfg_after_phy_bringup
 * @brief SYSCFG snapshot captured at the end of ra8_usb_device_init.
 *
 * @details
 * Bisect probe for the "USBE clears between phy bring-up and echo loop"
 * regression. Captured AFTER priv_usbhs_phy_bringup AND
 * priv_usb_init_common have both run. Expected value: 0x0081
 * (USBE | HSE). HUM Ch 37.2.1 SYSCFG p 2060.
 *
 * @note Read-only from outside; written only by ::ra8_usb_device_init.
 * @since 0.1.0
 */
volatile uint16_t g_syscfg_after_phy_bringup = 0U;

/**
 * @var g_lpsts_after_phy_bringup
 * @brief LPSTS snapshot at the end of ra8_usb_device_init.
 *
 * @details Companion to ::g_syscfg_after_phy_bringup. Expected value:
 * SUSPENDM=1 (bit 14), so 0x4000. HUM Ch 37.2.43 LPSTS p 2111.
 *
 * @note Read-only from outside; written only by ::ra8_usb_device_init.
 * @since 0.1.0
 */
volatile uint16_t g_lpsts_after_phy_bringup = 0U;

/**
 * @var g_syscfg_after_attach
 * @brief SYSCFG snapshot captured at the end of ra8_usb_device_attach(true).
 *
 * @details Bisect probe; expected value with attach=true: 0x0091
 * (USBE | DPRPU | HSE). HUM Ch 37.2.1 SYSCFG p 2060.
 *
 * @note Read-only from outside; written only by ::ra8_usb_device_attach.
 * @since 0.1.0
 */
volatile uint16_t g_syscfg_after_attach = 0U;

/**
 * @var g_lpsts_after_attach
 * @brief LPSTS snapshot at the end of ra8_usb_device_attach(true).
 *
 * @details Bisect probe; expected SUSPENDM=1 (0x4000). HUM Ch 37.2.43
 * LPSTS p 2111.
 *
 * @note Read-only from outside; written only by ::ra8_usb_device_attach.
 * @since 0.1.0
 */
volatile uint16_t g_lpsts_after_attach = 0U;

/**
 * @var g_syssts0_after_attach
 * @brief SYSSTS0 snapshot captured at the end of ra8_usb_device_attach(true).
 *
 * @details Bisect probe; this is the LOAD-BEARING readback for the HS
 * device-mode attach gate. Per HUM Ch 37.2.3 SYSSTS0 p 2063 and
 * Table 37.4 p 2064:
 *   - LNST[1:0] = 00b -> SE0    (D+ pull-up NOT visible)
 *   - LNST[1:0] = 01b -> J-State (FS device pull-up visible to host)
 *   - LNST[1:0] = 10b -> K-State (HS chirp / FS K)
 *
 * Expected post-attach value with HS cable plugged: 0x0001 (LNST=01,
 * J-state) -- the host then issues a USB bus reset within ~100 ms,
 * after which DVST fires and FRMNUM begins incrementing.
 *
 * Per HUM Ch 37.2.1 SYSCFG description on p 2062 (CNEN bit), LNST
 * reads as SE0 unless SYSCFG.CNEN=1 in device-mode -- the single-end
 * receivers must be powered before the line state is observable.
 *
 * @note Read-only from outside; written only by ::ra8_usb_device_attach.
 * @since 0.1.0
 */
volatile uint16_t g_syssts0_after_attach = 0U;

/**
 * @var g_syscfg_before_dprpu
 * @brief SYSCFG snapshot captured immediately BEFORE the DPRPU write.
 *
 * @details Bisect probe; expected: 0x0181 (USBE | CNEN | HSE) on the
 * HS instance after the CNEN-then-DPRPU split that aligns with HUM
 * Ch 37.3.3 Figure 37.2 p 2121 ("Resistor control: set SYSCFG.DPRPU
 * and SYSCFG.DRPD" comes AFTER "Enable USB operation: set SYSCFG.USBE
 * = 1" and AFTER the CNEN single-end-receiver enable).
 *
 * @note Read-only from outside; written only by ::ra8_usb_device_attach.
 * @since 0.1.0
 */
volatile uint16_t g_syscfg_before_dprpu = 0U;

/* =============================================================================
 * Device-mode state decode
 * =============================================================================
 */

/**
 * @brief Translate `INTSTS0.DVSQ[2:0]` into the public state enum.
 *
 * @details See implementation.
 * @param[in] intsts0 See implementation.
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
static ra8_usb_dev_state_t internal_decode_dvsq(uint16_t intsts0)
{
  const uint16_t dvsq = (uint16_t)(intsts0 & k_ra8_intsts0_mask_dvsq);
  /* Suspend bit (0x40) wins over the lower three states. */
  if ((dvsq & k_ra8_dvsq_suspend) != 0U) {
    return k_ra8_usb_dev_state_suspended;
  }
  if (dvsq == k_ra8_dvsq_default) {
    return k_ra8_usb_dev_state_default;
  }
  if (dvsq == k_ra8_dvsq_address) {
    return k_ra8_usb_dev_state_address;
  }
  if (dvsq == k_ra8_dvsq_configured) {
    return k_ra8_usb_dev_state_configured;
  }
  return k_ra8_usb_dev_state_powered;
}
/**
 * @brief Implementation of `ra8_usb_device_init()`.
 * @details Dispatches FS vs HS bring-up, then programmes shared FIFO,
 *          DCP, and INTENB0 fields.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @retval k_ra8_err_invalid_arg Invalid speed.
 * @retval k_ra8_err_hw_timeout HS PHY PLL lock timeout.
 * @pre MSTP and clock subsystem are armed.
 * @pre Caller is single-threaded init context.
 * @post Module powered and SYSCFG.USBE = 1.
 * @post INTENB0 carries device-mode interrupt mask.
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_device_init(ra8_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  const ra8_err_t mst_err = ra8_mstp_enable(priv_mstp(speed));
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "usb_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  if (speed == k_ra8_usb_speed_hs) {
    const ra8_err_t phy_err = priv_usbhs_phy_bringup(reg);
    RA8_RETURN_ON_ERROR(phy_err, s_tag, "usb_init: HS PHY bring-up"); /* GCOVR_EXCL_BR_LINE */
  } else {
    const ra8_err_t fs_err = priv_usbfs_module_bringup(reg);
    RA8_RETURN_ON_ERROR(fs_err, s_tag, "usb_init: FS module bring-up"); /* GCOVR_EXCL_BR_LINE */
  }

  priv_usb_init_common(reg);

  /* Bisect probes: capture SYSCFG/LPSTS state at the END of device-init,
   * BEFORE control returns to ra8_board_usbhs_device_init / the DCD
   * bridge. HUM Ch 37.2.1 SYSCFG p 2060, HUM Ch 37.2.43 LPSTS p 2111. */
  if (speed == k_ra8_usb_speed_hs) {
    g_syscfg_after_phy_bringup = reg->SYSCFG;
    g_lpsts_after_phy_bringup  = *ra8_usbhs_lpsts();
  }

  ra8_log_info_val(s_tag, "usb device init speed", (uint32_t)speed);
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_device_deinit()`.
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
ra8_err_t ra8_usb_device_deinit(ra8_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 36.2.10 "INTENB0 : Interrupt Enable Register 0", p 1980 */
  reg->INTENB0 = 0U;
  reg->INTENB1 = 0U;
  reg->BRDYENB = 0U;
  reg->NRDYENB = 0U;
  reg->BEMPENB = 0U;

  /* HUM Ch 36.2.1 "SYSCFG : System Configuration Control Register", p 1967 */
  reg->SYSCFG = 0U;
  return ra8_mstp_disable(priv_mstp(speed));
}

/**
 * @brief Implementation of `ra8_usb_device_attach()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] attached See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_device_attach(ra8_usb_speed_t speed, bool attached)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 36.2.1 "SYSCFG : System Configuration Control Register", p 1967 */
  /* HUM Ch 37.2.1 "SYSCFG : System Configuration Control Register", p 2061 */
  const uint16_t dprpu = (uint16_t)(1U << k_ra8_syscfg_bit_dprpu);
  const uint16_t cnen  = (uint16_t)(1U << k_ra8_syscfg_bit_cnen);

  if (attached) {
    /* HUM Ch 37.2.1 SYSCFG, "CNEN bit (Single-ended Receiver Enable)"
     * p 2062: "In device controller mode, set this bit to 1 when VBUS
     * is detected because of a VBUS interrupt, and set it to 0 when
     * the VBUS line is removed."
     *
     * Without CNEN=1 the HS PHY's single-end receivers are powered
     * down and SYSSTS0.LNST[1:0] reads 00b (SE0) regardless of what
     * the host's 1.5 kohm-or-15 kohm pull-down arrangement and the
     * device's DPRPU 1.5 kohm pull-up are doing on the wires.
     *
     * Programme CNEN BEFORE DPRPU so the receivers latch the line
     * state the moment DPRPU pulls D+ high; this is the FS-attach
     * sequence sketched in HUM Ch 37.3.3 Figure 37.2 p 2121
     * ("Resistor control" step comes after USBE/CNEN are settled).
     * The FS instance has no CNEN gate (HUM Ch 36.2.1 SYSCFG p 1966),
     * so CNEN is HS-only here. */
    if (speed == k_ra8_usb_speed_hs) {
      priv_rmw16(&reg->SYSCFG, cnen, 0U);
      g_syscfg_before_dprpu = reg->SYSCFG;
    }
    priv_rmw16(&reg->SYSCFG, dprpu, 0U);
  } else {
    priv_rmw16(&reg->SYSCFG, 0U, dprpu);
    if (speed == k_ra8_usb_speed_hs) {
      /* HUM Ch 37.2.1 p 2062: clear CNEN when VBUS is removed /
       * detach is requested, to avoid through-current on floating
       * D+/D- when the cable is unplugged. */
      priv_rmw16(&reg->SYSCFG, 0U, cnen);
    }
  }

  /* Bisect probes: capture SYSCFG/LPSTS/SYSSTS0 at end of attach. HUM
   * Ch 37.2.1 SYSCFG p 2060, HUM Ch 37.2.3 SYSSTS0 p 2063, HUM
   * Ch 37.2.43 LPSTS p 2111. */
  if (speed == k_ra8_usb_speed_hs) {
    g_syscfg_after_attach  = reg->SYSCFG;
    g_lpsts_after_attach   = *ra8_usbhs_lpsts();
    g_syssts0_after_attach = reg->SYSSTS0;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Status / state
 * =============================================================================
 */

/**
 * @brief Implementation of `ra8_usb_get_status()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] out_mask See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_get_status(ra8_usb_speed_t speed, uint16_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1986 */
  *out_mask = reg->INTSTS0;
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_clear_status()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] mask See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_clear_status(ra8_usb_speed_t speed, uint16_t mask)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1986 */
  reg->INTSTS0 = (uint16_t)(reg->INTSTS0 & (uint16_t)~mask);
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_get_device_state()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] out_state See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_get_device_state(ra8_usb_speed_t speed, ra8_usb_dev_state_t* out_state)
{
  RA8_CHECK_NULL_PTR(out_state, s_tag, "out_state must not be nullptr");
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1986 */
  *out_state = internal_decode_dvsq(reg->INTSTS0);
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_set_address()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] address See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_set_address(ra8_usb_speed_t speed, uint8_t address)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (address > k_ra8_usb_max_address) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 36.2.16 "USBADDR : USB Address Register", p 1994 */
  /* HUM Ch 37.2.16 "USBADDR : USB Address Register", p 2089 */
  reg->USBADDR = (uint16_t)((uint16_t)address & k_ra8_usbaddr_addr_mask);
  return k_ra8_ok;
}

/**
 * @brief Reset DCP defaults + clear DCP FIFO on bus-reset rearm.
 * @details Helper extracted from ::ra8_usb_device_busreset_rearm to keep
 *          the top-level function under the clang-tidy line budget.
 *          Re-asserts DCPCFG=0 and DCPMAXP=64, then pulses
 *          CFIFOCTR.BCLR (HUM Ch 36.2.8 / 37.2.10 p 1979 / 2073) to
 *          wipe any bytes a previous incomplete control transfer left
 *          in the DCP FIFO. Without the BCLR pulse the next
 *          ra8_usb_dcp_in_data sees FRDY=0, times out, and the DCD
 *          path STALLs EP0 (Linux dmesg "device descriptor read/N,
 *          error -110" on every retry).
 * @param[in] reg Selected USB instance register block.
 * @pre reg != NULL.
 * @pre Caller is in IRQ-callback context.
 * @post DCPCFG = 0, DCPMAXP = 64, DCP FIFO cleared.
 * @post CFIFOSEL points at DCP IN (CURPIPE=0, MBW=16, ISEL=1).
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_dcp_reset_defaults(volatile r_usb_regs_t* reg)
{
  reg->DCPCFG  = 0U;
  reg->DCPMAXP = k_ra8_usb_dcp_max_packet;
  priv_select_cfifo(reg, 0U, true);
  reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_bclr;
}

/**
 * @brief Implementation of ra8_usb_device_busreset_rearm (see header).
 * @details Re-default DCPCFG/DCPMAXP, clear all PIPECTR[*], drop
 *          BRDYSTS/NRDYSTS/BEMPSTS, clear the DCP FIFO via
 *          CFIFOCTR.BCLR, and re-arm INTENB0 with the post-init mask.
 *          Mirrors FSP r_usb_psignal.c::usb_pstd_bus_reset.
 * @param[in] speed Which controller (FS or HS).
 * @return ra8_err_t result code.
 * @retval k_ra8_ok               Success.
 * @retval k_ra8_err_invalid_arg  speed out of range.
 * @pre Module clock and power are on.
 * @pre Caller is in IRQ-callback context (DVST=Default branch).
 * @post DCPCFG=0, DCPMAXP=64, DCP FIFO cleared, INTENB0 re-armed.
 * @post All non-control PIPECTR[*] cleared (PID=NAK).
 * @note Not thread-safe; caller holds the DCP lock.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_device_busreset_rearm(ra8_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 36.2.27 "PIPECTR : Pipe n Control Register", p 2007.
   * Clear PID for every non-control pipe so any half-completed pre-
   * reset transfer is forgotten. The class driver re-issues queue_in /
   * queue_out which will set PID=BUF when ready. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_usb_pipectr_count; i++) {
    reg->PIPECTR[i] = 0U;
  }

  /* HUM Ch 36.2.13/14 BRDYSTS / NRDYSTS / BEMPSTS, p 1983-1985 (W0C).
   * Drop any per-pipe status bits left over from before the reset so
   * stale BRDY edges don't fire spurious BRDY callbacks during the next
   * enumeration. Writing 0 clears every bit; writing 1 preserves. */
  reg->BRDYSTS = 0U;
  reg->NRDYSTS = 0U;
  reg->BEMPSTS = 0U;

  /* DCP defaults + FIFO clear (see helper). HUM Ch 36.2.19 / 37.2.29
   * DCPCFG p 1989 / 2091 and HUM Ch 36.2.20 / 37.2.30 DCPMAXP p 1990
   * / 2092. DCPCTR is intentionally NOT written here: HUM Ch 36.2.21
   * / 37.2.31 (p 1991 / 2093) states the IP auto-defaults the
   * writable DCPCTR fields on bus reset (PID=NAK, CCPL=0). */
  internal_dcp_reset_defaults(reg);

  /* HUM Ch 36.2.10 "INTENB0 : Interrupt Enable Register 0", p 1980.
   * Some RA silicon revisions clear individual INTENB0 bits across a
   * bus reset (notably CTRT). Re-apply the post-init mask defensively
   * so the next SETUP raises CTRT as expected. Mirrors the mask in
   * priv_usb_init_common. */
  /* SOFR and RSME are intentionally NOT enabled here:
   *   - SOFR fires every 125us on HS (= 8 kHz);
   *   - RSME stays asserted on USBHS via the PHY's USBR signal
   *     while the host holds the bus in resume signalling;
   * Both starve PendSV and the demo worker thread never gets
   * scheduled. NRDY is still useful (drives the bridge's per-pipe
   * NAK re-arm) and does not cause a storm on its own. RSME / SOFR
   * status is still readable from INTSTS0 for poll-style drivers. */
  reg->INTENB0 = (uint16_t)((1U << k_ra8_int0_bit_bemp) | (1U << k_ra8_int0_bit_brdy) |
                            (1U << k_ra8_int0_bit_nrdy) | (1U << k_ra8_int0_bit_ctrt) |
                            (1U << k_ra8_int0_bit_dvst) | (1U << k_ra8_int0_bit_vbse));

  return k_ra8_ok;
}
/* =============================================================================
 * Endpoints
 * =============================================================================
 */

/**
 * @brief Validate the argument set for `ra8_usb_configure_endpoint`.
 *
 * @return k_ra8_ok if all arguments are in range.
 *
 * @details See implementation.
 * @param[in] pipe_num See implementation.
 * @param[in] ep_addr See implementation.
 * @param[in] dir See implementation.
 * @param[in] type See implementation.
 * @param[in] max_packet See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_check_ep_args(uint8_t           pipe_num,
                                        uint8_t           ep_addr,
                                        ra8_usb_ep_dir_t  dir,
                                        ra8_usb_ep_type_t type,
                                        uint16_t          max_packet)
{
  if ((pipe_num == 0U) || (pipe_num > k_ra8_usb_max_pipe_num)) {
    return k_ra8_err_invalid_arg;
  }
  if ((ep_addr == 0U) || (ep_addr > k_ra8_usb_max_ep_addr)) {
    return k_ra8_err_invalid_arg;
  }
  if ((dir != k_ra8_usb_ep_dir_in) && (dir != k_ra8_usb_ep_dir_out)) {
    return k_ra8_err_invalid_arg;
  }
  if (type > k_ra8_usb_ep_type_iso) {
    return k_ra8_err_invalid_arg;
  }
  if ((max_packet == 0U) || (max_packet > k_ra8_usb_pipe_max_packet)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Finalize the pipe after PIPECFG/PIPEMAXP/PIPEPERI have landed.
 * @details Pulses SQCLR + ACLRM, clears BRDYSTS/BEMPSTS for this pipe,
 *          then sets PID. OUT goes to BUF so the first host OUT token
 *          is ACKed; IN stays at NAK until the caller has data to push
 *          (HUM Ch 36.2.27; mirrors STAR
 *          rx_usb_hw.c::internal_usb_finalize_pipe).
 * @param[in,out] reg Controller register window.
 * @param[in] pipe_num Pipe index 1..9.
 * @param[in] dir Endpoint direction.
 * @pre reg is non-null and points at a powered controller.
 * @pre pipe_num in [1,9] and PIPECFG has been written.
 * @post Pipe FIFO buffer cleared and data toggle reset to DATA0.
 * @post PIPECTR PID == BUF (OUT) or NAK (IN).
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_pipe_finalize(volatile r_usb_regs_t* reg, uint8_t pipe_num, ra8_usb_ep_dir_t dir)
{
  const uint8_t  ctr_idx  = (uint8_t)(pipe_num - 1U);
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  priv_rmw16(&reg->PIPECTR[ctr_idx], k_ra8_pipectr_sqclr, 0U);
  priv_rmw16(&reg->PIPECTR[ctr_idx], k_ra8_pipectr_aclrm, 0U);
  priv_rmw16(&reg->PIPECTR[ctr_idx], 0U, k_ra8_pipectr_aclrm);
  reg->BRDYSTS = (uint16_t)(~pipe_bit);
  reg->BEMPSTS = (uint16_t)(~pipe_bit);
  priv_pipe_pid(reg, pipe_num, (dir == k_ra8_usb_ep_dir_out) ? k_ra8_pid_buf : k_ra8_pid_nak);
}

/**
 * @brief Re-arm the per-pipe interrupt the dispatcher routes off of.
 * @details Sets BRDYENB for OUT pipes and BEMPENB for IN pipes (mirrors
 *          STAR rx_usb_hw.c::rx_usb_hw_configure_pipe step 8). Without
 *          this, BRDYSTS for the freshly configured OUT pipe never
 *          propagates to INTSTS0.BRDY -- the polled-dispatch BRDYSTS
 *          scan stays clean forever, queue_out keeps returning
 *          no_data, the pipe sits at PID=NAK forever, and the host
 *          (macOS AppleUSBCDCACM) gives up retrying bulk-OUT tokens.
 * @param[in,out] reg Controller register window.
 * @param[in] pipe_num Pipe index 1..9.
 * @param[in] dir Endpoint direction.
 * @pre reg is non-null and points at a powered controller.
 * @pre pipe_num in [1,9].
 * @post BRDYENB bit set for OUT, BEMPENB bit set for IN.
 * @post Other pipes' interrupt-enable bits unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_pipe_arm_irq(volatile r_usb_regs_t* reg, uint8_t pipe_num, ra8_usb_ep_dir_t dir)
{
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  if (dir == k_ra8_usb_ep_dir_out) {
    reg->BRDYENB = (uint16_t)(reg->BRDYENB | pipe_bit);
  } else {
    reg->BEMPENB = (uint16_t)(reg->BEMPENB | pipe_bit);
  }
}

/**
 * @brief Implementation of `ra8_usb_configure_endpoint()`.
 * @details See the public header for the documented contract; this
 *          definition implements it. Sequence (FIT
 *          r_usb_creg_abs.c::usb_cstd_pipe_init mirror): quiesce ->
 *          PIPESEL window write of PIPECFG/PIPEMAXP/PIPEPERI ->
 *          deselect window -> finalize PIPECTR -> arm per-pipe IRQ.
 * @param[in] speed Which controller (FS/HS).
 * @param[in] pipe_num Pipe index 1..9.
 * @param[in] ep_addr USB endpoint address (low nibble = EP number).
 * @param[in] dir IN/OUT direction.
 * @param[in] type Bulk / Interrupt / Iso.
 * @param[in] max_packet wMaxPacketSize from the descriptor.
 * @return Result code.
 * @retval k_ra8_ok Pipe configured and PID set per direction.
 * @retval k_ra8_err_invalid_arg speed/pipe/ep/type/max_packet out of range.
 * @pre Controller is powered (ra8_usb_device_init has run).
 * @pre Pipe is not currently mid-transfer (caller serialises).
 * @post PIPECFG/PIPEMAXP/PIPEPERI reflect the requested config.
 * @post BRDYENB or BEMPENB bit for pipe is set per direction.
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_configure_endpoint(ra8_usb_speed_t   speed,
                                     uint8_t           pipe_num,
                                     uint8_t           ep_addr,
                                     ra8_usb_ep_dir_t  dir,
                                     ra8_usb_ep_type_t type,
                                     uint16_t          max_packet)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t arg_err = internal_check_ep_args(pipe_num, ep_addr, dir, type, max_packet);
  if (arg_err != k_ra8_ok) {
    return arg_err;
  }

  priv_pipe_quiesce(reg, pipe_num);

  /* HUM Ch 36.2.23 "PIPESEL : Pipe Window Select Register", p 2002 */
  reg->PIPESEL = pipe_num;
  /* HUM Ch 36.2.24 "PIPECFG : Pipe Configuration Register", p 2003 */
  reg->PIPECFG = priv_pipecfg_word(ep_addr, dir, type, false);
  /* HUM Ch 37.2.35 "PIPEBUF : Pipe Buffer Register", p 2100. PIPEBUF is a
   * USBHS-only register; the USBFS instance (Ch 36) has no equivalent.
   * Bulk pipes get a single MPS-sized bank (DBLB clear). Interrupt/iso
   * pipes keep the reset default. */
  if (type == k_ra8_usb_ep_type_bulk) {
    reg->PIPEBUF = priv_pipebuf_word(pipe_num, max_packet);
  }
  /* HUM Ch 36.2.26 "PIPEMAXP : Pipe Maximum Packet Size Register", p 2005 */
  reg->PIPEMAXP = max_packet;
  reg->PIPEPERI = 0U;
  /* Deselect window (FIT step 4) so a later stray PIPECFG write does
   * not land on this pipe. */
  reg->PIPESEL = 0U;

  internal_pipe_finalize(reg, pipe_num, dir);
  internal_pipe_arm_irq(reg, pipe_num, dir);
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_stall_endpoint()`.
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] pipe_num See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_stall_endpoint(ra8_usb_speed_t speed, uint8_t pipe_num)
{
  volatile r_usb_regs_t* reg = priv_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (pipe_num > k_ra8_usb_max_pipe_num) {
    return k_ra8_err_invalid_arg;
  }
  if (pipe_num == 0U) {
    /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1999 */
    priv_dcp_pid(reg, k_ra8_pid_stall);
  } else {
    /* HUM Ch 36.2.27 "PIPEnCTR : PIPE n Control Register", p 2005 */
    priv_pipe_pid(reg, pipe_num, k_ra8_pid_stall);
  }
  return k_ra8_ok;
}
