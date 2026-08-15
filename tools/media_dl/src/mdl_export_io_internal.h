/**
 * @file mdl_export_io_internal.h
 * @brief Private portable byte-stream contracts for media exporters.
 *
 * @details Binds archive sources and validated publication transactions to the
 * existing downloader storage dependency. The contract owns no allocation,
 * host descriptor, or device-specific path operation.
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
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief One open regular source with a snapshotted identity. */
typedef struct {
  mdl_storage_t* storage; /**< Borrowed exclusive storage binding. */
  fw_fs_file_t   file;    /**< Open portable source stream.        */
  uint64_t       size;    /**< Snapshotted source extent.          */
  uint64_t       offset;  /**< Sequential callback cursor.         */
  uint64_t       hash;    /**< First-pass FNV identity.            */
  uint32_t       calls;   /**< Bounded read-call tally.            */
  ra8_err_t      error;   /**< First callback error, if any.       */
} mdl_export_source_t;

/** @brief One validated staged publication, including random ZIP backfill. */
typedef struct {
  mdl_storage_txn_t writer; /**< Active storage transaction.    */
  mdl_format_t      format; /**< Canonical verifier selection.  */
  uint64_t          offset; /**< Current staged write position. */
  uint64_t          extent; /**< Greatest written byte edge.    */
  ra8_err_t         error;  /**< First callback error, if any.  */
} mdl_export_output_t;

/** @brief Portable archive-output sink signature. */
typedef ra8_err_t (*mdl_export_sink_fn_t)(void* ctx, const uint8_t* bytes, uint32_t length);

/**
 * @brief Bind a validated publication transaction to one destination.
 * @details Initializes the output cursor only after the injected storage
 *          transaction accepts the canonical destination.
 * @param[out] output Caller-owned output state to initialize.
 * @param[in,out] storage Exclusive portable storage transaction provider.
 * @param[in] destination Canonical NUL-terminated destination path.
 * @param[in] format Exact format used by the canonical staged verifier.
 * @return Transaction-open status.
 * @retval k_ra8_ok A new unpublished stage is active.
 * @retval k_ra8_err_invalid_arg An argument or format is invalid.
 * @pre All pointers are non-NULL and @p destination is stable for the call.
 * @pre @p output is not already bound to an active transaction.
 * @post Success initializes every output field and owns one active stage.
 * @post Failure leaves @p output inactive and publishes no destination bytes.
 * @note Thread-safe across distinct storage and output bindings.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_output_begin(mdl_export_output_t* output,
                                                mdl_storage_t*       storage,
                                                const char*          destination,
                                                mdl_format_t         format);

/**
 * @brief Append one complete byte span to an active export stage.
 * @details Retries bounded short writes, advances the sequential cursor, and
 *          retains the first storage failure in the output state.
 * @param[in,out] output Active caller-owned staged output.
 * @param[in] bytes Readable bytes to append.
 * @param[in] length Exact byte count to append.
 * @return Complete-write status.
 * @retval k_ra8_ok Every requested byte was written.
 * @retval k_ra8_fail The sink failed or made zero progress.
 * @pre @p output owns an active transaction and has no retained error.
 * @pre @p bytes addresses @p length readable bytes when length is nonzero.
 * @post Success advances offset and extent by exactly @p length.
 * @post Failure retains the first error and never reports partial success.
 * @note Not thread-safe for a shared output state.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_output_write(mdl_export_output_t* output,
                                                const uint8_t*       bytes,
                                                uint32_t             length);

/**
 * @brief Adapt random-offset miniz output to the active export stage.
 * @details Seeks the injected transaction for ZIP backfill, writes the whole
 *          span with bounded progress, and returns miniz-compatible byte count.
 * @param[in,out] opaque Borrowed ::mdl_export_output_t callback context.
 * @param[in] file_offset Absolute staged-file offset selected by miniz.
 * @param[in] bytes Readable archive bytes.
 * @param[in] length Requested byte count.
 * @return @p length on complete success, otherwise zero.
 * @retval 0 Seek, size, or write failure occurred.
 * @pre @p opaque is a live output and @p bytes is readable for @p length.
 * @pre @p file_offset and @p length admit a uint64 extent without overflow.
 * @post Success updates the cursor and greatest staged extent exactly.
 * @post Failure retains the first error for the owning writer.
 * @note Called synchronously by one miniz writer.
 * @since 0.1.0
 */
RA8_PRIV size_t priv_mdl_export_zip_write(void*       opaque,
                                          mz_uint64   file_offset,
                                          const void* bytes,
                                          size_t      length);

