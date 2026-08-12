/**
 * @file ra8_usb_regs.h
 * @brief USB Full-Speed + High-Speed controller layout for the Renesas RA8D2
 * @ingroup grp_hal_usb
 *
 * @details
 * RA8D2 has two USB controllers:
 *
 *  - **USB FS** at `0x40250000` -- 12 Mbps, integrated PHY (HUM Ch 36).
 *  - **USB HS** at `0x40351000` -- 480 Mbps, embedded HS PHY (HUM Ch 37).
 *
 * Both use the Renesas "USB2_B" IP (common across RA / RZ / RX). The
 * register layouts are identical between the two instances for the
 * device-side bring-up subset modelled here; HS-only registers
 * (PHYSET, LPSTS, PLLSTA, ...) live past offset 0x90 and are not
 * consumed by the device-mode driver yet.
 *
 * Device and host mode share the same register block; the mode bit
 * is in `SYSCFG.DCFM`. This driver targets device mode only.
 *
 * ## Register map (device-mode subset)
 *
 * | Offset | Name      | Width | Purpose                              |
 * |-------:|-----------|------:|--------------------------------------|
 * | 0x000  | SYSCFG    | 16    | System Configuration                 |
 * | 0x002  | BUSWAIT   | 16    | CPU bus wait                         |
 * | 0x004  | SYSSTS0   | 16    | System Status 0                      |
 * | 0x008  | DVSTCTR0  | 16    | Device State Control 0               |
 * | 0x014  | CFIFO     | 16    | Control FIFO port                    |
 * | 0x018  | D0FIFO    | 16    | DMA FIFO 0                           |
 * | 0x01C  | D1FIFO    | 16    | DMA FIFO 1                           |
 * | 0x020  | CFIFOSEL  | 16    | Control FIFO select                  |
 * | 0x022  | CFIFOCTR  | 16    | Control FIFO control (FRDY/BVAL/BCLR)|
 * | 0x028  | D0FIFOSEL | 16    | DMA FIFO 0 select                    |
 * | 0x02A  | D0FIFOCTR | 16    | DMA FIFO 0 control                   |
 * | 0x02C  | D1FIFOSEL | 16    | DMA FIFO 1 select                    |
 * | 0x02E  | D1FIFOCTR | 16    | DMA FIFO 1 control                   |
 * | 0x030  | INTENB0   | 16    | Interrupt Enable 0                   |
 * | 0x032  | INTENB1   | 16    | Interrupt Enable 1                   |
 * | 0x036  | BRDYENB   | 16    | BRDY Interrupt Enable                |
 * | 0x038  | NRDYENB   | 16    | NRDY Interrupt Enable                |
 * | 0x03A  | BEMPENB   | 16    | BEMP Interrupt Enable                |
 * | 0x03C  | SOFCFG    | 16    | SOF Output Configuration             |
 * | 0x040  | INTSTS0   | 16    | Interrupt Status 0                   |
 * | 0x042  | INTSTS1   | 16    | Interrupt Status 1                   |
 * | 0x046  | BRDYSTS   | 16    | BRDY Interrupt Status                |
 * | 0x048  | NRDYSTS   | 16    | NRDY Interrupt Status                |
 * | 0x04A  | BEMPSTS   | 16    | BEMP Interrupt Status                |
 * | 0x04C  | FRMNUM    | 16    | Frame Number                         |
 * | 0x050  | USBADDR   | 16    | USB Address                          |
 * | 0x054  | USBREQ    | 16    | USB Request Type / bRequest          |
 * | 0x056  | USBVAL    | 16    | USB Request wValue                   |
 * | 0x058  | USBINDX   | 16    | USB Request wIndex                   |
 * | 0x05A  | USBLENG   | 16    | USB Request wLength                  |
 * | 0x05C  | DCPCFG    | 16    | DCP Config                           |
 * | 0x05E  | DCPMAXP   | 16    | DCP Max Packet                       |
 * | 0x060  | DCPCTR    | 16    | DCP Control (PID/CCPL/SUREQ)         |
 * | 0x064  | PIPESEL   | 16    | Pipe window select                   |
 * | 0x068  | PIPECFG   | 16    | Selected pipe Config                 |
 * | 0x06A  | PIPEBUF   | 16    | Selected pipe Buffer                 |
 * | 0x06C  | PIPEMAXP  | 16    | Selected pipe Max Packet             |
 * | 0x06E  | PIPEPERI  | 16    | Selected pipe Period                 |
 * | 0x070  | PIPECTR[9]| 16x9  | PIPE1..PIPE9 Control                 |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum ra8_usb_addr_t
 * @brief Per-instance base addresses for the two USB controllers.
 *
 * @details Sourced from HUM Ch 36 "USB 2.0 Full-Speed Module (USBFS)"
 * p 1965 and HUM Ch 37 "USB 2.0 High-Speed Module (USBHS)" p 2059.
 */
typedef enum : uintptr_t {
#ifdef RA8_PERIPH_NS_ALIAS
  /* TrustZone Non-secure build (two-project NS image, #96): reach the
   * controllers through the IDAU bit[28]=1 Non-secure alias (HUM "PSCU_NS =
   * 0x5020_4000" pattern, +0x1000_0000) so the SAU NS peripheral region
   * (0x5000_0000-0xDFFF_FFFF) permits the access. The Secure side marks USBFS0
   * Non-secure in PSARB (PSARB11) so the alias actually responds. */
  k_ra8_usb_fs0_base_addr = 0x50250000UL, /**< USB-FS base (NS alias bit[28]=1). */
  k_ra8_usb_hs0_base_addr = 0x50351000UL, /**< USB-HS base (NS alias bit[28]=1). */
#else
  k_ra8_usb_fs0_base_addr = 0x40250000UL, /**< USB Full-Speed base. */
  k_ra8_usb_hs0_base_addr = 0x40351000UL, /**< USB High-Speed base. */
#endif /**< (anon). */
} ra8_usb_addr_t;

