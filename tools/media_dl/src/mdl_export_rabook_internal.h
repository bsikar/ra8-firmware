/**
 * @file mdl_export_rabook_internal.h
 * @brief Private bounded RABOOK export contracts for the media downloader.
 * @details Separates portable temporary-EPUB I/O from the fixed-profile
 *          EPUB-to-RABOOK compiler and validated RBKC publication coordinator.
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

#include "mdl_export_internal.h"
#include "mdl_export_io_internal.h"
#include "ra8_epub.h"

/** @brief Open portable EPUB stream retained during one compile. */
typedef struct {
  fw_fs_file_t file;       /**< Borrowed portable file handle.   */
  uint64_t     size_bytes; /**< Immutable complete EPUB extent.  */
  uint32_t     calls;      /**< Bounded random-read call tally.  */
  ra8_err_t    error;      /**< First read/seek protocol failure. */
} mdl_rabook_epub_source_t;

/** @brief Resident flat RABOOK1 source for the chunked container writer. */
typedef struct {
  const uint8_t* bytes;      /**< Immutable flat blob bytes. */
  uint32_t       size_bytes; /**< Complete flat blob extent. */
} mdl_rabook_flat_source_t;

/**
 * @brief Reserve one collision-free temporary EPUB publication path.
 * @details Hashes the final destination into bounded 8.3-compatible sibling
 *          names and uses create-new transactions so no prior file is replaced.
 * @param[out] output Active create-new EPUB output on success.
 * @param[in,out] storage Exclusive portable storage binding.
 * @param[in] directory Canonical directory that will contain the temporary file.
 * @param[in] destination Final RABOOK path used only as a deterministic salt.
 * @param[out] path Destination for the chosen canonical temporary path.
 * @param[in] capacity Writable bytes at @p path.
 * @return Path construction or transaction-open status.
 * @retval k_ra8_ok One private EPUB stage is active.
 * @retval k_ra8_err_exists Every bounded candidate already exists.
 * @retval k_ra8_err_invalid_size The temporary path cannot fit.
 * @pre Pointers are non-NULL and strings are NUL-terminated.
 * @pre @p output is inactive and storage is exclusively owned.
 * @post Success initializes @p path and one active output transaction.
 * @post Failure changes no named file and leaves @p output inactive.
 * @note At most sixteen candidate names are attempted.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_rabook_temp_begin(mdl_export_output_t* output,
                                              mdl_storage_t*       storage,
                                              const char*          directory,
                                              const char*          destination,
                                              char*                path,
                                              size_t               capacity);

/**
 * @brief Open one validated temporary EPUB as a portable random-read stream.
 * @details Stats and opens one regular file, retaining its exact immutable
 *          extent for the callback lifetime.
 * @param[out] source Caller-owned stream state.
 * @param[in,out] storage Exclusive portable storage binding.
 * @param[in] path Canonical temporary EPUB path.
 * @return File metadata/open status.
 * @retval k_ra8_ok A nonempty regular file is open with a stable extent.
 * @retval k_ra8_err_invalid_arg The node is nonregular or an argument is invalid.
 * @pre Pointers are non-NULL and @p source owns no open file.
 * @pre No concurrent writer mutates @p path.
 * @post Success initializes the complete source state.
 * @post Failure leaves @p source clear and owns no file.
 * @note The storage file workspace is borrowed until close.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_rabook_epub_open(mdl_rabook_epub_source_t* source,
                                             mdl_storage_t*            storage,
                                             const char*               path);

/**
 * @brief Close one temporary EPUB stream.
 * @details Delegates to the injected file adapter and clears retained state
 *          only after the close operation succeeds.
 * @param[in,out] source Open source state.
 * @return Portable close status.
 * @retval k_ra8_ok The file closed and state was cleared.
 * @retval other The portable close operation failed.
 * @pre @p source is non-NULL and owns an open file.
 * @pre No EPUB reader continues to borrow the source callback.
 * @post Success clears the complete source state.
 * @post Failure remains visible and the state is not claimed reusable.
 * @note Not thread-safe for a shared source.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_rabook_epub_close(mdl_rabook_epub_source_t* source);

/**
 * @brief Serve one exact random EPUB read to the streamed reader.
 * @details Bounds the request against the retained extent, seeks explicitly,
 *          and latches any short read or excessive callback count.
 * @param[in,out] opaque Bound ::mdl_rabook_epub_source_t.
 * @param[in] offset Absolute archive byte offset.
 * @param[out] destination Writable destination.
 * @param[in] length Exact requested byte count.
 * @return @p length on complete success, otherwise zero.
 * @retval 0 The request was invalid, out of range, short, or faulted.
 * @retval other The exact positive @p length requested by the reader.
 * @pre Pointer arguments are non-NULL and the file remains open.
 * @pre @p destination spans @p length writable bytes.
 * @post Success initializes every requested destination byte.
 * @post Failure latches the first canonical error in the source state.
 * @note The callback is serialized by the EPUB reader.
 * @since 0.1.0
 */
RA8_PRIV size_t priv_mdl_rabook_epub_read(void*    opaque,
                                          uint64_t offset,
                                          void*    destination,
                                          size_t   length);

/**
 * @brief Serve one exact flat-blob read to the RBKC writer.
 * @details Validates offset arithmetic before copying the requested resident
 *          span and reporting its exact transferred count.
 * @param[in] opaque Bound ::mdl_rabook_flat_source_t.
 * @param[in] offset Flat blob byte offset.
 * @param[out] destination Writable destination.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_read Exact copied count on success.
 * @return Range or copy status.
 * @retval k_ra8_ok The complete requested range was copied.
 * @retval k_ra8_err_out_of_range The range exceeds the flat blob.
 * @pre All pointers are non-NULL and spans match their declared extents.
 * @pre The flat blob remains immutable for the call.
 * @post Success initializes the complete destination range.
 * @post Failure sets @p out_read to zero.
 * @note Thread-safe for immutable distinct destinations.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_rabook_flat_read(void*     opaque,
                                             uint32_t  offset,
                                             uint8_t*  destination,
                                             uint32_t  requested,
                                             uint32_t* out_read);
