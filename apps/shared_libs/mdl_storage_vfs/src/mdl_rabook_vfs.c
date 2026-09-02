/**
 * @file mdl_rabook_vfs.c
 * @brief Strict no-heap RBKC validation and reading through the RA8 VFS
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details Opens staged or published RBKC data through the named VFS, performs
 * strict streamed validation, and exposes bounded chunk reads to callers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "mdl_rabook_vfs.h"

#include <stdint.h>
#include <string.h>

#include "book_chunked.h"
#include "ra8_attributes.h"
#include "ra8_io_vfs.h"

/**
 * @brief Read one exact random range through an already-open VFS stream.
 * @details Seeks once, then converts permitted VFS short reads into an exact
 * callback contract while rejecting successful zero progress and over-report.
 * @param[in,out] opaque Bound ::mdl_rabook_vfs_t.
 * @param[in] offset Absolute RBKC file offset.
 * @param[out] dst Destination for exactly @p len bytes.
 * @param[in] len Exact byte count.
 * @return VFS or exact-read status.
 * @retval k_ra8_ok The complete range was read.
 * @retval k_ra8_err_invalid_state The VFS stream is not open.
 * @retval k_ra8_err_invalid_size A successful read made zero progress.
 * @pre Public validation established every pointer and range.
 * @pre The file remains immutable and open for the callback duration.
 * @post Success initializes all destination bytes.
 * @post Failure never reports a complete range.
 * @note Not thread-safe through one context.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_rabook_read_exact(void* opaque, uint64_t offset, uint8_t* dst, uint32_t len)
{
  mdl_rabook_vfs_t* const ctx = opaque;
  if (ctx == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (ctx->file == nullptr) {
    return k_ra8_err_invalid_state;
  }
  ra8_err_t err  = ra8_io_vfs_file_seek(ctx->file, offset);
  uint32_t  done = 0U;
  RA8_LOOP_BOUND(UINT32_MAX);
  while (done < len) {
    if (err != k_ra8_ok) {
      break;
    }
    uint32_t got = 0U;
    err          = ra8_io_vfs_file_read(ctx->file, &dst[done], len - done, &got);
    if (err == k_ra8_ok) {
      if (got == 0U) {
        err = k_ra8_err_invalid_size;
        continue;
      }
    }
    if (got > (len - done)) {
      err = k_ra8_err_protocol_error;
    } else {
      done += got;
    }
  }
  return err;
}

/**
 * @brief Clear all state derived from an opened artifact.
 * @details Resets only state authorized by an open operation; initialization
 * bindings and transfer-validation evidence deliberately remain available.
 * @param[in,out] ctx Initialized context with no live file facade.
 * @pre @p ctx is non-null and `file` was already released.
 * @pre No callback or reader concurrently observes @p ctx.
 * @post Reader geometry, metadata, size, and `open` are reset.
 * @post Caller workspace bindings and transfer-validation evidence remain.
 * @note Not thread-safe through one context.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_rabook_clear_open(mdl_rabook_vfs_t* ctx)
{
  ctx->reader    = (book_chunked_t){};
  ctx->header    = (book_header_t){};
  ctx->file      = nullptr;
  ctx->file_size = 0U;
  ctx->open      = false;
}

/**
 * @brief Close a possibly live facade and clear open-derived state.
 * @details Attempts the VFS close when needed, then clears open state on every
 * result so a reported close failure cannot leave a usable reader facade.
 * @param[in,out] ctx Initialized context.
 * @return VFS close status or success when already closed.
 * @retval k_ra8_ok No facade was live or the facade closed successfully.
 * @pre No concurrent reader uses @p ctx.
 * @pre @p ctx is non-null and was initialized by the public initializer.
 * @post `file` is null and `open` false on every path.
 * @post The VFS facade slot is released according to its close contract.
 * @note Not thread-safe through one context.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rabook_close(mdl_rabook_vfs_t* ctx)
{
  ra8_err_t err = k_ra8_ok;
  if (ctx->file != nullptr) {
    err = ra8_io_vfs_file_close(ctx->file);
  }
  internal_rabook_clear_open(ctx);
  return err;
}

/**
 * @brief Open and parse one exact-size RBKC container without strict payload
 * validation.
 * @details Opens through the existing VFS, enforces the requested size policy,
 * and binds RBKC geometry to the retained read-only facade.
 * @param[in,out] ctx Initialized closed context.
 * @param[in] path Named VFS artifact path.
 * @param[in] expected_size Required container length when @p enforce_size is
 * true.
 * @param[in] enforce_size Whether the observed length must match exactly.
 * @return Open, size, or RBKC-geometry status.
 * @retval k_ra8_ok The exact-size policy and outer RBKC geometry passed.
 * @pre Required workspace bindings were validated at initialization.
 * @pre @p ctx has no live file facade.
 * @post Success binds `reader` and retains one open file.
 * @post Failure releases any acquired facade and clears open-derived state.
 * @note Strict inner validation is deliberately a separate ordered step.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rabook_open_reader(mdl_rabook_vfs_t* ctx,
                                             const char*       path,
                                             uint64_t          expected_size,
                                             bool              enforce_size)
{
  ra8_err_t err = ra8_io_vfs_file_open(path, k_ra8_fs_mode_read, &ctx->file);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_io_vfs_file_size(ctx->file, &ctx->file_size);
  if (err == k_ra8_ok) {
    if (enforce_size) {
      if (ctx->file_size != expected_size) {
        err = k_ra8_err_invalid_size;
      }
    }
  }
  if (err == k_ra8_ok) {
    err = book_chunked_open(&ctx->reader,
                            internal_rabook_read_exact,
                            ctx,
                            ctx->file_size,
                            ctx->inflate_cb,
                            ctx->table,
                            ctx->table_cap,
                            ctx->compressed,
                            ctx->compressed_cap);
  }
  if (err != k_ra8_ok) {
    const ra8_err_t close_err = internal_rabook_close(ctx);
    (void)close_err;
  }
  return err;
}

/**
 * @brief Strictly validate the complete inner flat blob of an open reader.
 * @details Delegates to the existing strict streamed validator with the same
 * caller-owned chunk and semantic scratch workspaces bound at initialization.
 * @param[in,out] ctx Open parsed RBKC context.
 * @param[out] out_header Separate output storage for decoded metadata.
 * @return Strict container/payload validation status.
 * @retval k_ra8_ok Every chunk and the canonical inner RABOOK1 payload passed.
 * @pre Reader, callback, and workspaces are live and mutually non-overlapping.
 * @pre @p out_header does not alias the context or any workspace.
 * @post Success initializes @p out_header completely.
 * @post Failure zeroes @p out_header according to the strict validator
 * contract.
 * @note Reads and inflates every RBKC chunk without retaining the flat book.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rabook_validate_open(mdl_rabook_vfs_t* ctx, book_header_t* out_header)
{
  return book_chunked_validate_strict(&ctx->reader,
                                      ctx->chunk,
                                      ctx->chunk_cap,
                                      ctx->scratch,
                                      ctx->scratch_cap,
                                      out_header);
}

ra8_err_t mdl_rabook_vfs_init(mdl_rabook_vfs_t* ctx, const mdl_rabook_vfs_config_t* config)
{
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (config == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (config->inflate_cb == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (config->table == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (config->compressed == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (config->chunk == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (config->scratch == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (config->table_cap < 2U) {
    return k_ra8_err_invalid_size;
  }
  if (config->compressed_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (config->chunk_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (config->scratch_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  const mdl_rabook_vfs_t initialized = {
    .inflate_cb     = config->inflate_cb,
    .table          = config->table,
    .compressed     = config->compressed,
    .chunk          = config->chunk,
    .scratch        = config->scratch,
    .table_cap      = config->table_cap,
    .compressed_cap = config->compressed_cap,
    .chunk_cap      = config->chunk_cap,
    .scratch_cap    = config->scratch_cap,
  };
  *ctx = initialized;
  return k_ra8_ok;
}

ra8_err_t mdl_rabook_vfs_validate(void*         opaque,
                                  const char*   staging_path,
                                  uint64_t      total_bytes,
                                  const uint8_t sha256[k_ra8_mdl_sha256_bytes])
{
  if (opaque == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (staging_path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (sha256 == nullptr) {
    return k_ra8_err_null_ptr;
  }
  mdl_rabook_vfs_t* const ctx = opaque;
  if (ctx->open) {
    return k_ra8_err_invalid_state;
  }
  if (ctx->file != nullptr) {
    return k_ra8_err_invalid_state;
  }
  ctx->transfer_validated = false;
  (void)memset(ctx->digest, 0, sizeof(ctx->digest));
  ra8_err_t     err       = internal_rabook_open_reader(ctx, staging_path, total_bytes, true);
  book_header_t validated = {};
  if (err == k_ra8_ok) {
    err = internal_rabook_validate_open(ctx, &validated);
  }
  const ra8_err_t close_err = internal_rabook_close(ctx);
  if (err == k_ra8_ok) {
    err = close_err;
  }
  if (err == k_ra8_ok) {
    ctx->header             = validated;
    ctx->file_size          = total_bytes;
    ctx->transfer_validated = true;
    (void)memcpy(ctx->digest, sha256, sizeof(ctx->digest));
  }
  return err;
}

ra8_err_t mdl_rabook_vfs_open(mdl_rabook_vfs_t* ctx, const char* path)
{
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (ctx->open) {
    return k_ra8_err_invalid_state;
  }
  if (ctx->file != nullptr) {
    return k_ra8_err_invalid_state;
  }
  ra8_err_t     err       = internal_rabook_open_reader(ctx, path, 0U, false);
  book_header_t validated = {};
  if (err == k_ra8_ok) {
    err = internal_rabook_validate_open(ctx, &validated);
  }
  if (err == k_ra8_ok) {
    ctx->header = validated;
    ctx->open   = true;
  } else {
    const ra8_err_t close_err = internal_rabook_close(ctx);
    (void)close_err;
  }
  return err;
}

ra8_err_t
mdl_rabook_vfs_read_chunk(mdl_rabook_vfs_t* ctx, uint64_t offset, uint8_t* dst, uint32_t len)
{
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (dst == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!ctx->open) {
    return k_ra8_err_invalid_state;
  }
  if (ctx->file == nullptr) {
    return k_ra8_err_invalid_state;
  }
  return book_chunked_read(&ctx->reader, offset, dst, len);
}

ra8_err_t
mdl_rabook_vfs_info(const mdl_rabook_vfs_t* ctx, book_header_t* out_header, uint64_t* out_flat_size)
{
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_header == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_flat_size == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!ctx->open) {
    return k_ra8_err_invalid_state;
  }
  if (ctx->file == nullptr) {
    return k_ra8_err_invalid_state;
  }
  *out_header    = ctx->header;
  *out_flat_size = ctx->reader.inflated_total;
  return k_ra8_ok;
}

ra8_err_t mdl_rabook_vfs_close(mdl_rabook_vfs_t* ctx)
{
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  return internal_rabook_close(ctx);
}
