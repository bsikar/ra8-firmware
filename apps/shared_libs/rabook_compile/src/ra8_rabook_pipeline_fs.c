/**
 * @file ra8_rabook_pipeline_fs.c
 * @brief Optional ra8_fs publication wrapper for the RABOOK pipeline.
 * @details Keeps filesystem linkage out of the reusable caller-buffer compiler
 *          while preserving the original public convenience entry point.
 *
 * [Ring 4 / EPUB Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_fs.h"
#include "ra8_rabook_pipeline.h"
#include "ra8_rabook_pipeline_internal.h"

ra8_err_t rabook_compile_from_epub(epub_book_t*                         epub,
                                   const ra8_rabook_buffers_t*          buffers,
                                   const ra8_rabook_pipeline_scratch_t* scratch,
                                   ra8_fs_mount_t*                      mount,
                                   const char*                          out_path)
{
  static const char* const tag   = "ra8_rabook_pipeline";
  ra8_err_t                error = priv_rabook_pipeline_check_common(epub, buffers, scratch);
  if (error != k_ra8_ok) {
    return error;
  }
  RA8_CHECK_NULL_PTR(mount, tag, "mount");
  RA8_CHECK_NULL_PTR(out_path, tag, "out_path");

  const void* blob     = nullptr;
  uint32_t    blob_len = 0U;
  error = rabook_compile_from_epub_to_buffer(epub, buffers, scratch, &blob, &blob_len);
  if (error != k_ra8_ok) {
    return error;
  }
  return ra8_fs_write_file(mount, out_path, (const uint8_t*)blob, blob_len);
}
