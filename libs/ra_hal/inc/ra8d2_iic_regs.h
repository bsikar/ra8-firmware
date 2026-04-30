/**
 * @file ra8d2_iic_regs.h
 * @brief Legacy IIC (RIIC) controller register layout (stubbed) for the Renesas RA8D2
 *
 * @details
 * Older RA / RX silicon ships a legacy IIC ("RIIC") block whose
 * register file is byte-wide and lives below the I3C / IIC_B block now
 * carried by the RA8D2. FSP exposes that legacy block through
 * ``r_iic_master`` (``fsp/src/r_iic_master/r_iic_master.c``) and
 * ``r_iic_slave`` (``fsp/src/r_iic_slave/r_iic_slave.c``). RA8D2 does
 * not include this block -- the matching MSTPB7..B9 ``IIC0/1/2`` IDs
 * exist in the MSTP map only as legacy aliases. The driver in
 * ``ra_iic.c`` mirrors the FSP API so applications that previously
 * targeted RX or RA4/RA6 keep their compile shape.
 *
 * Layout summary (offsets relative to the channel base, FSP
 * ``R_IIC0_Type`` -- byte registers throughout):
 *
 *  - 0x00 : ICCR1   -- I2C Bus Control 1
 *  - 0x01 : ICCR2   -- I2C Bus Control 2 (ST/RS/SP)
 *  - 0x02 : ICMR1   -- I2C Bus Mode 1 (BC[2:0], BCWP, MTWP)
 *  - 0x03 : ICMR2   -- I2C Bus Mode 2
 *  - 0x04 : ICMR3   -- I2C Bus Mode 3 (ACKBT, ACKWP, RDRFS, WAIT)
 *  - 0x05 : ICFER   -- I2C Bus Function Enable
 *  - 0x06 : ICSER   -- I2C Bus Status Enable
 *  - 0x07 : ICIER   -- I2C Bus Interrupt Enable
 *  - 0x08 : ICSR1   -- I2C Bus Status 1 (slave-address match flags)
 *  - 0x09 : ICSR2   -- I2C Bus Status 2 (TDRE, TEND, RDRF, NACKF, ...)
 *  - 0x0A..0x0F : SAR0/SAR1/SAR2 -- 7-bit slave addresses (low byte)
 *  - 0x10..0x11 : ICBRL / ICBRH -- baud-rate registers
 *  - 0x12 : ICDRT  -- transmit data register
 *  - 0x13 : ICDRR  -- receive data register
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
 * @enum ra_iic_legacy_limits_t
 * @brief Channel and slave-address-slot counts.
 */
typedef enum : uint8_t {
  k_ra_iic_legacy_channel_count = 3U, /**< Three legacy IIC channels matching MSTP IIC0..2. */
  k_ra_iic_legacy_sar_count     = 3U, /**< SAR0..SAR2.                                       */
} ra_iic_legacy_limits_t;

/**
 * @enum ra_iic_legacy_addr_t
 * @brief Channel base addresses (placeholder window inside the
 *        simulator peripheral region).
 */
typedef enum : uintptr_t {
  k_ra_iic_legacy0_base_addr = 0x40720000UL, /**< Placeholder; legacy IIC absent on RA8D2. */
  k_ra_iic_legacy1_base_addr = 0x40720100UL, /**< Channel 1 stride = 0x100.                */
  k_ra_iic_legacy2_base_addr = 0x40720200UL, /**< Channel 2.                               */
} ra_iic_legacy_addr_t;

/**
 * @struct r_iic_legacy_t
 * @brief Memory-mapped layout of the legacy IIC controller.
 */
