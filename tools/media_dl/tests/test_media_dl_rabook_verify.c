/**
 * @file test_media_dl_rabook_verify.c
 * @brief Positive and corruption tests for the downloader's structural readers.
 *
 * @details Produces a real flat RABOOK1 with the firmware builder, wraps it with
 * the production RBKC writer, publishes it through portable storage, and proves
 * the downloader validator accepts the exact file and rejects a corrupted copy.
 * The same portable storage then feeds hand-built USTAR and RFC 1952 fixtures straight into the
 * streamed tarball validators, driving every framing, checksum, octal-field, terminator, inflate
 * and trailer guard from both sides.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mdl_export.h"
#include "mdl_storage.h"
#include "mdl_test_storage.h"
#include "mdl_verify.h"
#include "mdl_verify_internal.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_book.h"
#include "ra8_rabook_container.h"
#include "support/rabook_compile_test_fixture.h"
#include "tiny_jpeg_fixture.h"
#include "unity_minimal.h"

/** @brief Small complete-book fixture capacities. */
typedef enum : uint32_t {
  k_test_rbkc_bytes         = 8192U,               /**< Complete compressed container.  */
  k_test_chunk_bytes        = 1024U,               /**< Inflated bytes per chunk.       */
  k_test_compressed_bytes   = 2048U,               /**< One compressed stream.          */
  k_test_offset_entries     = 16U,                 /**< Chunk-table entries.            */
  k_test_verify_arena_bytes = 96U * 1024U * 1024U, /**< Writer and strict-reader arena. */
  k_test_small_arena_bytes  = 1U * 1024U * 1024U,  /**< Forced profile rejection.       */
  k_test_dir_work_bytes     = 8192U,               /**< Portable directory cursor.      */
} mdl_rabook_verify_limit_t;

/** @brief Maximally aligned miniz compressor storage. */
typedef struct {
  alignas(max_align_t) tdefl_compressor compressor; /**< Production compressor state. */
} mdl_rabook_compressor_t;

/** @brief Maximally aligned portable directory workspace. */
typedef struct {
  /** @brief Opaque directory cursor state. */
  alignas(max_align_t) uint8_t bytes[k_test_dir_work_bytes];
} mdl_rabook_dir_workspace_t;

/** @brief Memory source/destination for one container write. */
typedef struct {
  const uint8_t* flat;      /**< Complete flat RABOOK1 bytes. */
  uint32_t       flat_size; /**< Flat source extent.          */
  uint8_t*       rbkc;      /**< RBKC destination bytes.      */
  uint32_t       rbkc_cap;  /**< RBKC destination capacity.   */
} mdl_rabook_memory_t;

/** @brief Hand-built USTAR fixture geometry and injected stream sizing. */
typedef enum : uint32_t {
  k_fx_block        = 512U,        /**< USTAR logical record extent.          */
  k_fx_size_off     = 124U,        /**< Size field offset inside a header.    */
  k_fx_size_len     = 12U,         /**< Size field extent.                    */
  k_fx_sum_off      = 148U,        /**< Checksum field offset in a header.    */
  k_fx_sum_len      = 8U,          /**< Checksum field extent.                */
  k_fx_sum_digits   = 6U,          /**< Octal digits a tar checksum holds.    */
  k_fx_type_off     = 156U,        /**< Type flag offset inside a header.     */
  k_fx_spare_off    = 200U,        /**< Header byte no validator ever reads.  */
  k_fx_third_off    = 2560U,       /**< Archive offset of the third record.   */
  k_fx_end_pair     = 2U,          /**< Records in a complete terminator.     */
  k_fx_meta_len     = 30U,         /**< ComicInfo.xml payload extent.         */
  k_fx_page_len     = 700U,        /**< Small image payload extent.           */
  k_fx_page_pad     = 1024U,       /**< Padded extent of that payload.        */
  k_fx_page_big     = 28000U,      /**< Payload forcing chunked inflate.      */
  k_fx_ragged       = 100U,        /**< Unaligned trailing byte count.        */
  k_fx_tar_cap      = 64U * 1024U, /**< Fixture archive capacity.             */
  k_fx_gz_cap       = 16U * 1024U, /**< Fixture gzip frame capacity.          */
  k_fx_io_cap       = 4096U,       /**< Injected stream scratch capacity.     */
  k_fx_io_chunk     = 100U,        /**< Chunk that splits every tar record.   */
  k_fx_io_tiny      = 64U,         /**< Chunk forcing many inflate rounds.    */
  k_fx_sentinel     = 4242U,       /**< Marker proving a report is untouched. */
  k_fx_arena_tiny   = 64U,         /**< Arena too small for the output span.  */
  k_fx_arena_mid    = 20000U,      /**< Arena too small for the inflater.     */
  k_fx_chunk_len    = 8192U,       /**< Bounded decoded chunk extent.         */
  k_fx_want_pages   = 2U,          /**< Image members in the valid fixture.   */
  k_fx_want_members = 3U,          /**< Total members in the valid fixture.   */
} mdl_tarball_fixture_t;

/** @brief Fixed RFC 1952 framing emitted and corrupted by the fixture writer. */
typedef enum : uint16_t {
  k_fx_gz_head   = 10U,   /**< Fixed gzip header extent.            */
  k_fx_gz_tail   = 8U,    /**< CRC32 plus ISIZE trailer extent.     */
  k_fx_gz_isize  = 4U,    /**< ISIZE offset inside that trailer.    */
  k_fx_gz_id1    = 0x1FU, /**< First RFC 1952 magic byte.           */
  k_fx_gz_id2    = 0x8BU, /**< Second RFC 1952 magic byte.          */
  k_fx_gz_method = 8U,    /**< RFC 1952 DEFLATE method identifier.  */
  k_fx_gz_probes = 128U,  /**< Deflate hash probes per lookup.      */
  k_fx_gz_extra  = 4U,    /**< Bytes appended after the end marker. */
  k_fx_gz_cut    = 8U,    /**< Compressed bytes withheld from feed. */
  k_fx_gz_over   = 1000U, /**< Bytes claimed beyond the real file.  */
  k_fx_gz_flag   = 0x04U, /**< Unsupported FEXTRA flag bit.         */
  k_fx_gz_short  = 17U,   /**< Extent below the fixed framing.      */
} mdl_gzip_fixture_t;

