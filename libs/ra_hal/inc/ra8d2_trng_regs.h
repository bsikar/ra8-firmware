/**
 * @file ra8d2_trng_regs.h
 * @brief True Random Number Generator (TRNG) register layout for the RA8D2
 *
 * @details
 * The RA8D2 exposes a hardware TRNG whose output can be read 32 bits
 * at a time from the `TRNGDO` register. A status register reports
 * when a fresh word is ready, and a control register arms the
 * collector. The block lives at base `0x40312000`.
 *
 * Register map:
 *
 * | Offset | Name    | Width | Purpose                              |
 * |-------:|---------|------:|--------------------------------------|
 * |  0x00  | TRNGCR  |     8 | Control (enable, reseed)             |
 * |  0x04  | TRNGSR  |     8 | Status (data-ready, busy, fault)     |
 * |  0x08  | TRNGDO  |    32 | Output sample (32 random bits)       |
 * |  0x0C  | TRNGSEED|    32 | Reseed / entropy-source write        |
 *
 * TODO: verify against HUM section 49 (TRNG) -- offsets are best
 * effort based on the FSP `r_rm_rnga.h` / `r_sce_trng.h` shims.
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
 * @enum ra_trng_addr_t
 * @brief Memory-mapped base address for TRNG.
 */
typedef enum : uintptr_t {
  k_ra_trng_base_addr = 0x40312000UL, /**< TRNG peripheral base. */
} ra_trng_addr_t;

/**
 * @enum ra_trngcr_bit_t
 * @brief TRNGCR control-bit positions.
 */
typedef enum : uint8_t {
  k_ra_trng_bit_en     = 0U, /**< 1 = enable the collector.          */
  k_ra_trng_bit_reseed = 1U, /**< Write 1 to reseed the LFSR.        */
  k_ra_trng_bit_ie     = 2U, /**< Interrupt enable on data-ready.    */
} ra_trngcr_bit_t;

/**
 * @enum ra_trng_mask_t
 * @brief TRNGCR bit masks.
 */
typedef enum : uint8_t {
  k_ra_trng_mask_en     = 0x01U, /**< EN bit.     */
  k_ra_trng_mask_reseed = 0x02U, /**< RESEED bit. */
  k_ra_trng_mask_ie     = 0x04U, /**< IE bit.     */
} ra_trng_mask_t;

/**
 * @enum ra_trngsr_bit_t
 * @brief TRNGSR status-bit positions.
 */
typedef enum : uint8_t {
  k_ra_trng_bit_drdy  = 0U, /**< Data-ready (1 = TRNGDO is valid).  */
  k_ra_trng_bit_busy  = 1U, /**< Collector busy.                    */
  k_ra_trng_bit_fault = 2U, /**< Fault flag (noise too low).        */
} ra_trngsr_bit_t;

/**
 * @enum ra_trngsr_mask_t
 * @brief TRNGSR bit masks.
 */
typedef enum : uint8_t {
  k_ra_trng_mask_drdy  = 0x01U, /**< DRDY bit.  */
  k_ra_trng_mask_busy  = 0x02U, /**< BUSY bit.  */
  k_ra_trng_mask_fault = 0x04U, /**< FAULT bit. */
} ra_trngsr_mask_t;

/**
 * @struct r_trng_regs_t
 * @brief TRNG register window.
 */
typedef struct {
  volatile uint8_t  TRNGCR;   /**< +0x00 Control register.           */
  volatile uint8_t  _r0[3];   /**< Padding.                          */
  volatile uint8_t  TRNGSR;   /**< +0x04 Status register.            */
  volatile uint8_t  _r1[3];   /**< Padding.                          */
  volatile uint32_t TRNGDO;   /**< +0x08 32-bit random output.       */
  volatile uint32_t TRNGSEED; /**< +0x0C Reseed value.               */
} r_trng_regs_t;

/**
 * @brief Get pointer to the TRNG register block.
 */
static inline volatile r_trng_regs_t* ra_trng(void)
{
  return (volatile r_trng_regs_t*)k_ra_trng_base_addr;
}

#ifdef __cplusplus
}
#endif
