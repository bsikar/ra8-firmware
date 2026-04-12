/**
 * @file ra8d2_mstp_regs.h
 * @brief Module Stop Control (MSTP) register layout for the Renesas RA8D2
 *
 * @details
 * Every RA-family peripheral has a Module Stop bit. When that bit is
 * `1` the peripheral is clock-gated off and does not respond to bus
 * accesses. `Module Stop Control Registers A..E` (MSTPCRA..MSTPCRE)
 * at `0x40203000` hold 32 bits each, one bit per peripheral.
 *
 * The exact MSTP bit for each peripheral is documented in Table 11.1
 * ("Module Stop List") of the RA8D2 Hardware User's Manual. We encode
 * the (register, bit) pair as a packed `ra_mstp_t` value so callers
 * can write:
 *
 * @code{.c}
 * ra_mstp_enable(k_ra_mstp_sci0);   // turns on SCI0 clock
 * ra_mstp_enable(k_ra_mstp_ioport); // turns on IOPORT clock
 * @endcode
 *
 * instead of remembering which register owns which bit.
 *
 * ## Encoding
 *
 * `ra_mstp_t` is a 16-bit value packed as `(reg << 8) | bit`:
 *  - `reg` 0..4 selects MSTPCRA..MSTPCRE.
 *  - `bit` 0..31 selects the bit within the register.
 *
 * This matches the FSP `bsp_peripheral.h` convention and keeps the
 * list searchable in the Hardware User's Manual by name.
 *
 * ## Write protection
 *
 * MSTPCR registers live in the SYSC block. They are NOT protected by
 * PRCR; writes are permitted directly (unlike CGC registers which
 * require the PRCR unlock sequence). Only IOPORT, SYSC, and CPU-core
 * features are always enabled regardless of MSTPCR state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_bit_constants.h"

/* =============================================================================
 * Base / layout
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra_mstp_base_addr = 0x40203000UL, /**< R_MSTP block base.                */
} ra_mstp_addr_t;

/**
 * @struct r_mstp_regs_t
 * @brief Memory layout of the Module Stop Control Register block.
 *
 * @details
 * 32-bit registers at offset 0x00 / 0x04 / 0x08 / 0x0C / 0x10.
 */
typedef struct {
  volatile uint32_t MSTPCRA; /**< Module Stop Control Register A (+0x00). */
  volatile uint32_t MSTPCRB; /**< Module Stop Control Register B (+0x04). */
  volatile uint32_t MSTPCRC; /**< Module Stop Control Register C (+0x08). */
  volatile uint32_t MSTPCRD; /**< Module Stop Control Register D (+0x0C). */
  volatile uint32_t MSTPCRE; /**< Module Stop Control Register E (+0x10). */
} r_mstp_regs_t;

/** @brief Expected byte size of the MSTP register block. */
typedef enum : uint8_t {
  k_ra_mstp_block_bytes = 20U, /**< 5 x uint32_t = 20 bytes. */
} ra_mstp_block_size_t;

static_assert(sizeof(r_mstp_regs_t) == (size_t)k_ra_mstp_block_bytes,
              "R_MSTP struct must be 20 bytes");

/** @brief Get pointer to the MSTP register block. */
static inline volatile r_mstp_regs_t* ra_mstp(void)
{
  return (volatile r_mstp_regs_t*)k_ra_mstp_base_addr;
}

/* =============================================================================
 * Packed (register, bit) identifier
 * =============================================================================
 */

/**
 * @enum ra_mstp_reg_t
 * @brief Which of MSTPCRA..MSTPCRE owns the bit.
 */
typedef enum : uint8_t {
  k_ra_mstp_reg_a = 0U,
  k_ra_mstp_reg_b = 1U,
  k_ra_mstp_reg_c = 2U,
  k_ra_mstp_reg_d = 3U,
  k_ra_mstp_reg_e = 4U,
} ra_mstp_reg_t;

