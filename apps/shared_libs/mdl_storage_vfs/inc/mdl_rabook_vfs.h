/**
 * @file mdl_rabook_vfs.h
 * @brief Strict no-heap RBKC validator and reader over named RA8 VFS mounts
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Adapts the existing format-neutral ::ra8_io_vfs_file_t stream to the strict
 * ::book_chunked_t reader. The same caller-owned workspace can be used as
 * the artifact validator in ::mdl_storage_vfs_config_t and, after the
 * transaction publishes, reopened on the final path for demand-paged reading.
 * No file object is retained by validation; an open reader is retained only
 * between ::mdl_rabook_vfs_open and ::mdl_rabook_vfs_close.
 *
 * This module does not create a second filesystem facade. It only composes the
 * existing VFS and book contracts and owns no heap, static workspace, mount,
 * path, or publication policy.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "book_chunked.h"
#include "ra8_c6link_mdl_transfer.h"
#include "ra8_io_vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct mdl_rabook_vfs_config_t
 * @brief Caller-owned decompression and strict-validation workspace binding.
 *
 * @invariant All storage spans remain live and exclusively mutable while used.
 * @invariant `chunk` and `compressed` cover the largest inflated/compressed
 * chunk.
 * @invariant `table_cap` covers the RBKC chunk count plus its terminal entry.
 * @since 0.1.0
 */
typedef struct {
  book_inflate_fn inflate_cb;     /**< Heap-free RFC 1950 inflater.        */
  uint64_t*       table;          /**< Parsed RBKC offset-table workspace. */
  uint8_t*        compressed;     /**< One compressed-chunk staging span.  */
  uint8_t*        chunk;          /**< One complete inflated-chunk span.   */
  uint8_t*        scratch;        /**< Strict semantic/CRC workspace.      */
  uint32_t        table_cap;      /**< Entries available at @p table.      */
  uint32_t        compressed_cap; /**< Bytes available at @p compressed.   */
  uint32_t        chunk_cap;      /**< Bytes available at @p chunk.        */
  uint32_t        scratch_cap;    /**< Bytes available at @p scratch.      */
} mdl_rabook_vfs_config_t;

/**
 * @struct mdl_rabook_vfs_t
 * @brief One initialized strict validator or one open validated RBKC reader.
 *
 * @details Treat fields as private after ::mdl_rabook_vfs_init. Workspace
 * pointers borrow the caller's storage; the VFS owns the opaque file facade.
 *
 * @invariant Between public calls, `file != nullptr` exactly while `open` is
 * true; validation and open may hold a transient facade internally.
 * @invariant A true `open` means `reader` and `header` passed strict
 * validation.
 * @invariant No member points to dynamically allocated first-party storage.
 * @since 0.1.0
 */
typedef struct {
  book_chunked_t     reader;                         /**< Bound RBKC reader.           */
  book_header_t      header;                         /**< Strict decoded header.       */
  ra8_io_vfs_file_t* file;                           /**< Borrowed VFS facade slot.    */
  book_inflate_fn    inflate_cb;                     /**< Caller inflater.             */
  uint64_t*          table;                          /**< Caller table workspace.      */
  uint8_t*           compressed;                     /**< Caller compressed staging.   */
  uint8_t*           chunk;                          /**< Caller inflated chunk.       */
  uint8_t*           scratch;                        /**< Caller strict workspace.     */
  uint64_t           file_size;                      /**< Validated RBKC byte length.  */
  uint8_t            digest[k_ra8_mdl_sha256_bytes]; /**< Transfer digest at validate. */
  uint32_t           table_cap;                      /**< Table entry capacity.        */
  uint32_t           compressed_cap;                 /**< Compressed staging bytes.    */
  uint32_t           chunk_cap;                      /**< Inflated chunk bytes.        */
  uint32_t           scratch_cap;                    /**< Strict workspace bytes.      */
  bool               open;                           /**< Live final-file reader.      */
  bool               transfer_validated;             /**< Stage validation succeeded.  */
} mdl_rabook_vfs_t;

