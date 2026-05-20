/**
 * @file ra_usb.c
 * @brief Native USB controller driver implementation (device + host)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Hand-written driver for the two RA8D2 USB controllers
 * (USBFS @ 0x40250000 -- HUM Ch 36, USBHS @ 0x40351000 -- HUM Ch 37).
 * The two instances share the FSP "USB2_B" register layout so this
 * file multiplexes them via a `ra_usb_speed_t` argument and an
 * `internal_pick(speed)` helper. No FSP, CherryUSB, or TinyUSB
 * source ships in this tree -- this file is the native peripheral
 * driver, modelled on FSP's `r_usb_pdriver.c` /
 * `r_usb_preg_access.c` / `r_usb_preg_abs.c` (device) and
 * `r_usb_hreg_access.c` / `r_usb_hreg_abs.c` (host) flow.
 *
 * Mapping vs FSP (FSP function -> our entry point):
 *
 *  - `hw_usb_pmodule_init`          -> `ra_usb_device_init`
 *  - `hw_usb_hmodule_init`          -> `ra_usb_host_init`
 *  - `hw_usb_pclear_dprpu/_pset_dprpu` -> `ra_usb_device_attach`
 *  - `hw_usb_set_uact / _clear_uact`-> `ra_usb_host_set_uact`
 *  - `usb_hstd_bus_reset` (set/clr) -> `ra_usb_host_bus_reset`
 *  - `usb_hstd_setup_command`       -> `ra_usb_host_setup_request`
 *  - `usb_pstd_save_request`        -> `ra_usb_read_setup_if_valid` /
 *                                      `ra_usb_read_setup_unconditional`
 *  - `hw_usb_pcontrol_dcpctr_pid` + `hw_usb_pset_ccpl` ->
 *    `ra_usb_control_response`
 *  - `usb_pstd_set_pipe_table`      -> `ra_usb_configure_endpoint`
 *  - `usb_pstd_write_fifo` (CFIFO)  -> `ra_usb_queue_in`
 *  - `usb_pstd_read_fifo`  (CFIFO)  -> `ra_usb_queue_out`
 *  - `usb_pstd_interrupt_handler`   -> `ra_usb_dispatch`
 *
 * Intentional gaps (deferred work, with FSP file:line + reason):
 *
 *  - HS PHY power-down / PLL bring-up (`r_usb_preg_access.c`)
 *    -- the EK-RA8D2 boots its HS PHY from the same 48 MHz clock the
 *    bootloader leaves running, so the driver assumes the PHY is
 *    already powered. Adding a full HS PHY sequence is tracked
 *    against the next iteration.
 *  - DMA glue (`r_usb_dma.c`) -- this driver runs in CPU-FIFO mode
 *    only.
 *  - OTG / role-swap paths (`r_usb_hdriver.c`) -- not ported. Host
 *    and device modes are independently selected at init time.
 *  - USB hubs -- the host-side starter targets a single attached
 *    device. Hub class enumeration is out of scope.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_usb.h"

#include <stdint.h>

#include "ra8d2_mstp_regs.h"
#include "ra8d2_pfs_regs.h"
#include "ra8d2_system_regs.h"
#include "ra8d2_usb_regs.h"
#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_time.h"

static const char* s_tag = "USB";

/* =============================================================================
 * Tunables / sizing
 * =============================================================================
 */

/**
 * @enum ra_usb_internal_lim_t
 * @brief Driver-wide bounds that aren't part of the public API.
 */
typedef enum : uint16_t {
  k_ra_usb_max_pipe_num    = 9U,    /**< PIPE1..PIPE9 + DCP at 0.       */
  k_ra_usb_max_ep_addr     = 15U,   /**< USB EP number is 4 bits.       */
  k_ra_usb_max_address     = 127U,  /**< 7-bit USB address.             */
  k_ra_usb_dcp_max_packet  = 64U,   /**< EP0 default packet size.       */
  k_ra_usb_pipe_max_packet = 1024U, /**< Max packet ceiling for pipes.  */
} ra_usb_internal_lim_t;

/**
 * @enum ra_usb_internal_lim32_t
 * @brief 32-bit driver-wide bounds (don't fit in uint16_t).
 *
 * @details ``frdy_poll_limit`` is sized for the worst-case wait
 * between two consecutive DCP IN chunks. The DCP is single-buffered:
 * after pushing chunk N, FRDY does NOT re-assert until the host has
 * actually pulled chunk N off the wire (one full IN token + data +
 * ACK round-trip on USB-FS, ~50 us). The original 1000-spin limit
 * (~1 us at 1 GHz) timed out unconditionally on every multi-chunk
 * EP0 IN, so 75-byte CONFIGURATION descriptors stalled at chunk 1.
 * 10 million spins == ~10 ms ceiling at 1 GHz, well above the
 * USB-FS host's IN re-issue cadence; the loop exits early on the
 * first FRDY=1 sample so the typical post-host-pull wait is still
 * sub-100 us. Synchronous polling is acceptable because the dispatch
 * loop runs in a dedicated ThreadX worker, not in NVIC context.
 */
typedef enum : uint32_t {
  k_ra_usb_frdy_poll_limit = 10000000UL, /**< Spin-loops before timeout. */
  /* Short-bound FRDY poll used by queue_out's post-BCLR DBLB-second-
   * bank check. Must be tight (we're typically on the IRQ-time
   * drain path) but big enough to absorb the few cycles between BCLR
   * and the FIFO pointer flipping to the other bank when DBLB is on.
   * 256 iters x ~3 cycles == sub-microsecond at 1 GHz. */
  k_ra_usb_dblb_frdy_poll_limit = 256UL,
} ra_usb_internal_lim32_t;

/**
 * @enum ra_usb_byte_mask_t
 * @brief Byte-extraction masks shared across the FIFO byte path.
 */
typedef enum : uint16_t {
  k_ra_usb_byte_mask = 0x00FFU, /**< Low byte of a 16-bit FIFO word. */
  k_ra_usb_byte_bits = 8U,      /**< Bits per byte (shift constant). */
} ra_usb_byte_mask_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Resolve the per-speed register pointer.
 */
static volatile r_usb_regs_t* internal_pick(ra_usb_speed_t speed)
{
  if (speed == k_ra_usb_speed_fs) {
    return ra_usb_fs();
  }
  if (speed == k_ra_usb_speed_hs) {
    return ra_usb_hs();
  }
  return nullptr;
}

/**
 * @brief Resolve the per-speed MSTP id.
 *
 * @details See implementation.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_mstp_t internal_mstp(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? k_ra_mstp_usbhs : k_ra_mstp_usbfs;
}

/**
 * @brief Apply a generic read-modify-write to a 16-bit register.
 *
 * @details See implementation.
 * @param[in] reg See implementation.
 * @param[in] set_mask See implementation.
 * @param[in] clr_mask See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_rmw16(volatile uint16_t* reg, uint16_t set_mask, uint16_t clr_mask)
{
  const uint16_t old     = *reg;
  uint16_t       new_val = (uint16_t)(old & (uint16_t)~clr_mask);
  new_val                = (uint16_t)(new_val | set_mask);
  *reg                   = new_val;
}

/**
 * @brief Configure the CFIFO port to talk to a given pipe + width.
 *
 * @details Mirrors the per-instance `CFIFOSEL` programming from
 * `hw_usb_pmodule_init` (FSP `r_usb_preg_access.c`). MBW is
 * always 16-bit on this driver path -- hardware FIFOs are
 * little-endian; the host build relies on identical behaviour.
 *
 * @param[in] reg See implementation.
 * @param[in] pipe_num See implementation.
 * @param[in] is_in_dir See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
/**
 * @brief Detect whether reg points at the USBHS (IP1) register block.
 * @details FSP gates `USB1_CFIFO_MBW = USB_MBW_32` on the same predicate
 *          (`p_utr->ip == USB_CFG_IP1`), and the FIFO write helpers below
 *          mirror that to keep CFIFOSEL.MBW and the CFIFO write width in
 *          agreement on each controller.
 * @param[in] reg USB instance register block pointer.
 * @return true if reg is the HS instance, false for FS.
 * @retval true USBHS (IP1) -- caller should use MBW=32 + 32-bit FIFO.
 * @retval false USBFS (IP0) -- caller should use MBW=16 + 16-bit FIFO.
 * @pre reg is a pointer returned by ra_usb_fs() or ra_usb_hs().
 * @pre USB module pointers are populated.
 * @post No state mutated.
 * @post Returned value reflects controller identity.
 * @note Pure function.
 * @since 0.1.0
 */
static inline bool internal_is_hs(volatile r_usb_regs_t* reg)
{
  return reg == ra_usb_hs();
}

/**
 * @brief Set CFIFOSEL.MBW + CURPIPE + ISEL for the given pipe / direction.
 * @details Picks MBW=32 for USBHS (FSP USB1_CFIFO_MBW) and MBW=16 for
 *          USBFS (FSP USB0_CFIFO_MBW). The CFIFO data-port access
 *          width must match the MBW field on subsequent CFIFO accesses.
 * @param[in] reg USB instance register block.
 * @param[in] pipe_num CURPIPE value (0 = DCP, 1..n = data pipe).
 * @param[in] is_in_dir true = device-to-host (write), false = host-to-device.
 * @pre reg != NULL.
 * @pre Caller holds the DCP / pipe lock.
 * @post CFIFOSEL = MBW(speed) | (is_in_dir ? ISEL : 0) | pipe_num.
 * @post Subsequent CFIFO accesses must use the matching width.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_select_cfifo(volatile r_usb_regs_t* reg, uint16_t pipe_num, bool is_in_dir)
{
  /* HUM Ch 36.2.7 / 37.2.8 CFIFOSEL p 1976 / 2071. The USBHS module
   * (IP1) requires MBW=32 -- FSP wires `USB1_CFIFO_MBW = USB_MBW_32`
   * unconditionally for this controller. With MBW=16 the SIE refuses
   * to arm an IN response (BSTS reads 0 and PID stays effectively
   * NAK on the wire even though DCPCTR.PID=BUF), and the host sees
   * unending NAKs / "device descriptor read/N, error -110". The
   * USBFS module (IP0) keeps MBW=16. */
  uint16_t sel = (uint16_t)(pipe_num & k_ra_fifosel_curpipe);
  if (internal_is_hs(reg)) {
    sel = (uint16_t)(sel | k_ra_fifosel_mbw_32);
  } else {
    sel = (uint16_t)(sel | k_ra_fifosel_mbw_16);
  }
  if (is_in_dir) {
    sel = (uint16_t)(sel | k_ra_fifosel_isel);
  }
  reg->CFIFOSEL = sel;
}

