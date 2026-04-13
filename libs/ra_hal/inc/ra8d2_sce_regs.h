/**
 * @file ra8d2_sce_regs.h
 * @brief Secure Cryptographic Engine (SCE) register layout for the RA8D2
 *
 * @details
 * The RA8D2 includes an SCE block (SCE9 / RSIP variant) that provides
 * hardware AES, SHA, and ECC primitives. The full register surface is
 * huge; this header models only the handful of registers we need for
 * bringup -- command register, status register, input / output FIFOs,
 * and an AES key window.
 *
 * Block base address is `0x400C0000`. Drivers should always go through
 * `ra_sce()` rather than poking raw offsets.
 *
 * Register offsets tracked against HUM Ch 52 "Renesas Secure IP
 * (RSIP-E50D)" p 3302 -- the RA8D2 SCE block is part of the
 * RSIP-E50D surface the HUM calls out.
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
 * @enum ra_sce_addr_t
 * @brief Memory-mapped base address for SCE.
 */
typedef enum : uintptr_t {
  k_ra_sce_base_addr = 0x400C0000UL, /**< SCE peripheral base. */
} ra_sce_addr_t;

/**
 * @enum ra_sce_limits_t
 * @brief Sizes and counts.
 */
typedef enum : uint16_t {
  k_ra_sce_aes128_key_bytes   = 16U, /**< AES-128 key size.  */
  k_ra_sce_aes128_block_bytes = 16U, /**< AES block size.    */
} ra_sce_limits_t;

/**
 * @enum ra_scecr_bit_t
 * @brief SCECR control-bit positions.
 */
typedef enum : uint8_t {
  k_ra_sce_bit_en    = 0U, /**< 1 = enable the SCE clock / block.   */
  k_ra_sce_bit_start = 1U, /**< Write 1 to kick off a command.      */
  k_ra_sce_bit_reset = 2U, /**< Write 1 to reset engine state.      */
  k_ra_sce_bit_irqen = 3U, /**< Interrupt enable on completion.     */
} ra_scecr_bit_t;

/**
 * @enum ra_sce_mask_t
 * @brief SCECR bit masks.
 */
typedef enum : uint32_t {
  k_ra_sce_mask_en    = 0x00000001UL, /**< EN bit.    */
  k_ra_sce_mask_start = 0x00000002UL, /**< START bit. */
  k_ra_sce_mask_reset = 0x00000004UL, /**< RESET bit. */
  k_ra_sce_mask_done  = 0x00000001UL, /**< DONE status bit. */
  k_ra_sce_mask_error = 0x00000002UL, /**< ERROR status bit. */
} ra_sce_mask_t;

/**
 * @enum ra_sce_cmd_t
 * @brief Values written to `SCE_CMD` to select an operation.
 */
typedef enum : uint32_t {
  k_ra_sce_cmd_none           = 0UL, /**< Idle.                    */
  k_ra_sce_cmd_aes128_encrypt = 1UL, /**< AES-128 ECB encrypt.     */
  k_ra_sce_cmd_aes128_decrypt = 2UL, /**< AES-128 ECB decrypt.     */
  k_ra_sce_cmd_sha256         = 3UL, /**< SHA-256 digest.          */
} ra_sce_cmd_t;

/**
 * @struct r_sce_regs_t
 * @brief SCE register window (minimal stub).
 *
 * @details
 * Only the registers the bringup driver touches are modeled. The
 * real block has dozens more -- add them when the first consumer
 * needs them.
 */
typedef struct {
  volatile uint32_t CR;  /**< +0x00 Control register.   */
  volatile uint32_t SR;  /**< +0x04 Status register.    */
  volatile uint32_t CMD; /**< +0x08 Command register.   */
  volatile uint32_t _r0; /**< +0x0C Reserved.           */
  volatile uint32_t KEY[k_ra_sce_aes128_key_bytes / sizeof(uint32_t)];    /**< +0x10 AES key.  */
  volatile uint32_t DIN[k_ra_sce_aes128_block_bytes / sizeof(uint32_t)];  /**< +0x20 Block in. */
  volatile uint32_t DOUT[k_ra_sce_aes128_block_bytes / sizeof(uint32_t)]; /**< +0x30 Block out.*/
} r_sce_regs_t;

/**
 * @brief Get pointer to the SCE register block.
 */
static inline volatile r_sce_regs_t* ra_sce(void)
{
  return (volatile r_sce_regs_t*)k_ra_sce_base_addr;
}

#ifdef __cplusplus
}
#endif
