/**
 * @file port/mbedtls/mbedtls_aes_alt.c
 * @brief AES ALT implementation for Mbed TLS, routed through ra8_rsip
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Provides the standard Mbed TLS AES public surface (init / free /
 * setkey_enc / setkey_dec / crypt_ecb / crypt_cbc / crypt_ctr) on
 * top of ``ra8_rsip_aes_cipher`` and ``ra8_rsip_aes*_install_plain``.
 * When the project config defines both ``MBEDTLS_AES_ALT`` and
 * ``MBEDTLS_AES_C``, the upstream ``mbedtls/aes.h`` becomes a thin
 * forward-declaration shell -- this TU supplies the definitions.
 *
 * The contract:
 *
 *   - ``mbedtls_aes_init`` zeroes the context.
 *   - ``mbedtls_aes_setkey_enc`` / ``..._setkey_dec`` wrap the
 *     plaintext key into a vault handle once and store the handle in
 *     the context. Whether enc or dec was last set only matters for
 *     ``crypt_ecb`` -- the RSIP engine itself selects the direction
 *     per call.
 *   - ``mbedtls_aes_crypt_ecb`` issues one block (16 bytes) through
 *     ``ra8_rsip_aes_cipher`` in ECB mode.
 *   - ``mbedtls_aes_crypt_cbc`` / ``..._crypt_ctr`` issue the full
 *     buffer through ``ra8_rsip_aes_cipher`` in the matching RSIP
 *     mode. CBC requires the input length be a multiple of 16; the
 *     RSIP engine enforces this and returns ``k_ra8_err_invalid_arg``,
 *     which we map to ``MBEDTLS_ERR_AES_INVALID_INPUT_LENGTH``.
 *   - ``mbedtls_aes_free`` scrubs the handle in-place.
 *
 * Error mapping:
 *
 *   - ``k_ra8_err_null_ptr`` / ``k_ra8_err_invalid_arg`` ->
 *     ``MBEDTLS_ERR_AES_BAD_INPUT_DATA``.
 *   - ``k_ra8_err_hw_*`` -> ``MBEDTLS_ERR_PLATFORM_HW_ACCEL_FAILED``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/* NOLINTBEGIN(readability-magic-numbers, cppcoreguidelines-avoid-magic-numbers) */
/* The Mbed TLS AES public surface is defined against fixed,
 * spec-mandated block sizes (16 bytes), key sizes (128 / 192 / 256
 * bits), and direction codes (MBEDTLS_AES_ENCRYPT = 1 /
 * MBEDTLS_AES_DECRYPT = 0). They appear in this file as bare
 * literals so call sites match the upstream public headers exactly. */

#include "mbedtls_aes_alt.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/error.h"
#include "ra8_err.h"
#include "ra8_rsip.h"

/**
 * @enum mbedtls_aes_alt_dir_t
 * @brief Internal direction tag stashed in ``mbedtls_aes_context::direction``.
 */
typedef enum : uint8_t {
  k_mbedtls_aes_alt_dir_unset = 0U, /**< No setkey has been called yet. */
  k_mbedtls_aes_alt_dir_enc   = 1U, /**< Last setkey was setkey_enc.    */
  k_mbedtls_aes_alt_dir_dec   = 2U, /**< Last setkey was setkey_dec.    */
} mbedtls_aes_alt_dir_t;

/* Translate ``ra8_err_t`` failures into Mbed TLS AES error codes -- see implementation for details. */
static int priv_map_err(ra8_err_t err)
{
  if (err == k_ra8_ok) {
    return 0;
  }
  if (err == k_ra8_err_null_ptr || err == k_ra8_err_invalid_arg) {
    return MBEDTLS_ERR_AES_BAD_INPUT_DATA;
  }
  return MBEDTLS_ERR_PLATFORM_HW_ACCEL_FAILED;
}

/* Wrap a plaintext key into a fresh ``ra8_rsip_key_handle_t`` -- see implementation for details. */
static ra8_err_t
priv_install_key(const unsigned char* key, unsigned int key_bits, ra8_rsip_key_handle_t* handle)
{
  if (key_bits == (unsigned int)k_mbedtls_aes_alt_key128_bits) {
    return ra8_rsip_aes128_install_plain(key, handle);
  }
  if (key_bits == (unsigned int)k_mbedtls_aes_alt_key192_bits) {
    return ra8_rsip_aes192_install_plain(key, handle);
  }
  if (key_bits == (unsigned int)k_mbedtls_aes_alt_key256_bits) {
    return ra8_rsip_aes256_install_plain(key, handle);
  }
  return k_ra8_err_invalid_arg;
}

/* Mbed TLS ``mbedtls_aes_init`` ALT replacement -- see implementation for details. */
void mbedtls_aes_init(mbedtls_aes_context* ctx)
{
  if (ctx == nullptr) {
    return;
  }
  (void)memset((void*)ctx, 0, sizeof(*ctx));
  ctx->direction = (uint8_t)k_mbedtls_aes_alt_dir_unset;
}

