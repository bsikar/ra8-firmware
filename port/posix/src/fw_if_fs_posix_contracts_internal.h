/**
 * @file fw_if_fs_posix_contracts_internal.h
 * @brief File-local contracts for the confined POSIX filesystem adapter.
 *
 * @details
 * Declares the adapter's static resolver, namespace, stream, and transaction
 * helpers so their complete contracts remain readable without forcing the
 * implementation translation unit beyond the repository size ceiling. This
 * header is private to fw_if_fs_posix.c and widens no symbol's linkage.
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
 * @brief Reject an intermediate symlink before attempting directory open.
 * @details Uses no-follow metadata relative to an owned parent descriptor and
 *          permits only a real directory as the next confinement component.
 * @param[in] parent_fd Open descriptor for the confined parent directory.
 * @param[in] component Terminated child component.
 * @return Intermediate-component validation status.
 * @retval k_ra8_ok The component is a real directory.
 * @retval k_ra8_err_access_denied The component is a symbolic link.
 * @retval k_ra8_err_not_found The component is not a directory.
 * @retval k_ra8_err_* Mapped `fstatat` failure.
 * @pre @p parent_fd is open and owned by the resolver.
 * @pre @p component is a validated portable path component.
 * @post No descriptor ownership or filesystem contents change.
 * @post Success permits a subsequent `openat` with `O_NOFOLLOW`.
 * @note Thread-safe subject to host namespace race semantics.
 * @since Version 0.1.0
 */
static ra8_err_t internal_intermediate_check(int parent_fd, const char* component);

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
 * @details Validates the component is a real non-symlink directory, opens it
 *          no-follow, then closes the descriptor owned on entry -- on every
 *          path, including a failed validation, open, or close.
 * @param[in,out] current Owned parent descriptor on entry; replaced with the
 *        newly opened descriptor on success.
 * @param[in] name Terminated intermediate component name.
 * @return Descent status.
 * @retval k_ra8_ok @p current now owns the newly opened component directory.
 * @retval other Validation, open, or close failed; @p current is closed.
 * @pre @p current addresses an owned open directory descriptor.
 * @pre @p name is a terminated component distinct from the final leaf.
 * @post The descriptor owned on entry is closed on every return path.
 * @note Not thread-safe for a shared descriptor.
 * @since Version 0.1.0
 */
static ra8_err_t internal_parent_open_step(int* current, const char* name);

/**
 * @brief Resolve a canonical path's parent without following any symlink.
 * @details Duplicates the bound root and walks each intermediate component with
 *          no-follow stat/open calls, closing each descriptor as ownership moves.
 * @param[in] state Initialized confined-root adapter state.
 * @param[in] path Validated portable path below the bound root.
 * @param[out] out_parent_fd Receives the owned parent directory descriptor.
 * @param[out] out_leaf Receives the terminated final component.
 * @return Resolution status.
 * @retval k_ra8_ok Both parent descriptor and leaf were published.
 * @retval k_ra8_err_access_denied An intermediate component is a symlink.
 * @retval k_ra8_err_not_found An intermediate component is absent or not a directory.
 * @retval k_ra8_err_invalid_size A component or iteration bound is invalid.
 * @retval k_ra8_err_* Mapped descriptor operation failure.
 * @pre Pointer arguments are non-NULL and outputs have advertised capacity.
 * @pre @p path passed portable lexical validation and is not root-only.
 * @post On success caller owns `*out_parent_fd` and `out_leaf` is populated.
 * @post On failure every descriptor opened by this resolver is closed.
 * @note Thread-safe for independent state; host namespace changes can race resolution.
 * @since Version 0.1.0
 */
static ra8_err_t internal_parent_open(fw_fs_posix_state_t* state,
                                      const char*          path,
                                      int*                 out_parent_fd,
                                      char*                out_leaf);

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
 * @brief Open a confined raw-directory cursor in caller storage.
 * @details Opens beneath the bound root without following the final leaf and
 *          initializes the fixed raw-record buffer in caller storage.
 * @param[in,out] ctx Bound POSIX adapter context.
 * @param[in] path Validated canonical directory path.
 * @param[out] directory_state Caller-owned cursor workspace.
 * @param[in] state_bytes Accessible workspace extent.
 * @return Confined open or workspace status.
 * @retval k_ra8_ok The workspace owns one directory descriptor.
 * @retval k_ra8_err_* Capacity, confinement, open, or platform failure.
 * @pre Required pointers are non-NULL and @p path passed facade validation.
 * @pre @p state_bytes describes the writable extent at @p directory_state.
 * @post Success owns one directory descriptor in @p directory_state.
 * @post Failure closes every descriptor acquired by this operation.
 * @note No allocator-backed C runtime directory object is used.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_fs_posix_dir_open(void*       ctx,
                                          const char* path,
                                          void*       directory_state,
                                          uint32_t    state_bytes);

