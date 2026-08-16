/**
 * @file test_ra8_c6link_rabook.c
 * @brief Mixed-image C6 RPC to strict VFS `.rabook` production/consumption test
 *
 * @details Produces a real RABOOK1 book and RBKC container with the firmware
 * compiler, serves it through the independently decoded modelled C6 service,
 * transfers it through the RA client with a real SHA-256 stream, stages it on
 * a real FAT/VFS volume, strictly validates before publication, then reopens
 * and consumes the published book. Corrupted transport bytes and a coherently
 * hashed invalid container both prove fail-closed cleanup.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_book.h"
#include "ra8_c6_model.h"
#include "ra8_c6link_mdl_transfer.h"
#include "ra8_c6link_model_test_internal.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_compress.h"
#include "ra8_io_vfs.h"
#include "ra8_log.h"
#include "ra8_mdl_rabook_vfs.h"
#include "ra8_mdl_storage_vfs.h"
#include "ra8_rabook_container.h"
#include "ra8_rsip.h"
#include "support/rabook_compile_test_fixture.h"
#include "unity_minimal.h"

/** @brief Exact bounded fixture capacities. */
typedef enum : uint32_t {
  k_internal_disk_blocks        = 16384U, /**< 8 MiB FAT16 RAM medium.         */
  k_internal_rbkc_cap           = 8192U,  /**< Generated container bytes.      */
  k_internal_rbkc_chunk_bytes   = 2048U,  /**< One-chunk fixture geometry.     */
  k_internal_rbkc_work_bytes    = 4096U,  /**< Chunk/compressed workspaces.    */
  k_internal_rbkc_table_entries = 32U,    /**< Container offset-table cap.     */
  k_internal_rpc_chunk_bytes    = 777U,   /**< Non-power-of-two transfer pull. */
  k_internal_rpc_max_chunks     = 64U,    /**< Absolute coordinator bound.     */
} internal_rabook_limit_t;

/** @brief Compressor storage with portable maximum alignment. */
typedef union {
  max_align_t align;                                  /**< Alignment forcing member. */
  uint8_t     bytes[k_ra8_io_compress_scratch_bytes]; /**< tdefl workspace.          */
} internal_compressor_t;

/** @brief Memory callbacks used only while generating the RBKC source. */
typedef struct {
  const uint8_t* flat;     /**< Finalized RABOOK1 bytes.   */
  uint32_t       flat_len; /**< Exact flat source length.  */
  uint8_t*       rbkc;     /**< RBKC destination bytes.    */
  uint32_t       rbkc_cap; /**< RBKC destination capacity. */
} internal_container_io_t;

static uint8_t s_disk[(size_t)k_internal_disk_blocks * k_ra8_io_block_size_bytes];
static ra8_io_blockdev_ram_state_t s_ram_state;
static ra8_io_blockdev_t           s_blockdev;
static ra8_fs_backend_t            s_backend;
static ra8_fs_mount_t*             s_mount;

static ra8_test_rabook_fixture_t   s_compile_fixture;
static ra8_test_rabook_roundtrip_t s_roundtrip;
static uint8_t                     s_rbkc[k_internal_rbkc_cap];
static uint8_t                     s_invalid_rbkc[k_internal_rbkc_cap];
static uint8_t                     s_container_input[k_internal_rbkc_work_bytes];
static uint8_t                     s_container_compressed[k_internal_rbkc_work_bytes];
static uint64_t                    s_container_offsets[k_internal_rbkc_table_entries];
static internal_compressor_t       s_compressor;
static uint64_t                    s_rbkc_len;
static uint8_t                     s_rbkc_digest[k_ra8_mdl_sha256_bytes];

static uint64_t              s_reader_table[k_internal_rbkc_table_entries];
static uint8_t               s_reader_compressed[k_internal_rbkc_work_bytes];
static uint8_t               s_reader_chunk[k_internal_rbkc_work_bytes];
static uint8_t               s_reader_scratch[k_internal_rbkc_work_bytes];
static uint8_t               s_consumed_flat[k_ra8_test_rabook_out_cap];
static ra8_rsip_sha256_ctx_t s_transfer_sha;

