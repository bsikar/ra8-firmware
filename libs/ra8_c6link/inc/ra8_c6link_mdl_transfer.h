/**
 * @file ra8_c6link_mdl_transfer.h
 * @brief Transactional RA8-side coordinator for C6 media byte streams
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * This module composes an already-open ::ra8_c6link_t with caller-owned
 * storage and SHA-256 implementations. The C6 only fetches source bytes; the
 * RA8 owns the destination, verifies the digest independently, and publishes
 * the temporary object atomically. Every buffer and context is caller-owned,
 * and the transfer loop is bounded by `max_chunks`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_c6link_mdl.h"
#include "ra8_err.h"

/**
 * @struct ra8_mdl_storage_iface
 * @brief Transactional destination seam for one downloaded object
 *
 * @details `begin` creates private temporary state, `write` appends bytes, the
 * optional `validate` inspects that private object after transport integrity
 * succeeds, `commit` atomically publishes it, and `abort` destroys all
 * temporary state. A backend may represent SD, XSPI, MRAM, or an e-reader
 * content store without exposing its handles to the C6 protocol. A `.rabook`
 * integration supplies `validate` to check its magic, header, and structure;
 * leaving it null explicitly selects format-agnostic raw-byte transfer.
 *
 * @invariant Every function pointer and `ctx` is non-null during a transfer.
 * @invariant A successful `commit` is atomic from the reader's perspective.
 *
 * @code
 * ra8_mdl_storage_iface_t storage = {
 *   .begin = store_begin, .write = store_write,
 *   .validate = rabook_validate_temp,
 *   .commit = store_commit, .abort = store_abort, .ctx = &store,
 * };
 * @endcode
 *
 * @see ra8_c6link_mdl_transfer
 * @since 0.1.0
 */
typedef struct ra8_mdl_storage_iface {
  /** @brief Create private temporary state for `destination`. */
  ra8_err_t (*begin)(void* ctx, const char* destination);
  /** @brief Append `len` bytes and report the exact number persisted. */
  ra8_err_t (*write)(void* ctx, const uint8_t* data, uint16_t len, uint16_t* written);
  /**
   * @brief Validate artifact identity and structure before publication
   * @param[in,out] ctx Backend context with one complete private object.
   * @param[in] total_bytes Verified object byte length.
   * @param[in] sha256 Independently verified SHA-256 digest.
   * @return Validation status; non-success prevents commit and triggers abort.
   * @note Optional. Null means raw bytes with no claimed artifact format.
   */
  ra8_err_t (*validate)(void*         ctx,
                        uint64_t      total_bytes,
                        const uint8_t sha256[k_ra8_mdl_sha256_bytes]);
  /** @brief Atomically publish the complete temporary object. */
  ra8_err_t (*commit)(void* ctx);
  /** @brief Destroy temporary state; valid after every successful `begin`. */
  ra8_err_t (*abort)(void* ctx);
  void* ctx; /**< Backend context passed to every function. */
} ra8_mdl_storage_iface_t;

/**
 * @struct ra8_mdl_sha256_iface
 * @brief Streaming SHA-256 seam with caller-owned fixed storage
 *
 * @details The implementation may wrap RSIP, PSA Crypto, or a bounded
 * software implementation. The coordinator never allocates or knows the
 * concrete context type.
 *
 * @invariant `init`, `update`, `final`, and `ctx` are non-null.
 * @invariant `final` writes exactly ::k_ra8_mdl_sha256_bytes bytes.
 *
 * @code
 * ra8_mdl_sha256_iface_t hash = {
 *   .init = sha_init, .update = sha_update, .final = sha_final, .ctx = &sha,
 * };
 * @endcode
 *
 * @see ra8_mdl_transfer_config_t
 * @since 0.1.0
 */
typedef struct ra8_mdl_sha256_iface {
  /** @brief Reset the caller-owned context for a SHA-256 operation. */
  ra8_err_t (*init)(void* ctx);
  /** @brief Feed one ordered byte span to the running hash. */
  ra8_err_t (*update)(void* ctx, const uint8_t* data, uint16_t len);
  /** @brief Finalise the hash into the caller-provided digest. */
  ra8_err_t (*final)(void* ctx, uint8_t out[k_ra8_mdl_sha256_bytes]);
  void* ctx; /**< Hash context passed to every function. */
} ra8_mdl_sha256_iface_t;

/**
 * @brief Query whether the caller requests cooperative cancellation
 *
 * @details Called before each remote pull. Returning true causes remote
 * cancellation followed by unconditional local transaction abort.
 *
 * @param[in] ctx Caller-owned cancellation context.
 * @return Whether cancellation is requested.
 * @retval true Stop before pulling another chunk.
 * @retval false Continue the transfer.
 * @pre @p ctx remains valid for the transfer duration.
 * @pre The callback does not mutate the c6link handle.
 * @post No coordinator-owned state is modified directly.
 * @post A true result is observed before another storage write.
 * @note Called synchronously; thread safety belongs to the implementation.
 * @since 0.1.0
 */
