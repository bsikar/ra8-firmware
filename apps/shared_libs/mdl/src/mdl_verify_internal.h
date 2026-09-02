/**
 * @file mdl_verify_internal.h
 * @brief Private bounded validator seams shared by the mdl verifiers.
 * @details Publishes the borrowed-input and miniz-arena state that both the
 *          container verifier and the streamed tarball verifier operate on,
 *          together with the bounded member policy every format shares.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_export.h"
#include "mdl_storage.h"
#include "mdl_verify.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Bounded member policy shared by every structural verifier. */
typedef enum : uint32_t {
  k_verify_member_max = 100000U, /**< Maximum archive member count. */
} mdl_verify_limit_t;

/** @brief Allocation bridge retaining an explicit capacity failure. */
typedef struct {
  mdl_export_workspace_t* workspace; /**< Caller-owned bump arena.   */
  bool                    exhausted; /**< An allocation did not fit. */
} mdl_verify_arena_t;

/** @brief One open portable input and its immutable size snapshot. */
typedef struct {
  mdl_storage_t* storage;    /**< Borrowed storage binding.                */
  fw_fs_file_t*  file;       /**< Borrowed or locally owned facade handle. */
  fw_fs_file_t   owned_file; /**< Path wrapper's facade handle storage.    */
  uint64_t       size_bytes; /**< Length observed after open.              */
  ra8_err_t      read_error; /**< First random-reader failure.             */
  bool           owned;      /**< Whether close remains required.          */
} mdl_verify_io_t;

/**
 * @brief Reserve one aligned span from the verifier's caller-owned arena.
 * @details Rounds the bump cursor up to @p alignment, refuses a request that
 *          would leave the arena, and records the resulting high-water mark.
 * @param[in,out] workspace Exclusive caller-owned bump arena.
 * @param[in] bytes Nonzero span extent to reserve.
 * @param[in] alignment Power-of-two alignment required by the caller.
 * @return Reserved span, or NULL when the request cannot be satisfied.
 * @retval NULL The request was malformed or the arena is exhausted.
 * @retval other One writable span of @p bytes bytes inside the arena.
 * @pre @p workspace is non-NULL and owns writable arena storage.
 * @pre @p alignment is a nonzero power of two.
 * @post Success advances the arena cursor past the reserved span.
 * @post Failure leaves the arena cursor and high-water mark unchanged.
 * @note Not thread-safe for a shared workspace.
 * @since 0.1.0
 */
RA8_PRIV void*
priv_mdl_verify_workspace_take(mdl_export_workspace_t* workspace, size_t bytes, size_t alignment);

/**
 * @brief Allocate one aligned miniz span from a monotonic arena.
 * @details Prefixes each payload with a bounded header recording its extent so
 *          a later reallocation can copy exactly the preserved bytes.
 * @param[in,out] opaque Bound ::mdl_verify_arena_t callback context.
 * @param[in] items Element count requested by miniz.
 * @param[in] size Element size requested by miniz.
 * @return Payload pointer, or NULL when the arena cannot serve the request.
 * @retval NULL The product overflowed or the arena is exhausted.
 * @retval other One writable payload of @p items times @p size bytes.
 * @pre @p opaque addresses one arena bound to a reset workspace.
 * @pre The caller treats NULL as an allocation failure.
 * @post Failure latches the arena exhaustion flag.
 * @post Success reserves exactly one header plus payload block.
 * @note Not thread-safe for a shared arena.
 * @since 0.1.0
 */
RA8_PRIV void* priv_mdl_verify_arena_alloc(void* opaque, size_t items, size_t size);

/**
 * @brief Accept a miniz free for a monotonic arena.
 * @details Individual allocations remain reserved until the owning verifier
 *          resets the workspace, so this release has nothing to reclaim.
 * @param[in,out] opaque Borrowed arena context.
 * @param[in] address Allocation being released.
 * @pre @p opaque is the callback context handed to miniz.
 * @pre @p address is NULL or was produced by ::priv_mdl_verify_arena_alloc.
 * @post Arena offsets are unchanged.
 * @post @p address is not accessed.
 * @note Miniz requires a free callback even for arena allocation.
 * @since 0.1.0
 */
RA8_PRIV void priv_mdl_verify_arena_free(void* opaque, void* address);