/**
 * @enum ra8_usb_limits_t
 * @brief Pipe / endpoint count constants.
 */
typedef enum : uint8_t {
  k_ra8_usb_pipe_count        = 10U, /**< DCP (pipe 0) + PIPE1..PIPE9.  */
  k_ra8_usb_pipectr_count     = 9U,  /**< PIPECTR[9] = PIPE1..PIPE9.    */
  k_ra8_usb_pad_2             = 2U,  /**< Reserved length (2 hwords).   */
  k_ra8_usb_pad_3             = 3U,  /**< Reserved length (3 hwords).   */
  k_ra8_usb_pad_5             = 5U,  /**< Reserved length (5 hwords).   */
  k_ra8_usb_pad_8             = 8U,  /**< Reserved length (8 hwords).   */
  k_ra8_usb_dcp_max_packet_fs = 64U, /**< EP0 max packet at FS / HS-FS. */
} ra8_usb_limits_t;

/**
 * @struct r_usb_regs_t
 * @brief USB FS / HS register window (device-mode subset).
 *
 * @details Modelled after the FSP/CMSIS `R_USB_FS0_Type` and
 * `R_USB_HS0_Type` register layouts, restricted to the offsets the
 * device-mode driver actually touches. Trailing HS-only registers
 * (PHYSET, LPCTRL, LPSTS, BCCTRL, ...) are intentionally omitted;
 * the driver does not enable HS-PHY power-down sequences yet.
 */
typedef struct {
  volatile uint16_t SYSCFG;                           /**< +0x000 System Configuration.      */
  volatile uint16_t BUSWAIT;                          /**< +0x002 CPU bus wait.              */
  volatile uint16_t SYSSTS0;                          /**< +0x004 System Status 0.           */
  volatile uint16_t _r0;                              /**< +0x006 Reserved.                  */
  volatile uint16_t DVSTCTR0;                         /**< +0x008 Device State Control.      */
  volatile uint16_t _r1[k_ra8_usb_pad_5];             /**< Reserved.                         */
  volatile uint16_t CFIFO;                            /**< +0x014 Control FIFO data port.    */
  volatile uint16_t _r2;                              /**< +0x016 Reserved.                  */
  volatile uint16_t D0FIFO;                           /**< +0x018 DMA FIFO 0 data port.      */
  volatile uint16_t _r3;                              /**< +0x01A Reserved.                  */
  volatile uint16_t D1FIFO;                           /**< +0x01C DMA FIFO 1 data port.      */
  volatile uint16_t _r4;                              /**< +0x01E Reserved.                  */
  volatile uint16_t CFIFOSEL;                         /**< +0x020 Control FIFO select.       */
  volatile uint16_t CFIFOCTR;                         /**< +0x022 Control FIFO control.      */
  volatile uint16_t _r5[k_ra8_usb_pad_2];             /**< Reserved.                         */
  volatile uint16_t D0FIFOSEL;                        /**< +0x028 DMA FIFO 0 select.         */
  volatile uint16_t D0FIFOCTR;                        /**< +0x02A DMA FIFO 0 control.        */
  volatile uint16_t D1FIFOSEL;                        /**< +0x02C DMA FIFO 1 select.         */
  volatile uint16_t D1FIFOCTR;                        /**< +0x02E DMA FIFO 1 control.        */
  volatile uint16_t INTENB0;                          /**< +0x030 Interrupt Enable 0.        */
  volatile uint16_t INTENB1;                          /**< +0x032 Interrupt Enable 1.        */
  volatile uint16_t _r6;                              /**< +0x034 Reserved.                  */
  volatile uint16_t BRDYENB;                          /**< +0x036 BRDY Interrupt Enable.     */
  volatile uint16_t NRDYENB;                          /**< +0x038 NRDY Interrupt Enable.     */
  volatile uint16_t BEMPENB;                          /**< +0x03A BEMP Interrupt Enable.     */
  volatile uint16_t SOFCFG;                           /**< +0x03C SOF Output Configuration.  */
  volatile uint16_t _r7;                              /**< +0x03E Reserved.                  */
  volatile uint16_t INTSTS0;                          /**< +0x040 Interrupt Status 0.        */
  volatile uint16_t INTSTS1;                          /**< +0x042 Interrupt Status 1.        */
  volatile uint16_t _r8;                              /**< +0x044 Reserved.                  */
  volatile uint16_t BRDYSTS;                          /**< +0x046 BRDY Interrupt Status.     */
  volatile uint16_t NRDYSTS;                          /**< +0x048 NRDY Interrupt Status.     */
  volatile uint16_t BEMPSTS;                          /**< +0x04A BEMP Interrupt Status.     */
  volatile uint16_t FRMNUM;                           /**< +0x04C Frame Number.              */
  volatile uint16_t _r9;                              /**< +0x04E Reserved.                  */
  volatile uint16_t USBADDR;                          /**< +0x050 USB Address.               */
  volatile uint16_t _ra;                              /**< +0x052 Reserved.                  */
  volatile uint16_t USBREQ;                           /**< +0x054 USB Request type/bRequest. */
  volatile uint16_t USBVAL;                           /**< +0x056 USB Request wValue.        */
  volatile uint16_t USBINDX;                          /**< +0x058 USB Request wIndex.        */
  volatile uint16_t USBLENG;                          /**< +0x05A USB Request wLength.       */
  volatile uint16_t DCPCFG;                           /**< +0x05C DCP Configuration.         */
  volatile uint16_t DCPMAXP;                          /**< +0x05E DCP Max Packet.            */
  volatile uint16_t DCPCTR;                           /**< +0x060 DCP Control.               */
  volatile uint16_t _rb;                              /**< +0x062 Reserved.                  */
  volatile uint16_t PIPESEL;                          /**< +0x064 Pipe window select.        */
  volatile uint16_t _rc;                              /**< +0x066 Reserved.                  */
  volatile uint16_t PIPECFG;                          /**< +0x068 Selected pipe config.      */
  volatile uint16_t PIPEBUF;                          /**< +0x06A Selected pipe buffer.      */
  volatile uint16_t PIPEMAXP;                         /**< +0x06C Selected pipe max packet.  */
  volatile uint16_t PIPEPERI;                         /**< +0x06E Selected pipe period.      */
  volatile uint16_t PIPECTR[k_ra8_usb_pipectr_count]; /**< +0x070 PIPE1..9 ctrl.             */
} r_usb_regs_t;