/**
 * @enum ra_mstp_t
 * @brief Packed `(reg << 8) | bit` module-stop identifier.
 *
 * @details
 * The values below are the ones this firmware actually drives so far;
 * extend as new peripherals come online. Each entry cites the register
 * and bit from the RA8D2 Hardware User's Manual Table 11.1.
 *
 * @note Bit numbering in the manual is already 0-based from the LSB,
 *       matching what the 32-bit registers expect.
 */
typedef enum : uint16_t {
  /* MSTPCRB: SCI channels 0..9 live here (bits 31..22). */
  k_ra_mstp_sci0 = (uint16_t)((k_ra_mstp_reg_b << 8) | 31U),
  k_ra_mstp_sci1 = (uint16_t)((k_ra_mstp_reg_b << 8) | 30U),
  k_ra_mstp_sci2 = (uint16_t)((k_ra_mstp_reg_b << 8) | 29U),
  k_ra_mstp_sci3 = (uint16_t)((k_ra_mstp_reg_b << 8) | 28U),
  k_ra_mstp_sci4 = (uint16_t)((k_ra_mstp_reg_b << 8) | 27U),
  k_ra_mstp_sci5 = (uint16_t)((k_ra_mstp_reg_b << 8) | 26U),
  k_ra_mstp_sci6 = (uint16_t)((k_ra_mstp_reg_b << 8) | 25U),
  k_ra_mstp_sci7 = (uint16_t)((k_ra_mstp_reg_b << 8) | 24U),
  k_ra_mstp_sci8 = (uint16_t)((k_ra_mstp_reg_b << 8) | 23U),
  k_ra_mstp_sci9 = (uint16_t)((k_ra_mstp_reg_b << 8) | 22U),

  /* MSTPCRC: CRC is bit 1. */
  k_ra_mstp_crc = (uint16_t)((k_ra_mstp_reg_c << 8) | 1U),
} ra_mstp_t;

/**
 * @brief Extract the register index from a packed MSTP id.
 * @param id Packed id.
 * @return Register index 0..4.
 */
static inline ra_mstp_reg_t ra_mstp_id_reg(ra_mstp_t id)
{
  return (ra_mstp_reg_t)(((uint16_t)id >> k_ra_bits_per_byte) & k_ra_mask_byte);
}

/**
 * @brief Extract the bit number from a packed MSTP id.
 * @param id Packed id.
 * @return Bit position 0..31.
 */
static inline uint8_t ra_mstp_id_bit(ra_mstp_t id)
{
  return (uint8_t)((uint16_t)id & k_ra_mask_byte);
}

/* =============================================================================
 * Enable / disable helpers
 * =============================================================================
 */

/**
 * @brief Enable a peripheral (clear its MSTP bit).
 *
 * @param[in] id Packed `ra_mstp_t` value.
 *
 * @note Not thread-safe: reads, modifies, writes. Protect with IRQ
 *       masking or call only from single-threaded init.
 * @note Writes take effect one cycle after the bus write; code that
 *       touches the peripheral immediately after should insert a
 *       `__DSB()` / `__ISB()` pair or an `asm volatile ("" : : : "memory")`.
 */
static inline void ra_mstp_enable(ra_mstp_t id)
{
  const uint8_t     reg  = (uint8_t)ra_mstp_id_reg(id);
  const uint8_t     bit  = ra_mstp_id_bit(id);
  volatile uint32_t* base = &ra_mstp()->MSTPCRA;
  base[reg] &= ~(1UL << bit);
}

/**
 * @brief Disable a peripheral (set its MSTP bit).
 *
 * @param[in] id Packed `ra_mstp_t` value.
 *
 * @note Same threading caveats as `ra_mstp_enable()`.
 */
static inline void ra_mstp_disable(ra_mstp_t id)
{
  const uint8_t     reg  = (uint8_t)ra_mstp_id_reg(id);
  const uint8_t     bit  = ra_mstp_id_bit(id);
  volatile uint32_t* base = &ra_mstp()->MSTPCRA;
  base[reg] |= (1UL << bit);
}

#ifdef __cplusplus
}
#endif
