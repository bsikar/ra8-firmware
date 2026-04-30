/**
 * @file ra8d2_cec_regs.h
 * @brief HDMI Consumer Electronics Control (CEC) register layout
 *
 * @details
 * The CEC peripheral is the single-wire HDMI control bus
 * (compliant with HDMI CEC version 1.4 / 2.0). It transports
 * 10-bit framed bytes (start bit, eight data bits, EOM bit, ACK
 * bit) between consumer-electronics devices over the HDMI cable's
 * pin 13. The register layout below is a host-testable mirror of
 * the FSP `R_CEC_Type` block found on RA6E2 / RA4E2 silicon
 * (cmsis device header, base 0x400AC000, size 70 bytes).
 *
 * @warning The Renesas RA8D2 silicon does not carry a CEC block;
 *          consumer-electronics control on RA8D2 is expected to be
 *          done through the GLCDC/MIPI HDMI sideband or by the
 *          off-chip HDMI transmitter. This file ships a placeholder
 *          register layout so portable code keeps compiling and so
 *          unit tests have a definite struct to point at. The
 *          base address `k_ra_cec_base_addr` is reserved on the
 *          RA8D2 memory map and any access goes to the simulator's
 *          shadow RAM only -- the real silicon will fault.
 *
 * Reference: FSP CMSIS device header `R7FA6E2BB.h` lines 16475..16819
 *            (`R_CEC_Type`, base 0x400AC000).
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
 * @enum ra_cec_addr_t
 * @brief Base address of the CEC peripheral (placeholder on RA8D2).
 */
typedef enum : uintptr_t {
  k_ra_cec_base_addr = 0x400AC000UL, /**< Reserved on RA8D2. */
} ra_cec_addr_t;

/**
 * @enum ra_cec_buf_t
 * @brief CEC transmit / receive buffer dimensions.
 */
typedef enum : uint8_t {
  k_ra_cec_max_payload = 14U, /**< CEC payload bytes per message.   */
} ra_cec_buf_t;

/**
 * @struct r_cec_regs_t
 * @brief CEC register block (mirrors FSP `R_CEC_Type`).
 */
typedef struct {
  volatile uint16_t CADR;    /**< +0x00 Local Address Setting.       */
  volatile uint8_t  CECCTL1; /**< +0x02 Control 1.                   */
  volatile uint8_t  _r0;
  volatile uint16_t STATB;  /**< +0x04 TX Start Bit Width.          */
  volatile uint16_t STATL;  /**< +0x06 TX Start Bit Low Width.      */
  volatile uint16_t LGC0L;  /**< +0x08 TX Logical 0 Low Width.      */
  volatile uint16_t LGC1L;  /**< +0x0A TX Logical 1 Low Width.      */
  volatile uint16_t DATB;   /**< +0x0C TX Data Bit Width.           */
  volatile uint16_t NOMT;   /**< +0x0E RX Sample Time.              */
  volatile uint16_t STATLL; /**< +0x10 RX Start Bit Min Low.        */
  volatile uint16_t STATLH; /**< +0x12 RX Start Bit Max Low.        */
  volatile uint16_t STATBL; /**< +0x14 RX Start Bit Min Width.      */
  volatile uint16_t STATBH; /**< +0x16 RX Start Bit Max Width.      */
  volatile uint16_t LGC0LL; /**< +0x18 RX Logical 0 Min Low.        */
  volatile uint16_t LGC0LH; /**< +0x1A RX Logical 0 Max Low.        */
  volatile uint16_t LGC1LL; /**< +0x1C RX Logical 1 Min Low.        */
  volatile uint16_t LGC1LH; /**< +0x1E RX Logical 1 Max Low.        */
  volatile uint16_t DATBL;  /**< +0x20 RX Data Bit Min Width.       */
  volatile uint16_t DATBH;  /**< +0x22 RX Data Bit Max Width.       */
  volatile uint16_t NOMP;   /**< +0x24 Data Bit Reference Width.    */
  volatile uint16_t _r1;
  volatile uint8_t  CECEXMD; /**< +0x28 Extension Mode.              */
  volatile uint8_t  _r2;
  volatile uint8_t  CECEXMON; /**< +0x2A Extension Monitor.          */
  volatile uint8_t  _r3;
  /* NOLINTNEXTLINE(readability-magic-numbers) -- 10 = (0x40 - 0x2C) / 2, structural padding. */
  volatile uint16_t _r4[10];
  volatile uint8_t  CTXD;    /**< +0x40 TX Buffer.                   */
  volatile uint8_t  CRXD;    /**< +0x41 RX Buffer.                   */
  volatile uint8_t  CECES;   /**< +0x42 Communication Error Status.  */
  volatile uint8_t  CECS;    /**< +0x43 Communication Status.        */
  volatile uint8_t  CECFC;   /**< +0x44 Comm Error Flag Clear.       */
  volatile uint8_t  CECCTL0; /**< +0x45 Control 0.                   */
} r_cec_regs_t;

/** @brief Get pointer to the CEC block. */
static inline volatile r_cec_regs_t* ra_cec_regs(void)
{
  return (volatile r_cec_regs_t*)k_ra_cec_base_addr;
}

#ifdef __cplusplus
}
#endif
