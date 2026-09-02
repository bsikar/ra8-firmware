/**
 * @file fw_if_fs_posix_internal.h
 * @brief Shared errno/descriptor helpers for the POSIX filesystem port.
 * @ingroup grp_io
 *
 * @details
 * Defines the fixed caller-workspace layouts and bounded helper contracts used
 * across the POSIX adapter translation units. These declarations keep hosted
 * descriptor, timestamp, path, transaction-name, and Darwin root-alias policy
 * private to the port while avoiding duplicated platform logic.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "fw_if_fs.h"
#include "fw_if_fs_posix.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Local component and stage-search bounds. */
typedef enum : uint16_t {
  k_posix_stage_leaf_span = 13U,  /**< Stage leaf plus terminating NUL.     */
  k_posix_max_open_files  = 64U,  /**< Truthful hosted descriptor capacity. */
  k_posix_component_cap   = 256U, /**< Component buffer including NUL.      */
  k_posix_file_mode       = 0600, /**< Owner-only created-file mode.        */
  k_posix_directory_mode  = 0700, /**< Owner-only created-directory mode.   */
  k_posix_stage_attempts  = 64U,  /**< Collision-search attempt cap.        */
} posix_limits_t;

/**
 * @enum posix_root_alias_t
 * @brief Classification of a verified filesystem-root directory alias.
 *
 * @details
 * Distinguishes ordinary directory components from the two Darwin filesystem
 * root aliases whose exact relative targets may be opened canonically. The
 * pure classifier produces a non-`none` value only for an exact component-
 * target tuple; Darwin callers additionally verify the parent descriptor
 * identifies `/` before treating that classification as permission to open.
 *
 * @invariant Only `tmp` -> `private/tmp` and `var` -> `private/var` can be
 *            classified; production use is restricted to Darwin actual root.
 *
 * @par Example
 * @code{.c}
 * posix_root_alias_t alias = k_posix_root_alias_none;
 * @endcode
 *
 * @see priv_fs_posix_root_alias_verify()
 * @see priv_fs_posix_root_alias_open()
 * @see priv_fs_posix_root_alias_classify()
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_posix_root_alias_none = 0U, /**< Component is not an approved root alias. */
  k_posix_root_alias_tmp  = 1U, /**< Root `tmp` resolves to `private/tmp`.    */
  k_posix_root_alias_var  = 2U, /**< Root `var` resolves to `private/var`.    */
} posix_root_alias_t;

/** @brief Hexadecimal stage-name encoding constants. */
typedef enum : uint8_t {
  k_posix_hex_nibble_bits  = 4U, /**< Bits represented by one hex digit. */
  k_posix_stage_hex_digits = 6U, /**< Hex digits in a stage identifier.  */
  /** @brief Highest valid index in a stage identifier. */
  k_posix_hex_last_digit  = k_posix_stage_hex_digits - 1U,
  k_posix_hex_nibble_mask = 0x0FU, /**< Low-nibble mask. */
} posix_hex_limits_t;

/** @brief POSIX timestamp and transaction arithmetic constants. */
typedef enum : uint32_t {
  k_posix_epoch_year_offset   = 1900U,       /**< `struct tm` year origin. */
  k_posix_nanosecond_max      = 999999999U,  /**< Largest valid subsecond. */
  k_posix_transaction_id_mask = 0x00FFFFFFU, /**< Six hexadecimal digits.  */
} posix_numeric_limits_t;

/** @brief Raw directory buffer and retry bounds. */
typedef enum : uint16_t {
  k_posix_directory_buffer_bytes = 4096U, /**< Fixed local kernel buffer. */
  k_posix_directory_read_retries = 16U,   /**< Maximum interrupted reads. */
} posix_directory_limits_t;

/** @brief Linux `getdents64` wire-record offsets and alignment. */
typedef enum : uint8_t {
  k_posix_linux_reclen_offset = 16U, /**< Offset of `d_reclen`.    */
  k_posix_linux_name_offset   = 19U, /**< Offset of `d_name`.      */
  k_posix_linux_record_align  = 8U,  /**< Kernel record alignment. */
} posix_linux_dirent_layout_t;

