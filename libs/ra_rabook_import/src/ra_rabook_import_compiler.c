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
