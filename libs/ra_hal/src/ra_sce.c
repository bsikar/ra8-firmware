/**
 * @file ra_sce.c
 * @brief Secure Cryptographic Engine (SCE) driver implementation
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sce.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8d2_sce_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "SCE";

/**
 * @enum ra_sce_local_t
 * @brief File-local constants used by the driver.
 */
typedef enum : uint8_t {
  k_ra_sce_word_bytes = 4U, /**< Bytes per 32-bit key/block word.   */
  k_ra_sce_word_count = 4U, /**< 4 words in a 128-bit block / key.  */
} ra_sce_local_t;

/**
 * @enum ra_sce_shift_t
 * @brief Byte-packing shift amounts for 32-bit words.
 */
typedef enum : uint8_t {
  k_ra_sce_shift_b0 = 0U,  /**< Byte 0 shift amount. */
  k_ra_sce_shift_b1 = 8U,  /**< Byte 1 shift amount. */
  k_ra_sce_shift_b2 = 16U, /**< Byte 2 shift amount. */
  k_ra_sce_shift_b3 = 24U, /**< Byte 3 shift amount. */
} ra_sce_shift_t;

/**
 * @enum ra_sce_mask_byte_t
 * @brief Byte-masking constants.
 */
typedef enum : uint32_t {
  k_ra_sce_mask_byte = 0xFFUL, /**< Low-byte mask. */
} ra_sce_mask_byte_t;

/**
 * @brief Pack four little-endian bytes into a 32-bit word.
 *
 * @param[in] src Pointer to the first of four bytes.
 * @return `(src[0]) | (src[1]<<8) | (src[2]<<16) | (src[3]<<24)`.
 */
static inline uint32_t internal_ra_sce_pack_word(const uint8_t* src)
{
  return ((uint32_t)src[0] << (uint32_t)k_ra_sce_shift_b0) |
         ((uint32_t)src[1] << (uint32_t)k_ra_sce_shift_b1) |
         ((uint32_t)src[2] << (uint32_t)k_ra_sce_shift_b2) |
         ((uint32_t)src[3] << (uint32_t)k_ra_sce_shift_b3);
}

/**
 * @brief Load a 16-byte buffer into a 4-word register window.
 *
 * @param[out] dst   Destination array of four volatile 32-bit words.
 * @param[in]  bytes Source buffer of 16 bytes.
 */
static inline void internal_ra_sce_load_block(volatile uint32_t* dst, const uint8_t* bytes)
{
  for (size_t i = 0U; i < (size_t)k_ra_sce_word_count; ++i) {
    dst[i] = internal_ra_sce_pack_word(&bytes[i * (size_t)k_ra_sce_word_bytes]);
  }
}

/**
 * @brief Unpack a 32-bit word into four little-endian bytes.
 *
 * @param[out] dst Destination 4-byte buffer.
 * @param[in]  w   Source 32-bit word.
 */
static inline void internal_ra_sce_unpack_word(uint8_t* dst, uint32_t w)
{
  dst[0] = (uint8_t)((w >> (uint32_t)k_ra_sce_shift_b0) & (uint32_t)k_ra_sce_mask_byte);
  dst[1] = (uint8_t)((w >> (uint32_t)k_ra_sce_shift_b1) & (uint32_t)k_ra_sce_mask_byte);
  dst[2] = (uint8_t)((w >> (uint32_t)k_ra_sce_shift_b2) & (uint32_t)k_ra_sce_mask_byte);
  dst[3] = (uint8_t)((w >> (uint32_t)k_ra_sce_shift_b3) & (uint32_t)k_ra_sce_mask_byte);
}

[[nodiscard]] ra_err_t ra_sce_init(void)
{
  /* SCE and TRNG share the RSIP block (MSTPC31).
   * HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_rsip);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "sce_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_sce_regs_t* reg = ra_sce();
  reg->CR                    = 0UL;
  reg->SR                    = 0UL;
  reg->CMD                   = (uint32_t)k_ra_sce_cmd_none;
  reg->CR                    = (uint32_t)k_ra_sce_mask_en;
  ra_log_info(s_tag, "sce_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t
ra_sce_aes128_encrypt(const uint8_t* key, const uint8_t* plaintext, uint8_t* ciphertext)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(plaintext, s_tag, "plaintext must not be nullptr");
  RA_CHECK_NULL_PTR(ciphertext, s_tag, "ciphertext must not be nullptr");

  volatile r_sce_regs_t* reg = ra_sce();
  internal_ra_sce_load_block(reg->KEY, key);
  internal_ra_sce_load_block(reg->DIN, plaintext);
  reg->CMD = (uint32_t)k_ra_sce_cmd_aes128_encrypt;
  reg->CR  = (uint32_t)(k_ra_sce_mask_en | k_ra_sce_mask_start);

#ifdef RA_SIMULATOR_MODE
  /* Simulator: no real crypto core is available, just mirror the
   * plaintext into DOUT so the unpack path runs unmodified and
   * callers observe the same buffer.
   */
  for (size_t i = 0U; i < (size_t)k_ra_sce_word_count; ++i) {
    reg->DOUT[i] = internal_ra_sce_pack_word(&plaintext[i * (size_t)k_ra_sce_word_bytes]);
  }
  reg->SR = (uint32_t)k_ra_sce_mask_done;
#endif
  for (size_t i = 0U; i < (size_t)k_ra_sce_word_count; ++i) {
    internal_ra_sce_unpack_word(&ciphertext[i * (size_t)k_ra_sce_word_bytes], reg->DOUT[i]);
  }
  return k_ra_ok;
}