/**
 * @brief Read up to a requested portable span.
 * @details Repeats short reads and stops only at end of file or on failure.
 * @param[in,out] io Open verifier input.
 * @param[out] destination Writable destination buffer.
 * @param[in] length Maximum byte count to transfer.
 * @param[out] out_read Byte count actually produced.
 * @return Portable read status.
 * @retval k_ra8_ok Data or a clean end of file was observed.
 * @retval other The portable read failed; @p out_read holds the prefix.
 * @pre @p io is open and @p destination spans @p length bytes.
 * @pre @p out_read is non-NULL and does not alias @p destination.
 * @post @p out_read never exceeds @p length.
 * @post A zero count with ::k_ra8_ok denotes end of file.
 * @note Not thread-safe for a shared input.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_verify_io_read_up_to(mdl_verify_io_t* io,
                                                 uint8_t*         destination,
                                                 size_t           length,
                                                 size_t*          out_read);

/**
 * @brief Recognize supported image suffixes.
 * @details Applies the archive image allowlist case-insensitively.
 * @param[in] name Archive member name.
 * @return Whether the member carries a supported image suffix.
 * @retval true The suffix is one of the supported image extensions.
 * @retval false No supported suffix matched.
 * @pre @p name is a NUL-terminated string.
 * @pre @p name remains valid for the call.
 * @post @p name is unchanged.
 * @post No storage is accessed.
 * @note Content magic is validated during export, never here.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_verify_is_image(const char* name);

/**
 * @brief Reject unsafe archive member paths.
 * @details Rejects absolute, empty, dot, parent, and backslash segments.
 * @param[in] name Archive member name, or NULL.
 * @return Whether the path is lexically contained.
 * @retval true The name is a safe relative path.
 * @retval false The name is NULL, absolute, or escapes its root.
 * @pre @p name is NULL or NUL-terminated.
 * @pre A non-NULL name remains valid for the call.
 * @post @p name is unchanged.
 * @post No filesystem lookup occurs.
 * @note This is a lexical containment check only.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_verify_safe_member_name(const char* name);

/**
 * @brief Validate an uncompressed CBT.
 * @details Streams borrowed reads straight into the USTAR state machine, so
 *          no archive-sized buffer is retained for any member.
 * @param[in,out] io Borrowed open input positioned at offset zero.
 * @param[out] report Candidate report populated only on success.
 * @return Structural validation status.
 * @retval k_ra8_ok Every record and the terminating pair were valid.
 * @retval k_ra8_err_validation_failed A record, name, or terminator was invalid.
 * @retval k_ra8_err_invalid_size A member count or byte total exceeded its bound.
 * @pre Both pointers are non-NULL and @p io is open.
 * @pre The bound storage supplies a nonzero I/O buffer.
 * @post The input remains open and its final cursor is unspecified.
 * @post Success publishes the page, member, and metadata counts.
 * @note Not thread-safe for a shared input.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_verify_tar(mdl_verify_io_t* io, mdl_verify_report_t* report);

/**
 * @brief Validate a gzip-compressed CBT.
 * @details Streams raw inflate into the USTAR state machine and checks the
 *          fixed RFC 1952 framing, the stored CRC32 and ISIZE, and that no
 *          compressed byte follows the end-of-stream marker.
 * @param[in,out] io Borrowed open input positioned at offset zero.
 * @param[in,out] workspace Exclusive caller-owned scratch arena.
 * @param[out] report Candidate report populated only on success.
 * @return Framing, inflate, or structural validation status.
 * @retval k_ra8_ok The complete gzip frame and inner TAR were valid.
 * @retval k_ra8_err_validation_failed Framing, CRC, size, or structure failed.
 * @retval k_ra8_err_invalid_size The scratch arena or a byte bound was exceeded.
 * @pre All pointers are non-NULL and @p io is open.
 * @pre @p workspace was reset by the owning verifier.
 * @post The inflater is ended on every path that opened it.
 * @post Success publishes the page, member, and metadata counts.
 * @note Not thread-safe for a shared input or workspace.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_verify_gzip_tar(mdl_verify_io_t*        io,
                                            mdl_export_workspace_t* workspace,
                                            mdl_verify_report_t*    report);
