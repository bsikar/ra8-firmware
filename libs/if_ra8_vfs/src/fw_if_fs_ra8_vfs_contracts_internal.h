/**
 * @file fw_if_fs_ra8_vfs_contracts_internal.h
 * @brief File-local contracts for the firmware VFS filesystem adapter.
 *
 * @details
 * Declares the adapter's static namespace, stream, and transaction helpers so
 * their complete contracts remain readable without forcing the implementation
 * translation unit beyond the repository size ceiling. This header is private
 * to fw_if_fs_ra8_vfs.c and does not widen any symbol's linkage.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#pragma once

#include "fw_if_fs_ra8_vfs.h"

/**
 * @brief Translate one decoded FAT/exFAT civil timestamp without an epoch.
 * @details Copies valid civil fields and converts centiseconds to nanoseconds;
 *          invalid native timestamps remain a fully zeroed portable value.
 * @param[in] native Decoded filesystem timestamp.
 * @return Portable civil timestamp and validity metadata.
 * @retval fw_fs_timestamp_t{} @p native is not valid.
 * @retval fw_fs_timestamp_t A field-preserving valid timestamp otherwise.
 * @pre @p native addresses one readable timestamp object.
 * @pre Native civil fields were validated by the mounted filesystem parser.
 * @post No native or external state is modified.
 * @post A valid result preserves UTC-offset validity independently.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
static fw_fs_timestamp_t internal_timestamp(const ra8_fs_timestamp_t* native);

/**
 * @brief Measure a string without reading beyond a fixed byte cap.
 * @details Scans for the first NUL and returns @p cap when no terminator occurs
 *          within the readable bound.
 * @param[in] text Candidate string bytes.
 * @param[in] cap Maximum readable and returnable byte count.
 * @return Bytes preceding the first NUL, or @p cap.
 * @retval 0 The first byte is NUL or @p cap is zero.
 * @retval cap No NUL occurs in the bounded span.
 * @pre @p text addresses at least @p cap readable bytes.
 * @pre The source remains stable throughout the bounded scan.
 * @post No source or external state is modified.
 * @post The result never exceeds @p cap.
 * @note Pure and thread-safe for immutable input.
 * @since Version 0.1.0
 */
static uint16_t internal_len(const char* text, uint16_t cap);

/**
 * @brief Prefix one portable path with the bound VFS mount name.
 * @details Builds `mount:/path` in caller-owned adapter scratch only after
 *          proving both inputs are terminated within their fixed capacities.
 * @param[in,out] state Bound adapter state containing the mount name.
 * @param[in] path Validated portable rooted path.
 * @param[out] out Destination scratch for the native VFS path.
 * @return Path-construction status.
 * @retval k_ra8_ok The complete native path including NUL was written.
 * @retval k_ra8_err_invalid_state The stored mount name is unterminated.
 * @retval k_ra8_err_invalid_size The portable path is unterminated.
 * @pre @p out has capacity for mount name, colon, path, and NUL.
 * @pre Public dispatch already validated @p path against binding limits.
 * @post Success writes one terminated native path to @p out.
 * @post Input strings and mount metadata are unchanged.
 * @note Not thread-safe when @p out is shared adapter scratch.
 * @since Version 0.1.0
 */
static ra8_err_t internal_full_path(fw_fs_ra8_vfs_state_t* state, const char* path, char* out);

/**
 * @brief Convert one VFS stat result to portable metadata.
 * @details Prefixes the bound mount, queries native metadata, translates civil
 *          timestamps, and maps existence/type without exposing native types.
 * @param[in,out] ctx Bound ::fw_fs_ra8_vfs_state_t adapter context.
 * @param[in] path Validated portable path.
 * @param[out] out Portable metadata destination.
 * @return Native path/stat or conversion status.
 * @retval k_ra8_ok @p out contains the native query result.
 * @retval k_ra8_err_invalid_state Bound path metadata is invalid.
 * @retval k_ra8_err_invalid_size Path construction exceeded a bound.
 * @retval k_ra8_err_* Native VFS stat failure, returned verbatim.
 * @pre All pointers are non-NULL and @p ctx is initialized.
 * @pre @p out addresses one writable, non-aliased metadata object.
 * @post Success fully initializes @p out.
 * @post Failure does not invoke any operation outside the bound mount.
 * @note Not thread-safe; uses adapter path scratch.
 * @since Version 0.1.0
 */