/** @brief Byte arithmetic and arena edges used by the fixture encoders. */
typedef enum : uint8_t {
  k_fx_oct_mask  = 7U,    /**< Low octal digit mask.                 */
  k_fx_oct_shift = 3U,    /**< Bits consumed per octal digit.        */
  k_fx_byte_mask = 0xFFU, /**< Single byte mask.                     */
  k_fx_shift1    = 8U,    /**< Shift for little-endian byte one.     */
  k_fx_shift2    = 16U,   /**< Shift for little-endian byte two.     */
  k_fx_shift3    = 24U,   /**< Shift for little-endian byte three.   */
  k_fx_idx0      = 0U,    /**< First byte index.                     */
  k_fx_idx1      = 1U,    /**< Second byte index.                    */
  k_fx_idx2      = 2U,    /**< Third byte index.                     */
  k_fx_idx3      = 3U,    /**< Fourth byte index.                    */
  k_fx_valid     = 0U,    /**< Table index of the complete archive.  */
  k_fx_large     = 9U,    /**< Table index of the oversized archive. */
  k_fx_flip      = 0x01U, /**< Single-bit corruption mask.           */
  k_fx_big_align = 16U,   /**< Alignment overrunning the edge arena. */
  k_fx_edge_cap  = 8U,    /**< Edge arena capacity.                  */
  k_fx_odd_align = 3U,    /**< Non power-of-two alignment.           */
  k_fx_one       = 1U,    /**< One: byte extent, alignment, record.  */
  k_fx_digit_at  = 3U,    /**< Size field digit index corrupted.     */
} mdl_fixture_byte_t;

/** @brief One archive shape fed straight into the streamed USTAR validator. */
typedef struct {
  const char* label;      /**< Diagnostic name of the shape.         */
  bool        metadata;   /**< Emit the ComicInfo.xml member.        */
  uint32_t    page_bytes; /**< Image payload extent, zero to omit.   */
  uint32_t    drop_bytes; /**< Bytes cut from the last payload.      */
  uint8_t     end_blocks; /**< Terminator records actually written.  */
  uint16_t    tail_bytes; /**< Extra bytes written after the end.    */
  uint8_t     tail_fill;  /**< Value stored in those extra bytes.    */
  bool        accepted;   /**< Whether the validator must accept it. */
} mdl_tar_shape_t;

/** @brief One byte-level corruption of an otherwise complete archive. */
typedef struct {
  const char* label;      /**< Diagnostic name of the corruption.     */
  uint16_t    offset;     /**< Archive byte offset being overwritten. */
  uint8_t     fill;       /**< Replacement byte value.                */
  uint8_t     count;      /**< Replacement extent in bytes.           */
  bool        rechecksum; /**< Repair the record checksum afterwards. */
} mdl_tar_poke_t;

/** @brief One single-byte flip applied to a complete gzip frame. */
typedef struct {
  const char* label;    /**< Diagnostic name of the corruption.     */
  uint16_t    offset;   /**< Frame offset, from the end when noted. */
  uint8_t     flip;     /**< Bits inverted at that offset.          */
  bool        from_end; /**< Measure @c offset back from the end.   */
} mdl_gz_poke_t;

static const mdl_tar_shape_t s_tar_shapes[] = {
  {"complete cbt", true, k_fx_page_len, 0U, k_fx_end_pair, 0U, 0U, true},
  {"aligned zero tail", true, k_fx_page_len, 0U, k_fx_end_pair, k_fx_block, 0U, true},
  {"ragged zero tail", true, k_fx_page_len, 0U, k_fx_end_pair, k_fx_ragged, 0U, false},
  {"non-zero tail", true, k_fx_page_len, 0U, k_fx_end_pair, k_fx_block, k_fx_byte_mask, false},
  {"missing terminator", true, k_fx_page_len, 0U, 0U, 0U, 0U, false},
  {"single terminator record", true, k_fx_page_len, 0U, k_fx_one, 0U, 0U, false},
  {"no metadata member", false, k_fx_page_len, 0U, k_fx_end_pair, 0U, 0U, false},
  {"no image member", true, 0U, 0U, k_fx_end_pair, 0U, 0U, false},
  {"payload past end", true, k_fx_page_len, k_fx_page_pad, 0U, 0U, 0U, false},
  {"large cbt", true, k_fx_page_big, 0U, k_fx_end_pair, 0U, 0U, true},
};

static const mdl_tar_poke_t s_tar_pokes[] = {
  {"unsupported typeflag", k_fx_third_off + k_fx_type_off, (uint8_t)'5', k_fx_one, true},
  {"stale record checksum", k_fx_spare_off, (uint8_t)'D', k_fx_one, false},
  {"above-octal checksum digit", k_fx_sum_off + 1U, (uint8_t)'9', k_fx_one, false},
  {"below-octal checksum digit", k_fx_sum_off + 1U, (uint8_t)'/', k_fx_one, false},
  {"blank checksum field", k_fx_sum_off, (uint8_t)' ', k_fx_sum_len, false},
  {"above-octal size digit", k_fx_size_off + k_fx_digit_at, (uint8_t)'8', k_fx_one, true},
  {"blank size field", k_fx_size_off, 0U, k_fx_size_len, true},
  {"absolute member name", k_fx_page_pad, (uint8_t)'/', k_fx_one, true},
};

static const mdl_gz_poke_t s_gz_pokes[] = {
  {"first magic byte", k_fx_idx0, k_fx_flip, false},
  {"second magic byte", k_fx_idx1, k_fx_flip, false},
  {"compression method", k_fx_idx2, k_fx_flip, false},
  {"unsupported flag bits", k_fx_idx3, k_fx_gz_flag, false},
  {"stored crc32", k_fx_gz_tail, k_fx_flip, true},
  {"stored isize", k_fx_gz_isize, k_fx_flip, true},
};

static const char s_stream_path[] = "/tmp/mdl-verify-stream.bin";

static uint8_t                s_tar[k_fx_tar_cap];
static uint8_t                s_gz[k_fx_gz_cap];
static uint8_t                s_io[k_fx_io_cap];
static mdl_export_workspace_t s_gz_workspace;

static ra8_test_rabook_fixture_t  s_fixture;
static uint8_t                    s_rbkc[k_test_rbkc_bytes];
static uint8_t                    s_chunk[k_test_chunk_bytes];
static uint8_t                    s_compressed[k_test_compressed_bytes];
static uint64_t                   s_offsets[k_test_offset_entries];
static mdl_rabook_compressor_t    s_compressor;
static uint8_t                    s_verify_arena[k_test_verify_arena_bytes];
static uint8_t                    s_small_arena[k_test_small_arena_bytes];
static mdl_rabook_dir_workspace_t s_dir_workspace;

