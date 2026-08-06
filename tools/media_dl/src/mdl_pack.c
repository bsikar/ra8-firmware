/**
 * @file mdl_pack.c
 * @brief Implementation of the downloaded-folder archive packaging.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_pack.h"

#include <stdint.h>
#include <stdio.h>

#include "mdl_export.h"
#include "mdl_fetch.h"
#include "mdl_fetch_internal.h"
#include "mdl_sanitize.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Path buffer sizes for a composed archive leaf and directory. */
typedef enum : uint16_t {
  k_pack_leaf_bytes = 256,  /**< Composed archive/dir leaf name. */
  k_pack_dir_bytes  = 1024, /**< Directory-path buffer.          */
} mdl_pack_size_t;

/** @brief True when an snprintf result of `n` fully fit a buffer of `cap`. */
RA8_INTERNAL static bool snprintf_fit(int n, size_t cap)
{
  return (n >= 0) && ((size_t)n < cap);
}

size_t
mdl_pack_one(ra8_arena_t* arena, mdl_format_t format, const char* series_dir, const char* chap_id)
{
  const char* ext = mdl_format_ext(format);
  char        dir[k_pack_dir_bytes];
  if (!mdl_path_join(series_dir, chap_id, dir, sizeof(dir))) {
    (void)fprintf(stderr, "  export %s.%s path rejected, skipped\n", chap_id, ext);
    return 1U;
  }
  if (mdl_format_is_dir_output(format)) {
    /* JOF writes per-page `.jof` siblings into the chapter dir; report that dir,
     * never a single-container name that was not created. */
    const ra8_err_t drc = mdl_export_chapter(arena, format, dir, dir);
    if (drc != k_ra8_ok) {
      (void)fprintf(stderr, "  export %s .%s FAILED (err 0x%X)\n", chap_id, ext, (unsigned)drc);
      return 1U;
    }
    (void)printf("  converted %s -> %s/*.%s\n", chap_id, dir, ext);
    return 0U;
  }
  char      leaf[k_pack_leaf_bytes];
  const int ln = snprintf(leaf, sizeof(leaf), "%s.%s", chap_id, ext);
  char      out[k_pack_dir_bytes];
  if (!snprintf_fit(ln, sizeof(leaf)) || !mdl_path_join(series_dir, leaf, out, sizeof(out))) {
    (void)fprintf(stderr, "  export %s.%s path rejected, skipped\n", chap_id, ext);
    return 1U;
  }
  const ra8_err_t rc = mdl_export_chapter(arena, format, dir, out);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "  export %s.%s FAILED (err 0x%X)\n", chap_id, ext, (unsigned)rc);
    return 1U;
  }
  (void)printf("  packaged %s.%s\n", chap_id, ext);
  return 0U;
}

/** @brief Package the combined chapter folder `combined_rel` into `format`. */
RA8_INTERNAL static size_t pack_combined_dir(ra8_arena_t* arena,
                                             mdl_format_t format,
                                             const char*  series_dir,
                                             const char*  combined_rel,
                                             bool         incomplete)
{
  const char* ext  = mdl_format_ext(format);
  const char* mark = incomplete ? " (INCOMPLETE)" : "";
  char        dir[k_pack_dir_bytes];
  if (!mdl_path_join(series_dir, combined_rel, dir, sizeof(dir))) {
    (void)fprintf(stderr, "  combine export path rejected under %s\n", series_dir);
    return 1U;
  }
  if (mdl_format_is_dir_output(format)) {
    /* JOF: the combined pages become `.jof` siblings inside the combined dir. */
    const ra8_err_t drc = mdl_export_chapter(arena, format, dir, dir);
    if (drc != k_ra8_ok) {
      (void)fprintf(stderr, "  combine export FAILED (err 0x%X)\n", (unsigned)drc);
      return 1U;
    }
    (void)printf("  combined%s -> %s/*.%s\n", mark, dir, ext);
    return 0U;
  }
  char      leaf[k_pack_leaf_bytes];
  const int ln = incomplete ? snprintf(leaf, sizeof(leaf), "%s.INCOMPLETE.%s", combined_rel, ext)
                            : snprintf(leaf, sizeof(leaf), "%s.%s", combined_rel, ext);
  char      out[k_pack_dir_bytes];
  if (!snprintf_fit(ln, sizeof(leaf)) || !mdl_path_join(series_dir, leaf, out, sizeof(out))) {
    (void)fprintf(stderr, "  combine export path rejected under %s\n", series_dir);
    return 1U;
  }
  const ra8_err_t rc = mdl_export_chapter(arena, format, dir, out);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "  combine export FAILED (err 0x%X)\n", (unsigned)rc);
    return 1U;
  }
  (void)printf("  combined%s -> %s\n", mark, out);
  return 0U;
}

size_t mdl_pack_combined(ra8_arena_t*             arena,
                         mdl_format_t             format,
                         bool                     allow_incomplete,
                         const char*              series_dir,
                         const char*              combined_rel,
                         const mdl_fetch_stats_t* stats)
{
  if (stats->chapters_completed == 0U) {
    return 0U; /* nothing was fetched to package */
  }
  const bool incomplete = mdl_fetch_run_incomplete(stats);
  if (incomplete) {
    if (!allow_incomplete) {
      (void)fprintf(stderr,
                    "  combine: NOT packaged -- run is incomplete "
                    "(%zu chapter(s) / %zu page(s) failed); pass "
                    "--allow-incomplete to force a marked archive\n",
                    stats->chapters_failed,
                    stats->pages_failed);
      return 0U;
    }
  }
  return pack_combined_dir(arena, format, series_dir, combined_rel, incomplete);
}
