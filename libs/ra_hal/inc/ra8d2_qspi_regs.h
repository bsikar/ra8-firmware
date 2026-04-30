/**
 * @file ra8d2_qspi_regs.h
 * @brief Legacy QSPI controller register layout (stubbed) for the Renesas RA8D2
 *
 * @details
 * The RA8D2 group ships an OSPI / OSPI_B block (covered by
 * ``ra8d2_ospi_regs.h``) instead of the legacy QSPI peripheral that
 * older RA / RX silicon exposed. FSP keeps a separate ``r_qspi``
 * driver targeted at the legacy block (see ``fsp/src/r_qspi/r_qspi.c``)
 * and applications that previously targeted the legacy block expect
 * the same register surface.
 *
 * Layout summary (offsets relative to the channel base, FSP
 * ``R_QSPI_Type`` -- 32-bit registers throughout):
 *
 *  - 0x00 : SFMSMD  -- SPI Mode Control (mode, CPOL, CPHA, ...)
 *  - 0x04 : SFMSSC  -- SPI Slave-Select Control
 *  - 0x08 : SFMSKC  -- SPI Clock Control (divisor, brdv)
 *  - 0x0C : SFMSST  -- SPI Status (TXFULL/RXFULL/BUSY/...)
 *  - 0x10 : SFMCOM  -- SPI Direct Communication FIFO port
 *  - 0x14 : SFMCMD  -- SPI Command (HW-controlled XIP entry)
 *  - 0x18 : SFMCST  -- SPI Command Status
 *  - 0x20 : SFMSIC  -- SPI Instruction Code (XIP read instruction)
 *  - 0x24 : SFMSAC  -- SPI Address Code (XIP address bytes)
 *  - 0x28 : SFMSDC  -- SPI Dummy Cycles
 *  - 0x30 : SFMSPC  -- SPI Protocol Cfg (SPI/Dual/Quad mode)
 *  - 0x34 : SFMPMD  -- SPI Port Mode (drive strength)
 *  - 0x80 : SFMCNT1 -- Counter / auto-calibration
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

/**
 * @enum ra_qspi_limits_t
 * @brief Channel count and limits.
 */
typedef enum : uint8_t {
  k_ra_qspi_channel_count = 1U,  /**< RA8D2 stub: one legacy QSPI channel. */
  k_ra_qspi_pad_words     = 18U, /**< Reserved hole 0x38..0x7F = 18 words. */
} ra_qspi_limits_t;

/**
 * @enum ra_qspi_addr_t
 * @brief Base address of the legacy QSPI controller (placeholder).
 */
typedef enum : uintptr_t {
  k_ra_qspi0_base_addr = 0x40730000UL, /**< Placeholder; legacy QSPI absent on RA8D2. */
} ra_qspi_addr_t;

/**
 * @enum ra_qspi_xip_addr_t
 * @brief Base address of the memory-mapped (XIP) read window.
 */
typedef enum : uintptr_t {
  k_ra_qspi_xip_base_addr = 0x40740000UL, /**< Placeholder XIP window. */
  k_ra_qspi_xip_size      = 0x00010000UL, /**< 64 KiB XIP window.      */
} ra_qspi_xip_addr_t;

/**
 * @struct r_qspi_t
 * @brief Memory-mapped layout of the legacy QSPI controller.
 */
typedef struct {
  volatile uint32_t SFMSMD;                         /**< 0x00 SPI Mode Control.                */
  volatile uint32_t SFMSSC;                         /**< 0x04 Slave-Select Control.            */
  volatile uint32_t SFMSKC;                         /**< 0x08 Clock Control.                   */
  volatile uint32_t SFMSST;                         /**< 0x0C Status.                          */
  volatile uint32_t SFMCOM;                         /**< 0x10 Direct Communication FIFO.       */
  volatile uint32_t SFMCMD;                         /**< 0x14 Command.                         */
  volatile uint32_t SFMCST;                         /**< 0x18 Command Status.                  */
  volatile uint32_t RESERVED0;                      /**< 0x1C reserved.                        */
  volatile uint32_t SFMSIC;                         /**< 0x20 XIP Instruction Code.            */
  volatile uint32_t SFMSAC;                         /**< 0x24 XIP Address Code.                */
  volatile uint32_t SFMSDC;                         /**< 0x28 Dummy Cycles.                    */
  volatile uint32_t RESERVED1;                      /**< 0x2C reserved.                        */
  volatile uint32_t SFMSPC;                         /**< 0x30 Protocol Configuration.          */
  volatile uint32_t SFMPMD;                         /**< 0x34 Port Mode.                       */
  volatile uint32_t RESERVED2[k_ra_qspi_pad_words]; /**< 0x38..0x7F reserved.       */
  volatile uint32_t SFMCNT1;                        /**< 0x80 Auto-calibration counter.        */
} r_qspi_t;

/**
 * @enum ra_qspi_block_size_t
 * @brief Byte size of ``r_qspi_t`` (used by ``static_assert`` below).
 */
typedef enum : uint16_t {
  k_ra_qspi_block_bytes = 0x84U, /**< 132 bytes. */
} ra_qspi_block_size_t;

static_assert(sizeof(r_qspi_t) == (size_t)k_ra_qspi_block_bytes,
              "r_qspi_t layout drifted from documented size");

/**
 * @brief Accessor for the legacy-QSPI register block.
 *
 * @param[in] channel Channel index (must be 0).
 * @return Pointer to the memory-mapped register block, or NULL when out of range.
 *
 * @since 0.1.0
 */
static inline volatile r_qspi_t* ra_qspi_regs(uint8_t channel)
{
  if (channel >= k_ra_qspi_channel_count) {
    return (volatile r_qspi_t*)0;
  }
  return (volatile r_qspi_t*)k_ra_qspi0_base_addr;
}

/**
 * @brief Accessor for the memory-mapped XIP window.
 *
 * @return Pointer to the XIP read window.
 *
 * @since 0.1.0
 */
static inline volatile uint8_t* ra_qspi_xip(void)
{
  return (volatile uint8_t*)k_ra_qspi_xip_base_addr;
}

/**
 * @enum ra_qspi_sfmsst_t
 * @brief SFMSST status-register bit positions.
 */
typedef enum : uint32_t {
  k_ra_qspi_sfmsst_busy   = (uint32_t)(1U << 31U), /**< Channel busy.           */
  k_ra_qspi_sfmsst_txfull = (uint32_t)(1U << 1U),  /**< TX FIFO full.           */
  k_ra_qspi_sfmsst_rxrdy  = (uint32_t)(1U << 0U),  /**< RX FIFO has data.       */
} ra_qspi_sfmsst_t;

/**
 * @enum ra_qspi_sfmcmd_t
 * @brief SFMCMD command-register bit positions.
 */
typedef enum : uint32_t {
  k_ra_qspi_sfmcmd_xip_enter = 0x00000001UL, /**< Trigger XIP entry.           */
  k_ra_qspi_sfmcmd_xip_exit  = 0x00000000UL, /**< Default value -- XIP exit.   */
} ra_qspi_sfmcmd_t;

#ifdef __cplusplus
}
#endif
