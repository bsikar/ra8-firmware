/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_periph_crc.c
 * @brief CRC calculator peripheral-block model for the board emulator
 *
 * @details
 * Models the RA8D2 CRC unit (ra8_crc_regs.h, ra8_crc.c) at @c 0x40310000 so
 * @c crc_demo's hardware CRC matches its software reference (it printed
 * @c "crc: hw=00000000 ... match=N" against the sparse fallback, which returned
 * a constant 0). The unit is a single running-remainder register CRCDOR fed one
 * byte at a time through CRCDIR; this model reproduces that:
 *
 *  - **CRCDIR** (+0x04, write): each byte of the written value is folded into
 *    CRCDOR with the polynomial CRCCR0.GPS selects. A 32-bit access (CRC-32 /
 *    CRC-32C) folds four bytes LSB-first -- the order @c ra8_crc's word feed
 *    produces -- and an 8-bit access (CRC-8 / 16 / CCITT) folds one byte.
 *  - **CRCDOR** (+0x08, read/write): the running remainder. Writable so the
 *    driver can pre-seed it (CRC-32 / 32C seed 0xFFFFFFFF and XOR the readback
 *    in software; the hardware applies neither). CRCCR0.DORCLR clears it.
 *  - **CRCCR0** (+0x00): GPS[2:0] polynomial select, LMS bit-order, DORCLR.
 *
 * Reflection follows the RA8D2 polynomial definitions: CRC-32 / CRC-32C and the
 * reflected CRC-16 fold LSB-first; CRC-8 and CRC-CCITT fold MSB-first. Only the
 * CRC-32 path is exercised by an in-tree example (crc_demo, where it now reads
 * @c match=Y); the others use their standard parameters.
 *
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"

/** @brief Bit-width masks for the CRC unit model. */
typedef enum : uint32_t {
  k_u32_all   = 0xFFFFFFFFU, /**< 32-bit all-ones. */
  k_u16_mask  = 0xFFFFU,     /**< 16-bit mask.     */
  k_byte_mask = 0xFFU,       /**< 8-bit mask.      */
} crc_lit_t;

/** @brief CRC block geometry (ra8_crc_regs.h). */
typedef enum : uint64_t {
  k_crc_base    = 0x40310000UL, /**< CRC base (HUM Ch 48).        */
  k_crc_span    = 0x20UL,       /**< Register window.             */
  k_crc_off_cr0 = 0x00UL,       /**< CRCCR0 (GPS/LMS/DORCLR), 8b. */
  k_crc_off_cr1 = 0x01UL,       /**< CRCCR1 (snoop), 8b.          */
  k_crc_off_dir = 0x04UL,       /**< CRCDIR / CRCDIR_BY data in.  */
  k_crc_off_dor = 0x08UL,       /**< CRCDOR result (32b).         */
} crc_map_t;

/** @brief CRCCR0 fields (ra8_crc_regs.h). */
typedef enum : uint32_t {
  k_crc_gps_mask = 0x07U, /**< GPS[2:0] polynomial select.   */
  k_crc_dorclr   = 0x80U, /**< DORCLR: clear CRCDOR (bit 7). */
} crc_cr0_field_t;

/** @brief GPS polynomial encodings (CRCCR0.GPS). */
typedef enum : uint32_t {
  k_crc_gps_8     = 1U, /**< CRC-8  (X^8+X^2+X+1).        */
  k_crc_gps_16    = 2U, /**< CRC-16 (X^16+X^15+X^2+1).    */
  k_crc_gps_ccitt = 3U, /**< CRC-CCITT (X^16+X^12+X^5+1). */
  k_crc_gps_32    = 4U, /**< CRC-32 (IEEE 802.3).         */
  k_crc_gps_32c   = 5U, /**< CRC-32C (Castagnoli).        */
} crc_gps_t;

/** @brief Reflected (LSB-first) polynomials for the reflected GPS modes. */
typedef enum : uint32_t {
  k_crc_poly_16_refl  = 0x0000A001U, /**< Reflected 0x8005.     */
  k_crc_poly_32_refl  = 0xEDB88320U, /**< Reflected 0x04C11DB7. */
  k_crc_poly_32c_refl = 0x82F63B78U, /**< Reflected 0x1EDC6F41. */
} crc_poly_refl_t;

/** @brief MSB-first polynomials for the non-reflected GPS modes. */
typedef enum : uint32_t {
  k_crc_poly_8_fwd     = 0x00000007U, /**< CRC-8 X^8+X^2+X+1. */
  k_crc_poly_ccitt_fwd = 0x00001021U, /**< CRC-CCITT.         */
} crc_poly_fwd_t;

/** @brief Per-tick order slot for the CRC block (relative order only). */
typedef enum : uint32_t {
  k_crc_block_order = 140U, /**< After the existing blocks; report order. */
} crc_order_t;

/** @brief CRC register state. */
typedef struct {
  uint8_t  cr0;   /**< CRCCR0 shadow (GPS/LMS/DORCLR). */
  uint8_t  cr1;   /**< CRCCR1 shadow.                  */
  uint32_t dor;   /**< CRCDOR running remainder.       */
  uint32_t bytes; /**< Bytes folded since reset.       */
} crc_state_t;

static crc_state_t s_crc;

/** @brief Fold one byte into a reflected (LSB-first) CRC of any width. */
static uint32_t crc_step_reflected(uint32_t crc, uint8_t byte, uint32_t poly)
{
  crc ^= (uint32_t)byte;
  for (uint32_t i = 0U; i < 8U; i++) {
    crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ poly) : (crc >> 1);
  }
  return crc;
}

