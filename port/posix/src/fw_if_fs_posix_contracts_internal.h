/**
 * @file fw_if_fs_posix_contracts_internal.h
 * @brief File-local contracts for the confined POSIX filesystem adapter.
 *
 * @details
 * Declares the adapter's file-local resolver, namespace, and transaction
 * helpers so their complete contracts remain readable without forcing the
 * implementation translation unit beyond the repository size ceiling. The
 * cross-unit ::RA8_PRIV contracts live in fw_if_fs_posix_internal.h; this
 * header remains private to fw_if_fs_posix.c, beside the byte-stream unit's
 * corresponding file-local contracts.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#pragma once

#include <sys/stat.h>

#include "fw_if_fs_posix_internal.h"
#include "ra8_attributes.h"

/**
 * @brief Copy one bounded path component into a fixed local buffer.
 * @details Rejects empty or over-capacity names before copying exact bytes and
 *          appending a NUL for descriptor-relative system calls.
 * @param[in] start First byte of the component in the portable path.
 * @param[in] length Component length in bytes.
 * @param[out] out Fixed ::k_posix_component_cap-byte destination.
 * @return Bounded copy status.
 * @retval k_ra8_ok The component and NUL were written.
 * @retval k_ra8_err_invalid_size Length is zero or exceeds the local capacity.
 * @pre @p start addresses at least @p length readable bytes.
 * @pre @p out addresses ::k_posix_component_cap writable bytes.
 * @post Success writes one terminated component.
 * @post No byte outside the destination capacity is modified.
 * @note Thread-safe for disjoint caller-owned buffers.
 * @since Version 0.1.0
 */
static ra8_err_t internal_component_copy(const char* start, uint16_t length, char* out);

/**
 * @brief Validate one directory component without following a symbolic link.
 * @details Uses no-follow metadata relative to an owned parent descriptor.
 *          Real directories are accepted without alias traversal. Linux denies
 *          every symbolic link. Darwin alone may publish a selection for an
 *          exact, verified filesystem-root `tmp` or `var` alias; every nested,
 *          absolute-target, or otherwise mismatched link remains denied.
 * @param[in] parent_fd Open descriptor for the confined parent directory.
 * @param[in] component Terminated child component.
 * @param[out] out_alias Receives a verified alias selection or `none`.
 * @return Intermediate-component validation status.
 * @retval k_ra8_ok The component is a real directory or approved Darwin root alias.
 * @retval k_ra8_err_access_denied The component is an unapproved symbolic link.
 * @retval k_ra8_err_not_found The component is not a directory.
 * @retval k_ra8_err_* Mapped metadata or `readlinkat` failure.
 * @pre @p parent_fd is open and owned by the resolver.
 * @pre @p component and @p out_alias are non-NULL and @p component is validated.
 * @post No descriptor ownership or filesystem contents change.
 * @post Linux always publishes `none`; Darwin success publishes only a verified strategy.
 * @note Thread-safe subject to host namespace race semantics.
 * @since Version 0.1.0
 */
static ra8_err_t
internal_intermediate_check(int parent_fd, const char* component, posix_root_alias_t* out_alias);

/**
 * @brief Scan and copy the next slash-delimited component of a cursor.
 * @details Bounds the scan to ::k_posix_component_cap, then copies the
 *          scanned span through ::internal_component_copy; the cursor
 *          itself is left unmoved so the caller can inspect the byte
 *          immediately after the scanned span before advancing.
 * @param[in] cursor Address of the current scan position in the path.
 * @param[out] out_name Fixed ::k_posix_component_cap-byte destination.
 * @param[out] out_length Receives the scanned component's byte length.
 * @return Bounded scan-and-copy status.
 * @retval k_ra8_ok The component was scanned and copied.
 * @retval k_ra8_err_invalid_size The scan or the copy exceeded local capacity.
 * @pre `*cursor` addresses a NUL- or slash-terminated path remainder.
 * @pre @p out_name addresses ::k_posix_component_cap writable bytes.
 * @post Success writes one terminated component and its length.
 * @post `*cursor` is unchanged; the caller advances it explicitly.
 * @note Thread-safe for disjoint caller-owned buffers.
 * @since Version 0.1.0
 */
static ra8_err_t internal_next_component(const char** cursor, char* out_name, uint16_t* out_length);