static ra8_err_t internal_stat(void* ctx, const char* path, fw_fs_stat_t* out);

/**
 * @brief Open an independent repository-filesystem directory cursor.
 * @details Builds a qualified VFS path, aligns the format-private subregion,
 *          and delegates ownership to the registered format cursor seam.
 * @param[in,out] ctx Bound VFS adapter context.
 * @param[in] path Validated portable directory path.
 * @param[out] directory_state Caller-owned cursor workspace.
 * @param[in] state_bytes Accessible workspace extent.
 * @return Workspace or native cursor-open status.
 * @retval k_ra8_ok The caller workspace owns one open cursor.
 * @retval k_ra8_err_* Path, capacity, alignment, capability, or open failure.
 * @pre Required pointers are non-NULL and @p path passed facade validation.
 * @pre @p state_bytes describes the accessible extent at @p directory_state.
 * @post Success retains no adapter scratch and holds no filesystem lock.
 * @post Failure leaves no native cursor owned by the caller workspace.
 * @note The bound mount pointer selects the medium; no host path is introduced.
 * @since Version 0.1.0
 */
static ra8_err_t
internal_dir_open(void* ctx, const char* path, void* directory_state, uint32_t state_bytes);

/**
 * @brief Copy one native cursor entry and translate its attributes.
 * @details Advances the format cursor once and copies the resulting name,
 *          size, and portable node classification into caller-owned output.
 * @param[in,out] ctx Bound VFS adapter context.
 * @param[in,out] directory_state Open adapter cursor workspace.
 * @param[out] out Stable copied portable entry value.
 * @param[out] out_entry True when @p out contains an entry; false at clean end.
 * @return Native iteration or bounded-conversion status.
 * @retval k_ra8_ok One entry was copied or clean end was observed.
 * @retval k_ra8_err_* Native cursor or name-bound failure.
 * @pre All pointers are non-NULL and @p directory_state owns an open cursor.
 * @pre @p out and @p out_entry are writable and non-aliased.
 * @post Success with @p out_entry true fully initializes @p out.
 * @post No adapter path scratch or filesystem lock remains held.
 * @note Native attributes are reduced to the portable file/directory model.
 * @since Version 0.1.0
 */
static ra8_err_t
internal_dir_next(void* ctx, void* directory_state, fw_fs_dirent_value_t* out, bool* out_entry);

/**
 * @brief Close one repository-filesystem directory cursor.
 * @details Delegates close to the VFS cursor that is embedded in the adapter
 *          workspace; it does not free or retain caller storage.
 * @param[in,out] ctx Bound VFS adapter context.
 * @param[in,out] directory_state Open adapter cursor workspace.
 * @return Native cursor-close status.
 * @retval k_ra8_ok The native cursor was consumed.
 * @retval k_ra8_err_* Native VFS close failure.
 * @pre Both pointers are non-NULL.
 * @pre @p directory_state owns one open VFS cursor.
 * @post The backend no longer owns a live cursor resource on success.
 * @post No adapter path scratch or filesystem lock remains held.
 * @note The generic facade consumes its public handle on every close return.
 * @since Version 0.1.0
 */
static ra8_err_t internal_dir_close(void* ctx, void* directory_state);

/**
 * @brief Enumerate a VFS directory while bounding callback delivery.
 * @details Adapts the cursor API while distinguishing native completion, user
 *          stop, budget stop, and callback failure.
 * @param[in,out] ctx Bound adapter context.
 * @param[in] path Validated portable directory path.
 * @param[in] max_entries Maximum callback deliveries.
 * @param[in] callback Portable entry callback.
 * @param[in,out] callback_ctx Opaque portable callback context.
 * @param[out] out_count Number of callbacks attempted.
 * @param[out] out_complete Whether native enumeration completed without a stop.
 * @return Enumeration status.
 * @retval k_ra8_ok Enumeration completed or stopped without an error.
 * @retval k_ra8_err_* Path, native listing, or callback failure.
 * @pre Pointer arguments are non-NULL and @p ctx is initialized.
 * @pre Public dispatch established the callback budget and path contract.
 * @post Both output objects are written from bounded bridge state.
 * @post Callback delivery never exceeds @p max_entries.
 * @note Not thread-safe; uses adapter path scratch and mutable bridge state.
 * @since Version 0.1.0
 */
