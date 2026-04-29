/**
 * @file ra8d2_dtc_regs.h
 * @brief DTC (Data Transfer Controller) register layout for the RA8D2
 *
 * @details
 * The DTC is a lighter-weight alternative to the DMAC for moving
 * small amounts of data in response to peripheral interrupts. Its
 * control registers live at `0x4000AC00` and the actual transfer
 * descriptors are stored in SRAM, indexed by the activation-source
 * vector number (Transfer Information vector table).
 *
 * Register-block layout verified against FSP `R_DTC_Type` in
 * `R7KA8D2KF_core0.h` lines 6581-6732 (size = 48 / 0x30 bytes).
 * Bit-field positions are derived from the HUM:
 *  - 18.2.1  DTCCR     : p 786
 *  - 18.2.2  DTCVBR    : p 787
 *  - 18.2.3  DTCST     : p 787
 *  - 18.2.4  DTCSTS    : p 788
 *  - 18.2.5  DTCCR_SEC : p 788
 *  - 18.2.6  DTCVBR_SEC: p 789
 *  - 18.2.7  DTCDISP   : p 789
 *  - 18.2.8  DTEVR     : p 790
 *  - 18.2.9  DTCIBR    : p 791
 *  - 18.2.10 DTCOR     : p 791
 *  - 18.2.11 DTCSQE    : p 792
 *  - 18.2.12 DTCADMOD  : p 793
 *
 * Vector-table layout (per HUM Ch 18 "Transfer Information") --
 * each Transfer Information block is 16 bytes consisting of MRA,
 * MRB, SAR, DAR, CRA, CRB; the vector table is an array of
 * `r_dtc_xfer_info_t` indexed by activation-source vector number.
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
  k_ra_dtc_base_addr = 0x4000AC00UL, /**< DTC0 register block base. */
} ra_dtc_addr_t;

typedef enum : uint8_t {
  k_ra_dtc_xfer_info_size = 16U, /**< Per-source TI block (HUM 18.2 p 793-797). */
  k_ra_dtc_vector_align   = 16U, /**< TI alignment within table (HUM 18.3.1).   */
} ra_dtc_layout_t;

typedef enum : uint16_t {
  k_ra_dtc_vector_table_align = 1024U, /**< DTCVBR write granularity (HUM 18.2.2). */
} ra_dtc_vector_align_t;

/* DTCCR / DTCCR_SEC bit positions (HUM 18.2.1 p 786). Bit 3 is
 * documented as "reserved -- write 1, read 1"; FSP encodes the
 * RRS-enable / RRS-disable values as the literals 0x18 / 0x08 to
 * preserve that constraint. */
typedef enum : uint8_t {
  k_ra_dtccr_rrs_pos     = 4U,    /**< DTCCR.RRS bit position.            */
  k_ra_dtccr_rrs_msk     = 0x10U, /**< DTCCR.RRS mask.                    */
  k_ra_dtccr_reserved3   = 0x08U, /**< Bit 3: read-as-1 / write-as-1.     */
  k_ra_dtccr_rrs_disable = 0x08U, /**< DTCCR with RRS=0 (FSP value).      */
  k_ra_dtccr_rrs_enable  = 0x18U, /**< DTCCR with RRS=1 (FSP value).      */
} ra_dtc_dtccr_bits_t;

/* DTCST bit positions (HUM 18.2.3 p 787). */
typedef enum : uint8_t {
  k_ra_dtcst_dtcst_pos = 0U,   /**< DTCST.DTCST bit position. */
  k_ra_dtcst_dtcst_msk = 0x1U, /**< DTCST.DTCST mask.         */
} ra_dtc_dtcst_bits_t;

/* DTCSTS bit positions (HUM 18.2.4 p 788). */
typedef enum : uint16_t {
  k_ra_dtcsts_vecn_msk = 0x00FFU, /**< Active-vector number field. */
  k_ra_dtcsts_act_msk  = 0x8000U, /**< Active flag (bit 15).       */
} ra_dtc_dtcsts_bits_t;

/* DTCADMOD bit positions (HUM 18.2.12 p 793). */
typedef enum : uint8_t {
  k_ra_dtcadmod_short_msk = 0x1U, /**< SHORT mode bit. */
} ra_dtc_dtcadmod_bits_t;

/**
 * @struct r_dtc_regs_t
 * @brief DTC controller register block at `0x4000AC00`.
 *
 * @details
 * Layout verified against FSP `R_DTC_Type` (`R7KA8D2KF_core0.h`
 * lines 6581-6732). Total size = 0x30 bytes. Reserved members are
 * named with an underscore prefix to keep the FSP register names
 * directly addressable.
 */