/**
 * @brief Descend into one intermediate path component, retiring the old parent.
 * @details Opens a normal component no-follow, or on Darwin resolves an approved
 *          filesystem-root alias through its canonical no-follow path, then
 *          closes the descriptor owned on entry on every return path. Linux
 *          rejects every symbolic-link component before this descent.
 * @param[in,out] current Owned parent descriptor on entry; replaced with the
 *        newly opened descriptor on success.
 * @param[in] name Terminated intermediate component name.
 * @return Descent status.
 * @retval k_ra8_ok @p current now owns the newly opened component directory.
 * @retval other Validation, open, or close failed; @p current is closed.
 * @pre @p current addresses an owned open directory descriptor.
 * @pre @p name is a terminated component distinct from the final leaf.
 * @post The descriptor owned on entry is closed on every return path.
 * @post Success leaves exactly one owned descriptor in @p current.
 * @note Not thread-safe for a shared descriptor.
 * @since Version 0.1.0
 */
static ra8_err_t internal_parent_open_step(int* current, const char* name);

/**
 * @brief Convert a POSIX mode into a portable node kind.
 * @details Recognizes regular files, directories, and symbolic links explicitly;
 *          every remaining host object is reported as portable `other`.
 * @param[in] mode POSIX `st_mode` value.
 * @return Corresponding portable node type.
 * @retval k_fw_fs_node_file @p mode describes a regular file.
 * @retval k_fw_fs_node_directory @p mode describes a directory.
 * @retval k_fw_fs_node_symlink @p mode describes a symbolic link.
 * @retval k_fw_fs_node_other @p mode describes another host object.
 * @pre @p mode was obtained from a successful POSIX metadata query.
 * @pre Standard `S_IS*` macros are available for the host ABI.
 * @post No memory or filesystem state is modified.
 * @post The result depends only on @p mode.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
static fw_fs_node_type_t internal_node_type(mode_t mode);

/**
 * @brief Query native metadata without following the final component.
 * @details Handles the bound root directly; other paths resolve a confined
 *          parent and use `fstatat(AT_SYMLINK_NOFOLLOW)`, mapping absence to data.
 * @param[in,out] state Initialized confined-root adapter state.
 * @param[in] path Validated portable path.
 * @param[out] out Receives native metadata when the entry exists.
 * @param[out] out_exists Receives whether the entry exists.
 * @return Resolution or metadata-query status.
 * @retval k_ra8_ok Existence was determined, including a clean miss.
 * @retval k_ra8_err_* Mapped resolution, stat, or descriptor-close failure.
 * @pre Pointer arguments are non-NULL and @p state owns an open root descriptor.
 * @pre @p path passed public portable validation.
 * @post Success always initializes @p out_exists.
 * @post A missing entry is success with @p out_exists false.
 * @note Thread-safe subject to host namespace race semantics.
 * @since Version 0.1.0
 */
static ra8_err_t internal_native_stat(fw_fs_posix_state_t* state,
                                      const char*          path,
                                      struct stat*         out,
                                      bool*                out_exists);

/**
 * @brief Produce portable metadata for one path below the confined root.
 * @details Converts no-follow native metadata, timestamps, type, and size while
 *          normalizing directory size to zero and absence to `node_none`.
 * @param[in,out] ctx Initialized confined-root adapter context.
 * @param[in] path Validated portable path.
 * @param[out] out Portable metadata destination.
 * @return Native stat or conversion status.
 * @retval k_ra8_ok @p out contains a complete existence snapshot.
 * @retval k_ra8_err_* Mapped confined metadata-query failure.
 * @pre Pointer arguments are non-NULL and @p ctx owns an open root descriptor.
 * @pre @p out is writable and disjoint from adapter state.
 * @post Success fully initializes @p out.
 * @post Symbolic links are reported, never followed.
 * @note Thread-safe subject to host namespace race semantics.
 * @since Version 0.1.0
 */
static ra8_err_t internal_stat(void* ctx, const char* path, fw_fs_stat_t* out);

