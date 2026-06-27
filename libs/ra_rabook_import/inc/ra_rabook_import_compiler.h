/**
 * @file ra_rabook_import_compiler.h
 * @brief Production adapter binding the import seam to the real compiler (#151).
 *
 * @details
 * @ref ra_rabook_import.h keeps the cache manager decoupled from the EPUB parser
 * and the RABOOK1 emitter by reaching the compile step through the injected
 * @ref ra_rabook_import_compile_fn seam. This adapter is the production binding
 * of that seam: it loads the source `.epub` off the filesystem, drives
 * @ref ra_rabook_compile_from_epub, and closes the book -- matching the seam's
 * exact signature so an app passes `ra_rabook_import_compile_adapter` (with a
 * @ref ra_rabook_import_compiler_ctx_t cookie) straight into
 * @ref ra_rabook_import_cfg_t.
 *
 * Host tests bypass this adapter entirely with a lightweight compile spy, which
 * is the whole point of the seam -- they need neither the parser nor the
 * multi-megabyte builder arenas.
 *
 * @note Not thread-safe; the cookie's storage is single-owner.
 * @see ra_rabook_import.h          The cache manager that calls this seam.
 * @see ra_rabook_compile_from_epub The compiler this wraps.
 * @since Version 0.1.0
 *
 * [Ring 4 / EPUB Import] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "ra_epub.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_rabook_pipeline.h"

/**
 * @struct ra_rabook_import_compiler_ctx_t
 * @brief Cookie carrying the storage @ref ra_rabook_import_compile_adapter needs.
 *
 * @details All buffers are caller-owned and sized for the worst-case book. The
 *          adapter loads the whole `.epub` into @p epub_load_buf (the in-RAM
 *          form `ra_epub` requires), then compiles via the builder arenas in
 *          @p bufs and the decode scratch in @p scr.
 *
 * @invariant Every pointer is non-NULL and each buffer holds its stated size.
 * @since Version 0.1.0
 */
typedef struct {
  ra_epub_book_t*                     epub;          /**< Caller-owned open-book storage.  */
  uint8_t*                            epub_load_buf; /**< Buffer the `.epub` is read into. */
  size_t                              epub_load_cap; /**< Capacity of @p epub_load_buf.    */
  const ra_rabook_buffers_t*          bufs;          /**< RABOOK1 builder arenas.          */
  const ra_rabook_pipeline_scratch_t* scr;           /**< XHTML + image decode scratch.    */
} ra_rabook_import_compiler_ctx_t;

/**
 * @brief Import-seam adapter: load a `.epub` off @p mount and compile it.
 *
 * @details Signature matches @ref ra_rabook_import_compile_fn. Opens the source
 *          with `ra_epub_open_fs`, runs `ra_rabook_compile_from_epub` to
 *          @p out_path, and closes the book on every path.
 *
 * @param[in]     compile_ctx Pointer to a @ref ra_rabook_import_compiler_ctx_t.
 * @param[in,out] mount       Mounted volume holding the source and the output.
 * @param[in]     epub_path   Root-level 8.3 path of the source `.epub`.
 * @param[in]     out_path    Path to write the RABOOK1 body to (the importer's
 *                            temp name).
 *
 * @return Error code.
 * @retval k_ra_ok           Book compiled and written to @p out_path.
 * @retval k_ra_err_null_ptr A required pointer (incl. a cookie field) is NULL.
 * @retval k_ra_err_*        Propagated EPUB-open or compile error.
 *
 * @pre @p compile_ctx points at a fully-populated cookie.
 * @pre @p mount, @p epub_path, and @p out_path are non-NULL.
 * @post On `k_ra_ok`, @p out_path holds a valid RABOOK1 blob and the book is closed.
 * @post On error the book is closed and no valid output is implied.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
[[nodiscard]] ra_err_t ra_rabook_import_compile_adapter(void*          compile_ctx,
                                                        ra_fs_mount_t* mount,
                                                        const char*    epub_path,
                                                        const char*    out_path);

#ifdef __cplusplus
}
#endif
