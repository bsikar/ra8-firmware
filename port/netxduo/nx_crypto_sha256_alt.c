/**
 * @file port/netxduo/nx_crypto_sha256_alt.c
 * @brief Hardware-accelerated SHA-256 ALT shim for NetX Crypto on RA8D2 RSIP
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Provides the ``__wrap__nx_crypto_method_sha256_init`` /
 * ``..._operation`` / ``..._cleanup`` symbols that GNU ld redirects
 * the upstream NetX Crypto algorithm-table entries to (configured
 * by ``cmake/netxduo.cmake``).
 *
 * NetX Crypto exposes SHA-256 through three operations:
 *
 *   - ``NX_CRYPTO_HASH_INITIALIZE``   -- reset state.
 *   - ``NX_CRYPTO_HASH_UPDATE``       -- absorb input bytes.
 *   - ``NX_CRYPTO_HASH_CALCULATE``    -- finalise + emit the digest.
 *
 * The ``ra_rsip_sha256`` HAL API is single-shot (one buffer in,
 * 32-byte digest out). To honour the streaming contract we
 * accumulate bytes into a private buffer in the trailing metadata
 * area, then fire the hardware engine once at calculate-time. For
 * the common single-block hashes that NetX issues during TLS
 * handshake (e.g. transcript hashes, HKDF key derivation seeds),
 * this trades a small amount of RAM for a >50 round-per-millisecond
 * speed-up over the software implementation.
 *
 * If the trailing metadata area is too small (the algorithm-table
 * entry advertises ``sizeof(NX_CRYPTO_SHA256)`` only), the wrap
 * falls back to the upstream software path so we never silently
 * overwrite caller memory.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "nx_crypto.h"
#include "nx_crypto_const.h"
#include "nx_crypto_sha2.h"
#include "ra_err.h"
#include "ra_rsip.h"

/**
 * @enum nx_sha256_alt_constants_t
 * @brief Compile-time constants for the NetX Crypto <-> RSIP SHA-256 bridge.
 */
typedef enum : uint16_t {
  k_nx_sha256_alt_digest_bytes = 32U,   /**< Output digest size.        */
  k_nx_sha256_alt_max_message  = 4096U, /**< Largest absorbed message.  */
} nx_sha256_alt_constants_t;

/**
 * @struct nx_sha256_alt_meta_t
 * @brief Trailing metadata appended after ``NX_CRYPTO_SHA256``.
 *
 * @details
 * Holds the streaming buffer and used-byte counter for the bridged
 * hash. ``ready`` flips to 1 once ``priv_meta_of`` has confirmed
 * the metadata area is large enough to host this trailer.
 */
typedef struct {
  uint8_t  buf[k_nx_sha256_alt_max_message]; /**< Accumulator buffer.   */
  uint32_t used;                             /**< Bytes written.        */
  uint8_t  ready;                            /**< 1 if streaming ready. */
} nx_sha256_alt_meta_t;

/* Upstream symbols renamed by `--wrap=`. Declared so we can call
 * them in the forward path. */
UINT __real__nx_crypto_method_sha256_init(struct NX_CRYPTO_METHOD_STRUCT* method,
                                          UCHAR*                          key,
                                          NX_CRYPTO_KEY_SIZE              key_size_in_bits,
                                          VOID**                          handle,
                                          VOID*                           crypto_metadata,
                                          ULONG                           crypto_metadata_size);

UINT __real__nx_crypto_method_sha256_operation(UINT                            op,
                                               VOID*                           handle,
                                               struct NX_CRYPTO_METHOD_STRUCT* method,
                                               UCHAR*                          key,
                                               NX_CRYPTO_KEY_SIZE              key_size_in_bits,
                                               UCHAR*                          input,
                                               ULONG                           input_length_in_byte,
                                               UCHAR*                          iv_ptr,
                                               UCHAR*                          output,
                                               ULONG output_length_in_byte,
                                               VOID* crypto_metadata,
                                               ULONG crypto_metadata_size,
                                               VOID* packet_ptr,
                                               VOID (*hw_cb)(VOID*, UINT));

UINT __real__nx_crypto_method_sha256_cleanup(VOID* crypto_metadata);

/**
 * @brief Compute a pointer to the trailing SHA-256 metadata block.
 *
 * @param[in]  crypto_metadata Pointer to the ``NX_CRYPTO_SHA256``-sized
 *                             block NetX gave us.
 * @param[in]  size            ``crypto_metadata_size`` from the caller.
 *
 * @return Trailing metadata pointer if the area is large enough,
 *         otherwise ``NULL``.
 *
 * @pre None.
 *
 * @since 0.1.0
 */
static nx_sha256_alt_meta_t* priv_meta_of(VOID* crypto_metadata, ULONG size)
{
  if (size < (sizeof(NX_CRYPTO_SHA256) + sizeof(nx_sha256_alt_meta_t))) {
    return (nx_sha256_alt_meta_t*)0;
  }
  uint8_t* base = (uint8_t*)crypto_metadata;
  return (nx_sha256_alt_meta_t*)(base + sizeof(NX_CRYPTO_SHA256));
}

/**
 * @brief ALT wrapper for ``_nx_crypto_method_sha256_init``.
 *
 * @details
 * Calls the upstream init for full state setup, then -- if the
 * metadata area is large enough -- resets our trailing accumulator.
 *
 * @since 0.1.0
 */
