/**
 * @file ra8d2_doc_regs.h
 * @brief Data Operation Circuit (DOC) register layout for the Renesas RA8D2
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
 * @enum ra_doc_addr_t
 * @brief Memory-mapped base address for the DOC block.
 */
typedef enum : uintptr_t {
  k_ra_doc_base_addr = 0x40311000UL, /**< DOC_B peripheral base, HUM Ch 57.2 p 3519. */
} ra_doc_addr_t;

/**
 * @enum ra_docr_oms_t
 * @brief Operation-mode codes written to `DOCR.OMS[1:0]` (HUM 57.2.1 p 3519).
 */
typedef enum : uint8_t {
  k_ra_doc_mode_compare  = 0U, /**< Data compare: set DOPCF when DCSEL hits. */
  k_ra_doc_mode_add      = 1U, /**< Add of DODIR into DODSR0.                */
  k_ra_doc_mode_subtract = 2U, /**< Subtract of DODIR from DODSR0.           */
  k_ra_doc_mode_reserved = 3U, /**< Reserved, must not be written.           */
} ra_docr_oms_t;

/**
 * @enum ra_docr_bit_t
 * @brief DOCR control-bit positions (HUM 57.2.1 p 3519).
 */
typedef enum : uint8_t {
  k_ra_doc_bit_oms0  = 0U, /**< Operation mode select bit 0.        */
  k_ra_doc_bit_oms1  = 1U, /**< Operation mode select bit 1.        */
  k_ra_doc_bit_dobw  = 3U, /**< Data operation bit-width select.    */
  k_ra_doc_bit_dcsel = 4U, /**< Detection condition select bit 0.   */
} ra_docr_bit_t;

/**
 * @enum ra_docr_mask_t
 * @brief DOCR bit masks (HUM 57.2.1 p 3519).
 */
typedef enum : uint8_t {
  k_ra_doc_mask_oms   = 0x03U, /**< OMS[1:0] field mask.    */
  k_ra_doc_mask_dobw  = 0x08U, /**< DOBW bit mask.          */
  k_ra_doc_mask_dcsel = 0x70U, /**< DCSEL[2:0] field mask.  */
} ra_docr_mask_t;

/**
 * @enum ra_dosr_mask_t
 * @brief DOSR status bit masks (HUM 57.2.2 p 3520).
 */
typedef enum : uint8_t {
  k_ra_doc_mask_dopcf = 0x01U, /**< Data-operation circuit flag.    */
} ra_dosr_mask_t;

/**
 * @enum ra_doscr_mask_t
 * @brief DOSCR status-clear bit masks (HUM 57.2.3 p 3521).
 */
typedef enum : uint8_t {
  k_ra_doc_mask_dopcfcl = 0x01U, /**< Write 1 to clear DOPCF.       */
} ra_doscr_mask_t;

/**
 * @struct r_doc_regs_t
 * @brief DOC_B register window (HUM Ch 57.2 p 3519-3522).
 */
typedef struct {
  volatile uint8_t  DOCR;   /**< +0x00 Control (mode, width, DCSEL).      */
  volatile uint8_t  _r0;    /**< +0x01 padding.                           */
  volatile uint16_t _r1;    /**< +0x02 padding.                           */
  volatile uint8_t  DOSR;   /**< +0x04 Status flag (DOPCF).               */
  volatile uint8_t  _r2;    /**< +0x05 padding.                           */
  volatile uint16_t _r3;    /**< +0x06 padding.                           */
  volatile uint8_t  DOSCR;  /**< +0x08 Status clear (DOPCFCL, write-only).*/
  volatile uint8_t  _r4;    /**< +0x09 padding.                           */
  volatile uint16_t _r5;    /**< +0x0A padding.                           */
  volatile uint32_t DODIR;  /**< +0x0C Data input (access at DOBW width). */
  volatile uint32_t DODSR0; /**< +0x10 Reference / running result.        */
  volatile uint32_t DODSR1; /**< +0x14 Upper window threshold.            */
} r_doc_regs_t;

/**
 * @brief Get a pointer to the DOC register block.
 * @return Volatile pointer to the DOC register window.
 */
static inline volatile r_doc_regs_t* ra_doc(void)
{
  return (volatile r_doc_regs_t*)k_ra_doc_base_addr;
}

#ifdef __cplusplus
}
#endif