static ra8_err_t internal_listdir(void*           ctx,
                                  const char*     path,
                                  uint32_t        max_entries,
                                  fw_fs_list_fn_t callback,
                                  void*           callback_ctx,
                                  uint32_t*       out_count,
                                  bool*           out_complete);

/**
 * @brief Create one directory inside the bound VFS mount.
 * @details Prefixes the portable path and delegates exactly once to native mkdir.
 * @param[in,out] ctx Bound adapter context.
 * @param[in] path Validated portable directory path.
 * @return Path construction or native mkdir status.
 * @retval k_ra8_ok The directory was created.
 * @retval k_ra8_err_* Path or native creation failure.
 * @pre @p ctx is initialized and @p path passed public validation.
 * @pre Parent directories already exist.
 * @post Success creates exactly the requested directory.
 * @post No path outside the bound mount is accessed.
 * @note Not thread-safe; uses adapter path scratch.
 * @since Version 0.1.0
 */
static ra8_err_t internal_mkdir(void* ctx, const char* path);

/**
 * @brief Unlink one file inside the bound VFS mount.
 * @details Prefixes the portable path and delegates exactly once to native unlink.
 * @param[in,out] ctx Bound adapter context.
 * @param[in] path Validated portable file path.
 * @return Path construction or native unlink status.
 * @retval k_ra8_ok The file was removed.
 * @retval k_ra8_err_* Path or native removal failure.
 * @pre @p ctx is initialized and @p path passed public validation.
 * @pre The caller intends file removal rather than directory removal.
 * @post Success removes exactly the requested file entry.
 * @post No path outside the bound mount is accessed.
 * @note Not thread-safe; uses adapter path scratch.
 * @since Version 0.1.0
 */
static ra8_err_t internal_unlink(void* ctx, const char* path);

/**
 * @brief Remove one empty directory inside the bound VFS mount.
 * @details Prefixes the portable path and delegates exactly once to native rmdir.
 * @param[in,out] ctx Bound adapter context.
 * @param[in] path Validated portable directory path.
 * @return Path construction or native rmdir status.
 * @retval k_ra8_ok The empty directory was removed.
 * @retval k_ra8_err_* Path or native removal failure.
 * @pre @p ctx is initialized and @p path passed public validation.
 * @pre The target directory is empty according to the mounted filesystem.
 * @post Success removes exactly the requested directory entry.
 * @post No recursive removal is attempted.
 * @note Not thread-safe; uses adapter path scratch.
 * @since Version 0.1.0
 */
static ra8_err_t internal_rmdir(void* ctx, const char* path);

/**
 * @brief Rename without replacement inside the bound VFS mount.
 * @details Builds both native paths in separate adapter scratch buffers and
 *          refuses replacement because the native seam cannot guarantee it.
 * @param[in,out] ctx Bound adapter context.
 * @param[in] old_path Validated existing portable source path.
 * @param[in] new_path Validated portable destination path.
 * @param[in] replace Whether an existing destination may be replaced.
 * @return Path construction or native rename status.
 * @retval k_ra8_ok The entry was renamed within the mount.
 * @retval k_ra8_err_not_supported @p replace is true.
 * @retval k_ra8_err_* Path or native rename failure.
 * @pre Pointer arguments are non-NULL and @p ctx is initialized.
 * @pre Both paths passed public portable validation.
 * @post Success moves one entry without overwriting a destination.
 * @post A true replacement request performs no native mutation.
 * @note Not thread-safe; uses both adapter path buffers.
 * @since Version 0.1.0
 */
static ra8_err_t
internal_rename(void* ctx, const char* old_path, const char* new_path, bool replace);

/**
 * @brief Query free space directly from the matching live mount.
 * @details Uses the retained mount object rather than reparsing a path and
 *          copies total, free, and used byte counts into the portable shape.
 * @param[in] ctx Bound adapter context.
 * @param[out] out Portable space result.
 * @return Native free-space query status.
 * @retval k_ra8_ok @p out contains the complete native space snapshot.
 * @retval k_ra8_err_* Native mount query failure, returned verbatim.
 * @pre @p ctx is initialized with a live mounted filesystem.
 * @pre @p out addresses one writable result object.
 * @post Success initializes all byte-count fields of @p out.
 * @post The mount and filesystem contents are unchanged.
 * @note Thread-safe only when mount query and lifecycle are synchronized.
 * @since Version 0.1.0
 */