/**
 * @brief Copy one exact flat range. @details Bounds the interval before copying it out.
 * @param[in] opaque Bound ::mdl_rabook_memory_t. @param[in] offset Source offset. @param[out] destination Read destination. @param[in] requested Exact bytes. @param[out] out_read Copied bytes. @return Status. @retval k_ra8_ok The complete range was copied. @retval k_ra8_err_out_of_range The range exceeds the blob.
 * @pre All pointers are valid for their extents. @pre The flat fixture stays immutable.
 * @post Success initializes every requested byte. @post Failure sets @p out_read to zero. @note Test-only and synchronous. @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_flat_read(void*     opaque,
                                    uint32_t  offset,
                                    uint8_t*  destination,
                                    uint32_t  requested,
                                    uint32_t* out_read)
{
  mdl_rabook_memory_t* memory = (mdl_rabook_memory_t*)opaque;
  *out_read                   = 0U;
  if ((offset > memory->flat_size) || (requested > (memory->flat_size - offset))) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(destination, &memory->flat[offset], requested);
  *out_read = requested;
  return k_ra8_ok;
}

/**
 * @brief Copy one exact RBKC range. @details Implements the writer's random-write contract.
 * @param[in] opaque Bound ::mdl_rabook_memory_t. @param[in] offset Destination offset. @param[in] source Source bytes. @param[in] requested Exact bytes. @param[out] out_written Copied bytes. @return Status. @retval k_ra8_ok The complete range was copied. @retval k_ra8_err_invalid_size The range exceeds its cap.
 * @pre All pointers are valid for their extents. @pre The destination stays exclusively mutable.
 * @post Success initializes the requested range. @post Failure sets @p out_written to zero. @note Test-only and synchronous. @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rbkc_write(void*          opaque,
                                     uint64_t       offset,
                                     const uint8_t* source,
                                     uint32_t       requested,
                                     uint32_t*      out_written)
{
  mdl_rabook_memory_t* memory = (mdl_rabook_memory_t*)opaque;
  *out_written                = 0U;
  if ((offset > memory->rbkc_cap) ||
      ((uint64_t)requested > ((uint64_t)memory->rbkc_cap - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(&memory->rbkc[(size_t)offset], source, requested);
  *out_written = requested;
  return k_ra8_ok;
}

/**
 * @brief Publish one byte span through the transaction seam. @details Stages, validates, commits.
 * @param[in] path Canonical destination. @param[in] bytes Payload bytes. @param[in] length Payload extent. @return Status. @retval k_ra8_ok The fixture was published. @retval other Namespace, write, validation or commit failure.
 * @pre Storage is initialized and every pointer is valid. @pre No concurrent fixture writer exists.
 * @post Success exposes exactly @p length bytes. @post Failure claims no publication. @note Test-only and serial. @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_publish(const char* path, const uint8_t* bytes, uint32_t length)
{
  mdl_storage_t* storage = mdl_test_storage_get();
  fw_fs_stat_t   stat    = {};
  ra8_err_t      error   = fw_fs_stat(&storage->fs->names, path, &stat);
  if ((error == k_ra8_ok) && stat.exists) {
    error = fw_fs_unlink(&storage->fs->names, path);
  }
  mdl_storage_txn_t writer = {};
  if (error == k_ra8_ok) {
    error = mdl_storage_txn_begin_new(&writer, storage, path);
  }
  if (error == k_ra8_ok) {
    error = mdl_storage_txn_write(&writer, bytes, length);
  }
  if (error == k_ra8_ok) {
    error = mdl_storage_txn_commit(&writer);
  } else if (writer.transaction.active) {
    const ra8_err_t aborted = mdl_storage_txn_abort(&writer);
    if (aborted != k_ra8_ok) {
      error = aborted;
    }
  }
  return error;
}

/**
 * @brief Remove one fixture path when present. @details A missing path is already success.
 * @param[in] path Canonical fixture path. @param[in] directory Remove it as a directory. @return Status. @retval k_ra8_ok The path is now absent. @retval other The namespace operation failed.
 * @pre Test storage is initialized and @p path is non-NULL. @pre No cursor retains the node.
 * @post Success leaves the path absent. @post Unrelated entries are unchanged. @note Test-only and serial. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_remove_path(const char* path, bool directory)
{
  mdl_storage_t* storage = mdl_test_storage_get();
  fw_fs_stat_t   stat    = {};
  ra8_err_t      error   = fw_fs_stat(&storage->fs->names, path, &stat);
  if ((error == k_ra8_ok) && stat.exists) {
    error =
      directory ? fw_fs_rmdir(&storage->fs->names, path) : fw_fs_unlink(&storage->fs->names, path);
  }
  return error;
}

/**
 * @brief Prove the private EPUB intermediate was removed. @details Accepts only the original page.
 * @param[in] directory Canonical fixture directory. @return Status. @retval k_ra8_ok Exactly the original page remains. @retval k_ra8_err_validation_failed A leaked or unexpected entry remains.
 * @pre Test storage is initialized and @p directory exists. @pre No concurrent mutation occurs.
 * @post Every entry remains unchanged. @post The cursor is closed on every opened path. @note Test-only and serial. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_check_no_temp(const char* directory)
{
  mdl_storage_t* storage = mdl_test_storage_get();
  fw_fs_dir_t    cursor  = {};
  ra8_err_t      error   = fw_fs_dir_open(&storage->fs->names,
                                          directory,
                                          &cursor,
                                          s_dir_workspace.bytes,
                                          sizeof(s_dir_workspace.bytes));
  uint32_t       entries = 0U;
  bool           more    = true;
  while ((error == k_ra8_ok) && more) {
    fw_fs_dirent_value_t value = {};
    error                      = fw_fs_dir_next(&cursor, &value, &more);
    if ((error == k_ra8_ok) && more) {
      ++entries;
      if (strcmp(value.name, "001.jpg") != 0) {
        error = k_ra8_err_validation_failed;
      }
    }
  }
  if (cursor.is_open) {
    const ra8_err_t closed = fw_fs_dir_close(&cursor);
    if ((error == k_ra8_ok) && (closed != k_ra8_ok)) {
      error = closed;
    }
  }
  return ((error == k_ra8_ok) && (entries != 1U)) ? k_ra8_err_validation_failed : error;
}

/**
 * @brief Build the flat book and wrap it in a real RBKC. @details Runs the production writers.
 * @param[out] rbkc_size Receives the encoded extent. @return Status. @retval k_ra8_ok The container occupies @p s_rbkc. @retval other Finalization or container encoding failed.
 * @pre The shared fixture arrays are exclusively owned. @pre @p rbkc_size is writable.
 * @post Success publishes an extent no larger than @p s_rbkc. @post Failure claims nothing about the output. @note Test-only and serial. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_build_rbkc(uint64_t* rbkc_size)
{
  ra8_rabook_ctx_t            builder   = {};
  ra8_test_rabook_roundtrip_t roundtrip = {};
  ra8_test_rabook_init(&s_fixture, &builder);
  ra8_test_rabook_populate(&s_fixture, &builder, &roundtrip, false);
  ra8_err_t error = ra8_rabook_finalize(&builder, &roundtrip.blob, &roundtrip.blob_len);

  mdl_rabook_memory_t              memory    = {.flat      = (const uint8_t*)roundtrip.blob,
                                                .flat_size = roundtrip.blob_len,
                                                .rbkc      = s_rbkc,
                                                .rbkc_cap  = sizeof(s_rbkc)};
  ra8_rabook_container_workspace_t workspace = {
    .input          = s_chunk,
    .compressed     = s_compressed,
    .compressor     = &s_compressor.compressor,
    .offsets        = s_offsets,
    .input_cap      = sizeof(s_chunk),
    .compressed_cap = sizeof(s_compressed),
    .compressor_cap = sizeof(s_compressor.compressor),
    .offset_cap     = (uint32_t)k_test_offset_entries,
  };
  if (error == k_ra8_ok) {
    error = ra8_rabook_container_write(internal_flat_read,
                                       &memory,
                                       roundtrip.blob_len,
                                       (uint32_t)k_test_chunk_bytes,
                                       internal_rbkc_write,
                                       &memory,
                                       &workspace,
                                       rbkc_size);
  }
  return error;
}

/**
 * @test internal_test_strict_rabook_verify
 * @brief A real RBKC passes and a one-byte-corrupted stream is rejected.
 * @details Builds through the production flat and container writers, publishes through portable
 * transactions, checks the reported counts, then flips a compressed payload byte.
 * @pre The shared fixture arrays and `/tmp` paths are exclusively owned. @pre Storage is initialized.
 * @post The valid report matches the builder fixture. @post Both temporary files are removed. @note Test-only; assertion failure terminates the process. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_strict_rabook_verify(void)
{
  TEST_BEGIN("media rabook strict verify");
  uint64_t rbkc_size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_build_rbkc(&rbkc_size));
  TEST_ASSERT(rbkc_size <= UINT32_MAX);
  const char* const valid_path = "/tmp/mdl-rabook-valid.rabook";
  const char* const bad_path   = "/tmp/mdl-rabook-bad.rabook";
  TEST_ASSERT_EQ(k_ra8_ok, internal_publish(valid_path, s_rbkc, (uint32_t)rbkc_size));

  mdl_export_workspace_t verify_workspace;
  mdl_export_workspace_init(&verify_workspace, s_verify_arena, sizeof(s_verify_arena));
  mdl_verify_report_t report = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_verify_file(mdl_test_storage_get(),
                                 k_ra8_mdl_format_rabook,
                                 valid_path,
                                 &verify_workspace,
                                 &report));
  TEST_ASSERT_EQ(1U, report.page_count);
  TEST_ASSERT_EQ(1U, report.member_count);
  TEST_ASSERT(report.metadata_present);

  s_rbkc[(size_t)rbkc_size - 1U] ^= UINT8_C(0x01);
  TEST_ASSERT_EQ(k_ra8_ok, internal_publish(bad_path, s_rbkc, (uint32_t)rbkc_size));
  mdl_export_workspace_init(&verify_workspace, s_verify_arena, sizeof(s_verify_arena));
  TEST_ASSERT(mdl_verify_file(mdl_test_storage_get(),
                              k_ra8_mdl_format_rabook,
                              bad_path,
                              &verify_workspace,
                              &report) != k_ra8_ok);

  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&mdl_test_storage_get()->fs->names, valid_path));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&mdl_test_storage_get()->fs->names, bad_path));
  TEST_END("media rabook strict verify");
}

/**
 * @test internal_test_rabook_writer
 * @brief A real page directory exports to strict RBKC with failure preservation.
 * @details Writes a deterministic JPEG, runs the public chapter exporter, verifies the reader
 * report, proves no private EPUB remains, then forces workspace exhaustion.
 * @pre Test storage and shared arenas are exclusively owned. @pre The `/tmp` fixtures are writable.
 * @post The writer output and all sources are removed. @post The failed export leaves the prior artifact unchanged. @note Test-only; assertion failure terminates the process. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_rabook_writer(void)
{
  TEST_BEGIN("media rabook writer");
  const char* const directory = "/tmp/mdl-rabook-export";
  const char* const page      = "/tmp/mdl-rabook-export/001.jpg";
  const char* const output    = "/tmp/mdl-rabook-export.rabook";
  TEST_ASSERT_EQ(k_ra8_ok, internal_remove_path(output, false));
  TEST_ASSERT_EQ(k_ra8_ok, internal_remove_path(page, false));
  TEST_ASSERT_EQ(k_ra8_ok, internal_remove_path(directory, true));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&mdl_test_storage_get()->fs->names, directory));
  TEST_ASSERT_EQ(k_ra8_ok, internal_publish(page, s_tiny_jpeg, s_tiny_jpeg_len));

  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  (void)memcpy(meta.series_title, "Writer Fixture", sizeof("Writer Fixture"));
  (void)memcpy(meta.chapter_title, "Chapter One", sizeof("Chapter One"));
  (void)memcpy(meta.identifier, "urn:ra8:test:rabook-writer", sizeof("urn:ra8:test:rabook-writer"));
  mdl_export_workspace_t workspace;
  mdl_export_workspace_init(&workspace, s_verify_arena, sizeof(s_verify_arena));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_export_chapter_meta_ws(mdl_test_storage_get(),
                                            k_ra8_mdl_format_rabook,
                                            directory,
                                            output,
                                            &meta,
                                            &workspace));
  TEST_ASSERT(workspace.high_water <= sizeof(s_verify_arena));
  TEST_ASSERT_EQ(k_ra8_ok, internal_check_no_temp(directory));

  mdl_verify_report_t report = {};
  mdl_export_workspace_init(&workspace, s_verify_arena, sizeof(s_verify_arena));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    mdl_verify_file(mdl_test_storage_get(), k_ra8_mdl_format_rabook, output, &workspace, &report));
  TEST_ASSERT_EQ(1U, report.page_count);
  TEST_ASSERT_EQ(1U, report.member_count);
  TEST_ASSERT(report.metadata_present);

  mdl_export_workspace_init(&workspace, s_small_arena, sizeof(s_small_arena));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 mdl_export_chapter_meta_ws(mdl_test_storage_get(),
                                            k_ra8_mdl_format_rabook,
                                            directory,
                                            output,
                                            &meta,
                                            &workspace));
  mdl_export_workspace_init(&workspace, s_verify_arena, sizeof(s_verify_arena));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    mdl_verify_file(mdl_test_storage_get(), k_ra8_mdl_format_rabook, output, &workspace, &report));
  TEST_ASSERT_EQ(k_ra8_ok, internal_check_no_temp(directory));

  TEST_ASSERT_EQ(k_ra8_ok, internal_remove_path(output, false));
  TEST_ASSERT_EQ(k_ra8_ok, internal_remove_path(page, false));
  TEST_ASSERT_EQ(k_ra8_ok, internal_remove_path(directory, true));
  TEST_END("media rabook writer");
}

/**
 * @brief Encode one bounded octal tar field. @details Emits @p width minus one digits then a NUL.
 * @param[out] field Field bytes. @param[in] width Field extent. @param[in] value Encoded value.
 * @pre @p field spans @p width writable bytes. @pre @p width is at least two.
 * @post The field holds a zero-padded octal number. @post The final byte is NUL.
 * @note Test-only and synchronous. @since 0.1.0
 */