/**
 * @brief Copy the next visible raw-directory entry.
 * @details Decodes bounded native records, skips dot entries, and performs a
 *          no-follow metadata lookup before publishing a stable copied value.
 * @param[in,out] ctx Bound POSIX adapter context.
 * @param[in,out] directory_state Open caller-owned cursor workspace.
 * @param[out] out Stable copied portable entry value.
 * @param[out] out_entry True when @p out contains an entry; false at EOF.
 * @return Raw read, metadata, or validation status.
 * @retval k_ra8_ok One entry was copied or native end was observed.
 * @retval k_ra8_err_* Raw-read, record, metadata, or retry-bound failure.
 * @pre Output and cursor pointers are non-NULL and the cursor owns its descriptor.
 * @pre @p ctx names the same bound root that opened the cursor.
 * @post No lock is retained and borrowed kernel-record bytes never escape.
 * @post Success with @p out_entry true fully initializes @p out.
 * @note Namespace mutation may surface as the exact `fstatat` lookup error.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_fs_posix_dir_next(void*                 ctx,
                                          void*                 directory_state,
                                          fw_fs_dirent_value_t* out,
                                          bool*                 out_entry);

/**
 * @brief Close one owned raw-directory descriptor.
 * @details Invalidates the stored descriptor before mapping the host close
 *          result, preventing a retry from closing a reused descriptor number.
 * @param[in,out] ctx Bound POSIX adapter context.
 * @param[in,out] directory_state Open caller-owned cursor workspace.
 * @return Mapped descriptor-close status.
 * @retval k_ra8_ok The descriptor closed successfully.
 * @retval k_ra8_err_* Mapped host close failure.
 * @pre @p directory_state is non-NULL and owns a directory descriptor.
 * @pre @p ctx is the bound adapter context associated with the cursor.
 * @post The descriptor field is invalidated even when close reports an error.
 * @post Caller workspace ownership remains with the caller.
 * @note The generic facade consumes its handle on every return.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_fs_posix_dir_close(void* ctx, void* directory_state);

/**
 * @brief Enumerate a POSIX directory through bounded raw records.
 * @details Opens without symlink traversal, skips dot entries, bounds native
 *          records plus look-ahead, stats each leaf no-follow, and closes always.
 * @param[in,out] ctx Initialized confined-root adapter context.
 * @param[in] path Validated portable directory path.
 * @param[in] max_entries Maximum portable callback deliveries.
 * @param[in] callback Portable directory-entry callback.
 * @param[in,out] callback_ctx Opaque callback state.
 * @param[in,out] out_count Running count initialized by public dispatch.
 * @param[out] out_complete Whether native EOF was observed.
 * @return Enumeration, callback, metadata, or close status.
 * @retval k_ra8_ok Enumeration ended without an error.
 * @retval k_ra8_err_* First confined directory, callback, stat, or close failure.
 * @pre Pointer arguments are non-NULL and public bounds were validated.
 * @pre @p out_count initially contains zero.
 * @post Callback delivery never exceeds @p max_entries.
 * @post The directory descriptor is closed on every return path.
 * @note Not thread-safe with concurrent mutation of the enumerated directory.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_fs_posix_listdir(void*           ctx,
                                         const char*     path,
                                         uint32_t        max_entries,
                                         fw_fs_list_fn_t callback,
                                         void*           callback_ctx,
                                         uint32_t*       out_count,
                                         bool*           out_complete);

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
 * @brief Map a portable open mode to confined no-follow `openat` flags.
 * @details Adds close-on-exec and no-follow to read, truncate, append, and
 *          exclusive-create modes; rejects every unrecognized value.
 * @param[in] mode Portable open mode.
 * @param[out] out_flags Receives POSIX open flags.
 * @return Mode mapping status.
 * @retval k_ra8_ok @p out_flags contains the complete safe host flag set.
 * @retval k_ra8_err_invalid_arg @p mode is unsupported or invalid.
 * @pre @p out_flags addresses one writable integer.
 * @pre Required fallback macros preserve fail-closed behavior on this host.
 * @post Success writes flags corresponding exactly to @p mode.
 * @post Failure leaves @p out_flags unchanged.
 * @note Pure apart from caller output and thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t internal_open_flags(fw_fs_open_mode_t mode, int* out_flags);

/**
 * @brief Open one confined regular file into caller workspace.
 * @details Maps safe flags, resolves the no-follow parent, opens relative to it,
 *          closes the parent, then rejects any resulting non-regular object.
 * @param[in,out] ctx Initialized confined-root adapter context.
 * @param[in] path Validated portable file path.
 * @param[in] mode Portable open mode.
 * @param[out] file_state Caller workspace receiving ::posix_file_state_t.
 * @param[in] state_bytes Writable workspace size.
 * @return Workspace, mode, resolution, open, or type-check status.
 * @retval k_ra8_ok @p file_state owns one open regular-file descriptor.
 * @retval k_ra8_err_no_mem The workspace is undersized.
 * @retval k_ra8_err_invalid_arg The mode or opened object type is invalid.
 * @retval k_ra8_err_* Mapped resolution, open, stat, or close failure.
 * @pre Pointer arguments and alignment satisfy the bound stream contract.
 * @pre @p ctx owns a live confined root descriptor.
 * @post Success transfers exactly one descriptor into @p file_state.
 * @post Failure closes every descriptor acquired internally.
 * @note Thread-safe for independent file states subject to namespace races.
 * @since Version 0.1.0
 */
