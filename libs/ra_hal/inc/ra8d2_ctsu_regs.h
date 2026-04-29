/**
 * @file ra8d2_ctsu_regs.h
 * @brief Capacitive Touch Sensing Unit (CTSU2) register layout
 *
 * @details
 * Register window for the CTSU2 (Capacitive Touch Sensing Unit 2)
 * peripheral as exposed by the Renesas FSP ``R_CTSU2_Type`` typedef.
 * The RA8D2 device variant on the EK-RA8D2 board does not enumerate
 * CTSU2 in its core BSP, but every Renesas RA family chip with on-die
 * touch electrodes uses the same fixed CTSU2 register block; this
 * driver mirrors that block one-for-one so the same source compiles on
 * any RA family member that wires CTSU into its peripheral bus map.
 * The base address used here matches the R_CTSU2_BASE that FSP applies
 * for RA family devices that include the IP (peripheral bus + 0x81000).
 *
 * Register names map directly to FSP / HUM. The HAL only touches the
 * 8 / 16-bit byte aliases that are stable across silicon revisions; the
 * 32-bit unionised aliases used by FSP for CTSU2 are intentionally not
 * exposed here so that calling code does not depend on bit-field
 * ordering, which is implementation defined.
 *
 * Reference: HUM Ch 51 "Capacitive Touch Sensing Unit (CTSU)" and
 * ``r_ctsu.c`` from the Renesas FSP for the field-level semantics.
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
 * @enum ra_ctsu_addr_t
 * @brief CTSU2 base address constants.
 */
typedef enum : uintptr_t {
  k_ra_ctsu_base_addr = 0x40081000UL, /**< CTSU2 peripheral base. */
} ra_ctsu_addr_t;

/**
 * @enum ra_ctsu_limits_t
 * @brief Driver limits derived from silicon (HUM Ch 51 Table 51.1).
 */
typedef enum : uint8_t {
  k_ra_ctsu_max_electrodes  = 36U, /**< CTSU2 supports up to 36 TS pins. */
  k_ra_ctsu_calib_dummy_cnt = 3U,  /**< FSP-style 3 dummy scans before averaging. */
} ra_ctsu_limits_t;

/**
 * @enum ra_ctsu_cr0_bit_t
 * @brief CTSUCR0 control bit positions (HUM Ch 51 / FSP r_ctsu).
 */
typedef enum : uint8_t {
  k_ra_ctsu_cr0_bit_strt = 0U, /**< Measurement start. */
  k_ra_ctsu_cr0_bit_cap  = 1U, /**< Software trigger / capture. */
  k_ra_ctsu_cr0_bit_snz  = 2U, /**< Wait state enable. */
  k_ra_ctsu_cr0_bit_ie   = 3U, /**< Interrupt enable. */
  k_ra_ctsu_cr0_bit_pon  = 4U, /**< CTSU power on. */
  k_ra_ctsu_cr0_bit_csw  = 5U, /**< TSCAP discharge. */
  k_ra_ctsu_cr0_bit_atue = 6U, /**< Auto-tune enable. */
  k_ra_ctsu_cr0_bit_init = 7U, /**< Software reset. */
} ra_ctsu_cr0_bit_t;

/**
 * @enum ra_ctsu_cr0_mask_t
 * @brief CTSUCR0 bit masks.
 */
typedef enum : uint8_t {
  k_ra_ctsu_cr0_mask_strt = 0x01U, /**< STRT bit mask. */
  k_ra_ctsu_cr0_mask_pon  = 0x10U, /**< PON power-on bit. */
  k_ra_ctsu_cr0_mask_init = 0x80U, /**< INIT software-reset bit. */
} ra_ctsu_cr0_mask_t;

/**
 * @enum ra_ctsu_cr1_bit_t
 * @brief CTSUCR1 control bit positions.
 */