/**
 * @brief Open a confined directory without following its final component.
 * @details Duplicates the bound root for `/`; otherwise resolves a no-follow
 *          parent, validates the leaf as a real directory, and opens it safely.
 * @param[in,out] state Initialized confined-root adapter state.
 * @param[in] path Validated portable directory path.
 * @param[out] out_fd Receives an owned directory descriptor.
 * @return Resolution or directory-open status.
 * @retval k_ra8_ok @p out_fd owns an open directory descriptor.
 * @retval k_ra8_err_access_denied A traversed component is a symbolic link.
 * @retval k_ra8_err_not_found A component is absent or not a directory.
 * @retval k_ra8_err_* Mapped open or close failure.
 * @pre Pointer arguments are non-NULL and root_fd is live.
 * @pre @p path passed portable lexical validation.
 * @post Success transfers exactly one descriptor to the caller.
 * @post Failure closes every descriptor acquired internally.
 * @note Thread-safe subject to host namespace race semantics.
 * @since Version 0.1.0
 */
static ra8_err_t internal_directory_open(fw_fs_posix_state_t* state, const char* path, int* out_fd);

/**
 * @brief Create one confined directory with no implicit parent creation.
 * @details Resolves a no-follow parent and invokes `mkdirat` on the final leaf,
 *          preserving the first creation failure over descriptor-close status.
 * @param[in,out] ctx Initialized confined-root adapter context.
 * @param[in] path Validated portable directory path.
 * @return Resolution, creation, or close status.
 * @retval k_ra8_ok The directory was created and parent descriptor closed.
 * @retval k_ra8_err_* Mapped parent, `mkdirat`, or close failure.
 * @pre @p path is non-root and its parent already exists.
 * @pre @p ctx owns a live confined root descriptor.
 * @post Success creates exactly one directory with fixed repository mode.
 * @post No component outside the bound root is accessed.
 * @note Thread-safe subject to host namespace race semantics.
 * @since Version 0.1.0
 */
static ra8_err_t internal_mkdir(void* ctx, const char* path);

/**
 * @brief Remove one confined regular file while refusing directories and links.
 * @details No-follow stats the target before descriptor-relative unlink and
 *          rejects every target type except an existing regular file.
 * @param[in,out] ctx Initialized confined-root adapter context.
 * @param[in] path Validated portable file path.
 * @return Validation, unlink, or close status.
 * @retval k_ra8_ok The regular file was removed.
 * @retval k_ra8_err_not_found The target is absent.
 * @retval k_ra8_err_access_denied The target is a symbolic link.
 * @retval k_ra8_err_invalid_arg The target is not a regular file.
 * @retval k_ra8_err_* Mapped stat, resolution, unlink, or close failure.
 * @pre @p ctx owns a live confined root descriptor.
 * @pre @p path passed public portable validation.
 * @post Success removes exactly one regular-file directory entry.
 * @post Symbolic links and directories are never removed.
 * @note Thread-safe subject to host namespace race semantics.
 * @since Version 0.1.0
 */
static ra8_err_t internal_unlink(void* ctx, const char* path);

/**
 * @brief Remove one confined empty real directory.
 * @details No-follow stats the target before descriptor-relative `AT_REMOVEDIR`
 *          and rejects absent, symbolic-link, and non-directory targets.
 * @param[in,out] ctx Initialized confined-root adapter context.
 * @param[in] path Validated portable directory path.
 * @return Validation, removal, or close status.
 * @retval k_ra8_ok The empty directory was removed.
 * @retval k_ra8_err_not_found The target is absent.
 * @retval k_ra8_err_access_denied The target is a symbolic link.
 * @retval k_ra8_err_invalid_arg The target is not a directory.
 * @retval k_ra8_err_* Mapped stat, resolution, removal, or close failure.
 * @pre @p ctx owns a live confined root descriptor.
 * @pre The target is expected to be empty; recursive removal is unsupported.
 * @post Success removes exactly one empty real directory.
 * @post Symbolic links and non-directories are never removed.
 * @note Thread-safe subject to host namespace race semantics.
 * @since Version 0.1.0
 */
static ra8_err_t internal_rmdir(void* ctx, const char* path);

/**
 * @brief Perform a host atomic no-replace rename without a TOCTOU fallback.
 * @details Uses Linux `renameat2(RENAME_NOREPLACE)` or Darwin
 *          `renameatx_np(RENAME_EXCL)` and fails closed on unsupported hosts.
 * @param[in] old_fd Open source-parent descriptor.
 * @param[in] old_leaf Terminated source leaf.
 * @param[in] new_fd Open destination-parent descriptor.
 * @param[in] new_leaf Terminated destination leaf.
 * @return Atomic no-replace rename status.
 * @retval k_ra8_ok The source was atomically renamed to an absent destination.
 * @retval k_ra8_err_not_supported The host primitive is unavailable.
 * @retval k_ra8_err_* Mapped host rename failure.
 * @pre Both descriptors are open directories on one filesystem.
 * @pre Both leaf names are validated and neither traversal nor symlinks are followed here.
 * @post Success moves the source without replacing a destination.
 * @post Unsupported hosts perform no filesystem mutation.
 * @note Thread-safe subject to host namespace race semantics.
 * @since Version 0.1.0
 */
