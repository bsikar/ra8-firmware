/**
 * @file ra8_bit_constants.h
 * @brief Named Bit Positions and Masks for Register Manipulation
 * @ingroup grp_core
 *
 * @details
 * Central home for bit-position constants used when poking hardware
 * registers. Every bit shift and mask in the firmware should be
 * expressed in terms of one of these enums rather than a raw integer
 * literal. The clang-tidy rule `readability-magic-numbers` enforces
 * this at commit time.
 *
 * ## Why this exists
 *
 * Without named constants, register writes look like:
 * @code{.c}
 * reg->CR |= (1U << 7) | (3U << 3);  // what bit? why 3?
 * @endcode
 *
 * With this header, they look like:
 * @code{.c}
 * reg->CR |= (k_ra8_bit_1 << k_ra8_bit_7) | (k_ra8_mask_lsn2 << k_ra8_bit_3);
 * @endcode
 *
 * The second version is searchable (`grep k_ra8_bit_7` tells you every
 * place bit 7 is touched), self-documenting, and compile-time type-safe.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =============================================================================
 * Bit positions (uint8_t)
 * =============================================================================
 */

/**
 * @enum ra8_bit_position_t
 * @brief Bit positions 0..31 as typed constants.
 *
 * @details
 * Use these with the shift operator to construct masks:
 * @code{.c}
 * uint32_t mask = (1U << k_ra8_bit_5);  // 0x20
 * @endcode
 *
 * Every position is a distinct enumerator so the compiler's switch
 * analysis and clang-tidy's naming rules both see them.
 */
typedef enum : uint8_t {
  k_ra8_bit_0  = 0U,  /**< RA8 bit 0.  */
  k_ra8_bit_1  = 1U,  /**< RA8 bit 1.  */
  k_ra8_bit_2  = 2U,  /**< RA8 bit 2.  */
  k_ra8_bit_3  = 3U,  /**< RA8 bit 3.  */
  k_ra8_bit_4  = 4U,  /**< RA8 bit 4.  */
  k_ra8_bit_5  = 5U,  /**< RA8 bit 5.  */
  k_ra8_bit_6  = 6U,  /**< RA8 bit 6.  */
  k_ra8_bit_7  = 7U,  /**< RA8 bit 7.  */
  k_ra8_bit_8  = 8U,  /**< RA8 bit 8.  */
  k_ra8_bit_9  = 9U,  /**< RA8 bit 9.  */
  k_ra8_bit_10 = 10U, /**< RA8 bit 10. */
  k_ra8_bit_11 = 11U, /**< RA8 bit 11. */
  k_ra8_bit_12 = 12U, /**< RA8 bit 12. */
  k_ra8_bit_13 = 13U, /**< RA8 bit 13. */
  k_ra8_bit_14 = 14U, /**< RA8 bit 14. */
  k_ra8_bit_15 = 15U, /**< RA8 bit 15. */
  k_ra8_bit_16 = 16U, /**< RA8 bit 16. */
  k_ra8_bit_17 = 17U, /**< RA8 bit 17. */
  k_ra8_bit_18 = 18U, /**< RA8 bit 18. */
  k_ra8_bit_19 = 19U, /**< RA8 bit 19. */
  k_ra8_bit_20 = 20U, /**< RA8 bit 20. */
  k_ra8_bit_21 = 21U, /**< RA8 bit 21. */
  k_ra8_bit_22 = 22U, /**< RA8 bit 22. */
  k_ra8_bit_23 = 23U, /**< RA8 bit 23. */
  k_ra8_bit_24 = 24U, /**< RA8 bit 24. */
  k_ra8_bit_25 = 25U, /**< RA8 bit 25. */
  k_ra8_bit_26 = 26U, /**< RA8 bit 26. */
  k_ra8_bit_27 = 27U, /**< RA8 bit 27. */
  k_ra8_bit_28 = 28U, /**< RA8 bit 28. */
  k_ra8_bit_29 = 29U, /**< RA8 bit 29. */
  k_ra8_bit_30 = 30U, /**< RA8 bit 30. */
  k_ra8_bit_31 = 31U, /**< RA8 bit 31. */
} ra8_bit_position_t;

/* =============================================================================
 * Byte-level masks
 * =============================================================================
 */

/**
 * @enum ra8_byte_mask_t
 * @brief Byte-wide bit masks and positions for byte packing/unpacking.
 */
typedef enum : uint16_t {
  k_ra8_mask_nibble = 0x000FU, /**< Low 4 bits mask.         */
  k_ra8_mask_byte   = 0x00FFU, /**< Low 8 bits mask.         */
  k_ra8_mask_word   = 0xFFFFU, /**< Low 16 bits mask.        */
  k_ra8_mask_lsn2   = 0x0003U, /**< Low 2-bit field mask.    */
  k_ra8_mask_lsn3   = 0x0007U, /**< Low 3-bit field mask.    */
  k_ra8_mask_lsn4   = 0x000FU, /**< Low 4-bit field (alias). */
  k_ra8_mask_lsn5   = 0x001FU, /**< Low 5-bit field mask.    */
  k_ra8_mask_lsn6   = 0x003FU, /**< Low 6-bit field mask.    */
  k_ra8_mask_lsn7   = 0x007FU, /**< Low 7-bit field mask.    */
  k_ra8_mask_lsn8   = 0x00FFU, /**< Low 8-bit field (alias). */
} ra8_byte_mask_t;

/**
 * @enum ra8_byte_count_t
 * @brief Bit counts per integral type.
 */
typedef enum : uint8_t {
  k_ra8_bits_per_byte = 8U,  /**< Bits in a byte.       */
  k_ra8_bits_per_u16  = 16U, /**< Bits in a `uint16_t`. */
  k_ra8_bits_per_u32  = 32U, /**< Bits in a `uint32_t`. */
  k_ra8_bits_per_u64  = 64U, /**< Bits in a `uint64_t`. */
} ra8_byte_count_t;

/* =============================================================================
 * Byte-index helpers for MSB-first packing
 * =============================================================================
 */

/**
 * @enum ra8_be_byte_idx_t
 * @brief Byte indices when packing big-endian multi-byte values.
 *
 * @details
 * Use in place of raw literals when splitting a `uint16_t` or `uint32_t`
 * into a byte buffer. Keeps code self-documenting and greppable.
 *
 * @code{.c}
 * buf[k_ra8_be_idx_hi16] = (uint8_t)(val >> k_ra8_bit_8);
 * buf[k_ra8_be_idx_lo16] = (uint8_t)(val & k_ra8_mask_byte);
 * @endcode
 */
typedef enum : uint8_t {
  k_ra8_be_idx_hi32 = 0U, /**< Most significant byte of a 32-bit big-endian value. */
  k_ra8_be_idx_mh32 = 1U, /**< Mid-high byte.                                      */
  k_ra8_be_idx_ml32 = 2U, /**< Mid-low byte.                                       */
  k_ra8_be_idx_lo32 = 3U, /**< Least significant byte.                             */
  k_ra8_be_idx_hi16 = 0U, /**< Most significant byte of a 16-bit big-endian value. */
  k_ra8_be_idx_lo16 = 1U, /**< Least significant byte.                             */
} ra8_be_byte_idx_t;

#ifdef __cplusplus
}
#endif
