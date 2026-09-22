/**
 * @file mdl_storage_txn.c
 * @brief Trampolines from the coordinator storage seam into one fw_if_fs
 *        transaction port.
 * @details Every callback is a guard plus one call into `fw_fs_transaction_*`.
 *          No path is built, no file is opened, and no rename is performed
 *          here: the bound port owns staging and publication.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "mdl_storage_txn.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"

/**
 * @brief Open a staging artifact for one destination through the bound port.
 * @details Rejects an overlapping transaction before the port is touched, so a
 *          failed begin never disturbs a live one.
 * @param[in,out] context Bound ::mdl_storage_txn_t.
 * @param[in] destination Portable canonical destination path.
 * @return Begin status.
 * @retval k_ra8_ok A private stage is open and appendable.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_arg The destination is empty.
 * @retval k_ra8_err_invalid_state A transaction is already live.
 * @pre @p context was initialised by ::mdl_storage_txn_init.
 * @post Success leaves the adapter writing with a zero accepted extent.
 * @post Failure preserves the complete adapter state.
 * @note Coordinator callback; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_txn_begin(void* context, const char* destination)
{
  if ((context == nullptr) || (destination == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  mdl_storage_txn_t* storage = context;
  if (destination[0] == '\0') {
    return k_ra8_err_invalid_arg;
  }
  if ((storage->state == k_mdl_storage_txn_writing)
      || (storage->state == k_mdl_storage_txn_validated)) {
    return k_ra8_err_invalid_state;
  }

  storage->transaction = (fw_fs_transaction_t){0};

  const ra8_err_t err = fw_fs_transaction_begin(storage->port,
                                                destination,
                                                storage->policy,
                                                &storage->transaction,
                                                storage->workspace,
                                                storage->workspace_size);
  if (err != k_ra8_ok) {
    storage->state = k_mdl_storage_txn_idle;
    return err;
  }

  storage->bytes_written  = 0U;
  storage->expected_bytes = 0U;
  storage->state          = k_mdl_storage_txn_writing;
  return k_ra8_ok;
}

/**
 * @brief Append one transfer fragment to the private stage.
 * @details Reports exactly what the port accepted. A port claiming more than
 *          it was offered violates its own contract and is refused as
 *          ::k_ra8_err_invalid_state rather than narrowed silently.
 * @param[in,out] context Bound ::mdl_storage_txn_t.
 * @param[in] data Ordered source bytes.
 * @param[in] length Requested byte count.
 * @param[out] written Exact accepted byte count.
 * @return Append status.
 * @retval k_ra8_ok Bytes were appended and reported.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_state No stage is open, or the port over-reported.
 * @pre Nonzero @p length implies @p data spans that many readable bytes.
 * @post Failure reports zero written and leaves the stage open for abort.
 * @note Coordinator callback; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_txn_write(void* context, const uint8_t* data, uint16_t length, uint16_t* written)
{
  if ((context == nullptr) || (written == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *written = 0U;
  if ((data == nullptr) && (length != 0U)) {
    return k_ra8_err_null_ptr;
  }
  mdl_storage_txn_t* storage = context;
  if (storage->state != k_mdl_storage_txn_writing) {
    return k_ra8_err_invalid_state;
  }

  uint32_t        accepted = 0U;
  const ra8_err_t err
      = fw_fs_transaction_write(&storage->transaction, data, (uint32_t)length, &accepted);
  if (err != k_ra8_ok) {
    return err;
  }
  if (accepted > (uint32_t)length) {
    return k_ra8_err_invalid_state;
  }

  storage->bytes_written += (uint64_t)accepted;
  *written = (uint16_t)accepted;
  return k_ra8_ok;
}

/**
 * @brief Run the caller's check against the open stage.
 * @details Called by the port once the stage is flushed and reopened
 *          read-only. A null caller validator selects the length check only.
 * @param[in,out] context Bound ::mdl_storage_txn_t.
 * @param[in,out] staged Read-only handle on the complete stage.
 * @return Validation status.
 * @retval k_ra8_ok The artifact may be published.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_size The stage length disagrees with the transfer.
 * @pre The port refuses writes and seeks after this returns successfully.
 * @post No publication or deletion happens here.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_txn_validate_stage(void* context, fw_fs_file_t* staged)
{
  if ((context == nullptr) || (staged == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  mdl_storage_txn_t* storage = context;

  uint64_t        size = 0U;
  const ra8_err_t err  = fw_fs_file_size(staged, &size);
  if (err != k_ra8_ok) {
    return err;
  }
  if (size != storage->expected_bytes) {
    return k_ra8_err_invalid_size;
  }
  if (storage->validate == nullptr) {
    return k_ra8_ok;
  }
  return storage->validate(storage->validate_ctx,
                           staged,
                           storage->expected_bytes,
                           storage->expected_sha256);
}

/**
 * @brief Validate the private artifact before publication.
 * @details Checks the coordinator's byte count against what the port actually
 *          accepted, then delegates the artifact check to the port so the
 *          caller sees an open handle instead of a path.
 * @param[in,out] context Bound ::mdl_storage_txn_t.
 * @param[in] total_bytes Verified object byte length.
 * @param[in] sha256 Independently verified digest.
 * @return Validation status.
 * @retval k_ra8_ok The stage is validated and commit is available.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_state No open stage exists.
 * @retval k_ra8_err_invalid_size Accepted bytes disagree with @p total_bytes.
 * @pre The transport verified length and digest before this call.
 * @post Failure leaves the stage open so the coordinator can abort it.
 * @note Coordinator callback; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_txn_validate(void*         context,
                                                    uint64_t      total_bytes,
                                                    const uint8_t sha256[k_ra8_mdl_sha256_bytes])
{
  if ((context == nullptr) || (sha256 == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  mdl_storage_txn_t* storage = context;
  if (storage->state != k_mdl_storage_txn_writing) {
    return k_ra8_err_invalid_state;
  }
  if (total_bytes != storage->bytes_written) {
    return k_ra8_err_invalid_size;
  }

  storage->expected_bytes = total_bytes;
  (void)memcpy(storage->expected_sha256, sha256, sizeof(storage->expected_sha256));

  const ra8_err_t err
      = fw_fs_transaction_validate(&storage->transaction, internal_txn_validate_stage, storage);
  if (err != k_ra8_ok) {
    return err;
  }

  storage->state = k_mdl_storage_txn_validated;
  return k_ra8_ok;
}

/**
 * @brief Publish the validated stage through the bound port.
 * @details A port reporting success without publication contradicts the
 *          fw_if_fs contract; that is refused and the transaction is left live
 *          so the coordinator can abort it.
 * @param[in,out] context Bound ::mdl_storage_txn_t.
 * @return Commit status.
 * @retval k_ra8_ok The destination is published.
 * @retval k_ra8_err_null_ptr @p context is NULL.
 * @retval k_ra8_err_invalid_state Not validated, or publication was not made.
 * @pre The stage passed ::internal_txn_validate.
 * @post Success leaves the adapter committed with no live transaction.
 * @note Coordinator callback; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_txn_commit(void* context)
{
  if (context == nullptr) {
    return k_ra8_err_null_ptr;
  }
  mdl_storage_txn_t* storage = context;
  if (storage->state != k_mdl_storage_txn_validated) {
    return k_ra8_err_invalid_state;
  }

  bool            published = false;
  const ra8_err_t err       = fw_fs_transaction_commit(&storage->transaction, &published);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!published) {
    return k_ra8_err_invalid_state;
  }

  storage->state = k_mdl_storage_txn_committed;
  return k_ra8_ok;
}

/**
 * @brief Destroy the unpublished stage through the bound port.
 * @details Valid after every successful begin, including after a failed
 *          validate or commit.
 * @param[in,out] context Bound ::mdl_storage_txn_t.
 * @return Abort status.
 * @retval k_ra8_ok The stage was removed.
 * @retval k_ra8_err_null_ptr @p context is NULL.
 * @retval k_ra8_err_invalid_state No live transaction exists.
 * @post The adapter is idle whether or not the port reported success, because
 *       the port consumes the handle either way.
 * @note Coordinator callback; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_txn_abort(void* context)
{
  if (context == nullptr) {
    return k_ra8_err_null_ptr;
  }
  mdl_storage_txn_t* storage = context;
  if ((storage->state != k_mdl_storage_txn_writing)
      && (storage->state != k_mdl_storage_txn_validated)) {
    return k_ra8_err_invalid_state;
  }

  const ra8_err_t err = fw_fs_transaction_abort(&storage->transaction);
  storage->state      = k_mdl_storage_txn_idle;
  storage->bytes_written  = 0U;
  storage->expected_bytes = 0U;
  return err;
}

ra8_err_t mdl_storage_txn_init(mdl_storage_txn_t*           storage,
                               const mdl_storage_txn_cfg_t* config,
                               ra8_mdl_storage_iface_t*     out_iface)
{
  if ((storage == nullptr) || (config == nullptr) || (out_iface == nullptr)
      || (config->port == nullptr) || (config->workspace == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (config->workspace_size == 0U) {
    return k_ra8_err_invalid_size;
  }

  *storage = (mdl_storage_txn_t){
    .port           = config->port,
    .workspace      = config->workspace,
    .validate       = config->validate,
    .validate_ctx   = config->validate_ctx,
    .workspace_size = config->workspace_size,
    .policy         = config->policy,
    .state          = k_mdl_storage_txn_idle,
  };

  *out_iface = (ra8_mdl_storage_iface_t){
    .begin    = internal_txn_begin,
    .write    = internal_txn_write,
    .validate = internal_txn_validate,
    .commit   = internal_txn_commit,
    .abort    = internal_txn_abort,
    .ctx      = storage,
  };
  return k_ra8_ok;
}

ra8_err_t mdl_storage_txn_state(const mdl_storage_txn_t* storage,
                                mdl_storage_txn_state_t* out_state)
{
  if ((storage == nullptr) || (out_state == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_state = storage->state;
  return k_ra8_ok;
}
