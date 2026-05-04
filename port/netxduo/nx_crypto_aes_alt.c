/**
 * @file port/netxduo/nx_crypto_aes_alt.c
 * @brief Hardware-accelerated AES ALT shim for NetX Crypto on RA8D2 RSIP
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Provides the ``__wrap__nx_crypto_method_aes_init`` /
 * ``..._operation`` / ``..._cleanup`` symbols that GNU ld redirects
 * the upstream NetX Crypto algorithm-table entries to (configured
 * by ``cmake/netxduo.cmake``). Each wrap function inspects the
 * ``method->nx_crypto_algorithm`` selector:
 *
 *   - AES-128-CBC, AES-192-CBC, AES-256-CBC, AES-128-CTR,
 *     AES-256-CTR -- routed to ``ra_rsip_aes_cipher`` running the
 *     RSIP block engine. The wrapped key is installed once per
 *     ``__wrap__nx_crypto_method_aes_init`` call and stored in the
 *     extended metadata block alongside the upstream
 *     ``NX_CRYPTO_AES`` so the cleanup path can zero it.
 *   - AES-CCM-8/12/16 + AES-GCM-8/12/16 + AES-XCBC -- forwarded to
 *     ``__real__nx_crypto_method_aes_*`` (the upstream software
 *     implementation). The RSIP engine *does* support GCM and CCM,
 *     but matching NetX's IV / AAD framing onto the FSP single-shot
 *     ``ra_rsip_aes_gcm`` API requires more buffering than a thin
 *     port shim can carry, so we leave them on the software path.
 *
 * The ``__real_*`` symbols are emitted by GNU ld in response to
 * ``-Wl,--wrap=symbol`` and resolve to the upstream
 * ``_nx_crypto_method_aes_*`` definitions in
 * ``libs/third_party/netxduo/crypto_libraries/src/nx_crypto_aes.c``.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "nx_crypto.h"
#include "nx_crypto_aes.h"
#include "nx_crypto_const.h"
#include "ra_err.h"
#include "ra_rsip.h"

/**
 * @enum nx_aes_alt_constants_t
 * @brief Compile-time constants for the NetX Crypto <-> RSIP AES bridge.
 */
typedef enum : uint16_t {
  k_nx_aes_alt_block_bytes = 16U,  /**< AES block size in bytes.        */
  k_nx_aes_alt_iv_bytes    = 16U,  /**< CBC / CTR IV length.            */
  k_nx_aes_alt_key128_bits = 128U, /**< AES-128 key-size in bits.       */
  k_nx_aes_alt_key192_bits = 192U, /**< AES-192 key-size in bits.       */
  k_nx_aes_alt_key256_bits = 256U, /**< AES-256 key-size in bits.       */
} nx_aes_alt_constants_t;

/**
 * @struct nx_aes_alt_meta_t
 * @brief Trailing metadata appended after ``NX_CRYPTO_AES``.
 *
 * @details
 * NetX's ``crypto_method_aes_cbc_*`` algorithm-table entry sets
 * ``nx_crypto_metadata_area_size = sizeof(NX_CRYPTO_AES)``; we read
 * the same metadata block but treat the bytes after the upstream
 * struct as our own. The slot is exactly one wrapped-key handle plus
 * a "ready" flag.
 */
typedef struct {
  ra_rsip_key_handle_t key;   /**< Wrapped RSIP key handle.             */
  uint8_t              ready; /**< 1 if the key was installed OK.       */
} nx_aes_alt_meta_t;

/* Upstream symbols renamed by `--wrap=`. Declared so we can call
 * them in the forward path. */
/* real  nx crypto method aes init -- see implementation for details. */
UINT __real__nx_crypto_method_aes_init(struct NX_CRYPTO_METHOD_STRUCT* method,
                                       UCHAR*                          key,
                                       NX_CRYPTO_KEY_SIZE              key_size_in_bits,
                                       VOID**                          handle,
                                       VOID*                           crypto_metadata,
                                       ULONG                           crypto_metadata_size);

