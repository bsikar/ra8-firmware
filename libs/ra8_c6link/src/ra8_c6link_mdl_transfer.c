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
#include "ra8_c6link_mdl_transfer_internal.h"

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
internal_mdl_transfer_validate(const ra8_c6link_t*              link,
                               const char*                      url,
                               const char*                      destination,
                               const ra8_mdl_transfer_config_t* config,
                               const ra8_mdl_transfer_result_t* result)
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
  if ((uint32_t)config->format > (uint32_t)k_mdl_format_rabook) {
    return k_ra8_err_invalid_arg;
  }
  if ((config->format != k_mdl_format_loose) && (config->storage.validate == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const uint64_t byte_budget = (uint64_t)config->chunk_bytes * config->max_chunks;
  if ((config->chunk_bytes == 0U) || (config->chunk_bytes > k_ra8_mdl_chunk_data_max) ||
      (config->max_chunks == 0U) || (byte_budget > k_ra8_mdl_transfer_bytes_max)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Cancel the remote job when active and always abort local temporary
 * state
 * @details Cleanup failures are returned only when no earlier cause exists.
 * @param[in,out] state Transfer resources to unwind.
 * @param[in] cause Original failure, or success when cleanup itself triggered
 * the result.
 * @return Original cause, then cancel error, then abort error in priority
 * order.
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
      .format          = state->config->format,
      .response        = chunk->response,
    };
    memcpy(result->sha256, digest, sizeof(digest));
    state->storage_active = false;
  }
  return err;
}

RA8_TEST_HELPER ra8_err_t
ra8_c6link_mdl_transfer_commit_test(const ra8_mdl_transfer_config_t* config,
                                    const ra8_mdl_chunk_t*           chunk,
                                    uint64_t                         bytes_stored,
                                    uint32_t                         chunks_received,
                                    ra8_mdl_transfer_result_t*       result)
{
  mdl_transfer_state_t state = {.config = config, .storage_active = true};
  return internal_mdl_transfer_commit(&state, chunk, bytes_stored, chunks_received, result);
}

/**
 * @brief Validate configuration, then start local storage and the remote job
 * @details Splits the transactional preamble from the per-chunk pull loop so
 * ::ra8_c6link_mdl_transfer stays within its statement budget. Zeroes
 * @p result once validation passes, and leaves `state->storage_active` false
 * on any failure that precedes a successful `storage.begin` -- the caller
 * uses that flag to decide whether cleanup is required.
 * @param[in] link Open c6link handle to bind into @p state.
 * @param[in] url Source URL for the remote StartRequest.
 * @param[in] destination Local destination handed to storage.begin.
 * @param[in] config Injected storage/hash/transport configuration.
 * @param[out] result Zeroed once configuration validation passes.
 * @param[out] state Prepared transfer state for the pull loop or cleanup.
 * @return Status to return immediately, or k_ra8_ok to enter the pull loop.
 * @retval k_ra8_ok The remote job is started; the pull loop may proceed.
 * @retval other Validation, storage, hash, or transport start-up failed.
 * @pre @p link, @p url, @p destination, @p config, and @p result are non-null.
 * @pre @p state has not been used by an earlier transfer.
 * @post `state->storage_active` is true if and only if `config->storage.begin`
 * returned `k_ra8_ok`.
 * @post On validation failure, neither storage nor the remote job is touched.
 * @note Not thread-safe; it mutates injected storage/hash contexts.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_transfer_begin(ra8_c6link_t* link,
                                                          const char*   url,
                                                          const char*   destination,
                                                          const ra8_mdl_transfer_config_t* config,
                                                          ra8_mdl_transfer_result_t*       result,
                                                          mdl_transfer_state_t*            state)
{
  const ra8_err_t validation =
    internal_mdl_transfer_validate(link, url, destination, config, result);
  if (validation != k_ra8_ok) {
    return validation;
  }
  *result       = (ra8_mdl_transfer_result_t){};
  state->link   = link;
  state->config = config;
  ra8_err_t err = config->storage.begin(config->storage.ctx, destination);
  if (err != k_ra8_ok) {
    return err;
  }
  state->storage_active = true;
  err                   = config->sha256.init(config->sha256.ctx);
  if (err == k_ra8_ok) {
    const ra8_mdl_request_t request = {
      .url    = url,
      .format = config->format,
      .http   = config->http,
    };
    err = ra8_c6link_mdl_start_request(link, &request, &state->session);
  }
  return err;
}

ra8_err_t ra8_c6link_mdl_transfer(ra8_c6link_t*                    link,
                                  const char*                      url,
                                  const char*                      destination,
                                  const ra8_mdl_transfer_config_t* config,
                                  ra8_mdl_transfer_result_t*       result)
{
  mdl_transfer_state_t state = {};
  ra8_err_t err = internal_mdl_transfer_begin(link, url, destination, config, result, &state);
  if (!state.storage_active) {
    return err;
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
      // mcdc-deactivated: ra8_c6link_mdl_transfer CANCELLED dispatch guard; whenever this arm is evaluated with err == k_ra8_ok the state is necessarily CANCELLED, so the second operand cannot be flipped. DOWNLOADING and COMPLETE are consumed by the two preceding arms, ACCEPTED and every unassigned state value are rejected by internal_mdl_chunk_semantics_valid's default arm before ra8_c6link_mdl_next returns, and FAILED must carry a nonzero status by that same validator, which internal_mdl_accept_chunk returns verbatim -- so a FAILED chunk always arrives with err != k_ra8_ok.
    } else if ((err == k_ra8_ok) && (chunk.state == k_ra8_mdl_state_cancelled)) {
      err = k_ra8_err_cancelled;
    }
  }
  // mcdc-deactivated: ra8_c6link_mdl_transfer exhausted-budget timeout guard; `state.session.active` is constant-true whenever err == k_ra8_ok here. The loop leaves err == k_ra8_ok only by exhausting max_chunks, and the session is deactivated only by a terminal response -- COMPLETE returns from inside the loop, CANCELLED sets err = k_ra8_err_cancelled, and FAILED returns its mandatory nonzero status -- so no reachable path arrives here with an inactive session and no earlier cause.
  if ((err == k_ra8_ok) && state.session.active) {
    err = k_ra8_err_timeout;
  }
  return internal_mdl_transfer_abort(&state, err);
}