/**
 * @brief Initialize one strict RBKC VFS workspace binding.
 * @param[out] ctx Caller-owned context.
 * @param[in] config Complete inflater and workspace descriptor.
 * @return Initialization status.
 * @retval k_ra8_ok The context is ready for validation or open.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_size A workspace capacity is zero or table has
 * fewer than two entries.
 * @pre No reader is open in @p ctx.
 * @post Success leaves @p ctx closed with no retained VFS facade.
 * @post Failure leaves @p ctx unchanged.
 * @note Does not touch the VFS or inspect workspace contents.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_rabook_vfs_init(mdl_rabook_vfs_t*              ctx,
                                            const mdl_rabook_vfs_config_t* config);

/**
 * @brief Strictly validate one closed private RBKC staging object.
 * @details Opens @p staging_path read-only, proves its exact size, validates
 * every RBKC stream and the complete canonical RABOOK1 payload, then closes it.
 * This signature directly satisfies ::mdl_storage_vfs_validate_fn.
 * @param[in,out] opaque Initialized ::mdl_rabook_vfs_t.
 * @param[in] staging_path Named VFS path to the private closed artifact.
 * @param[in] total_bytes Independently verified transfer byte count.
 * @param[in] sha256 Independently verified transfer digest.
 * @return Strict validation, VFS, inflater, or close status.
 * @pre The staging path remains immutable for the call.
 * @pre No reader is open in @p opaque.
 * @post Success retains only decoded metadata and the digest, never a file
 * handle.
 * @post Failure leaves the context closed and not transfer-validated.
 * @note This function neither commits nor deletes the staging object.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_rabook_vfs_validate(void*         opaque,
                                                const char*   staging_path,
                                                uint64_t      total_bytes,
                                                const uint8_t sha256[k_ra8_mdl_sha256_bytes]);

/**
 * @brief Open and strictly revalidate a published RBKC object for consumption.
 * @param[in,out] ctx Initialized closed context.
 * @param[in] path Named VFS path of the published `.rabook`.
 * @return Strict validation, VFS, or inflater status.
 * @retval k_ra8_err_invalid_state A reader is already open.
 * @pre The selected mount remains registered until close.
 * @post Success retains one read-only VFS file and a strictly validated reader.
 * @post Failure retains no file facade and publishes no reader metadata.
 * @note Revalidation prevents a stage-time verdict from authorizing changed
 * bytes.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_rabook_vfs_open(mdl_rabook_vfs_t* ctx, const char* path);

/**
 * @brief Read one exact chunk-aligned flat-blob span from an open book.
 * @param[in,out] ctx Open validated context.
 * @param[in] offset Chunk-aligned inflated-blob offset.
 * @param[out] dst Destination for exactly @p len bytes.
 * @param[in] len Exact inflated span for the selected chunk.
 * @return ::book_chunked_read status.
 * @pre ::mdl_rabook_vfs_open succeeded and the context remains exclusive.
 * @post Success initializes exactly @p len bytes in @p dst.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mdl_rabook_vfs_read_chunk(mdl_rabook_vfs_t* ctx, uint64_t offset, uint8_t* dst, uint32_t len);

/**
 * @brief Copy metadata from an open strictly validated book.
 * @param[in] ctx Open validated context.
 * @param[out] out_header Receives the decoded flat RABOOK1 header.
 * @param[out] out_flat_size Receives the inflated flat-blob length.
 * @return Query status.
 * @retval k_ra8_ok Both outputs were initialized.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_state No validated reader is open.
 * @pre No concurrent operation mutates @p ctx.
 * @post The open reader and file position are unchanged.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_rabook_vfs_info(const mdl_rabook_vfs_t* ctx,
                                            book_header_t*          out_header,
                                            uint64_t*               out_flat_size);

/**
 * @brief Close and release an open VFS reader facade.
 * @param[in,out] ctx Initialized context.
 * @return VFS close status, or success when already closed.
 * @pre No concurrent read uses @p ctx.
 * @post The context retains no file handle even when close reports an error.
 * @note Does not unmount storage or modify the published file.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_rabook_vfs_close(mdl_rabook_vfs_t* ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif
