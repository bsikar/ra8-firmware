/**
 * @file mdl_verify_rabook.c
 * @brief Strict allocation-free RBKC validation for media downloader artifacts.
 *
 * @details Adapts positioned portable-file reads to the production chunked
 * reader and validates every RFC 1950 stream plus the complete inner RABOOK1
 * structure and CRC using only bounded caller workspace.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>

#include "mdl_verify_rabook_internal.h"
#include "miniz.h"
#include "ra8_book_chunked.h"

/** @brief Bounded strict-reader workspace profile. */
typedef enum : uint32_t {
  k_rabook_chunk_bytes      = 4U * 1024U * 1024U, /**< Largest inflated chunk. */
  k_rabook_compressed_bytes = k_rabook_chunk_bytes + (64U * 1024U),
  /**< Largest accepted compressed zlib stream. */
  k_rabook_scratch_bytes = 4U * 1024U * 1024U, /**< CRC and node ownership work. */
  k_rabook_table_entries = 65537U,             /**< At most 65,536 RBKC chunks.  */
  k_rabook_read_calls    = 1000000U,           /**< Short-read progress ceiling. */
} mdl_verify_rabook_limit_t;

/** @brief Positioned exact-read adapter state. */
typedef struct {
  fw_fs_file_t* file;       /**< Borrowed portable file.        */
  uint64_t      size_bytes; /**< Immutable complete extent.     */
  uint32_t      calls;      /**< Bounded backend read attempts. */
} mdl_rabook_io_t;

/**
 * @brief Serve one exact positioned RBKC read through the portable file facade.
 * @details Bounds the range against the snapshotted extent, seeks once, then
 *          tolerates positive short reads under a fixed progress ceiling.
 * @param[in,out] opaque Bound ::mdl_rabook_io_t.
 * @param[in] offset Absolute source byte offset.
 * @param[out] destination Destination spanning @p length bytes.
 * @param[in] length Exact requested byte count.
 * @return Portable seek/read or range status.
 * @retval k_ra8_ok Exactly @p length bytes were copied.
 * @retval k_ra8_err_out_of_range The requested range exceeds the snapshot.
 * @retval k_ra8_err_invalid_state The backend stopped making progress.
 * @pre All pointers are non-NULL and the file remains open.
 * @pre @p destination spans @p length writable bytes.
 * @post Success initializes the complete destination span.
 * @post Failure never claims a complete transfer.
 * @note Not thread-safe for a shared file cursor.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_rabook_read(void* opaque, uint64_t offset, uint8_t* destination, uint32_t length)
{
  mdl_rabook_io_t* io = (mdl_rabook_io_t*)opaque;
  if ((offset > io->size_bytes) || ((uint64_t)length > (io->size_bytes - offset))) {
    return k_ra8_err_out_of_range;
  }
  ra8_err_t error = fw_fs_seek(io->file, offset);
  uint32_t  done  = 0U;
  while ((error == k_ra8_ok) && (done < length)) {
    if (io->calls >= (uint32_t)k_rabook_read_calls) {
      return k_ra8_err_invalid_size;
    }
    uint32_t got = 0U;
    error        = fw_fs_read(io->file, &destination[done], length - done, &got);
    ++io->calls;
    if ((error == k_ra8_ok) && (got == 0U)) {
      error = k_ra8_err_invalid_state;
    }
    done += got;
  }
  return error;
}

/**
 * @brief Inflate one complete RFC 1950 chunk into caller storage.
 * @details Uses miniz's bounded memory inflater and reports the exact produced
 *          extent for the chunk reader's independent length check.
 * @param[in] source Complete compressed stream.
 * @param[in] source_bytes Compressed stream extent.
 * @param[out] destination Inflated destination.
 * @param[in] destination_bytes Writable destination extent.
 * @param[out] out_bytes Exact inflated extent on success.
 * @return Canonical inflate status.
 * @retval k_ra8_ok The zlib stream inflated completely.
 * @retval k_ra8_err_validation_failed The stream was malformed or oversized.
 * @pre Pointer arguments are non-NULL and spans match their byte counts.
 * @pre Source and destination storage do not overlap.
 * @post Success initializes @p out_bytes and that many destination bytes.
 * @post Failure does not publish a usable output extent.
 * @note Performs no allocation and retains no stream state.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rabook_inflate(const void* source,
                                         size_t      source_bytes,
                                         void*       destination,
                                         size_t      destination_bytes,
                                         size_t*     out_bytes)
{
  const size_t result = tinfl_decompress_mem_to_mem(
    destination,
    destination_bytes,
    source,
    source_bytes,
    (int)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  if (result == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
    return k_ra8_err_validation_failed;
  }
  *out_bytes = result;
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_mdl_verify_rabook(fw_fs_file_t*           file,
                                          uint64_t                size_bytes,
                                          mdl_export_workspace_t* workspace,
                                          mdl_verify_report_t*    report)
{
  if ((file == nullptr) || (workspace == nullptr) || (report == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  uint64_t* table =
    (uint64_t*)mdl_export_workspace_take(workspace,
                                         (size_t)k_rabook_table_entries * sizeof(uint64_t),
                                         _Alignof(uint64_t));
  uint8_t* compressed = (uint8_t*)mdl_export_workspace_take(workspace,
                                                            (size_t)k_rabook_compressed_bytes,
                                                            _Alignof(max_align_t));
  uint8_t* chunk      = (uint8_t*)mdl_export_workspace_take(workspace,
                                                            (size_t)k_rabook_chunk_bytes,
                                                            _Alignof(max_align_t));
  uint8_t* scratch    = (uint8_t*)mdl_export_workspace_take(workspace,
                                                            (size_t)k_rabook_scratch_bytes,
                                                            _Alignof(max_align_t));
  if ((table == nullptr) || (compressed == nullptr) || (chunk == nullptr) || (scratch == nullptr)) {
    return k_ra8_err_invalid_size;
  }
  mdl_rabook_io_t    io     = {.file = file, .size_bytes = size_bytes};
  ra8_book_chunked_t reader = {};
  ra8_err_t          error  = ra8_book_chunked_open(&reader,
                                                    internal_rabook_read,
                                                    &io,
                                                    size_bytes,
                                                    internal_rabook_inflate,
                                                    table,
                                                    (uint32_t)k_rabook_table_entries,
                                                    compressed,
                                                    (uint32_t)k_rabook_compressed_bytes);
  ra8_book_header_t  header = {};
  if (error == k_ra8_ok) {
    error = ra8_book_chunked_validate_strict(&reader,
                                             chunk,
                                             (uint32_t)k_rabook_chunk_bytes,
                                             scratch,
                                             (uint32_t)k_rabook_scratch_bytes,
                                             &header);
  }
  if (error == k_ra8_ok) {
    report->page_count   = header.chapter_count;
    report->member_count = header.image_count;
    report->metadata_present =
      (header.title_off != 0U) || (header.author_off != 0U) || (header.identifier_off != 0U);
  }
  return error;
}