/**
 * @brief Spin until `CFIFOCTR.FRDY` asserts or a deadline elapses.
 *
 * @return ra_ok on FRDY, k_ra_err_hw_timeout otherwise.
 *
 * @details See implementation.
 * @param[in] reg See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_wait_frdy(volatile r_usb_regs_t* reg)
{
  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979.
   * Loop bound is large (~10 ms ceiling at 1 GHz) because the DCP
   * is single-buffered: between consecutive EP0 IN chunks FRDY stays
   * low until the host actually pulls the previous chunk off the
   * wire. See ra_usb_internal_lim32_t for the rationale. */
  for (uint32_t i = 0U; i < (uint32_t)k_ra_usb_frdy_poll_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((reg->CFIFOCTR & k_ra_fifoctr_frdy) != 0U) {                   /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Set DCPCTR PID field to a specific value while preserving the
 *        rest of the register.
 *
 * @details See implementation.
 * @param[in] reg See implementation.
 * @param[in] pid See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_dcp_pid(volatile r_usb_regs_t* reg, ra_usb_pid_t pid)
{
  /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1991 */
  internal_rmw16(&reg->DCPCTR, pid, k_ra_pid_mask);
}

/**
 * @brief Set PIPECTR[idx] PID field.
 *
 * @details See implementation.
 * @param[in] reg See implementation.
 * @param[in] pipe_num See implementation.
 * @param[in] pid See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_pipe_pid(volatile r_usb_regs_t* reg, uint8_t pipe_num, ra_usb_pid_t pid)
{
  /* HUM Ch 36.2.27 "PIPEnCTR : PIPE n Control Register", p 2005 */
  const uint8_t idx = (uint8_t)(pipe_num - 1U);
  internal_rmw16(&reg->PIPECTR[idx], pid, k_ra_pid_mask);
}

/**
 * @brief Translate `INTSTS0.DVSQ[2:0]` into the public state enum.
 *
 * @details See implementation.
 * @param[in] intsts0 See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_usb_dev_state_t internal_decode_dvsq(uint16_t intsts0)
{
  const uint16_t dvsq = (uint16_t)(intsts0 & k_ra_intsts0_mask_dvsq);
  /* Suspend bit (0x40) wins over the lower three states. */
  if ((dvsq & k_ra_dvsq_suspend) != 0U) {
    return k_ra_usb_dev_state_suspended;
  }
  if (dvsq == k_ra_dvsq_default) {
    return k_ra_usb_dev_state_default;
  }
  if (dvsq == k_ra_dvsq_address) {
    return k_ra_usb_dev_state_address;
  }
  if (dvsq == k_ra_dvsq_configured) {
    return k_ra_usb_dev_state_configured;
  }
  return k_ra_usb_dev_state_powered;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @enum ra_usbhs_init_local_t
 * @brief Local sentinels used by the USBHS PHY bring-up sequence.
 *
 * @details
 * - ``delay_1us_iters`` -- NOP-loop iteration count for ~1 us at 1 GHz.
 * - ``pll_lock_poll_limit`` -- bounded spin count for PLLSTA.PLLLOCK
 *   wait (~200k iters; covers worst-case PHY analog ramp).
 * - ``scke_poll_limit`` -- same bound for the FS-path SCKE settle wait.
 * - ``probe_hi_shift`` -- shift to pack SYSCFG into the high half of
 *   ``s_usbhs_init_probe``.
 * - ``probe_lo_mask`` -- mask of the low half of the probe word.
 */
typedef enum : uint32_t {
  k_ra_usbhs_delay_1us_iters = 2000U,
  /* PHY analog PLL can take multiple ms to lock at cold start; FSP
   * uses an unbounded while loop. We pick 5 million NOP-spins
   * (~5 ms ceiling at 1 GHz Cortex-M85) which is well above the
   * worst-case observed lock time but still bounded for NASA Rule 2. */
  k_ra_usbhs_pll_lock_poll_limit = 5000000U,
  /* Per-CLKSEL bisect attempt: ~50 ms ceiling at 1 GHz so the 4-attempt
   * sweep stays under the 250 ms total wall-time budget mandated by the
   * USB-HS bring-up debug spec. */
  k_ra_usbhs_pll_lock_attempt_limit = 50000000U / 100U,
  k_ra_usbhs_scke_poll_limit        = 200000U,
  k_ra_usbhs_probe_hi_shift         = 16U,
  k_ra_usbhs_probe_lo_mask          = 0x0000FFFFU,
} ra_usbhs_init_local_t;

/**
 * @enum ra_usbhs_clksel_attempt_t
 * @brief Iteration order for the CLKSEL[1:0] bisect harness.
 *
 * @details HUM Ch 37.2.17 PHYSET CLKSEL[1:0] bit-field, p 2080:
 *   - 00b = 12 MHz (canonical UTMI reference; PHY divides USB60CLK
 *     by 5 internally to obtain 12 MHz from a 60 MHz USB60CLK)
 *   - 01b = 48 MHz
 *   - 10b = 20 MHz
 *   - 11b = 24 MHz (PHYSET reset value)
 *
 * USB60CKDIVCR is now /4 so USB60CLK = PLL2P/4 = 240/4 = 60.000 MHz,
 * matching the HUM Ch 37.3.3 named-rate requirement (p 2102: "A
 * 60-MHz clock must be supplied"). With a 60 MHz USB60CLK the
 * canonical CLKSEL is 12 MHz (the standard UTMI+ reference), so the
 * bisect tries CLKSEL=12 first and only falls through to 48 / 20 / 24
 * as a defensive sweep if a CGC regression delivers an unexpected
 * USB60CLK rate. Capture the winner (or 0xFF on full sweep failure)
 * into ::s_clksel_winner.
 */
typedef enum : uint8_t {
  k_ra_usbhs_clksel_attempts_n = 4U,
  k_ra_usbhs_clksel_no_winner  = 0xFFU, /**< Sentinel: bisect exhausted. */
} ra_usbhs_clksel_attempt_t;

/**
 * @enum ra_usbhs_phy_step_t
 * @brief Sentinel values for the per-step USBHS PHY bring-up probe.
 *
 * @details Each value marks a completed sub-step of
 * ::internal_usbhs_phy_bringup. A JLink read of ``s_phy_step_probe``
 * pinpoints the furthest-reached step on a stalled boot.
 */
typedef enum : uint8_t {
  k_ra_usbhs_phy_step_hse_set      = 1U, /**< SYSCFG.HSE asserted.          */
  k_ra_usbhs_phy_step_clksel_12    = 2U, /**< PHYSET CLKSEL forced to 12.   */
  k_ra_usbhs_phy_step_dirpd_clear  = 3U, /**< PHY analog powered.           */
  k_ra_usbhs_phy_step_pll_released = 4U, /**< PHY PLLRESET cleared.         */
  k_ra_usbhs_phy_step_usbe_set     = 5U, /**< SYSCFG DRPD=0 USBE=1.         */
  k_ra_usbhs_phy_step_suspendm_set = 6U, /**< LPSTS SUSPENDM=1.             */
  k_ra_usbhs_phy_step_pll_locked   = 7U, /**< PLLSTA.PLLLOCK observed.      */
} ra_usbhs_phy_step_t;

/**
 * @var s_phy_step_probe
 * @brief Diagnostic step counter for the USBHS PHY bring-up sequence.
 *
 * @details Each step in ::internal_usbhs_phy_bringup writes a fixed
 * sentinel here so a JLink read pinpoints the last completed step:
 * - 1 = SYSCFG.HSE asserted (pre-PHY).
 * - 2 = PHYSET DIRPD|CLKSEL set, CLKSEL field forced to 12 MHz.
 * - 3 = PHYSET DIRPD released (PHY analog powered).
 * - 4 = PHYSET PLLRESET released.
 * - 5 = SYSCFG DRPD cleared, USBE set.
 * - 6 = LPSTS.SUSPENDM set.
 * - 7 = PLLSTA.PLLLOCK observed (PHY ready).
 *
 * @note Read-only from outside; written only by the PHY bring-up.
 * @warning Direct modification breaks the diagnostic invariant.
 * @since 0.1.0
 */
static volatile uint8_t s_phy_step_probe = 0U;

/**
 * @var s_usbhs_init_probe
 * @brief Diagnostic capture of staged USBHS bring-up register values.
 *
 * @details Each phase of the HS PHY bring-up writes a distinct slot
 * here so a JLink read of this SRAM word reveals exactly which step
 * was reached. Layout (low half = PHYSET-after-power-up, high half =
 * SYSCFG-after-USBE-write). On a healthy boot the low half ends at
 * ``DIRPD=0, PLLRESET=0, CLKSEL=0`` and the high half ends at
 * ``USBE|HSE = 0x081``.
 *
 * @note Read-only from outside; written only by ::ra_usb_device_init.
 * @warning Direct modification breaks the diagnostic invariant.
 * @since 0.1.0
 */
static volatile uint32_t s_usbhs_init_probe = 0U;

/**
 * @var s_usbhs_pllsta_probe
 * @brief Diagnostic capture of PLLSTA at end of PHY bring-up.
 *
 * @note Read-only from outside; written only by ::ra_usb_device_init.
 * @warning Direct modification breaks the diagnostic invariant.
 * @since 0.1.0
 */
static volatile uint16_t s_usbhs_pllsta_probe = 0U;

/**
 * @var s_syscfg_after_phy_bringup
 * @brief SYSCFG snapshot captured at the end of ra_usb_device_init.
 *
 * @details
 * Bisect probe for the "USBE clears between phy bring-up and echo loop"
 * regression. Captured AFTER internal_usbhs_phy_bringup AND
 * internal_usb_init_common have both run. Expected value: 0x0081
 * (USBE | HSE). HUM Ch 37.2.1 SYSCFG p 2060.
 *
 * @note Read-only from outside; written only by ::ra_usb_device_init.
 * @since 0.1.0
 */
volatile uint16_t s_syscfg_after_phy_bringup = 0U;

/**
 * @var s_lpsts_after_phy_bringup
 * @brief LPSTS snapshot at the end of ra_usb_device_init.
 *
 * @details Companion to ::s_syscfg_after_phy_bringup. Expected value:
 * SUSPENDM=1 (bit 14), so 0x4000. HUM Ch 37.2.43 LPSTS p 2111.
 *
 * @note Read-only from outside; written only by ::ra_usb_device_init.
 * @since 0.1.0
 */
volatile uint16_t s_lpsts_after_phy_bringup = 0U;

/**
 * @var s_syscfg_after_attach
 * @brief SYSCFG snapshot captured at the end of ra_usb_device_attach(true).
 *
 * @details Bisect probe; expected value with attach=true: 0x0091
 * (USBE | DPRPU | HSE). HUM Ch 37.2.1 SYSCFG p 2060.
 *
 * @note Read-only from outside; written only by ::ra_usb_device_attach.
 * @since 0.1.0
 */
volatile uint16_t s_syscfg_after_attach = 0U;

/**
 * @var s_lpsts_after_attach
 * @brief LPSTS snapshot at the end of ra_usb_device_attach(true).
 *
 * @details Bisect probe; expected SUSPENDM=1 (0x4000). HUM Ch 37.2.43
 * LPSTS p 2111.
 *
 * @note Read-only from outside; written only by ::ra_usb_device_attach.
 * @since 0.1.0
 */
volatile uint16_t s_lpsts_after_attach = 0U;

/**
 * @var s_syssts0_after_attach
 * @brief SYSSTS0 snapshot captured at the end of ra_usb_device_attach(true).
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
 * @note Read-only from outside; written only by ::ra_usb_device_attach.
 * @since 0.1.0
 */
volatile uint16_t s_syssts0_after_attach = 0U;

/**
 * @var s_syscfg_before_dprpu
 * @brief SYSCFG snapshot captured immediately BEFORE the DPRPU write.
 *
 * @details Bisect probe; expected: 0x0181 (USBE | CNEN | HSE) on the
 * HS instance after the CNEN-then-DPRPU split that aligns with HUM
 * Ch 37.3.3 Figure 37.2 p 2121 ("Resistor control: set SYSCFG.DPRPU
 * and SYSCFG.DRPD" comes AFTER "Enable USB operation: set SYSCFG.USBE
 * = 1" and AFTER the CNEN single-end-receiver enable).
 *
 * @note Read-only from outside; written only by ::ra_usb_device_attach.
 * @since 0.1.0
 */
volatile uint16_t s_syscfg_before_dprpu = 0U;

/**
 * @var s_clksel_winner
 * @brief Diagnostic capture of the PHYSET.CLKSEL[1:0] codepoint that
 *        produced PLL lock during the bisect harness.
 *
 * @details Layout: low nibble is the masked PHYSET CLKSEL field
 * (0x00, 0x10, 0x20, or 0x30 corresponding to 12 / 48 / 20 / 24 MHz
 * per HUM Ch 37.2.17 PHYSET p 2080 Table). Sentinel 0xFF means the
 * full 4-codepoint sweep completed without observing PLLSTA.PLLLOCK.
 *
 * @note Read-only from outside; written only by ::internal_usbhs_phy_bringup.
 * @warning Direct modification breaks the diagnostic invariant.
 * @since 0.1.0
 */
static volatile uint8_t s_clksel_winner = (uint8_t)k_ra_usbhs_clksel_no_winner;

/**
 * @var s_clksel_attempt_pllsta
 * @brief Per-attempt PLLSTA capture array for the bisect harness.
 *
 * @details Index = attempt number (0..3). Each slot captures the
 * PLLSTA value observed at the end of that attempt's polling window
 * so a JLink read of all four words tells the operator which CLKSEL
 * codepoints saw any partial lock activity.
 *
 * @note Read-only from outside.
 * @warning Direct modification breaks the diagnostic invariant.
 * @since 0.1.0
 */
static volatile uint16_t s_clksel_attempt_pllsta[4] = {0U, 0U, 0U, 0U};

/**
 * @brief Microsecond busy-wait helper for USB PHY bring-up.
 *
 * @details Cortex-M85 at 1 GHz: ~1000 cycles per microsecond. We use
 * a NOP loop calibrated to comfortably exceed 1 us so PHY power-up
 * settling completes. FSP `r_usb_preg_access.c` calls
 * `usb_cpu_delay_1us` -- this is the local equivalent.
 *
 * @pre Caller is in init context (busy-wait blocks the CPU).
 * @pre CPU clock is the one assumed by ``k_ra_usbhs_delay_1us_iters``.
 * @post No state mutation; loop iteration count is bounded.
 * @post At least 1 us has elapsed.
 * @note Not thread-safe; calibrated for 1 GHz Cortex-M85.
 * @since 0.1.0
 */
static void internal_usb_delay_1us(void)
{
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra_usbhs_delay_1us_iters;
       ++i) { /* GCOVR_EXCL_BR_LINE */
    __asm__ volatile("nop");
  }
}

/**
 * @brief Drive USBHS SYSCFG DRPD=0, USBE=1 after PLLRESET is released.
 *
 * @details Splits the SYSCFG mutation off from
 * ::internal_usbhs_phy_bringup so each helper stays under the NASA
 * Rule 4 / clang-tidy size budget. HSE is set separately at the start
 * of the bring-up (mirrors FSP `hw_usb_set_hse` which is called BEFORE
 * `hw_usb_pmodule_init` -- r_usb_basic.c). Captures the
 * SYSCFG-after-USBE value into the probe word's high half.
 *
 * @param[in] reg HS register block pointer.
 *
 * @pre PHYSET DIRPD/PLLRESET are released.
 * @pre SYSCFG.HSE is already 1.
 * @pre s_usbhs_init_probe low half holds PHYSET-after-power-up.
 * @post SYSCFG: DRPD=0, USBE=1, HSE=1.
 * @post s_usbhs_init_probe high half = SYSCFG.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static void internal_usbhs_enable_syscfg(volatile r_usb_regs_t* reg)
{
  /* HUM Ch 37.2.1 "SYSCFG : System Configuration Control Register", p 2060 */
  reg->SYSCFG = (uint16_t)(reg->SYSCFG & (uint16_t)~(uint16_t)(1U << k_ra_syscfg_bit_drpd));
  reg->SYSCFG = (uint16_t)(reg->SYSCFG | (uint16_t)(1U << k_ra_syscfg_bit_usbe));

  const uint32_t lo  = s_usbhs_init_probe & (uint32_t)k_ra_usbhs_probe_lo_mask;
  const uint32_t hi  = (uint32_t)reg->SYSCFG << (uint32_t)k_ra_usbhs_probe_hi_shift;
  s_usbhs_init_probe = lo | hi;
}

/**
 * @brief Bounded PHY PLL-lock poll for a single CLKSEL attempt.
 *
 * @details Polls PLLSTA.PLLLOCK up to a ~50 ms ceiling. Captures the
 * final PLLSTA value into ``s_usbhs_pllsta_probe`` regardless of
 * outcome so JLink reads can see the chip's view of the PLL state.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok PLLLOCK observed within the per-attempt window.
 * @retval k_ra_err_hw_timeout PLLLOCK never asserted.
 *
 * @pre LPSTS.SUSPENDM = 1 and PHYSET is configured for the chosen
 *      CLKSEL value.
 * @pre USB60CLK is running (PLL2P / 4 = 60 MHz).
 * @post Loop iteration count is bounded.
 * @post s_usbhs_pllsta_probe holds the final PLLSTA word.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static ra_err_t internal_usbhs_wait_pll_lock_short(void)
{
#ifdef RA_SIMULATOR_MODE
  /* No real PHY in simulator -- PLLSTA is always 0; skip the poll. */
  s_usbhs_pllsta_probe = 0U;
  return k_ra_ok;
#else
  volatile uint16_t* const pllsta = ra_usbhs_pllsta();
  ra_err_t                 lock   = k_ra_err_hw_timeout;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_usbhs_pll_lock_attempt_limit;
       ++i) {                                              /* GCOVR_EXCL_BR_LINE */
    if ((*pllsta & (uint16_t)k_ra_pllsta_plllock) != 0U) { /* GCOVR_EXCL_BR_LINE */
      lock = k_ra_ok;
      break;
    }
  }
  s_usbhs_pllsta_probe = *pllsta;
  return lock;
