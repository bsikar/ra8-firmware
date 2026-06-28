/**
 * @file ra_rabook_import_compiler.c
 * @brief Production adapter binding the import seam to the real compiler (#151).
 * @since Version 0.1.0
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB Import] {World: NS}
 */

#include "ra_rabook_import_compiler.h"

#include <stddef.h>

#include "ra_book.h"
#include "ra_check.h"
#include "ra_epub.h"
#include "ra_epub_fs.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_rabook_pipeline.h"

/** @brief Log tag for this adapter. @since Version 0.1.0 */
static const char* const s_tag = "ra_rabook_import_compiler";

ra_err_t ra_rabook_import_compile_adapter(void*          compile_ctx,
                                          ra_fs_mount_t* mount,
                                          const char*    epub_path,
                                          const char*    out_path)
{
  RA_CHECK_NULL_PTR(compile_ctx, s_tag, "compile_ctx");
  RA_CHECK_NULL_PTR(mount, s_tag, "mount");
  RA_CHECK_NULL_PTR(epub_path, s_tag, "epub_path");
  RA_CHECK_NULL_PTR(out_path, s_tag, "out_path");

  /* The cookie's epub/load-buf are validated by `ra_epub_open_fs` and the
   * bufs/scr by `ra_rabook_compile_from_epub`, so a NULL cookie field still
   * yields k_ra_err_null_ptr without restating those guards here. */
  ra_rabook_import_compiler_ctx_t* ctx = (ra_rabook_import_compiler_ctx_t*)compile_ctx;

  ra_err_t err =
    ra_epub_open_fs(mount, epub_path, ctx->epub_load_buf, ctx->epub_load_cap, ctx->epub);
  if (err != k_ra_ok) {
    return err;
  }

  err           = ra_rabook_compile_from_epub(ctx->epub, ctx->bufs, ctx->scr, mount, out_path);
  ra_err_t cerr = ra_epub_close(ctx->epub);
  if (err != k_ra_ok) {
    return err;
  }
  return cerr;
}

/**
 * @brief Read a whole file off @p mount into @p buf, closing it on every path.
 * @param[in]  mount   Mounted volume (non-NULL).
 * @param[in]  path    Root-level 8.3 source path (non-NULL).
 * @param[out] buf     Destination buffer (non-NULL).
 * @param[in]  cap     Capacity of @p buf in bytes.
 * @param[out] out_len Receives the number of bytes read on success.
 * @return Error code.
 * @retval k_ra_ok    The file was read; @p *out_len holds its length.
 * @retval k_ra_err_* Propagated open / read / close error.
 * @pre @p buf and @p out_len are non-NULL.
 * @pre @p path names an existing file on @p mount.
 * @post On success @p buf holds @p *out_len bytes and the file is closed.
 * @post On any error the file is closed if it was opened.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra_err_t s_read_whole_file(ra_fs_mount_t* mount,
                                  const char*    path,
                                  uint8_t*       buf,
                                  uint32_t       cap,
                                  uint32_t*      out_len)
{
  ra_fs_file_t* file = nullptr;
  ra_err_t      err  = ra_fs_open(mount, path, k_ra_fs_mode_read, &file);
  if (err != k_ra_ok) {
    return err;
  }
  err           = ra_fs_read(file, buf, cap, out_len);
  ra_err_t cerr = ra_fs_close(file);
  if (err != k_ra_ok) {
    return err;
  }
  return cerr;
}

/**
 * @brief Dispatch the staged compile to the secondary core, validate, cache it.
 * @param[in]     ctx      Populated M33 cookie (dispatch + buffers).
 * @param[in]     epub_len Source length already in @p ctx->epub_load_buf.
 * @param[in,out] mount    Mounted volume the validated blob is written to.
 * @param[in]     out_path Path to write the RABOOK1 body to (importer temp name).
 * @return Error code.
 * @retval k_ra_ok    Blob dispatched, validated, and written to @p out_path.
 * @retval k_ra_err_* Propagated dispatch / validate / write error.
 * @pre @p ctx->dispatch and @p ctx->blob_buf are non-NULL.
 * @pre @p ctx->epub_load_buf holds @p epub_len readable bytes.
 * @post On success @p out_path holds a `ra_book_validate`-clean blob.
 * @post On any error @p out_path is not written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra_err_t s_dispatch_and_cache(const ra_rabook_import_compiler_m33_ctx_t* ctx,
                                     uint32_t                                   epub_len,
                                     ra_fs_mount_t*                             mount,
                                     const char*                                out_path)
{
  uint32_t blob_len = 0U;
  ra_err_t err      = ctx->dispatch(ctx->dispatch_ctx,
                                    ctx->epub_load_buf,
                                    epub_len,
                                    ctx->blob_buf,
                                    ctx->blob_cap,
                                    &blob_len);
  if (err != k_ra_ok) {
    return err;
  }
  /* Validate the worker-produced blob before caching -- a cross-core transfer
   * slip or a worker fault could yield a structurally-broken blob. */
  err = ra_book_validate(ctx->blob_buf, (size_t)blob_len);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_fs_write_file(mount, out_path, ctx->blob_buf, blob_len);
}

ra_err_t ra_rabook_import_compile_adapter_m33(void*          compile_ctx,
                                              ra_fs_mount_t* mount,
                                              const char*    epub_path,
                                              const char*    out_path)
{
  RA_CHECK_NULL_PTR(compile_ctx, s_tag, "compile_ctx");
  RA_CHECK_NULL_PTR(mount, s_tag, "mount");
  RA_CHECK_NULL_PTR(epub_path, s_tag, "epub_path");
  RA_CHECK_NULL_PTR(out_path, s_tag, "out_path");

  ra_rabook_import_compiler_m33_ctx_t* ctx = (ra_rabook_import_compiler_m33_ctx_t*)compile_ctx;
  RA_CHECK_NULL_PTR(ctx->dispatch, s_tag, "dispatch");

  /* ctx->epub_load_buf / ctx->blob_buf are caller-owned per the cookie invariant
   * and reached through ra_fs_read / the dispatch, so a NULL there is a caller
   * bug -- matching how the in-core adapter leans on its downstream guards. */

  /* Read the source .epub off the mount (the M85 owns the FS), then offload. */
  uint32_t epub_len = 0U;
  ra_err_t err =
    s_read_whole_file(mount, epub_path, ctx->epub_load_buf, ctx->epub_load_cap, &epub_len);
  if (err != k_ra_ok) {
    return err;
  }
  return s_dispatch_and_cache(ctx, epub_len, mount, out_path);
}
