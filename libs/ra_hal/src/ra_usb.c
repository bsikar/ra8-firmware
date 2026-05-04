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
 *  - `usb_pstd_save_request`        -> `ra_usb_read_setup`
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
static void internal_select_cfifo(volatile r_usb_regs_t* reg, uint16_t pipe_num, bool is_in_dir)
{
  /* HUM Ch 36.2.7 "CFIFOSEL : CFIFO Port Select Register", p 1976 */
  uint16_t sel = (uint16_t)(pipe_num & k_ra_fifosel_curpipe);
  sel          = (uint16_t)(sel | k_ra_fifosel_mbw_16);
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
  for (uint32_t i = 0U; i < (uint32_t)k_ra_usb_frdy_poll_limit; ++i) {
    if ((reg->CFIFOCTR & k_ra_fifoctr_frdy) != 0U) {
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
  k_ra_usbhs_delay_1us_iters     = 2000U,
  k_ra_usbhs_pll_lock_poll_limit = 200000U,
  k_ra_usbhs_scke_poll_limit     = 200000U,
  k_ra_usbhs_probe_hi_shift      = 16U,
  k_ra_usbhs_probe_lo_mask       = 0x0000FFFFU,
} ra_usbhs_init_local_t;

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
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra_usbhs_delay_1us_iters; ++i) {
    __asm__ volatile("nop");
  }
}

