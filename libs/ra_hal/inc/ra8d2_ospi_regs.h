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
 * | Offset | Name       | Width | Purpose                             |
 * |-------:|------------|------:|-------------------------------------|
 * | 0x00   | XSPI_WRAPCFG | 32  | Wrapper configuration                |
 * | 0x04   | XSPI_COMCFG  | 32  | Command configuration                |
 * | 0x08   | XSPI_LIOCFG  | 32  | Link-layer I/O config                |
 * | 0x10   | XSPI_CMDCFG0 | 32  | Direct command config 0              |
 * | 0x14   | XSPI_CMDCFG1 | 32  | Direct command config 1              |
 * | 0x18   | XSPI_CMDCFG2 | 32  | Direct command config 2              |
 * | 0x20   | XSPI_CMDBUF0_0 | 32 | Direct command buffer (16 bytes)     |
 * | ...    | ...          | ... | ...                                   |
 * | 0x40   | XSPI_LIOSTS  | 32  | Link-layer I/O status                 |
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
  volatile uint32_t CMDBUF[4]; /**< +0x020..+0x02C 16-byte cmd buf. */
  volatile uint32_t _r2[4];
  volatile uint32_t LIOSTS; /**< +0x040 Link I/O status.         */
  volatile uint32_t INT;    /**< +0x044 Interrupt status.        */
  volatile uint32_t INTE;   /**< +0x048 Interrupt enable.        */
  volatile uint32_t INTC;   /**< +0x04C Interrupt clear.         */
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

#ifdef __cplusplus
}
#endif