/**
 * @enum ra8_usb_offset_t
 * @brief Byte offsets of the named USB register fields within `r_usb_regs_t`.
 *
 * @details Sourced from HUM Ch 36 (USBFS, p 1965) and HUM Ch 37 (USBHS,
 * p 2059). Both controllers share the same device-mode register layout in
 * this offset window, so a single set of constants covers both instances.
 */
typedef enum : uint16_t {
  k_ra8_usb_off_syscfg    = 0x000U, /**< SYSCFG    : System config.        */
  k_ra8_usb_off_buswait   = 0x002U, /**< BUSWAIT   : CPU bus wait.         */
  k_ra8_usb_off_syssts0   = 0x004U, /**< SYSSTS0   : System status 0.      */
  k_ra8_usb_off_dvstctr0  = 0x008U, /**< DVSTCTR0  : Device state ctrl.    */
  k_ra8_usb_off_cfifo     = 0x014U, /**< CFIFO     : Control FIFO port.    */
  k_ra8_usb_off_d0fifo    = 0x018U, /**< D0FIFO    : DMA FIFO 0 port.      */
  k_ra8_usb_off_d1fifo    = 0x01CU, /**< D1FIFO    : DMA FIFO 1 port.      */
  k_ra8_usb_off_cfifosel  = 0x020U, /**< CFIFOSEL  : Control FIFO select.  */
  k_ra8_usb_off_cfifoctr  = 0x022U, /**< CFIFOCTR  : Control FIFO control. */
  k_ra8_usb_off_d0fifosel = 0x028U, /**< D0FIFOSEL : DMA FIFO 0 select.    */
  k_ra8_usb_off_d0fifoctr = 0x02AU, /**< D0FIFOCTR : DMA FIFO 0 control.   */
  k_ra8_usb_off_d1fifosel = 0x02CU, /**< D1FIFOSEL : DMA FIFO 1 select.    */
  k_ra8_usb_off_d1fifoctr = 0x02EU, /**< D1FIFOCTR : DMA FIFO 1 control.   */
  k_ra8_usb_off_intenb0   = 0x030U, /**< INTENB0   : Interrupt Enable 0.   */
  k_ra8_usb_off_intenb1   = 0x032U, /**< INTENB1   : Interrupt Enable 1.   */
  k_ra8_usb_off_brdyenb   = 0x036U, /**< BRDYENB   : BRDY Enable.          */
  k_ra8_usb_off_nrdyenb   = 0x038U, /**< NRDYENB   : NRDY Enable.          */
  k_ra8_usb_off_bempenb   = 0x03AU, /**< BEMPENB   : BEMP Enable.          */
  k_ra8_usb_off_sofcfg    = 0x03CU, /**< SOFCFG    : SOF Output Config.    */
  k_ra8_usb_off_intsts0   = 0x040U, /**< INTSTS0   : Interrupt Status 0.   */
  k_ra8_usb_off_intsts1   = 0x042U, /**< INTSTS1   : Interrupt Status 1.   */
  k_ra8_usb_off_brdysts   = 0x046U, /**< BRDYSTS   : BRDY Status.          */
  k_ra8_usb_off_nrdysts   = 0x048U, /**< NRDYSTS   : NRDY Status.          */
  k_ra8_usb_off_bempsts   = 0x04AU, /**< BEMPSTS   : BEMP Status.          */
  k_ra8_usb_off_frmnum    = 0x04CU, /**< FRMNUM    : Frame number.         */
  k_ra8_usb_off_usbaddr   = 0x050U, /**< USBADDR   : USB address.          */
  k_ra8_usb_off_usbreq    = 0x054U, /**< USBREQ    : USB request type.     */
  k_ra8_usb_off_usbval    = 0x056U, /**< USBVAL    : USB request wValue.   */
  k_ra8_usb_off_usbindx   = 0x058U, /**< USBINDX   : USB request wIndex.   */
  k_ra8_usb_off_usbleng   = 0x05AU, /**< USBLENG   : USB request wLength.  */
  k_ra8_usb_off_dcpcfg    = 0x05CU, /**< DCPCFG    : DCP configuration.    */
  k_ra8_usb_off_dcpmaxp   = 0x05EU, /**< DCPMAXP   : DCP max packet.       */
  k_ra8_usb_off_dcpctr    = 0x060U, /**< DCPCTR    : DCP control.          */
  k_ra8_usb_off_pipesel   = 0x064U, /**< PIPESEL   : Pipe window select.   */
  k_ra8_usb_off_pipecfg   = 0x068U, /**< PIPECFG   : Selected pipe cfg.    */
  k_ra8_usb_off_pipebuf   = 0x06AU, /**< PIPEBUF   : Selected pipe buf.    */
  k_ra8_usb_off_pipemaxp  = 0x06CU, /**< PIPEMAXP  : Selected pipe maxp.   */
  k_ra8_usb_off_pipeperi  = 0x06EU, /**< PIPEPERI  : Selected pipe period. */
  k_ra8_usb_off_pipectr   = 0x070U, /**< PIPECTR   : PIPE1..9 control.     */
} ra8_usb_offset_t;