RA8_INTERNAL static void internal_put_octal(uint8_t* field, size_t width, uint64_t value)
{
  uint64_t left  = value;
  size_t   index = width - 1U;
  field[index]   = (uint8_t)'\0';
  while (index > 0U) {
    --index;
    field[index] = (uint8_t)((uint64_t)'0' + (left & (uint64_t)k_fx_oct_mask));
    left >>= (uint64_t)k_fx_oct_shift;
  }
}

/**
 * @brief Store one little-endian word. @details Writes four bytes without alignment assumptions.
 * @param[out] bytes Four writable bytes. @param[in] value Word to encode.
 * @pre @p bytes spans four writable bytes. @pre @p bytes is unaliased for the call.
 * @post All four bytes are initialized. @post The encoding is host-endian independent.
 * @note Test-only and synchronous. @since 0.1.0
 */
RA8_INTERNAL static void internal_put_u32le(uint8_t* bytes, uint32_t value)
{
  bytes[k_fx_idx0] = (uint8_t)(value & (uint32_t)k_fx_byte_mask);
  bytes[k_fx_idx1] = (uint8_t)((value >> k_fx_shift1) & (uint32_t)k_fx_byte_mask);
  bytes[k_fx_idx2] = (uint8_t)((value >> k_fx_shift2) & (uint32_t)k_fx_byte_mask);
  bytes[k_fx_idx3] = (uint8_t)((value >> k_fx_shift3) & (uint32_t)k_fx_byte_mask);
}