typedef enum : uint8_t {
  k_ra_ctsu_cr1_bit_ctsuchac = 0U, /**< Channel control select. */
  k_ra_ctsu_cr1_bit_atune0   = 1U, /**< Power-supply tune lo. */
  k_ra_ctsu_cr1_bit_atune1   = 2U, /**< Power-supply tune hi. */
  k_ra_ctsu_cr1_bit_clk      = 3U, /**< Operating clock select. */
  k_ra_ctsu_cr1_bit_md0      = 4U, /**< Measurement mode bit 0. */
  k_ra_ctsu_cr1_bit_md1      = 6U, /**< Measurement mode bit 1. */
} ra_ctsu_cr1_bit_t;

/**
 * @enum ra_ctsu_cr1_mask_t
 * @brief CTSUCR1 field masks.
 */
typedef enum : uint8_t {
  k_ra_ctsu_cr1_mask_md = 0x30U, /**< MD[1:0] field mask. */
} ra_ctsu_cr1_mask_t;

/**
 * @enum ra_ctsu_st_bit_t
 * @brief CTSUST status bit positions (FSP names).
 */
typedef enum : uint8_t {
  k_ra_ctsu_st_bit_stc    = 0U, /**< Measurement-status counter LSB. */
  k_ra_ctsu_st_bit_dtsr   = 4U, /**< Data-transfer status. */
  k_ra_ctsu_st_bit_sovf   = 5U, /**< Sensor counter overflow. */
  k_ra_ctsu_st_bit_icomp1 = 6U, /**< Sense current monitor. */
  k_ra_ctsu_st_bit_icomp0 = 7U, /**< TSCAP voltage monitor. */
} ra_ctsu_st_bit_t;

/**
 * @enum ra_ctsu_st_mask_t
 * @brief CTSUST status masks.
 */
typedef enum : uint8_t {
  k_ra_ctsu_st_mask_dtsr = 0x10U, /**< DTSR bit mask. */
  k_ra_ctsu_st_mask_sovf = 0x20U, /**< SOVF bit mask. */
  k_ra_ctsu_st_mask_err  = 0xC0U, /**< ICOMP0 | ICOMP1. */
} ra_ctsu_st_mask_t;

/**
 * @enum ra_ctsu_block_size_t
 * @brief Sizes of the CTSU register block (used by ``static_assert``).
 */
typedef enum : uint16_t {
  k_ra_ctsu_block_bytes = 0x30U, /**< Total register-window size. */
} ra_ctsu_block_size_t;

/**
 * @enum ra_ctsu_byte_t
 * @brief Helpers for splitting the 36-bit electrode bitmap into bytes.
 */
typedef enum : uint8_t {
  k_ra_ctsu_byte_mask = 0xFFU, /**< Single-byte mask. */
  k_ra_ctsu_shift_b1  = 8U,    /**< Shift to byte index 1. */
  k_ra_ctsu_shift_b2  = 16U,   /**< Shift to byte index 2. */
  k_ra_ctsu_shift_b3  = 24U,   /**< Shift to byte index 3. */
  k_ra_ctsu_shift_b4  = 32U,   /**< Shift to byte index 4. */
} ra_ctsu_byte_t;

/**
 * @struct r_ctsu_regs_t
 * @brief CTSU2 register window mirrored from FSP ``R_CTSU2_Type``.
 *
 * @details
 * Field offsets (in bytes from base):
 * - 0x00 CTSUCR0 / CTSUCR1 / CTSUCR2 / CTSUCR3 (control A)
 * - 0x04 CTSUSDPRS / CTSUSST / reserved / CTSUDCLKC (control B)
 * - 0x08 CTSUMCH0 / CTSUMCH1 + extension (measurement channel)
 * - 0x0C CTSUCHAC0..CTSUCHAC3 (channel enable A)
 * - 0x10 CTSUCHAC4 + reserved (channel enable B)
 * - 0x14 CTSUCHTRC0..CTSUCHTRC3 (channel transmit/receive A)
 * - 0x18 CTSUCHTRC4 + reserved (channel transmit/receive B)
 * - 0x1C CTSUSR0 / CTSUST (status)
 * - 0x20 CTSUSO0 / CTSUSO1 (sensor offset)
 * - 0x24 CTSUSC (sensor counter result, low half) / CTSUSC1 (high)
 * - 0x28 CTSUCALIB (analog calibration)
 * - 0x2C CTSUERRS (error status)
 */
typedef struct {
  volatile uint8_t  CTSUCR0;    /**< +0x00 Control register A0. */
  volatile uint8_t  CTSUCR1;    /**< +0x01 Control register A1. */
  volatile uint8_t  CTSUCR2;    /**< +0x02 Control register A2. */
  volatile uint8_t  CTSUCR3;    /**< +0x03 Control register A3. */
  volatile uint8_t  CTSUSDPRS;  /**< +0x04 Scan period setting. */
  volatile uint8_t  CTSUSST;    /**< +0x05 Sensor stabilisation wait. */
  volatile uint8_t  _r0;        /**< +0x06 Reserved. */
  volatile uint8_t  CTSUDCLKC;  /**< +0x07 Drive clock control. */
  volatile uint8_t  CTSUMCH0;   /**< +0x08 Measurement channel 0. */
  volatile uint8_t  CTSUMCH1;   /**< +0x09 Measurement channel 1. */
  volatile uint16_t _r1;        /**< +0x0A Reserved (high half of CTSUMCH). */
  volatile uint8_t  CTSUCHAC0;  /**< +0x0C Channel enable bitmap byte 0. */
  volatile uint8_t  CTSUCHAC1;  /**< +0x0D Channel enable bitmap byte 1. */
  volatile uint8_t  CTSUCHAC2;  /**< +0x0E Channel enable bitmap byte 2. */
  volatile uint8_t  CTSUCHAC3;  /**< +0x0F Channel enable bitmap byte 3. */
  volatile uint8_t  CTSUCHAC4;  /**< +0x10 Channel enable bitmap byte 4. */
  volatile uint8_t  _r2;        /**< +0x11 Reserved. */
  volatile uint16_t _r3;        /**< +0x12 Reserved. */
  volatile uint8_t  CTSUCHTRC0; /**< +0x14 Tx/Rx select bitmap byte 0. */
  volatile uint8_t  CTSUCHTRC1; /**< +0x15 Tx/Rx select bitmap byte 1. */
  volatile uint8_t  CTSUCHTRC2; /**< +0x16 Tx/Rx select bitmap byte 2. */
  volatile uint8_t  CTSUCHTRC3; /**< +0x17 Tx/Rx select bitmap byte 3. */
  volatile uint8_t  CTSUCHTRC4; /**< +0x18 Tx/Rx select bitmap byte 4. */
  volatile uint8_t  _r4;        /**< +0x19 Reserved. */
  volatile uint16_t _r5;        /**< +0x1A Reserved. */
  volatile uint8_t  CTSUSR0;    /**< +0x1C Status register low. */
  volatile uint8_t  CTSUST;     /**< +0x1D Status register high (FSP CTSUST). */
  volatile uint16_t _r6;        /**< +0x1E Reserved. */
  volatile uint16_t CTSUSO0;    /**< +0x20 Sensor offset register 0. */
  volatile uint16_t CTSUSO1;    /**< +0x22 Sensor offset register 1. */
  volatile uint16_t CTSUSC0;    /**< +0x24 Sensor count low (measurement result). */
  volatile uint16_t CTSUSC1;    /**< +0x26 Sensor count high. */
  volatile uint32_t CTSUCALIB;  /**< +0x28 Calibration register. */
  volatile uint16_t CTSUERRS;   /**< +0x2C Error status. */
  volatile uint16_t _r7;        /**< +0x2E Reserved. */
} r_ctsu_regs_t;

static_assert(sizeof(r_ctsu_regs_t) == (size_t)k_ra_ctsu_block_bytes, "CTSU register block size");

/**
 * @brief Pointer to the CTSU2 register window.
 * @return Volatile pointer to the CTSU2 block.
 */
static inline volatile r_ctsu_regs_t* ra_ctsu_regs(void)
{
  return (volatile r_ctsu_regs_t*)k_ra_ctsu_base_addr;
}

#ifdef __cplusplus
}
#endif
