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
  k_ra_usb_frdy_poll_limit = 1000U, /**< Spin-loops before timeout.     */
} ra_usb_internal_lim_t;

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
  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 */
  for (uint16_t i = 0U; i < k_ra_usb_frdy_poll_limit; ++i) {
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
 * @brief Implementation of ra_usb_device_init (see header for full contract).
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
ra_err_t ra_usb_device_init(ra_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  const ra_err_t mst_err = ra_mstp_enable(internal_mstp(speed));
  RA_RETURN_ON_ERROR(mst_err, s_tag, "usb_init: mstp enable");

  /* HUM Ch 36.2.1 "SYSCFG : System Configuration Control Register", p 1966 */
  /* HUM Ch 37.2.1 "SYSCFG : System Configuration Control Register", p 2060 */
  /* SCKE first; clear DRPD (host pull-down); set USBE; HSE on HS only. */
  uint16_t syscfg = (uint16_t)(1U << k_ra_syscfg_bit_scke);
  syscfg          = (uint16_t)(syscfg | (uint16_t)(1U << k_ra_syscfg_bit_usbe));
  if (speed == k_ra_usb_speed_hs) {
    syscfg = (uint16_t)(syscfg | (uint16_t)(1U << k_ra_syscfg_bit_hse));
  }
  reg->SYSCFG = syscfg;

  /* HUM Ch 36.2.7 "CFIFOSEL : CFIFO Port Select Register", p 1976 */
  reg->CFIFOSEL  = k_ra_fifosel_mbw_16;
  reg->D0FIFOSEL = k_ra_fifosel_mbw_16;
  reg->D1FIFOSEL = k_ra_fifosel_mbw_16;

  /* HUM Ch 36.2.20 "DCPMAXP : DCP Max Packet Size Register", p 1990 */
  reg->DCPCFG  = 0U;
  reg->DCPMAXP = k_ra_usb_dcp_max_packet;
  reg->DCPCTR  = 0U;

  /* HUM Ch 36.2.10 "INTENB0 : Interrupt Enable Register 0", p 1980 */
  /* Enable the device-mode interrupt set: BEMP/BRDY/NRDY/CTRT/DVST/VBSE. */
  reg->INTENB0 = (uint16_t)((1U << k_ra_int0_bit_bemp) | (1U << k_ra_int0_bit_brdy) |
                            (1U << k_ra_int0_bit_nrdy) | (1U << k_ra_int0_bit_ctrt) |
                            (1U << k_ra_int0_bit_dvst) | (1U << k_ra_int0_bit_vbse));
  reg->INTENB1 = 0U;
  reg->BRDYENB = 0U;
  reg->NRDYENB = 0U;
  reg->BEMPENB = 0U;

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
 * @brief Implementation of ra_usb_configure_endpoint (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] speed See implementation.
 * @param[in] pipe_num See implementation.
 * @param[in] ep_addr See implementation.
 * @param[in] dir See implementation.
 * @param[in] type See implementation.
 * @param[in] max_packet See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
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

  /* HUM Ch 36.2.23 "PIPESEL : Pipe Window Select Register", p 1995 */
  reg->PIPESEL = pipe_num;

  /* HUM Ch 36.2.24 "PIPECFG : Pipe Configuration Register", p 1996 */
  reg->PIPECFG = internal_pipecfg_word(ep_addr, dir, type);

  /* HUM Ch 36.2.26 "PIPEMAXP : Pipe Maximum Packet Size Register", p 2003 */
  reg->PIPEMAXP = max_packet;
  reg->PIPEPERI = 0U;

  /* HUM Ch 36.2.27 "PIPEnCTR : PIPE n Control Register", p 2005 */
  /* PID = NAK; toggle clear so first packet is DATA0. */
  internal_pipe_pid(reg, pipe_num, k_ra_pid_nak);
  internal_rmw16(&reg->PIPECTR[(uint8_t)(pipe_num - 1U)], k_ra_pipectr_sqclr, 0U);
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

  internal_select_cfifo(reg, pipe_num, false);
  const ra_err_t ready = internal_wait_frdy(reg);
  RA_RETURN_ON_ERROR(ready, s_tag, "queue_out: FRDY timeout");

  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979 */
  const uint16_t available = (uint16_t)(reg->CFIFOCTR & k_ra_fifoctr_dtln);
  if (available == 0U) {
    *inout_len = 0U;
    return k_ra_err_no_data;
  }
  const uint16_t take = (available < *inout_len) ? available : *inout_len;
  internal_fifo_read(reg, out_buf, take);
  *inout_len    = take;
  reg->CFIFOCTR = k_ra_fifoctr_bclr;
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
  /* HUM Ch 36.2.14 "INTSTS0 : Interrupt Status Register 0", p 1985 */
  const uint16_t          mask = reg->INTSTS0;
  const ra_usb_event_fn_t fn   = s_usb_fn;
  void* const             ctx  = s_usb_ctx;
  reg->INTSTS0                 = 0U;
  if (fn != nullptr) {
    fn(ctx, speed, mask);
  }
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