/** @brief Discard expected negative-path logs without host ITM access. @details
 * Implements the discard log fixture operation used only by this focused test
 * executable. @param[in,out] ctx Fixture argument governed by the exercised
 * interface contract. @param[in] byte Fixture argument governed by the
 * exercised interface contract. @pre Fixed-capacity fixture storage required by
 * this operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_discard_log(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/** @brief Read an exact range from the finalized flat compiler output. @details
 * Implements the flat read fixture operation used only by this focused test
 * executable. @param[in,out] opaque Fixture argument governed by the exercised
 * interface contract. @param[in] offset Fixture argument governed by the
 * exercised interface contract. @param[out] dst Fixture argument governed by
 * the exercised interface contract. @param[in] requested Fixture argument
 * governed by the exercised interface contract. @param[out] out_read Fixture
 * argument governed by the exercised interface contract. @return RA8 status
 * from the exercised fixture operation. @retval k_ra8_ok The fixture operation
 * completed successfully. @pre Fixed-capacity fixture storage required by this
 * operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_flat_read(void*     opaque,
                                    uint32_t  offset,
                                    uint8_t*  dst,
                                    uint32_t  requested,
                                    uint32_t* out_read)
{
  internal_container_io_t* const io = opaque;
  *out_read                         = 0U;
  if ((offset > io->flat_len) || (requested > (io->flat_len - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(dst, &io->flat[offset], requested);
  *out_read = requested;
  return k_ra8_ok;
}

/** @brief Random-write one exact generated RBKC range into bounded RAM.
 * @details Implements the rbkc write fixture operation used only by this
 * focused test executable. @param[in,out] opaque Fixture argument governed by
 * the exercised interface contract. @param[in] offset Fixture argument governed
 * by the exercised interface contract. @param[in] src Fixture argument governed
 * by the exercised interface contract. @param[in] requested Fixture argument
 * governed by the exercised interface contract. @param[out] out_written Fixture
 * argument governed by the exercised interface contract. @return RA8 status
 * from the exercised fixture operation. @retval k_ra8_ok The fixture operation
 * completed successfully. @pre Fixed-capacity fixture storage required by this
 * operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_rbkc_write(void*          opaque,
                                     uint64_t       offset,
                                     const uint8_t* src,
                                     uint32_t       requested,
                                     uint32_t*      out_written)
{
  internal_container_io_t* const io = opaque;
  *out_written                      = 0U;
  if ((offset > io->rbkc_cap) || ((uint64_t)requested > ((uint64_t)io->rbkc_cap - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(&io->rbkc[offset], src, requested);
  *out_written = requested;
  return k_ra8_ok;
}

/** @brief Heap-free RFC 1950 inflater matching the book reader callback.
 * @details Implements the inflate fixture operation used only by this focused
 * test executable. @param[in] src Fixture argument governed by the exercised
 * interface contract. @param[in] src_len Fixture argument governed by the
 * exercised interface contract. @param[out] dst Fixture argument governed by
 * the exercised interface contract. @param[in] dst_cap Fixture argument
 * governed by the exercised interface contract. @param[out] out_len Fixture
 * argument governed by the exercised interface contract. @return RA8 status
 * from the exercised fixture operation. @retval k_ra8_ok The fixture operation
 * completed successfully. @pre Fixed-capacity fixture storage required by this
 * operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t
internal_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
{
  if ((src == nullptr) || (dst == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const size_t result = tinfl_decompress_mem_to_mem(
    dst,
    dst_cap,
    src,
    src_len,
    (uint32_t)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  if (result == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
    return k_ra8_err_validation_failed;
  }
  *out_len = result;
  return k_ra8_ok;
}

/** @brief Initialize the real software SHA-256 stream. @details Implements the
 * sha init fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @return RA8 status from the exercised fixture operation. @retval
 * k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity
 * fixture storage required by this operation is available. @pre Arguments
 * follow the interface contract exercised by this helper. @post Documented
 * outputs contain the exercised result when the operation succeeds. @post
 * Mutations remain confined to documented outputs and file-local fixture state.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_sha_init(void* ctx)
{
  return ra8_rsip_sha256_init((ra8_rsip_sha256_ctx_t*)ctx);
}

/** @brief Feed one RPC chunk to the real software SHA-256 stream. @details
 * Implements the sha update fixture operation used only by this focused test
 * executable. @param[in,out] ctx Fixture argument governed by the exercised
 * interface contract. @param[in] data Fixture argument governed by the
 * exercised interface contract. @param[in] len Fixture argument governed by the
 * exercised interface contract. @return RA8 status from the exercised fixture
 * operation. @retval k_ra8_ok The fixture operation completed successfully.
 * @pre Fixed-capacity fixture storage required by this operation is available.
 * @pre Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_sha_update(void* ctx, const uint8_t* data, uint16_t len)
{
  return ra8_rsip_sha256_update((ra8_rsip_sha256_ctx_t*)ctx, data, len);
}

/** @brief Finalize the real software SHA-256 stream. @details Implements the
 * sha final fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @param[in] out Fixture argument governed by the exercised interface
 * contract. @return RA8 status from the exercised fixture operation. @retval
 * k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity
 * fixture storage required by this operation is available. @pre Arguments
 * follow the interface contract exercised by this helper. @post Documented
 * outputs contain the exercised result when the operation succeeds. @post
 * Mutations remain confined to documented outputs and file-local fixture state.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_sha_final(void* ctx, uint8_t out[k_ra8_mdl_sha256_bytes])
{
  return ra8_rsip_sha256_final((ra8_rsip_sha256_ctx_t*)ctx, out);
}

/** @brief Format and mount one fresh real FAT16 volume. @details Implements the
 * setup volume fixture operation used only by this focused test executable.
 * @pre Fixed-capacity fixture storage required by this operation is available.
 * @pre Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_setup_volume(void)
{
  if (s_mount != nullptr) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_mount));
    s_mount = nullptr;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_ram_init(&s_blockdev, &s_ram_state, s_disk, k_internal_disk_blocks, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&s_blockdev, &s_backend));
  const ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "C6RBKC"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &s_mount));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("ram", s_mount));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("ram:/BOOKS"));
}

/** @brief Assert one named VFS path is absent. @details Implements the assert
 * absent fixture operation used only by this focused test executable.
 * @param[in] path Fixture argument governed by the exercised interface
 * contract. @pre Fixed-capacity fixture storage required by this operation is
 * available. @pre Arguments follow the interface contract exercised by this
 * helper. @post Documented outputs contain the exercised result when the
 * operation succeeds. @post Mutations remain confined to documented outputs and
 * file-local fixture state. @note File-local helper; no ownership escapes this
 * focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_assert_absent(const char* path)
{
  ra8_io_vfs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat(path, &stat));
  TEST_ASSERT(!stat.exists);
}

/** @brief Produce a real strict RBKC file and its real SHA-256 digest. @details
 * Implements the generate rabook fixture operation used only by this focused
 * test executable. @pre Fixed-capacity fixture storage required by this
 * operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_generate_rabook(void)
{
  s_compile_fixture        = (ra8_test_rabook_fixture_t){};
  s_roundtrip              = (ra8_test_rabook_roundtrip_t){};
  ra8_rabook_ctx_t builder = {};
  ra8_test_rabook_init(&s_compile_fixture, &builder);
  ra8_test_rabook_populate(&s_compile_fixture, &builder, &s_roundtrip, false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_finalize(&builder, &s_roundtrip.blob, &s_roundtrip.blob_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_roundtrip.blob, s_roundtrip.blob_len));

  internal_container_io_t          io        = {.flat     = s_roundtrip.blob,
                                                .flat_len = s_roundtrip.blob_len,
                                                .rbkc     = s_rbkc,
                                                .rbkc_cap = sizeof(s_rbkc)};
  ra8_rabook_container_workspace_t workspace = {
    .input          = s_container_input,
    .compressed     = s_container_compressed,
    .compressor     = s_compressor.bytes,
    .offsets        = s_container_offsets,
    .input_cap      = sizeof(s_container_input),
    .compressed_cap = sizeof(s_container_compressed),
    .compressor_cap = sizeof(s_compressor.bytes),
    .offset_cap     = k_internal_rbkc_table_entries,
  };
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_container_write(internal_flat_read,
                                            &io,
                                            s_roundtrip.blob_len,
                                            k_internal_rbkc_chunk_bytes,
                                            internal_rbkc_write,
                                            &io,
                                            &workspace,
                                            &s_rbkc_len));
  TEST_ASSERT(s_rbkc_len <= sizeof(s_rbkc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256(s_rbkc, (uint32_t)s_rbkc_len, s_rbkc_digest));
}

/** @brief Bind validator, VFS transaction, SHA stream, and transfer policy.
 * @details Implements the bind pipeline fixture operation used only by this
 * focused test executable. @param[in,out] rabook Fixture argument governed by
 * the exercised interface contract. @param[in,out] storage Fixture argument
 * governed by the exercised interface contract. @param[in,out] transfer Fixture
 * argument governed by the exercised interface contract. @pre Fixed-capacity
 * fixture storage required by this operation is available. @pre Arguments
 * follow the interface contract exercised by this helper. @post Documented
 * outputs contain the exercised result when the operation succeeds. @post
 * Mutations remain confined to documented outputs and file-local fixture state.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0 */
