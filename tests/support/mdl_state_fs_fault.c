/**
 * @file mdl_state_fs_fault.c
 * @brief Delegating portable-filesystem fault wrapper for state tests.
 * @details Injects deterministic stream and transaction failures around a real fw_fs backend.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_state_fs_fault.h"

#include <stddef.h>
#include <stdint.h>

#include "fw_if_fs_backend.h"
#include "ra8_attributes.h"

/** @brief Maximum real-backend state retained inside one wrapper workspace. */
typedef enum : uint32_t {
  k_fault_workspace_bytes = 4096U, /**< Capacity of each nested backend workspace. */
} mdl_state_fault_limit_t;

/** @brief Maximally aligned nested backend workspace. */
typedef union {
  max_align_t alignment;                      /**< Enforce natural alignment. */
  uint8_t     bytes[k_fault_workspace_bytes]; /**< Real backend state bytes.  */
} mdl_state_fault_workspace_t;

/** @brief Delegated cursor handle followed by its aligned backend workspace. */
typedef struct {
  fw_fs_dir_t directory; /**< Live wrapped cursor. */
} mdl_state_fault_directory_t;

/**
 * @brief Test whether one injected behavior is active.
 * @details Intersects the facade bitmask with one typed fault flag.
 * @param[in] fault Initialized fault facade.
 * @param[in] flag Single behavior to query.
 * @return Whether the behavior is armed.
 * @retval true The behavior is armed.
 * @pre @p fault is non-null.
 * @pre @p flag names a defined fault bit.
 * @post The facade is unchanged.
 * @post The result depends only on the current flag mask.
 * @note Combined masks are intentionally supported by callers.
 * @since v1.0.0
 */
RA8_INTERNAL static bool internal_mdl_state_fault(const mdl_state_fault_fs_t* fault,
                                                  mdl_state_fault_flag_t      flag)
{
  return (fault->flags & (uint32_t)flag) != 0U;
}

