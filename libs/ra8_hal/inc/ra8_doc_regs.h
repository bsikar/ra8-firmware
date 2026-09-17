/**
 * @file ra8_doc_regs.h
 * @brief Data Operation Circuit (DOC) register layout for the Renesas RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * The Data Operation Circuit on the RA8D2 is the **DOC_B** variant.
 * It supports 16- or 32-bit add, subtract, and compare operations.
 * Operation mode and bit-width are selected via the `DOCR.OMS[1:0]`
 * and `DOCR.DOBW` fields:
 *
 * | OMS | Mode     | Action                                      |
 * |----:|----------|---------------------------------------------|
 * |  00 | Compare  | Set DOPCF when DOCR.DCSEL condition matches |
 * |  01 | Add      | DODSR0 <- DODSR0 + DODIR (overflow flag)    |
 * |  10 | Subtract | DODSR0 <- DODSR0 - DODIR (borrow flag)      |
 * |  11 | reserved | -                                           |
 *
 * Register window layout (HUM Ch 57.2, p 3519-3522):
 *
 * | Offset | Width | Name   | Function                                  |
 * |-------:|------:|--------|-------------------------------------------|
 * |  0x00  |  8    | DOCR   | Control (OMS, DOBW, DCSEL)                |
 * |  0x04  |  8    | DOSR   | Status flag (DOPCF)                       |
 * |  0x08  |  8    | DOSCR  | Status clear (DOPCFCL, write-only)        |
 * |  0x0C  | 32    | DODIR  | Data input (access width per DOBW)        |
 * |  0x10  | 32    | DODSR0 | Reference / running result (per DOBW)     |
 * |  0x14  | 32    | DODSR1 | Upper window threshold (compare mode)     |
 *
 * The block lives at base `0x40311000` (peripheral bus). Layout
 * cross-verified against HUM Ch 57 "Data Operation Circuit (DOC)"
 * p 3518-3522.
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
 * @enum ra8_doc_addr_t
 * @brief Memory-mapped base address for the DOC block.
 */
typedef enum : uintptr_t {
  k_ra8_doc_base_addr = 0x40311000UL, /**< DOC_B peripheral base, HUM Ch 57.2 p 3519. */
} ra8_doc_addr_t;

/**
 * @enum ra8_docr_oms_t
 * @brief Operation-mode codes written to `DOCR.OMS[1:0]` (HUM 57.2.1 p 3519).
 */
typedef enum : uint8_t {
  k_ra8_doc_mode_compare  = 0U, /**< Data compare: set DOPCF when DCSEL hits. */
  k_ra8_doc_mode_add      = 1U, /**< Add of DODIR into DODSR0.                */
  k_ra8_doc_mode_subtract = 2U, /**< Subtract of DODIR from DODSR0.           */
  k_ra8_doc_mode_reserved = 3U, /**< Reserved, must not be written.           */
} ra8_docr_oms_t;

/**
 * @enum ra8_docr_bit_t
 * @brief DOCR control-bit positions (HUM 57.2.1 p 3519).
 */
typedef enum : uint8_t {
  k_ra8_doc_bit_oms0  = 0U, /**< Operation mode select bit 0.      */
  k_ra8_doc_bit_oms1  = 1U, /**< Operation mode select bit 1.      */
  k_ra8_doc_bit_dobw  = 3U, /**< Data operation bit-width select.  */
  k_ra8_doc_bit_dcsel = 4U, /**< Detection condition select bit 0. */
} ra8_docr_bit_t;

/**
 * @enum ra8_docr_mask_t
 * @brief DOCR bit masks (HUM 57.2.1 p 3519).
 */
typedef enum : uint8_t {
  k_ra8_doc_mask_oms   = 0x03U, /**< OMS[1:0] field mask.                      */
  k_ra8_doc_mask_dobw  = 0x08U, /**< DOBW bit mask.                            */
  k_ra8_doc_mask_dcsel = 0x70U, /**< DCSEL[2:0] field mask in DOCR (bits 6:4). */
} ra8_docr_mask_t;

/**
 * @enum ra8_docr_field_mask_t
 * @brief Mask for the 3-bit DCSEL sub-field after right-shifting DOCR by
 *        k_ra8_doc_bit_dcsel (HUM 57.2.1 p 3519).
 *
 * @details
 * After reading DOCR and shifting right by k_ra8_doc_bit_dcsel (4), AND with
 * this mask to isolate the 3-bit DCSEL value before comparing against
 * ra8_docr_dcsel_t constants.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_doc_mask_dcsel_field = 0x07U, /**< 3-bit DCSEL value mask (bits 2:0). */
} ra8_docr_field_mask_t;

/**
 * @enum ra8_docr_dcsel_t
 * @brief Detection-condition select codes for DOCR.DCSEL[2:0] (HUM 57.2.1 p 3519).
 *
 * @details
 * These are the 3-bit values written into DOCR bits [6:4] (i.e. the
 * pre-shift field value). They are valid only when DOCR.OMS[1:0] == 00
 * (data comparison mode). Values 6 and 7 are reserved per the HUM.
 *
 * For window modes (k_ra8_doc_dcsel_inside and k_ra8_doc_dcsel_outside)
 * DODSR0 is the lower threshold and DODSR1 is the upper threshold.
 * The HUM constraint DODSR1 > DODSR0 (strict) must hold; the driver
 * API enforces this at ra8_doc_set_window() time.
 *
 * Boundary values (DODIR == DODSR0 or DODIR == DODSR1) do NOT satisfy
 * either window condition; the comparisons are strictly less-than / greater-than.
 *
 * @invariant Values 6 and 7 must never be written to DOCR.DCSEL.
 * @invariant DODSR1 > DODSR0 whenever inside or outside window mode is active.
 *
 * @code
 * // Select inside-window compare: DOCR bits[6:4] = 4
 * reg->DOCR = (uint8_t)((uint8_t)k_ra8_doc_dcsel_inside
 *                       << (uint8_t)k_ra8_doc_bit_dcsel);
 * @endcode
 *
 * @see ra8_doc_set_window() High-level window-configuration API.
 * @see ra8_doc_window_compare() Trigger a comparison and read DOPCF.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_doc_dcsel_mismatch = 0U, /**< Set DOPCF when DODSR0 != DODIR.                            */
  k_ra8_doc_dcsel_match    = 1U, /**< Set DOPCF when DODSR0 == DODIR.                            */
  k_ra8_doc_dcsel_lower    = 2U, /**< Set DOPCF when DODSR0 > DODIR.                             */
  k_ra8_doc_dcsel_upper    = 3U, /**< Set DOPCF when DODSR0 < DODIR.                             */
  k_ra8_doc_dcsel_inside   = 4U, /**< Set DOPCF when DODSR0 < DODIR < DODSR1 (inside window).    */
  k_ra8_doc_dcsel_outside  = 5U, /**< Set DOPCF when DODIR < DODSR0 or DODSR1 < DODIR (outside). */
} ra8_docr_dcsel_t;

/**
 * @enum ra8_dosr_mask_t
 * @brief DOSR status bit masks (HUM 57.2.2 p 3520).
 */
typedef enum : uint8_t {
  k_ra8_doc_mask_dopcf = 0x01U, /**< Data-operation circuit flag. */
} ra8_dosr_mask_t;

/**
 * @enum ra8_doscr_mask_t
 * @brief DOSCR status-clear bit masks (HUM 57.2.3 p 3521).
 */
typedef enum : uint8_t {
  k_ra8_doc_mask_dopcfcl = 0x01U, /**< Write 1 to clear DOPCF. */
} ra8_doscr_mask_t;

/**
 * @struct r_doc_regs_t
 * @brief DOC_B register window (HUM Ch 57.2 p 3519-3522).
 */
typedef struct {
  volatile uint8_t  DOCR;   /**< +0x00 Control (mode, width, DCSEL).       */
  volatile uint8_t  _r0;    /**< +0x01 padding.                            */
  volatile uint16_t _r1;    /**< +0x02 padding.                            */
  volatile uint8_t  DOSR;   /**< +0x04 Status flag (DOPCF).                */
  volatile uint8_t  _r2;    /**< +0x05 padding.                            */
  volatile uint16_t _r3;    /**< +0x06 padding.                            */
  volatile uint8_t  DOSCR;  /**< +0x08 Status clear (DOPCFCL, write-only). */
  volatile uint8_t  _r4;    /**< +0x09 padding.                            */
  volatile uint16_t _r5;    /**< +0x0A padding.                            */
  volatile uint32_t DODIR;  /**< +0x0C Data input (access at DOBW width).  */
  volatile uint32_t DODSR0; /**< +0x10 Reference / running result.         */
  volatile uint32_t DODSR1; /**< +0x14 Upper window threshold.             */
} r_doc_regs_t;

/**
 * @brief Get a pointer to the DOC register block.
 * @return Volatile pointer to the DOC register window.
 */
static inline volatile r_doc_regs_t* ra8_doc(void)
{
  return (volatile r_doc_regs_t*)k_ra8_doc_base_addr;
}

#ifdef __cplusplus
}
#endif
