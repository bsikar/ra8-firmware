/**
 * @file ra8d2_usb_regs.h
 * @brief USB Full-Speed + High-Speed controller layout for the Renesas RA8D2
 *
 * @details
 * RA8D2 has two USB controllers:
 *
 *  - **USB FS** at `0x40250000` -- 12 Mbps, standalone PHY.
 *  - **USB HS** at `0x40351000` -- 480 Mbps, embedded HS PHY.
 *
 * Both use the Renesas "USB2_B" IP (common across RA / RZ / RX).
 * The register layouts are identical between the two instances
 * except for per-endpoint PIPE count and a few HS-only fields; we
 * model the common subset needed for device-mode bring-up.
 *
 * Device and host mode share the same register block; the mode bit
 * is in SYSCFG.DCFM.
 *
 * ## Register map (partial)
 *
 * | Offset | Name      | Width | Purpose                              |
 * |-------:|-----------|------:|--------------------------------------|
 * | 0x000  | SYSCFG    | 16    | System Configuration                  |
 * | 0x002  | BUSWAIT   | 16    | CPU bus wait                          |
 * | 0x004  | SYSSTS0   | 16    | System Status 0                       |
 * | 0x008  | DVSTCTR0  | 16    | Device State Control 0                |
 * | 0x014  | CFIFO     | 16    | Control FIFO port                     |
 * | 0x018  | D0FIFO    | 16    | DMA FIFO 0                            |
 * | 0x01C  | D1FIFO    | 16    | DMA FIFO 1                            |
 * | 0x020  | CFIFOSEL  | 16    | Control FIFO select                   |
 * | 0x028  | D0FIFOSEL | 16    | DMA FIFO 0 select                     |
 * | 0x030  | INTENB0   | 16    | Interrupt Enable 0                    |
 * | 0x032  | INTENB1   | 16    | Interrupt Enable 1                    |
 * | 0x040  | INTSTS0   | 16    | Interrupt Status 0                    |
 * | 0x042  | INTSTS1   | 16    | Interrupt Status 1                    |
 * | 0x054  | DCPCFG    | 16    | Default Control Pipe Config           |
 * | 0x058  | DCPCTR    | 16    | Default Control Pipe Control          |
 * | 0x064  | PIPESEL   | 16    | Pipe Window select                    |
 * | 0x068  | PIPECFG   | 16    | Selected pipe Config                  |
 * | 0x070  | PIPEMAXP  | 16    | Selected pipe Max Packet              |
 * | 0x0C0  | PIPExCTR  | 16    | Pipe x Control (x = 1..9 for FS)      |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra_usb_fs0_base_addr = 0x40250000UL,
  k_ra_usb_hs0_base_addr = 0x40351000UL,
} ra_usb_addr_t;

typedef enum : uint8_t {
  k_ra_usb_pipe_count = 10U, /**< DCP (pipe 0) + PIPE1..PIPE9.       */
  k_ra_usb_pad_5      = 5U,  /**< Reserved-field length (5 halfwords). */
} ra_usb_limits_t;

/**
 * @struct r_usb_regs_t
 * @brief USB FS / HS register window (common subset).
 */
typedef struct {
  volatile uint16_t SYSCFG;  /**< +0x000 System Configuration.        */
  volatile uint16_t BUSWAIT; /**< +0x002 CPU bus wait.                */
  volatile uint16_t SYSSTS0; /**< +0x004 System Status 0.             */
  volatile uint16_t _r0;
  volatile uint16_t DVSTCTR0; /**< +0x008 Device State Control.        */
  volatile uint16_t _r1[k_ra_usb_pad_5];
  volatile uint16_t CFIFO; /**< +0x014 Control FIFO port.           */
  volatile uint16_t _r2;
  volatile uint16_t D0FIFO; /**< +0x018 DMA FIFO 0.                  */
  volatile uint16_t _r3;
  volatile uint16_t D1FIFO; /**< +0x01C DMA FIFO 1.                  */
  volatile uint16_t _r4;
  volatile uint16_t CFIFOSEL; /**< +0x020 Control FIFO select.         */
  volatile uint16_t CFIFOCTR; /**< +0x022 Control FIFO control.        */
  volatile uint16_t _r5[2];
  volatile uint16_t D0FIFOSEL; /**< +0x028 DMA FIFO 0 select.           */
  volatile uint16_t D0FIFOCTR; /**< +0x02A DMA FIFO 0 control.          */
  volatile uint16_t D1FIFOSEL; /**< +0x02C DMA FIFO 1 select.           */
  volatile uint16_t D1FIFOCTR; /**< +0x02E DMA FIFO 1 control.          */
  volatile uint16_t INTENB0;   /**< +0x030 Interrupt Enable 0.          */
  volatile uint16_t INTENB1;   /**< +0x032 Interrupt Enable 1.          */
  volatile uint16_t _r6[6];
  volatile uint16_t INTSTS0; /**< +0x040 Interrupt Status 0.          */
  volatile uint16_t INTSTS1; /**< +0x042 Interrupt Status 1.          */
  volatile uint16_t _r7[8];
  volatile uint16_t DCPCFG;  /**< +0x054 Default Control Pipe Cfg.    */
  volatile uint16_t DCPMAXP; /**< +0x056 DCP Max Packet.              */
  volatile uint16_t DCPCTR;  /**< +0x058 DCP Control.                 */
  volatile uint16_t _r8[k_ra_usb_pad_5];
  volatile uint16_t PIPESEL; /**< +0x064 Pipe window select.          */
  volatile uint16_t _r9;
  volatile uint16_t PIPECFG; /**< +0x068 Selected pipe config.        */
  volatile uint16_t _ra;
  volatile uint16_t PIPEBUF; /**< +0x06C Selected pipe buffer.        */
  volatile uint16_t _rb;
  volatile uint16_t PIPEMAXP; /**< +0x070 Selected pipe max packet.    */
  volatile uint16_t PIPEPERI; /**< +0x072 Selected pipe period.        */
} r_usb_regs_t;

/** @brief Get pointer to the USB FS controller. */
static inline volatile r_usb_regs_t* ra_usb_fs(void)
{
  return (volatile r_usb_regs_t*)k_ra_usb_fs0_base_addr;
}

/** @brief Get pointer to the USB HS controller. */
static inline volatile r_usb_regs_t* ra_usb_hs(void)
{
  return (volatile r_usb_regs_t*)k_ra_usb_hs0_base_addr;
}

/**
 * @enum ra_usb_syscfg_bit_t
 * @brief SYSCFG bit positions.
 */
typedef enum : uint8_t {
  k_ra_syscfg_bit_usbe  = 0U,  /**< USB module enable.             */
  k_ra_syscfg_bit_dprpu = 4U,  /**< D+ pull-up enable (device).    */
  k_ra_syscfg_bit_drpd  = 5U,  /**< D+/D- pull-down (host).        */
  k_ra_syscfg_bit_dcfm  = 6U,  /**< 1 = host mode, 0 = device.     */
  k_ra_syscfg_bit_hse   = 7U,  /**< HS enable (HS instance only).  */
  k_ra_syscfg_bit_cnen  = 8U,  /**< Single-end receiver enable.    */
  k_ra_syscfg_bit_scke  = 10U, /**< USB clock enable.              */
} ra_usb_syscfg_bit_t;

#ifdef __cplusplus
}
#endif