RA8_INTERNAL
static void internal_bind_pipeline(ra8_mdl_rabook_vfs_t*      rabook,
                                   ra8_mdl_storage_vfs_t*     storage,
                                   ra8_mdl_transfer_config_t* transfer)
{
  const ra8_mdl_rabook_vfs_config_t rabook_config = {
    .inflate        = internal_inflate,
    .table          = s_reader_table,
    .compressed     = s_reader_compressed,
    .chunk          = s_reader_chunk,
    .scratch        = s_reader_scratch,
    .table_cap      = k_internal_rbkc_table_entries,
    .compressed_cap = sizeof(s_reader_compressed),
    .chunk_cap      = sizeof(s_reader_chunk),
    .scratch_cap    = sizeof(s_reader_scratch),
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_rabook_vfs_init(rabook, &rabook_config));
  const ra8_mdl_storage_vfs_config_t storage_config = {
    .stage_leaf   = "C6STAGE.TMP",
    .validate     = ra8_mdl_rabook_vfs_validate,
    .validate_ctx = rabook,
  };
  ra8_mdl_storage_iface_t storage_iface = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_storage_vfs_init(storage, &storage_config, &storage_iface));
  *transfer = (ra8_mdl_transfer_config_t){
    .storage     = storage_iface,
    .sha256      = {.init   = internal_sha_init,
                    .update = internal_sha_update,
                    .final  = internal_sha_final,
                    .ctx    = &s_transfer_sha},
    .format      = k_ra8_mdl_format_rabook,
    .chunk_bytes = k_internal_rpc_chunk_bytes,
    .max_chunks  = k_internal_rpc_max_chunks,
  };
}