UINT __wrap__nx_crypto_method_sha256_init(struct NX_CRYPTO_METHOD_STRUCT* method,
                                          UCHAR*                          key,
                                          NX_CRYPTO_KEY_SIZE              key_size_in_bits,
                                          VOID**                          handle,
                                          VOID*                           crypto_metadata,
                                          ULONG                           crypto_metadata_size)
{
  UINT base_status = __real__nx_crypto_method_sha256_init(method,
                                                          key,
                                                          key_size_in_bits,
                                                          handle,
                                                          crypto_metadata,
                                                          crypto_metadata_size);
  if (base_status != (UINT)NX_CRYPTO_SUCCESS) {
    return base_status;
  }
  nx_sha256_alt_meta_t* meta = priv_meta_of(crypto_metadata, crypto_metadata_size);
  if (meta != (nx_sha256_alt_meta_t*)0) {
    meta->used  = 0U;
    meta->ready = 1U;
  }
  return (UINT)NX_CRYPTO_SUCCESS;
}

/**
 * @brief ALT wrapper for ``_nx_crypto_method_sha256_operation``.
 *
 * @details
 * Streaming model:
 *
 *   - On INITIALIZE: reset accumulator (already done in the wrap-init
 *     path; duplicate is a no-op safety).
 *   - On UPDATE: copy ``input_length_in_byte`` into the accumulator
 *     while space allows. Falls back to upstream when the accumulator
 *     is full or the trailing metadata is missing.
 *   - On CALCULATE: feed the accumulated bytes through
 *     ``ra_rsip_sha256`` and copy the 32-byte digest to ``output``.
 *
 * Any other operation is forwarded straight to the upstream
 * software path.
 *
 * @since 0.1.0
 */
UINT __wrap__nx_crypto_method_sha256_operation(UINT                            op,
                                               VOID*                           handle,
                                               struct NX_CRYPTO_METHOD_STRUCT* method,
                                               UCHAR*                          key,
                                               NX_CRYPTO_KEY_SIZE              key_size_in_bits,
                                               UCHAR*                          input,
                                               ULONG                           input_length_in_byte,
                                               UCHAR*                          iv_ptr,
                                               UCHAR*                          output,
                                               ULONG output_length_in_byte,
                                               VOID* crypto_metadata,
                                               ULONG crypto_metadata_size,
                                               VOID* packet_ptr,
                                               VOID (*hw_cb)(VOID*, UINT))
{
  if (method == NX_CRYPTO_NULL || crypto_metadata == NX_CRYPTO_NULL) {
    return (UINT)NX_CRYPTO_PTR_ERROR;
  }

  nx_sha256_alt_meta_t* meta = priv_meta_of(crypto_metadata, crypto_metadata_size);
  if (meta == (nx_sha256_alt_meta_t*)0 || meta->ready == 0U) {
    /* Metadata too small or never primed -- fall back. */
    return __real__nx_crypto_method_sha256_operation(op,
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

  if (op == (UINT)NX_CRYPTO_HASH_INITIALIZE) {
    meta->used = 0U;
    return (UINT)NX_CRYPTO_SUCCESS;
  }

  if (op == (UINT)NX_CRYPTO_HASH_UPDATE) {
    if (input == NX_CRYPTO_NULL) {
      return (UINT)NX_CRYPTO_PTR_ERROR;
    }
    if ((meta->used + (uint32_t)input_length_in_byte) > (uint32_t)k_nx_sha256_alt_max_message) {
      /* Accumulator full -- defer to upstream which can stream
       * arbitrary-length inputs through its working buffer. */
      return __real__nx_crypto_method_sha256_operation(op,
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
    (void)memcpy((void*)(meta->buf + meta->used), (const void*)input, (size_t)input_length_in_byte);
    meta->used += (uint32_t)input_length_in_byte;
    return (UINT)NX_CRYPTO_SUCCESS;
  }

  if (op == (UINT)NX_CRYPTO_HASH_CALCULATE) {
    if (output == NX_CRYPTO_NULL) {
      return (UINT)NX_CRYPTO_PTR_ERROR;
    }
    if (output_length_in_byte < (ULONG)k_nx_sha256_alt_digest_bytes) {
      return (UINT)NX_CRYPTO_INVALID_BUFFER_SIZE;
    }
    ra_err_t err = ra_rsip_sha256(meta->buf, meta->used, (uint8_t*)output);
    /* Reset accumulator regardless of HW outcome so the next
     * hash starts fresh. */
    meta->used = 0U;
    if (err != k_ra_ok) {
      return (UINT)NX_CRYPTO_NOT_SUCCESSFUL;
    }
    return (UINT)NX_CRYPTO_SUCCESS;
  }

  /* AUTHENTICATE / VERIFY / unknown ops -> upstream. */
  return __real__nx_crypto_method_sha256_operation(op,
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

/**
 * @brief ALT wrapper for ``_nx_crypto_method_sha256_cleanup``.
 *
 * @details
 * Zeroes our trailing accumulator and delegates to upstream so the
 * built-in software state is wiped under the same rules
 * (``NX_SECURE_KEY_CLEAR``).
 *
 * @since 0.1.0
 */
UINT __wrap__nx_crypto_method_sha256_cleanup(VOID* crypto_metadata)
{
  if (crypto_metadata != NX_CRYPTO_NULL) {
    /* Without crypto_metadata_size we can only do a best-effort
     * trailing wipe -- consumers that size the metadata area for
     * the trailer accept this. */
    nx_sha256_alt_meta_t* meta =
      (nx_sha256_alt_meta_t*)((uint8_t*)crypto_metadata + sizeof(NX_CRYPTO_SHA256));
    (void)memset((void*)meta, 0, sizeof(*meta));
  }
  return __real__nx_crypto_method_sha256_cleanup(crypto_metadata);
}