#endif
}

/**
 * @brief Drive PHYSET into "PHY in reset" with the given CLKSEL field.
 *
 * @details Asserts DIRPD=1 and PLLRESET=1, then writes the chosen
 * CLKSEL[1:0] codepoint. HUM Ch 37.2.17 PHYSET, p 2080 Table:
 * 00b=12 MHz, 01b=48 MHz, 10b=20 MHz, 11b=24 MHz. Used by both
 * the canonical bring-up path and the CLKSEL bisect harness.
 *
 * @param[in] physet_reg PHYSET register pointer (::ra_usbhs_physet()).
 * @param[in] clksel_value Pre-shifted CLKSEL field value (0x00, 0x10,
 *                         0x20, or 0x30).
 *
 * @pre physet_reg non-null.
 * @pre LPSTS.SUSPENDM may be 0 or 1; HUM Ch 37.2.43 LPSTS, p 2111
 *      states only a small whitelist of registers can be written
 *      while SUSPENDM=0 and PHYSET is not on it. The harness clears
 *      SUSPENDM-then-sets it again per attempt to flush latched
 *      writes regardless.
 * @post PHYSET: DIRPD=1, PLLRESET=1, CLKSEL field = clksel_value.
 * @post No other PHYSET fields (REPSEL, HSEB, CDPEN) are touched.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static void internal_usbhs_arm_phy_reset(volatile uint16_t* physet_reg, uint16_t clksel_value)
{
  /* HUM Ch 37.2.17 PHYSET, p 2080: assert PHY power-down and PLL
   * reset, mask out CLKSEL[1:0], then OR in the candidate field. */
  uint16_t v  = *physet_reg;
  v           = (uint16_t)(v | (uint16_t)k_ra_physet_dirpd | (uint16_t)k_ra_physet_pllreset);
  v           = (uint16_t)(v & (uint16_t)~(uint16_t)k_ra_physet_clksel);
  v           = (uint16_t)(v | (uint16_t)(clksel_value & (uint16_t)k_ra_physet_clksel));
  *physet_reg = v;
}

/**
 * @brief One iteration of the CLKSEL bisect: try a single codepoint.
 *
 * @details For the given CLKSEL value:
 *   1. Toggle LPSTS.SUSPENDM 1->0->1 to flush latched PHYSET writes
 *      (HUM Ch 37.2.43 LPSTS p 2111: writes to USBHS regs only take
 *      effect once the PHY clock oscillates, which happens when
 *      SUSPENDM is set to 1).
 *   2. Arm PHYSET in reset with this CLKSEL.
 *   3. Release DIRPD (1 us settle), wait 1 ms, release PLLRESET.
 *   4. Set LPSTS.SUSPENDM = 1.
 *   5. Poll PLLSTA.PLLLOCK with the per-attempt budget.
 *
 * @param[in] physet PHYSET register pointer.
 * @param[in] lpsts  LPSTS register pointer.
 * @param[in] clksel_value PHYSET CLKSEL field value (0x00/10/20/30).
 *
 * @return ::ra_err_t
 * @retval k_ra_ok PLL locked under this CLKSEL.
 * @retval k_ra_err_hw_timeout PLL did not lock within the per-attempt
 *                              window.
 *
 * @pre SYSCFG.HSE = 1.
 * @pre physet and lpsts pointers are non-null and point at the HS
 *      USBHS register block.
 * @post On success: PHY powered, PLLRESET cleared, SUSPENDM=1.
 * @post On failure: SUSPENDM=1 is left asserted; caller may retry the
 *       next CLKSEL value or surface the timeout.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static ra_err_t internal_usbhs_try_clksel(volatile uint16_t* physet,
                                          volatile uint16_t* lpsts,
                                          uint16_t           clksel_value)
{
  /* Drop SUSPENDM so the PHY clock is gated; subsequent PHYSET writes
   * are then sequenced through a single SUSPENDM 0->1 transition. */
  *lpsts = (uint16_t)(*lpsts & (uint16_t)~(uint16_t)k_ra_lpsts_suspendm);
  internal_usb_delay_1us();

  internal_usbhs_arm_phy_reset(physet, clksel_value);
  internal_usb_delay_1us();

  /* Release DIRPD (PHY analog up), wait 1 ms, release PLLRESET. */
  *physet = (uint16_t)(*physet & (uint16_t)~(uint16_t)k_ra_physet_dirpd);
  ra_delay_ms(1U);
  *physet = (uint16_t)(*physet & (uint16_t)~(uint16_t)k_ra_physet_pllreset);

  /* HUM Ch 37.2.43 LPSTS, p 2111: SUSPENDM=1 starts the PHY clock,
   * which both commits any previously-latched PHYSET writes and
   * begins the PLL lock countdown. */
  *lpsts = (uint16_t)(*lpsts | (uint16_t)k_ra_lpsts_suspendm);

  return internal_usbhs_wait_pll_lock_short();
}

/**
 * @brief USBHS embedded-PHY bring-up (HUM Figure 37.2 p 2121).
 *
 * @details Implements the device-mode PHY bring-up flow exactly as
 * documented in HUM Ch 37.3.3 + Figure 37.2. The PHY UTMI internal PLL
 * takes EXTAL directly (24 MHz on EK-RA8D2); USB60CLK = 60 MHz is the
 * separate LINK domain clock supplied by ::ra_cgc_usbhs_pll_enable.
 *  1. SYSCFG |= HSE                   (high-speed enable, before PHY)
 *  2. PHYSET |= DIRPD | PLLRESET; CLKSEL = 11b (24 MHz)
 *  3. delay 1 us                      (HUM "Wait 1 us" after CLKSEL)
 *  4. PHYSET &= ~DIRPD                (release PHY analog power-down)
 *  5. delay 1 ms                      (HUM "Wait 1 ms" after DIRPD)
 *  6. PHYSET &= ~PLLRESET             (release UTMI PHY PLL reset)
 *  7. LPSTS  |= SUSPENDM              (start the PHY clock)
 *  8. wait PLLSTA.PLLLOCK = 1         (UTMI PLL lock confirmation)
 *  9. SYSCFG &= ~DRPD; |= USBE        (enable module operation)
 * 10. BUSWAIT = 0x0F04                (4-wait + reserved b11-b8)
 * 11. PHYSET |= REPSEL_16             (terminator adjust cycle)
 *
 * The HS instance has no SCKE bit (HUM Ch 37.2.1 "SYSCFG" register
 * layout, p 2060); writing bit 10 there is a no-op.
 *
 * @param[in] reg HS register block pointer (must be ::ra_usb_hs()).
 *
 * @return ::ra_err_t
 * @retval k_ra_ok HS PHY locked and ready for ``DPRPU`` attach.
 * @retval k_ra_err_hw_timeout PLLSTA.PLLLOCK never asserted.
 *
 * @pre MSTPB12 ungated; USB60CLK = PLL2P / 4 = 60 MHz.
 * @pre Caller is single-threaded init context.
 *
 * @post On success: PHY powered, PLL locked, USBE=1, SUSPENDM=1.
 * @post BUSWAIT programmed.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static ra_err_t internal_usbhs_phy_bringup(volatile r_usb_regs_t* reg)
{
  volatile uint16_t* const physet = ra_usbhs_physet();
  volatile uint16_t* const lpsts  = ra_usbhs_lpsts();

  /* Step 1: SYSCFG.HSE = 1 (mirrors FSP `hw_usb_set_hse`, called from
   * r_usb_basic.c BEFORE hw_usb_pmodule_init). HUM Ch 37 "SYSCFG"
   * register, p 2060. */
  reg->SYSCFG      = (uint16_t)(reg->SYSCFG | (uint16_t)(1U << k_ra_syscfg_bit_hse));
  s_phy_step_probe = (uint8_t)k_ra_usbhs_phy_step_hse_set;

  /* Step 2: CLKSEL = 24 MHz (PHYSET[5:4] = 11b, value 0x30).
   *
   * Per HUM Ch 37.3.3 "Supplying the Clock", Table 37.17 (p 2120) the
   * USB-PHY internal PLL takes its reference clock DIRECTLY FROM THE
   * EXTAL PIN -- not from USB60CLK. Allowed EXTAL frequencies are 12,
   * 20, 24 or 48 MHz; PHYSET.CLKSEL[1:0] selects which one.
   *
   * EK-RA8D2 fits a 24 MHz crystal (see ra_cgc.c::k_ra_pll2_local_t
   * comments), so CLKSEL must be 11b (k_ra_physet_clksel_24). USB60CLK
   * (60 MHz, PLL2P/4) is still required -- it is the LINK domain clock
   * for the USBHS module itself -- but it does NOT feed the PHY UTMI
   * PLL. Earlier revisions of this code mis-modeled CLKSEL as
   * "USB60CLK / 5 = 12 MHz" and bisected over {12,48,24,20}; the host
   * never enumerated because the PHY PLL was locking at the wrong
   * frequency, producing malformed HS chirp K signaling. */
  s_phy_step_probe  = (uint8_t)k_ra_usbhs_phy_step_clksel_12;
  ra_err_t lock_err = internal_usbhs_try_clksel(physet, lpsts, (uint16_t)k_ra_physet_clksel_24);
  s_clksel_attempt_pllsta[0] = s_usbhs_pllsta_probe;
  s_usbhs_init_probe         = (uint32_t)*physet;

  if (lock_err != k_ra_ok) {
    s_clksel_winner = (uint8_t)k_ra_usbhs_clksel_no_winner;
    ra_log_error(s_tag, "usbhs: PHY PLL lock timeout (CLKSEL=24)");
    return lock_err;
  }
  s_clksel_winner  = (uint8_t)k_ra_physet_clksel_24;
  s_phy_step_probe = (uint8_t)k_ra_usbhs_phy_step_pll_locked;

  /* HUM Figure 37.2 p 2121: USBE/DRPD are programmed AFTER PLLLOCK
   * is observed. Internally splits into a separate helper to keep
   * each function under the NASA Rule 4 budget. */
  internal_usbhs_enable_syscfg(reg);
  s_phy_step_probe = (uint8_t)k_ra_usbhs_phy_step_usbe_set;

  /* HUM Ch 37.2.2 "BUSWAIT : CPU Bus Wait Register", p 2062 */
  reg->BUSWAIT = (uint16_t)k_ra_buswait_default;
  *physet      = (uint16_t)(*physet | (uint16_t)k_ra_physet_repsel_16);
  return k_ra_ok;
}

/**
 * @brief Programme the post-SYSCFG common registers (FIFO, DCP, INTENB).
 *
 * @details Shared tail of FS and HS device-mode bring-up: CFIFOSEL,
 * DCP defaults, and the device-mode INTENB0 interrupt mask.
 *
 * @param[in] reg Selected USB instance register block.
 *
 * @pre Caller has already enabled SYSCFG.USBE.
 * @pre reg is non-null.
 * @post All listed registers carry deterministic device-mode defaults.
 * @post INTENB1 / BRDYENB / NRDYENB / BEMPENB are cleared.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static void internal_usb_init_common(volatile r_usb_regs_t* reg)
{
  /* HUM Ch 36.2.7 "CFIFOSEL : CFIFO Port Select Register", p 1976 */
  /* HUM Ch 37.2.8 "CFIFOSEL : CFIFO Port Select Register", p 2071 */
  reg->CFIFOSEL  = k_ra_fifosel_mbw_16;
  reg->D0FIFOSEL = k_ra_fifosel_mbw_16;
  reg->D1FIFOSEL = k_ra_fifosel_mbw_16;

  /* HUM Ch 36.2.20 "DCPMAXP : DCP Max Packet Size Register", p 1990 */
  /* HUM Ch 37.2.30 "DCPMAXP : DCP Max Packet Size Register", p 2092 */
  reg->DCPCFG  = 0U;
  reg->DCPMAXP = k_ra_usb_dcp_max_packet;
  reg->DCPCTR  = 0U;

  /* HUM Ch 37.2.18 INTSTS0 Note 3 (p 2082): "To clear the CTRT, DVST,
   * SOFR, RESM, or VBINT flags, write 0 only to the flags to be cleared.
   * Write 1 to the other flags. Do not write 0 to the status flags
   * indicating 0." A wholesale ``INTSTS0 = 0`` write therefore clears
   * any stale CTRT/DVST/SOFR/RESM/VBINT edges that the PHY bring-up
   * sequence latched (mirrors FSP IP1 ``hw_usb_pmodule_init``: see
   * r_usb_preg_access.c::hw_usb_pmodule_init which writes
   * ``INTSTS0 = 0`` immediately before INTENB0 on the HS branch).
   * Without this flush a stale RESM/VBINT bit gets ORed into every
   * polled-dispatch snapshot and clutters the JLink-readable
   * ``s_intsts0_observed_or`` probe (observed value 0xD0D0 with bits
   * 14/15 set even though the host issued no real resume). */
  reg->INTSTS0 = 0U;

  /* HUM Ch 36.2.10 "INTENB0 : Interrupt Enable Register 0", p 1980.
   * Mirrors FSP r_usb_basic.c::usb_pmodule_init mask: BEMP, BRDY, NRDY,
   * CTRT, DVST, SOFR, RSME, VBSE. Enabling SOFR + RSME is required for
   * HS so the IP latches resume / SOF events in INTSTS0; without RSME
   * a host wake from suspend leaves the device asleep. */
  /* SOFR and RSME are intentionally NOT enabled here:
   *   - SOFR fires every 125us on HS (= 8 kHz);
   *   - RSME stays asserted on USBHS via the PHY's USBR signal
   *     while the host holds the bus in resume signalling;
   * Both starve PendSV and the demo worker thread never gets
   * scheduled. NRDY is still useful (drives the bridge's per-pipe
   * NAK re-arm) and does not cause a storm on its own. RSME / SOFR
   * status is still readable from INTSTS0 for poll-style drivers. */
  reg->INTENB0 = (uint16_t)((1U << k_ra_int0_bit_bemp) | (1U << k_ra_int0_bit_brdy) |
                            (1U << k_ra_int0_bit_nrdy) | (1U << k_ra_int0_bit_ctrt) |
                            (1U << k_ra_int0_bit_dvst) | (1U << k_ra_int0_bit_vbse));
  reg->INTENB1 = 0U;
  reg->BRDYENB = 0U;
  reg->NRDYENB = 0U;
  reg->BEMPENB = 0U;
}