static ra8_err_t
internal_rename_noreplace(int old_fd, const char* old_leaf, int new_fd, const char* new_leaf);

/**
 * @brief Validate that a rename's source exists and neither endpoint is a symlink.
 * @details Stats both paths through the confined resolver and rejects an
 *          absent source or a present-but-symlinked source or destination
 *          before any parent descriptor is opened.
 * @param[in] state Initialized confined-root adapter state.
 * @param[in] old_path Validated existing source path.
 * @param[in] new_path Validated destination path.
 * @return Endpoint validation status.
 * @retval k_ra8_ok The source exists and neither present endpoint is a symlink.
 * @retval k_ra8_err_not_found The source is absent.
 * @retval k_ra8_err_access_denied Either endpoint is a symbolic link.
 * @retval k_ra8_err_* Mapped metadata failure.
 * @pre Both paths passed public portable validation.
 * @pre @p state owns a live confined root descriptor.
 * @post No descriptor is opened or closed by this check.
 * @post Neither endpoint's contents or metadata are modified.
 * @note Thread-safe subject to host namespace race semantics.
 * @since Version 0.1.0
 */
static ra8_err_t internal_rename_validate_endpoints(fw_fs_posix_state_t* state,
                                                    const char*          old_path,
                                                    const char*          new_path);

/**
 * @brief Rename within the selected root without following either leaf.
 * @details No-follow stats both endpoints, rejects symlinks and cross-device
 *          parents, and selects replace or atomic no-replace host semantics.
 * @param[in,out] ctx Initialized confined-root adapter context.
 * @param[in] old_path Validated existing source path.
 * @param[in] new_path Validated destination path.
 * @param[in] replace Whether an existing destination may be replaced atomically.
 * @return Validation, resolution, rename, or close status.
 * @retval k_ra8_ok The entry was renamed within one filesystem.
 * @retval k_ra8_err_not_found The source is absent.
 * @retval k_ra8_err_access_denied Either endpoint is a symbolic link.
 * @retval k_ra8_err_invalid_arg Parent directories are on different devices.
 * @retval k_ra8_err_not_supported Atomic no-replace is unavailable.
 * @retval k_ra8_err_* Mapped stat, resolution, rename, or close failure.
 * @pre Both paths passed public portable validation.
 * @pre @p ctx owns a live confined root descriptor.
 * @post Success moves one entry with the requested replacement semantics.
 * @post Every acquired parent descriptor is closed.
 * @note Thread-safe subject to host namespace race semantics.
 * @since Version 0.1.0
 */
static ra8_err_t
internal_rename(void* ctx, const char* old_path, const char* new_path, bool replace);

/**
 * @brief Query confined-root volume byte totals with `fstatvfs`.
 * @details Multiplies block counts by the fragment size and distinguishes user-
 *          available free bytes from blocks reserved by the host filesystem.
 * @param[in] ctx Initialized confined-root adapter context.
 * @param[out] out Portable total, free, and used byte snapshot.
 * @return Host space-query status.
 * @retval k_ra8_ok Every byte count was populated.
 * @retval k_ra8_err_* Mapped `fstatvfs` failure.
 * @pre @p ctx owns a live root descriptor.
 * @pre @p out addresses one writable result object.
 * @post Success initializes all three portable byte counts.
 * @post Filesystem contents and descriptor position are unchanged.
 * @note Thread-safe subject to root descriptor lifecycle synchronization.
 * @since Version 0.1.0
 */
static ra8_err_t internal_space(void* ctx, fw_fs_space_t* out);