/**
 * @brief Repair one record checksum. @details Sums the record with its own field read as spaces.
 * @param[in,out] block One complete 512-byte record.
 * @pre @p block spans one complete record. @pre Every other field is already final.
 * @post The record satisfies the POSIX checksum rule. @post The field ends in NUL then space.
 * @note Test-only; call it last for any record. @since 0.1.0
 */
RA8_INTERNAL static void internal_put_checksum(uint8_t* block)
{
  (void)memset(&block[k_fx_sum_off], (int)' ', (size_t)k_fx_sum_len);
  uint32_t sum = 0U;
  for (size_t i = 0U; i < (size_t)k_fx_block; ++i) {
    sum += block[i];
  }
  internal_put_octal(&block[k_fx_sum_off], (size_t)k_fx_sum_digits + 1U, (uint64_t)sum);
  block[(size_t)k_fx_sum_off + (size_t)k_fx_sum_digits + 1U] = (uint8_t)' ';
}

/**
 * @brief Append one regular USTAR member. @details Writes the header then reserves its padding.
 * @param[in,out] used Cursor advanced past the member. @param[in] name Path. @param[in] size Payload extent.
 * @pre The shared archive buffer is already zeroed. @pre The member fits the remaining capacity.
 * @post Only fields this validator parses are written. @post The cursor lands on a record boundary.
 * @note Test-only; the payload stays zero. @since 0.1.0
 */
RA8_INTERNAL static void internal_tar_append(size_t* used, const char* name, uint32_t size)
{
  uint8_t* block = &s_tar[*used];
  (void)memcpy(block, name, strlen(name));
  internal_put_octal(&block[k_fx_size_off], (size_t)k_fx_size_len, (uint64_t)size);
  block[k_fx_type_off] = (uint8_t)'0';
  internal_put_checksum(block);
  *used += (size_t)k_fx_block;
  *used += (((size_t)size + (size_t)k_fx_block - 1U) / (size_t)k_fx_block) * (size_t)k_fx_block;
}

/**
 * @brief Compose one archive shape. @details Selects members, terminator records and trailing bytes.
 * @param[in] shape Requested archive shape. @return Complete archive extent in bytes.
 * @retval 0 The shape requested no member and no terminator.
 * @pre @p shape is one table row. @pre The shape fits the shared archive buffer.
 * @post The buffer holds exactly the returned extent. @post Untouched bytes remain zero.
 * @note Test-only and not thread-safe. @since 0.1.0
 */
RA8_INTERNAL static size_t internal_tar_build(const mdl_tar_shape_t* shape)
{
  (void)memset(s_tar, 0, sizeof(s_tar));
  size_t used = 0U;
  if (shape->metadata) {
    internal_tar_append(&used, "ComicInfo.xml", (uint32_t)k_fx_meta_len);
  }
  if (shape->page_bytes != 0U) {
    internal_tar_append(&used, "001.jpg", shape->page_bytes);
    internal_tar_append(&used, "002.jpg", shape->page_bytes);
  }
  used -= (size_t)shape->drop_bytes;
  used += (size_t)shape->end_blocks * (size_t)k_fx_block;
  (void)memset(&s_tar[used], (int)shape->tail_fill, (size_t)shape->tail_bytes);
  return used + (size_t)shape->tail_bytes;
}

/**
 * @brief Compose one corrupted archive. @details Builds the complete shape then rewrites one field.
 * @param[in] poke Requested corruption. @return Complete archive extent in bytes.
 * @retval 0 Never; the complete shape is always non-empty.
 * @pre @p poke is one table row. @pre The corrupted span lies inside the archive.
 * @post Exactly one field differs from the accepted archive. @post A repair isolates one guard.
 * @note Test-only and not thread-safe. @since 0.1.0
 */
RA8_INTERNAL static size_t internal_tar_poked(const mdl_tar_poke_t* poke)
{
  const size_t length = internal_tar_build(&s_tar_shapes[k_fx_valid]);
  (void)memset(&s_tar[poke->offset], (int)poke->fill, (size_t)poke->count);
  if (poke->rechecksum) {
    internal_put_checksum(&s_tar[((size_t)poke->offset / (size_t)k_fx_block) * (size_t)k_fx_block]);
  }
  return length;
}

