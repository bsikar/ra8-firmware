/**
 * @file mdl_fetch_body.c
 * @brief Portable single-transaction response-body sink implementation.
 * @details Streams network body fragments into one injected transaction and
 *          validates complete content before publication.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "mdl_fetch_body_internal.h"
#include "mdl_urlname.h"

/**
 * @brief Replace both requested suffixes with one verified image suffix.
 * @details Formats absolute and relative destinations from their last suffix.
 * @param[in,out] body Initialized magic-typed response body.
 * @param[in] extension Verified suffix without a leading dot.
 * @return Whether both bounded destinations fit.
 * @retval true Both paths were written completely.
 * @retval false A suffix was absent or one result exceeded capacity.
 * @pre @p body and @p extension are non-NULL.
 * @pre Both source paths remain live and NUL-terminated.
 * @post Success initializes both actual paths.
 * @post Failure never begins or publishes a transaction.
 * @note The requested source paths are not modified.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_body_paths(mdl_fetch_body_t* body, const char* extension)
{
  const char* abs_dot = strrchr(body->target_abs, '.');
  const char* rel_dot = strrchr(body->target_rel, '.');
  if ((abs_dot == nullptr) || (rel_dot == nullptr) || (extension[0] == '\0')) {
    return false;
  }
  const int abs_length = snprintf(body->actual_abs,
                                  sizeof(body->actual_abs),
                                  "%.*s.%s",
                                  (int)(abs_dot - body->target_abs),
                                  body->target_abs,
                                  extension);
  const int rel_length = snprintf(body->actual_rel,
                                  sizeof(body->actual_rel),
                                  "%.*s.%s",
                                  (int)(rel_dot - body->target_rel),
                                  body->target_rel,
                                  extension);
  return (abs_length > 0) && ((size_t)abs_length < sizeof(body->actual_abs)) && (rel_length > 0) &&
         ((size_t)rel_length < sizeof(body->actual_rel));
}

/**
 * @brief Begin the exact or classified destination transaction once.
 * @details Copies an exact destination or classifies buffered image magic,
 *          then delegates truthful create/replace policy to storage.
 * @param[in,out] body Initialized response body.
 * @return Canonical classification, path, or transaction status.
 * @retval k_ra8_ok A transaction was already active or is now active.
 * @retval k_ra8_err_validation_failed Buffered image magic is unsupported.
 * @pre @p body is non-NULL and owns an initialized storage binding.
 * @pre Required source path strings remain live and NUL-terminated.
 * @post Success leaves exactly one private transaction active.
 * @post Failure publishes no destination.
 * @note Repeated calls while active are idempotent.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_body_begin(mdl_fetch_body_t* body)
{
  if (body->writer.transaction.active) {
    return k_ra8_ok;
  }
  if (body->mode == k_mdl_fetch_body_exact) {
    const int copied = snprintf(body->actual_abs, sizeof(body->actual_abs), "%s", body->target_abs);
    if ((copied < 0) || ((size_t)copied >= sizeof(body->actual_abs))) {
      return k_ra8_err_invalid_size;
    }
  } else {
    char extension[8] = {};
    if (!mdl_urlname_sniff_image_type(body->prefix,
                                      body->prefix_bytes,
                                      nullptr,
                                      extension,
                                      sizeof(extension),
                                      nullptr,
                                      0U)) {
      return k_ra8_err_validation_failed;
    }
    if (!internal_body_paths(body, extension)) {
      return k_ra8_err_invalid_size;
    }
  }
  return mdl_storage_txn_begin(&body->writer, body->storage, body->actual_abs);
}

/**
 * @brief Flush retained signature bytes into a newly active stage.
 * @details Begins the selected transaction and writes the deferred prefix once.
 * @param[in,out] body Initialized response body with a bounded prefix.
 * @return Canonical classification, begin, or write status.
 * @retval k_ra8_ok The prefix is empty and its transaction is active.
 * @retval other The underlying classification or transaction operation failed.
 * @pre @p body is non-NULL and owns initialized storage.
 * @pre Retained prefix length does not exceed its fixed capacity.
 * @post Success clears the retained prefix count.
 * @post Failure publishes no destination and leaves cleanup to the caller.
 * @note Prefix bytes are folded into the same running identity as later bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_body_flush_prefix(mdl_fetch_body_t* body)
{
  ra8_err_t error = internal_body_begin(body);
  if ((error == k_ra8_ok) && (body->prefix_bytes != 0U)) {
    error = mdl_storage_txn_write(&body->writer, body->prefix, body->prefix_bytes);
    if (error == k_ra8_ok) {
      body->prefix_bytes = 0U;
    }
  }
  return error;
}

/**
 * @brief Abort a prior retry attempt without beginning a new one.
 * @details Implements the network reset callback over caller-owned body state.
 * @param[in,out] context Borrowed ::mdl_fetch_body_t context.
 * @return Canonical argument or transaction-abort status.
 * @retval k_ra8_ok No private stage or accepted response byte remains.
 * @retval k_ra8_err_invalid_arg The context is NULL.
 * @pre A non-NULL context points to initialized writable body state.
 * @pre The body is exclusively owned for the callback duration.
 * @post Success resets paths, prefix length, and accepted-byte count.
 * @post Failure leaves the abort error and diagnostic transaction state visible.
 * @note Reset never creates a transaction, including before a 304 response.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_body_reset(void* context)
{
  mdl_fetch_body_t* body = (mdl_fetch_body_t*)context;
  if (body == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t error = mdl_storage_txn_abort(&body->writer);
  if (error != k_ra8_ok) {
    return error;
  }
  body->prefix_bytes  = 0U;
  body->received      = 0U;
  body->actual_abs[0] = '\0';
  body->actual_rel[0] = '\0';
  return k_ra8_ok;
}

/**
 * @brief Retain the bounded image prefix and return consumed source bytes.
 * @details Copies only the remaining portion of the fixed magic-prefix buffer.
 * @param[in,out] body Initialized image body.
 * @param[in] bytes Readable network bytes.
 * @param[in] length Available source extent.
 * @return Number of bytes copied into the prefix.
 * @retval 0 The prefix was already full or the source was empty.
 * @pre @p body and @p bytes are non-NULL for nonempty input.
 * @pre Existing prefix length is within its fixed capacity.
 * @post Prefix length advances by the returned count.
 * @post No transaction or persistent destination is mutated.
 * @note The caller handles all source bytes after the returned prefix extent.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_body_buffer_prefix(mdl_fetch_body_t* body, const uint8_t* bytes, uint32_t length)
{
  const uint32_t room = (uint32_t)k_mdl_fetch_magic_bytes - (uint32_t)body->prefix_bytes;
  const uint32_t take = (length < room) ? length : room;
  memcpy(body->prefix + body->prefix_bytes, bytes, take);
  body->prefix_bytes = (uint8_t)((uint32_t)body->prefix_bytes + take);
  return take;
}

/**
 * @brief Consume one exact or magic-typed network body chunk.
 * @details Defers a bounded magic prefix, begins one destination transaction,
 *          and appends every accepted byte through the running identity writer.
 * @param[in,out] context Borrowed ::mdl_fetch_body_t context.
 * @param[in] bytes Readable source bytes or NULL for an empty chunk.
 * @param[in] length Source extent.
 * @param[out] out_written Exact accepted extent on success.
 * @return Canonical argument, classification, or transaction status.
 * @retval k_ra8_ok The complete chunk was accepted.
 * @retval k_ra8_err_invalid_arg A required pointer contract is invalid.
 * @pre Non-NULL context points to initialized, exclusively owned body state.
 * @pre @p bytes covers @p length bytes whenever length is nonzero.
 * @post Success advances received bytes by exactly @p length.
 * @post Failure reports zero current-chunk progress and publishes nothing.
 * @note Empty writes are no-ops and do not begin a transaction.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_body_write(void* context, const uint8_t* bytes, uint32_t length, uint32_t* out_written)
{
  mdl_fetch_body_t* body = (mdl_fetch_body_t*)context;
  if ((body == nullptr) || (out_written == nullptr) || ((bytes == nullptr) && (length != 0U))) {
    return k_ra8_err_invalid_arg;
  }
  *out_written = 0U;
  if (length == 0U) {
    return k_ra8_ok;
  }
  uint32_t offset = 0U;
  if ((body->mode == k_mdl_fetch_body_image) && !body->writer.transaction.active) {
    offset = internal_body_buffer_prefix(body, bytes, length);
    if (body->prefix_bytes < (uint8_t)k_mdl_fetch_magic_bytes) {
      body->received += (uint64_t)length;
      *out_written = length;
      return k_ra8_ok;
    }
    const ra8_err_t error = internal_body_flush_prefix(body);
    if (error != k_ra8_ok) {
      return error;
    }
  } else if (!body->writer.transaction.active) {
    const ra8_err_t error = internal_body_begin(body);
    if (error != k_ra8_ok) {
      return error;
    }
  }
  const ra8_err_t error = mdl_storage_txn_write(&body->writer, bytes + offset, length - offset);
  if (error == k_ra8_ok) {
    body->received += (uint64_t)length;
    *out_written = length;
  }
  return error;
}

ra8_err_t priv_mdl_fetch_body_init_exact(mdl_fetch_body_t* body,
                                         mdl_storage_t*    storage,
                                         const char*       target_abs)
{
  if ((body == nullptr) || (storage == nullptr) || (target_abs == nullptr) ||
      (target_abs[0] != '/')) {
    return k_ra8_err_invalid_arg;
  }
  *body = (mdl_fetch_body_t){.storage    = storage,
                             .target_abs = target_abs,
                             .target_rel = nullptr,
                             .mode       = k_mdl_fetch_body_exact};
  return k_ra8_ok;
}

ra8_err_t priv_mdl_fetch_body_init_image(mdl_fetch_body_t* body,
                                         mdl_storage_t*    storage,
                                         const char*       target_abs,
                                         const char*       target_rel)
{
  if ((body == nullptr) || (storage == nullptr) || (target_abs == nullptr) ||
      (target_rel == nullptr) || (target_abs[0] != '/')) {
    return k_ra8_err_invalid_arg;
  }
  *body = (mdl_fetch_body_t){.storage    = storage,
                             .target_abs = target_abs,
                             .target_rel = target_rel,
                             .mode       = k_mdl_fetch_body_image};
  return k_ra8_ok;
}

mdl_net_body_sink_t priv_mdl_fetch_body_sink(mdl_fetch_body_t* body)
{
  return (
    mdl_net_body_sink_t){.reset = internal_body_reset, .write = internal_body_write, .ctx = body};
}

ra8_err_t priv_mdl_fetch_body_prepare(mdl_fetch_body_t* body)
{
  if ((body == nullptr) || (body->storage == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (body->received == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (!body->writer.transaction.active) {
    return internal_body_flush_prefix(body);
  }
  return k_ra8_ok;
}

ra8_err_t priv_mdl_fetch_body_commit(mdl_fetch_body_t* body)
{
  if ((body == nullptr) || !body->writer.transaction.active) {
    return k_ra8_err_invalid_arg;
  }
  return mdl_storage_txn_commit(&body->writer);
}

ra8_err_t priv_mdl_fetch_body_abort(mdl_fetch_body_t* body)
{
  if (body == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t error = mdl_storage_txn_abort(&body->writer);
  if (error == k_ra8_ok) {
    body->prefix_bytes = 0U;
    body->received     = 0U;
  }
  return error;
}