/**
 * @brief USBFS module bring-up (FSP `hw_usb_pmodule_init` IP0 branch).
 *
 * @details Sets SCKE, polls SCKE-readback, clears DRPD, sets USBE.
 * The FS instance has no PHY-side registers (PHYSET / LPSTS / PLLSTA
 * are HS-only) so this is a pure SYSCFG-driven sequence.
 *
 * @param[in] reg FS register block pointer (must be ::ra_usb_fs()).
 *
 * @return ::ra_err_t
 * @retval k_ra_ok USBE asserted.
 *
 * @pre MSTPB11 ungated; USB48CLK fed from PLL2P/5 = 48 MHz.
 * @pre Caller is single-threaded init context.
 *
 * @post SYSCFG: SCKE=1, DRPD=0, USBE=1.
 * @post Loop iteration count is bounded.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static ra_err_t internal_usbfs_module_bringup(volatile r_usb_regs_t* reg)
{
  /* HUM Ch 36.2.1 "SYSCFG : System Configuration Control Register", p 1966 */
  reg->SYSCFG = (uint16_t)(reg->SYSCFG | (uint16_t)(1U << k_ra_syscfg_bit_scke));
  for (uint32_t i = 0U; i < (uint32_t)k_ra_usbhs_scke_poll_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((reg->SYSCFG & (uint16_t)(1U << k_ra_syscfg_bit_scke)) != 0U) {  /* GCOVR_EXCL_BR_LINE */
      break;
    }
  }
  reg->SYSCFG = (uint16_t)(reg->SYSCFG & (uint16_t)~(uint16_t)(1U << k_ra_syscfg_bit_drpd));
  reg->SYSCFG = (uint16_t)(reg->SYSCFG | (uint16_t)(1U << k_ra_syscfg_bit_usbe));
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_device_init (see header for full contract).
 * @details Dispatches FS vs HS bring-up, then programmes shared FIFO,
 *          DCP, and INTENB0 fields.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @retval k_ra_err_invalid_arg Invalid speed.
 * @retval k_ra_err_hw_timeout HS PHY PLL lock timeout.
 * @pre MSTP and clock subsystem are armed.
 * @pre Caller is single-threaded init context.
 * @post Module powered and SYSCFG.USBE = 1.
 * @post INTENB0 carries device-mode interrupt mask.
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
ra_err_t ra_usb_device_init(ra_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  const ra_err_t mst_err = ra_mstp_enable(internal_mstp(speed));
  RA_RETURN_ON_ERROR(mst_err, s_tag, "usb_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  if (speed == k_ra_usb_speed_hs) {
    const ra_err_t phy_err = internal_usbhs_phy_bringup(reg);
    RA_RETURN_ON_ERROR(phy_err, s_tag, "usb_init: HS PHY bring-up"); /* GCOVR_EXCL_BR_LINE */
  } else {
    const ra_err_t fs_err = internal_usbfs_module_bringup(reg);
    RA_RETURN_ON_ERROR(fs_err, s_tag, "usb_init: FS module bring-up"); /* GCOVR_EXCL_BR_LINE */
  }

  internal_usb_init_common(reg);

  /* Bisect probes: capture SYSCFG/LPSTS state at the END of device-init,
   * BEFORE control returns to ra_board_usbhs_device_init / the DCD
   * bridge. HUM Ch 37.2.1 SYSCFG p 2060, HUM Ch 37.2.43 LPSTS p 2111. */
  if (speed == k_ra_usb_speed_hs) {
    s_syscfg_after_phy_bringup = reg->SYSCFG;
    s_lpsts_after_phy_bringup  = *ra_usbhs_lpsts();
  }

  ra_log_info_val(s_tag, "usb device init speed", (uint32_t)speed);
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_device_deinit (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_device_deinit(ra_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 36.2.10 "INTENB0 : Interrupt Enable Register 0", p 1980 */
  reg->INTENB0 = 0U;
  reg->INTENB1 = 0U;
  reg->BRDYENB = 0U;
  reg->NRDYENB = 0U;
  reg->BEMPENB = 0U;

  /* HUM Ch 36.2.1 "SYSCFG : System Configuration Control Register", p 1966 */
  reg->SYSCFG = 0U;
  return ra_mstp_disable(internal_mstp(speed));
}

/**
 * @brief Implementation of ra_usb_device_attach (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] attached See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_device_attach(ra_usb_speed_t speed, bool attached)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 36.2.1 "SYSCFG : System Configuration Control Register", p 1966 */
  /* HUM Ch 37.2.1 "SYSCFG : System Configuration Control Register", p 2060 */
  const uint16_t dprpu = (uint16_t)(1U << k_ra_syscfg_bit_dprpu);
  const uint16_t cnen  = (uint16_t)(1U << k_ra_syscfg_bit_cnen);

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
    if (speed == k_ra_usb_speed_hs) {
      internal_rmw16(&reg->SYSCFG, cnen, 0U);
      s_syscfg_before_dprpu = reg->SYSCFG;
    }
    internal_rmw16(&reg->SYSCFG, dprpu, 0U);
  } else {
    internal_rmw16(&reg->SYSCFG, 0U, dprpu);
    if (speed == k_ra_usb_speed_hs) {
      /* HUM Ch 37.2.1 p 2062: clear CNEN when VBUS is removed /
       * detach is requested, to avoid through-current on floating
       * D+/D- when the cable is unplugged. */
      internal_rmw16(&reg->SYSCFG, 0U, cnen);
    }
  }

  /* Bisect probes: capture SYSCFG/LPSTS/SYSSTS0 at end of attach. HUM
   * Ch 37.2.1 SYSCFG p 2060, HUM Ch 37.2.3 SYSSTS0 p 2063, HUM
   * Ch 37.2.43 LPSTS p 2111. */
  if (speed == k_ra_usb_speed_hs) {
    s_syscfg_after_attach  = reg->SYSCFG;
    s_lpsts_after_attach   = *ra_usbhs_lpsts();
    s_syssts0_after_attach = reg->SYSSTS0;
  }
  return k_ra_ok;
}

/* =============================================================================
 * Status / state
 * =============================================================================
 */

