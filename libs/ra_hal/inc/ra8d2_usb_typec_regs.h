/**
 * @file ra8d2_usb_typec_regs.h
 * @brief USB Type-C / Power Delivery controller register layout (RA8D2)
 *
 * @details
 * The RA8 family Type-C block (`R_USBCC` in FSP) is a small 24-byte
 * register window on the peripheral bus. The struct mirrors the
 * `R_USBCC_Type` layout from the FSP CMSIS BSP header
 * `R7FA2L209.h` (the RA8D2 BSP `R7KA8D2KF_core0.h` does NOT define
 * this block -- the silicon documentation is sparse, so this file
 * derives the layout from the closest sibling BSP that does declare
 * USBCC). The driver programs registers in the same order
 * `r_usb_typec.c` does.
 *
 * The 24-byte FSP block covers Type-C plug-orientation, CC voltage
 * detection and source-current detection, but it does NOT contain
 * a USB-PD message FIFO. PD framing on RA silicon today is done by
 * a separate, undocumented co-processor. We expose a stub PD
 * message-FIFO register pair after the official window so the
 * driver code path that the task spec requires (send / receive PD
 * messages) has somewhere to land in simulator tests. Each PD-FIFO
 * field is tagged TODO and references the USB PD 3.0 spec section
 * that defines the wire-format byte being staged.
 *
 * ## Register map
 *
 * | Offset | Name | Width | Purpose                                |
 * |-------:|------|------:|----------------------------------------|
 * | 0x00   | TCC  | 32    | TYPE-C Control (b31 RESET)             |
 * | 0x04   | MEC  | 32    | Mode + state (MODE / PMODE / GD / GUS) |
 * | 0x08   | CCC  | 32    | CC-PHY Control (RD / ZOPEN / PDOWN)    |
 * | 0x0C   | IES  | 32    | Interrupt enable + status flags        |
 * | 0x10   | TCS  | 32    | Connection state + CC1/CC2 status      |
 * | 0x14   | TSET | 32    | Type-C debounce timers                 |
 * | 0x18   | PDFC | 32    | (driver stub) PD FIFO Control          |
 * | 0x1C   | PDFD | 32    | (driver stub) PD FIFO Data port        |
 * | 0x20   | PDFS | 32    | (driver stub) PD FIFO Status           |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra_usb_typec_addr_t
 * @brief Base address of the Type-C controller block.
 *
 * @details Sourced from FSP `R_USBCC` placement (`@ 0x40091000`).
 * The RA8D2 HUM does not document USBCC; the address falls inside
 * the simulator's peripheral window (0x40000000 + 8 MiB), so the
 * host tests can back the registers with ordinary memory.
 */
typedef enum : uintptr_t {
  k_ra_usb_typec_base_addr = 0x40091000UL, /**< R_USBCC base. */
} ra_usb_typec_addr_t;

/**
 * @enum ra_usb_typec_tcc_bit_t
 * @brief Bit positions for the TCC (Type-C Control) register.
 */
typedef enum : uint8_t {
  k_ra_typec_tcc_bit_reset = 31U, /**< b31 software reset. */
} ra_usb_typec_tcc_bit_t;

/**
 * @enum ra_usb_typec_mec_bit_t
 * @brief Bit positions for the MEC (Mode/State Control) register.
 */
typedef enum : uint8_t {
  k_ra_typec_mec_bit_mode  = 0U,  /**< b0  Connection State Mode.   */
  k_ra_typec_mec_bit_pmode = 1U,  /**< b2..b1 Source detect mode.   */
  k_ra_typec_mec_bit_gd    = 16U, /**< b16 Force to Disable.        */
  k_ra_typec_mec_bit_gus   = 17U, /**< b17 Force to Unattached_SNK. */
} ra_usb_typec_mec_bit_t;

/**
 * @enum ra_usb_typec_ccc_bit_t
 * @brief Bit positions for the CCC (CC-PHY Control) register.
 */
typedef enum : uint8_t {
  k_ra_typec_ccc_bit_rd      = 0U,  /**< b0  Rd termination status.   */
  k_ra_typec_ccc_bit_zopen   = 1U,  /**< b1  zOpen control.           */
  k_ra_typec_ccc_bit_ccsel   = 8U,  /**< b8  CC selection.            */
  k_ra_typec_ccc_bit_vraen   = 16U, /**< b16 vRa detect enable.       */
  k_ra_typec_ccc_bit_vrd15en = 17U, /**< b17 vRd-1.5 detect enable.   */
  k_ra_typec_ccc_bit_vrd30en = 18U, /**< b18 vRd-3.0 detect enable.   */
  k_ra_typec_ccc_bit_pdown   = 31U, /**< b31 CC-PHY power down.       */
} ra_usb_typec_ccc_bit_t;

/**
 * @enum ra_usb_typec_ies_bit_t
 * @brief Bit positions for IES (Interrupt enable + status).
 */
typedef enum : uint8_t {
  k_ra_typec_ies_bit_iscn    = 0U,  /**< b0  Interrupt Status Conn.   */
  k_ra_typec_ies_bit_iscc    = 4U,  /**< b4  Interrupt Status CC.     */
  k_ra_typec_ies_bit_isvbus  = 5U,  /**< b5  Interrupt Status VBUS.   */
  k_ra_typec_ies_bit_isvra   = 15U, /**< b15 Interrupt Status VRA.    */
  k_ra_typec_ies_bit_cien    = 16U, /**< b16 Conn IRQ enable.         */
  k_ra_typec_ies_bit_ccien   = 20U, /**< b20 CC IRQ enable.           */
  k_ra_typec_ies_bit_vbusien = 21U, /**< b21 VBUS IRQ enable.         */
  k_ra_typec_ies_bit_vraien  = 31U, /**< b31 VRA IRQ enable.          */
} ra_usb_typec_ies_bit_t;