/**
 * @enum ra8_usb_size_t
 * @brief Total byte size of the modelled USB register window.
 *
 * @details The device-mode subset ends at PIPECTR[9] (last halfword at
 * offset 0x080..0x081). HS-only registers past 0x090 (PHYSET, LPCTRL,
 * LPSTS, BCCTRL, ...) are intentionally not modelled.
 */
typedef enum : uint16_t {
  k_ra8_usb_window_bytes = 0x082U, /**< 0x070 + (9 * 2) = 0x082 bytes. */
} ra8_usb_size_t;

/**
 * @brief Compile-time offset and size insurance for `r_usb_regs_t`.
 *
 * @details
 * Mirrors HUM Ch 36 / Ch 37 byte-for-byte. If any field drifts (a
 * reserved gap is mis-sized, or a halfword-versus-word width slips),
 * these assertions fire at compile time before any test runs -- silent
 * register-bank corruption (writing the wrong USB pipe, or stomping on
 * the next peripheral) is avoided.
 *
 * Both USB FS (HUM Ch 36, p 1965) and USB HS (HUM Ch 37, p 2059) share
 * the same device-mode register layout in this window, so one set of
 * offset assertions covers both controller instances.
 */
static_assert(sizeof(r_usb_regs_t) == (size_t)k_ra8_usb_window_bytes,
              "r_usb_regs_t window size must be 0x082 bytes (PIPECTR[9] end)");

static_assert(offsetof(r_usb_regs_t, SYSCFG) == (size_t)k_ra8_usb_off_syscfg, "SYSCFG offset");
static_assert(offsetof(r_usb_regs_t, BUSWAIT) == (size_t)k_ra8_usb_off_buswait, "BUSWAIT offset");
static_assert(offsetof(r_usb_regs_t, SYSSTS0) == (size_t)k_ra8_usb_off_syssts0, "SYSSTS0 offset");
static_assert(offsetof(r_usb_regs_t, DVSTCTR0) == (size_t)k_ra8_usb_off_dvstctr0,
              "DVSTCTR0 offset");
static_assert(offsetof(r_usb_regs_t, CFIFO) == (size_t)k_ra8_usb_off_cfifo, "CFIFO offset");
static_assert(offsetof(r_usb_regs_t, D0FIFO) == (size_t)k_ra8_usb_off_d0fifo, "D0FIFO offset");
static_assert(offsetof(r_usb_regs_t, D1FIFO) == (size_t)k_ra8_usb_off_d1fifo, "D1FIFO offset");
static_assert(offsetof(r_usb_regs_t, CFIFOSEL) == (size_t)k_ra8_usb_off_cfifosel,
              "CFIFOSEL offset");
static_assert(offsetof(r_usb_regs_t, CFIFOCTR) == (size_t)k_ra8_usb_off_cfifoctr,
              "CFIFOCTR offset");
static_assert(offsetof(r_usb_regs_t, D0FIFOSEL) == (size_t)k_ra8_usb_off_d0fifosel,
              "D0FIFOSEL offset");
static_assert(offsetof(r_usb_regs_t, D0FIFOCTR) == (size_t)k_ra8_usb_off_d0fifoctr,
              "D0FIFOCTR offset");
static_assert(offsetof(r_usb_regs_t, D1FIFOSEL) == (size_t)k_ra8_usb_off_d1fifosel,
              "D1FIFOSEL offset");
static_assert(offsetof(r_usb_regs_t, D1FIFOCTR) == (size_t)k_ra8_usb_off_d1fifoctr,
              "D1FIFOCTR offset");
static_assert(offsetof(r_usb_regs_t, INTENB0) == (size_t)k_ra8_usb_off_intenb0, "INTENB0 offset");
static_assert(offsetof(r_usb_regs_t, INTENB1) == (size_t)k_ra8_usb_off_intenb1, "INTENB1 offset");
static_assert(offsetof(r_usb_regs_t, BRDYENB) == (size_t)k_ra8_usb_off_brdyenb, "BRDYENB offset");
static_assert(offsetof(r_usb_regs_t, NRDYENB) == (size_t)k_ra8_usb_off_nrdyenb, "NRDYENB offset");
static_assert(offsetof(r_usb_regs_t, BEMPENB) == (size_t)k_ra8_usb_off_bempenb, "BEMPENB offset");
static_assert(offsetof(r_usb_regs_t, SOFCFG) == (size_t)k_ra8_usb_off_sofcfg, "SOFCFG offset");
static_assert(offsetof(r_usb_regs_t, INTSTS0) == (size_t)k_ra8_usb_off_intsts0, "INTSTS0 offset");
static_assert(offsetof(r_usb_regs_t, INTSTS1) == (size_t)k_ra8_usb_off_intsts1, "INTSTS1 offset");
static_assert(offsetof(r_usb_regs_t, BRDYSTS) == (size_t)k_ra8_usb_off_brdysts, "BRDYSTS offset");
static_assert(offsetof(r_usb_regs_t, NRDYSTS) == (size_t)k_ra8_usb_off_nrdysts, "NRDYSTS offset");
static_assert(offsetof(r_usb_regs_t, BEMPSTS) == (size_t)k_ra8_usb_off_bempsts, "BEMPSTS offset");
static_assert(offsetof(r_usb_regs_t, FRMNUM) == (size_t)k_ra8_usb_off_frmnum, "FRMNUM offset");
static_assert(offsetof(r_usb_regs_t, USBADDR) == (size_t)k_ra8_usb_off_usbaddr, "USBADDR offset");
static_assert(offsetof(r_usb_regs_t, USBREQ) == (size_t)k_ra8_usb_off_usbreq, "USBREQ offset");
static_assert(offsetof(r_usb_regs_t, USBVAL) == (size_t)k_ra8_usb_off_usbval, "USBVAL offset");
static_assert(offsetof(r_usb_regs_t, USBINDX) == (size_t)k_ra8_usb_off_usbindx, "USBINDX offset");
static_assert(offsetof(r_usb_regs_t, USBLENG) == (size_t)k_ra8_usb_off_usbleng, "USBLENG offset");
static_assert(offsetof(r_usb_regs_t, DCPCFG) == (size_t)k_ra8_usb_off_dcpcfg, "DCPCFG offset");
static_assert(offsetof(r_usb_regs_t, DCPMAXP) == (size_t)k_ra8_usb_off_dcpmaxp, "DCPMAXP offset");
static_assert(offsetof(r_usb_regs_t, DCPCTR) == (size_t)k_ra8_usb_off_dcpctr, "DCPCTR offset");
static_assert(offsetof(r_usb_regs_t, PIPESEL) == (size_t)k_ra8_usb_off_pipesel, "PIPESEL offset");
static_assert(offsetof(r_usb_regs_t, PIPECFG) == (size_t)k_ra8_usb_off_pipecfg, "PIPECFG offset");
static_assert(offsetof(r_usb_regs_t, PIPEBUF) == (size_t)k_ra8_usb_off_pipebuf, "PIPEBUF offset");
static_assert(offsetof(r_usb_regs_t, PIPEMAXP) == (size_t)k_ra8_usb_off_pipemaxp,
              "PIPEMAXP offset");