/**
 * @brief Implementation of ra_usb_get_status (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] out_mask See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_get_status(ra_usb_speed_t speed, uint16_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1985 */
  *out_mask = reg->INTSTS0;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_clear_status (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] mask See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_clear_status(ra_usb_speed_t speed, uint16_t mask)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1985 */
  reg->INTSTS0 = (uint16_t)(reg->INTSTS0 & (uint16_t)~mask);
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_get_device_state (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] out_state See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_get_device_state(ra_usb_speed_t speed, ra_usb_dev_state_t* out_state)
{
  RA_CHECK_NULL_PTR(out_state, s_tag, "out_state must not be nullptr");
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1985 */
  *out_state = internal_decode_dvsq(reg->INTSTS0);
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_set_address (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] address See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_set_address(ra_usb_speed_t speed, uint8_t address)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (address > k_ra_usb_max_address) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 36.2.16 "USBADDR : USB Address Register", p 1988 */
  /* HUM Ch 37.2.16 "USBADDR : USB Address Register", p 2080 */
  reg->USBADDR = (uint16_t)((uint16_t)address & k_ra_usbaddr_addr_mask);
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_device_busreset_rearm (see header for full contract).
 *
 * @details Mirrors FSP `usb_pstd_bus_reset` (r_usb_basic/src/driver/r_usb_psignal.c):
 * re-programmes DCPCFG / DCPMAXP / DCPCTR, clears PIPECTR[1..9], and
 * re-applies the device-mode INTENB0 mask. The IP requires this on
 * every transition to the Default state or it silently NAKs the host's
 * first SETUP and the host loops back to bus reset.
 *
 * @param[in] speed See header.
 * @return Result code (see header).
 * @retval k_ra_ok DCP re-armed.
 * @retval k_ra_err_invalid_arg speed out of range.
 * @pre Caller observed DVST with DVSQ == default.
 * @pre ra_usb_device_init has run for this speed.
 * @post DCP regs re-defaulted; INTENB0 mask re-applied.
 * @post All PIPECTR PID fields cleared (NAK).
 * @note Safe in IRQ-callback context (no spin loops).
 * @since 0.1.0
 */
/**
 * @brief Reset DCP defaults + clear DCP FIFO on bus-reset rearm.
 * @details Helper extracted from ::ra_usb_device_busreset_rearm to keep
 *          the top-level function under the clang-tidy line budget.
 *          Re-asserts DCPCFG=0 and DCPMAXP=64, then pulses
 *          CFIFOCTR.BCLR (HUM Ch 36.2.8 / 37.2.10 p 1979 / 2073) to
 *          wipe any bytes a previous incomplete control transfer left
 *          in the DCP FIFO. Without the BCLR pulse the next
 *          ra_usb_dcp_in_data sees FRDY=0, times out, and the DCD
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
static void internal_dcp_reset_defaults(volatile r_usb_regs_t* reg)
{
  reg->DCPCFG  = 0U;
  reg->DCPMAXP = k_ra_usb_dcp_max_packet;
  internal_select_cfifo(reg, 0U, true);
  reg->CFIFOCTR = (uint16_t)k_ra_fifoctr_bclr;
}

/**
 * @brief Implementation of ra_usb_device_busreset_rearm (see header).
 * @details Re-default DCPCFG/DCPMAXP, clear all PIPECTR[*], drop
 *          BRDYSTS/NRDYSTS/BEMPSTS, clear the DCP FIFO via
 *          CFIFOCTR.BCLR, and re-arm INTENB0 with the post-init mask.
 *          Mirrors FSP r_usb_psignal.c::usb_pstd_bus_reset.
 * @param[in] speed Which controller (FS or HS).
 * @return ra_err_t result code.
 * @retval k_ra_ok               Success.
 * @retval k_ra_err_invalid_arg  speed out of range.
 * @pre Module clock and power are on.
 * @pre Caller is in IRQ-callback context (DVST=Default branch).
 * @post DCPCFG=0, DCPMAXP=64, DCP FIFO cleared, INTENB0 re-armed.
 * @post All non-control PIPECTR[*] cleared (PID=NAK).
 * @note Not thread-safe; caller holds the DCP lock.
 * @since 0.1.0
 */
ra_err_t ra_usb_device_busreset_rearm(ra_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 36.2.27 "PIPECTR : Pipe n Control Register", p 1998.
   * Clear PID for every non-control pipe so any half-completed pre-
   * reset transfer is forgotten. The class driver re-issues queue_in /
   * queue_out which will set PID=BUF when ready. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra_usb_pipectr_count; i++) {
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
   * internal_usb_init_common. */
  /* SOFR and RSME are intentionally NOT enabled here:
   *   - SOFR fires every 125us on HS (= 8 kHz);
   *   - RSME stays asserted on USBHS via the PHY's USBR signal
   *     while the host holds the bus in resume signalling;
   * Both starve PendSV and the demo worker thread never gets
   * scheduled. NRDY is still useful (drives the bridge's per-pipe
   * NAK re-arm) and does not cause a storm on its own. RSME / SOFR
   * status is still readable from INTSTS0 for poll-style drivers. */
  reg->INTENB0 = (uint16_t)((1U << k_ra_int0_bit_bemp) | (1U << k_ra_int0_bit_brdy) |
                            (1U << k_ra_int0_bit_nrdy) | (1U << k_ra_int0_bit_ctrt) |
                            (1U << k_ra_int0_bit_dvst) | (1U << k_ra_int0_bit_vbse));

  return k_ra_ok;
}

/* =============================================================================
 * Endpoints
 * =============================================================================
 */

/**
 * @brief Build the PIPECFG word for a non-control endpoint.
 *
 * @details See implementation.
 * @param[in] ep_addr See implementation.
 * @param[in] dir See implementation.
 * @param[in] type See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
/**
 * @brief Compute the PIPEBUF word for a bulk pipe (2*MPS region).
 *
 * @details
 * Statically partition the controller's internal FIFO RAM among the
 * bulk pipes. Reserve blocks 0..7 (64-byte units) for the DCP (per
 * FSP/Renesas examples), then pack user pipes in pipe-number order
 * with 2*MPS per pipe so PIPECFG.DBLB has two banks.
 *
 *   BUFNMB  = 8 + (pipe_num - 1) * blocks_per_pipe
 *   BUFSIZE = (2 * MPS / 64) - 1
 *
 *   HS MPS=512 -> blocks_per_pipe = 16, BUFSIZE = 15
 *   FS MPS=64  -> blocks_per_pipe = 2,  BUFSIZE = 1
 *
 * @param[in] pipe_num   PIPE number 1..9.
 * @param[in] max_packet Pipe MPS (bytes).
 *
 * @return PIPEBUF word ready to be written to register ``PIPEBUF``.
 * @retval 0..0xFFFF Packed BUFNMB/BUFSIZE word (no error condition; the
 *                   helper is total over its enum-checked inputs).
 *
 * @pre ``pipe_num`` is in the 1..9 range (caller-checked).
 * @pre ``max_packet`` is the pipe's MPS (caller validated <= 1024).
 * @post No global state is touched; the helper is pure.
 * @post Returned word satisfies HUM Ch 36.2.25 PIPEBUF layout.
 *
 * @note Pure / thread-safe.
 * @since 0.1.0
 */
static uint16_t internal_pipebuf_word(uint8_t pipe_num, uint16_t max_packet)
{
  const uint16_t mps_blocks =
    (uint16_t)(((uint32_t)max_packet + (k_ra_pipebuf_block_bytes - 1U)) / k_ra_pipebuf_block_bytes);
  const uint16_t blocks_per_pipe = (uint16_t)(mps_blocks * 2U);
  const uint16_t bufsize_field   = (uint16_t)(blocks_per_pipe - 1U);
  const uint16_t bufnmb_field =
    (uint16_t)(8U + (uint16_t)((uint16_t)(pipe_num - 1U) * blocks_per_pipe));
  return (uint16_t)((bufsize_field << k_ra_pipebuf_bufsize_shift) |
                    (bufnmb_field & k_ra_pipebuf_bufnmb_mask));
}

/**
 * @brief Pack PIPECFG fields for a configured non-control pipe.
 *
 * @details Encodes endpoint number, direction (DIR), pipe type (TYPE),
 * and, for bulk pipes, the SHTNAK + DBLB flags into the PIPECFG word.
 * HUM Ch 36.2.24 PIPECFG / DBLB; we always enable hardware double-
 * buffering on bulk pipes so the matching PIPEBUF (2*MPS) gives two
 * banks of MPS bytes each.
 *
 * @param[in] ep_addr Endpoint address (low nibble = EP number; bit 7
 *                    direction; the helper reads only the EP number).
 * @param[in] dir     Pipe direction (::k_ra_usb_ep_dir_in or _out).
 * @param[in] type    Pipe type (bulk / interrupt / iso).
 *
 * @return PIPECFG word ready to write to ``PIPECFG``.
 * @retval 0..0xFFFF Packed configuration word; no error condition.
 *
 * @pre ``ep_addr`` low nibble is the EP number (caller validated 1..15).
 * @pre ``dir`` / ``type`` are valid enum values.
 * @post No global state is touched; the helper is pure.
 * @post For bulk pipes the SHTNAK + DBLB flags are set in the result.
 *
 * @note Pure / thread-safe.
 * @since 0.1.0
 */
static uint16_t internal_pipecfg_word(uint8_t ep_addr, ra_usb_ep_dir_t dir, ra_usb_ep_type_t type)
{
  uint16_t cfg = (uint16_t)((uint16_t)ep_addr & k_ra_pipecfg_epnum_mask);
  if (dir == k_ra_usb_ep_dir_in) {
    cfg = (uint16_t)(cfg | k_ra_pipecfg_dir_in);
  }
  if (type == k_ra_usb_ep_type_bulk) {
    cfg = (uint16_t)(cfg | k_ra_pipecfg_type_bulk);
    cfg = (uint16_t)(cfg | k_ra_pipecfg_shtnak);
    /* HUM Ch 36.2.24 PIPECFG.DBLB: enable hardware double-buffering on
     * bulk pipes. With matching PIPEBUF (2*MPS per pipe) and the
     * DBLB-aware drain in queue_out's post-BCLR path (drains both
     * banks before clearing BRDYSTS), the controller can have one
     * bank on the wire while the CPU loads the other, doubling
     * sustained throughput. */
    cfg = (uint16_t)(cfg | k_ra_pipecfg_dblb);
  } else if (type == k_ra_usb_ep_type_intr) {
    cfg = (uint16_t)(cfg | k_ra_pipecfg_type_intr);
  } else {
    cfg = (uint16_t)(cfg | k_ra_pipecfg_type_iso);
  }
  return cfg;
}

/**
 * @brief Validate the argument set for `ra_usb_configure_endpoint`.
 *
 * @return k_ra_ok if all arguments are in range.
 *
 * @details See implementation.
 * @param[in] pipe_num See implementation.
 * @param[in] ep_addr See implementation.
 * @param[in] dir See implementation.
 * @param[in] type See implementation.
 * @param[in] max_packet See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_check_ep_args(uint8_t          pipe_num,
                                       uint8_t          ep_addr,
                                       ra_usb_ep_dir_t  dir,
                                       ra_usb_ep_type_t type,
                                       uint16_t         max_packet)
{
  if ((pipe_num == 0U) || (pipe_num > k_ra_usb_max_pipe_num)) {
    return k_ra_err_invalid_arg;
  }
  if ((ep_addr == 0U) || (ep_addr > k_ra_usb_max_ep_addr)) {
    return k_ra_err_invalid_arg;
  }
  if ((dir != k_ra_usb_ep_dir_in) && (dir != k_ra_usb_ep_dir_out)) {
    return k_ra_err_invalid_arg;
  }
  if (type > k_ra_usb_ep_type_iso) {
    return k_ra_err_invalid_arg;
  }
  if ((max_packet == 0U) || (max_packet > k_ra_usb_pipe_max_packet)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Quiesce the pipe so PIPECFG/PIPEMAXP/PIPEPERI become writable.
 * @details Clears BRDYENB/NRDYENB/BEMPENB for this pipe and forces
 *          PID=NAK (HUM Ch 36.2.24 NOTE 1, p 1996; mirrors STAR
 *          rx_usb_hw.c::internal_usb_quiesce_pipe).
 * @param[in,out] reg Controller register window.
 * @param[in] pipe_num Pipe index 1..9.
 * @pre reg is non-null and points at a powered controller.
 * @pre pipe_num in [1,9].
 * @post BRDY/NRDY/BEMPENB bit for pipe_num is cleared.
 * @post PIPECTR PID for pipe_num == NAK.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_pipe_quiesce(volatile r_usb_regs_t* reg, uint8_t pipe_num)
{
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  reg->BRDYENB            = (uint16_t)(reg->BRDYENB & (uint16_t)~pipe_bit);
  reg->NRDYENB            = (uint16_t)(reg->NRDYENB & (uint16_t)~pipe_bit);
  reg->BEMPENB            = (uint16_t)(reg->BEMPENB & (uint16_t)~pipe_bit);
  internal_pipe_pid(reg, pipe_num, k_ra_pid_nak);
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
static void
internal_pipe_finalize(volatile r_usb_regs_t* reg, uint8_t pipe_num, ra_usb_ep_dir_t dir)
{
  const uint8_t  ctr_idx  = (uint8_t)(pipe_num - 1U);
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  internal_rmw16(&reg->PIPECTR[ctr_idx], k_ra_pipectr_sqclr, 0U);
  internal_rmw16(&reg->PIPECTR[ctr_idx], k_ra_pipectr_aclrm, 0U);
  internal_rmw16(&reg->PIPECTR[ctr_idx], 0U, k_ra_pipectr_aclrm);
  reg->BRDYSTS = (uint16_t)(~pipe_bit);
  reg->BEMPSTS = (uint16_t)(~pipe_bit);
  internal_pipe_pid(reg, pipe_num, (dir == k_ra_usb_ep_dir_out) ? k_ra_pid_buf : k_ra_pid_nak);
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
static void internal_pipe_arm_irq(volatile r_usb_regs_t* reg, uint8_t pipe_num, ra_usb_ep_dir_t dir)
{
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  if (dir == k_ra_usb_ep_dir_out) {
    reg->BRDYENB = (uint16_t)(reg->BRDYENB | pipe_bit);
  } else {
    reg->BEMPENB = (uint16_t)(reg->BEMPENB | pipe_bit);
  }
}

/**
 * @brief Implementation of ra_usb_configure_endpoint (see header for full contract).
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
 * @retval k_ra_ok Pipe configured and PID set per direction.
 * @retval k_ra_err_invalid_arg speed/pipe/ep/type/max_packet out of range.
 * @pre Controller is powered (ra_usb_device_init has run).
 * @pre Pipe is not currently mid-transfer (caller serialises).
 * @post PIPECFG/PIPEMAXP/PIPEPERI reflect the requested config.
 * @post BRDYENB or BEMPENB bit for pipe is set per direction.
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_usb_configure_endpoint(ra_usb_speed_t   speed,
                                   uint8_t          pipe_num,
                                   uint8_t          ep_addr,
                                   ra_usb_ep_dir_t  dir,
                                   ra_usb_ep_type_t type,
                                   uint16_t         max_packet)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t arg_err = internal_check_ep_args(pipe_num, ep_addr, dir, type, max_packet);
  if (arg_err != k_ra_ok) {
    return arg_err;
  }

  internal_pipe_quiesce(reg, pipe_num);

  /* HUM Ch 36.2.23 "PIPESEL : Pipe Window Select Register", p 1995 */
  reg->PIPESEL = pipe_num;
  /* HUM Ch 36.2.24 "PIPECFG : Pipe Configuration Register", p 1996 */
  reg->PIPECFG = internal_pipecfg_word(ep_addr, dir, type);
  /* HUM Ch 36.2.25 "PIPEBUF : Pipe Buffer Setting Register", p 2002.
   * Bulk pipes get a 2*MPS region so the matching PIPECFG.DBLB has
   * two banks of MPS bytes each. Interrupt/iso pipes keep the reset
   * default (single bank). */
  if (type == k_ra_usb_ep_type_bulk) {
    reg->PIPEBUF = internal_pipebuf_word(pipe_num, max_packet);
  }
  /* HUM Ch 36.2.26 "PIPEMAXP : Pipe Maximum Packet Size Register", p 2003 */
  reg->PIPEMAXP = max_packet;
  reg->PIPEPERI = 0U;
  /* Deselect window (FIT step 4) so a later stray PIPECFG write does
   * not land on this pipe. */
  reg->PIPESEL = 0U;

  internal_pipe_finalize(reg, pipe_num, dir);
  internal_pipe_arm_irq(reg, pipe_num, dir);
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_stall_endpoint (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] pipe_num See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_stall_endpoint(ra_usb_speed_t speed, uint8_t pipe_num)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (pipe_num > k_ra_usb_max_pipe_num) {
    return k_ra_err_invalid_arg;
  }
  if (pipe_num == 0U) {
    /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1991 */
    internal_dcp_pid(reg, k_ra_pid_stall);
  } else {
    /* HUM Ch 36.2.27 "PIPEnCTR : PIPE n Control Register", p 2005 */
    internal_pipe_pid(reg, pipe_num, k_ra_pid_stall);
  }
  return k_ra_ok;
}

/**
 * @brief Push `len` bytes through the CFIFO data port (16-bit packed).
 *
 * @details Mirrors `usb_pstd_write_fifo` (FSP `r_usb_preg_abs.c`)
 * for the 16-bit MBW path. The trailing byte is written via the
 * 8-bit halfword-low aperture of CFIFO using a halfword-aligned
 * left-padded write.
 *
 * @param[in] reg See implementation.
 * @param[in] data See implementation.
 * @param[in] len See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
/**
 * @enum ra_usb_fifo_shift_t
 * @brief Byte-shift constants for packing CFIFO writes.
 */
typedef enum : uint8_t {
  k_ra_usb_shift_b1 = 8U,  /**< Shift for byte 1. */
  k_ra_usb_shift_b2 = 16U, /**< Shift for byte 2. */
  k_ra_usb_shift_b3 = 24U, /**< Shift for byte 3. */
} ra_usb_fifo_shift_t;

/**
 * @brief HS-only: write the residual 0-3 bytes after 32-bit chunks.
 * @details FSP narrows CFIFOSEL.MBW to 16 then 8 for trailing
 *          halfword/byte. We save+restore MBW around these writes.
 * @param[in] reg HS register block.
 * @param[in] data Source byte pointer.
 * @param[in] len Total payload length.
 * @pre Caller already wrote (len & ~0x3) bytes via 32-bit access.
 * @pre CFIFOSEL.MBW currently == 32.
 * @post Tail bytes pushed; CFIFOSEL.MBW restored.
 * @post DTLN advanced by exactly (len & 0x3) bytes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void
internal_fifo_write_hs_tail(volatile r_usb_regs_t* reg, const uint8_t* data, uint16_t len)
{
  const uint16_t tail = (uint16_t)(len & 0x3U);
  if (tail == 0U) {
    return;
  }
  const uint16_t sel_save = reg->CFIFOSEL;
  const uint16_t sel_base = (uint16_t)(sel_save & (uint16_t)~(uint16_t)k_ra_fifosel_mbw_msk);
  uint16_t       offset   = (uint16_t)(len & (uint16_t)~(uint16_t)0x3U);
  /* For USBHS in little-endian mode, FSP writes the 16-bit residual
   * halfword to CFIFOH (CFIFO base + 0x02) and the 8-bit residual
   * byte to CFIFOHH (CFIFO base + 0x03), NOT to CFIFO itself. See
   * FSP r_usb_reg_access.h: USB1_CFIFO16=USB_M1->CFIFOH and
   * USB1_CFIFO8=USB_M1->CFIFOHH (in USB_CFG_LITTLE branch). The
   * chip uses these address aliases to commit the trailing 0..3
   * bytes when MBW is narrowed; a write to CFIFO at offset +0x00
   * in MBW=16/8 mode is silently dropped on USBHS, leaving DTLN
   * unchanged at 4N (verified empirically: 18-byte push showed
   * DTLN=16 instead of 18, and Linux saw -EOVERFLOW). */
  volatile uint16_t* const cfifoh  = (volatile uint16_t*)((uintptr_t)&reg->CFIFO + 2U);
  volatile uint8_t* const  cfifohh = (volatile uint8_t*)((uintptr_t)&reg->CFIFO + 3U);
  if ((tail & 0x2U) != 0U) {
    reg->CFIFOSEL     = (uint16_t)(sel_base | k_ra_fifosel_mbw_16);
    const uint16_t lo = (uint16_t)data[offset + 0U];
    const uint16_t hi = (uint16_t)data[offset + 1U];
    *cfifoh           = (uint16_t)(lo | (uint16_t)(hi << k_ra_usb_byte_bits));
    offset            = (uint16_t)(offset + 2U);
  }
  if ((tail & 0x1U) != 0U) {
    reg->CFIFOSEL = (uint16_t)(sel_base | k_ra_fifosel_mbw_8);
    *cfifohh      = data[offset];
  }
  reg->CFIFOSEL = sel_save;
}