/**
 * @brief Create an exclusive sibling stage after bounded collision retries.
 * @details Advances the adapter transaction id, builds sibling names, and uses
 *          `O_EXCL` open so a race cannot silently replace an existing entry.
 * @param[in,out] state Bound adapter state and transaction-id source.
 * @param[in,out] txn Transaction receiving stage path and writer descriptor.
 * @return Bounded stage-open status.
 * @retval k_ra8_ok A new exclusive stage is open and tracked.
 * @retval k_ra8_err_no_mem Every bounded candidate collided.
 * @retval k_ra8_err_* Naming or non-collision open failure.
 * @pre @p txn contains a validated terminated destination path.
 * @pre Adapter root and transaction state remain live for all attempts.
 * @post Success sets writer_open and stage_exists.
 * @post At most ::k_posix_stage_attempts candidates are attempted.
 * @note Not thread-safe; advances shared adapter transaction-id state.
 * @since Version 0.1.0
 */
static ra8_err_t internal_stage_open(fw_fs_posix_state_t* state, posix_transaction_state_t* txn);

/**
 * @brief Begin a staged POSIX create-new or atomic-replacement transaction.
 * @details Validates workspace, host capability, and destination type/policy,
 *          copies the path, and opens a private exclusive sibling stage.
 * @param[in,out] ctx Bound confined-root adapter context.
 * @param[out] transaction_state Caller workspace receiving transaction state.
 * @param[in] state_bytes Writable transaction workspace size.
 * @param[in] destination Validated portable destination path.
 * @param[in] policy Create-new or atomic-replace publication policy.
 * @return Transaction initialization status.
 * @retval k_ra8_ok A private stage is open for writing.
 * @retval k_ra8_err_no_mem Workspace is undersized or candidates are exhausted.
 * @retval k_ra8_err_not_supported Atomic no-replace is unavailable when required.
 * @retval k_ra8_err_access_denied Destination is a symbolic link.
 * @retval k_ra8_err_invalid_arg Destination is a directory.
 * @retval k_ra8_err_exists Create-new destination already exists.
 * @retval k_ra8_err_* Stat, copy, naming, or open failure.
 * @pre Pointer arguments and alignment satisfy the bound transaction contract.
 * @pre Adapter root and host capability snapshot remain valid.
 * @post Success initializes an active unpublished stage.
 * @post Begin never creates or modifies the destination itself.
 * @note Not thread-safe; uses adapter transaction-id and namespace state.
 * @since Version 0.1.0
 */
static ra8_err_t internal_txn_begin(void*                      ctx,
                                    void*                      transaction_state,
                                    uint32_t                   state_bytes,
                                    const char*                destination,
                                    fw_fs_transaction_policy_t policy);

/**
 * @brief Append bytes to an open POSIX transaction stage.
 * @details Rejects a closed writer and otherwise reuses the complete-write
 *          stream adapter while preserving accepted-prefix error reporting.
 * @param[in] ctx Bound adapter context.
 * @param[in,out] transaction_state Active transaction workspace.
 * @param[in] src Source bytes.
 * @param[in] len Exact byte count.
 * @param[out] out_written Accepted byte count.
 * @return Transaction or host write status.
 * @retval k_ra8_ok All @p len bytes were accepted.
 * @retval k_ra8_err_invalid_state The writer is not open.
 * @retval k_ra8_err_* Mapped stage write failure.
 * @pre Pointer and buffer arguments satisfy the public transaction contract.
 * @pre @p src addresses @p len readable bytes when non-zero.
 * @post Success advances the stage position by @p len.
 * @post The destination remains unpublished and unchanged.
 * @note Not thread-safe for concurrent use of one transaction.
 * @since Version 0.1.0
 */
static ra8_err_t internal_txn_write(void*          ctx,
                                    void*          transaction_state,
                                    const uint8_t* src,
                                    uint32_t       len,
                                    uint32_t*      out_written);

/**
 * @brief Seek within the open POSIX stage for bounded header or table backfill.
 * @details Queries current stage length and rejects offsets beyond EOF before
 *          delegating an absolute seek.
 * @param[in] ctx Bound adapter context.
 * @param[in,out] transaction_state Active transaction workspace.
 * @param[in] offset Absolute stage byte offset.
 * @return Transaction, size, or seek status.
 * @retval k_ra8_ok The stage position is @p offset.
 * @retval k_ra8_err_invalid_state The writer is not open.
 * @retval k_ra8_err_invalid_size @p offset exceeds current stage length.
 * @retval k_ra8_err_* Mapped size or seek failure.
 * @pre @p transaction_state contains an initialized transaction.
 * @pre No concurrent stage I/O occurs.
 * @post Success changes only the stage position.
 * @post Stage length and destination contents are unchanged.
 * @note Not thread-safe for concurrent use of one transaction.
 * @since Version 0.1.0
 */