static_assert(offsetof(r_usb_regs_t, PIPEPERI) == (size_t)k_ra8_usb_off_pipeperi,
              "PIPEPERI offset");
static_assert(offsetof(r_usb_regs_t, PIPECTR) == (size_t)k_ra8_usb_off_pipectr,
              "PIPECTR base offset");
static_assert(sizeof(((r_usb_regs_t*)0)->PIPECTR) ==
                ((size_t)k_ra8_usb_pipectr_count * sizeof(uint16_t)),
              "PIPECTR array spans PIPE1..PIPE9 (9 * 2 bytes)");

/** @brief Get pointer to the USB FS controller register block. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_usb_regs_t* ra8_usb_fs(void)
{
  return (volatile r_usb_regs_t*)k_ra8_usb_fs0_base_addr;
}

/** @brief Get pointer to the USB HS controller register block. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_usb_regs_t* ra8_usb_hs(void)
{
  return (volatile r_usb_regs_t*)k_ra8_usb_hs0_base_addr;
}

/* =============================================================================
 * USBHS-only register offsets (HUM Ch 37 "USB 2.0 High-Speed Module")
 * =============================================================================
 */

/**
 * @enum ra8_usbhs_offset_t
 * @brief Byte offsets of USBHS-only registers not modelled in `r_usb_regs_t`.
 *
 * @details These exist only on the HS instance (HUM Ch 37) and are
 * required for embedded-PHY bring-up. They sit either inside reserved
 * gaps of the shared device-mode struct (PLLSTA at 0x06, PHYSET at
 * 0x3E) or past it (LPSTS at 0x102). Accessed via base + offset rather
 * than struct fields to keep the shared `r_usb_regs_t` layout common
 * between FS and HS instances.
 *
 * - PLLSTA at 0x06 -- HUM Ch 37 "PLL Status Register"
 * - PHYSET at 0x3E -- HUM Ch 37 "PHY Setting Register"
 * - LPSTS  at 0x102 -- HUM Ch 37 "Low Power Status Register"
 */
typedef enum : uint16_t {
  k_ra8_usbhs_off_pllsta = 0x006U, /**< PLLSTA  : PLL lock flag.      */
  k_ra8_usbhs_off_physet = 0x03EU, /**< PHYSET  : embedded-PHY setup. */
  k_ra8_usbhs_off_lpsts  = 0x102U, /**< LPSTS   : SUSPENDM gate.      */
} ra8_usbhs_offset_t;

/** @brief Get pointer to USBHS PLLSTA register (read-only). */
RA8_HW_REGISTER_ACCESS
static inline volatile uint16_t* ra8_usbhs_pllsta(void)
{
  return (volatile uint16_t*)(k_ra8_usb_hs0_base_addr + (uintptr_t)k_ra8_usbhs_off_pllsta);
}

/** @brief Get pointer to USBHS PHYSET register. */
RA8_HW_REGISTER_ACCESS
static inline volatile uint16_t* ra8_usbhs_physet(void)
{
  return (volatile uint16_t*)(k_ra8_usb_hs0_base_addr + (uintptr_t)k_ra8_usbhs_off_physet);
}

/** @brief Get pointer to USBHS LPSTS register. */
RA8_HW_REGISTER_ACCESS
static inline volatile uint16_t* ra8_usbhs_lpsts(void)
{
  return (volatile uint16_t*)(k_ra8_usb_hs0_base_addr + (uintptr_t)k_ra8_usbhs_off_lpsts);
}

/**
 * @enum ra8_usbhs_physet_bit_t
 * @brief PHYSET bit positions and masks (HUM Ch 37 "PHYSET" register).
 */
typedef enum : uint16_t {
  k_ra8_physet_dirpd     = 0x0001U, /**< b0  : PHY power-down (1=PD).   */
  k_ra8_physet_pllreset  = 0x0002U, /**< b1  : PHY PLL reset.           */
  k_ra8_physet_clksel    = 0x0030U, /**< b5-4: input clock select mask. */
  k_ra8_physet_clksel_12 = 0x0000U, /**< b5-4 = 00b: 12 MHz reference.  */
  k_ra8_physet_clksel_48 = 0x0010U, /**< b5-4 = 01b: 48 MHz reference.  */
  k_ra8_physet_clksel_20 = 0x0020U, /**< b5-4 = 10b: 20 MHz reference.  */
  k_ra8_physet_clksel_24 = 0x0030U, /**< b5-4 = 11b: 24 MHz reference.  */
  k_ra8_physet_repsel_16 = 0x0100U, /**< b9-8: 16-cycle terminator.     */
  k_ra8_physet_hseb      = 0x8000U, /**< b15 : CL-only mode.            */
} ra8_usbhs_physet_bit_t;

