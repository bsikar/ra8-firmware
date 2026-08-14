/**
 * @file mdl_pack.c
 * @brief Implementation of the downloaded-folder archive packaging.
 * @details Discovers prepared chapter directories, validates bounded paths,
 *          and delegates deterministic container creation to the exporter
 *          using the caller-owned workspace.
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

/**
 * @brief Test whether an snprintf result fully fit its destination
 * @details Treats negative encoding errors and the terminating-NUL boundary as
 *          failures so path construction never accepts truncated output.
 * @param[in] n Return value produced by snprintf.
 * @param[in] cap Destination buffer capacity passed to snprintf.
 * @return Whether the complete formatted string fit.
 * @retval true @p n is non-negative and strictly smaller than @p cap.
 * @retval false Formatting failed or required at least @p cap bytes.
 * @pre @p cap is the exact capacity used by the matching snprintf call.
 * @pre @p n has not been altered after that call.
 * @post No state is modified.
 * @post The result can safely gate subsequent path use.
 * @note Thread-safe: this is a pure arithmetic predicate.
 * @since 0.1.0
 */
RA8_INTERNAL static bool snprintf_fit(int n, size_t cap)
{
  return (n >= 0) && ((size_t)n < cap);
}

size_t mdl_pack_one_meta(mdl_format_t             format,
                         const char*              series_dir,
                         const char*              chap_id,
                         const mdl_export_meta_t* meta,
                         mdl_export_workspace_t*  ws)
{
  const char* ext = mdl_format_ext(format);
  char        dir[k_pack_dir_bytes];
  if (!mdl_path_join(series_dir, chap_id, dir, sizeof(dir))) {
    (void)fprintf(stderr, "  export %s.%s path rejected, skipped\n", chap_id, ext);
    return 1U;
  }

  mdl_export_meta_t m;
  if (meta != nullptr) {
    m = *meta;
  } else {
    (void)mdl_meta_load_dir(&m, dir);
  }

  if (mdl_format_is_dir_output(format)) {
    /* JOF writes per-page `.jof` siblings into the chapter dir; report that dir,
     * never a single-container name that was not created. */
    const ra8_err_t drc = mdl_export_chapter_meta_ws(format, dir, dir, &m, ws);
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
  const ra8_err_t rc = mdl_export_chapter_meta_ws(format, dir, out, &m, ws);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "  export %s.%s FAILED (err 0x%X)\n", chap_id, ext, (unsigned)rc);
    return 1U;
  }
  (void)printf("  packaged %s.%s\n", chap_id, ext);
  return 0U;
}

size_t mdl_pack_one(mdl_format_t            format,
                    const char*             series_dir,
                    const char*             chap_id,
                    mdl_export_workspace_t* ws)
{
  return mdl_pack_one_meta(format, series_dir, chap_id, nullptr, ws);
}

/**
 * @brief Package a combined chapter folder into the selected format
 * @details Composes guarded input/output paths, auto-loads metadata when
 *          needed, marks incomplete filenames, and handles directory-output
 *          formats without claiming that a container file was created.
 * @param[in] format Output format to write.
 * @param[in] series_dir Absolute series directory.
 * @param[in] combined_rel Sanitized combined-directory leaf.
 * @param[in] incomplete Whether the output filename must be marked incomplete.
 * @param[in] meta Metadata to embed, or NULL to auto-load it.
 * @param[in,out] ws Exclusive caller-owned exporter workspace.
 * @return Count of failures from this operation.
 * @retval 0U Packaging succeeded.
 * @retval 1U A path was rejected or export failed.
 * @pre String arguments are non-NULL, NUL-terminated, and stable.
 * @pre @p ws owns writable arena storage for the call.
 * @post Success leaves output matching @p format and @p incomplete.
 * @post Failure is diagnosed and never counted more than once.
 * @note Not thread-safe for a shared workspace or output directory.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t pack_combined_dir(mdl_format_t             format,
                                             const char*              series_dir,
                                             const char*              combined_rel,
                                             bool                     incomplete,
                                             const mdl_export_meta_t* meta,
                                             mdl_export_workspace_t*  ws)
{
  const char* ext  = mdl_format_ext(format);
  const char* mark = incomplete ? " (INCOMPLETE)" : "";
  char        dir[k_pack_dir_bytes];
  if (!mdl_path_join(series_dir, combined_rel, dir, sizeof(dir))) {
    (void)fprintf(stderr, "  combine export path rejected under %s\n", series_dir);
    return 1U;
  }

  mdl_export_meta_t m;
  if (meta != nullptr) {
    m = *meta;
  } else {
    (void)mdl_meta_load_dir(&m, dir);
  }

  if (mdl_format_is_dir_output(format)) {
    /* JOF: the combined pages become `.jof` siblings inside the combined dir. */
    const ra8_err_t drc = mdl_export_chapter_meta_ws(format, dir, dir, &m, ws);
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
  const ra8_err_t rc = mdl_export_chapter_meta_ws(format, dir, out, &m, ws);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "  combine export FAILED (err 0x%X)\n", (unsigned)rc);
    return 1U;
  }
  (void)printf("  combined%s -> %s\n", mark, out);
  return 0U;
}

size_t mdl_pack_combined_meta(mdl_format_t             format,
                              bool                     allow_incomplete,
                              const char*              series_dir,
                              const char*              combined_rel,
                              const mdl_fetch_stats_t* stats,
                              const mdl_export_meta_t* meta,
                              mdl_export_workspace_t*  ws)
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
  return pack_combined_dir(format, series_dir, combined_rel, incomplete, meta, ws);
}

size_t mdl_pack_combined(mdl_format_t             format,
                         bool                     allow_incomplete,
                         const char*              series_dir,
                         const char*              combined_rel,
                         const mdl_fetch_stats_t* stats,
                         mdl_export_workspace_t*  ws)
{
  return mdl_pack_combined_meta(format,
                                allow_incomplete,
                                series_dir,
                                combined_rel,
                                stats,
                                nullptr,
                                ws);
}