/** @brief Fold one byte into an MSB-first CRC of @p width bits. */
static uint32_t crc_step_forward(uint32_t crc, uint8_t byte, uint32_t poly, uint32_t width)
{
  const uint32_t top = 1U << (width - 1U);
  const uint32_t msk = (width >= 32U) ? (uint32_t)k_u32_all : ((1U << width) - 1U);
  crc ^= (uint32_t)byte << (width - 8U);
  for (uint32_t i = 0U; i < 8U; i++) {
    const bool set = (crc & top) != 0U;
    crc            = (crc << 1) & msk;
    if (set) {
      crc ^= poly;
    }
  }
  return crc & msk;
}

/** @brief Fold one input byte into CRCDOR using the CRCCR0.GPS polynomial. */
static void crc_fold_byte(uint8_t byte)
{
  switch ((uint32_t)(s_crc.cr0 & (uint8_t)k_crc_gps_mask)) {
    case (uint32_t)k_crc_gps_8:
      s_crc.dor = crc_step_forward(s_crc.dor, byte, (uint32_t)k_crc_poly_8_fwd, 8U);
      break;
    case (uint32_t)k_crc_gps_16:
      s_crc.dor = crc_step_reflected(s_crc.dor, byte, (uint32_t)k_crc_poly_16_refl);
      break;
    case (uint32_t)k_crc_gps_ccitt:
      s_crc.dor = crc_step_forward(s_crc.dor, byte, (uint32_t)k_crc_poly_ccitt_fwd, 16U);
      break;
    case (uint32_t)k_crc_gps_32:
      s_crc.dor = crc_step_reflected(s_crc.dor, byte, (uint32_t)k_crc_poly_32_refl);
      break;
    case (uint32_t)k_crc_gps_32c:
      s_crc.dor = crc_step_reflected(s_crc.dor, byte, (uint32_t)k_crc_poly_32c_refl);
      break;
    default:
      break; /* GPS=0: no calculation. */
  }
  s_crc.bytes++;
}

/** @brief Reset the CRC unit to power-on state. */
static void crc_reset(void)
{
  s_crc.cr0   = 0U;
  s_crc.cr1   = 0U;
  s_crc.dor   = 0U;
  s_crc.bytes = 0U;
}

/** @brief MMIO read inside the CRC window. */
static uint64_t crc_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_crc_base;
  if (off == (uint64_t)k_crc_off_cr0) {
    return s_crc.cr0;
  }
  if (off == (uint64_t)k_crc_off_cr1) {
    return s_crc.cr1;
  }
  if ((off >= (uint64_t)k_crc_off_dor) && (off < (uint64_t)k_crc_off_dor + 4UL)) {
    /* CRCDOR and its 16/8-bit aliases share +0x08; serve the low bytes. */
    const uint32_t shift = (uint32_t)(off - (uint64_t)k_crc_off_dor) * 8U;
    return (uint64_t)(s_crc.dor >> shift);
  }
  return 0U;
}

/** @brief MMIO write inside the CRC window. */
static void crc_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  const uint64_t off = addr - (uint64_t)k_crc_base;
  if (off == (uint64_t)k_crc_off_cr0) {
    s_crc.cr0 = (uint8_t)value;
    if (((uint8_t)value & (uint8_t)k_crc_dorclr) != 0U) {
      s_crc.dor = 0U; /* DORCLR clears the running remainder. */
    }
    return;
  }
  if (off == (uint64_t)k_crc_off_cr1) {
    s_crc.cr1 = (uint8_t)value;
    return;
  }
  if (off == (uint64_t)k_crc_off_dir) {
    /* Fold each byte of the access LSB-first: a 32-bit word feed (CRC-32/32C)
     * delivers four bytes in the order ra8_crc's word packing produces, an
     * 8-bit feed (CRC-8/16/CCITT) delivers one. */
    unsigned n = size;
    if (size > 4U) {
      n = 4U;
    } else if (size == 0U) {
      n = 1U;
    }
    for (unsigned i = 0U; i < n; i++) {
      crc_fold_byte((uint8_t)((value >> (8U * i)) & (uint32_t)k_byte_mask));
    }
    return;
  }
  if ((off >= (uint64_t)k_crc_off_dor) && (off < (uint64_t)k_crc_off_dor + 4UL)) {
    /* Seed / overwrite CRCDOR (driver pre-seeds 0xFFFFFFFF for CRC-32/32C). */
    if (size >= 4U) {
      s_crc.dor = (uint32_t)value;
    } else {
      const uint32_t shift = (uint32_t)(off - (uint64_t)k_crc_off_dor) * 8U;
      const uint32_t mask  = ((size == 2U) ? (uint32_t)k_u16_mask : (uint32_t)k_byte_mask) << shift;
      s_crc.dor            = (s_crc.dor & ~mask) | (((uint32_t)value << shift) & mask);
    }
  }
}

/** @brief End-of-run CRC section: last result + bytes folded. */
static void crc_report(void)
{
  if (s_crc.bytes == 0U) {
    return; /* Untouched: stay quiet. */
  }
  (void)fprintf(stderr,
                "  CRC           : GPS=%u CRCDOR=0x%08X bytes=%u\n",
                (unsigned)(s_crc.cr0 & (uint8_t)k_crc_gps_mask),
                s_crc.dor,
                s_crc.bytes);
}

/** @brief CRC block descriptor (self-registered with the core). */
static const board_periph_block_t k_crc_block = {
  .base   = (uint32_t)k_crc_base,
  .span   = (uint32_t)k_crc_span,
  .order  = (uint32_t)k_crc_block_order,
  .read   = crc_read,
  .write  = crc_write,
  .tick   = nullptr,
  .reset  = crc_reset,
  .report = crc_report,
  .name   = "CRC",
};

/** @brief Register the CRC block before main (host constructor). */
[[gnu::constructor]] static void crc_block_register(void)
{
  board_periph_register_block(&k_crc_block);
}