static ra8_err_t internal_txn_seek(void* ctx, void* transaction_state, uint64_t offset);

/**
 * @brief Durably sync, reopen read-only, and validate a POSIX stage.
 * @details Fsyncs and consumes the writer, reopens the private stage, invokes
 *          the injected portable validator, and closes the reader on every path.
 * @param[in] ctx Bound adapter context.
 * @param[in,out] transaction_state Active transaction workspace.
 * @param[in] validator Read-only portable stage validator.
 * @param[in,out] validator_ctx Opaque validator context.
 * @return Sync, close, open, validation, or final-close status.
 * @retval k_ra8_ok Stage durability, validation, and cleanup succeeded.
 * @retval k_ra8_err_invalid_state The writer is not open.
 * @retval k_ra8_err_* First host or validator failure.
 * @pre Validator and transaction pointers satisfy the public contract.
 * @pre The transaction owns one open private writer.
 * @post The transaction has no open writer or reader on return.
 * @post The stage remains private and abortable until commit.
 * @note Not thread-safe for concurrent use of one transaction.
 * @since Version 0.1.0
 */
static ra8_err_t internal_txn_validate(void*               ctx,
                                       void*               transaction_state,
                                       fw_fs_validate_fn_t validator,
                                       void*               validator_ctx);

/**
 * @brief Sync the destination's containing directory after publication.
 * @details Resolves the confined parent, requests directory durability, closes
 *          the descriptor, and preserves the fsync failure over close status.
 * @param[in,out] state Initialized confined-root adapter state.
 * @param[in] path Validated published destination path.
 * @return Parent resolution, sync, or close status.
 * @retval k_ra8_ok Directory synchronization and close succeeded.
 * @retval k_ra8_err_* Mapped parent, `fsync`, or close failure.
 * @pre @p state owns a live root descriptor.
 * @pre @p path names an already published entry.
 * @post Every acquired parent descriptor is closed.
 * @post Success makes the rename durable according to host guarantees.
 * @note Thread-safe subject to host namespace and root lifecycle synchronization.
 * @since Version 0.1.0
 */
static ra8_err_t internal_parent_sync(fw_fs_posix_state_t* state, const char* path);

/**
 * @brief Atomically publish then durably sync the destination directory.
 * @details Selects replace semantics from policy, renames the sibling stage,
 *          reports publication immediately, then fsyncs the containing directory.
 * @param[in,out] ctx Bound confined-root adapter context.
 * @param[in,out] transaction_state Validated transaction workspace.
 * @param[out] out_published Receives publication truth independently of durability.
 * @return Transaction, rename, or directory-sync status.
 * @retval k_ra8_ok Publication and directory durability succeeded.
 * @retval k_ra8_err_invalid_state A stage writer remains open.
 * @retval k_ra8_err_* Rename or post-publication directory-sync failure.
 * @pre @p out_published is initialized false by public guarded dispatch.
 * @pre Validation completed and the private stage exists.
 * @post Successful rename clears stage_exists and sets publication true.
 * @post A later sync failure still reports @p out_published true.
 * @note Not thread-safe for concurrent transaction or namespace access.
 * @since Version 0.1.0
 */
static ra8_err_t internal_txn_commit(void* ctx, void* transaction_state, bool* out_published);

/**
 * @brief Close and remove a POSIX stage while preserving the destination.
 * @details Attempts every owned cleanup step, returns the first failure, and
 *          clears ownership flags only for resources actually released.
 * @param[in] ctx Bound adapter context.
 * @param[in,out] transaction_state Transaction workspace to consume or retry.
 * @return First close or unlink failure, or success.
 * @retval k_ra8_ok Every owned stage resource was released.
 * @retval k_ra8_err_* First mapped close or unlink failure.
 * @pre @p transaction_state contains initialized ownership flags.
 * @pre No concurrent operation uses the stage descriptor or path.
 * @post The destination is never modified.
 * @post Successfully released resources have ownership flags cleared.
 * @note Not thread-safe for concurrent use of one transaction.
 * @since Version 0.1.0
 */
static ra8_err_t internal_txn_abort(void* ctx, void* transaction_state);