static ra8_err_t internal_space(void* ctx, fw_fs_space_t* out);

/**
 * @brief Map portable open modes to the native VFS mode set.
 * @details Accepts only read, write-truncate, and append because the native VFS
 *          cannot implement the remaining portable combinations faithfully.
 * @param[in] mode Portable open mode.
 * @param[out] out Receives the corresponding native mode.
 * @return Mode translation status.
 * @retval k_ra8_ok @p out contains the exact native equivalent.
 * @retval k_ra8_err_not_supported No faithful native equivalent exists.
 * @pre @p out addresses one writable mode object.
 * @pre @p mode is a value representable by ::fw_fs_open_mode_t.
 * @post Success writes one supported native mode.
 * @post Failure leaves @p out unchanged.
 * @note Pure apart from caller output and thread-safe for disjoint output.
 * @since Version 0.1.0
 */
static ra8_err_t internal_mode(fw_fs_open_mode_t mode, ra8_fs_mode_t* out);

/**
 * @brief Open a VFS file into caller-owned workspace.
 * @details Validates workspace size and mode, builds the bound native path,
 *          clears the native handle slot, then delegates to the VFS.
 * @param[in,out] ctx Bound adapter context.
 * @param[in] path Validated portable file path.
 * @param[in] mode Portable open mode.
 * @param[out] file_state Caller workspace receiving ::vfs_file_state_t.
 * @param[in] state_bytes Writable workspace size in bytes.
 * @return Workspace, mode, path, or native open status.
 * @retval k_ra8_ok A native handle is stored in @p file_state.
 * @retval k_ra8_err_no_mem The workspace is undersized.
 * @retval k_ra8_err_not_supported The mode lacks a native equivalent.
 * @retval k_ra8_err_* Path or native open failure.
 * @pre Pointer arguments are non-NULL and @p ctx is initialized.
 * @pre @p file_state meets the binding's advertised alignment.
 * @post Success initializes exactly one open native handle.
 * @post Failure never exposes a stale handle as open.
 * @note Not thread-safe; uses adapter path scratch.
 * @since Version 0.1.0
 */
static ra8_err_t internal_open(void*             ctx,
                               const char*       path,
                               fw_fs_open_mode_t mode,
                               void*             file_state,
                               uint32_t          state_bytes);

/**
 * @brief Read a bounded prefix from an open native VFS file.
 * @details Unwraps the caller-owned adapter state and preserves native read,
 *          EOF, byte-count, and error semantics.
 * @param[in] ctx Unused bound adapter context.
 * @param[in,out] file_state Open ::vfs_file_state_t workspace.
 * @param[out] dst Destination for at most @p cap bytes.
 * @param[in] cap Writable destination capacity.
 * @param[out] out_read Accepted byte count, with zero representing EOF.
 * @return Native read status.
 * @retval k_ra8_ok A bounded prefix or EOF was reported.
 * @retval k_ra8_err_* Native file read failure, returned verbatim.
 * @pre File and output arguments satisfy the bound stream contract.
 * @pre @p dst addresses @p cap writable bytes when @p cap is non-zero.
 * @post Success reports no more than @p cap bytes.
 * @post The native file offset advances by the reported count.
 * @note Not thread-safe for concurrent access to one file handle.
 * @since Version 0.1.0
 */
static ra8_err_t
internal_read(void* ctx, void* file_state, uint8_t* dst, uint32_t cap, uint32_t* out_read);

/**
 * @brief Write one all-or-error span to an open native VFS file.
 * @details Maps the native API's complete-write contract into the portable
 *          count result, publishing @p len only after native success.
 * @param[in] ctx Unused bound adapter context.
 * @param[in,out] file_state Open ::vfs_file_state_t workspace.
 * @param[in] src Source bytes to write.
 * @param[in] len Exact source length.
 * @param[out] out_written Accepted byte count.
 * @return Native write status.
 * @retval k_ra8_ok All @p len bytes were accepted.
 * @retval k_ra8_err_* Native write failure, returned verbatim.
 * @pre File and output arguments satisfy the bound stream contract.
 * @pre @p src addresses @p len readable bytes when non-zero.
 * @post Success sets @p out_written to exactly @p len.
 * @post Failure does not claim a successful byte count.
 * @note Not thread-safe for concurrent access to one file handle.
 * @since Version 0.1.0
 */