/**
 * @brief HS-only: 32-bit CFIFO write loop for the head bytes.
 * @details Mirrors FSP hw_usb_write_fifo32: cast &CFIFO to uint32_t*
 *          and write `len/4` 32-bit words.
 * @param[in] reg HS register block.
 * @param[in] data Source byte pointer.
 * @param[in] len Total payload length; this helper writes only the
 *                head (len & ~0x3) bytes.
 * @pre CFIFOSEL.MBW == 32.
 * @pre data != NULL when len > 0.
 * @post DTLN advanced by exactly (len & ~0x3) bytes.
 * @post FIFO contains head bytes ready for BVAL commit.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void
internal_fifo_write_hs_head(volatile r_usb_regs_t* reg, const uint8_t* data, uint16_t len)
{
  volatile uint32_t* const cfifo32 = (volatile uint32_t*)(uintptr_t)&reg->CFIFO;
  const uint16_t           quads   = (uint16_t)(len >> 2U);
  for (uint16_t i = 0U; i < quads; ++i) {
    const uint32_t b0 = (uint32_t)data[(4U * i) + 0U];
    const uint32_t b1 = (uint32_t)data[(4U * i) + 1U];
    const uint32_t b2 = (uint32_t)data[(4U * i) + 2U];
    const uint32_t b3 = (uint32_t)data[(4U * i) + 3U];
    *cfifo32 =
      b0 | (b1 << k_ra_usb_shift_b1) | (b2 << k_ra_usb_shift_b2) | (b3 << k_ra_usb_shift_b3);
  }
}

/**
 * @brief Push a byte buffer into the CFIFO data port.
 * @details Dispatches to the speed-appropriate access width: USBHS
 *          uses 32-bit writes (with FSP-style 16/8 narrowing for the
 *          trailing 0..3 bytes), USBFS uses 16-bit writes with a
 *          single-byte tail. Mirrors FSP `usb_pstd_write_fifo` for
 *          IP1 / IP0 respectively.
 * @param[in] reg USB instance register block.
 * @param[in] data Source byte pointer (may be NULL when len == 0).
 * @param[in] len Number of bytes to push.
 * @pre Caller selected DCP / pipe with internal_select_cfifo and
 *      observed FRDY=1.
 * @pre data != NULL when len > 0.
 * @post DTLN advanced by len bytes.
 * @post FIFO ready for caller's BVAL commit.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_fifo_write(volatile r_usb_regs_t* reg, const uint8_t* data, uint16_t len)
{
  /* HUM Ch 36.2.5 / 37.2.7 CFIFO p 1973 / 2070. CFIFO access width
   * must match CFIFOSEL.MBW. USBHS = 32-bit, USBFS = 16-bit. */
  if (internal_is_hs(reg)) {
    internal_fifo_write_hs_head(reg, data, len);
    internal_fifo_write_hs_tail(reg, data, len);
    return;
  }
  /* USBFS / MBW=16 path. */
  const uint16_t even = (uint16_t)(len >> 1U);
  for (uint16_t i = 0U; i < even; ++i) {
    const uint16_t lo = (uint16_t)data[(2U * i) + 0U];
    const uint16_t hi = (uint16_t)data[(2U * i) + 1U];
    reg->CFIFO        = (uint16_t)(lo | (uint16_t)(hi << k_ra_usb_byte_bits));
  }
  if ((len & 1U) != 0U) {
    /* Trailing odd byte: switch to MBW=8 and write one byte. A 16-bit
     * write here adds a phantom zero byte to DTLN and the host sees
     * EOVERFLOW on every odd-length descriptor. Mirrors FSP
     * hw_usb_write_fifo8 for USBFS. HUM Ch 36.2.6 CFIFOSEL.MBW. */
    const uint16_t sel_save = reg->CFIFOSEL;
    const uint16_t sel_8 =
      (uint16_t)((sel_save & (uint16_t)~(uint16_t)k_ra_fifosel_mbw_msk) | k_ra_fifosel_mbw_8);
    volatile uint8_t* const cfifo8 = (volatile uint8_t*)(uintptr_t)&reg->CFIFO;
    reg->CFIFOSEL                  = sel_8;
    *cfifo8                        = data[len - 1U];
    reg->CFIFOSEL                  = sel_save;
  }
}

/**
 * @brief HS-only: 32-bit CFIFO read loop for the head bytes.
 * @details Mirrors FSP hw_usb_read_fifo32: cast &CFIFO to uint32_t* and
 *          read `len/4` 32-bit words into the destination buffer
 *          (little-endian byte order).
 * @param[in]  reg  HS register block.
 * @param[out] data Destination byte pointer.
 * @param[in]  len  Total payload length; reads only the head (len & ~0x3) bytes.
 * @pre CFIFOSEL.MBW == 32.
 * @pre data != NULL when len > 0.
 * @post DTLN advanced by exactly (len & ~0x3) bytes.
 * @post data[0..(len&~0x3)-1] holds the received bytes in LE order.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_fifo_read_hs_head(volatile r_usb_regs_t* reg, uint8_t* data, uint16_t len)
{
  volatile uint32_t* const cfifo32 = (volatile uint32_t*)(uintptr_t)&reg->CFIFO;
  const uint16_t           quads   = (uint16_t)(len >> 2U);
  for (uint16_t i = 0U; i < quads; ++i) {
    const uint32_t word = *cfifo32;
    data[(4U * i) + 0U] = (uint8_t)(word & k_ra_usb_byte_mask);
    data[(4U * i) + 1U] = (uint8_t)((word >> k_ra_usb_shift_b1) & k_ra_usb_byte_mask);
    data[(4U * i) + 2U] = (uint8_t)((word >> k_ra_usb_shift_b2) & k_ra_usb_byte_mask);
    data[(4U * i) + 3U] = (uint8_t)((word >> k_ra_usb_shift_b3) & k_ra_usb_byte_mask);
  }
}

/**
 * @brief HS-only: read trailing 1..3 bytes from CFIFOH / CFIFOHH aliases.
 * @details Mirrors FSP hw_usb_read_fifo16 / hw_usb_read_fifo8 (little-endian).
 *          On USBHS a narrow read must go to CFIFOH (+0x02) / CFIFOHH (+0x03);
 *          reading CFIFO itself at MBW=16/8 does not advance the read pointer.
 * @param[in]  reg  HS register block.
 * @param[out] data Destination byte pointer.
 * @param[in]  len  Total payload length; reads only the tail (len & 0x3) bytes.
 * @pre CFIFOSEL.MBW == 32 on entry (restored on exit).
 * @pre data != NULL when len > 0.
 * @post Tail bytes written; CFIFOSEL.MBW restored.
 * @post DTLN advanced by exactly (len & 0x3) bytes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_fifo_read_hs_tail(volatile r_usb_regs_t* reg, uint8_t* data, uint16_t len)
{
  const uint16_t tail = (uint16_t)(len & 0x3U);
  if (tail == 0U) {
    return;
  }
  /* HUM Ch 37.2.7 CFIFO: at MBW=32 the FIFO read pointer only advances
   * on 32-bit reads at CFIFO+0. Narrow CFIFOH/CFIFOHH reads at MBW=16/8
   * do not advance the pointer reliably (a 3-byte bulk-OUT packet drains
   * as bytes [0],[1],[0] when mixed). For the trailing 1..3 bytes of a
   * non-quadword packet we keep MBW=32 and consume one final 32-bit
   * word, extracting only the `tail` low-order bytes (the FIFO presents
   * unused-byte slots as don't-care). One read, one pointer advance,
   * no MBW transition. */
  const uint32_t word     = *(volatile uint32_t*)(uintptr_t)&reg->CFIFO;
  const uint16_t off_base = (uint16_t)(len & (uint16_t)~(uint16_t)0x3U);
  const uint8_t  bytes[4] = {
    (uint8_t)(word & k_ra_usb_byte_mask),
    (uint8_t)((word >> k_ra_usb_shift_b1) & k_ra_usb_byte_mask),
    (uint8_t)((word >> k_ra_usb_shift_b2) & k_ra_usb_byte_mask),
    (uint8_t)((word >> k_ra_usb_shift_b3) & k_ra_usb_byte_mask),
  };
  for (uint16_t i = 0U; i < tail; ++i) {
    data[off_base + i] = bytes[i];
  }
}

/**
 * @brief Drain the CFIFO data port into a buffer.
 * @details Dispatches to the speed-appropriate access width: USBHS requires
 *          32-bit reads (MBW=32) -- a 16-bit read does not advance the FIFO
 *          read pointer (HUM Ch 37.2.7 p 2070, FSP hw_usb_read_fifo32).
 *          USBFS keeps MBW=16. Mirrors FSP `usb_pstd_read_fifo` for IP1/IP0.
 * @param[in]  reg  USB instance register block.
 * @param[out] data Destination byte pointer (may be NULL when len == 0).
 * @param[in]  len  Number of bytes to drain.
 * @pre Caller selected DCP / pipe with internal_select_cfifo and FRDY=1.
 * @pre data != NULL when len > 0.
 * @post DTLN advanced by len bytes.
 * @post data[0..len-1] holds the received payload bytes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_fifo_read(volatile r_usb_regs_t* reg, uint8_t* data, uint16_t len)
{
  /* HUM Ch 37.2.7 "CFIFO : CFIFO Port Register" p 2070 */
  if (internal_is_hs(reg)) {
    internal_fifo_read_hs_head(reg, data, len);
    internal_fifo_read_hs_tail(reg, data, len);
    return;
  }
  /* USBFS / MBW=16 path. */
  const uint16_t even = (uint16_t)(len >> 1U);
  for (uint16_t i = 0U; i < even; ++i) {
    const uint16_t word = reg->CFIFO;
    data[(2U * i) + 0U] = (uint8_t)(word & k_ra_usb_byte_mask);
    data[(2U * i) + 1U] = (uint8_t)((word >> k_ra_usb_byte_bits) & k_ra_usb_byte_mask);
  }
  if ((len & 1U) != 0U) {
    /* Trailing odd byte: switch to MBW=8 and read one byte so DTLN
     * advances by exactly 1. Mirrors FSP hw_usb_read_fifo8 for USBFS. */
    const uint16_t sel_save = reg->CFIFOSEL;
    const uint16_t sel_8 =
      (uint16_t)((sel_save & (uint16_t)~(uint16_t)k_ra_fifosel_mbw_msk) | k_ra_fifosel_mbw_8);
    volatile uint8_t* const cfifo8 = (volatile uint8_t*)(uintptr_t)&reg->CFIFO;
    reg->CFIFOSEL                  = sel_8;
    data[len - 1U]                 = *cfifo8;
    reg->CFIFOSEL                  = sel_save;
  }
}

/**
 * @brief Implementation of ra_usb_queue_in (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] pipe_num See implementation.
 * @param[in] data See implementation.
 * @param[in] len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_queue_in(ra_usb_speed_t speed, uint8_t pipe_num, const uint8_t* data, uint16_t len)
{
  /* THROUGHPUT NOTE.
   * queue_in writes ONE packet (up to MPS bytes) into the IN pipe's
   * CFIFO bank and asserts BVAL. The hardware transmits the bank on
   * the next IN token from the host. With PIPECFG.DBLB the pipe has
   * two banks: queue_in can fill bank B while bank A is being TX'd,
   * doubling sustained throughput on a fast host. Our measured
   * ceiling (2.66 MB/s on HS) is bounded by the Linux cdc_acm read-
   * URB completion path on the host, not by this code. See the
   * matching note in port/usbx/ux_dcd_ra_usb.c (auto-echo block) for
   * the full chain-of-causality + what it would take to lift the
   * ceiling. */
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((pipe_num == 0U) || (pipe_num > k_ra_usb_max_pipe_num)) {
    return k_ra_err_invalid_arg;
  }
  if ((len > k_ra_usb_pipe_max_packet) || ((data == nullptr) && (len != 0U))) {
    return k_ra_err_invalid_arg;
  }

  internal_select_cfifo(reg, pipe_num, true);
  const ra_err_t ready = internal_wait_frdy(reg);
  RA_RETURN_ON_ERROR(ready, s_tag, "queue_in: FRDY timeout"); /* GCOVR_EXCL_BR_LINE */

  if (len > 0U) {
    internal_fifo_write(reg, data, len);
  }

  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 */
  reg->CFIFOCTR = k_ra_fifoctr_bval;
  internal_pipe_pid(reg, pipe_num, k_ra_pid_buf);
  return k_ra_ok;
}

/**
 * @brief Push a single DCP IN chunk: wait for FRDY, write FIFO, pulse BVAL.
 *
 * @details Bounded helper extracted from ``ra_usb_dcp_in_data`` so the
 * top-level function stays under the clang-tidy
 * ``readability-function-size`` threshold. Performs exactly one
 * controller-buffer transfer cycle.
 *
 * @param[in,out] reg DCP register block (chip or host shim).
 * @param[in]     p   Source byte pointer; must hold at least ``n`` bytes.
 * @param[in]     n   Chunk size in bytes; must be > 0 and <= the DCP MPS.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok            Chunk queued; BVAL pulsed.
 * @retval k_ra_err_timeout   FRDY never asserted within the bound.
 *
 * @pre ``reg`` was returned by ``internal_pick()`` and is non-NULL.
 * @pre CFIFO is already selected on DCP (CURPIPE=0) in IN direction.
 * @post On success, the controller buffer holds the new chunk and BVAL
 *       has been pulsed.
 * @post On error, no PID transition has been performed.
 *
 * @note Not thread-safe; the parent function holds the DCP lock.
 * @since 0.1.0
 */
static ra_err_t internal_dcp_push_chunk(volatile r_usb_regs_t* reg, const uint8_t* p, uint16_t n)
{
  const ra_err_t ready = internal_wait_frdy(reg);
  RA_RETURN_ON_ERROR(ready, s_tag, "dcp_in_data: FRDY timeout (chunk)"); /* GCOVR_EXCL_BR_LINE */
  internal_fifo_write(reg, p, n);
  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979.
   *
   * 2026-05-20 attempt: FSP r_usb_plibusbip.c only sets BVAL on the
   * short last chunk (n < MPS), letting hardware auto-flag full-MPS
   * chunks. Tried that here; usb_cdc_echo dropped off the bus and
   * the chip didn't enumerate. Our synchronous multi-chunk push (vs
   * FSP's BRDY-IRQ-per-chunk) apparently requires the explicit BVAL
   * pulse on every chunk to commit each one before the next FRDY
   * poll. Keep the unconditional write. */
  reg->CFIFOCTR = k_ra_fifoctr_bval;
  return k_ra_ok;
}

/**
 * @brief Send the EP0 IN data-stage payload as one or more DCP chunks.
 *
 * @details Drives the multi-chunk loop so ``ra_usb_dcp_in_data`` stays
 * under the clang-tidy statement-count threshold. Pushes at most
 * DCPMAXP bytes per iteration via ``internal_dcp_push_chunk``, then
 * raises DCPCTR.PID to BUF after the first successful push. The
 * controller services subsequent IN tokens automatically.
 *
 * @param[in,out] reg  Selected DCP register block (CFIFO already
 *                     pointed at DCP / IN direction by the caller).
 * @param[in]     data Source payload; must hold at least ``len`` bytes.
 * @param[in]     len  Total payload length in bytes; must be > 0.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok            Payload fully queued; PID set to BUF.
 * @retval k_ra_err_timeout   FRDY never asserted for some chunk.
 *
 * @pre ``reg`` is non-NULL and CFIFOSEL is already programmed for DCP IN.
 * @pre ``data`` is non-NULL and ``len > 0``.
 * @post On success, all ``len`` bytes have been queued and DCPCTR.PID == BUF.
 * @post On error, DCPCTR.PID is left unchanged from its prior value.
 *
 * @note Not thread-safe; caller holds the DCP lock.
 * @since 0.1.0
 */