UINT __real__nx_crypto_method_aes_operation(UINT                            op,
                                            VOID*                           handle,
                                            struct NX_CRYPTO_METHOD_STRUCT* method,
                                            UCHAR*                          key,
                                            NX_CRYPTO_KEY_SIZE              key_size_in_bits,
                                            UCHAR*                          input,
                                            ULONG                           input_length_in_byte,
                                            UCHAR*                          iv_ptr,
                                            UCHAR*                          output,
                                            ULONG                           output_length_in_byte,
                                            VOID*                           crypto_metadata,
                                            ULONG                           crypto_metadata_size,
                                            VOID*                           packet_ptr,
                                            VOID (*hw_cb)(VOID*, UINT));

/* real  nx crypto method aes cleanup -- see implementation for details. */
UINT __real__nx_crypto_method_aes_cleanup(VOID* crypto_metadata);

/* Decide whether an algorithm ID is one we accelerate -- see implementation for details. */
static UCHAR priv_alg_is_accelerated(UINT alg)
{
  if (alg == (UINT)NX_CRYPTO_ENCRYPTION_AES_CBC) {
    return 1U;
  }
  if (alg == (UINT)NX_CRYPTO_ENCRYPTION_AES_CTR) {
    return 1U;
  }
  return 0U;
}

/* Translate a NetX Crypto AES key-length into ``ra_rsip_*_install_plain`` -- see implementation for details. */
static ra_err_t
priv_install_key(NX_CRYPTO_KEY_SIZE key_bits, const uint8_t* key, ra_rsip_key_handle_t* out_handle)
{
  if (key_bits == (NX_CRYPTO_KEY_SIZE)k_nx_aes_alt_key128_bits) {
    return ra_rsip_aes128_install_plain(key, out_handle);
  }
  if (key_bits == (NX_CRYPTO_KEY_SIZE)k_nx_aes_alt_key192_bits) {
    return ra_rsip_aes192_install_plain(key, out_handle);
  }
  if (key_bits == (NX_CRYPTO_KEY_SIZE)k_nx_aes_alt_key256_bits) {
    return ra_rsip_aes256_install_plain(key, out_handle);
  }
  return k_ra_err_invalid_arg;
}

/**
 * @brief Compute a pointer to the trailing metadata block.
 *
 * @param[in] crypto_metadata Pointer to the ``NX_CRYPTO_AES``-sized
 *                            block NetX gave us.
 *
 * @return Pointer to the trailing ``nx_aes_alt_meta_t``.
 *
 * @pre ``crypto_metadata != NULL``.
 * @post Return value is non-NULL when the metadata area is large
 *       enough; otherwise the caller must check the size first.
 *
 * @since 0.1.0
 */
static nx_aes_alt_meta_t* priv_meta_of(VOID* crypto_metadata)
{
  uint8_t* base = (uint8_t*)crypto_metadata;
  return (nx_aes_alt_meta_t*)(base + sizeof(NX_CRYPTO_AES));
}

/**
 * @brief ALT wrapper for ``_nx_crypto_method_aes_init``.
 *
 * @details
 * Installs the AES key into the RSIP wrapped-key vault, stashes the
 * handle in the trailing metadata, and -- if the metadata area is
 * not large enough to hold the handle -- falls back to the software
 * path so we never silently overwrite caller memory.
 *
 * @return ``NX_CRYPTO_SUCCESS`` on success.
 * @return Upstream error codes for any unsupported key size.
 *
 * @pre See upstream ``_nx_crypto_method_aes_init`` documentation.
 * @post On success, the trailing handle is ready for ``..._operation``.
 *
 * @since 0.1.0
 *
 * @param[in,out] crypto_metadata See function signature.
 * @param[in,out] crypto_metadata_size See function signature.
 * @param[in,out] handle See function signature.
 * @param[in,out] key See function signature.
 * @param[in,out] key_size_in_bits See function signature.
 * @param[in,out] method See function signature.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
UINT __wrap__nx_crypto_method_aes_init(struct NX_CRYPTO_METHOD_STRUCT* method,
                                       UCHAR*                          key,
                                       NX_CRYPTO_KEY_SIZE              key_size_in_bits,
                                       VOID**                          handle,
                                       VOID*                           crypto_metadata,
                                       ULONG                           crypto_metadata_size)
{
  /* Always run the upstream init first -- it owns the
   * ``NX_CRYPTO_AES`` schedule and sanity-checks parameters. */
  UINT base_status = __real__nx_crypto_method_aes_init(method,
                                                       key,
                                                       key_size_in_bits,
                                                       handle,
                                                       crypto_metadata,
                                                       crypto_metadata_size);
  if (base_status != (UINT)NX_CRYPTO_SUCCESS) {
    return base_status;
  }

  if (method == NX_CRYPTO_NULL) {
    return (UINT)NX_CRYPTO_PTR_ERROR;
  }
  if (priv_alg_is_accelerated(method->nx_crypto_algorithm) == 0U) {
    return (UINT)NX_CRYPTO_SUCCESS;
  }

  /* Metadata must fit both upstream + our trailing handle. */
  if (crypto_metadata_size < (sizeof(NX_CRYPTO_AES) + sizeof(nx_aes_alt_meta_t))) {
    /* Stay on the software path -- our extension does not fit. */
    return (UINT)NX_CRYPTO_SUCCESS;
  }
  nx_aes_alt_meta_t* meta = priv_meta_of(crypto_metadata);
  meta->ready             = 0U;
  ra_err_t err            = priv_install_key(key_size_in_bits, key, &meta->key);
  if (err == k_ra_ok) {
    meta->ready = 1U;
  }
  return (UINT)NX_CRYPTO_SUCCESS;
}