/**
 * @brief Structurally validate and publish one completed export stage.
 * @details Runs the canonical format verifier against the borrowed staged
 *          file, then asks the injected transaction to publish it once.
 * @param[in,out] output Completed caller-owned staged output.
 * @param[in,out] workspace Exclusive caller arena used by the verifier.
 * @param[out] out_published Whether the transaction reports destination visibility.
 * @return Validation or commit status.
 * @retval k_ra8_ok Validation and publication completed.
 * @retval k_ra8_err_validation_failed Canonical structural validation failed.
 * @pre @p output owns an active transaction with no retained writer error.
 * @pre @p workspace and @p out_published are writable and caller-owned.
 * @post Success leaves no active stage and sets @p out_published true.
 * @post Failure reports publication truthfully and leaves cleanup to the transaction.
 * @note Power-loss durability depends on the injected storage capabilities.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_output_commit(mdl_export_output_t*    output,
                                                 mdl_export_workspace_t* workspace,
                                                 bool*                   out_published);

/**
 * @brief Abort one unpublished export stage, retaining cleanup failure.
 * @details Delegates stage removal once and clears the exporter binding only
 *          after capturing the transaction's cleanup result.
 * @param[in,out] output Active caller-owned staged output.
 * @return Abort status.
 * @retval k_ra8_ok The unpublished stage was removed.
 * @retval k_ra8_fail The injected transaction could not clean its stage.
 * @pre @p output is non-NULL and owns an active transaction.
 * @pre No writer or verifier continues to borrow the staged handle.
 * @post The output no longer reports an active transaction.
 * @post The destination is never published by this call.
 * @note Not thread-safe for a shared output state.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_output_abort(mdl_export_output_t* output);

/**
 * @brief Open one regular source and snapshot its exact nonempty extent.
 * @details Rejects missing, empty, symlink, and nonregular nodes before opening
 *          the stream through the injected portable filesystem.
 * @param[out] source Caller-owned source state to initialize.
 * @param[in,out] storage Exclusive portable stream provider.
 * @param[in] path Canonical NUL-terminated source path.
 * @return Source-open status.
 * @retval k_ra8_ok A regular nonempty source is open.
 * @retval k_ra8_err_invalid_arg The node is symlinked or nonregular.
 * @pre Pointers are non-NULL and @p path remains stable for the call.
 * @pre @p source does not own an open stream.
 * @post Success snapshots size and initializes hash/cursor state.
 * @post Failure leaves @p source clear and owns no stream.
 * @note Not thread-safe against concurrent source replacement.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_source_open(mdl_export_source_t* source,
                                               mdl_storage_t*       storage,
                                               const char*          path);

/**
 * @brief Close one open source and clear its retained binding.
 * @details Closes the injected stream exactly once and clears all borrowed
 *          storage and cursor state regardless of close status.
 * @param[in,out] source Open caller-owned source state.
 * @return Close status.
 * @retval k_ra8_ok The source closed cleanly.
 * @retval k_ra8_fail The injected close operation failed.
 * @pre @p source is non-NULL and owns an open portable stream.
 * @pre No callback continues to use the source handle.
 * @post The source owns no open stream.
 * @post The source's retained binding and extent are cleared.
 * @note Not thread-safe for a shared source state.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_source_close(mdl_export_source_t* source);

/**
 * @brief Adapt one sequential miniz source read to a portable file stream.
 * @details Enforces exact sequential offsets, bounded progress, snapshotted
 *          extent, and a running first-pass identity hash for later reread.
 * @param[in,out] opaque Borrowed ::mdl_export_source_t callback context.
 * @param[in] file_offset Absolute source offset requested by miniz.
 * @param[out] destination Writable callback destination.
 * @param[in] capacity Maximum bytes requested.
 * @return Bytes read, or zero on EOF/failure.
 * @retval 0 End of the snapshot or a retained protocol/storage failure.
 * @pre @p opaque owns an open source and @p destination is writable.
 * @pre Calls are sequential and do not exceed the snapshotted extent.
 * @post Success advances the source cursor and hash by the returned count.
 * @post Failure retains a non-ok error unless the snapshot is exactly exhausted.
 * @note Called synchronously by one miniz writer.
 * @since 0.1.0
 */
RA8_PRIV size_t priv_mdl_export_zip_read(void*     opaque,
                                         mz_uint64 file_offset,
                                         void*     destination,
                                         size_t    capacity);