typedef struct {
  volatile uint8_t  DTCCR;      /**< +0x00 Control Register (HUM 18.2.1).             */
  volatile uint8_t  _r0;        /**< +0x01 reserved.                                   */
  volatile uint16_t _r1;        /**< +0x02 reserved.                                   */
  volatile uint32_t DTCVBR;     /**< +0x04 Vector Base Register (HUM 18.2.2).         */
  volatile uint8_t  DTCADMOD;   /**< +0x08 Address Mode Register (HUM 18.2.12).        */
  volatile uint8_t  _r2;        /**< +0x09 reserved.                                   */
  volatile uint16_t _r3;        /**< +0x0A reserved.                                   */
  volatile uint8_t  DTCST;      /**< +0x0C Module Start Register (HUM 18.2.3).        */
  volatile uint8_t  _r4;        /**< +0x0D reserved.                                   */
  volatile uint16_t DTCSTS;     /**< +0x0E Status Register (HUM 18.2.4).              */
  volatile uint8_t  DTCCR_SEC;  /**< +0x10 Secure Control Register (HUM 18.2.5).      */
  volatile uint8_t  _r5;        /**< +0x11 reserved.                                   */
  volatile uint16_t _r6;        /**< +0x12 reserved.                                   */
  volatile uint32_t DTCVBR_SEC; /**< +0x14 Secure Vector Base (HUM 18.2.6).           */
  volatile uint32_t DTCDISP;    /**< +0x18 Address Displacement (HUM 18.2.7).         */
  volatile uint32_t _r7;        /**< +0x1C reserved.                                   */
  volatile uint32_t DTEVR;      /**< +0x20 DTC Error Vector Register (HUM 18.2.8).    */
  volatile uint32_t DTCIBR;     /**< +0x24 Index Table Base Register (HUM 18.2.9).    */
  volatile uint8_t  DTCOR;      /**< +0x28 Operation Register (HUM 18.2.10).          */
  volatile uint8_t  _r8;        /**< +0x29 reserved.                                   */
  volatile uint16_t _r9;        /**< +0x2A reserved.                                   */
  volatile uint16_t DTCSQE;     /**< +0x2C Sequence Transfer Enable (HUM 18.2.11).    */
  volatile uint16_t _r10;       /**< +0x2E reserved.                                   */
} r_dtc_regs_t;

/**
 * @struct r_dtc_xfer_info_t
 * @brief One Transfer Information (TI) entry in the DTC vector table.
 *
 * @details
 * Each entry is exactly 16 bytes (HUM Ch 18 "Transfer Information"
 * p 793-797). The vector table is an array of these structures
 * placed in SRAM and pointed to by `DTCVBR`; each activation
 * source's TI lives at `DTCVBR + (vector_number * 16)`.
 *
 * Field layout matches FSP `transfer_info_t` so call-sites can be
 * ported one-for-one (and the on-the-wire DTC TI block layout is
 * therefore 16 bytes on every target):
 *  - MR (32 bits, MRA upper / MRB lower) = transfer settings word.
 *  - SAR / DAR = source / destination 32-bit bus addresses.
 *  - CRB = block-mode block count.
 *  - CRA = transfer count (16 bits) -- repeat / block split as
 *    upper-byte (CRAH) / lower-byte (CRAL) halves.
 *
 * SAR / DAR are `uint32_t` rather than `void*` so the structure is
 * exactly 16 bytes on both the 32-bit Cortex-M85 target and the
 * 64-bit unit-test host. Call sites build addresses with
 * `(uint32_t)(uintptr_t)ptr` (HUM 18.2 p 793-797).
 */
typedef struct {
  volatile uint32_t MR;  /**< Mode register (MRA upper / MRB lower / reserved). */
  volatile uint32_t SAR; /**< Source address register.                          */
  volatile uint32_t DAR; /**< Destination address register.                     */
  volatile uint16_t CRB; /**< Block-count register (block mode only).           */
  volatile uint16_t CRA; /**< Transfer-count register (CRAH/CRAL).              */
} r_dtc_xfer_info_t;

/** @brief Get pointer to the DTC controller register block. */
static inline volatile r_dtc_regs_t* ra_dtc(void)
{
  return (volatile r_dtc_regs_t*)k_ra_dtc_base_addr;
}

#ifdef __cplusplus
}
#endif