static ra8_err_t internal_write(void*          ctx,
                                void*          file_state,
                                const uint8_t* src,
                                uint32_t       len,
                                uint32_t*      out_written);

/**
 * @brief Seek an open native VFS file to an absolute byte offset.
 * @details Unwraps the native handle and delegates the portable absolute offset.
 * @param[in] ctx Unused bound adapter context.
 * @param[in,out] file_state Open ::vfs_file_state_t workspace.
 * @param[in] offset Absolute byte offset from file start.
 * @return Native seek status.
 * @retval k_ra8_ok The file position is @p offset.
 * @retval k_ra8_err_* Native seek failure, returned verbatim.
 * @pre @p file_state contains one open native handle.
 * @pre @p offset is within limits accepted by the mounted filesystem.
 * @post Success sets the next I/O position to @p offset.
 * @post File contents are unchanged.
 * @note Not thread-safe for concurrent access to one file handle.
 * @since Version 0.1.0
 */
static ra8_err_t internal_seek(void* ctx, void* file_state, uint64_t offset);

/**
 * @brief Report an open native VFS file's absolute byte offset.
 * @details Unwraps the native handle and delegates the position query.
 * @param[in] ctx Unused bound adapter context.
 * @param[in] file_state Open ::vfs_file_state_t workspace.
 * @param[out] out_offset Receives the absolute file offset.
 * @return Native tell status.
 * @retval k_ra8_ok @p out_offset contains the current position.
 * @retval k_ra8_err_* Native tell failure, returned verbatim.
 * @pre @p file_state contains one open native handle.
 * @pre @p out_offset addresses one writable uint64_t object.
 * @post Success writes the position without changing it.
 * @post File contents are unchanged.
 * @note Thread-safe only with external handle synchronization.
 * @since Version 0.1.0
 */
static ra8_err_t internal_tell(void* ctx, void* file_state, uint64_t* out_offset);

/**
 * @brief Report an open native VFS file's current size.
 * @details Unwraps the native handle and delegates the length query.
 * @param[in] ctx Unused bound adapter context.
 * @param[in] file_state Open ::vfs_file_state_t workspace.
 * @param[out] out_size Receives the file length in bytes.
 * @return Native size-query status.
 * @retval k_ra8_ok @p out_size contains the current length.
 * @retval k_ra8_err_* Native size failure, returned verbatim.
 * @pre @p file_state contains one open native handle.
 * @pre @p out_size addresses one writable uint64_t object.
 * @post Success writes the length without changing the position.
 * @post File contents are unchanged.
 * @note Thread-safe only with external handle synchronization.
 * @since Version 0.1.0
 */
static ra8_err_t internal_size(void* ctx, void* file_state, uint64_t* out_size);

/**
 * @brief Close and consume an open native VFS file state.
 * @details Delegates close once and clears the stored native pointer regardless
 *          of result so callers cannot retry an unsafe consumed handle.
 * @param[in] ctx Unused bound adapter context.
 * @param[in,out] file_state Open ::vfs_file_state_t workspace to consume.
 * @return Native close status.
 * @retval k_ra8_ok The native close completed.
 * @retval k_ra8_err_* Native close failure, returned verbatim.
 * @pre @p file_state contains one open native handle.
 * @pre No concurrent I/O uses the same handle.
 * @post The stored native pointer is NULL on every return path.
 * @post The handle state cannot be used for later I/O without reopening.
 * @note Not thread-safe for concurrent access to one file handle.
 * @since Version 0.1.0
 */
static ra8_err_t internal_close(void* ctx, void* file_state);

/**
 * @brief Copy one portable path within the fixed binding capacity.
 * @details Copies through the first NUL and rejects input lacking a terminator
 *          in ::k_fw_fs_path_cap bytes.
 * @param[out] out Destination path buffer.
 * @param[in] path Candidate portable path.
 * @return Bounded copy status.
 * @retval k_ra8_ok The path and terminating NUL were copied.
 * @retval k_ra8_err_invalid_size No NUL occurred within the path cap.
 * @pre Both buffers address at least ::k_fw_fs_path_cap bytes unless NUL terminates input.
 * @pre Source and destination are either disjoint or identically based.
 * @post Success writes a terminated copy to @p out.
 * @post No bytes beyond the path cap are accessed.
 * @note Thread-safe for disjoint caller-owned buffers.
 * @since Version 0.1.0
 */