/**
 * @brief Verify the first source pass against an independent reread.
 * @details Requires complete first-pass consumption, seeks to the beginning,
 *          hashes an independent bounded reread, checks size stability, and closes.
 * @param[in,out] source Open source after a complete first pass.
 * @return Identity and close status.
 * @retval k_ra8_ok Both passes and the final size snapshot match.
 * @retval k_ra8_err_validation_failed The source changed between passes.
 * @pre @p source owns an open regular stream.
 * @pre Its first-pass cursor and hash describe the exported member bytes.
 * @post The source is closed on every return path.
 * @post Success proves both passes consumed the snapshotted extent identically.
 * @note Fails closed on seek, read, stat, truncation, or growth.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_source_verify_close(mdl_export_source_t* source);

/**
 * @brief Add one portable source file through miniz callbacks.
 * @details Opens the source through injected storage, supplies deterministic
 *          callback metadata to miniz, then independently rereads before success.
 * @param[in,out] zip Initialized caller-arena miniz writer.
 * @param[in,out] storage Exclusive portable source storage.
 * @param[in] member Canonical archive member name.
 * @param[in] path Canonical source path.
 * @param[in] flags Miniz compression flags.
 * @return Member-add status.
 * @retval k_ra8_ok The complete stable source was added.
 * @retval k_ra8_err_validation_failed The source changed during export.
 * @pre Pointers and strings are non-NULL and stable for the call.
 * @pre @p zip is initialized with caller-owned allocator and output callbacks.
 * @post Success appends exactly one deterministic member.
 * @post Every opened source closes on success and failure.
 * @note Uses a NULL member timestamp to preserve byte determinism.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_zip_add_file(mz_zip_archive* zip,
                                                mdl_storage_t*  storage,
                                                const char*     member,
                                                const char*     path,
                                                mz_uint         flags);

/**
 * @brief Add caller-owned memory through the deterministic ZIP read seam.
 * @details Binds an immutable bounded cursor to miniz's read-callback API so
 *          generated metadata uses the same deterministic member path.
 * @param[in,out] zip Initialized caller-arena miniz writer.
 * @param[in] member Canonical archive member name.
 * @param[in] bytes Immutable caller-owned member bytes.
 * @param[in] length Exact member extent.
 * @param[in] flags Miniz compression flags.
 * @return Member-add status.
 * @retval k_ra8_ok The memory member was added completely.
 * @retval k_ra8_fail Miniz rejected or incompletely consumed the member.
 * @pre Pointers are non-NULL and @p bytes remains readable for @p length.
 * @pre @p zip is initialized and its caller arena remains live.
 * @post Success appends exactly one deterministic member.
 * @post Input bytes and caller ownership remain unchanged.
 * @note Uses a NULL member timestamp to avoid host-clock dependence.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_zip_add_memory(mz_zip_archive* zip,
                                                  const char*     member,
                                                  const uint8_t*  bytes,
                                                  size_t          length,
                                                  mz_uint         flags);

/**
 * @brief Stream one portable source into a bounded caller sink.
 * @details Reads the exact snapshotted extent through storage scratch, offers
 *          each bounded span to the sink, then performs an independent reread.
 * @param[in,out] source Open caller-owned source.
 * @param[in] sink Non-NULL bounded output callback.
 * @param[in,out] sink_ctx Opaque sink context retained by the caller.
 * @return Copy, identity, or close status.
 * @retval k_ra8_ok The whole stable source reached the sink.
 * @retval k_ra8_err_validation_failed The source changed during the copy.
 * @pre @p source owns an open stream and @p sink is callable.
 * @pre The storage I/O scratch is exclusive to this operation.
 * @post The source is closed on every return path.
 * @post Success offers every source byte exactly once in order.
 * @note Sink ownership never transfers to the exporter.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_source_copy(mdl_export_source_t* source,
                                               mdl_export_sink_fn_t sink,
                                               void*                sink_ctx);

/**
 * @brief Read one complete bounded portable source into caller storage.
 * @details Opens and consumes one stable regular file without truncation,
 *          verifies it through an independent reread, and returns exact length.
 * @param[in,out] storage Exclusive portable source storage.
 * @param[in] path Canonical source path.
 * @param[out] destination Caller-owned byte buffer.
 * @param[in] capacity Writable destination capacity.
 * @param[out] out_length Exact copied extent on success.
 * @return Bounded read status.
 * @retval k_ra8_ok A stable complete source was copied.
 * @retval k_ra8_err_invalid_size The snapshot exceeds @p capacity.
 * @pre Pointers are non-NULL and @p destination is writable for @p capacity.
 * @pre @p storage is initialized and exclusive to this read.
 * @post Success sets @p out_length and fills exactly that many bytes.
 * @post Failure sets @p out_length to zero and closes any opened stream.
 * @note No partial buffer content is claimed on failure.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_source_slurp(mdl_storage_t* storage,
                                                const char*    path,
                                                uint8_t*       destination,
                                                size_t         capacity,
                                                size_t*        out_length);

/**
 * @brief Join a canonical directory and leaf without truncation.
 * @details Inserts one separator only when required and rejects arithmetic or
 *          capacity overflow before copying either component.
 * @param[out] out Caller-owned destination string.
 * @param[in] capacity Total writable bytes including the terminator.
 * @param[in] directory Canonical NUL-terminated directory.
 * @param[in] leaf Canonical NUL-terminated leaf.
 * @return Join status.
 * @retval k_ra8_ok The complete joined path fits.
 * @retval k_ra8_err_invalid_size The complete path exceeds @p capacity.
 * @pre All pointers are non-NULL and input strings are stable.
 * @pre @p out is writable for @p capacity bytes and does not overlap inputs.
 * @post Success leaves one NUL-terminated joined path.
 * @post Failure leaves @p out as an empty string when capacity is nonzero.
 * @note Pure apart from the caller-owned destination.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_path_join(char*       out,
                                             size_t      capacity,
                                             const char* directory,
                                             const char* leaf);