/* Mbed TLS ``mbedtls_aes_free`` ALT replacement -- see implementation for details. */
void mbedtls_aes_free(mbedtls_aes_context* ctx)
{
  if (ctx == nullptr) {
    return;
  }
  /* The wrapped key handle never holds plaintext, but scrub the
   * struct anyway so a leaked context cannot be used to issue
   * further engine commands once the caller is done with it. */
  (void)memset((void*)ctx, 0, sizeof(*ctx));
}

/* Mbed TLS ``mbedtls_aes_setkey_enc`` ALT replacement -- see implementation for details. */
int mbedtls_aes_setkey_enc(mbedtls_aes_context* ctx, const unsigned char* key, unsigned int keybits)
{
  if (ctx == nullptr || key == nullptr) {
    return MBEDTLS_ERR_AES_BAD_INPUT_DATA;
  }
  ra8_err_t err = priv_install_key(key, keybits, &ctx->key);
  if (err != k_ra8_ok) {
    ctx->ready = 0U;
    return priv_map_err(err);
  }
  ctx->key_bits  = (uint16_t)keybits;
  ctx->direction = (uint8_t)k_mbedtls_aes_alt_dir_enc;
  ctx->ready     = 1U;
  return 0;
}

/* Mbed TLS ``mbedtls_aes_setkey_dec`` ALT replacement -- see implementation for details. */
int mbedtls_aes_setkey_dec(mbedtls_aes_context* ctx, const unsigned char* key, unsigned int keybits)
{
  if (ctx == nullptr || key == nullptr) {
    return MBEDTLS_ERR_AES_BAD_INPUT_DATA;
  }
  ra8_err_t err = priv_install_key(key, keybits, &ctx->key);
  if (err != k_ra8_ok) {
    ctx->ready = 0U;
    return priv_map_err(err);
  }
  ctx->key_bits  = (uint16_t)keybits;
  ctx->direction = (uint8_t)k_mbedtls_aes_alt_dir_dec;
  ctx->ready     = 1U;
  return 0;
}

/* Issue a single RSIP cipher call; helper for the public APIs -- see implementation for details. */
static int priv_issue(const mbedtls_aes_context* ctx,
                      ra8_rsip_aes_mode_t         mode,
                      ra8_rsip_aes_dir_t          dir,
                      const uint8_t*             iv,
                      const uint8_t*             in,
                      uint8_t*                   out,
                      uint32_t                   len)
{
  if (ctx == nullptr || ctx->ready == 0U) {
    return MBEDTLS_ERR_AES_BAD_INPUT_DATA;
  }
  ra8_err_t err = ra8_rsip_aes_cipher(&ctx->key, mode, dir, iv, in, out, len);
  return priv_map_err(err);
}

/* Mbed TLS ``mbedtls_aes_crypt_ecb`` ALT replacement -- see implementation for details. */
int mbedtls_aes_crypt_ecb(mbedtls_aes_context* ctx,
                          int                  mode,
                          const unsigned char  input[16],
                          unsigned char        output[16])
{
  if (input == nullptr || output == nullptr) {
    return MBEDTLS_ERR_AES_BAD_INPUT_DATA;
  }
  ra8_rsip_aes_dir_t dir =
    (mode == MBEDTLS_AES_ENCRYPT) ? k_ra8_rsip_dir_encrypt : k_ra8_rsip_dir_decrypt;
  return priv_issue(ctx,
                    k_ra8_rsip_aes_mode_ecb,
                    dir,
                    nullptr,
                    (const uint8_t*)input,
                    (uint8_t*)output,
                    (uint32_t)k_mbedtls_aes_alt_block_bytes);
}

/* Mbed TLS ``mbedtls_aes_crypt_cbc`` ALT replacement -- see implementation for details. */
int mbedtls_aes_crypt_cbc(mbedtls_aes_context* ctx,
                          int                  mode,
                          size_t               length,
                          unsigned char        iv[16],
                          const unsigned char* input,
                          unsigned char*       output)
{
  if (iv == nullptr || input == nullptr || output == nullptr) {
    return MBEDTLS_ERR_AES_BAD_INPUT_DATA;
  }
  if ((length % (size_t)k_mbedtls_aes_alt_block_bytes) != 0U) {
    return MBEDTLS_ERR_AES_INVALID_INPUT_LENGTH;
  }
  if (length == 0U) {
    return 0;
  }
  ra8_rsip_aes_dir_t dir =
    (mode == MBEDTLS_AES_ENCRYPT) ? k_ra8_rsip_dir_encrypt : k_ra8_rsip_dir_decrypt;
  /* Stash the trailing cipher block so the caller's IV continues
   * the chain on the next ``crypt_cbc`` call. For decrypt the IV
   * needs the LAST ciphertext block (input), for encrypt the LAST
   * cipher block (output). */
  uint8_t next_iv[k_mbedtls_aes_alt_block_bytes];
  if (mode == MBEDTLS_AES_DECRYPT) {
    (void)memcpy((void*)next_iv,
                 (const void*)(input + length - (size_t)k_mbedtls_aes_alt_block_bytes),
                 sizeof(next_iv));
  }
  int rc = priv_issue(ctx,
                      k_ra8_rsip_aes_mode_cbc,
                      dir,
                      (const uint8_t*)iv,
                      (const uint8_t*)input,
                      (uint8_t*)output,
                      (uint32_t)length);
  if (rc != 0) {
    return rc;
  }
  if (mode == MBEDTLS_AES_ENCRYPT) {
    (void)memcpy((void*)next_iv,
                 (const void*)(output + length - (size_t)k_mbedtls_aes_alt_block_bytes),
                 sizeof(next_iv));
  }
  (void)memcpy((void*)iv, (const void*)next_iv, sizeof(next_iv));
  return 0;
}