static ra_err_t
internal_dcp_in_payload(volatile r_usb_regs_t* reg, const uint8_t* data, uint16_t len)
{
  uint16_t remaining  = len;
  uint16_t offset     = 0U;
  bool     pid_raised = false;
  while (remaining > 0U) {
    const uint16_t chunk =
      (remaining > k_ra_usb_dcp_max_packet) ? (uint16_t)k_ra_usb_dcp_max_packet : remaining;
    const ra_err_t pushed = internal_dcp_push_chunk(reg, &data[offset], chunk);
    RA_RETURN_ON_ERROR(pushed, s_tag, "dcp_in_data: chunk push failed"); /* GCOVR_EXCL_BR_LINE */
    if (!pid_raised) {
      /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1991 */
      internal_dcp_pid(reg, k_ra_pid_buf);
      pid_raised = true;
    }
    offset    = (uint16_t)(offset + chunk);
    remaining = (uint16_t)(remaining - chunk);
  }
  return k_ra_ok;
}

/**
 * @brief Send a zero-length data stage on the DCP IN endpoint.
 *
 * @details Waits for FRDY, pulses CFIFOCTR.BVAL on an empty buffer
 * (producing a ZLP on the wire) and raises DCPCTR.PID to BUF.
 * Extracted from ``ra_usb_dcp_in_data`` so the top-level function fits
 * under the clang-tidy statement-count threshold.
 *
 * @param[in,out] reg Selected DCP register block (CFIFO already
 *                    pointed at DCP / IN direction by the caller).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           ZLP queued; PID set to BUF.
 * @retval k_ra_err_timeout  FRDY never asserted.
 *
 * @pre ``reg`` is non-NULL and CFIFOSEL is already programmed for DCP IN.
 * @pre USB module clock and power are on.
 * @post On success, an empty buffer is queued and DCPCTR.PID == BUF.
 * @post On error, DCPCTR.PID is left unchanged from its prior value.
 *
 * @note Not thread-safe; caller holds the DCP lock.
 * @since 0.1.0
 */
static ra_err_t internal_dcp_in_zlp(volatile r_usb_regs_t* reg)
{
  const ra_err_t ready = internal_wait_frdy(reg);
  RA_RETURN_ON_ERROR(ready, s_tag, "dcp_in_data: FRDY timeout (zlp)"); /* GCOVR_EXCL_BR_LINE */
  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 */
  reg->CFIFOCTR = k_ra_fifoctr_bval;
  /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1991 */
  internal_dcp_pid(reg, k_ra_pid_buf);
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_dcp_in_data (see header for full contract).
 *
 * @details Selects DCP on CFIFO once, then dispatches to either
 * ``internal_dcp_in_zlp`` (zero-length) or ``internal_dcp_in_payload``
 * (one or more chunks). Status stage (CCPL) is intentionally NOT
 * pulsed here -- the bridge handles it on the CTSQ status-stage edge.
 *
 * @param[in] speed USB speed selector (FS / HS) -> picks the controller block.
 * @param[in] data  Source payload; may be NULL only when ``len == 0``.
 * @param[in] len   Payload length in bytes (0 sends a ZLP).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Payload queued; PID set to BUF.
 * @retval k_ra_err_invalid_arg  ``speed`` not recognised, or NULL data
 *                               with non-zero ``len``.
 * @retval k_ra_err_timeout      FRDY never asserted for ZLP or any chunk.
 *
 * @pre USB module clock and power are on.
 * @pre Caller serialises DCP IN/OUT issue.
 * @post On success, CFIFO is empty and DCPCTR.PID == BUF.
 * @post On error, DCPCTR.PID is left unchanged from its prior value.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
/* Diagnostic probes for the DCP IN data push (read via JLink). */
volatile uint32_t s_dcp_push_count       = 0U;
volatile uint16_t s_dcp_dcpctr_pre_push  = 0U;
volatile uint16_t s_dcp_dcpctr_post_push = 0U;
volatile uint16_t s_dcp_cfifoctr_pre     = 0U;
volatile uint16_t s_dcp_cfifoctr_post    = 0U;
volatile uint16_t s_dcp_last_len         = 0U;
volatile uint8_t  s_dcp_last_err         = 0U; /* 0=ok, 1=frdy timeout, 2=null arg */

/**
 * @brief Implementation of ra_usb_dcp_in_data (see header for full contract).
 * @details Push a control-IN data-stage payload (or zero-length packet) into
 *          the DCP FIFO, set BVAL=1 and DCPCTR.PID=BUF so the chip
 *          transmits on the next IN token from the host. Captures
 *          pre/post register snapshots into ::s_dcp_push_count and
 *          friends for JLink-readable diagnostic.
 * @param[in] speed Which controller (FS or HS).
 * @param[in] data  Payload bytes (may be NULL when len==0).
 * @param[in] len   Byte count; may exceed DCPMAXP and will be chunked.
 * @return ra_err_t result code.
 * @retval k_ra_ok               Payload queued; PID=BUF.
 * @retval k_ra_err_invalid_arg  speed out of range OR data NULL with
 *                                len > 0.
 * @retval k_ra_err_hw_timeout   FRDY never asserted within the bound.
 * @pre Caller has cleared INTSTS0.VALID (PID writes are gated by VALID
 *      per HUM Ch 37.2.31 p 2095).
 * @pre USB module clock and power are on.
 * @post On success, len bytes have been queued and DCPCTR.PID == BUF.
 * @post On error, DCPCTR.PID is left unchanged from its prior value.
 * @note Not thread-safe; caller holds the DCP lock.
 * @since 0.1.0
 */
ra_err_t ra_usb_dcp_in_data(ra_usb_speed_t speed, const uint8_t* data, uint16_t len)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    s_dcp_last_err = 2U;
    return k_ra_err_invalid_arg;
  }
  if ((data == nullptr) && (len != 0U)) {
    s_dcp_last_err = 2U;
    return k_ra_err_invalid_arg;
  }
  s_dcp_push_count++;
  s_dcp_last_len        = len;
  s_dcp_dcpctr_pre_push = reg->DCPCTR;

  /* Select DCP (CURPIPE = 0) on CFIFO in IN direction. Done once; the
   * selection persists across chunks.
   * HUM Ch 36.2.7 "CFIFOSEL : CFIFO Port Select Register", p 1976 */
  internal_select_cfifo(reg, 0U, true);
  s_dcp_cfifoctr_pre = reg->CFIFOCTR;

  ra_err_t err;
  if (len == 0U) {
    err = internal_dcp_in_zlp(reg);
  } else {
    err = internal_dcp_in_payload(reg, data, len);
  }
  s_dcp_cfifoctr_post    = reg->CFIFOCTR;
  s_dcp_dcpctr_post_push = reg->DCPCTR;
  s_dcp_last_err         = (err == k_ra_ok) ? 0U : 1U;
  return err;
}

/**
 * @brief Argument validation helper for `ra_usb_queue_out`.
 *
 * @details Read-only over the buffers; the caller mutates them on
 * success. Marked `const` so clang-tidy's `readability-non-const-parameter`
 * is satisfied.
 *
 * @param[in] pipe_num See implementation.
 * @param[in] out_buf See implementation.
 * @param[in] inout_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t
internal_check_queue_out_args(uint8_t pipe_num, const uint8_t* out_buf, const uint16_t* inout_len)
{
  if ((out_buf == nullptr) || (inout_len == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if ((pipe_num == 0U) || (pipe_num > k_ra_usb_max_pipe_num)) {
    return k_ra_err_invalid_arg;
  }
  if ((*inout_len == 0U) || (*inout_len > k_ra_usb_pipe_max_packet)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_queue_out (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] pipe_num See implementation.
 * @param[in] out_buf See implementation.
 * @param[in] inout_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t
ra_usb_queue_out(ra_usb_speed_t speed, uint8_t pipe_num, uint8_t* out_buf, uint16_t* inout_len)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t arg_err = internal_check_queue_out_args(pipe_num, out_buf, inout_len);
  if (arg_err != k_ra_ok) {
    return arg_err;
  }

  /* Fast path: BRDYSTS bit `pipe_num` is set by hardware when an OUT
   * packet has landed in this pipe's FIFO buffer. If it's clear, no
   * data is waiting -- return k_ra_err_no_data WITHOUT entering the
   * 10ms FRDY spin.
   * HUM Ch 36.2.12 "BRDYSTS : BRDY Interrupt Status Register", p 1983. */
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  if ((reg->BRDYSTS & pipe_bit) == 0U) {
    *inout_len = 0U;
    return k_ra_err_no_data;
  }

  /* W0C BRDYSTS for this pipe BEFORE draining (FSP pattern, mirrors
   * `r_usb_pinthandler_usbip0.c` order). Critical for PIPECFG.DBLB:
   * if bank B raises a fresh BRDY edge while we are still draining
   * bank A, the BRDYSTS bit will be re-set by hardware and survive a
   * future read; clearing AFTER the drain would wipe that edge and
   * lose every other packet. */
  reg->BRDYSTS = (uint16_t)(~pipe_bit);

  internal_select_cfifo(reg, pipe_num, false);
  const ra_err_t ready = internal_wait_frdy(reg);
  RA_RETURN_ON_ERROR(ready, s_tag, "queue_out: FRDY timeout"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 */
  const uint16_t available = (uint16_t)(reg->CFIFOCTR & k_ra_fifoctr_dtln);
  if (available == 0U) {
    /* Zero-length packet (ZLP). FSP releases the bank explicitly via
     * BCLR in this case (`r_usb_plibusbip.c` usb_pstd_read_data line
     * 700: "0 length packet -> Clear BVAL"). For a non-zero drain the
     * FIFO empties naturally as we read DTLN bytes; BCLR is NOT used
     * and would confuse the bank pointer in DBLB mode. */
    reg->CFIFOCTR = k_ra_fifoctr_bclr;
    *inout_len    = 0U;
    return k_ra_err_no_data;
  }
  const uint16_t take = (available < *inout_len) ? available : *inout_len;
  internal_fifo_read(reg, out_buf, take);
  *inout_len = take;
  /* No BCLR for non-zero drain (FSP semantics). The FIFO read above
   * drained `take` bytes; the remainder (if take < available) is lost
   * by design -- our caller passed a buffer large enough for one MPS
   * packet, which is what one bank holds. */
  internal_pipe_pid(reg, pipe_num, k_ra_pid_buf);
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_rearm_out_pipe (see header for full contract).
 *
 * @details See the public header for the documented contract; this
 * definition implements it. The hardware-required sequence per HUM
 * Ch 36.2.13 (NRDYSTS, W0C) and Ch 36.2.27 (PIPECTR.PID) is:
 *   1. Ack NRDYSTS bit `pipe_num` by writing 0 to that bit (W0C: write
 *      `~pipe_bit` to clear only the target bit and preserve the rest).
 *   2. Force PID=BUF on the pipe so the controller ACKs the next OUT
 *      token from the host instead of NAK'ing it.
 *
 * @param[in] speed    See header.
 * @param[in] pipe_num See header.
 *
 * @return Result code.
 * @retval k_ra_ok              Pipe re-armed.
 * @retval k_ra_err_invalid_arg Argument out of range.
 *
 * @pre Speed maps to a real controller.
 * @pre Pipe 1..9.
 * @post NRDYSTS bit `pipe_num` cleared.
 * @post PIPECTR PID == BUF for `pipe_num`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_usb_rearm_out_pipe(ra_usb_speed_t speed, uint8_t pipe_num)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((pipe_num == 0U) || (pipe_num > k_ra_usb_max_pipe_num)) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 36.2.13 "NRDYSTS : NRDY Interrupt Status Register", p 1984.
   * W0C semantics: write 0 to clear the target bit, 1 to preserve the
   * rest. */
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  reg->NRDYSTS            = (uint16_t)(~pipe_bit);
  /* HUM Ch 36.2.27 "PIPEnCTR : PIPE n Control Register", p 2005. Force
   * PID=BUF so the next host OUT token is ACKed. */
  internal_pipe_pid(reg, pipe_num, k_ra_pid_buf);
  return k_ra_ok;
}

/* =============================================================================
 * Control transfers
 * =============================================================================
 */

/**
 * @brief Implementation of ra_usb_read_setup_if_valid (see header for full contract).
 * @details VALID-gated SETUP drain. Returns ::k_ra_err_no_data when
 *          INTSTS0.VALID is clear; otherwise drains USBREQ/USBVAL/
 *          USBINDX/USBLENG and W0C-clears VALID.
 * @param[in] speed See header.
 * @param[in] out_setup See header.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @retval k_ra_err_no_data INTSTS0.VALID was clear at entry.
 * @retval k_ra_err_invalid_arg speed out of range.
 * @retval k_ra_err_null_ptr out_setup was NULL.
 * @pre Module state is consistent.
 * @pre out_setup is non-NULL.
 * @post On success, INTSTS0.VALID is W0C-cleared.
 * @post On success, *out_setup mirrors the chip's SETUP latch.
 * @note Not thread-safe; FS / CTRT path uses this variant.
 * @since 0.1.0
 */
