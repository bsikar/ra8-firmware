/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_agt_regs.h
 * @brief Asynchronous General-Purpose Timer (AGT) register layout for RA8D2
 * @ingroup grp_hal_timers
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
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

typedef enum : uintptr_t {
  k_ra8_agt0_base_addr = 0x40221000UL, /**< RA8 agt0 base address. */
} ra8_agt_addr_t;

typedef enum : uint16_t {
  k_ra8_agt_channel_count  = 10U,    /**< RA8 AGT channel count.  */
  k_ra8_agt_channel_stride = 0x100U, /**< RA8 AGT channel stride. */
} ra8_agt_limits_t;

/**
 * @brief AGTCR (AGT Control Register) bit positions.
 * @details HUM Ch 24.2.4 "AGTCR : AGT Control Register" p 1167.
 */
typedef enum : uint8_t {
  k_ra8_agt_agtcr_tstart_pos = 0U, /**< Start request (RW).      */
  k_ra8_agt_agtcr_tcstf_pos  = 1U, /**< Count status flag (RO).  */
  k_ra8_agt_agtcr_tstop_pos  = 2U, /**< Forced stop (W).         */
  k_ra8_agt_agtcr_tedgf_pos  = 4U, /**< Active edge flag (RW1C). */
  k_ra8_agt_agtcr_tundf_pos  = 5U, /**< Underflow flag (RW1C).   */
  k_ra8_agt_agtcr_tcmaf_pos  = 6U, /**< Compare-match A (RW1C).  */
  k_ra8_agt_agtcr_tcmbf_pos  = 7U, /**< Compare-match B (RW1C).  */
} ra8_agt_agtcr_bits_t;

/**
 * @brief AGTCR bit masks.
 * @details Mirror of `ra8_agt_agtcr_bits_t` for ALU use.
 */
typedef enum : uint8_t {
  k_ra8_agt_agtcr_tstart_msk = 0x01U, /**< RA8 AGT agtcr tstart msk. */
  k_ra8_agt_agtcr_tcstf_msk  = 0x02U, /**< RA8 AGT agtcr tcstf msk.  */
  k_ra8_agt_agtcr_tstop_msk  = 0x04U, /**< RA8 AGT agtcr tstop msk.  */
  k_ra8_agt_agtcr_tundf_msk  = 0x20U, /**< Underflow flag.           */
  k_ra8_agt_agtcr_tcmaf_msk  = 0x40U, /**< Compare-match A flag.     */
  k_ra8_agt_agtcr_tcmbf_msk  = 0x80U, /**< Compare-match B flag.     */
  k_ra8_agt_agtcr_status_msk = 0xF0U, /**< TEDGF|TUNDF|TCMAF|TCMBF   */
} ra8_agt_agtcr_masks_t;

/**
 * @brief AGTMR1 (AGT Mode Register 1) field encodings.
 * @details HUM Ch 24.2.5 "AGTMR1 : AGT Mode Register 1" p 1168.
 */
typedef enum : uint8_t {
  k_ra8_agt_agtmr1_tmod_timer         = 0x00U, /**< TMOD = 000b.            */
  k_ra8_agt_agtmr1_tmod_pulse_output  = 0x01U, /**< TMOD = 001b.            */
  k_ra8_agt_agtmr1_tmod_event_count   = 0x02U, /**< TMOD = 010b.            */
  k_ra8_agt_agtmr1_tmod_mask          = 0x07U, /**< TMOD[2:0] field mask.   */
  k_ra8_agt_agtmr1_tck_pclkb          = 0x00U, /**< TCK = 000b @ bit 4.     */
  k_ra8_agt_agtmr1_tck_pclkb_div8     = 0x10U, /**< TCK = 001b @ bit 4.     */
  k_ra8_agt_agtmr1_tck_pclkb_div2     = 0x30U, /**< TCK = 011b @ bit 4.     */
  k_ra8_agt_agtmr1_tck_agt0_underflow = 0x50U, /**< TCK = 101b (AGT1 only). */
  k_ra8_agt_agtmr1_tck_mask           = 0x70U, /**< TCK[2:0] field mask.    */
} ra8_agt_agtmr1_fields_t;

/**
 * @brief AGTIOC (AGT I/O Control Register) bit masks.
 * @details HUM Ch 24.2.7 "AGTIOC : AGT I/O Control Register" p 1170.
 */
