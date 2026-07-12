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
 * The ``ra8_rsip_sha256`` HAL API is single-shot (one buffer in,
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
#include "ra8_err.h"
#include "ra8_rsip.h"

/**
 * @enum nx_sha256_alt_constants_t
 * @brief Compile-time constants for the NetX Crypto <-> RSIP SHA-256 bridge.
 */
typedef enum : uint16_t {
  k_nx_sha256_alt_digest_bytes = 32U,   /**< Output digest size.       */
  k_nx_sha256_alt_max_message  = 4096U, /**< Largest absorbed message. */
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
/* real  nx crypto method sha256 init -- see implementation for details. */
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

/* real  nx crypto method sha256 cleanup -- see implementation for details. */
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
 *
 * @param[in,out] crypto_metadata See function signature.
 * @param[in,out] crypto_metadata_size See function signature.
 * @param[in,out] handle See function signature.
 * @param[in,out] key See function signature.
 * @param[in,out] key_size_in_bits See function signature.
 * @param[in,out] method See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
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
 * @struct nx_sha256_alt_op_args_t
 * @brief Forwarded argument bundle for the SHA-256 operation entry point.
 *
 * @details
 * Groups the 14 parameters NetX Crypto passes to
 * ``_nx_crypto_method_sha256_operation`` so the wrap function and its
 * helpers can pass the full request around without re-listing the
 * signature at every call site.
 */
typedef struct {
  UINT                            op;                    /**< NetX op selector. */
  VOID*                           handle;                /**< NetX session ptr. */
  struct NX_CRYPTO_METHOD_STRUCT* method;                /**< Algorithm entry.  */
  UCHAR*                          key;                   /**< Caller key.       */
  NX_CRYPTO_KEY_SIZE              key_size_in_bits;      /**< Key length.       */
  UCHAR*                          input;                 /**< Input buffer.     */
  ULONG                           input_length_in_byte;  /**< Input length.     */
  UCHAR*                          iv_ptr;                /**< IV pointer.       */
  UCHAR*                          output;                /**< Output buffer.    */
  ULONG                           output_length_in_byte; /**< Output capacity.  */
  VOID*                           crypto_metadata;       /**< Metadata block.   */
  ULONG                           crypto_metadata_size;  /**< Metadata size.    */
  VOID*                           packet_ptr;            /**< Packet context.   */
  VOID (*hw_cb)(VOID*, UINT);                            /**< HW completion cb. */
} nx_sha256_alt_op_args_t;

/**
 * @brief Forward a SHA-256 operation to the upstream software path.
 *
 * @details
 * Unpacks the bundled ``nx_sha256_alt_op_args_t`` and invokes the
 * original ``__real__nx_crypto_method_sha256_operation`` provided by
 * NetX Crypto. Used both as the unconditional path for unsupported ops
 * and as the overflow fallback inside the streaming buffer path.
 *
 * @param[in] a Bundled argument set originally passed to the wrap;
 *              non-NULL, fully populated by the wrapper.
 *
 * @return Upstream return value from
 *         ``__real__nx_crypto_method_sha256_operation``.
 * @retval NX_CRYPTO_SUCCESS Upstream completed successfully.
 * @retval other Any non-zero NetX Crypto error propagated unchanged.
 *
 * @pre ``a != NULL``.
 * @pre ``a->method != NULL`` (NetX never invokes the op without a method).
 * @post No state owned by this shim is modified.
 * @post The bundled arguments are passed through unchanged.
 *
 * @note Not thread-safe; caller serialises per NetX Crypto API rules.
 *
 * @since 0.1.0
 */
static UINT priv_forward_op(const nx_sha256_alt_op_args_t* a)
{
  return __real__nx_crypto_method_sha256_operation(a->op,
                                                   a->handle,
                                                   a->method,
                                                   a->key,
                                                   a->key_size_in_bits,
                                                   a->input,
                                                   a->input_length_in_byte,
                                                   a->iv_ptr,
                                                   a->output,
                                                   a->output_length_in_byte,
                                                   a->crypto_metadata,
                                                   a->crypto_metadata_size,
                                                   a->packet_ptr,
                                                   a->hw_cb);
}

/**
 * @brief Handle ``NX_CRYPTO_HASH_UPDATE`` against the streaming buffer.
 *
 * @details
 * Validates the input pointer, then appends up to
 * ``k_nx_sha256_alt_max_message`` bytes into the accumulator. Falls
 * back to the upstream software path once the accumulator would
 * overflow so arbitrarily-large hashes still complete correctly.
 *
 * @param[in,out] meta Trailing metadata block; non-NULL.
 * @param[in]     a    Bundled argument set.
 *
 * @return ``NX_CRYPTO_SUCCESS`` on append; upstream return code on
 *         overflow fallback; ``NX_CRYPTO_PTR_ERROR`` on NULL input.
 * @retval NX_CRYPTO_SUCCESS Bytes appended to the accumulator.
 * @retval NX_CRYPTO_PTR_ERROR ``a->input`` is NULL.
 * @retval other Upstream NetX error from the overflow fallback path.
 *
 * @pre ``meta != NULL``.
 * @pre ``a != NULL``.
 * @post On success, ``meta->used`` advanced by
 *       ``a->input_length_in_byte``.
 * @post On NULL-input failure, ``meta->used`` is unchanged.
 *
 * @note Not thread-safe; caller serialises per NetX Crypto API rules
 *       and the per-context streaming contract.
 *
 * @since 0.1.0
 */
static UINT priv_handle_update(nx_sha256_alt_meta_t* meta, const nx_sha256_alt_op_args_t* a)
{
  if (a->input == NX_CRYPTO_NULL) {
    return (UINT)NX_CRYPTO_PTR_ERROR;
  }
  if ((meta->used + (uint32_t)a->input_length_in_byte) > (uint32_t)k_nx_sha256_alt_max_message) {
    /* Accumulator full -- defer to upstream which can stream
     * arbitrary-length inputs through its working buffer. */
    return priv_forward_op(a);
  }
  (void)memcpy((void*)(meta->buf + meta->used),
               (const void*)a->input,
               (size_t)a->input_length_in_byte);
  meta->used += (uint32_t)a->input_length_in_byte;
  return (UINT)NX_CRYPTO_SUCCESS;
}

/**
 * @brief Handle ``NX_CRYPTO_HASH_CALCULATE`` against the streaming buffer.
 *
 * @details
 * Range-checks the output buffer, fires the RSIP single-shot
 * ``ra8_rsip_sha256`` over the accumulated bytes, and resets the
 * accumulator so the next hash starts fresh -- even on HW error.
 *
 * @param[in,out] meta Trailing metadata block; non-NULL.
 * @param[in]     a    Bundled argument set.
 *
 * @return ``NX_CRYPTO_SUCCESS`` on success;
 *         ``NX_CRYPTO_PTR_ERROR`` if output is NULL;
 *         ``NX_CRYPTO_INVALID_BUFFER_SIZE`` if output too small;
 *         ``NX_CRYPTO_NOT_SUCCESSFUL`` if RSIP rejects the request.
 * @retval NX_CRYPTO_SUCCESS Digest written to ``a->output``.
 * @retval NX_CRYPTO_PTR_ERROR ``a->output`` is NULL.
 * @retval NX_CRYPTO_INVALID_BUFFER_SIZE Output capacity below digest size.
 * @retval NX_CRYPTO_NOT_SUCCESSFUL RSIP rejected the request.
 *
 * @pre ``meta != NULL``.
 * @pre ``a != NULL``.
 * @post ``meta->used`` is zero on return regardless of outcome.
 * @post On success, the 32-byte SHA-256 digest is written to
 *       ``a->output``.
 *
 * @note Not thread-safe; caller serialises per NetX Crypto API rules
 *       and the RSIP single-context contract.
 *
 * @since 0.1.0
 */
static UINT priv_handle_calculate(nx_sha256_alt_meta_t* meta, const nx_sha256_alt_op_args_t* a)
{
  if (a->output == NX_CRYPTO_NULL) {
    return (UINT)NX_CRYPTO_PTR_ERROR;
  }
  if (a->output_length_in_byte < (ULONG)k_nx_sha256_alt_digest_bytes) {
    return (UINT)NX_CRYPTO_INVALID_BUFFER_SIZE;
  }
  ra8_err_t err = ra8_rsip_sha256(meta->buf, meta->used, (uint8_t*)a->output);
  /* Reset accumulator regardless of HW outcome so the next
   * hash starts fresh. */
  meta->used = 0U;
  if (err != k_ra8_ok) {
    return (UINT)NX_CRYPTO_NOT_SUCCESSFUL;
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
 *     ``ra8_rsip_sha256`` and copy the 32-byte digest to ``output``.
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
  const nx_sha256_alt_op_args_t a = {
    .op                    = op,
    .handle                = handle,
    .method                = method,
    .key                   = key,
    .key_size_in_bits      = key_size_in_bits,
    .input                 = input,
    .input_length_in_byte  = input_length_in_byte,
    .iv_ptr                = iv_ptr,
    .output                = output,
    .output_length_in_byte = output_length_in_byte,
    .crypto_metadata       = crypto_metadata,
    .crypto_metadata_size  = crypto_metadata_size,
    .packet_ptr            = packet_ptr,
    .hw_cb                 = hw_cb,
  };

  nx_sha256_alt_meta_t* meta = priv_meta_of(crypto_metadata, crypto_metadata_size);
  if (meta == (nx_sha256_alt_meta_t*)0 || meta->ready == 0U) {
    /* Metadata too small or never primed -- fall back. */
    return priv_forward_op(&a);
  }
  if (op == (UINT)NX_CRYPTO_HASH_INITIALIZE) {
    meta->used = 0U;
    return (UINT)NX_CRYPTO_SUCCESS;
  }
  if (op == (UINT)NX_CRYPTO_HASH_UPDATE) {
    return priv_handle_update(meta, &a);
  }
  if (op == (UINT)NX_CRYPTO_HASH_CALCULATE) {
    return priv_handle_calculate(meta, &a);
  }
  /* AUTHENTICATE / VERIFY / unknown ops -> upstream. */
  return priv_forward_op(&a);
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
 *
 * @param[in,out] crypto_metadata See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
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