/**
 * @enum ra_usb_typec_tcs_field_t
 * @brief Bit positions and shifts for TCS (Connection state).
 */
typedef enum : uint8_t {
  k_ra_typec_tcs_bit_srcd   = 0U,  /**< b0  Source connection.       */
  k_ra_typec_tcs_bit_vrd15d = 1U,  /**< b1  Power1.5 source detect.  */
  k_ra_typec_tcs_bit_vrd30d = 2U,  /**< b2  Power3.0 source detect.  */
  k_ra_typec_tcs_bit_plug   = 3U,  /**< b3  Plug orientation flag.   */
  k_ra_typec_tcs_shift_cns  = 4U,  /**< b7..b4 CC connection state.  */
  k_ra_typec_tcs_shift_cc1s = 8U,  /**< b9..b8  CC1 voltage state.   */
  k_ra_typec_tcs_shift_cc2s = 10U, /**< b11..b10 CC2 voltage state.  */
  k_ra_typec_tcs_bit_vbuss  = 12U, /**< b12 VBUS present flag.       */
} ra_usb_typec_tcs_field_t;

/**
 * @enum ra_usb_typec_field_mask_t
 * @brief 32-bit field masks for TCS / MEC composite fields.
 */
typedef enum : uint32_t {
  k_ra_typec_mask_cns      = 0x000000F0UL, /**< CNS  4-bit field.    */
  k_ra_typec_mask_cc1s     = 0x00000300UL, /**< CC1S 2-bit field.    */
  k_ra_typec_mask_cc2s     = 0x00000C00UL, /**< CC2S 2-bit field.    */
  k_ra_typec_mask_pmode    = 0x00000006UL, /**< PMODE 2-bit field.   */
  k_ra_typec_mask_pdfs_cnt = 0x000000FFUL, /**< PD FIFO count.       */
} ra_usb_typec_field_mask_t;

/**
 * @enum ra_usb_typec_pdfc_bit_t
 * @brief (driver stub) PD FIFO Control register bits.
 *
 * @details TODO: there is no public RA8 register for these bits;
 * they are placeholders so the driver and its host-side tests have
 * something concrete to write. When the silicon spec lands, replace
 * this enum with the real BMC / PHY-PD register bits.
 */
typedef enum : uint8_t {
  k_ra_typec_pdfc_bit_tx_start = 0U, /**< b0 Start TX of staged msg. */
  k_ra_typec_pdfc_bit_rx_drop  = 1U, /**< b1 Drop oldest RX entry.   */
  k_ra_typec_pdfc_bit_irq_en   = 8U, /**< b8 PD msg IRQ enable.      */
} ra_usb_typec_pdfc_bit_t;

/**
 * @enum ra_usb_typec_pdfs_bit_t
 * @brief (driver stub) PD FIFO Status register bits.
 *
 * @details TODO: placeholder, see ::ra_usb_typec_pdfc_bit_t.
 */
typedef enum : uint8_t {
  k_ra_typec_pdfs_bit_rx_avail = 16U, /**< b16 RX msg pending. */
  k_ra_typec_pdfs_bit_tx_busy  = 17U, /**< b17 TX in progress. */
} ra_usb_typec_pdfs_bit_t;

/**
 * @struct r_usb_typec_regs_t
 * @brief Register window for the RA8 USB Type-C / PD controller.
 *
 * @details
 * Bytes 0x00..0x17 mirror FSP `R_USBCC_Type` exactly. Bytes
 * 0x18..0x23 are this driver's PD FIFO stub and are flagged with
 * TODO comments -- the silicon-true layout will replace them once
 * the RA8D2 HUM USB-PD chapter is published.
 *
 * @invariant TCC.RESET reads as 0 once reset completes.
 * @invariant CCC.PDOWN reads as 0 while the CC-PHY is powered.
 *
 * @see r_usb_typec.c FSP reference driver
 * @since 0.1.0
 */
typedef struct {
  volatile uint32_t TCC;  /**< +0x00 Type-C Control.                 */
  volatile uint32_t MEC;  /**< +0x04 Mode + State Control.           */
  volatile uint32_t CCC;  /**< +0x08 CC-PHY Control.                 */
  volatile uint32_t IES;  /**< +0x0C IRQ enable + status.            */
  volatile uint32_t TCS;  /**< +0x10 Connection state (read-only).   */
  volatile uint32_t TSET; /**< +0x14 Debounce timer setting.         */
  volatile uint32_t PDFC; /**< +0x18 (TODO) PD FIFO control.         */
  volatile uint32_t PDFD; /**< +0x1C (TODO) PD FIFO data port.       */
  volatile uint32_t PDFS; /**< +0x20 (TODO) PD FIFO status / count.  */
} r_usb_typec_regs_t;

/**
 * @brief Get pointer to the Type-C controller register block.
 *
 * @return Volatile pointer to the mapped 36-byte window.
 *
 * @pre On target: USBCC MSTP gate is open.
 * @pre On host (`RA_SIMULATOR_MODE`): `ra_sim_mmap` constructor ran.
 *
 * @post Returned pointer is non-NULL.
 *
 * @note Re-entrant -- read-only state.
 * @since 0.1.0
 */
static inline volatile r_usb_typec_regs_t* ra_usb_typec_regs(void)
{
  return (volatile r_usb_typec_regs_t*)k_ra_usb_typec_base_addr;
}

#ifdef __cplusplus
}
#endif