/**
 * @brief Open the published fixture as a borrowed verifier input.
 * @details Mirrors the production open seam but leaves the size snapshot under test control, so an
 * overstated or understated extent can drive the streamed readers' bounds.
 * @param[in,out] storage Bound storage. @param[out] io Input to initialize. @param[in] size_bytes Snapshot to honour.
 * @return Portable open status. @retval k_ra8_ok The fixture is open and must be closed.
 * @pre The fixture was published at ::s_stream_path. @pre @p io is exclusively owned.
 * @post Success leaves exactly one open handle. @post Failure leaves no handle to close.
 * @note Test-only and serial. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_io_open_fixture(mdl_storage_t* storage, mdl_verify_io_t* io, uint64_t size_bytes)
{
  *io                   = (mdl_verify_io_t){.storage = storage, .size_bytes = size_bytes};
  io->file              = &io->owned_file;
  const ra8_err_t error = fw_fs_open(&storage->fs->streams,
                                     s_stream_path,
                                     k_fw_fs_open_read,
                                     io->file,
                                     storage->file_workspace,
                                     storage->file_workspace_bytes);
  io->owned             = (error == k_ra8_ok);
  return error;
}

/**
 * @brief Publish and validate one USTAR fixture. @details Injects a chunk size that splits records.
 * @param[in] length Published extent. @param[in] io_bytes Injected chunk extent. @param[out] report Candidate report.
 * @return Publication, open, validation or close status. @retval k_ra8_ok The archive was accepted.
 * @pre Test storage is initialized. @pre @p io_bytes does not exceed the injected scratch.
 * @post The handle is closed whenever it opened. @post A rejection leaves @p report untouched.
 * @note Test-only and serial. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_run_tar(size_t length, uint32_t io_bytes, mdl_verify_report_t* report)
{
  mdl_storage_t storage   = *mdl_test_storage_get();
  storage.io_buffer       = s_io;
  storage.io_buffer_bytes = io_bytes;
  mdl_verify_io_t io      = {};
  ra8_err_t       error   = internal_publish(s_stream_path, s_tar, (uint32_t)length);
  if (error == k_ra8_ok) {
    error = internal_io_open_fixture(&storage, &io, length);
  }
  if (error == k_ra8_ok) {
    const ra8_err_t verified = priv_mdl_verify_tar(&io, report);
    const ra8_err_t closed   = fw_fs_close(io.file);
    error                    = (verified != k_ra8_ok) ? verified : closed;
  }
  return error;
}

/**
 * @brief Publish and validate one gzip fixture. @details Streams the frame through the inflater.
 * @param[in] length Published extent. @param[in] io_bytes Injected chunk extent. @param[in] size_hint Extent to honour. @param[in] arena Scratch capacity. @param[out] report Candidate report.
 * @return Publication, open, validation or close status. @retval k_ra8_ok The frame was accepted.
 * @pre Test storage is initialized. @pre @p io_bytes does not exceed the injected scratch.
 * @post The handle is closed whenever it opened. @post The shared workspace records this attempt.
 * @note Test-only and serial. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_run_gzip(size_t               length,
                                                uint32_t             io_bytes,
                                                uint64_t             size_hint,
                                                size_t               arena,
                                                mdl_verify_report_t* report)
{
  mdl_storage_t storage   = *mdl_test_storage_get();
  storage.io_buffer       = s_io;
  storage.io_buffer_bytes = io_bytes;
  mdl_verify_io_t io      = {};
  mdl_export_workspace_init(&s_gz_workspace, s_verify_arena, arena);
  ra8_err_t error = internal_publish(s_stream_path, s_gz, (uint32_t)length);
  if (error == k_ra8_ok) {
    error = internal_io_open_fixture(&storage, &io, size_hint);
  }
  if (error == k_ra8_ok) {
    const ra8_err_t verified = priv_mdl_verify_gzip_tar(&io, &s_gz_workspace, report);
    const ra8_err_t closed   = fw_fs_close(io.file);
    error                    = (verified != k_ra8_ok) ? verified : closed;
  }
  return error;
}

/**
 * @brief Wrap the built archive in a complete RFC 1952 frame.
 * @details Emits the fixed ten-byte header, one raw DEFLATE stream from the vendored compressor,
 * optional bytes after the end marker, then the exact CRC32 and ISIZE trailer.
 * @param[in] raw_len Archive extent to compress. @param[in] extra Bytes after the end marker. @param[out] out_body Compressed extent excluding @p extra.
 * @return Complete gzip frame extent. @retval 0 Never; the fixed framing is always present.
 * @pre The archive fixture holds @p raw_len bytes. @pre The frame fits the shared buffer.
 * @post The trailer describes the archive exactly. @post Every unwritten byte stays zero.
 * @note Test-only; assertion failure terminates the process. @since 0.1.0
 */
RA8_INTERNAL static size_t internal_gz_build(size_t raw_len, uint32_t extra, size_t* out_body)
{
  (void)memset(s_gz, 0, sizeof(s_gz));
  s_gz[k_fx_idx0] = (uint8_t)k_fx_gz_id1;
  s_gz[k_fx_idx1] = (uint8_t)k_fx_gz_id2;
  s_gz[k_fx_idx2] = (uint8_t)k_fx_gz_method;
  size_t in_size  = raw_len;
  size_t out_size = sizeof(s_gz) - (size_t)k_fx_gz_head - (size_t)k_fx_gz_tail - (size_t)extra;
  TEST_ASSERT_EQ(TDEFL_STATUS_OKAY,
                 tdefl_init(&s_compressor.compressor, nullptr, nullptr, (int)k_fx_gz_probes));
  TEST_ASSERT_EQ(TDEFL_STATUS_DONE,
                 tdefl_compress(&s_compressor.compressor,
                                s_tar,
                                &in_size,
                                &s_gz[k_fx_gz_head],
                                &out_size,
                                TDEFL_FINISH));
  TEST_ASSERT_EQ(raw_len, in_size);
  uint8_t* trailer = &s_gz[(size_t)k_fx_gz_head + out_size + (size_t)extra];
  internal_put_u32le(trailer, (uint32_t)mz_crc32(MZ_CRC32_INIT, s_tar, raw_len));
  internal_put_u32le(&trailer[k_fx_gz_isize], (uint32_t)raw_len);
  *out_body = out_size;
  return (size_t)k_fx_gz_head + out_size + (size_t)extra + (size_t)k_fx_gz_tail;
}

