/**
 * @file ra8d2_ospi_regs.h
 * @brief xSPI / Octo-SPI controller register layout for the Renesas RA8D2
 *
 * @details
 * The RA8D2 xSPI controller supports OPI (Octo Peripheral Interface)
 * + Octa-DDR + HyperBus flash chips. Two instances:
 *
 *  - `XSPI0` at `0x40268000` -- wired to the 64 MiB xSPI flash on
 *    the EK-RA8D2.
 *  - `XSPI1` at `0x40268400` -- second bank (unused on the EK).
 *
 * The DOTF (Decryption on the Fly) block at `0x40268800` sits next
 * to them for encrypted XIP execution and is accessed separately.
 *
 * ## Register map (partial, per instance)
 *
 * | Offset | Name            | Width | Purpose                         |
 * |-------:|-----------------|------:|---------------------------------|
 * | 0x00   | XSPI_WRAPCFG    | 32    | Wrapper configuration            |
 * | 0x04   | XSPI_COMCFG     | 32    | Command configuration            |
 * | 0x08   | XSPI_LIOCFG     | 32    | Link-layer I/O config            |
 * | 0x10   | XSPI_CMDCFG0    | 32    | Direct command config 0          |
 * | 0x14   | XSPI_CMDCFG1    | 32    | Direct command config 1          |
 * | 0x18   | XSPI_CMDCFG2    | 32    | Direct command config 2          |
 * | 0x20   | XSPI_CMDBUF0_0  | 32    | Direct command buffer (16 bytes) |
 * | 0x40   | XSPI_LIOSTS     | 32    | Link-layer I/O status            |
 * | 0x44   | XSPI_INT        | 32    | Interrupt status                 |
 * | 0x48   | XSPI_INTE       | 32    | Interrupt enable                 |
 * | 0x4C   | XSPI_INTC       | 32    | Interrupt clear                  |
 * | 0x50   | XSPI_COMSTT     | 32    | Command status (TRBUSY, etc.)    |
 * | 0x54   | XSPI_RDBUF0_0   | 32    | Read-data buffer (16 bytes)      |
 * | 0x70   | XSPI_BMCFG      | 32    | Buffer manager config            |
 *
 * Full field layouts land with the first xSPI driver -- bring-up
 * uses direct command mode, XIP mapping, and DMA read-write.
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
  k_ra_xspi0_base_addr = 0x40268000UL,
  k_ra_xspi1_base_addr = 0x40268400UL,
  k_ra_dotf_base_addr  = 0x40268800UL,
  k_ra_xspi_stride     = 0x400U,
} ra_xspi_addr_t;

typedef enum : uint8_t {
  k_ra_xspi_instance_count = 2U,
} ra_xspi_limits_t;

/**
 * @enum ra_xspi_xfer_limits_t
 * @brief Limits for direct-command transfers.
 */
typedef enum : uint32_t {
  k_ra_xspi_max_xfer   = 4096U, /**< Maximum bytes per flash read/program call.  */
  k_ra_xspi_page_len   = 256U,  /**< Page size of standard NOR flash parts.      */
  k_ra_xspi_sector_len = 4096U, /**< Sector size for opcode 0x20 / 0x21.         */
} ra_xspi_xfer_limits_t;

/**
 * @struct r_xspi_regs_t
 * @brief xSPI per-instance register window (partial).
 */
typedef struct {
  volatile uint32_t WRAPCFG; /**< +0x000 Wrapper configuration.   */
  volatile uint32_t COMCFG;  /**< +0x004 Command configuration.   */
  volatile uint32_t LIOCFG;  /**< +0x008 Link I/O configuration.  */
  volatile uint32_t _r0;
  volatile uint32_t CMDCFG0; /**< +0x010 Direct command cfg 0.    */
  volatile uint32_t CMDCFG1; /**< +0x014 Direct command cfg 1.    */
  volatile uint32_t CMDCFG2; /**< +0x018 Direct command cfg 2.    */
  volatile uint32_t _r1;
  volatile uint32_t CMDBUF[4]; /**< +0x020..+0x02C Command buffer.  */
  volatile uint32_t _r2[4];
  volatile uint32_t LIOSTS;   /**< +0x040 Link I/O status.         */
  volatile uint32_t INT;      /**< +0x044 Interrupt status.        */
  volatile uint32_t INTE;     /**< +0x048 Interrupt enable.        */
  volatile uint32_t INTC;     /**< +0x04C Interrupt clear.         */
  volatile uint32_t COMSTT;   /**< +0x050 Command status.          */
  volatile uint32_t RDBUF[4]; /**< +0x054..+0x060 Read-data buffer. */
  volatile uint32_t _r3[3];
  volatile uint32_t BMCFG; /**< +0x070 Buffer manager config.   */
} r_xspi_regs_t;

/** @brief Get pointer to xSPI instance N (0..1). */
static inline volatile r_xspi_regs_t* ra_xspi(uint8_t instance)
{
  if (instance >= (uint8_t)k_ra_xspi_instance_count) {
    return nullptr;
  }
  return (volatile r_xspi_regs_t*)(k_ra_xspi0_base_addr +
                                   ((uintptr_t)instance * (uintptr_t)k_ra_xspi_stride));
}

/**
 * @enum ra_xspi_lio_mode_t
 * @brief Values written to `LIOCFG.CALEN` to set link-layer IO width.
 */
typedef enum : uint8_t {
  k_ra_xspi_lio_1s1s1s = 0U, /**< Single-bit command/address/data (SPI). */
  k_ra_xspi_lio_1s2s2s = 1U, /**< Dual IO.                                */
  k_ra_xspi_lio_1s4s4s = 2U, /**< Quad IO.                                */
  k_ra_xspi_lio_1s8s8s = 3U, /**< Octal IO.                               */
  k_ra_xspi_lio_8d8d8d = 4U, /**< Octal DDR (OPI).                        */
} ra_xspi_lio_mode_t;

/**
 * @enum ra_xspi_comstt_bit_t
 * @brief Bit positions in the COMSTT (command-status) register.
 */
typedef enum : uint8_t {
  k_ra_xspi_comstt_bit_trbusy = 0U, /**< 1 = a direct command is in flight. */
  k_ra_xspi_comstt_bit_done   = 1U, /**< 1 = last command finished.         */
} ra_xspi_comstt_bit_t;

/**
 * @enum ra_xspi_cmdcfg0_bit_t
 * @brief Bit positions in CMDCFG0 (direct command config 0).
 */
typedef enum : uint8_t {
  k_ra_xspi_cmdcfg0_bit_ffmt = 0U,  /**< Frame format field shift.  */
  k_ra_xspi_cmdcfg0_bit_addr = 4U,  /**< Address-size field shift.  */
  k_ra_xspi_cmdcfg0_bit_cmd  = 8U,  /**< Command-size field shift.  */
  k_ra_xspi_cmdcfg0_bit_xfer = 16U, /**< Transfer-length field shift. */
} ra_xspi_cmdcfg0_bit_t;

/**
 * @enum ra_xspi_intc_bit_t
 * @brief Bit values used when clearing interrupts.
 */
typedef enum : uint32_t {
  k_ra_xspi_intc_clear_all = 0xFFFFFFFFUL, /**< Clear every INT flag.        */
  k_ra_xspi_intc_cmdcmp    = 1UL << 0U,    /**< Command-complete clear bit.  */
} ra_xspi_intc_bit_t;

#ifdef __cplusplus
}
#endif
