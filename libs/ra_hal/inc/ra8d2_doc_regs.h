/**
 * @file ra8d2_doc_regs.h
 * @brief Data Operation Circuit (DOC) register layout for the Renesas RA8D2
 *
 * @details
 * The Data Operation Circuit is a tiny hardware helper that performs
 * 16-bit add, subtract, and compare operations between a data-in
 * register (`DODIR`) and a running result register (`DODSR`). The
 * supported modes are selected by the `DOCR.OMS[1:0]` field:
 *
 * | OMS | Mode      | Action                                 |
 * |----:|-----------|----------------------------------------|
 * |  00 | Compare   | Set DOPCF if DODIR equals DODSR        |
 * |  01 | Add       | DODSR <- DODSR + DODIR (overflow flag) |
 * |  10 | Subtract  | DODSR <- DODSR - DODIR (borrow flag)   |
 * |  11 | reserved  | -                                      |
 *
 * Typical firmware uses it as a cheap hardware-assisted 16-bit
 * checksum accumulator for telemetry frames. The block lives at
 * base `0x40354100` (peripheral bus). This header models the full
 * register window needed by `ra_doc.c`.
 *
 * Register offsets tracked against HUM Ch 57 "Data Operation
 * Circuit (DOC)" p 3518.
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
  k_ra_doc_base_addr = 0x40354100UL, /**< DOC peripheral base address. */
} ra_doc_addr_t;

/**
 * @enum ra_docr_oms_t
 * @brief Operation-mode codes written to `DOCR.OMS[1:0]`.
 */
typedef enum : uint8_t {
  k_ra_doc_mode_compare  = 0U, /**< Data compare: set DOPCF on equality. */
  k_ra_doc_mode_add      = 1U, /**< 16-bit add of DODIR into DODSR.     */
  k_ra_doc_mode_subtract = 2U, /**< 16-bit subtract of DODIR from DODSR.*/
  k_ra_doc_mode_reserved = 3U, /**< Reserved, must not be written.       */
} ra_docr_oms_t;

/**
 * @enum ra_docr_bit_t
 * @brief DOCR control-bit positions.
 */
typedef enum : uint8_t {
  k_ra_doc_bit_oms0   = 0U, /**< Operation mode select bit 0.         */
  k_ra_doc_bit_oms1   = 1U, /**< Operation mode select bit 1.         */
  k_ra_doc_bit_dcsel  = 2U, /**< Compare match/ mismatch select.      */
  k_ra_doc_bit_dopcie = 5U, /**< Event-output enable.                 */
  k_ra_doc_bit_dopcf  = 6U, /**< Operation-complete / fault flag.     */
  k_ra_doc_bit_dopcfc = 7U, /**< DOPCF clear bit (write 1 to clear).  */
} ra_docr_bit_t;

/**
 * @enum ra_docr_mask_t
 * @brief DOCR bit masks (use the explicit values in driver code).
 */
typedef enum : uint8_t {
  k_ra_doc_mask_oms   = 0x03U, /**< OMS[1:0] field mask.              */
  k_ra_doc_mask_dcsel = 0x04U, /**< DCSEL bit mask.                   */
  k_ra_doc_mask_dopcf = 0x40U, /**< DOPCF status bit mask.            */
} ra_docr_mask_t;

/**
 * @struct r_doc_regs_t
 * @brief DOC register window.
 */
typedef struct {
  volatile uint8_t  DOCR;  /**< +0x00 Control register (mode, flags).  */
  volatile uint8_t  _r0;   /**< Padding.                               */
  volatile uint16_t DODIR; /**< +0x02 Data-input register.             */
  volatile uint16_t DODSR; /**< +0x04 Data-operation / result register.*/
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
