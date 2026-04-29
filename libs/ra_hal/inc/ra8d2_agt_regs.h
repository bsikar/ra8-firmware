/**
 * @file ra8d2_agt_regs.h
 * @brief Asynchronous General-Purpose Timer (AGT) register layout for RA8D2
 *
 * @details
 * The AGT is a 16-bit down-counter typically used as a tick timer or
 * a low-power wakeup source. RA8D2 has AGTX0..AGTX9 at `0x40221000`
 * with a `0x100` stride per instance. Each AGT can clock from PCLKB,
 * LOCO, subclock, or an underflow of another AGT. RA8D2 also exposes
 * each channel as a 32-bit AGTW alias starting at offset 0x00; the
 * 16-bit and 32-bit views overlay the same channel registers and the
 * 8-bit CTRL block lives at offset 0x08 (16-bit view) / 0x0C (32-bit
 * view). This driver targets the 16-bit view and matches the
 * authoritative FSP `R_AGTX0_AGT16_Type` layout exactly. HUM Ch 24
 * "Low Power Asynchronous General Purpose Timer (AGT)" p 1164-1186.
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
  k_ra_agt0_base_addr = 0x40221000UL,
} ra_agt_addr_t;

typedef enum : uint16_t {
  k_ra_agt_channel_count  = 10U,
  k_ra_agt_channel_stride = 0x100U,
} ra_agt_limits_t;

/**
 * @brief AGTCR (AGT Control Register) bit positions.
 * @details HUM Ch 24.2.6 "AGTCR : AGT Control Register" p 1175.
 */
typedef enum : uint8_t {
  k_ra_agt_agtcr_tstart_pos = 0U, /**< Start request (RW).        */
  k_ra_agt_agtcr_tcstf_pos  = 1U, /**< Count status flag (RO).    */
  k_ra_agt_agtcr_tstop_pos  = 2U, /**< Forced stop (W).           */
  k_ra_agt_agtcr_tedgf_pos  = 4U, /**< Active edge flag (RW1C).   */
  k_ra_agt_agtcr_tundf_pos  = 5U, /**< Underflow flag (RW1C).     */
  k_ra_agt_agtcr_tcmaf_pos  = 6U, /**< Compare-match A (RW1C).    */
  k_ra_agt_agtcr_tcmbf_pos  = 7U, /**< Compare-match B (RW1C).    */
} ra_agt_agtcr_bits_t;

/**
 * @brief AGTCR bit masks.
 * @details Mirror of `ra_agt_agtcr_bits_t` for ALU use.
 */
typedef enum : uint8_t {
  k_ra_agt_agtcr_tstart_msk = 0x01U,
  k_ra_agt_agtcr_tcstf_msk  = 0x02U,
  k_ra_agt_agtcr_tstop_msk  = 0x04U,
  k_ra_agt_agtcr_status_msk = 0xF0U, /**< TEDGF|TUNDF|TCMAF|TCMBF */
} ra_agt_agtcr_masks_t;

/**
 * @struct r_agt_regs_t
 * @brief AGT (16-bit view) register layout, one per channel.
 *
 * @details
 * Mirrors the FSP `R_AGTX0_AGT16_Type` layout from the RA8D2 BSP
 * (CMSIS device header `R7KA8D2KF_core0.h`). The 32-bit AGTW view
 * shares the same channel base but widens AGT/AGTCMA/AGTCMB to
 * 32 bits and pushes CTRL to offset 0x0C; this struct is the 16-bit
 * mode driver path. HUM Ch 24.2 "Register Descriptions" p 1167-1180.
 */
typedef struct {
  volatile uint16_t AGT;          /**< +0x00 16-bit counter.            */
  volatile uint16_t AGTCMA;       /**< +0x02 Compare Match A.           */
  volatile uint16_t AGTCMB;       /**< +0x04 Compare Match B.           */
  volatile uint16_t _r0;          /**< +0x06 reserved.                  */
  volatile uint8_t  AGTCR;        /**< +0x08 Control.                   */
  volatile uint8_t  AGTMR1;       /**< +0x09 Mode 1.                    */
  volatile uint8_t  AGTMR2;       /**< +0x0A Mode 2.                    */
  volatile uint8_t  AGTIOSEL_ALT; /**< +0x0B Pin Select (alt alias).    */
  volatile uint8_t  AGTIOC;       /**< +0x0C I/O Control.               */
  volatile uint8_t  AGTISR;       /**< +0x0D Event Pin Select.          */
  volatile uint8_t  AGTCMSR;      /**< +0x0E Compare Match Function.    */
  volatile uint8_t  AGTIOSEL;     /**< +0x0F Pin Select.                */
} r_agt_regs_t;

/** @brief Get pointer to AGT channel N. */
static inline volatile r_agt_regs_t* ra_agt(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_agt_channel_count) {
    return nullptr;
  }
  return (volatile r_agt_regs_t*)(k_ra_agt0_base_addr +
                                  ((uintptr_t)channel * (uintptr_t)k_ra_agt_channel_stride));
}

#ifdef __cplusplus
}
#endif