/**
 * @test internal_test_tar_shapes
 * @brief Every archive shape the streamed USTAR reader must accept or reject.
 * @details Drives complete, zero-padded, ragged, unterminated, half-terminated, metadata-free,
 * image-free, over-declared and post-terminator-data archives through the validator with a chunk
 * size that splits every record, so header assembly and payload skipping both cross feeds.
 * @pre The shared archive buffer and fixture path are exclusively owned. @pre Storage is ready.
 * @post Each accepted shape reports the exact counts. @post Each rejection leaves the report at its sentinel. @note Test-only; assertion failure terminates the process. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_tar_shapes(void)
{
  TEST_BEGIN("media cbt stream shapes");
  for (size_t i = 0U; i < (sizeof(s_tar_shapes) / sizeof(s_tar_shapes[0])); ++i) {
    const mdl_tar_shape_t* shape  = &s_tar_shapes[i];
    const size_t           length = internal_tar_build(shape);
    const ra8_err_t        want   = shape->accepted ? k_ra8_ok : k_ra8_err_validation_failed;
    mdl_verify_report_t    report = {.member_count = (size_t)k_fx_sentinel};
    const ra8_err_t        error  = internal_run_tar(length, k_fx_io_chunk, &report);
    if (error != want) {
      TEST_FAIL_FMT("%s: expected %d, got %d", shape->label, (int)want, (int)error);
    }
    if (shape->accepted) {
      TEST_ASSERT_EQ(k_fx_want_pages, report.page_count);
      TEST_ASSERT_EQ(k_fx_want_members, report.member_count);
      TEST_ASSERT(report.metadata_present);
    } else {
      TEST_ASSERT_EQ(k_fx_sentinel, report.member_count);
    }
  }
  TEST_END("media cbt stream shapes");
}

/**
 * @test internal_test_tar_header_guards
 * @brief One byte-level corruption per USTAR header guard is rejected.
 * @details Each vector starts from the archive the shape test accepts and changes exactly one
 * field: an unsupported typeflag, a stale checksum, an above-range or below-range checksum digit, a
 * blank checksum, an above-range size digit, a blank size field, and an absolute member name. A
 * corruption outside the checksum field is followed by a repaired checksum, isolating one guard.
 * @pre The shared archive buffer and fixture path are exclusively owned. @pre Storage is ready.
 * @post Every vector is rejected as a validation failure. @post Every vector leaves the report at its sentinel. @note Test-only; assertion failure terminates the process. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_tar_header_guards(void)
{
  TEST_BEGIN("media cbt header guards");
  for (size_t i = 0U; i < (sizeof(s_tar_pokes) / sizeof(s_tar_pokes[0])); ++i) {
    const mdl_tar_poke_t* poke   = &s_tar_pokes[i];
    const size_t          length = internal_tar_poked(poke);
    mdl_verify_report_t   report = {.member_count = (size_t)k_fx_sentinel};
    const ra8_err_t       error  = internal_run_tar(length, k_fx_io_chunk, &report);
    if (error != k_ra8_err_validation_failed) {
      TEST_FAIL_FMT("%s: expected rejection, got %d", poke->label, (int)error);
    }
    TEST_ASSERT_EQ(k_fx_sentinel, report.member_count);
  }
  TEST_END("media cbt header guards");
}

/**
 * @test internal_test_tar_field_encodings
 * @brief Legal but unusual octal field encodings stay acceptable.
 * @details Rewrites the accepted archive so its first record carries a NUL-padded checksum and a
 * space-terminated size, and its second carries a size filling the complete twelve-byte field with
 * no terminator at all. All three forms are produced by real tar writers and parse to the same
 * values, so the archive must still be accepted with unchanged counts.
 * @pre The shared archive buffer and fixture path are exclusively owned. @pre Row zero is complete.
 * @post The archive keeps its original counts. @post No record checksum is left inconsistent. @note Test-only; assertion failure terminates the process. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_tar_field_encodings(void)
{
  TEST_BEGIN("media cbt field encodings");
  const size_t length = internal_tar_build(&s_tar_shapes[k_fx_valid]);
  uint8_t*     second = &s_tar[k_fx_page_pad];
  internal_put_octal(&second[k_fx_size_off], (size_t)k_fx_size_len + 1U, (uint64_t)k_fx_page_len);
  internal_put_checksum(second);
  s_tar[(size_t)k_fx_size_off + (size_t)k_fx_size_len - 1U] = (uint8_t)' ';
  internal_put_checksum(s_tar);
  s_tar[k_fx_sum_off]        = (uint8_t)'\0';
  mdl_verify_report_t report = {.member_count = (size_t)k_fx_sentinel};
  TEST_ASSERT_EQ(k_ra8_ok, internal_run_tar(length, k_fx_io_chunk, &report));
  TEST_ASSERT_EQ(k_fx_want_pages, report.page_count);
  TEST_ASSERT_EQ(k_fx_want_members, report.member_count);
  TEST_ASSERT(report.metadata_present);
  TEST_END("media cbt field encodings");
}

/**
 * @test internal_test_gzip_framing
 * @brief The fixed RFC 1952 framing and its trailer accounting are both enforced.
 * @details Builds a real gzip frame over the accepted archive, proves it validates, rejects a frame
 * shorter than the fixed framing, then flips one bit at a time in each magic byte, the compression
 * method, the flag byte, the stored CRC32 and the stored ISIZE, requiring a rejection for each.
 * @pre The shared archive and frame buffers are exclusively owned. @pre Storage is ready.
 * @post The intact frame reports the archive's counts. @post Every corrupted frame is rejected. @note Test-only; assertion failure terminates the process. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gzip_framing(void)
{
  TEST_BEGIN("media cbt gzip framing");
  size_t body   = 0U;
  size_t length = internal_gz_build(internal_tar_build(&s_tar_shapes[k_fx_valid]), 0U, &body);
  mdl_verify_report_t report = {.member_count = (size_t)k_fx_sentinel};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_run_gzip(length, k_fx_io_cap, length, k_test_verify_arena_bytes, &report));
  TEST_ASSERT_EQ(k_fx_want_pages, report.page_count);
  TEST_ASSERT_EQ(k_fx_want_members, report.member_count);
  TEST_ASSERT_EQ(
    k_ra8_err_validation_failed,
    internal_run_gzip(length, k_fx_io_cap, k_fx_gz_short, k_test_verify_arena_bytes, &report));
  for (size_t i = 0U; i < (sizeof(s_gz_pokes) / sizeof(s_gz_pokes[0])); ++i) {
    const mdl_gz_poke_t* poke = &s_gz_pokes[i];
    length = internal_gz_build(internal_tar_build(&s_tar_shapes[k_fx_valid]), 0U, &body);
    const size_t offset = poke->from_end ? (length - (size_t)poke->offset) : (size_t)poke->offset;
    s_gz[offset] ^= poke->flip;
    report = (mdl_verify_report_t){.member_count = (size_t)k_fx_sentinel};
    const ra8_err_t error =
      internal_run_gzip(length, k_fx_io_cap, length, k_test_verify_arena_bytes, &report);
    if (error != k_ra8_err_validation_failed) {
      TEST_FAIL_FMT("%s: expected rejection, got %d", poke->label, (int)error);
    }
    TEST_ASSERT_EQ(k_fx_sentinel, report.member_count);
  }
  TEST_END("media cbt gzip framing");
}

/**
 * @test internal_test_gzip_stream_bounds
 * @brief Chunked inflate, stream truncation, overrun, and trailing compressed bytes.
 * @details Compresses an archive far larger than one decoded chunk so inflate must round-trip its
 * dictionary repeatedly, then reuses that fixture with a size snapshot that overstates the file (a
 * short read), understates it (an unterminated DEFLATE stream), and with bytes deliberately left
 * after the end-of-stream marker both inside and beyond the chunk that carries the marker.
 * @pre The shared archive and frame buffers are exclusively owned. @pre Storage is ready.
 * @post The intact large frame keeps its exact counts. @post Every bound violation is rejected. @note Test-only; assertion failure terminates the process. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gzip_stream_bounds(void)
{
  TEST_BEGIN("media cbt gzip stream bounds");
  const mdl_tar_shape_t* large  = &s_tar_shapes[k_fx_large];
  size_t                 body   = 0U;
  size_t                 length = internal_gz_build(internal_tar_build(large), 0U, &body);
  mdl_verify_report_t    report = {.member_count = (size_t)k_fx_sentinel};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_run_gzip(length, k_fx_io_tiny, length, k_test_verify_arena_bytes, &report));
  TEST_ASSERT_EQ(k_fx_want_pages, report.page_count);
  TEST_ASSERT_EQ(k_fx_want_members, report.member_count);
  report = (mdl_verify_report_t){.member_count = (size_t)k_fx_sentinel};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 internal_run_gzip(length,
                                   k_fx_io_tiny,
                                   length + (uint64_t)k_fx_gz_over,
                                   k_test_verify_arena_bytes,
                                   &report));
  TEST_ASSERT_EQ(k_fx_sentinel, report.member_count);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 internal_run_gzip(length,
                                   k_fx_io_cap,
                                   length - (uint64_t)k_fx_gz_cut,
                                   k_test_verify_arena_bytes,
                                   &report));
  length = internal_gz_build(internal_tar_build(large), k_fx_gz_extra, &body);
  TEST_ASSERT_EQ(
    k_ra8_err_validation_failed,
    internal_run_gzip(length, k_fx_io_cap, length, k_test_verify_arena_bytes, &report));
  TEST_ASSERT_EQ(
    k_ra8_err_validation_failed,
    internal_run_gzip(length, (uint32_t)body, length, k_test_verify_arena_bytes, &report));
  TEST_ASSERT_EQ(k_fx_sentinel, report.member_count);
  TEST_END("media cbt gzip stream bounds");
}

/**
 * @test internal_test_gzip_payload_faults
 * @brief Corrupt compressed data, an invalid inner archive, and an exhausted arena.
 * @details Compresses an archive the USTAR reader rejects so the inner failure propagates out of
 * the inflate loop, flips one byte inside the compressed body so inflate itself fails part-way, then
 * runs an intact frame against an arena too small for the decoded chunk and against one that fits
 * the chunk but not the inflater, discriminated by the high-water mark each leaves behind.
 * @pre The shared archive and frame buffers are exclusively owned. @pre Storage is ready.
 * @post Corrupt data and a bad inner archive are validation failures. @post Both capacity failures report a size error with distinct high-water marks. @note Test-only; assertion failure terminates the process. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gzip_payload_faults(void)
{
  TEST_BEGIN("media cbt gzip payload faults");
  size_t              body   = 0U;
  size_t              length = internal_gz_build(internal_tar_poked(&s_tar_pokes[0]), 0U, &body);
  mdl_verify_report_t report = {.member_count = (size_t)k_fx_sentinel};
  TEST_ASSERT_EQ(
    k_ra8_err_validation_failed,
    internal_run_gzip(length, k_fx_io_tiny, length, k_test_verify_arena_bytes, &report));
  length = internal_gz_build(internal_tar_build(&s_tar_shapes[k_fx_valid]), 0U, &body);
  s_gz[(size_t)k_fx_gz_head + (body / 2U)] ^= (uint8_t)k_fx_byte_mask;
  TEST_ASSERT_EQ(
    k_ra8_err_validation_failed,
    internal_run_gzip(length, k_fx_io_tiny, length, k_test_verify_arena_bytes, &report));
  TEST_ASSERT_EQ(k_fx_sentinel, report.member_count);
  length = internal_gz_build(internal_tar_build(&s_tar_shapes[k_fx_valid]), 0U, &body);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_run_gzip(length, k_fx_io_cap, length, k_fx_arena_tiny, &report));
  TEST_ASSERT_EQ(0U, s_gz_workspace.high_water);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_run_gzip(length, k_fx_io_cap, length, k_fx_arena_mid, &report));
  TEST_ASSERT_EQ(k_fx_chunk_len, s_gz_workspace.high_water);
  TEST_END("media cbt gzip payload faults");
}

/**
 * @test internal_test_verify_arguments
 * @brief Arena reservation limits and the validator's argument and format contracts.
 * @details Exercises every rejection the bounded arena reservation makes -- a zero request, a zero
 * alignment, a non power-of-two alignment, an alignment walking the cursor past the capacity, an
 * oversized request, and a cursor whose rounding would overflow -- then proves a reserved format is
 * refused as unsupported, a non-container format as an invalid argument, and that both public entry
 * points reject a missing pointer before touching storage.
 * @pre Storage is initialized and the fixture path is writable. @pre The scratch buffer is owned.
 * @post No rejected reservation advances the cursor. @post Every rejection carries its documented error code. @note Test-only; assertion failure terminates the process. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_verify_arguments(void)
{
  TEST_BEGIN("media verify argument guards");
  mdl_storage_t*         store = mdl_test_storage_get();
  mdl_export_workspace_t ws;
  mdl_export_workspace_init(&ws, s_io, sizeof(s_io));
  TEST_ASSERT_NULL(priv_mdl_verify_workspace_take(&ws, 0U, alignof(max_align_t)));
  TEST_ASSERT_NULL(priv_mdl_verify_workspace_take(&ws, k_fx_one, 0U));
  TEST_ASSERT_NULL(priv_mdl_verify_workspace_take(&ws, k_fx_one, k_fx_odd_align));
  TEST_ASSERT_NULL(priv_mdl_verify_workspace_take(&ws, sizeof(s_io) + 1U, k_fx_one));
  TEST_ASSERT_EQ(0U, ws.used);
  mdl_export_workspace_t edge = {.data = s_io, .cap = k_fx_edge_cap, .used = k_fx_edge_cap};
  TEST_ASSERT_NULL(priv_mdl_verify_workspace_take(&edge, k_fx_one, k_fx_big_align));
  mdl_export_workspace_t over = {.data = s_io, .cap = sizeof(s_io), .used = SIZE_MAX};
  TEST_ASSERT_NULL(priv_mdl_verify_workspace_take(&over, k_fx_one, alignof(max_align_t)));

  mdl_verify_report_t report = {};
  mdl_verify_io_t     io     = {};
  const uint32_t      length = (uint32_t)internal_tar_build(&s_tar_shapes[k_fx_valid]);
  mdl_export_workspace_init(&ws, s_verify_arena, sizeof(s_verify_arena));
  TEST_ASSERT_EQ(k_ra8_ok, internal_publish(s_stream_path, s_tar, length));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 mdl_verify_file(store, k_ra8_mdl_format_cbr, s_stream_path, &ws, &report));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 mdl_verify_file(store, k_ra8_mdl_format_loose, s_stream_path, &ws, &report));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 mdl_verify_file(store, k_ra8_mdl_format_cbt, nullptr, &ws, &report));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    mdl_verify_open_file(store, k_ra8_mdl_format_cbt, &io.owned_file, 0U, &ws, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&store->fs->names, s_stream_path));
  TEST_END("media verify argument guards");
}

/**
 * @brief Run the RBKC, streamed USTAR and gzip verifier regressions.
 * @return Zero after all assertions pass. @retval 0 Every registered vector behaved exactly.
 * @pre The root-confined portable test storage can be initialized. @pre The assertion process is active.
 * @post Test storage is deinitialized and fixture paths are absent. @post No ownership escapes the process. @note Host-only and serial. @since 0.1.0
 */
int main(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_init());
  internal_test_strict_rabook_verify();
  internal_test_rabook_writer();
  internal_test_tar_shapes();
  internal_test_tar_header_guards();
  internal_test_tar_field_encodings();
  internal_test_gzip_framing();
  internal_test_gzip_stream_bounds();
  internal_test_gzip_payload_faults();
  internal_test_verify_arguments();
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_deinit());
  return 0;
}