typedef struct {
  volatile uint8_t ICCR1;                           /**< 0x00. */
  volatile uint8_t ICCR2;                           /**< 0x01. */
  volatile uint8_t ICMR1;                           /**< 0x02. */
  volatile uint8_t ICMR2;                           /**< 0x03. */
  volatile uint8_t ICMR3;                           /**< 0x04. */
  volatile uint8_t ICFER;                           /**< 0x05. */
  volatile uint8_t ICSER;                           /**< 0x06. */
  volatile uint8_t ICIER;                           /**< 0x07. */
  volatile uint8_t ICSR1;                           /**< 0x08. */
  volatile uint8_t ICSR2;                           /**< 0x09. */
  volatile uint8_t SARL[k_ra_iic_legacy_sar_count]; /**< 0x0A SAR0L/SAR1L/SAR2L. */
  volatile uint8_t SARU[k_ra_iic_legacy_sar_count]; /**< 0x0D SAR0U/SAR1U/SAR2U. */
  volatile uint8_t ICBRL;                           /**< 0x10. */
  volatile uint8_t ICBRH;                           /**< 0x11. */
  volatile uint8_t ICDRT;                           /**< 0x12. */
  volatile uint8_t ICDRR;                           /**< 0x13. */
} r_iic_legacy_t;

/**
 * @enum ra_iic_legacy_block_size_t
 * @brief Total byte size of ``r_iic_legacy_t`` (used by ``static_assert`` below).
 */
typedef enum : uint8_t {
  k_ra_iic_legacy_block_bytes = 0x14U, /**< 20 bytes per channel. */
} ra_iic_legacy_block_size_t;

static_assert(sizeof(r_iic_legacy_t) == (size_t)k_ra_iic_legacy_block_bytes,
              "r_iic_legacy_t layout drifted from documented size");

/**
 * @brief Accessor for a legacy-IIC channel register block.
 *
 * @param[in] channel Channel index (must be < ``k_ra_iic_legacy_channel_count``).
 * @return Pointer to the memory-mapped register block, or NULL for an out-of-range channel.
 *
 * @since 0.1.0
 */
static inline volatile r_iic_legacy_t* ra_iic_legacy_regs(uint8_t channel)
{
  switch (channel) {
    case 0:
      return (volatile r_iic_legacy_t*)k_ra_iic_legacy0_base_addr;
    case 1:
      return (volatile r_iic_legacy_t*)k_ra_iic_legacy1_base_addr;
    case 2:
      return (volatile r_iic_legacy_t*)k_ra_iic_legacy2_base_addr;
    default:
      return (volatile r_iic_legacy_t*)0;
  }
}

/**
 * @enum ra_iic_legacy_iccr2_t
 * @brief ICCR2 bit positions (matches FSP r_iic_master).
 */
typedef enum : uint8_t {
  k_ra_iic_legacy_iccr2_st_pos   = 1U, /**< Start condition.            */
  k_ra_iic_legacy_iccr2_rs_pos   = 2U, /**< Restart condition.          */
  k_ra_iic_legacy_iccr2_sp_pos   = 3U, /**< Stop condition.             */
  k_ra_iic_legacy_iccr2_trs_pos  = 5U, /**< Transmit / Receive select.  */
  k_ra_iic_legacy_iccr2_mst_pos  = 6U, /**< Master mode.                */
  k_ra_iic_legacy_iccr2_bbsy_pos = 7U, /**< Bus busy.                  */
} ra_iic_legacy_iccr2_t;

/**
 * @enum ra_iic_legacy_icsr2_t
 * @brief ICSR2 bit positions (status flags).
 */
typedef enum : uint8_t {
  k_ra_iic_legacy_icsr2_aas   = 0x01U, /**< Address-As-Slave match.   */
  k_ra_iic_legacy_icsr2_stop  = 0x08U, /**< Stop detected.            */
  k_ra_iic_legacy_icsr2_nackf = 0x10U, /**< NACK received.            */
  k_ra_iic_legacy_icsr2_rdrf  = 0x20U, /**< Receive data full.        */
  k_ra_iic_legacy_icsr2_tend  = 0x40U, /**< Transmit end.             */
  k_ra_iic_legacy_icsr2_tdre  = 0x80U, /**< Transmit data reg empty.  */
} ra_iic_legacy_icsr2_t;

/**
 * @enum ra_iic_legacy_iccr1_t
 * @brief ICCR1 bit positions.
 */
typedef enum : uint8_t {
  k_ra_iic_legacy_iccr1_iicrst = 0x40U, /**< Internal reset.        */
  k_ra_iic_legacy_iccr1_ice    = 0x80U, /**< Module enable.         */
} ra_iic_legacy_iccr1_t;

#ifdef __cplusplus
}
#endif