/** @brief Run one mixed-image transfer with selected source and wire fault.
 * @details Implements the transfer fixture operation used only by this focused
 * test executable. @param[in] source Fixture argument governed by the exercised
 * interface contract. @param[in] source_len Fixture argument governed by the
 * exercised interface contract. @param[in] digest Fixture argument governed by
 * the exercised interface contract. @param[in] fault Fixture argument governed
 * by the exercised interface contract. @param[out] destination Fixture argument
 * governed by the exercised interface contract. @param[in,out] rabook Fixture
 * argument governed by the exercised interface contract. @param[in,out] storage
 * Fixture argument governed by the exercised interface contract. @param[out] result
 * Fixture argument governed by the exercised interface contract. @return
 * RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture
 * operation completed successfully. @pre Fixed-capacity fixture storage
 * required by this operation is available. @pre Arguments follow the interface
 * contract exercised by this helper. @post Documented outputs contain the
 * exercised result when the operation succeeds. @post Mutations remain confined
 * to documented outputs and file-local fixture state. @note File-local helper;
 * no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_transfer(const uint8_t*             source,
                                   uint32_t                   source_len,
                                   const uint8_t              digest[k_ra8_mdl_sha256_bytes],
                                   ra8_c6_model_mdl_fault_t   fault,
                                   const char*                destination,
                                   ra8_mdl_rabook_vfs_t*      rabook,
                                   ra8_mdl_storage_vfs_t*     storage,
                                   ra8_mdl_transfer_result_t* result)
{
  priv_c6link_test_bringup();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6_model_mdl_source(source, source_len, digest));
  ra8_c6_model()->mdl_fault        = fault;
  ra8_mdl_transfer_config_t config = {};
  internal_bind_pipeline(rabook, storage, &config);
  return ra8_c6link_mdl_transfer(priv_c6link_test_link(),
                                 "https://example.test/book",
                                 destination,
                                 &config,
                                 result);
}

/**
 * @brief Publish, reopen, and consume the generated `.rabook` end to end.
 * @par MC/DC:
 * Supplies all-success vectors through RPC ordering, exact storage writes,
 * digest equality, optional validator dispatch, and strict RBKC validation.
 * @details Executes the publish and consume scenario with bounded fixture state
 * and asserts the contract-specific result. @pre Fixed-capacity fixture storage
 * required by this operation is available. @pre Arguments follow the interface
 * contract exercised by this helper. @post Documented outputs contain the
 * exercised result when the operation succeeds. @post Mutations remain confined
 * to documented outputs and file-local fixture state. @note File-local helper;
 * no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_publish_and_consume(void)
{
  TEST_BEGIN("C6 RPC strict rabook publish and consume");
  internal_setup_volume();
  ra8_mdl_rabook_vfs_t      rabook  = {};
  ra8_mdl_storage_vfs_t     storage = {};
  ra8_mdl_transfer_result_t result  = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_transfer(s_rbkc,
                                   (uint32_t)s_rbkc_len,
                                   s_rbkc_digest,
                                   k_c6m_mdl_fault_none,
                                   "ram:/BOOKS/C6BOOK.RBK",
                                   &rabook,
                                   &storage,
                                   &result));
  TEST_ASSERT_EQ(s_rbkc_len, result.bytes_stored);
  TEST_ASSERT_EQ(k_ra8_mdl_format_rabook, result.format);
  TEST_ASSERT(memcmp(result.sha256, s_rbkc_digest, sizeof(s_rbkc_digest)) == 0);
  TEST_ASSERT(rabook.transfer_validated);
  TEST_ASSERT(memcmp(rabook.digest, s_rbkc_digest, sizeof(s_rbkc_digest)) == 0);
  internal_assert_absent("ram:/BOOKS/C6STAGE.TMP");

  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_mdl_rabook_vfs_validate(&rabook, "ram:/BOOKS/C6BOOK.RBK", 0U, s_rbkc_digest));
  TEST_ASSERT(!rabook.transfer_validated);
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_mdl_rabook_vfs_validate(&rabook, "ram:/BOOKS/C6BOOK.RBK", s_rbkc_len + 1U, s_rbkc_digest));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_rabook_vfs_open(&rabook, "ram:/BOOKS/C6BOOK.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_mdl_rabook_vfs_open(&rabook, "ram:/BOOKS/C6BOOK.RBK"));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_state,
    ra8_mdl_rabook_vfs_validate(&rabook, "ram:/BOOKS/C6BOOK.RBK", s_rbkc_len, s_rbkc_digest));
  ra8_book_header_t header    = {};
  uint64_t          flat_size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_rabook_vfs_info(&rabook, &header, &flat_size));
  TEST_ASSERT_EQ(s_roundtrip.blob_len, flat_size);
  TEST_ASSERT_EQ(1U, header.chapter_count);
  TEST_ASSERT(flat_size <= sizeof(s_consumed_flat));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_rabook_vfs_read_chunk(&rabook, 0U, s_consumed_flat, (uint32_t)flat_size));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_consumed_flat, (size_t)flat_size));
  ra8_test_rabook_roundtrip_t consumed = s_roundtrip;
  consumed.blob                        = s_consumed_flat;
  consumed.blob_len                    = (uint32_t)flat_size;
  ra8_test_rabook_verify(&consumed);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_rabook_vfs_close(&rabook));
  TEST_END("C6 RPC strict rabook publish and consume");
}

/**
 * @brief Corrupt one data response after C6 hashing and reject at RA SHA
 * equality.
 * @par MC/DC:
 * Holds the source, terminal digest, and storage backend constant while only
 * the mixed-image response mutation changes; digest comparison alone selects
 * failure and transactional abort. @details Executes the transport corruption
 * aborts scenario with bounded fixture state and asserts the contract-specific
 * result. @pre Fixed-capacity fixture storage required by this operation is
 * available. @pre Arguments follow the interface contract exercised by this
 * helper. @post Documented outputs contain the exercised result when the
 * operation succeeds. @post Mutations remain confined to documented outputs and
 * file-local fixture state. @note File-local helper; no ownership escapes this
 * focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_transport_corruption_aborts(void)
{
  TEST_BEGIN("C6 RPC rabook transport corruption aborts");
  internal_setup_volume();
  ra8_mdl_rabook_vfs_t      rabook  = {};
  ra8_mdl_storage_vfs_t     storage = {};
  ra8_mdl_transfer_result_t result  = {};
  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch,
                 internal_transfer(s_rbkc,
                                   (uint32_t)s_rbkc_len,
                                   s_rbkc_digest,
                                   k_c6m_mdl_fault_corrupt_data,
                                   "ram:/BOOKS/CORRUPT.RBK",
                                   &rabook,
                                   &storage,
                                   &result));
  TEST_ASSERT(!rabook.transfer_validated);
  internal_assert_absent("ram:/BOOKS/CORRUPT.RBK");
  internal_assert_absent("ram:/BOOKS/C6STAGE.TMP");
  TEST_END("C6 RPC rabook transport corruption aborts");
}

/**
 * @brief Reject an invalid RBKC even when transport byte count and SHA agree.
 * @par MC/DC:
 * Digest equality remains true while only the first RBKC magic byte differs,
 * proving artifact validation independently prevents commit. @details Executes
 * the invalid artifact aborts scenario with bounded fixture state and asserts
 * the contract-specific result. @pre Fixed-capacity fixture storage required by
 * this operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_invalid_artifact_aborts(void)
{
  TEST_BEGIN("C6 RPC coherently hashed invalid rabook aborts");
  internal_setup_volume();
  (void)memcpy(s_invalid_rbkc, s_rbkc, (size_t)s_rbkc_len);
  s_invalid_rbkc[0] ^= 1U;
  uint8_t invalid_digest[k_ra8_mdl_sha256_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256(s_invalid_rbkc, (uint32_t)s_rbkc_len, invalid_digest));
  ra8_mdl_rabook_vfs_t      rabook  = {};
  ra8_mdl_storage_vfs_t     storage = {};
  ra8_mdl_transfer_result_t result  = {};
  TEST_ASSERT(internal_transfer(s_invalid_rbkc,
                                (uint32_t)s_rbkc_len,
                                invalid_digest,
                                k_c6m_mdl_fault_none,
                                "ram:/BOOKS/INVALID.RBK",
                                &rabook,
                                &storage,
                                &result) != k_ra8_ok);
  TEST_ASSERT(!rabook.transfer_validated);
  internal_assert_absent("ram:/BOOKS/INVALID.RBK");
  internal_assert_absent("ram:/BOOKS/C6STAGE.TMP");
  TEST_END("C6 RPC coherently hashed invalid rabook aborts");
}

/** @brief Pin the real SHA-256 adapter to the standard `abc` known answer.
 * @details Executes the sha256 known answer scenario with bounded fixture state
 * and asserts the contract-specific result. @pre Fixed-capacity fixture storage
 * required by this operation is available. @pre Arguments follow the interface
 * contract exercised by this helper. @post Documented outputs contain the
 * exercised result when the operation succeeds. @post Mutations remain confined
 * to documented outputs and file-local fixture state. @note File-local helper;
 * no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_sha256_known_answer(void)
{
  TEST_BEGIN("C6 RPC transfer SHA256 known answer");
  static const uint8_t local_expected[k_ra8_mdl_sha256_bytes] = {
    0xBAU, 0x78U, 0x16U, 0xBFU, 0x8FU, 0x01U, 0xCFU, 0xEAU, 0x41U, 0x41U, 0x40U,
    0xDEU, 0x5DU, 0xAEU, 0x22U, 0x23U, 0xB0U, 0x03U, 0x61U, 0xA3U, 0x96U, 0x17U,
    0x7AU, 0x9CU, 0xB4U, 0x10U, 0xFFU, 0x61U, 0xF2U, 0x00U, 0x15U, 0xADU,
  };
  uint8_t digest[k_ra8_mdl_sha256_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256((const uint8_t*)"abc", 3U, digest));
  TEST_ASSERT(memcmp(digest, local_expected, sizeof(digest)) == 0);
  TEST_END("C6 RPC transfer SHA256 known answer");
}

int main(void)
{
  ra8_log_set_byte_sink(internal_discard_log, nullptr);
  internal_test_sha256_known_answer();
  internal_generate_rabook();
  internal_test_publish_and_consume();
  internal_test_transport_corruption_aborts();
  internal_test_invalid_artifact_aborts();
  return 0;
}