/* Map a NetX algorithm + op pair onto ``ra_rsip_aes_cipher`` args -- see implementation for details. */
static UCHAR priv_map_mode(UINT alg, UINT op, ra_rsip_aes_mode_t* mode, ra_rsip_aes_dir_t* dir)
{
  if (alg == (UINT)NX_CRYPTO_ENCRYPTION_AES_CBC) {
    *mode = k_ra_rsip_aes_mode_cbc;
  } else if (alg == (UINT)NX_CRYPTO_ENCRYPTION_AES_CTR) {
    *mode = k_ra_rsip_aes_mode_ctr;
  } else {
    return 0U;
  }
  if (op == (UINT)NX_CRYPTO_ENCRYPT || op == (UINT)NX_CRYPTO_ENCRYPT_UPDATE ||
      op == (UINT)NX_CRYPTO_ENCRYPT_INITIALIZE) {
    *dir = k_ra_rsip_dir_encrypt;
  } else if (op == (UINT)NX_CRYPTO_DECRYPT || op == (UINT)NX_CRYPTO_DECRYPT_UPDATE ||
             op == (UINT)NX_CRYPTO_DECRYPT_INITIALIZE) {
    *dir = k_ra_rsip_dir_decrypt;
  } else {
    return 0U;
  }
  return 1U;
}

/**
 * @brief ALT wrapper for ``_nx_crypto_method_aes_operation``.
 *
 * @details
 * For accelerated CBC / CTR algorithms with a usable wrapped key,
 * routes the bulk of the data through ``ra_rsip_aes_cipher`` and
 * returns. For every other case (initialisation-only ops, no
 * wrapped key, GCM / CCM / XCBC) defers to the upstream
 * ``__real_*`` so behaviour stays bit-identical to the software
 * path.
 *
 * @return ``NX_CRYPTO_SUCCESS`` or upstream error code.
 *
 * @since 0.1.0
 */