/**
 * @enum ra8_usbhs_lpsts_bit_t
 * @brief LPSTS bit positions (HUM Ch 37 "LPSTS" register).
 */
typedef enum : uint16_t {
  k_ra8_lpsts_suspendm = 0x4000U, /**< b14: UTMI SuspendM (1=run). */
} ra8_usbhs_lpsts_bit_t;

/**
 * @enum ra8_usbhs_pllsta_bit_t
 * @brief PLLSTA bit positions (HUM Ch 37 "PLLSTA" register).
 */
typedef enum : uint16_t {
  k_ra8_pllsta_plllock = 0x0001U, /**< b0 : PHY PLL locked flag. */
} ra8_usbhs_pllsta_bit_t;

/**
 * @enum ra8_usbhs_buswait_t
 * @brief BUSWAIT canonical value (HUM Ch 37 "BUSWAIT").
 *
 * @details FSP `r_usb_preg_access.c` uses
 * ``USB_CFG_BUSWAIT | USB_BWAIT_B11_B8_WRITE`` where the b11-b8
 * "reserved must-write" pattern is 0x0F00 and the wait field defaults
 * to 4 cycles (USB_BWAIT_4 == 0x0F04) on RA8.
 */
typedef enum : uint16_t {
  k_ra8_buswait_default = 0x0F04U, /**< 4-wait + b11-b8 reserved-write. */
} ra8_usbhs_buswait_t;

/* =============================================================================
 * SYSCFG bit positions (HUM Ch 36.2.1 / Ch 37.2.1)
 * =============================================================================
 */

/**
 * @enum ra8_usb_syscfg_bit_t
 * @brief SYSCFG bit positions.
 */
typedef enum : uint8_t {
  k_ra8_syscfg_bit_usbe  = 0U,  /**< USB module enable.            */
  k_ra8_syscfg_bit_dprpu = 4U,  /**< D+ pull-up enable (device).   */
  k_ra8_syscfg_bit_drpd  = 5U,  /**< D+/D- pull-down (host).       */
  k_ra8_syscfg_bit_dcfm  = 6U,  /**< 1 = host mode, 0 = device.    */
  k_ra8_syscfg_bit_hse   = 7U,  /**< HS enable (HS instance only). */
  k_ra8_syscfg_bit_cnen  = 8U,  /**< Single-end receiver enable.   */
  k_ra8_syscfg_bit_scke  = 10U, /**< USB clock enable.             */
} ra8_usb_syscfg_bit_t;

/* =============================================================================
 * INTENB0 / INTSTS0 bit positions (HUM Ch 36.2.10 / Ch 36.2.14)
 * =============================================================================
 */

/**
 * @enum ra8_usb_intenb0_bit_t
 * @brief INTENB0 / INTSTS0 bit positions (the layouts match).
 */
typedef enum : uint8_t {
  k_ra8_int0_bit_brdy = 8U,  /**< Buffer-ready interrupt.            */
  k_ra8_int0_bit_nrdy = 9U,  /**< Buffer-not-ready interrupt.        */
  k_ra8_int0_bit_bemp = 10U, /**< Buffer-empty interrupt.            */
  k_ra8_int0_bit_ctrt = 11U, /**< Control-transfer-stage transition. */
  k_ra8_int0_bit_dvst = 12U, /**< Device-state transition.           */
  k_ra8_int0_bit_sofr = 13U, /**< Start-of-Frame.                    */
  k_ra8_int0_bit_rsme = 14U, /**< Resume.                            */
  k_ra8_int0_bit_vbse = 15U, /**< VBUS change.                       */
} ra8_usb_intenb0_bit_t;

/**
 * @enum ra8_usb_intsts1_bit_t
 * @brief INTSTS1 / INTENB1 host-mode bit positions (HUM Ch 36.2.11/15).
 *
 * @details Host-side bus events. SACK / SIGN report the outcome of a
 * SETUP transaction started with DCPCTR.SUREQ: SACK latches when the
 * device ACKs the SETUP, SIGN when three transmission attempts fail.
 */
typedef enum : uint8_t {
  k_ra8_int1_bit_sack   = 4U,  /**< SETUP transaction ACKed.       */
  k_ra8_int1_bit_sign   = 5U,  /**< SETUP transaction failed (3x). */
  k_ra8_int1_bit_eoferr = 6U,  /**< EOF error detected.            */
  k_ra8_int1_bit_attch  = 11U, /**< Device attach detected.        */
  k_ra8_int1_bit_dtch   = 12U, /**< Device detach detected.        */
  k_ra8_int1_bit_bchg   = 14U, /**< Bus change.                    */
  k_ra8_int1_bit_ovrcr  = 15U, /**< Overcurrent input change.      */
} ra8_usb_intsts1_bit_t;

/**
 * @enum ra8_usb_intenb0_mask_t
 * @brief INTENB0 aggregate masks (HUM Ch 36.2.10 p 1980).
 */
typedef enum : uint16_t {
  /**
   * Device-mode event mask: BRDY | NRDY | BEMP | CTRT | DVST | VBSE.
   * Equals 0x9F00. SOFR and RSME are intentionally omitted -- SOFR
   * fires every 125us on HS, and RSME on USBHS stays asserted while
   * the host holds resume signalling (PHY USBR signal); together
   * they starve PendSV. NRDY drives per-pipe NAK re-arm and does
   * not cause a storm on its own.
   * Used by the watchdog thread in tz_secure_only_usb_hs to re-arm
   * INTENB0 if a USB Reset clears it.
   */
  k_ra8_int0_full_mask = 0x9F00U,
} ra8_usb_intenb0_mask_t;