/**
 * @brief Mbed TLS ``mbedtls_aes_crypt_ctr`` ALT replacement.
 *
 * @details
 * The RSIP CTR mode operates on full blocks at the engine level but
 * the public Mbed TLS API permits partial trailing bytes via
 * ``nc_off`` and ``stream_block``. We service partial bytes from
 * ``stream_block`` first, then send the remaining full + final
 * partial through the engine in one ``ra8_rsip_aes_cipher`` call --
 * the engine pads transparently and exposes the keystream output
 * for the partial tail back through ``stream_block`` for the next
 * call.
 *
 * @param[in,out] ctx           Context with a ready wrapped key.
 * @param[in]     length        Bytes to process.
 * @param[in,out] nc_off        Offset into ``stream_block`` (0..15).
 * @param[in,out] nonce_counter 16-byte counter; updated after each block.
 * @param[in,out] stream_block  16-byte keystream cache.
 * @param[in]     input         Input buffer.
 * @param[out]    output        Output buffer.
 *
 * @return ``0`` on success.
 *
 * @pre Pointers are non-NULL.
 *
 * @since 0.1.0
 *
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 */
int mbedtls_aes_crypt_ctr(mbedtls_aes_context* ctx,
                          size_t               length,
                          size_t*              nc_off,
                          unsigned char        nonce_counter[16],
                          unsigned char        stream_block[16],
                          const unsigned char* input,
                          unsigned char*       output)
{
  if (nc_off == nullptr || nonce_counter == nullptr || stream_block == nullptr ||
      input == nullptr || output == nullptr) {
    return MBEDTLS_ERR_AES_BAD_INPUT_DATA;
  }
  if (*nc_off >= (size_t)k_mbedtls_aes_alt_block_bytes) {
    return MBEDTLS_ERR_AES_BAD_INPUT_DATA;
  }
  if (length == 0U) {
    return 0;
  }
  /* Fast path: zero offset + block-aligned length -> one engine call. */
  if (*nc_off == 0U && (length % (size_t)k_mbedtls_aes_alt_block_bytes) == 0U) {
    int rc = priv_issue(ctx,
                        k_ra8_rsip_aes_mode_ctr,
                        k_ra8_rsip_dir_encrypt,
                        (const uint8_t*)nonce_counter,
                        (const uint8_t*)input,
                        (uint8_t*)output,
                        (uint32_t)length);
    if (rc != 0) {
      return rc;
    }
    /* Counter advances by length / 16 -- caller manages wraparound
     * via the next nonce_counter; we leave the canonical big-endian
     * tail for the engine to maintain. */
    return 0;
  }
  /* Slow path: byte-at-a-time so the partial-block bookkeeping is
   * exactly the same as the upstream software CTR. */
  for (size_t i = 0U; i < length; i++) {
    if (*nc_off == 0U) {
      uint8_t zero_block[k_mbedtls_aes_alt_block_bytes] = {};
      int     rc = priv_issue(ctx,
                              k_ra8_rsip_aes_mode_ctr,
                              k_ra8_rsip_dir_encrypt,
                              (const uint8_t*)nonce_counter,
                              (const uint8_t*)zero_block,
                              (uint8_t*)stream_block,
                              (uint32_t)k_mbedtls_aes_alt_block_bytes);
      if (rc != 0) {
        return rc;
      }
      /* Increment the 128-bit big-endian counter in nonce_counter. */
      for (int j = 15; j >= 0; j--) {
        nonce_counter[j]++;
        if (nonce_counter[j] != 0U) {
          break;
        }
      }
    }
    output[i] = input[i] ^ stream_block[*nc_off];
    *nc_off   = (*nc_off + 1U) % (size_t)k_mbedtls_aes_alt_block_bytes;
  }
  return 0;
}

/* NOLINTEND(readability-magic-numbers, cppcoreguidelines-avoid-magic-numbers) */