static ra8_err_t internal_copy_path(char* out, const char* path);

/**
 * @brief Build an 8.3-compatible sibling transaction stage path.
 * @details Retains the destination directory and replaces its leaf with
 *          `TXxxxxxx.TMP`, using the bounded transaction identifier.
 * @param[in] destination Validated portable destination path.
 * @param[in] id Candidate transaction identifier.
 * @param[out] out Destination stage-path buffer.
 * @return Stage naming status.
 * @retval k_ra8_ok A terminated sibling stage path was written.
 * @retval k_ra8_err_invalid_size Input termination or output capacity is invalid.
 * @pre @p out has ::k_fw_fs_path_cap writable bytes.
 * @pre @p destination is rooted and contains a valid non-empty leaf.
 * @post Success writes a sibling path with an 8.3-compatible leaf.
 * @post The destination path is unchanged.
 * @note Thread-safe for disjoint caller-owned buffers.
 * @since Version 0.1.0
 */
static ra8_err_t internal_stage_path(const char* destination, uint32_t id, char* out);

/**
 * @brief Begin a create-new transaction without touching the destination.
 * @details Validates workspace and policy, proves the destination absent, copies
 *          its path, and opens a collision-free sibling stage.
 * @param[in,out] ctx Bound adapter context.
 * @param[out] transaction_state Caller workspace receiving transaction state.
 * @param[in] state_bytes Writable transaction workspace size.
 * @param[in] destination Validated portable destination path.
 * @param[in] policy Required publication policy.
 * @return Transaction initialization status.
 * @retval k_ra8_ok A private stage is open for writing.
 * @retval k_ra8_err_no_mem Workspace is undersized or candidates are exhausted.
 * @retval k_ra8_err_not_supported Policy is not create-new.
 * @retval k_ra8_err_exists The destination already exists.
 * @retval k_ra8_err_* Stat, copy, naming, or open failure.
 * @pre Pointer arguments and alignment satisfy the bound transaction contract.
 * @pre Adapter state and mount remain live for the operation.
 * @post Success initializes an active unpublished stage.
 * @post The destination is never created or modified by begin.
 * @note Not thread-safe; uses adapter scratch and transaction-id state.
 * @since Version 0.1.0
 */
static ra8_err_t internal_txn_begin(void*                      ctx,
                                    void*                      transaction_state,
                                    uint32_t                   state_bytes,
                                    const char*                destination,
                                    fw_fs_transaction_policy_t policy);

/**
 * @brief Write transaction bytes to the open VFS stage.
 * @details Rejects a closed writer and otherwise reuses the native all-or-error
 *          stream adapter for the private staging file.
 * @param[in] ctx Bound adapter context.
 * @param[in,out] transaction_state Active transaction workspace.
 * @param[in] src Source bytes.
 * @param[in] len Exact byte count.
 * @param[out] out_written Accepted byte count.
 * @return Transaction or native write status.
 * @retval k_ra8_ok All bytes were accepted by the stage.
 * @retval k_ra8_err_invalid_state The writer is not open.
 * @retval k_ra8_err_* Native stage write failure.
 * @pre Pointer and buffer arguments satisfy the public transaction contract.
 * @pre @p src addresses @p len readable bytes when non-zero.
 * @post Success advances stage length/position and reports @p len.
 * @post The destination remains absent and unchanged.
 * @note Not thread-safe for concurrent access to one transaction.
 * @since Version 0.1.0
 */
static ra8_err_t internal_txn_write(void*          ctx,
                                    void*          transaction_state,
                                    const uint8_t* src,
                                    uint32_t       len,
                                    uint32_t*      out_written);

/**
 * @brief Seek within the open VFS stage for bounded header or table backfill.
 * @details Queries current stage length and refuses offsets beyond EOF before
 *          delegating the absolute native seek.
 * @param[in] ctx Bound adapter context.
 * @param[in,out] transaction_state Active transaction workspace.
 * @param[in] offset Absolute stage byte offset.
 * @return Transaction size or seek status.
 * @retval k_ra8_ok The stage position is @p offset.
 * @retval k_ra8_err_invalid_state The writer is not open.
 * @retval k_ra8_err_invalid_size @p offset exceeds current stage length.
 * @retval k_ra8_err_* Native size or seek failure.
 * @pre @p transaction_state contains an initialized transaction.
 * @pre No concurrent stage I/O occurs.
 * @post Success changes only the stage position.
 * @post The stage length and destination remain unchanged.
 * @note Not thread-safe for concurrent transaction access.
 * @since Version 0.1.0
 */