/**
 * @brief Delegate a namespace stat.
 * @details Returns deterministic stat or unavailable-media faults when armed,
 *          otherwise forwards the query unchanged to the wrapped backend.
 * @param[in] ctx Fault facade context.
 * @param[in] path Namespace path to inspect.
 * @param[out] out Result record.
 * @return Wrapped backend status.
 * @retval k_ra8_err_hw_error Stat failure injection is armed.
 * @retval k_ra8_err_hw_not_ready Media-unavailable injection is armed.
 * @pre All pointers are non-null.
 * @pre The wrapped namespace interface is initialized.
 * @post @p out contains exactly the wrapped result on success.
 * @post Injected failure does not call the wrapped backend.
 * @note Armed bits remain active until the test clears them.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_state_stat(void* ctx, const char* path, fw_fs_stat_t* out)
{
  const mdl_state_fault_fs_t* fault = (const mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_media)) {
    return k_ra8_err_hw_not_ready;
  }
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_stat)) {
    return k_ra8_err_hw_error;
  }
  return fault->inner->names.iface->stat(fault->inner->names.ctx, path, out);
}

/**
 * @brief Delegate bounded directory enumeration.
 * @details Returns deterministic list or unavailable-media faults when armed,
 *          otherwise forwards every bound and callback unchanged.
 * @param[in] ctx Fault facade context.
 * @param[in] path Directory path.
 * @param[in] max_entries Maximum entries to emit.
 * @param[in] callback Entry consumer.
 * @param[in,out] callback_ctx Consumer context.
 * @param[out] out_count Number of entries emitted.
 * @param[out] out_complete Whether enumeration reached its end.
 * @return Wrapped backend status.
 * @retval k_ra8_err_hw_error List failure injection is armed.
 * @retval k_ra8_err_hw_not_ready Media-unavailable injection is armed.
 * @pre All required pointers are non-null.
 * @pre The wrapped namespace interface is initialized.
 * @post Injected failure does not call the wrapped backend.
 * @note Armed bits remain active until the test clears them.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_list(void*           ctx,
                                                      const char*     path,
                                                      uint32_t        max_entries,
                                                      fw_fs_list_fn_t callback,
                                                      void*           callback_ctx,
                                                      uint32_t*       out_count,
                                                      bool*           out_complete)
{
  const mdl_state_fault_fs_t* fault = (const mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_media)) {
    return k_ra8_err_hw_not_ready;
  }
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_list)) {
    return k_ra8_err_hw_error;
  }
  return fault->inner->names.iface->listdir(fault->inner->names.ctx,
                                            path,
                                            max_entries,
                                            callback,
                                            callback_ctx,
                                            out_count,
                                            out_complete);
}

/**
 * @brief Open one wrapped directory cursor in trailing caller-owned storage.
 * @details Aligns the nested workspace inside the outer backend workspace and
 *          delegates through the public facade so its lifecycle checks remain active.
 * @param[in] ctx Fault facade context.
 * @param[in] path Canonical directory path.
 * @param[out] state Outer backend workspace.
 * @param[in] state_bytes Available outer workspace bytes.
 * @return Injected, capacity, or wrapped open status.
 * @retval k_ra8_err_hw_error Directory-open injection is armed.
 * @retval k_ra8_err_hw_not_ready Media-unavailable injection is armed.
 * @retval k_ra8_err_no_mem The outer workspace cannot contain the nested state.
 * @pre All pointers are non-null and the wrapped filesystem remains bound.
 * @post Success leaves one nested cursor open; failure leaves none open.
 * @note No allocator or global cursor storage is used.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_state_dir_open(void* ctx, const char* path, void* state, uint32_t state_bytes)
{
  mdl_state_fault_fs_t* fault = (mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_media)) {
    return k_ra8_err_hw_not_ready;
  }
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_dir_open)) {
    return k_ra8_err_hw_error;
  }
  const uintptr_t start        = (uintptr_t)state;
  const uintptr_t after        = start + sizeof(mdl_state_fault_directory_t);
  const uintptr_t mask         = (uintptr_t)fault->inner->caps.directory_workspace_align - 1U;
  const uintptr_t nested_start = (after + mask) & ~mask;
  const uintptr_t used         = nested_start - start;
  if ((used > state_bytes) ||
      (fault->inner->caps.directory_workspace_bytes > (state_bytes - (uint32_t)used))) {
    return k_ra8_err_no_mem;
  }
  mdl_state_fault_directory_t* nested = (mdl_state_fault_directory_t*)state;
  *nested                             = (mdl_state_fault_directory_t){};
  return fw_fs_dir_open(&fault->inner->names,
                        path,
                        &nested->directory,
                        (void*)nested_start,
                        state_bytes - (uint32_t)used);
}

/**
 * @brief Inject or delegate one stable directory-cursor step.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Open outer cursor state.
 * @param[out] out Stable copied entry value.
 * @param[out] out_entry Whether one entry was returned.
 * @return Injected or wrapped cursor status.
 * @retval k_ra8_err_hw_error Directory-next injection is armed.
 * @retval k_ra8_err_hw_not_ready Media-unavailable injection is armed.
 * @pre All pointers are non-null and @p state contains an open cursor.
 * @post Injected failure clears both outputs and leaves the cursor closable.
 * @note The wrapper retains no borrowed entry pointer.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_state_dir_next(void* ctx, void* state, fw_fs_dirent_value_t* out, bool* out_entry)
{
  mdl_state_fault_fs_t* fault = (mdl_state_fault_fs_t*)ctx;
  *out                        = (fw_fs_dirent_value_t){};
  *out_entry                  = false;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_media)) {
    return k_ra8_err_hw_not_ready;
  }
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_dir_next)) {
    return k_ra8_err_hw_error;
  }
  mdl_state_fault_directory_t* nested = (mdl_state_fault_directory_t*)state;
  return fw_fs_dir_next(&nested->directory, out, out_entry);
}

/**
 * @brief Consume a real directory cursor, then optionally report close failure.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Open outer cursor state.
 * @return Wrapped or injected close status.
 * @retval k_ra8_err_hw_error Close injection follows a successful real close.
 * @pre Both pointers are non-null and @p state contains an open cursor.
 * @post The nested cursor is consumed regardless of returned status.
 * @note Cleanup occurs before fault injection to prevent resource retention.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_dir_close(void* ctx, void* state)
{
  mdl_state_fault_fs_t*        fault  = (mdl_state_fault_fs_t*)ctx;
  mdl_state_fault_directory_t* nested = (mdl_state_fault_directory_t*)state;
  const ra8_err_t              err    = fw_fs_dir_close(&nested->directory);
  return (internal_mdl_state_fault(fault, k_mdl_state_fault_close) && (err == k_ra8_ok))
           ? k_ra8_err_hw_error
           : err;
}

/**
 * @brief Delegate one-directory creation.
 * @details Forwards the path unchanged to the wrapped namespace.
 * @param[in] ctx Fault facade context.
 * @param[in] path Directory path to create.
 * @return Wrapped backend status.
 * @retval k_ra8_ok The directory was created.
 * @pre @p ctx and @p path are non-null.
 * @pre The wrapped namespace interface is initialized.
 * @post Namespace effects match the wrapped operation.
 * @post No fault flag is consumed.
 * @note Directory creation is deliberately never faulted.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_mkdir(void* ctx, const char* path)
{
  const mdl_state_fault_fs_t* fault = (const mdl_state_fault_fs_t*)ctx;
  return fault->inner->names.iface->mkdir(fault->inner->names.ctx, path);
}

/**
 * @brief Inject or delegate regular-file removal.
 * @details Returns a deterministic error when unlink injection is armed.
 * @param[in] ctx Fault facade context.
 * @param[in] path File path to remove.
 * @return Injected or wrapped backend status.
 * @retval k_ra8_err_hw_error Unlink injection is armed.
 * @pre @p ctx and @p path are non-null.
 * @pre The wrapped namespace interface is initialized.
 * @post Injected failure leaves the wrapped namespace untouched.
 * @post Otherwise effects match the wrapped unlink.
 * @note The armed bit remains active until the test clears it.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_unlink(void* ctx, const char* path)
{
  const mdl_state_fault_fs_t* fault = (const mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_media)) {
    return k_ra8_err_hw_not_ready;
  }
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_unlink)) {
    return k_ra8_err_hw_error;
  }
  return fault->inner->names.iface->unlink(fault->inner->names.ctx, path);
}

/**
 * @brief Inject or delegate empty-directory removal.
 * @details Returns deterministic removal or unavailable-media faults when armed.
 * @param[in] ctx Fault facade context.
 * @param[in] path Empty directory path.
 * @return Wrapped backend status.
 * @retval k_ra8_err_hw_error Directory-removal injection is armed.
 * @retval k_ra8_err_hw_not_ready Media-unavailable injection is armed.
 * @pre @p ctx and @p path are non-null.
 * @pre The wrapped namespace interface is initialized.
 * @post Injected failure leaves the wrapped namespace untouched.
 * @note Armed bits remain active until the test clears them.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_rmdir(void* ctx, const char* path)
{
  const mdl_state_fault_fs_t* fault = (const mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_media)) {
    return k_ra8_err_hw_not_ready;
  }
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_rmdir)) {
    return k_ra8_err_hw_error;
  }
  return fault->inner->names.iface->rmdir(fault->inner->names.ctx, path);
}

/**
 * @brief Delegate same-volume rename.
 * @details Forwards both paths and replacement policy unchanged.
 * @param[in] ctx Fault facade context.
 * @param[in] old_path Existing namespace path.
 * @param[in] new_path Destination namespace path.
 * @param[in] replace Whether an existing destination may be replaced.
 * @return Wrapped backend status.
 * @retval k_ra8_ok Rename completed.
 * @pre All pointer parameters are non-null.
 * @pre The wrapped namespace interface is initialized.
 * @post Namespace effects match the wrapped rename.
 * @post No fault flag is consumed.
 * @note State publication does not depend on rename support.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_state_rename(void* ctx, const char* old_path, const char* new_path, bool replace)
{
  const mdl_state_fault_fs_t* fault = (const mdl_state_fault_fs_t*)ctx;
  return fault->inner->names.iface->rename(fault->inner->names.ctx, old_path, new_path, replace);
}

/**
 * @brief Delegate optional space accounting.
 * @details Reports unsupported when the wrapped backend omits space queries.
 * @param[in] ctx Fault facade context.
 * @param[out] out Space accounting result.
 * @return Wrapped or unsupported status.
 * @retval k_ra8_err_not_supported No wrapped space operation exists.
 * @pre @p ctx and @p out are non-null.
 * @pre The wrapped namespace interface is initialized.
 * @post @p out matches the wrapped result on success.
 * @post No fault flag is consumed.
 * @note Space accounting is not required by state persistence.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_space(void* ctx, fw_fs_space_t* out)
{
  const mdl_state_fault_fs_t* fault = (const mdl_state_fault_fs_t*)ctx;
  if (fault->inner->names.iface->space == nullptr) {
    return k_ra8_err_not_supported;
  }
  return fault->inner->names.iface->space(fault->inner->names.ctx, out);
}

/**
 * @brief Open a real stream in the nested aligned workspace.
 * @details Adapts facade workspace bounds to the wrapped backend state.
 * @param[in] ctx Fault facade context.
 * @param[in] path File path.
 * @param[in] mode Requested open mode.
 * @param[out] state Facade-owned stream workspace.
 * @param[in] state_bytes Available workspace bytes.
 * @return Wrapped or capacity status.
 * @retval k_ra8_err_no_mem The facade workspace is too small.
 * @pre @p ctx, @p path, and @p state are non-null.
 * @pre The wrapped stream interface is initialized.
 * @post On success the nested stream occupies @p state.
 * @post On capacity failure the wrapped open is not called.
 * @note Alignment is guaranteed by the facade capability contract.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_open(void*             ctx,
                                                      const char*       path,
                                                      fw_fs_open_mode_t mode,
                                                      void*             state,
                                                      uint32_t          state_bytes)
{
  mdl_state_fault_fs_t* fault = (mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_media)) {
    return k_ra8_err_hw_not_ready;
  }
  if (state_bytes < sizeof(mdl_state_fault_workspace_t)) {
    return k_ra8_err_no_mem;
  }
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  return fault->inner->streams.iface->open(fault->inner->streams.ctx,
                                           path,
                                           mode,
                                           nested->bytes,
                                           fault->inner->caps.file_workspace_bytes);
}

/**
 * @brief Inject failure or short progress around a real read.
 * @details Fails before delegation or caps one delegated request to one byte.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Open nested stream state.
 * @param[out] destination Read destination.
 * @param[in] capacity Destination capacity.
 * @param[out] out_read Number of bytes produced.
 * @return Injected or wrapped backend status.
 * @retval k_ra8_err_hw_error Read failure injection is armed.
 * @pre All pointers are non-null.
 * @pre @p destination has capacity @p capacity.
 * @post Injected failure sets @p out_read to zero.
 * @post Otherwise outputs match the bounded wrapped read.
 * @note Short progress tests caller retry loops.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_read(void*     ctx,
                                                      void*     state,
                                                      uint8_t*  destination,
                                                      uint32_t  capacity,
                                                      uint32_t* out_read)
{
  mdl_state_fault_fs_t* fault = (mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_read)) {
    *out_read = 0U;
    return k_ra8_err_hw_error;
  }
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_short_read) && (capacity > 1U)) {
    capacity = 1U;
  }
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  return fault->inner->streams.iface->read(fault->inner->streams.ctx,
                                           nested->bytes,
                                           destination,
                                           capacity,
                                           out_read);
}

/**
 * @brief Inject short progress or delegate a real stream write.
 * @details Caps the delegated request to one byte when armed.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Open nested stream state.
 * @param[in] source Bytes to write.
 * @param[in] length Requested byte count.
 * @param[out] out_written Number of bytes accepted.
 * @return Wrapped backend status.
 * @retval k_ra8_ok The bounded write completed.
 * @pre All pointers are non-null.
 * @pre @p source addresses at least @p length readable bytes.
 * @post Progress does not exceed the delegated request.
 * @post The input buffer is unchanged.
 * @note Stream writes are used by fixture helpers, not journal commits.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_write(void*          ctx,
                                                       void*          state,
                                                       const uint8_t* source,
                                                       uint32_t       length,
                                                       uint32_t*      out_written)
{
  mdl_state_fault_fs_t* fault = (mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_short_write) && (length > 1U)) {
    length = 1U;
  }
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  return fault->inner->streams.iface->write(fault->inner->streams.ctx,
                                            nested->bytes,
                                            source,
                                            length,
                                            out_written);
}

/**
 * @brief Delegate an absolute stream seek.
 * @details Repositions the wrapped open stream to the requested offset.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Open nested stream state.
 * @param[in] offset Absolute byte offset.
 * @return Wrapped backend status.
 * @retval k_ra8_ok The stream was repositioned.
 * @pre @p ctx and @p state are non-null.
 * @pre The nested stream is open.
 * @post On success the next access begins at @p offset.
 * @post Injected failure leaves the nested stream position unchanged.
 * @note Stream seek has an independent fault from transaction seek.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_state_stream_seek(void* ctx, void* state, uint64_t offset)
{
  mdl_state_fault_fs_t*        fault  = (mdl_state_fault_fs_t*)ctx;
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_stream_seek)) {
    return k_ra8_err_hw_error;
  }
  return fault->inner->streams.iface->seek(fault->inner->streams.ctx, nested->bytes, offset);
}

/**
 * @brief Delegate a stream-position query.
 * @details Reads the current offset from the wrapped stream.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Open nested stream state.
 * @param[out] out Current absolute byte offset.
 * @return Wrapped backend status.
 * @retval k_ra8_ok @p out contains the current offset.
 * @pre All pointers are non-null.
 * @pre The nested stream is open.
 * @post On success @p out matches the wrapped stream position.
 * @post The stream position is unchanged.
 * @note This operation is not fault injected.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_tell(void* ctx, void* state, uint64_t* out)
{
  mdl_state_fault_fs_t*        fault  = (mdl_state_fault_fs_t*)ctx;
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  return fault->inner->streams.iface->tell(fault->inner->streams.ctx, nested->bytes, out);
}

/**
 * @brief Delegate a stream-size query.
 * @details Reads the current file length from the wrapped stream.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Open nested stream state.
 * @param[out] out Current file length.
 * @return Wrapped backend status.
 * @retval k_ra8_ok @p out contains the current length.
 * @pre All pointers are non-null.
 * @pre The nested stream is open.
 * @post On success @p out matches the wrapped file length.
 * @post The stream position is unchanged.
 * @note This operation is not fault injected.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_size(void* ctx, void* state, uint64_t* out)
{
  mdl_state_fault_fs_t*        fault  = (mdl_state_fault_fs_t*)ctx;
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  return fault->inner->streams.iface->size(fault->inner->streams.ctx, nested->bytes, out);
}

/**
 * @brief Delegate optional file synchronization.
 * @details Reports unsupported when the wrapped stream omits synchronization.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Open nested stream state.
 * @return Wrapped or unsupported status.
 * @retval k_ra8_err_not_supported No wrapped sync operation exists.
 * @pre @p ctx and @p state are non-null.
 * @pre The nested stream is open.
 * @post On success wrapped file data has requested durability.
 * @post No fault flag is consumed.
 * @note Transaction commit supplies journal publication durability.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_sync(void* ctx, void* state)
{
  mdl_state_fault_fs_t* fault = (mdl_state_fault_fs_t*)ctx;
  if (fault->inner->streams.iface->sync == nullptr) {
    return k_ra8_err_not_supported;
  }
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  return fault->inner->streams.iface->sync(fault->inner->streams.ctx, nested->bytes);
}

/**
 * @brief Consume a real stream, then optionally report close failure.
 * @details Always closes the wrapped stream before applying injected status.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Open nested stream state.
 * @return Wrapped or injected close status.
 * @retval k_ra8_err_hw_error Injected failure follows a successful close.
 * @pre @p ctx and @p state are non-null.
 * @pre The nested stream is open.
 * @post The wrapped stream is consumed.
 * @post Injected failure cannot leave the real handle open.
 * @note This ordering tests load cleanup without leaking resources.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_close(void* ctx, void* state)
{
  mdl_state_fault_fs_t*        fault  = (mdl_state_fault_fs_t*)ctx;
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  const ra8_err_t              err =
    fault->inner->streams.iface->close(fault->inner->streams.ctx, nested->bytes);
  return (internal_mdl_state_fault(fault, k_mdl_state_fault_close) && (err == k_ra8_ok))
           ? k_ra8_err_hw_error
           : err;
}

/**
 * @brief Inject failure or begin a real nested transaction.
 * @details Validates facade capacity before delegating transaction creation.
 * @param[in] ctx Fault facade context.
 * @param[out] state Facade-owned transaction workspace.
 * @param[in] state_bytes Available workspace bytes.
 * @param[in] destination Destination journal generation.
 * @param[in] policy Requested publication policy.
 * @return Injected, capacity, or wrapped status.
 * @retval k_ra8_err_hw_error Begin failure injection is armed.
 * @pre @p ctx, @p state, and @p destination are non-null.
 * @pre The wrapped transaction interface is initialized.
 * @post On success a nested transaction occupies @p state.
 * @post Injected failure does not call the wrapped begin.
 * @note The journal requests create-new publication policy.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_begin(void*                      ctx,
                                                       void*                      state,
                                                       uint32_t                   state_bytes,
                                                       const char*                destination,
                                                       fw_fs_transaction_policy_t policy)
{
  mdl_state_fault_fs_t* fault = (mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_begin)) {
    return k_ra8_err_hw_error;
  }
  if (state_bytes < sizeof(mdl_state_fault_workspace_t)) {
    return k_ra8_err_no_mem;
  }
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  return fault->inner->transactions.iface->begin(fault->inner->transactions.ctx,
                                                 nested->bytes,
                                                 fault->inner->caps.transaction_workspace_bytes,
                                                 destination,
                                                 policy);
}

/**
 * @brief Inject failure or short progress around transaction output.
 * @details Fails before delegation or caps a delegated request to one byte.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Active nested transaction.
 * @param[in] source Bytes to stage.
 * @param[in] length Requested byte count.
 * @param[out] out_written Number of bytes accepted.
 * @return Injected or wrapped backend status.
 * @retval k_ra8_err_hw_error Transaction-write injection is armed.
 * @pre All pointers are non-null.
 * @pre @p source addresses at least @p length readable bytes.
 * @post Injected failure sets @p out_written to zero.
 * @post Otherwise progress does not exceed the delegated request.
 * @note Short progress qualifies the journal writer retry loop.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_transaction_write(void*          ctx,
                                                                   void*          state,
                                                                   const uint8_t* source,
                                                                   uint32_t       length,
                                                                   uint32_t*      out_written)
{
  mdl_state_fault_fs_t* fault = (mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_transaction_write)) {
    *out_written = 0U;
    return k_ra8_err_hw_error;
  }
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_short_write) && (length > 1U)) {
    length = 1U;
  }
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  return fault->inner->transactions.iface->write(fault->inner->transactions.ctx,
                                                 nested->bytes,
                                                 source,
                                                 length,
                                                 out_written);
}

/**
 * @brief Inject failure or delegate transaction backfill seek.
 * @details Repositions the stage unless seek failure injection is armed.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Active nested transaction.
 * @param[in] offset Absolute stage offset.
 * @return Injected or wrapped backend status.
 * @retval k_ra8_err_hw_error Seek failure injection is armed.
 * @pre @p ctx and @p state are non-null.
 * @pre The nested transaction is active.
 * @post Injected failure leaves the stage position unchanged.
 * @post Otherwise the wrapped seek determines stage position.
 * @note Journal headers are backfilled after payload serialization.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_state_transaction_seek(void* ctx, void* state, uint64_t offset)
{
  mdl_state_fault_fs_t* fault = (mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_seek)) {
    return k_ra8_err_hw_error;
  }
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  return fault->inner->transactions.iface->seek(fault->inner->transactions.ctx,
                                                nested->bytes,
                                                offset);
}

/**
 * @brief Inject failure or delegate independent stage validation.
 * @details Calls the wrapped validator unless validation failure is armed.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Active nested transaction.
 * @param[in] validator Independent staged-file validator.
 * @param[in,out] validator_ctx Validator context.
 * @return Injected or wrapped backend status.
 * @retval k_ra8_err_validation_failed Validation injection is armed.
 * @pre @p ctx, @p state, and @p validator are non-null.
 * @pre The nested transaction is active.
 * @post Injected failure does not invoke the validator.
 * @post Otherwise validation results match the wrapped backend.
 * @note Validation occurs before any generation becomes visible.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_validate(void*               ctx,
                                                          void*               state,
                                                          fw_fs_validate_fn_t validator,
                                                          void*               validator_ctx)
{
  mdl_state_fault_fs_t* fault = (mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_validate)) {
    return k_ra8_err_validation_failed;
  }
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  return fault->inner->transactions.iface->validate(fault->inner->transactions.ctx,
                                                    nested->bytes,
                                                    validator,
                                                    validator_ctx);
}

/**
 * @brief Inject prepublication or postpublication commit failures.
 * @details Preserves the wrapped publication truth when failing after commit.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Active nested transaction.
 * @param[out] out_published Whether the destination became visible.
 * @return Injected or wrapped backend status.
 * @retval k_ra8_err_hw_timeout A commit fault is armed at its phase.
 * @pre All pointers are non-null.
 * @pre The nested transaction is active and validated.
 * @post Precommit injection reports publication false.
 * @post Postcommit injection retains the wrapped publication result.
 * @note Publication truth takes precedence over a success-shaped API.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_commit(void* ctx, void* state, bool* out_published)
{
  mdl_state_fault_fs_t* fault = (mdl_state_fault_fs_t*)ctx;
  if (internal_mdl_state_fault(fault, k_mdl_state_fault_commit_before)) {
    *out_published = false;
    return k_ra8_err_hw_timeout;
  }
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  const ra8_err_t err = fault->inner->transactions.iface->commit(fault->inner->transactions.ctx,
                                                                 nested->bytes,
                                                                 out_published);
  return (internal_mdl_state_fault(fault, k_mdl_state_fault_commit_after) && *out_published &&
          (err == k_ra8_ok))
           ? k_ra8_err_hw_timeout
           : err;
}

/**
 * @brief Consume a real transaction, then optionally report abort failure.
 * @details Always aborts the wrapped transaction before applying injected status.
 * @param[in] ctx Fault facade context.
 * @param[in,out] state Active nested transaction.
 * @return Wrapped or injected abort status.
 * @retval k_ra8_err_cancelled Injected failure follows a successful abort.
 * @pre @p ctx and @p state are non-null.
 * @pre The nested transaction has not been consumed.
 * @post The wrapped transaction is consumed.
 * @post Injected failure cannot retain staged resources.
 * @note This ordering qualifies save cleanup error precedence.
 * @since v1.0.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_abort(void* ctx, void* state)
{
  mdl_state_fault_fs_t*        fault  = (mdl_state_fault_fs_t*)ctx;
  mdl_state_fault_workspace_t* nested = (mdl_state_fault_workspace_t*)state;
  const ra8_err_t              err =
    fault->inner->transactions.iface->abort(fault->inner->transactions.ctx, nested->bytes);
  return (internal_mdl_state_fault(fault, k_mdl_state_fault_abort) && (err == k_ra8_ok))
           ? k_ra8_err_cancelled
           : err;
}

static const fw_fs_namespace_iface_t s_names = {.stat      = internal_mdl_state_stat,
                                                .listdir   = internal_mdl_state_list,
                                                .dir_open  = internal_mdl_state_dir_open,
                                                .dir_next  = internal_mdl_state_dir_next,
                                                .dir_close = internal_mdl_state_dir_close,
                                                .mkdir     = internal_mdl_state_mkdir,
                                                .unlink    = internal_mdl_state_unlink,
                                                .rmdir     = internal_mdl_state_rmdir,
                                                .rename    = internal_mdl_state_rename,
                                                .space     = internal_mdl_state_space};

static const fw_fs_stream_iface_t s_streams = {.open  = internal_mdl_state_open,
                                               .read  = internal_mdl_state_read,
                                               .write = internal_mdl_state_write,
                                               .seek  = internal_mdl_state_stream_seek,
                                               .tell  = internal_mdl_state_tell,
                                               .size  = internal_mdl_state_size,
                                               .sync  = internal_mdl_state_sync,
                                               .close = internal_mdl_state_close};

static const fw_fs_transaction_iface_t s_transactions = {
  .begin    = internal_mdl_state_begin,
  .write    = internal_mdl_state_transaction_write,
  .seek     = internal_mdl_state_transaction_seek,
  .validate = internal_mdl_state_validate,
  .commit   = internal_mdl_state_commit,
  .abort    = internal_mdl_state_abort};

ra8_err_t mdl_state_fault_fs_init(mdl_state_fault_fs_t* fault, const fw_fs_t* inner)
{
  if ((fault == nullptr) || (inner == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if ((inner->caps.file_workspace_bytes > (uint32_t)k_fault_workspace_bytes) ||
      (inner->caps.transaction_workspace_bytes > (uint32_t)k_fault_workspace_bytes) ||
      (inner->caps.file_workspace_align > _Alignof(max_align_t)) ||
      (inner->caps.transaction_workspace_align > _Alignof(max_align_t))) {
    return k_ra8_err_no_mem;
  }
  const uint64_t directory_bytes = (uint64_t)sizeof(mdl_state_fault_directory_t) +
                                   (uint64_t)inner->caps.directory_workspace_align - 1U +
                                   (uint64_t)inner->caps.directory_workspace_bytes;
  if (directory_bytes > UINT32_MAX) {
    return k_ra8_err_no_mem;
  }
  fw_fs_caps_t caps                = inner->caps;
  caps.file_workspace_bytes        = sizeof(mdl_state_fault_workspace_t);
  caps.directory_workspace_bytes   = (uint32_t)directory_bytes;
  caps.transaction_workspace_bytes = sizeof(mdl_state_fault_workspace_t);
  caps.file_workspace_align        = _Alignof(mdl_state_fault_workspace_t);
  caps.directory_workspace_align   = _Alignof(mdl_state_fault_directory_t);
  caps.transaction_workspace_align = _Alignof(mdl_state_fault_workspace_t);
  *fault                           = (mdl_state_fault_fs_t){.inner = inner};
  return fw_fs_bind(&fault->fs, &s_names, &s_streams, &s_transactions, fault, &caps);
}