ra_err_t ra_usb_read_setup_if_valid(ra_usb_speed_t speed, ra_usb_setup_t* out_setup)
{
  RA_CHECK_NULL_PTR(out_setup, s_tag, "read_setup_if_valid: out_setup");
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1985 */
  if ((reg->INTSTS0 & k_ra_intsts0_mask_valid) == 0U) {
    return k_ra_err_no_data;
  }

  /* HUM Ch 36.2.17 "USBREQ : USB Request Type Register", p 1989 */
  const uint16_t req         = reg->USBREQ;
  out_setup->bm_request_type = (uint8_t)(req & k_ra_usb_byte_mask);
  out_setup->b_request       = (uint8_t)((req >> k_ra_usb_byte_bits) & k_ra_usb_byte_mask);
  out_setup->w_value         = reg->USBVAL;
  out_setup->w_index         = reg->USBINDX;
  out_setup->w_length        = reg->USBLENG;

  /* Clear VALID by writing zero to the bit (W0C semantics). */
  reg->INTSTS0 = (uint16_t)(reg->INTSTS0 & (uint16_t)~k_ra_intsts0_mask_valid);
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_read_setup_unconditional (see header).
 * @details Race-free SETUP drain for the HS / SQMON polled-worker path.
 *          On HS the polled dispatcher routinely observes
 *          DCPCTR.SQMON == 1 (race-immune SETUP-latched signal, HUM
 *          Ch 37.2.31 p 2095) AFTER the SIE has already auto-cleared
 *          INTSTS0.VALID. The captured registers USBREQ/USBVAL/USBINDX/
 *          USBLENG remain latched (HUM Ch 37.2.21..24 p 2087..2090) --
 *          only the VALID flag is cleared. This entry point therefore
 *          skips the VALID gate and drains the captured registers
 *          directly, then defensively W0C-acks VALID in case the SIE
 *          re-asserted it before our store.
 * @param[in] speed Which controller.
 * @param[out] out_setup Decoded 8-byte SETUP packet.
 * @return Result code.
 * @retval k_ra_ok SETUP drained from the captured registers.
 * @retval k_ra_err_invalid_arg speed out of range.
 * @retval k_ra_err_null_ptr out_setup was NULL.
 * @pre Caller has independent proof a SETUP arrived (e.g. SQMON==1).
 * @pre out_setup is non-NULL.
 * @post *out_setup mirrors USBREQ/USBVAL/USBINDX/USBLENG.
 * @post INTSTS0.VALID is W0C-cleared (no-op if already 0).
 * @note Not thread-safe; HS / SQMON path uses this variant.
 * @since 0.1.0
 */
ra_err_t ra_usb_read_setup_unconditional(ra_usb_speed_t speed, ra_usb_setup_t* out_setup)
{
  RA_CHECK_NULL_PTR(out_setup, s_tag, "read_setup_unconditional: out_setup");
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 36.2.17 / 37.2.21 "USBREQ : USB Request Type Register",
   * p 1989 / 2087. The SETUP latch survives VALID being auto-cleared
   * by the SIE; only a fresh SETUP token can overwrite it. */
  const uint16_t req         = reg->USBREQ;
  out_setup->bm_request_type = (uint8_t)(req & k_ra_usb_byte_mask);
  out_setup->b_request       = (uint8_t)((req >> k_ra_usb_byte_bits) & k_ra_usb_byte_mask);
  out_setup->w_value         = reg->USBVAL;
  out_setup->w_index         = reg->USBINDX;
  out_setup->w_length        = reg->USBLENG;

  /* HUM Ch 36.2.14 INTSTS0 p 1985: defensively W0C-clear VALID in case
   * the SIE has re-asserted it (no-op when already 0). */
  reg->INTSTS0 = (uint16_t)(reg->INTSTS0 & (uint16_t)~k_ra_intsts0_mask_valid);
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_control_response (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] accept See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_control_response(ra_usb_speed_t speed, bool accept)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (!accept) {
    /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1991 */
    internal_dcp_pid(reg, k_ra_pid_stall);
    return k_ra_ok;
  }
  internal_dcp_pid(reg, k_ra_pid_buf);
  /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1991 */
  internal_rmw16(&reg->DCPCTR, (uint16_t)(1U << k_ra_dcpctr_bit_ccpl), 0U);
  return k_ra_ok;
}

/* =============================================================================
 * IRQ delivery + power
 * =============================================================================
 */

/* Per-controller callbacks. Two slots so USBFS and USBHS can have
 * independent event handlers -- this is what lets one firmware image
 * run the DCD bridge on one controller and a different driver (e.g.
 * bare-CDC) on the other. */
typedef enum : uint8_t {
  k_ra_usb_cb_slot_fs = 0U,
  k_ra_usb_cb_slot_hs = 1U,
  k_ra_usb_cb_slot_n  = 2U,
} ra_usb_cb_slot_t;

static ra_usb_event_fn_t s_usb_fn[k_ra_usb_cb_slot_n];
static void*             s_usb_ctx[k_ra_usb_cb_slot_n];

/**
 * @brief Map a USB speed enum to the per-controller callback slot index.
 *
 * @details Two slots exist: ``k_ra_usb_cb_slot_fs`` for the USBFS
 * controller and ``k_ra_usb_cb_slot_hs`` for the USBHS controller.
 * Every code path that indexes ``s_usb_fn[]`` or ``s_usb_ctx[]``
 * goes through this helper so we never confuse the two.
 *
 * @param[in] speed Controller selector (FS or HS).
 *
 * @return Slot index in ``s_usb_fn[]`` / ``s_usb_ctx[]``.
 * @retval k_ra_usb_cb_slot_hs ``speed == k_ra_usb_speed_hs``.
 * @retval k_ra_usb_cb_slot_fs Any other speed value (FS default).
 *
 * @pre ``speed`` is a valid ``ra_usb_speed_t`` enum.
 * @pre s_usb_fn / s_usb_ctx storage is in scope.
 * @post No global state is touched; the helper is pure.
 * @post Result is always < ``k_ra_usb_cb_slot_n``.
 *
 * @note Pure / thread-safe.
 * @since 0.1.0
 */
static uint8_t internal_cb_slot(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? (uint8_t)k_ra_usb_cb_slot_hs : (uint8_t)k_ra_usb_cb_slot_fs;
}

/**
 * @brief Implementation of ra_usb_attach_handler (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] fn See implementation.
 * @param[in] ctx See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_attach_handler(ra_usb_speed_t speed, ra_usb_event_fn_t fn, void* ctx)
{
  const uint8_t slot = internal_cb_slot(speed);
  s_usb_fn[slot]     = fn;
  s_usb_ctx[slot]    = ctx;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_dispatch (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void ra_usb_dispatch(ra_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) { /* GCOVR_EXCL_BR_LINE -- speeds always valid */
    return;             /* GCOVR_EXCL_LINE */
  }
  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1985.
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
   * (ra_usb_read_setup_if_valid / _unconditional) drain
   * USBREQ/USBVAL/USBINDX/USBLENG and clear VALID themselves.
   * CTSQ/DVSQ are read-only fields. */
  const uint16_t mask = reg->INTSTS0;

  const uint16_t ack_bits = (uint16_t)((1U << k_ra_int0_bit_ctrt) | (1U << k_ra_int0_bit_dvst) |
                                       (1U << k_ra_int0_bit_bemp) | (1U << k_ra_int0_bit_brdy) |
                                       (1U << k_ra_int0_bit_nrdy) | (1U << k_ra_int0_bit_vbse) |
                                       (1U << k_ra_int0_bit_rsme) | (1U << k_ra_int0_bit_sofr));
  reg->INTSTS0            = (uint16_t)~(uint16_t)(mask & ack_bits);

  const uint8_t           slot = internal_cb_slot(speed);
  const ra_usb_event_fn_t fn   = s_usb_fn[slot];
  void* const             ctx  = s_usb_ctx[slot];
  if (fn != nullptr) {
    fn(ctx, speed, mask);
  }
}

/**
 * @brief Implementation of ra_usb_intsts0_snapshot (see header for full contract).
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
uint16_t ra_usb_intsts0_snapshot(ra_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return 0U;
  }
  return reg->INTSTS0;
}

/**
 * @brief Implementation of ra_usb_enter_stop (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_enter_stop(ra_usb_speed_t speed)
{
  if ((speed != k_ra_usb_speed_fs) && (speed != k_ra_usb_speed_hs)) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_disable(internal_mstp(speed));
}

/**
 * @brief Implementation of ra_usb_exit_stop (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_exit_stop(ra_usb_speed_t speed)
{
  if ((speed != k_ra_usb_speed_fs) && (speed != k_ra_usb_speed_hs)) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(internal_mstp(speed));
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
 * @enum ra_usb_dvstctr_bit_t
 * @brief DVSTCTR0 host-mode bit positions (HUM Ch 36.2.5 "DVSTCTR0").
 *
 * @details Sourced from CMSIS `R_USB_FS0_DVSTCTR0_*_Pos` in
 * `R7KA8D2KF_core0.h` (lines 71382-71389) and confirmed for the HS
 * instance (`R_USB_HS0_DVSTCTR0_*_Pos`, lines 75228-75235).
 */
typedef enum : uint8_t {
  k_ra_dvstctr_bit_uact   = 4U, /**< Bus enable (host SOF generation). */
  k_ra_dvstctr_bit_resume = 5U, /**< Resume signal output (host).      */
  k_ra_dvstctr_bit_usbrst = 6U, /**< Bus reset signal (host).          */
  k_ra_dvstctr_bit_rwupe  = 7U, /**< Remote-wake detect enable (host). */
} ra_usb_dvstctr_bit_t;

/**
 * @brief Build the host-mode SYSCFG word.
 *
 * @details Sets SCKE | DCFM | DRPD | USBE; adds HSE for the HS
 * instance. DPRPU is intentionally not set (device-mode pull-up).
 *
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint16_t internal_host_syscfg_word(ra_usb_speed_t speed)
{
  uint16_t syscfg = (uint16_t)(1U << k_ra_syscfg_bit_scke);
  syscfg          = (uint16_t)(syscfg | (uint16_t)(1U << k_ra_syscfg_bit_dcfm));
  syscfg          = (uint16_t)(syscfg | (uint16_t)(1U << k_ra_syscfg_bit_drpd));
  syscfg          = (uint16_t)(syscfg | (uint16_t)(1U << k_ra_syscfg_bit_usbe));
  if (speed == k_ra_usb_speed_hs) {
    syscfg = (uint16_t)(syscfg | (uint16_t)(1U << k_ra_syscfg_bit_hse));
  }
  return syscfg;
}

/**
 * @brief Implementation of ra_usb_host_init (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_host_init(ra_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  const ra_err_t mst_err = ra_mstp_enable(internal_mstp(speed));
  RA_RETURN_ON_ERROR(mst_err, s_tag, "host_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 36.2.1 "SYSCFG : System Configuration Control Register", p 1966 */
  /* HUM Ch 37.2.1 "SYSCFG : System Configuration Control Register", p 2060 */
  reg->SYSCFG = internal_host_syscfg_word(speed);

  /* HUM Ch 36.2.5 "DVSTCTR0 : Device State Control Register 0", p 1971 */
  reg->DVSTCTR0 = 0U;

  /* HUM Ch 36.2.7 "CFIFOSEL : CFIFO Port Select Register", p 1976 */
  reg->CFIFOSEL  = k_ra_fifosel_mbw_16;
  reg->D0FIFOSEL = k_ra_fifosel_mbw_16;
  reg->D1FIFOSEL = k_ra_fifosel_mbw_16;

  /* HUM Ch 36.2.20 "DCPMAXP : DCP Max Packet Size Register", p 1990 */
  reg->DCPCFG  = 0U;
  reg->DCPMAXP = k_ra_usb_dcp_max_packet;
  reg->DCPCTR  = 0U;

  /* HUM Ch 36.2.16 "USBADDR : USB Address Register", p 1988 -- target
   * device address for the host's outgoing tokens. Default to 0
   * (newly-attached devices respond at address 0). */
  reg->USBADDR = 0U;

  /* HUM Ch 36.2.10 "INTENB0 : Interrupt Enable Register 0", p 1980 */
  /* Host needs the same interrupt set as device for transfer
   * completion + bus events. */
  reg->INTENB0 = (uint16_t)((1U << k_ra_int0_bit_bemp) | (1U << k_ra_int0_bit_brdy) |
                            (1U << k_ra_int0_bit_nrdy) | (1U << k_ra_int0_bit_ctrt) |
                            (1U << k_ra_int0_bit_dvst) | (1U << k_ra_int0_bit_vbse));
  reg->INTENB1 = 0U;
  reg->BRDYENB = 0U;
  reg->NRDYENB = 0U;
  reg->BEMPENB = 0U;

  ra_log_info_val(s_tag, "usb host init speed", (uint32_t)speed);
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_host_deinit (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_host_deinit(ra_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 36.2.5 "DVSTCTR0 : Device State Control Register 0", p 1971 */
  reg->DVSTCTR0 = 0U;
  reg->INTENB0  = 0U;
  reg->INTENB1  = 0U;
  reg->BRDYENB  = 0U;
  reg->NRDYENB  = 0U;
  reg->BEMPENB  = 0U;
  reg->SYSCFG   = 0U;
  return ra_mstp_disable(internal_mstp(speed));
}

/**
 * @brief Implementation of ra_usb_host_bus_reset (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] assert_reset See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_host_bus_reset(ra_usb_speed_t speed, bool assert_reset)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 36.2.5 "DVSTCTR0 : Device State Control Register 0", p 1971 */
  const uint16_t rst_bit  = (uint16_t)(1U << k_ra_dvstctr_bit_usbrst);
  const uint16_t uact_bit = (uint16_t)(1U << k_ra_dvstctr_bit_uact);
  if (assert_reset) {
    /* USBRST=1 forces UACT low; FSP atomically sets RST + clears UACT. */
    internal_rmw16(&reg->DVSTCTR0, rst_bit, uact_bit);
  } else {
    internal_rmw16(&reg->DVSTCTR0, 0U, rst_bit);
  }
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_host_set_uact (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] enable See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_host_set_uact(ra_usb_speed_t speed, bool enable)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 36.2.5 "DVSTCTR0 : Device State Control Register 0", p 1971 */
  const uint16_t uact_bit = (uint16_t)(1U << k_ra_dvstctr_bit_uact);
  if (enable) {
    internal_rmw16(&reg->DVSTCTR0, uact_bit, 0U);
  } else {
    internal_rmw16(&reg->DVSTCTR0, 0U, uact_bit);
  }
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_usb_host_setup_request (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] setup See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_host_setup_request(ra_usb_speed_t speed, const ra_usb_setup_t* setup)
{
  RA_CHECK_NULL_PTR(setup, s_tag, "host_setup_request: setup");
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1991 -- guard
   * against a still-pending request. */
  const uint16_t sureq_bit = (uint16_t)(1U << k_ra_dcpctr_bit_sureq);
  if ((reg->DCPCTR & sureq_bit) != 0U) {
    return k_ra_err_busy;
  }

  /* HUM Ch 36.2.17 "USBREQ : USB Request Type Register", p 1989 */
  const uint16_t req = (uint16_t)((uint16_t)setup->bm_request_type |
                                  (uint16_t)((uint16_t)setup->b_request << k_ra_usb_byte_bits));
  reg->USBREQ        = req;
  reg->USBVAL        = setup->w_value;
  reg->USBINDX       = setup->w_index;
  reg->USBLENG       = setup->w_length;

  /* SUREQ tells the SIE to issue the SETUP token on the next frame. */
  internal_rmw16(&reg->DCPCTR, sureq_bit, 0U);
  return k_ra_ok;
}