static ra8_err_t internal_open(void*             ctx,
                               const char*       path,
                               fw_fs_open_mode_t mode,
                               void*             file_state,
                               uint32_t          state_bytes);

/**
 * @brief Retry an interrupted POSIX read and expose a legitimate short read.
 * @details Repeats only `EINTR`, maps other host errors, and preserves short-read
 *          and zero-byte EOF semantics in the portable count.
 * @param[in] ctx Unused confined-root context.
 * @param[in,out] file_state Open ::posix_file_state_t.
 * @param[out] dst Destination for at most @p cap bytes.
 * @param[in] cap Writable destination capacity.
 * @param[out] out_read Accepted byte count.
 * @return Host read status.
 * @retval k_ra8_ok A bounded prefix or EOF was reported.
 * @retval k_ra8_err_* Mapped non-interrupt read failure.
 * @pre @p file_state owns an open readable descriptor.
 * @pre @p dst addresses @p cap writable bytes when non-zero.
 * @post Success reports no more than @p cap bytes.
 * @post The file offset advances by the reported count.
 * @note Not thread-safe for concurrent use of one descriptor offset.
 * @since Version 0.1.0
 */
static ra8_err_t
internal_read(void* ctx, void* file_state, uint8_t* dst, uint32_t cap, uint32_t* out_read);

/**
 * @brief Complete POSIX short writes while reporting any accepted prefix.
 * @details Retries interrupts, advances over positive short writes, maps host
 *          errors, and treats a zero-byte write before completion as failure.
 * @param[in] ctx Unused confined-root context.
 * @param[in,out] file_state Open ::posix_file_state_t.
 * @param[in] src Source bytes.
 * @param[in] len Exact requested byte count.
 * @param[in,out] out_written Running accepted count initialized by public dispatch.
 * @return Complete-write status.
 * @retval k_ra8_ok Exactly @p len bytes were accepted.
 * @retval k_ra8_fail The host returned zero before completion.
 * @retval k_ra8_err_* Mapped non-interrupt write failure.
 * @pre @p file_state owns an open writable descriptor.
 * @pre @p src addresses @p len readable bytes when non-zero.
 * @post Success sets @p out_written to @p len.
 * @post Failure preserves the exact accepted prefix count.
 * @note Not thread-safe for concurrent use of one descriptor offset.
 * @since Version 0.1.0
 */
static ra8_err_t internal_write(void*          ctx,
                                void*          file_state,
                                const uint8_t* src,
                                uint32_t       len,
                                uint32_t*      out_written);