static ra8_err_t internal_txn_seek(void* ctx, void* transaction_state, uint64_t offset);

/**
 * @brief Close and reopen the VFS stage so a validator reads stable bytes.
 * @details Consumes the writer, reopens the private stage read-only, wraps it in
 *          the portable facade, invokes validation, and closes on every path.
 * @param[in] ctx Bound adapter context.
 * @param[in,out] transaction_state Active transaction workspace.
 * @param[in] validator Read-only portable stage validator.
 * @param[in,out] validator_ctx Opaque validator context.
 * @return Close, open, validator, or final-close status.
 * @retval k_ra8_ok Validation and all handle closes succeeded.
 * @retval k_ra8_err_invalid_state The writer is not open.
 * @retval k_ra8_err_* Native or validator failure, preserving first validation error.
 * @pre Validator and transaction pointers satisfy the public contract.
 * @pre The transaction owns one open private writer.
 * @post The transaction has no open writer or reader on return.
 * @post The private stage remains unpublished for later commit or abort.
 * @note Not thread-safe for concurrent transaction access.
 * @since Version 0.1.0
 */
static ra8_err_t internal_txn_validate(void*               ctx,
                                       void*               transaction_state,
                                       fw_fs_validate_fn_t validator,
                                       void*               validator_ctx);

/**
 * @brief Publish an absent-destination stage by same-mount no-replace rename.
 * @details Requires validation to have closed the writer, then renames the
 *          sibling stage and reports publication separately from status.
 * @param[in] ctx Bound adapter context.
 * @param[in,out] transaction_state Validated transaction workspace.
 * @param[out] out_published Receives true only after successful rename.
 * @return Transaction or native rename status.
 * @retval k_ra8_ok The stage became the destination.
 * @retval k_ra8_err_invalid_state A stage writer remains open.
 * @retval k_ra8_err_* Native no-replace rename failure.
 * @pre @p out_published is initialized false by guarded public dispatch.
 * @pre Validation completed and the private stage still exists.
 * @post Success clears stage_exists and sets @p out_published true.
 * @post Failure leaves publication false and the transaction abortable.
 * @note Not thread-safe for concurrent transaction access.
 * @since Version 0.1.0
 */
static ra8_err_t internal_txn_commit(void* ctx, void* transaction_state, bool* out_published);

/**
 * @brief Close and unlink a VFS stage while preserving the destination.
 * @details Attempts both cleanup steps, returns the first failure, and clears
 *          ownership flags only for resources actually released.
 * @param[in] ctx Bound adapter context.
 * @param[in,out] transaction_state Transaction workspace to consume or retry.
 * @return First close or unlink failure, or success.
 * @retval k_ra8_ok Every owned stage resource was released.
 * @retval k_ra8_err_* First native close or unlink failure.
 * @pre @p transaction_state contains initialized ownership flags.
 * @pre No concurrent operation uses the stage handle or path.
 * @post The destination is never modified.
 * @post Successfully released resources have their ownership flags cleared.
 * @note Not thread-safe for concurrent transaction access.
 * @since Version 0.1.0
 */
static ra8_err_t internal_txn_abort(void* ctx, void* transaction_state);

/**
 * @brief Validate and copy a VFS mount name into adapter state.
 * @details Accepts one non-empty bounded name without path separators or a
 *          colon, then copies it including the terminating NUL.
 * @param[out] state Adapter state receiving the mount name.
 * @param[in] name Candidate native VFS mount name.
 * @return Mount-name validation status.
 * @retval k_ra8_ok A terminated validated mount name was copied.
 * @retval k_ra8_err_invalid_arg The name is empty, too long, or contains `:` or `/`.
 * @pre @p state and @p name are non-NULL.
 * @pre @p name is readable through ::k_ra8_io_vfs_name_max bytes unless NUL occurs.
 * @post Success initializes state->mount_name.
 * @post Failure performs no native mount or filesystem operation.
 * @note Thread-safe for private adapter state during initialization.
 * @since Version 0.1.0
 */
static ra8_err_t internal_mount_name(fw_fs_ra8_vfs_state_t* state, const char* name);