typedef enum : uint8_t {
  k_ra8_agt_agtioc_tedgsel_msk = 0x01U, /**< Output polarity bit 0.   */
  k_ra8_agt_agtioc_toe_msk     = 0x04U, /**< AGTOn pin output enable. */
} ra8_agt_agtioc_masks_t;

/**
 * @brief Compare-match register "parked" sentinel.
 * @details HUM Ch 24.2.2 "AGTCMA : AGT Compare Match A Register" p 1166
 *          Note 1: "Set the AGTCMA register to 0xFFFF when compare match
 *          A is not used." Same applies to AGTCMB (HUM Ch 24.2.3 p 1166).
 */
typedef enum : uint16_t {
  k_ra8_agt_compare_parked_value = 0xFFFFU, /**< RA8 AGT compare parked value. */
} ra8_agt_compare_sentinel_t;

/**
 * @brief AGTCMSR (AGT Compare Match Function Select) bit masks.
 * @details HUM Ch 24.2.9 "AGTCMSR : AGT Compare Match Function Select
 *          Register" p 1172.
 */
typedef enum : uint8_t {
  k_ra8_agt_agtcmsr_tcmea_msk  = 0x01U, /**< Compare match A enable. */
  k_ra8_agt_agtcmsr_toea_msk   = 0x02U, /**< AGTOAn output enable.   */
  k_ra8_agt_agtcmsr_topola_msk = 0x04U, /**< AGTOAn output polarity. */
  k_ra8_agt_agtcmsr_tcmeb_msk  = 0x10U, /**< Compare match B enable. */
  k_ra8_agt_agtcmsr_toeb_msk   = 0x20U, /**< AGTOBn output enable.   */
  k_ra8_agt_agtcmsr_topolb_msk = 0x40U, /**< AGTOBn output polarity. */
} ra8_agt_agtcmsr_masks_t;

/**
 * @brief Cascade-mode channel indices.
 * @details RA8D2 only supports cascade on the AGT0 / AGT1 pair --
 *          AGT1's TCK[2:0] = 101b uses "Underflow event signal from
 *          AGT0" (HUM Ch 24.2.5 p 1168, Note 6).
 */
typedef enum : uint8_t {
  k_ra8_agt_cascade_lo_channel = 0U, /**< Low 16 bits live in AGT0.  */
  k_ra8_agt_cascade_hi_channel = 1U, /**< High 16 bits live in AGT1. */
} ra8_agt_cascade_channels_t;

/**
 * @struct r_agt_regs_t
 * @brief AGT (16-bit view) register layout, one per channel.
 *
 * @details
 * Mirrors the FSP `R_AGTX0_AGT16_Type` layout from the RA8D2 BSP
 * (CMSIS device header `R7KA8D2KF_core0.h`). The 32-bit AGTW view
 * shares the same channel base but widens AGT/AGTCMA/AGTCMB to
 * 32 bits and pushes CTRL to offset 0x0C; this struct is the 16-bit
 * mode driver path. HUM Ch 24.2 "Register Descriptions" p 1165-1173.
 */
typedef struct {
  volatile uint16_t AGT;          /**< +0x00 16-bit counter.         */
  volatile uint16_t AGTCMA;       /**< +0x02 Compare Match A.        */
  volatile uint16_t AGTCMB;       /**< +0x04 Compare Match B.        */
  volatile uint16_t _r0;          /**< +0x06 reserved.               */
  volatile uint8_t  AGTCR;        /**< +0x08 Control.                */
  volatile uint8_t  AGTMR1;       /**< +0x09 Mode 1.                 */
  volatile uint8_t  AGTMR2;       /**< +0x0A Mode 2.                 */
  volatile uint8_t  AGTIOSEL_ALT; /**< +0x0B Pin Select (alt alias). */
  volatile uint8_t  AGTIOC;       /**< +0x0C I/O Control.            */
  volatile uint8_t  AGTISR;       /**< +0x0D Event Pin Select.       */
  volatile uint8_t  AGTCMSR;      /**< +0x0E Compare Match Function. */
  volatile uint8_t  AGTIOSEL;     /**< +0x0F Pin Select.             */
} r_agt_regs_t;

/** @brief Get pointer to AGT channel N. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_agt_regs_t* ra8_agt(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_agt_channel_count) {
    return nullptr;
  }
  return (volatile r_agt_regs_t*)(k_ra8_agt0_base_addr +
                                  ((uintptr_t)channel * (uintptr_t)k_ra8_agt_channel_stride));
}

#ifdef __cplusplus
}
#endif