/**
 * @brief Seek a POSIX descriptor to an unsigned absolute offset.
 * @details Rejects values not representable by signed 64-bit host offsets before
 *          issuing one `lseek(SEEK_SET)`.
 * @param[in] ctx Unused confined-root context.
 * @param[in,out] file_state Open ::posix_file_state_t.
 * @param[in] offset Absolute byte offset.
 * @return Host seek status.
 * @retval k_ra8_ok The descriptor position is @p offset.
 * @retval k_ra8_err_invalid_size @p offset exceeds INT64_MAX.
 * @retval k_ra8_err_* Mapped `lseek` failure.
 * @pre @p file_state owns an open seekable descriptor.
 * @pre No concurrent operation changes the same descriptor offset.
 * @post Success sets the next I/O position to @p offset.
 * @post File contents and length are unchanged.
 * @note Not thread-safe for concurrent use of one descriptor offset.
 * @since Version 0.1.0
 */
static ra8_err_t internal_seek(void* ctx, void* file_state, uint64_t offset);

/**
 * @brief Report the current POSIX descriptor offset.
 * @details Uses `lseek(SEEK_CUR)` without moving the offset and maps host errors.
 * @param[in] ctx Unused confined-root context.
 * @param[in] file_state Open ::posix_file_state_t.
 * @param[out] out_offset Receives the absolute byte offset.
 * @return Host position-query status.
 * @retval k_ra8_ok @p out_offset contains the current position.
 * @retval k_ra8_err_* Mapped `lseek` failure.
 * @pre @p file_state owns an open seekable descriptor.
 * @pre @p out_offset addresses one writable uint64_t object.
 * @post Success writes the position without changing it.
 * @post File contents and length are unchanged.
 * @note Thread-safe only with external descriptor-offset synchronization.
 * @since Version 0.1.0
 */
static ra8_err_t internal_tell(void* ctx, void* file_state, uint64_t* out_offset);

/**
 * @brief Report a POSIX descriptor's current file length.
 * @details Queries descriptor metadata with `fstat` and widens the non-negative
 *          regular-file size to the portable uint64_t result.
 * @param[in] ctx Unused confined-root context.
 * @param[in] file_state Open ::posix_file_state_t.
 * @param[out] out_size Receives current file length in bytes.
 * @return Host metadata-query status.
 * @retval k_ra8_ok @p out_size contains the current length.
 * @retval k_ra8_err_* Mapped `fstat` failure.
 * @pre @p file_state owns an open regular-file descriptor.
 * @pre @p out_size addresses one writable uint64_t object.
 * @post Success writes the length without changing descriptor position.
 * @post File contents are unchanged.
 * @note Thread-safe subject to file mutation and descriptor lifecycle synchronization.
 * @since Version 0.1.0
 */
static ra8_err_t internal_size(void* ctx, void* file_state, uint64_t* out_size);

/**
 * @brief Flush file contents and metadata through POSIX `fsync`.
 * @details Delegates durability to the host descriptor and maps any failure.
 * @param[in] ctx Unused confined-root context.
 * @param[in,out] file_state Open ::posix_file_state_t.
 * @return Host synchronization status.
 * @retval k_ra8_ok The host accepted the durability request.
 * @retval k_ra8_err_* Mapped `fsync` failure.
 * @pre @p file_state owns an open descriptor valid for synchronization.
 * @pre No concurrent close consumes the descriptor.
 * @post Success makes prior writes durable according to host filesystem guarantees.
 * @post Descriptor ownership and offset are unchanged.
 * @note Thread-safe only with external descriptor lifecycle synchronization.
 * @since Version 0.1.0
 */
static ra8_err_t internal_sync(void* ctx, void* file_state);

/**
 * @brief Close and consume one caller-owned POSIX file state.
 * @details Delegates to the shared EINTR-safe descriptor close helper, which
 *          invalidates the stored descriptor to prevent unsafe retries.
 * @param[in] ctx Unused confined-root context.
 * @param[in,out] file_state Open ::posix_file_state_t to consume.
 * @return Host close status.
 * @retval k_ra8_ok The descriptor closed successfully.
 * @retval k_ra8_err_* Mapped close failure.
 * @pre @p file_state owns one open descriptor.
 * @pre No concurrent I/O or close uses the descriptor.
 * @post The stored descriptor is invalidated on every return path.
 * @post The state cannot be used for I/O without reopening.
 * @note Not thread-safe for concurrent access to one file state.
 * @since Version 0.1.0
 */
static ra8_err_t internal_close(void* ctx, void* file_state);

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