/**
 * @enum ra8_usb_intsts0_mask_t
 * @brief INTSTS0 multi-bit field masks.
 */
typedef enum : uint16_t {
  k_ra8_intsts0_mask_ctsq  = 0x0007U, /**< Control transfer stage.   */
  k_ra8_intsts0_mask_valid = 0x0008U, /**< SETUP packet detect flag. */
  k_ra8_intsts0_mask_dvsq  = 0x0070U, /**< Device state.             */
  k_ra8_intsts0_mask_vbsts = 0x0080U, /**< VBUS input port level.    */
} ra8_usb_intsts0_mask_t;

/**
 * @enum ra8_usb_dvsq_t
 * @brief Device-state values reported in `INTSTS0.DVSQ[2:0]`.
 *
 * @details Read these as `(INTSTS0 & k_ra8_intsts0_mask_dvsq) ==
 * <value>`.
 */
typedef enum : uint16_t {
  k_ra8_dvsq_powered    = 0x0000U, /**< Powered (no reset yet).   */
  k_ra8_dvsq_default    = 0x0010U, /**< Default (post bus reset). */
  k_ra8_dvsq_address    = 0x0020U, /**< Address assigned.         */
  k_ra8_dvsq_configured = 0x0030U, /**< Configured.               */
  /* Suspend flag is bit 6 on BOTH controllers. USBHS additionally
   * reports VBUS presence in INTSTS0 bit 7 -- strip it before
   * comparing DVSQ values (observed live: bit 7 tracks the cable). */
  k_ra8_dvsq_suspend = 0x0040U, /**< Suspended (any sub-state). */
} ra8_usb_dvsq_t;

/**
 * @enum ra8_usb_ctsq_t
 * @brief Control-transfer-stage values reported in `INTSTS0.CTSQ[2:0]`.
 */
typedef enum : uint16_t {
  k_ra8_ctsq_idle = 0x0000U, /**< Idle / setup stage end.      */
  k_ra8_ctsq_rdds = 0x0001U, /**< Control read data stage.     */
  k_ra8_ctsq_rdss = 0x0002U, /**< Control read status stage.   */
  k_ra8_ctsq_wrds = 0x0003U, /**< Control write data stage.    */
  k_ra8_ctsq_wrss = 0x0004U, /**< Control write status stage.  */
  k_ra8_ctsq_wrnd = 0x0005U, /**< Control write nodata status. */
  k_ra8_ctsq_sqer = 0x0006U, /**< Sequence error.              */
} ra8_usb_ctsq_t;

/* =============================================================================
 * DCPCTR bit positions (HUM Ch 36.2.21)
 * =============================================================================
 */

/**
 * @enum ra8_usb_dcpctr_bit_t
 * @brief DCPCTR bit positions used by the device-mode driver.
 */
typedef enum : uint8_t {
  k_ra8_dcpctr_bit_pid_lo   = 0U,  /**< PID[0] (NAK / BUF / STALL).    */
  k_ra8_dcpctr_bit_pid_hi   = 1U,  /**< PID[1].                        */
  k_ra8_dcpctr_bit_ccpl     = 2U,  /**< Control transfer end enable.   */
  k_ra8_dcpctr_bit_pbusy    = 5U,  /**< Pipe busy.                     */
  k_ra8_dcpctr_bit_sqmon    = 6U,  /**< Sequence toggle monitor.       */
  k_ra8_dcpctr_bit_sqset    = 7U,  /**< Sequence toggle set.           */
  k_ra8_dcpctr_bit_sqclr    = 8U,  /**< Sequence toggle clear.         */
  k_ra8_dcpctr_bit_sureqclr = 11U, /**< SUREQ clear (USBHS host only). */
  k_ra8_dcpctr_bit_sureq    = 14U, /**< Send USB request (host mode).  */
  k_ra8_dcpctr_bit_bsts     = 15U, /**< Buffer status.                 */
} ra8_usb_dcpctr_bit_t;

/**
 * @enum ra8_usb_dcpcfg_bit_t
 * @brief DCPCFG bit positions used by the control-transfer engine.
 * @details In host mode DIR selects the data-stage token direction the
 * controller issues on the DCP: 0 = IN (host receives, e.g. GET_DESCRIPTOR),
 * 1 = OUT (host transmits, e.g. DFU_DNLOAD). The default DCPCFG = 0 issues
 * IN tokens, so a control-WRITE data stage MUST set DIR before sending or the
 * device sees an IN token in its write-data stage and flags CTSQ = SQER.
 */
typedef enum : uint8_t {
  k_ra8_dcpcfg_bit_dir = 4U, /**< Transfer direction (host: 0 = IN, 1 = OUT). */
} ra8_usb_dcpcfg_bit_t;

/**
 * @enum ra8_usb_pid_t
 * @brief Response PID values written into the low two bits of DCPCTR /
 * PIPECTR.
 */
typedef enum : uint16_t {
  k_ra8_pid_nak   = 0x0000U, /**< NAK response.                    */
  k_ra8_pid_buf   = 0x0001U, /**< BUF response (transmit/receive). */
  k_ra8_pid_stall = 0x0002U, /**< STALL response.                  */
  k_ra8_pid_mask  = 0x0003U, /**< PID[1:0] field mask.             */
} ra8_usb_pid_t;

/* =============================================================================
 * CFIFOSEL / DnFIFOSEL bit positions (HUM Ch 36.2.7)
 * =============================================================================
 */

/**
 * @enum ra8_usb_fifosel_field_t
 * @brief CFIFOSEL / DxFIFOSEL field masks.
 */
