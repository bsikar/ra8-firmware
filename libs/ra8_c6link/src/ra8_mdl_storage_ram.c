/**
 * @file ra8_mdl_storage_ram.c
 * @brief Caller-buffer transaction adapter for C6 media source bytes.
 * @details Implements private begin/write state, explicit logical publication,
 *          and failure cleanup over storage supplied entirely by the caller.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_mdl_storage_ram.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"

/**
 * @brief Start a fresh private object in the bound caller buffer.
 * @details Rejects overlapping transactions and resets the logical extent only
 *          after validating the nonempty coordinator destination label.
 * @param[in,out] context Bound ::ra8_mdl_storage_ram_t.
 * @param[in] destination Nonempty coordinator destination label.
 * @return Begin status.
 * @retval k_ra8_ok The private object is empty and writable.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_arg The destination label is empty.
 * @retval k_ra8_err_invalid_state A transaction is already active.
 * @pre @p context was initialized by ::ra8_mdl_storage_ram_init.
 * @pre The backing buffer is exclusively owned for the transfer.
 * @post Success clears the prior committed extent and marks storage active.
 * @post Failure preserves the complete adapter state.
 * @note Coordinator callback; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_ram_begin(void* context, const char* destination)
{
  if ((context == nullptr) || (destination == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  ra8_mdl_storage_ram_t* storage = context;
  if (destination[0] == '\0') {
    return k_ra8_err_invalid_arg;
  }
  if (storage->active) {
    return k_ra8_err_invalid_state;
  }
  storage->length    = 0U;
  storage->committed = false;
  storage->active    = true;
  return k_ra8_ok;
}

/**
 * @brief Append one complete transfer fragment to private RAM storage.
 * @details Proves addition and capacity before copying, then reports progress
 *          exactly equal to the accepted fragment length.
 * @param[in,out] context Bound ::ra8_mdl_storage_ram_t.
 * @param[in] data Ordered source bytes.
 * @param[in] length Requested byte count.
 * @param[out] written Exact accepted byte count.
 * @return Append status.
 * @retval k_ra8_ok Every byte was copied and reported.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_state No transaction is active.
 * @retval k_ra8_err_no_mem The complete fragment exceeds remaining capacity.
 * @pre Nonzero @p length implies @p data spans that many readable bytes.
 * @pre @p written is writable and the context is exclusively owned.
 * @post Success advances the private extent by exactly @p length.
 * @post Failure reports zero and preserves initialized bytes and extent.
 * @note Coordinator callback; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_ram_write(void* context, const uint8_t* data, uint16_t length, uint16_t* written)
{
  if ((context == nullptr) || (written == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *written = 0U;
  if ((data == nullptr) && (length != 0U)) {
    return k_ra8_err_null_ptr;
  }
  ra8_mdl_storage_ram_t* storage = context;
  if (!storage->active) {
    return k_ra8_err_invalid_state;
  }
  if ((size_t)length > (storage->capacity - storage->length)) {
    return k_ra8_err_no_mem;
  }
  if (length != 0U) {
    (void)memcpy(&storage->data[storage->length], data, length);
  }
  storage->length += length;
  *written = length;
  return k_ra8_ok;
}

/**
 * @brief Publish the current private extent to the caller.
 * @details Publication is a state transition only; the bytes already reside in
 *          caller memory and no pointer escapes until the view operation.
 * @param[in,out] context Bound ::ra8_mdl_storage_ram_t.
 * @return Commit status.
 * @retval k_ra8_ok A nonempty immutable view is now available.
 * @retval k_ra8_err_null_ptr @p context is NULL.
 * @retval k_ra8_err_invalid_state No active nonempty transaction exists.
 * @pre The coordinator verified the transfer byte count and digest.
 * @pre No caller reads the backing buffer while active.
 * @post Success clears active and sets committed without changing bytes.
 * @post Failure preserves the active private transaction for abort.
 * @note Coordinator callback; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_ram_commit(void* context)
{
  if (context == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_mdl_storage_ram_t* storage = context;
  if ((!storage->active) || (storage->length == 0U)) {
    return k_ra8_err_invalid_state;
  }
  storage->active    = false;
  storage->committed = true;
  return k_ra8_ok;
}

/**
 * @brief Discard the logical extent of one active private object.
 * @details Clears lifecycle metadata without spending time overwriting the
 *          caller buffer; no view is available after the operation.
 * @param[in,out] context Bound ::ra8_mdl_storage_ram_t.
 * @return Abort status.
 * @retval k_ra8_ok Active private state was discarded.
 * @retval k_ra8_err_null_ptr @p context is NULL.
 * @retval k_ra8_err_invalid_state No transaction is active.
 * @pre The coordinator previously completed begin successfully.
 * @pre No caller reads the private buffer.
 * @post Success leaves zero length, inactive, and uncommitted.
 * @post Failure preserves adapter state.
 * @note Buffer contents become unspecified private scratch after success.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_ram_abort(void* context)
{
  if (context == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_mdl_storage_ram_t* storage = context;
  if (!storage->active) {
    return k_ra8_err_invalid_state;
  }
  storage->length    = 0U;
  storage->active    = false;
  storage->committed = false;
  return k_ra8_ok;
}

/* data cannot be const: it seeds the struct's non-const uint8_t* data
 * field, which internal_ram_write() later writes through. */
ra8_err_t ra8_mdl_storage_ram_init(ra8_mdl_storage_ram_t*   storage,
                                   ra8_mdl_storage_iface_t* output,
                                   // NOLINTNEXTLINE(readability-non-const-parameter)
                                   uint8_t* data,
                                   size_t   capacity)
{
  if ((storage == nullptr) || (output == nullptr) || (data == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (capacity == 0U) {
    return k_ra8_err_invalid_size;
  }
  *storage = (ra8_mdl_storage_ram_t){.data = data, .capacity = capacity};
  *output  = (ra8_mdl_storage_iface_t){.begin    = internal_ram_begin,
                                       .write    = internal_ram_write,
                                       .validate = nullptr,
                                       .commit   = internal_ram_commit,
                                       .abort    = internal_ram_abort,
                                       .ctx      = storage};
  return k_ra8_ok;
}

ra8_err_t
ra8_mdl_storage_ram_view(const ra8_mdl_storage_ram_t* storage, const uint8_t** data, size_t* length)
{
  if ((storage == nullptr) || (data == nullptr) || (length == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((!storage->committed) || storage->active || (storage->length == 0U)) {
    return k_ra8_err_invalid_state;
  }
  *data   = storage->data;
  *length = storage->length;
  return k_ra8_ok;
}