/** @brief Darwin `getdirentries64` wire-record offsets and alignment. */
typedef enum : uint8_t {
  k_posix_darwin_reclen_offset = 16U, /**< Offset of `d_reclen`.    */
  k_posix_darwin_namlen_offset = 18U, /**< Offset of `d_namlen`.    */
  k_posix_darwin_name_offset   = 21U, /**< Offset of `d_name`.      */
  k_posix_darwin_record_align  = 4U,  /**< Kernel record alignment. */
} posix_darwin_dirent_layout_t;

/** @brief Validated view over one raw host directory record. */
typedef struct {
  const char* name;         /**< NUL-terminated name inside the read buffer. */
  uint16_t    name_bytes;   /**< Name length excluding the terminator.       */
  uint16_t    record_bytes; /**< Complete aligned record length.             */
} posix_directory_record_t;

/** @brief Caller-owned cursor and fixed storage for raw directory batches. */
typedef struct {
  uint8_t  buffer[k_posix_directory_buffer_bytes]; /**< Kernel record bytes.  */
  uint32_t valid_bytes;                            /**< Bytes in @ref buffer. */
  uint32_t cursor;                                 /**< Next record offset.   */
} posix_directory_reader_t;

/** @brief POSIX state placed in caller directory workspace. */
typedef struct {
  posix_directory_reader_t reader; /**< Bounded raw-record buffer and cursor. */
  int                      fd;     /**< Owned directory descriptor.           */
} posix_directory_state_t;

/** @brief POSIX state placed in caller file workspace. */
typedef struct {
  /** @brief Owned descriptor, or -1 while closed. */
  int fd;
} posix_file_state_t;

/** @brief POSIX state placed in caller transaction workspace. */
typedef struct {
  /** @brief Final publication path. */
  char destination[k_fw_fs_path_cap];
  /** @brief Private sibling stage path. */
  char stage[k_fw_fs_path_cap];
  /** @brief Open descriptor state for the stage. */
  posix_file_state_t file_state;
  /** @brief Requested destination collision policy. */
  fw_fs_transaction_policy_t policy;
  /** @brief True while the stage descriptor is owned. */
  bool writer_open;
  /** @brief True while the stage leaf needs cleanup. */
  bool stage_exists;
} posix_transaction_state_t;

#ifdef RA8_POSIX_TEST
/** @brief Injected raw-directory read used only by hosted fault tests. */
typedef int64_t (*fw_fs_posix_test_dir_read_fn_t)(void*    ctx,
                                                  int      fd,
                                                  uint8_t* buffer,
                                                  uint32_t capacity,
                                                  int*     out_errno);

/**
 * @brief Select a caller-owned raw-directory reader for the test build.
 * @details Installs @p reader and @p ctx in the port's file-scope selection so
 *          every later raw directory batch is served by the injected callback
 *          instead of the native `getdents64` / `getdirentries64` syscall. This
 *          is the only seam able to produce short, interrupted, or malformed
 *          kernel records on demand, so the bounded retry and record-decoding
 *          paths become reachable without a cooperating host filesystem.
 * @param[in] reader Raw-batch callback replacing the native syscall adapter.
 * @param[in,out] ctx Opaque state handed back to @p reader on every call.
 * @return Reader-selection status.
 * @retval k_ra8_ok Later raw reads dispatch through @p reader.
 * @retval k_ra8_err_null_ptr @p reader or @p ctx is NULL.
 * @pre The translation unit is built with `RA8_POSIX_TEST` defined; no
 *      production configuration declares or defines this helper.
 * @pre @p reader and @p ctx stay valid until a later call replaces them,
 *      because both are retained in file-scope storage rather than copied.
 * @post Success routes every subsequent raw directory batch in this port
 *       through @p reader.
 * @post Failure retains the previously selected reader and context unchanged.
 * @note Not thread-safe: the selection is one file-scope pair shared by every
 *       open directory cursor.
 * @par MC/DC:
 * Decision-free apart from its two null guards; it supplies the fault vectors
 * that reach `internal_directory_fill()`'s `EINTR` retry bound and
 * `internal_directory_decode()`'s malformed-record branches
 * (`port/posix/src/fw_if_fs_posix_common.c@internal_directory_fill`). See
 * `internal_test_empty_mixed_interrupted` and `internal_test_raw_failures` in
 * `tests/misc/src/test_fw_if_fs_posix_raw.c`.
 * @since Version 0.1.0
 */