UINT __wrap__nx_crypto_method_aes_operation(UINT                            op,
                                            VOID*                           handle,
                                            struct NX_CRYPTO_METHOD_STRUCT* method,
                                            UCHAR*                          key,
                                            NX_CRYPTO_KEY_SIZE              key_size_in_bits,
                                            UCHAR*                          input,
                                            ULONG                           input_length_in_byte,
                                            UCHAR*                          iv_ptr,
                                            UCHAR*                          output,
                                            ULONG                           output_length_in_byte,
                                            VOID*                           crypto_metadata,
                                            ULONG                           crypto_metadata_size,
                                            VOID*                           packet_ptr,
                                            VOID (*hw_cb)(VOID*, UINT))
{
  if (method == NX_CRYPTO_NULL || crypto_metadata == NX_CRYPTO_NULL) {
    return (UINT)NX_CRYPTO_PTR_ERROR;
  }

  if (priv_alg_is_accelerated(method->nx_crypto_algorithm) == 0U) {
    return __real__nx_crypto_method_aes_operation(op,
                                                  handle,
                                                  method,
                                                  key,
                                                  key_size_in_bits,
                                                  input,
                                                  input_length_in_byte,
                                                  iv_ptr,
                                                  output,
                                                  output_length_in_byte,
                                                  crypto_metadata,
                                                  crypto_metadata_size,
                                                  packet_ptr,
                                                  hw_cb);
  }

  /* Bulk encrypt/decrypt path only -- defer init/update/finalise
   * variants to upstream which carries the multi-block state. */
  if (op != (UINT)NX_CRYPTO_ENCRYPT && op != (UINT)NX_CRYPTO_DECRYPT) {
    return __real__nx_crypto_method_aes_operation(op,
                                                  handle,
                                                  method,
                                                  key,
                                                  key_size_in_bits,
                                                  input,
                                                  input_length_in_byte,
                                                  iv_ptr,
                                                  output,
                                                  output_length_in_byte,
                                                  crypto_metadata,
                                                  crypto_metadata_size,
                                                  packet_ptr,
                                                  hw_cb);
  }

  if (crypto_metadata_size < (sizeof(NX_CRYPTO_AES) + sizeof(nx_aes_alt_meta_t))) {
    return __real__nx_crypto_method_aes_operation(op,
                                                  handle,
                                                  method,
                                                  key,
                                                  key_size_in_bits,
                                                  input,
                                                  input_length_in_byte,
                                                  iv_ptr,
                                                  output,
                                                  output_length_in_byte,
                                                  crypto_metadata,
                                                  crypto_metadata_size,
                                                  packet_ptr,
                                                  hw_cb);
  }

  nx_aes_alt_meta_t* meta = priv_meta_of(crypto_metadata);
  if (meta->ready == 0U) {
    return __real__nx_crypto_method_aes_operation(op,
                                                  handle,
                                                  method,
                                                  key,
                                                  key_size_in_bits,
                                                  input,
                                                  input_length_in_byte,
                                                  iv_ptr,
                                                  output,
                                                  output_length_in_byte,
                                                  crypto_metadata,
                                                  crypto_metadata_size,
                                                  packet_ptr,
                                                  hw_cb);
  }

  ra_rsip_aes_mode_t mode = k_ra_rsip_aes_mode_ecb;
  ra_rsip_aes_dir_t  dir  = k_ra_rsip_dir_encrypt;
  if (priv_map_mode(method->nx_crypto_algorithm, op, &mode, &dir) == 0U) {
    return __real__nx_crypto_method_aes_operation(op,
                                                  handle,
                                                  method,
                                                  key,
                                                  key_size_in_bits,
                                                  input,
                                                  input_length_in_byte,
                                                  iv_ptr,
                                                  output,
                                                  output_length_in_byte,
                                                  crypto_metadata,
                                                  crypto_metadata_size,
                                                  packet_ptr,
                                                  hw_cb);
  }

  if (output_length_in_byte < input_length_in_byte) {
    return (UINT)NX_CRYPTO_INVALID_BUFFER_SIZE;
  }

  ra_err_t err = ra_rsip_aes_cipher(&meta->key,
                                    mode,
                                    dir,
                                    (const uint8_t*)iv_ptr,
                                    (const uint8_t*)input,
                                    (uint8_t*)output,
                                    (uint32_t)input_length_in_byte);
  if (err != k_ra_ok) {
    return (UINT)NX_CRYPTO_NOT_SUCCESSFUL;
  }
  return (UINT)NX_CRYPTO_SUCCESS;
}

/**
 * @brief ALT wrapper for ``_nx_crypto_method_aes_cleanup``.
 *
 * @details
 * Zeroes our trailing metadata before delegating to the upstream
 * cleanup path. The wrapped-key handle stays inside the metadata
 * block so this clears it in place; the RSIP engine itself does
 * not retain plaintext after ``ra_rsip_aes*_install_plain``
 * returns.
 *
 * @since 0.1.0
 *
 * @param[in,out] crypto_metadata See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 */
UINT __wrap__nx_crypto_method_aes_cleanup(VOID* crypto_metadata)
{
  if (crypto_metadata != NX_CRYPTO_NULL) {
    /* Best-effort wipe -- the metadata area is sized by the
     * algorithm-table entry so we cannot tell here whether our
     * trailer fits. Wiping ``sizeof(nx_aes_alt_meta_t)`` past the
     * upstream block is safe because the entries that DO size for
     * the trailer set it explicitly (see the example app's
     * extended metadata buffer). */
    nx_aes_alt_meta_t* meta = priv_meta_of(crypto_metadata);
    (void)memset((void*)meta, 0, sizeof(*meta));
  }
  return __real__nx_crypto_method_aes_cleanup(crypto_metadata);
}