/**
 * @brief Drive USBHS SYSCFG.USBE|HSE after the PHY-side init steps.
 *
 * @details Splits the SYSCFG mutation off from
 * ::internal_usbhs_phy_bringup so each helper stays under the NASA
 * Rule 4 / clang-tidy size budget. Also captures the SYSCFG-after-USBE
 * value into the diagnostic probe word's high half.
 *
 * @param[in] reg HS register block pointer.
 *
 * @pre PHYSET DIRPD/PLLRESET are released.
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
  reg->SYSCFG = (uint16_t)(reg->SYSCFG | (uint16_t)(1U << k_ra_syscfg_bit_usbe) |
                           (uint16_t)(1U << k_ra_syscfg_bit_hse));

  const uint32_t lo  = s_usbhs_init_probe & (uint32_t)k_ra_usbhs_probe_lo_mask;
  const uint32_t hi  = (uint32_t)reg->SYSCFG << (uint32_t)k_ra_usbhs_probe_hi_shift;
  s_usbhs_init_probe = lo | hi;
}

/**
 * @brief Wait for USBHS PHY PLL lock with bounded polling.
 *
 * @details Polls PLLSTA.PLLLOCK up to
 * ::k_ra_usbhs_pll_lock_poll_limit iterations. Captures the final
 * PLLSTA value into ``s_usbhs_pllsta_probe`` regardless of outcome.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok     PLLLOCK observed asserted.
 * @retval k_ra_err_hw_timeout PLLLOCK never asserted.
 *
 * @pre LPSTS.SUSPENDM has been set.
 * @pre USB60CLK is running at 60 MHz.
 * @post Loop iteration count is bounded.
 * @post s_usbhs_pllsta_probe holds the final PLLSTA word.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static ra_err_t internal_usbhs_wait_pll_lock(void)
{
  volatile uint16_t* const pllsta = ra_usbhs_pllsta();
  ra_err_t                 lock   = k_ra_err_hw_timeout;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_usbhs_pll_lock_poll_limit; ++i) {
    if ((*pllsta & (uint16_t)k_ra_pllsta_plllock) != 0U) {
      lock = k_ra_ok;
      break;
    }
  }
  s_usbhs_pllsta_probe = *pllsta;
  return lock;
}

/**
 * @brief USBHS embedded-PHY bring-up (FSP `hw_usb_pmodule_init` IP1 branch).
 *
 * @details Mirrors the Renesas FSP USBHS device-mode bring-up sequence
 * for the standard 12 MHz PHY reference (USB60CLK / 5):
 *  1. PHYSET |= DIRPD | CLKSEL_mask  (assert PD, set CLKSEL field)
 *  2. PHYSET &= ~CLKSEL; |= CLKSEL_12 (force 12 MHz reference)
 *  3. delay 1 us
 *  4. PHYSET &= ~DIRPD                (release PHY power-down)
 *  5. delay 1 ms
 *  6. PHYSET &= ~PLLRESET             (release PHY PLL reset)
 *  7. SYSCFG &= ~DRPD                 (clear host pull-down)
 *  8. SYSCFG |= USBE                  (enable USB module)
 *  9. LPSTS  |= SUSPENDM              (release UTMI SuspendM)
 * 10. wait PLLSTA.PLLLOCK = 1
 * 11. BUSWAIT = 0x0F04                (4-wait + reserved b11-b8)
 * 12. PHYSET |= REPSEL_16             (terminator adjust cycle)
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

  /* HUM Ch 37.2.5 "PHYSET : PHY Setting Register", p 2065 */
  *physet = (uint16_t)(*physet | (uint16_t)k_ra_physet_dirpd | (uint16_t)k_ra_physet_clksel);
  *physet = (uint16_t)(*physet & (uint16_t)~(uint16_t)k_ra_physet_clksel);
  *physet = (uint16_t)(*physet | (uint16_t)k_ra_physet_clksel_12);
  internal_usb_delay_1us();

  *physet = (uint16_t)(*physet & (uint16_t)~(uint16_t)k_ra_physet_dirpd);
  ra_delay_ms(1U);
  *physet = (uint16_t)(*physet & (uint16_t)~(uint16_t)k_ra_physet_pllreset);

  s_usbhs_init_probe = (uint32_t)*physet;

  internal_usbhs_enable_syscfg(reg);

  /* HUM Ch 37.2.10 "LPSTS : Low Power Status Register", p 2073 */
  *lpsts = (uint16_t)(*lpsts | (uint16_t)k_ra_lpsts_suspendm);

  /* HUM Ch 37.2.4 "PLLSTA : PLL Status Register", p 2064 */
  const ra_err_t lock_err = internal_usbhs_wait_pll_lock();
  if (lock_err != k_ra_ok) {
    ra_log_error(s_tag, "usbhs: PHY PLL lock timeout");
    return lock_err;
  }

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
  reg->CFIFOSEL  = k_ra_fifosel_mbw_16;
  reg->D0FIFOSEL = k_ra_fifosel_mbw_16;
  reg->D1FIFOSEL = k_ra_fifosel_mbw_16;

  /* HUM Ch 36.2.20 "DCPMAXP : DCP Max Packet Size Register", p 1990 */
  reg->DCPCFG  = 0U;
  reg->DCPMAXP = k_ra_usb_dcp_max_packet;
  reg->DCPCTR  = 0U;

  /* HUM Ch 36.2.10 "INTENB0 : Interrupt Enable Register 0", p 1980 */
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
  for (uint32_t i = 0U; i < (uint32_t)k_ra_usbhs_scke_poll_limit; ++i) {
    if ((reg->SYSCFG & (uint16_t)(1U << k_ra_syscfg_bit_scke)) != 0U) {
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
  RA_RETURN_ON_ERROR(mst_err, s_tag, "usb_init: mstp enable");

  if (speed == k_ra_usb_speed_hs) {
    const ra_err_t phy_err = internal_usbhs_phy_bringup(reg);
    RA_RETURN_ON_ERROR(phy_err, s_tag, "usb_init: HS PHY bring-up");
  } else {
    const ra_err_t fs_err = internal_usbfs_module_bringup(reg);
    RA_RETURN_ON_ERROR(fs_err, s_tag, "usb_init: FS module bring-up");
  }

  internal_usb_init_common(reg);

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
  if (attached) {
    internal_rmw16(&reg->SYSCFG, dprpu, 0U);
  } else {
    internal_rmw16(&reg->SYSCFG, 0U, dprpu);
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
static uint16_t internal_pipecfg_word(uint8_t ep_addr, ra_usb_ep_dir_t dir, ra_usb_ep_type_t type)
{
  uint16_t cfg = (uint16_t)((uint16_t)ep_addr & k_ra_pipecfg_epnum_mask);
  if (dir == k_ra_usb_ep_dir_in) {
    cfg = (uint16_t)(cfg | k_ra_pipecfg_dir_in);
  }
  if (type == k_ra_usb_ep_type_bulk) {
    cfg = (uint16_t)(cfg | k_ra_pipecfg_type_bulk);
    cfg = (uint16_t)(cfg | k_ra_pipecfg_shtnak);
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
static void internal_fifo_write(volatile r_usb_regs_t* reg, const uint8_t* data, uint16_t len)
{
  /* HUM Ch 36.2.5 "CFIFO : CFIFO Port Register", p 1973 */
  const uint16_t even = (uint16_t)(len >> 1U);
  for (uint16_t i = 0U; i < even; ++i) {
    const uint16_t lo = (uint16_t)data[(2U * i) + 0U];
    const uint16_t hi = (uint16_t)data[(2U * i) + 1U];
    reg->CFIFO        = (uint16_t)(lo | (uint16_t)(hi << k_ra_usb_byte_bits));
  }
  if ((len & 1U) != 0U) {
    reg->CFIFO = (uint16_t)data[len - 1U];
  }
}

/**
 * @brief Drain the CFIFO data port into a buffer (16-bit packed).
 *
 * @details See implementation.
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
static void internal_fifo_read(volatile r_usb_regs_t* reg, uint8_t* data, uint16_t len)
{
  /* HUM Ch 36.2.5 "CFIFO : CFIFO Port Register", p 1973 */
  const uint16_t even = (uint16_t)(len >> 1U);
  for (uint16_t i = 0U; i < even; ++i) {
    const uint16_t word = reg->CFIFO;
    data[(2U * i) + 0U] = (uint8_t)(word & k_ra_usb_byte_mask);
    data[(2U * i) + 1U] = (uint8_t)((word >> k_ra_usb_byte_bits) & k_ra_usb_byte_mask);
  }
  if ((len & 1U) != 0U) {
    const uint16_t word = reg->CFIFO;
    data[len - 1U]      = (uint8_t)(word & k_ra_usb_byte_mask);
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
  RA_RETURN_ON_ERROR(ready, s_tag, "queue_in: FRDY timeout");

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
  RA_RETURN_ON_ERROR(ready, s_tag, "dcp_in_data: FRDY timeout (chunk)");
  internal_fifo_write(reg, p, n);
  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 */
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
    RA_RETURN_ON_ERROR(pushed, s_tag, "dcp_in_data: chunk push failed");
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
  RA_RETURN_ON_ERROR(ready, s_tag, "dcp_in_data: FRDY timeout (zlp)");
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
ra_err_t ra_usb_dcp_in_data(ra_usb_speed_t speed, const uint8_t* data, uint16_t len)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((data == nullptr) && (len != 0U)) {
    return k_ra_err_invalid_arg;
  }

  /* Select DCP (CURPIPE = 0) on CFIFO in IN direction. Done once; the
   * selection persists across chunks.
   * HUM Ch 36.2.7 "CFIFOSEL : CFIFO Port Select Register", p 1976 */
  internal_select_cfifo(reg, 0U, true);

  if (len == 0U) {
    return internal_dcp_in_zlp(reg);
  }
  return internal_dcp_in_payload(reg, data, len);
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
   * 10ms FRDY spin (which would otherwise starve the polled-dispatch
   * worker that calls this every iteration).
   * HUM Ch 36.2.12 "BRDYSTS : BRDY Interrupt Status Register", p 1983. */
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  if ((reg->BRDYSTS & pipe_bit) == 0U) {
    *inout_len = 0U;
    return k_ra_err_no_data;
  }

  internal_select_cfifo(reg, pipe_num, false);
  const ra_err_t ready = internal_wait_frdy(reg);
  RA_RETURN_ON_ERROR(ready, s_tag, "queue_out: FRDY timeout");

  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 */
  const uint16_t available = (uint16_t)(reg->CFIFOCTR & k_ra_fifoctr_dtln);
  if (available == 0U) {
    /* Ack BRDY for this pipe and re-arm. */
    reg->BRDYSTS = (uint16_t)(~pipe_bit);
    *inout_len   = 0U;
    return k_ra_err_no_data;
  }
  const uint16_t take = (available < *inout_len) ? available : *inout_len;
  internal_fifo_read(reg, out_buf, take);
  *inout_len    = take;
  reg->CFIFOCTR = k_ra_fifoctr_bclr;
  /* Ack BRDY for this pipe (W0C: write 0 to clear). */
  reg->BRDYSTS = (uint16_t)(~pipe_bit);
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
 * @brief Implementation of ra_usb_read_setup (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] out_setup See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_usb_read_setup(ra_usb_speed_t speed, ra_usb_setup_t* out_setup)
{
  RA_CHECK_NULL_PTR(out_setup, s_tag, "read_setup: out_setup");
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

static ra_usb_event_fn_t s_usb_fn;
static void*             s_usb_ctx;

/**
 * @brief Implementation of ra_usb_attach_handler (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
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
ra_err_t ra_usb_attach_handler(ra_usb_event_fn_t fn, void* ctx)
{
  s_usb_fn  = fn;
  s_usb_ctx = ctx;
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
   * INTSTS0 is W0C: writing 0 to a bit clears it. The previous
   * implementation did `INTSTS0 = 0` which wiped *every* status bit
   * including VALID (SETUP-latched). That left the downstream handler
   * unable to read the SETUP packet because `ra_usb_read_setup` gates
   * on `INTSTS0.VALID` -- by the time it ran, the controller had
   * already cleared the latch. Result: GET_DESCRIPTOR(DEVICE) SETUPs
   * arrived, IRQs fired, but no descriptor data was ever pushed and
   * DCPCTR.PID stayed NAK.
   *
   * Fix: ack only the edge-triggered control/event bits we have
   * actually consumed (CTRT/DVST/VBINT/RESM/SOFR + the BEMP/BRDY/NRDY
   * group). VALID is preserved so the SETUP handler can drain USBREQ /
   * USBVAL / USBINDX / USBLENG and then clear VALID itself. CTSQ/DVSQ
   * are read-only fields and naturally pass through unchanged. */
  const uint16_t mask = reg->INTSTS0;

  const uint16_t ack_bits = (uint16_t)((1U << k_ra_int0_bit_ctrt) | (1U << k_ra_int0_bit_dvst) |
                                       (1U << k_ra_int0_bit_bemp) | (1U << k_ra_int0_bit_brdy) |
                                       (1U << k_ra_int0_bit_nrdy) | (1U << k_ra_int0_bit_vbse) |
                                       (1U << k_ra_int0_bit_rsme) | (1U << k_ra_int0_bit_sofr));
  reg->INTSTS0            = (uint16_t)(mask & (uint16_t)~ack_bits);

  const ra_usb_event_fn_t fn  = s_usb_fn;
  void* const             ctx = s_usb_ctx;
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
  RA_RETURN_ON_ERROR(mst_err, s_tag, "host_init: mstp enable");

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