RA8_TEST_HELPER RA8_NODISCARD ra8_err_t
ra8_fs_posix_test_set_directory_reader(fw_fs_posix_test_dir_read_fn_t reader, void* ctx);
#endif

/**
 * @brief Map one captured errno value into ::ra8_err_t.
 * @details Translates the host failure vocabulary into the portable one with a
 *          total switch, so every backend path in this port reports the same
 *          code for the same condition. Families collapse deliberately:
 *          descriptor and space exhaustion both read as ::k_ra8_err_no_mem, and
 *          `ELOOP` joins the permission family because an unapproved symbolic
 *          link is denied rather than missing. Any value the switch does not
 *          name becomes ::k_ra8_fail, never a success code.
 * @param[in] value Captured `errno` value, or zero for an observed success.
 * @return Portable status for @p value.
 * @retval k_ra8_ok @p value is zero.
 * @retval k_ra8_err_not_found `ENOENT` or `ENOTDIR`.
 * @retval k_ra8_err_exists `EEXIST`.
 * @retval k_ra8_err_no_mem `ENOSPC`, `EDQUOT`, `EMFILE`, or `ENFILE`.
 * @retval k_ra8_err_not_empty `ENOTEMPTY`.
 * @retval k_ra8_err_access_denied `EACCES`, `EPERM`, or `ELOOP`.
 * @retval k_ra8_err_invalid_arg `EINVAL`, `EXDEV`, `ENAMETOOLONG`, or `EISDIR`.
 * @retval k_ra8_err_invalid_size `EFBIG` or `EOVERFLOW`.
 * @retval k_ra8_err_invalid_state `EBADF`.
 * @retval k_ra8_err_busy `EBUSY`.
 * @retval k_ra8_err_not_supported `ENOTSUP` where the host defines it.
 * @retval k_ra8_fail Every other `errno` value.
 * @pre @p value was captured immediately after the failing host call; an
 *      intervening `close` or `stat` may already have overwritten `errno`.
 * @pre @p value is an `errno` code, never a negated syscall return, because a
 *      zero argument is reported as success.
 * @post No caller object, `errno`, or host state is read or written.
 * @post An unrecognized value collapses to ::k_ra8_fail, so no host failure can
 *       be mapped onto a success code.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_errno(int value);

/**
 * @brief Close exactly once and invalidate the caller's descriptor.
 * @details Reads `*fd`, stores -1 back, and only then calls `close`, so a caller
 *          that retries after a failure cannot close a descriptor number the
 *          host has already recycled for another object. An already-invalidated
 *          slot is reported rather than closed, which is what makes the
 *          unconditional cleanup calls on every error path in this port safe.
 * @param[in,out] fd Owned descriptor slot, invalidated before the close.
 * @return Mapped descriptor-close status.
 * @retval k_ra8_ok The descriptor closed successfully.
 * @retval k_ra8_err_invalid_state `*fd` was already negative; nothing was closed.
 * @retval k_ra8_err_* Mapped `close` failure from ::priv_fs_posix_errno.
 * @pre @p fd addresses one writable integer this port owns; the pointer is
 *      dereferenced without a null guard.
 * @pre No other owner closes or reuses `*fd` concurrently; the invalidation
 *      protects only against a repeated call made through @p fd.
 * @post `*fd` is -1 on every return path, including both failure paths.
 * @post `close` is issued at most once, and never for an already-invalid slot.
 * @note Not thread-safe for concurrent access to one descriptor slot.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_close_fd(int* fd);

/**
 * @brief Close one owned descriptor while preserving a primary status.
 * @details Delegates descriptor consumption to ::priv_fs_posix_close_fd. When
 *          @p primary already reports a failure, that failure remains the
 *          caller-visible result even if cleanup also fails. When the primary
 *          operation succeeded, the descriptor-close result is returned so a
 *          cleanup failure cannot be hidden.
 * @param[in,out] fd Owned descriptor slot, invalidated by the close attempt.
 * @param[in] primary Status produced before descriptor cleanup.
 * @return @p primary when it is not ::k_ra8_ok; otherwise the close status.
 * @retval k_ra8_ok The primary operation and descriptor close both succeeded.
 * @retval k_ra8_err_* The primary operation failed, or cleanup failed after a
 *                     successful primary operation.
 * @pre @p fd is non-NULL and addresses one writable descriptor slot.
 * @pre `*fd` satisfies the ownership contract of ::priv_fs_posix_close_fd.
 * @post `*fd` is -1 and the descriptor was closed at most once.
 * @post A primary failure is never replaced by a secondary cleanup failure.
 * @note Not thread-safe for concurrent access to one descriptor slot.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_close_fd_preserve(int* fd, ra8_err_t primary);

/**
 * @brief Classify one exact filesystem-root alias component and target pair.
 * @details Accepts only the byte-exact relative pairs `tmp` -> `private/tmp`
 *          and `var` -> `private/var`. Absolute targets, swapped components,
 *          prefixes, suffixes, truncation, and every other pair remain denied.
 * @param[in] component Terminated candidate alias basename.
 * @param[in] target Raw, not necessarily terminated link-target bytes.
 * @param[in] target_bytes Number of readable bytes in @p target.
 * @param[out] out_alias Receives the classified alias selection.
 * @return Root-alias tuple classification status.
 * @retval k_ra8_ok The component and target are one approved pair.
 * @retval k_ra8_err_access_denied The pair is not approved.
 * @pre @p component, @p target, and @p out_alias are non-NULL.
 * @pre @p target addresses at least @p target_bytes readable bytes.
 * @post Success publishes the pair's non-`none` alias selection.
 * @post Failure leaves @p out_alias set to ::k_posix_root_alias_none.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_root_alias_classify(const char*         component,
                                                                   const char*         target,
                                                                   size_t              target_bytes,
                                                                   posix_root_alias_t* out_alias);

#ifdef __APPLE__
/**
 * @brief Verify one exact Darwin filesystem-root alias without following it.
 * @details Confirms @p parent_fd identifies `/`, reads the candidate through
 *          `readlinkat`, then delegates its exact component and target bytes to
 *          ::priv_fs_posix_root_alias_classify. Nested links remain denied by
 *          the root-identity check and mismatched targets remain denied by the
 *          classifier. This declaration exists only for Darwin builds; every
 *          symbolic link is denied on the other supported POSIX host.
 * @param[in] parent_fd Open descriptor expected to identify `/`.
 * @param[in] component Terminated candidate alias basename.
 * @param[out] out_alias Receives the verified alias selection.
 * @return Root-alias verification status.
 * @retval k_ra8_ok The parent, component, and relative target are the approved tuple.
 * @retval k_ra8_err_access_denied The tuple is not an approved filesystem-root alias.
 * @retval k_ra8_err_* Mapped `fstat`, `stat`, or `readlinkat` failure.
 * @pre @p parent_fd is open for a directory.
 * @pre @p component and @p out_alias are non-NULL.
 * @post Success publishes one non-`none` alias selection.
 * @post Failure leaves @p out_alias set to ::k_posix_root_alias_none.
 * @note Thread-safe subject to host namespace race semantics; callers open the
 *       canonical path no-follow and never follow the inspected alias pathname.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_root_alias_verify(int                 parent_fd,
                                                                 const char*         component,
                                                                 posix_root_alias_t* out_alias);
#endif

/**
 * @brief Open a classified root alias through its canonical components.
 * @details Opens `private` beneath @p root_fd, then the selected `tmp` or `var`
 *          child, applying `O_NOFOLLOW` to both operations. The original alias
 *          path is never opened, closing the validation-to-use replacement
 *          window.
 * @param[in] root_fd Open descriptor for a root-like directory.
 * @param[in] alias Valid non-`none` alias selection.
 * @param[out] out_fd Receives the owned canonical directory descriptor.
 * @return Canonical directory-open status.
 * @retval k_ra8_ok @p out_fd owns the selected canonical directory.
 * @retval k_ra8_err_invalid_arg @p alias is not a supported selection.
 * @retval k_ra8_err_* Mapped canonical open or descriptor-close failure.
 * @pre @p root_fd remains open and contains real `private/tmp` or `private/var`
 *      directories corresponding to @p alias.
 * @pre @p alias is ::k_posix_root_alias_tmp or ::k_posix_root_alias_var.
 * @pre @p out_fd addresses one writable integer descriptor object.
 * @post Success publishes exactly one owned descriptor in @p out_fd.
 * @post Failure publishes no descriptor and closes every descriptor opened here.
 * @warning Darwin production callers must first prove @p root_fd identifies
 *          actual `/` through ::priv_fs_posix_root_alias_verify.
 * @note Fixture tests may exercise the canonical no-follow mechanics beneath a
 *       private root-like directory without weakening the production rule.
 * @note Thread-safe for independent descriptors.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_root_alias_open(int                root_fd,
                                                               posix_root_alias_t alias,
                                                               int*               out_fd);

/**
 * @brief Open one validated directory component without following its pathname.
 * @details Uses no-follow metadata and `openat` beneath @p parent_fd. Linux
 *          rejects every symbolic link. Darwin permits only an exact `tmp` or
 *          `var` alias whose parent descriptor identifies actual `/`, and opens
 *          that alias through canonical no-follow `private` components instead
 *          of its pathname.
 * @param[in] parent_fd Open descriptor for the selected parent directory.
 * @param[in] component Terminated child component without slash bytes.
 * @param[out] out_fd Receives the owned directory descriptor.
 * @return Validated component-open status.
 * @retval k_ra8_ok @p out_fd owns the requested directory.
 * @retval k_ra8_err_access_denied A symbolic link is not an approved Darwin alias.
 * @retval k_ra8_err_not_found The component is absent or is not a directory.
 * @retval k_ra8_err_* Mapped metadata, open, or descriptor-close failure.
 * @pre @p parent_fd is open and @p component is a validated non-empty name.
 * @pre @p out_fd is non-NULL and does not alias either input.
 * @post Success publishes exactly one owned descriptor.
 * @post Failure publishes no descriptor and retains @p parent_fd ownership.
 * @note Thread-safe subject to host namespace race semantics.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_component_open(int         parent_fd,
                                                              const char* component,
                                                              int*        out_fd);

/**
 * @brief Read and validate the next raw hosted directory record.
 * @details Serves the next record from @p reader's fixed buffer and refills that
 *          buffer with one bounded raw syscall batch when the cursor reaches the
 *          end of the previous batch, so enumeration needs no allocator-backed
 *          `DIR` object. Both the caller-supplied cursor state and every decoded
 *          record extent are re-validated on entry, and the published name
 *          borrows bytes inside @p reader instead of being copied. Dot entries
 *          are not filtered here; the portable layer above does that.
 * @param[in] fd Open directory descriptor owning the enumeration position.
 * @param[in,out] reader Caller-owned batch buffer and cursor.
 * @param[out] out Borrowed name view and exact record extent.
 * @param[out] out_end True when the host reported end of directory.
 * @return Raw record read, decode, or cursor-validation status.
 * @retval k_ra8_ok @p out describes one record, or @p out_end is true at end.
 * @retval k_ra8_err_null_ptr @p reader, @p out, or @p out_end is NULL.
 * @retval k_ra8_err_invalid_state @p fd is negative, the cursor and valid extent
 *         disagree, or a native record layout is malformed.
 * @retval k_ra8_err_invalid_size A record name exceeds ::k_posix_component_cap.
 * @retval k_ra8_err_busy Every bounded refill attempt was interrupted.
 * @retval k_ra8_err_* Mapped raw-read failure.
 * @pre @p fd is the same descriptor that produced @p reader's current batch;
 *      pairing a cursor with another descriptor interleaves two enumerations.
 * @pre @p reader was zero-initialized before the first call of an enumeration
 *      and is not shared with a second cursor.
 * @post Success advances the cursor by exactly the decoded record extent, so no
 *       record is delivered twice.
 * @post `out->name` points inside @p reader and stays valid only until the next
 *       call made on the same reader.
 * @note Not thread-safe for concurrent use of one descriptor offset or reader.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_directory_next(int                       fd,
                                                              posix_directory_reader_t* reader,
                                                              posix_directory_record_t* out,
                                                              bool*                     out_end);

/**
 * @brief Convert a POSIX UTC instant into the portable civil representation.
 * @details Rejects an out-of-range subsecond, converts @p seconds with the
 *          reentrant `gmtime_r`, and rejects a civil year that does not fit the
 *          portable uint16_t field. Rejection is total rather than partial: the
 *          zero-initialized value is returned unmodified, so `valid` is the one
 *          flag a caller has to test. No zone conversion is applied.
 * @param[in] seconds POSIX UTC epoch seconds taken from one `struct timespec`.
 * @param[in] nanoseconds Subsecond field of that same `struct timespec`, in
 *                        0..::k_posix_nanosecond_max.
 * @return Portable civil timestamp value.
 * @retval valid==true Every civil field and the subsecond are populated in UTC.
 * @retval valid==false @p nanoseconds is out of range, `gmtime_r` failed, or the
 *         civil year does not fit the portable field; the value stays zeroed.
 * @pre @p seconds is a UTC epoch instant rather than a local-time value, because
 *      the published `utc_offset_min` is hard-coded to zero.
 * @pre @p nanoseconds is the timespec subsecond, not a total nanosecond count,
 *      because it is published beside the separately converted @p seconds.
 * @post Success sets both `valid` and `utc_offset_valid` with a zero
 *       `utc_offset_min`, so the value always reads as UTC.
 * @post Rejection returns a fully zeroed value rather than a partly filled one.
 * @note Pure and thread-safe; the conversion uses the reentrant `gmtime_r`.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD fw_fs_timestamp_t priv_fs_posix_timestamp(time_t seconds, long nanoseconds);

/**
 * @brief Copy one bounded portable path.
 * @details Copies bytes forward until the terminator is reached or the portable
 *          path capacity is exhausted, so an oversized or unterminated source is
 *          reported instead of overrunning @p out. The bound is the portable
 *          ::k_fw_fs_path_cap rather than a host `PATH_MAX`, which keeps every
 *          adapter path the same size as the caller workspace that stores it.
 * @param[out] out Destination holding ::k_fw_fs_path_cap writable bytes.
 * @param[in] path Source path to copy, including its terminator.
 * @return Bounded path-copy status.
 * @retval k_ra8_ok @p out holds the terminated copy.
 * @retval k_ra8_err_invalid_size No terminator appears within the capacity.
 * @pre @p out addresses ::k_fw_fs_path_cap writable bytes and does not overlap
 *      @p path; both pointers are dereferenced without a null guard.
 * @pre @p path stays readable through its terminator, or for
 *      ::k_fw_fs_path_cap bytes when it carries none.
 * @post Success leaves @p out NUL-terminated with at most
 *       ::k_fw_fs_path_cap - 1 visible bytes.
 * @post Failure leaves @p out fully overwritten and unterminated, so a rejected
 *       copy must never be read back as a string.
 * @note Pure apart from the caller's destination, and thread-safe.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_copy_path(char* out, const char* path);

/**
 * @brief Build an 8.3-compatible sibling transaction path.
 * @details Retains everything through @p destination's final `/` and appends the
 *          fixed twelve-byte leaf `TX`, six lowercase hexadecimal digits of
 *          @p id's low 24 bits, and `.TMP`. Staging beside the destination
 *          rather than in a scratch directory is what lets publication be one
 *          rename inside a single directory and filesystem. Uniqueness belongs
 *          to the caller, which advances its transaction counter and retries
 *          when the constructed leaf already exists.
 * @param[in] destination Validated portable destination path.
 * @param[in] id Transaction identifier; only its low 24 bits are rendered.
 * @param[out] out Destination holding ::k_fw_fs_path_cap writable bytes.
 * @return Stage-name construction status.
 * @retval k_ra8_ok @p out holds the terminated sibling stage path.
 * @retval k_ra8_err_invalid_size @p destination has no terminator within
 *         ::k_fw_fs_path_cap, or its parent prefix leaves no room for the leaf.
 * @pre @p destination is a validated portable path beginning with `/`, so the
 *      retained prefix ends at a real separator.
 * @pre @p out addresses ::k_fw_fs_path_cap writable bytes and does not overlap
 *      @p destination.
 * @post Success publishes a path sharing @p destination's parent directory, so
 *       the later publication rename never crosses a filesystem.
 * @post Both capacity checks precede every write, so a rejected call leaves
 *       @p out untouched.
 * @note Pure apart from the caller's destination, and thread-safe; the helper
 *       contributes no uniqueness of its own.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_stage_path(const char* destination,
                                                          uint32_t    id,
                                                          char*       out);

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
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_dir_open(void*       ctx,
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
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_dir_next(void*                 ctx,
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
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_dir_close(void* ctx, void* directory_state);

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
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_listdir(void*           ctx,
                                                       const char*     path,
                                                       uint32_t        max_entries,
                                                       fw_fs_list_fn_t callback,
                                                       void*           callback_ctx,
                                                       uint32_t*       out_count,
                                                       bool*           out_complete);

/**
 * @brief Bind the immutable POSIX operation tables to initialized state.
 * @details Hands ::fw_fs_bind this port's namespace and transaction vtables,
 *          which are translation-unit-scoped, together with the shared table
 *          borrowed from ::priv_fs_posix_stream_iface and @p state as the single
 *          backend context. Routing the bind through one helper is what keeps
 *          those two private tables from needing a second copy in the
 *          initialization unit. Every consistency rule between the tables and
 *          @p caps is enforced inside the facade rather than here.
 * @param[out] out Facade populated with the bound tables, context, and caps.
 * @param[in,out] state Initialized adapter state published as backend context.
 * @param[in] caps Truthful capability and workspace-sizing descriptor.
 * @return Facade bind status.
 * @retval k_ra8_ok @p out is a complete facade over @p state.
 * @retval k_ra8_err_null_ptr @p out, @p state, or @p caps is NULL.
 * @retval k_ra8_err_invalid_arg An advertised capability, workspace alignment,
 *         or root-path rule is not satisfied by the bound tables.
 * @pre @p state is fully initialized with an open root descriptor, because a
 *      successful bind immediately publishes it as the backend context.
 * @pre @p caps describes @p state truthfully, including every workspace size
 *      and power-of-two alignment the facade re-validates.
 * @post Success installs the namespace, stream, and transaction tables and the
 *       same @p state pointer into all three facade sections.
 * @post Failure writes nothing to @p out and releases nothing from @p state, so
 *       unwinding the adapter state stays the caller's responsibility.
 * @note Thread-safe; the bound tables are immutable and statically initialized,
 *       and the call itself writes only @p out.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_bind_interfaces(fw_fs_t*             out,
                                                               fw_fs_posix_state_t* state,
                                                               const fw_fs_caps_t*  caps);

/**
 * @brief Resolve a canonical path's parent without following any symlink.
 * @details Duplicates the bound root and walks each intermediate component with
 *          no-follow stat/open calls, so normal paths remain confined beneath
 *          the owned descriptor as ownership moves. Darwin replaces a verified
 *          actual-root alias with its canonical no-follow descriptor; Linux
 *          rejects every alias and neither platform follows the alias pathname.
 * @param[in] state Initialized confined-root adapter state.
 * @param[in] path Validated portable path below the bound root.
 * @param[out] out_parent_fd Receives the owned parent directory descriptor.
 * @param[out] out_leaf Receives the terminated final component.
 * @return Resolution status.
 * @retval k_ra8_ok Both parent descriptor and leaf were published.
 * @retval k_ra8_err_access_denied An intermediate component is an unapproved symlink.
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
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_parent_open(fw_fs_posix_state_t* state,
                                                           const char*          path,
                                                           int*                 out_parent_fd,
                                                           char*                out_leaf);

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
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_open(void*             ctx,
                                                    const char*       path,
                                                    fw_fs_open_mode_t mode,
                                                    void*             file_state,
                                                    uint32_t          state_bytes);

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
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_write(void*          ctx,
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
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_seek(void* ctx, void* file_state, uint64_t offset);

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
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_size(void*     ctx,
                                                    void*     file_state,
                                                    uint64_t* out_size);

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
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_sync(void* ctx, void* file_state);

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
RA8_PRIV RA8_NODISCARD ra8_err_t priv_fs_posix_close(void* ctx, void* file_state);

/**
 * @brief Borrow the immutable POSIX byte-stream operation table.
 * @details Hands back the single translation-unit-scoped vtable so the adapter's
 *          namespace and transaction units can bind or borrow the same stream
 *          implementation without a second copy of the table.
 * @return Address of the immutable stream operation table.
 * @retval non-NULL The one stream vtable, valid for the program lifetime.
 * @pre The caller only reads through the returned table.
 * @pre No caller attempts to modify the referenced operations.
 * @post The returned table outlives every caller and is never reassigned.
 * @post No adapter state is read or written by the call itself.
 * @note Thread-safe; the table is immutable and statically initialized.
 * @since Version 0.1.0
 */
RA8_PRIV RA8_NODISCARD const fw_fs_stream_iface_t* priv_fs_posix_stream_iface(void);
