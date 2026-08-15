/**
 * @file ra8_c6link_mdl_transfer.c
 * @brief Bounded transactional coordinator for C6 media byte streams
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details Implements the storage/hash composition declared by
 * `ra8_c6link_mdl_transfer.h`. It owns no static state and allocates no heap.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_c6link_mdl_transfer.h"

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief State that must be unwound together after storage begins. */
typedef struct {
  ra8_c6link_t*                    link;    /**< Active media RPC link.                       */
  const ra8_mdl_transfer_config_t* config;  /**< Injected storage/hash seams.                 */
  ra8_mdl_session_t                session; /**< Correlated remote session.                   */
  bool storage_active;                      /**< Whether local temporary storage needs abort. */
} mdl_transfer_state_t;

/**
 * @brief Validate every injected mechanism before creating temporary state
 * @details Keeps configuration rejection separate from transactional cleanup.
 * @param[in] link Candidate open c6link handle.
 * @param[in] url Candidate source URL.
 * @param[in] destination Candidate RA8-local destination.
 * @param[in] config Candidate fixed transfer configuration.
 * @param[out] result Candidate result storage.
 * @return Validation status.
 * @retval k_ra8_ok Every required value is present and bounded.
 * @retval k_ra8_err_null_ptr A required value or function is null.
 * @retval k_ra8_err_invalid_size A numeric bound is invalid.
 * @pre Arguments may be null because this function validates them.
 * @pre No injected callback has run.
 * @post No caller-owned state is modified.
 * @post Success guarantees every later indirect call target is non-null.
 * @note Thread-safe when the caller does not mutate @p config concurrently.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_transfer_validate(ra8_c6link_t*                    link,
                               const char*                      url,
                               const char*                      destination,
                               const ra8_mdl_transfer_config_t* config,
                               ra8_mdl_transfer_result_t*       result)
{
  if ((link == nullptr) || (url == nullptr) || (destination == nullptr) || (config == nullptr) ||
      (result == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((config->storage.begin == nullptr) || (config->storage.write == nullptr) ||
      (config->storage.commit == nullptr) || (config->storage.abort == nullptr) ||
      (config->storage.ctx == nullptr) || (config->sha256.init == nullptr) ||
      (config->sha256.update == nullptr) || (config->sha256.final == nullptr) ||
      (config->sha256.ctx == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((config->chunk_bytes == 0U) || (config->chunk_bytes > k_ra8_mdl_chunk_data_max) ||
      (config->max_chunks == 0U)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Cancel the remote job when active and always abort local temporary state
 * @details Cleanup failures are returned only when no earlier cause exists.
 * @param[in,out] state Transfer resources to unwind.
 * @param[in] cause Original failure, or success when cleanup itself triggered the result.
 * @return Original cause, then cancel error, then abort error in priority order.
 * @retval k_ra8_ok No cause and both cleanup operations succeeded.
 * @retval k_ra8_err_cancelled Cancellation completed normally.
 * @retval k_ra8_fail A cleanup mechanism failed without an earlier cause.
 * @pre @p state and its configuration are non-null.
 * @pre `storage_active` accurately records successful `begin`.
 * @post The remote is asked to cancel if its session remains active.
 * @post Storage `abort` is called exactly once when active.
 * @note Not thread-safe; it mutates the session and storage context.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_transfer_abort(mdl_transfer_state_t* state,
                                                          ra8_err_t             cause)
{
  ra8_err_t cancel_result = k_ra8_ok;
  ra8_err_t abort_result  = k_ra8_ok;
  if (state->session.active) {
    cancel_result = ra8_c6link_mdl_cancel(state->link, &state->session);
  }
  if (state->storage_active) {
    abort_result          = state->config->storage.abort(state->config->storage.ctx);
    state->storage_active = false;
  }
  if (cause != k_ra8_ok) {
    return cause;
  }
  return (cancel_result != k_ra8_ok) ? cancel_result : abort_result;
}

/**
 * @brief Persist and hash one non-terminal ordered chunk
 * @details Rejects short successful writes because they would make the local
 * digest describe bytes that are not durable in the temporary object.
 * @param[in] config Injected storage and hash mechanisms.
 * @param[in] chunk Validated remote chunk.
 * @param[in,out] bytes_stored Running durable byte count.
 * @return Persistence status.
 * @retval k_ra8_ok Every chunk byte was persisted and hashed.
 * @retval k_ra8_err_invalid_size The write was short or the count overflowed.
 * @retval k_ra8_fail The storage or hash implementation failed.
 * @pre @p chunk is a DOWNLOADING response with non-zero `data_len`.
 * @pre @p bytes_stored equals the temporary object's current length.
 * @post Success advances @p bytes_stored by exactly `data_len`.
 * @post Failure never commits the temporary object.
 * @note Not thread-safe; it mutates injected contexts.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_transfer_store(const ra8_mdl_transfer_config_t* config,
                                                          const ra8_mdl_chunk_t*           chunk,
                                                          uint64_t* bytes_stored)
{
  if (*bytes_stored > (UINT64_MAX - chunk->data_len)) {
    return k_ra8_err_invalid_size;
  }
  uint16_t  written = 0U;
  ra8_err_t err =
    config->storage.write(config->storage.ctx, chunk->data, chunk->data_len, &written);
  if (err != k_ra8_ok) {
    return err;
  }
  if (written != chunk->data_len) {
    return k_ra8_err_invalid_size;
  }
  err = config->sha256.update(config->sha256.ctx, chunk->data, chunk->data_len);
  if (err == k_ra8_ok) {
    *bytes_stored += written;
  }
  return err;
}

/**
 * @brief Verify terminal metadata, validate identity, and commit the object
 * @details Finalises the independent hash only after byte-count agreement,
 * then gives an optional artifact validator the complete private object before
 * atomic publication.
 * @param[in,out] state Active transfer state.
 * @param[in] chunk Terminal COMPLETE response.
 * @param[in] bytes_stored Durable byte count.
 * @param[in] chunks_received Number of remote responses consumed.
 * @param[out] result Result written only after commit succeeds.
 * @return Verification or commit status.
 * @retval k_ra8_ok Digest matched and storage committed atomically.
 * @retval k_ra8_err_invalid_size Remote and local byte counts disagree.
 * @retval k_ra8_err_checksum_mismatch SHA-256 digests disagree.
 * @retval k_ra8_fail Hash finalisation or storage commit failed.
 * @pre @p chunk is a COMPLETE response carrying SHA-256.
 * @pre Storage transaction is active and uncommitted.
 * @post Success clears `storage_active` and fills @p result.
 * @post Failure leaves `storage_active` set for caller cleanup.
 * @note Not thread-safe; it finalises and commits injected contexts.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_transfer_commit(mdl_transfer_state_t*  state,
                                                           const ra8_mdl_chunk_t* chunk,
                                                           uint64_t               bytes_stored,
                                                           uint32_t               chunks_received,
                                                           ra8_mdl_transfer_result_t* result)
{
  if ((!chunk->has_sha256) || (chunk->total_bytes != bytes_stored)) {
    return k_ra8_err_invalid_size;
  }
  uint8_t   digest[k_ra8_mdl_sha256_bytes] = {};
  ra8_err_t err = state->config->sha256.final(state->config->sha256.ctx, digest);
  if (err != k_ra8_ok) {
    return err;
  }
  if (memcmp(digest, chunk->sha256, sizeof(digest)) != 0) {
    return k_ra8_err_checksum_mismatch;
  }
  if (state->config->storage.validate != nullptr) {
    err = state->config->storage.validate(state->config->storage.ctx, bytes_stored, digest);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  err = state->config->storage.commit(state->config->storage.ctx);
  if (err == k_ra8_ok) {
    *result = (ra8_mdl_transfer_result_t){
      .bytes_stored    = bytes_stored,
      .chunks_received = chunks_received,
    };
    memcpy(result->sha256, digest, sizeof(digest));
    state->storage_active = false;
  }
  return err;
}

ra8_err_t ra8_c6link_mdl_transfer(ra8_c6link_t*                    link,
                                  const char*                      url,
                                  const char*                      destination,
                                  const ra8_mdl_transfer_config_t* config,
                                  ra8_mdl_transfer_result_t*       result)
{
  const ra8_err_t validation =
    internal_mdl_transfer_validate(link, url, destination, config, result);
  if (validation != k_ra8_ok) {
    return validation;
  }
  *result                    = (ra8_mdl_transfer_result_t){};
  mdl_transfer_state_t state = {.link = link, .config = config};
  ra8_err_t            err   = config->storage.begin(config->storage.ctx, destination);
  if (err != k_ra8_ok) {
    return err;
  }
  state.storage_active = true;
  err                  = config->sha256.init(config->sha256.ctx);
  if (err == k_ra8_ok) {
    err = ra8_c6link_mdl_start(link, url, &state.session);
  }
  uint64_t bytes_stored = 0U;
  for (uint32_t pull = 0U; (pull < config->max_chunks) && (err == k_ra8_ok); pull++) {
    if ((config->cancel_requested != nullptr) && config->cancel_requested(config->cancel_ctx)) {
      err = k_ra8_err_cancelled;
      break;
    }
    ra8_mdl_chunk_t chunk = {};
    err                   = ra8_c6link_mdl_next(link, &state.session, config->chunk_bytes, &chunk);
    if ((err == k_ra8_ok) && (chunk.state == k_ra8_mdl_state_downloading)) {
      err = internal_mdl_transfer_store(config, &chunk, &bytes_stored);
    } else if ((err == k_ra8_ok) && (chunk.state == k_ra8_mdl_state_complete)) {
      err = internal_mdl_transfer_commit(&state, &chunk, bytes_stored, pull + 1U, result);
      return (err == k_ra8_ok) ? k_ra8_ok : internal_mdl_transfer_abort(&state, err);
    } else if ((err == k_ra8_ok) && (chunk.state == k_ra8_mdl_state_cancelled)) {
      err = k_ra8_err_cancelled;
    }
  }
  if ((err == k_ra8_ok) && state.session.active) {
    err = k_ra8_err_timeout;
  }
  return internal_mdl_transfer_abort(&state, err);
}