typedef bool (*ra8_mdl_cancel_requested_fn)(void* ctx);

/**
 * @struct ra8_mdl_transfer_config
 * @brief Fixed policy and injected mechanisms for one transfer
 *
 * @details `chunk_bytes` controls backpressure and `max_chunks` is the hard
 * loop bound. The optional cancellation callback is the only policy hook.
 * Storage and SHA implementations remain independently substitutable.
 *
 * @invariant `chunk_bytes` is in 1..::k_ra8_mdl_chunk_data_max.
 * @invariant `max_chunks` is non-zero and bounds every remote pull.
 *
 * @code
 * ra8_mdl_transfer_config_t cfg = {
 *   .storage = storage, .sha256 = hash,
 *   .cancel_requested = ui_cancelled, .cancel_ctx = &ui,
 *   .chunk_bytes = k_ra8_mdl_chunk_data_max, .max_chunks = 4096U,
 * };
 * @endcode
 *
 * @see ra8_c6link_mdl_transfer
 * @since 0.1.0
 */
typedef struct ra8_mdl_transfer_config {
  ra8_mdl_storage_iface_t     storage;          /**< Transactional RA8-local destination. */
  ra8_mdl_sha256_iface_t      sha256;           /**< Independent running digest.          */
  ra8_mdl_cancel_requested_fn cancel_requested; /**< Optional cooperative cancel query.   */
  void*                       cancel_ctx;       /**< Context for `cancel_requested`.      */
  uint16_t                    chunk_bytes;      /**< Maximum bytes requested per pull.    */
  uint32_t                    max_chunks;       /**< Absolute number of permitted pulls.  */
} ra8_mdl_transfer_config_t;

/**
 * @struct ra8_mdl_transfer_result
 * @brief Verified result published by a completed transfer
 *
 * @details Written only after the remote terminal digest, independently
 * calculated digest, byte count, and atomic storage commit all agree.
 *
 * @invariant `bytes_stored` equals the committed object's byte length.
 * @invariant `sha256` is the digest verified against the C6 terminal record.
 *
 * @code
 * ra8_mdl_transfer_result_t result = {};
 * @endcode
 *
 * @see ra8_c6link_mdl_transfer
 * @since 0.1.0
 */
typedef struct ra8_mdl_transfer_result {
  uint64_t bytes_stored;                   /**< Bytes atomically committed.    */
  uint32_t chunks_received;                /**< Remote responses consumed.     */
  uint8_t  sha256[k_ra8_mdl_sha256_bytes]; /**< Independently verified digest. */
} ra8_mdl_transfer_result_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fetch, verify, and atomically publish one remote byte stream
 *
 * @details Starts a raw HTTPS byte transfer on an already-open c6link, pulls
 * ordered bounded chunks, hashes exactly the bytes accepted by storage,
 * compares the local digest with the C6 terminal digest, invokes the optional
 * artifact validator against the private object, then commits. SHA equality
 * proves transport integrity but does not prove `.rabook` identity: callers
 * claiming that format must supply `storage.validate`. After a successful
 * storage `begin`, every non-success path calls `abort`; an active remote
 * session is also cancelled on local failure or cancellation.
 *
 * @param[in,out] link Already-open, exclusively owned c6link handle.
 * @param[in] url NUL-terminated source URL accepted by the C6 HTTPS backend.
 * @param[in] destination RA8-local destination understood only by `storage`.
 * @param[in] config Complete fixed-size storage, hash, cancellation, and bound policy.
 * @param[out] result Verified committed byte count and digest.
 * @return Canonical transfer status.
 * @retval k_ra8_ok Object verified and atomically committed.
 * @retval k_ra8_err_null_ptr Required pointer or injected function is null.
 * @retval k_ra8_err_invalid_arg Configuration or URL is invalid.
 * @retval k_ra8_err_invalid_size Chunk bound, response length, or byte count is invalid.
 * @retval k_ra8_err_protocol_error Remote state or ordering is incoherent.
 * @retval k_ra8_err_checksum_mismatch Local and remote SHA-256 digests differ.
 * @retval k_ra8_err_cancelled Caller requested cancellation or remote cancelled.
 * @retval k_ra8_err_timeout `max_chunks` was exhausted before completion.
 * @retval k_ra8_fail Injected storage, hash, or transport mechanism failed.
 * @pre ::ra8_c6link_open completed successfully for @p link.
 * @pre No other thread uses @p link or any injected context concurrently.
 * @post Success leaves exactly one committed destination and no temporary state.
 * @post Failure leaves no committed destination and calls storage `abort` exactly once.
 * @note Not thread-safe; c6link and injected contexts require exclusive ownership.
 * @warning `commit` must provide the atomic publication guarantee; this module cannot synthesize it.
 * @see ra8_c6link_mdl_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_mdl_transfer(ra8_c6link_t*                    link,
                                                const char*                      url,
                                                const char*                      destination,
                                                const ra8_mdl_transfer_config_t* config,
                                                ra8_mdl_transfer_result_t*       result);

#ifdef __cplusplus
}
#endif
