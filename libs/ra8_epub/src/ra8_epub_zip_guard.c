/**
 * @file ra8_epub_zip_guard.c
 * @brief Decompression-limits retrofit for every miniz ZIP consumer.
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: NS}
 *
 * @details
 * The two checks that bind the pre-existing ZIP-store / DEFLATE paths
 * (EPUB open, chapter / cover / resource extraction, the #231 iterative
 * entry reader, and the comic CBZ backend via `ra8_epub_miniz_alloc`'s
 * shared pool) to the unified decompression-limits policy
 * (`ra8_decomp_limits.h`):
 *
 * - ::ra8_epub_zip_guard_archive caps the central-directory entry count
 *   at open (the many-tiny-entries bomb dies before any entry work);
 * - ::ra8_epub_zip_guard_entry checks every entry's declared sizes before
 *   inflation (lying headers and declared decompression bombs die at
 *   O(1)). miniz itself then bounds the *actual* inflation to the
 *   declared uncompressed size, closing the loop.
 *
 * Both run the ONE default policy -- the same record every other decoder
 * in the content path enforces.
 *
 * @since 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>

#include "ra8_check.h"
#include "ra8_decomp_limits.h"
#include "ra8_epub_internal.h"

/** @brief Log tag for ZIP-guard diagnostics. */
static const char* const s_tag_zip_guard = "ra8_epub_zip";

ra8_err_t ra8_epub_zip_guard_archive(mz_zip_archive* zip)
{
  RA8_CHECK_NULL_PTR(zip, s_tag_zip_guard, "guard archive: null zip");
  const ra8_decomp_limits_t lim = ra8_decomp_limits_default();
  const mz_uint             num = mz_zip_reader_get_num_files(zip);
  if ((uint64_t)num > (uint64_t)lim.max_entries) {
    return k_ra8_err_decomp_entries;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_epub_zip_guard_entry(const mz_zip_archive_file_stat* st)
{
  RA8_CHECK_NULL_PTR(st, s_tag_zip_guard, "guard entry: null stat");
  const ra8_decomp_limits_t lim = ra8_decomp_limits_default();
  return ra8_decomp_check_declared(&lim, (uint64_t)st->m_comp_size, (uint64_t)st->m_uncomp_size);
}