typedef enum : uint16_t {
  k_ra8_fifosel_curpipe = 0x000FU, /**< Current pipe select [3:0].  */
  k_ra8_fifosel_isel    = 0x0020U, /**< CFIFO direction (DCP only). */
  k_ra8_fifosel_bigend  = 0x0100U, /**< Big-endian access.          */
  k_ra8_fifosel_mbw_8   = 0x0000U, /**< 8-bit FIFO access width.    */
  k_ra8_fifosel_mbw_16  = 0x0400U, /**< 16-bit FIFO access width.   */
  k_ra8_fifosel_mbw_32  = 0x0800U, /**< 32-bit FIFO access width.   */
  k_ra8_fifosel_mbw_msk = 0x0C00U, /**< MBW field mask.             */
  k_ra8_fifosel_dreqe   = 0x1000U, /**< DREQ enable (DMA).          */
  k_ra8_fifosel_dclrm   = 0x2000U, /**< Auto FIFO clear mode.       */
  k_ra8_fifosel_rew     = 0x4000U, /**< Buffer rewind.              */
  k_ra8_fifosel_rcnt    = 0x8000U, /**< Read count mode.            */
} ra8_usb_fifosel_field_t;

/* =============================================================================
 * CFIFOCTR / DnFIFOCTR bit positions (HUM Ch 36.2.8)
 * =============================================================================
 */

/**
 * @enum ra8_usb_fifoctr_field_t
 * @brief CFIFOCTR / DxFIFOCTR field masks.
 */
typedef enum : uint16_t {
  k_ra8_fifoctr_dtln = 0x0FFFU, /**< Data length (write-bytes-rem). */
  k_ra8_fifoctr_frdy = 0x2000U, /**< FIFO ready.                    */
  k_ra8_fifoctr_bclr = 0x4000U, /**< Buffer clear.                  */
  k_ra8_fifoctr_bval = 0x8000U, /**< Buffer valid.                  */
} ra8_usb_fifoctr_field_t;

/* =============================================================================
 * USBADDR field (HUM Ch 36.2.16)
 * =============================================================================
 */

/**
 * @enum ra8_usb_addr_field_t
 * @brief USBADDR field masks.
 */
typedef enum : uint16_t {
  k_ra8_usbaddr_addr_mask = 0x007FU, /**< 7-bit USB address [6:0]. */
} ra8_usb_addr_field_t;

/* =============================================================================
 * PIPECFG fields (HUM Ch 36.2.24)
 * =============================================================================
 */

/**
 * @enum ra8_usb_pipecfg_field_t
 * @brief PIPECFG field masks / shifts used by the driver.
 */
typedef enum : uint16_t {
  k_ra8_pipecfg_epnum_mask = 0x000FU, /**< Endpoint number [3:0].   */
  k_ra8_pipecfg_dir_in     = 0x0010U, /**< 1 = IN, 0 = OUT (peri).  */
  k_ra8_pipecfg_shtnak     = 0x0080U, /**< Auto NAK on short pkt.   */
  k_ra8_pipecfg_dblb       = 0x0200U, /**< Double-buffer mode.      */
  k_ra8_pipecfg_bfre       = 0x0400U, /**< BRDY notify on read end. */
  k_ra8_pipecfg_type_bulk  = 0x4000U, /**< Type = bulk.             */
  k_ra8_pipecfg_type_intr  = 0x8000U, /**< Type = interrupt.        */
  k_ra8_pipecfg_type_iso   = 0xC000U, /**< Type = isochronous.      */
  k_ra8_pipecfg_type_mask  = 0xC000U, /**< Type field mask.         */
} ra8_usb_pipecfg_field_t;

/* =============================================================================
 * PIPEBUF fields (HUM Ch 37.2.35)
 * =============================================================================
 */

/**
 * @enum ra8_usb_pipebuf_field_t
 * @brief PIPEBUF field masks / shifts.
 *
 * Layout: BUFSIZE[14:10] | BUFNMB[7:0]. Buffer block is 64 bytes; the
 * pipe's FIFO region is `(BUFSIZE+1) * 64` bytes starting at
 * `BUFNMB * 64`. With PIPECFG.DBLB the region is split into two equal
 * halves of `(BUFSIZE+1)/2 * 64` bytes each, so a 1024-byte region
 * (BUFSIZE=15) holds 2x512 packets for HS bulk MPS=512.
 */
typedef enum : uint16_t {
  k_ra8_pipebuf_bufnmb_mask   = 0x00FFU, /**< RA8 pipebuf bufnmb mask.       */
  k_ra8_pipebuf_bufsize_mask  = 0x7C00U, /**< RA8 pipebuf bufsize mask.      */
  k_ra8_pipebuf_bufsize_shift = 10U,     /**< RA8 pipebuf bufsize shift.     */
  k_ra8_pipebuf_block_bytes   = 64U,     /**< Each buffer block is 64 bytes. */
} ra8_usb_pipebuf_field_t;

/* =============================================================================
 * PIPECTR bit positions (HUM Ch 36.2.27)
 * =============================================================================
 */

/**
 * @enum ra8_usb_pipectr_bit_t
 * @brief PIPECTR bit positions / fields used by the driver.
 */
typedef enum : uint16_t {
  k_ra8_pipectr_pid_mask = 0x0003U, /**< PID[1:0] response field. */
  k_ra8_pipectr_pbusy    = 0x0020U, /**< Pipe busy (read-only).   */
  k_ra8_pipectr_sqmon    = 0x0040U, /**< Toggle monitor.          */
  k_ra8_pipectr_sqset    = 0x0080U, /**< Toggle set.              */
  k_ra8_pipectr_sqclr    = 0x0100U, /**< Toggle clear.            */
  k_ra8_pipectr_aclrm    = 0x0200U, /**< Auto buffer clear mode.  */
  k_ra8_pipectr_atrepm   = 0x0400U, /**< Auto response mode.      */
  k_ra8_pipectr_inbufm   = 0x4000U, /**< IN buffer monitor.       */
  k_ra8_pipectr_bsts     = 0x8000U, /**< Buffer status.           */
} ra8_usb_pipectr_bit_t;

#ifdef __cplusplus
}
#endif
