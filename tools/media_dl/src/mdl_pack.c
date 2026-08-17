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
#include "mdl_stream_internal.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Path buffer sizes for a composed archive leaf and directory. */
typedef enum : uint16_t {
  k_pack_leaf_bytes = 256,  /**< Composed archive/dir leaf name. */
  k_pack_dir_bytes  = 1024, /**< Directory-path buffer.          */
} mdl_pack_size_t;

/** @brief Append three borrowed packaging fragments to one stream.
 * @details Uses caller exporter workspace and injected filesystem and stream objects.
 *          Artifacts publish only through the selected bounded exporter.
 * @param[in,out] stream Destination stream state.
 * @param[in] first First text fragment.
 * @param[in] second Second text fragment.
 * @param[in] third Third text fragment.
 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_pack_text3(ra8_io_stream_t* stream,
                                                  const char*      first,
                                                  const char*      second,
                                                  const char*      third)
{
  ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, stream, first);
  error           = priv_mdl_stream_text(error, stream, second);
  return priv_mdl_stream_text(error, stream, third);
}

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
RA8_INTERNAL static bool internal_pack_snprintf_fit(int n, size_t cap)
{
  return (n >= 0) && ((size_t)n < cap);
}

/** @brief Copy explicit metadata or load it from a combined directory.
 * @details Uses caller exporter workspace and injected filesystem and stream objects.
 *          Artifacts publish only through the selected bounded exporter.
 * @param[in,out] storage Injected storage interface.
 * @param[in] meta Validated artifact metadata.
 * @param[in] dir Directory path or handle for the operation.
 * @return Selected metadata value, using on-disk defaults when @p meta is null.
 * @retval metadata Explicit metadata copy or bounded on-disk/default value.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static mdl_export_meta_t
internal_pack_metadata(mdl_storage_t* storage, const mdl_export_meta_t* meta, const char* dir)
{
  mdl_export_meta_t selected;
  if (meta != nullptr) {
    selected = *meta;
  } else {
    (void)mdl_meta_load_dir(storage, &selected, dir);
  }
  return selected;
}

/**
 * @brief Report one chapter's export failure with its error code.
 * @details Builds `"  export <chap_id>.<ext> FAILED (err 0x<rc>)\n"` on
 *          @p diagnostic, threading the running stream status through every
 *          fragment so a mid-message write failure is never masked.
 * @param[in,out] diagnostic Borrowed stream receiving the failure diagnostic.
 * @param[in] chap_id Chapter identifier.
 * @param[in] ext Format's canonical extension.
 * @param[in] rc The failure code being reported.
 * @return Nothing; the caller reports the packaging failure regardless.
 * @pre @p diagnostic, @p chap_id, and @p ext are non-NULL.
 * @post One diagnostic line was appended to @p diagnostic, or the stream's
 *       existing error was preserved.
 * @note Thread safety follows ownership of the supplied stream.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_pack_report_failure(ra8_io_stream_t* diagnostic,
                                                      const char*      chap_id,
                                                      const char*      ext,
                                                      ra8_err_t        rc)
{
  ra8_err_t output_error = internal_pack_text3(diagnostic, "  export ", chap_id, ".");
  output_error           = priv_mdl_stream_text(output_error, diagnostic, ext);
  output_error           = priv_mdl_stream_text(output_error, diagnostic, " FAILED (err 0x");
  output_error           = priv_mdl_stream_hex(output_error, diagnostic, (uint32_t)rc);
  (void)priv_mdl_stream_text(output_error, diagnostic, ")\n");
}

/**
 * @brief Package one chapter into a directory-output format (e.g. JOF).
 * @details Directory-output formats write per-page siblings directly into
 *          the chapter directory rather than one container file, so success
 *          is reported against that directory rather than a container name
 *          that was never created.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] format Output format to write; must be a directory-output format.
 * @param[in] dir Chapter directory, used as both source and destination.
 * @param[in] chap_id Chapter identifier, for diagnostics.
 * @param[in] ext Format's canonical extension, for diagnostics.
 * @param[in] meta Metadata to embed.
 * @param[in,out] ws Exclusive caller-owned exporter workspace.
 * @param[in,out] output Borrowed stream receiving the successful output path.
 * @param[in,out] diagnostic Borrowed stream receiving failure diagnostics.
 * @return Count of failures from this operation.
 * @retval 0U Packaging succeeded.
 * @retval 1U Export or diagnostic reporting failed.
 * @pre @p format is a directory-output format.
 * @pre String arguments are non-NULL, NUL-terminated, and stable.
 * @post Success leaves per-page siblings under @p dir.
 * @post Failure is diagnosed and never counted more than once.
 * @note Not thread-safe for a shared workspace or output directory.
 * @since Version 0.1.0
 */
RA8_INTERNAL static size_t internal_pack_dir_output(mdl_storage_t*           storage,
                                                    ra8_mdl_format_t         format,
                                                    const char*              dir,
                                                    const char*              chap_id,
                                                    const char*              ext,
                                                    const mdl_export_meta_t* meta,
                                                    mdl_export_workspace_t*  ws,
                                                    ra8_io_stream_t*         output,
                                                    ra8_io_stream_t*         diagnostic)
{
  const ra8_err_t drc = mdl_export_chapter_meta_ws(storage, format, dir, dir, meta, ws);
  if (drc != k_ra8_ok) {
    internal_pack_report_failure(diagnostic, chap_id, ext, drc);
    return 1U;
  }
  ra8_err_t output_error = internal_pack_text3(output, "  converted ", chap_id, " -> ");
  output_error           = priv_mdl_stream_text(output_error, output, dir);
  output_error           = priv_mdl_stream_text(output_error, output, "/*.");
  output_error           = priv_mdl_stream_text(output_error, output, ext);
  output_error           = priv_mdl_stream_text(output_error, output, "\n");
  return (output_error == k_ra8_ok) ? 0U : 1U;
}

size_t mdl_pack_one_meta(mdl_storage_t*           storage,
                         ra8_mdl_format_t         format,
                         const char*              series_dir,
                         const char*              chap_id,
                         const mdl_export_meta_t* meta,
                         mdl_export_workspace_t*  ws,
                         ra8_io_stream_t*         output,
                         ra8_io_stream_t*         diagnostic)
{
  const char* ext = mdl_format_ext(format);
  char        dir[k_pack_dir_bytes];
  if (!mdl_path_join(series_dir, chap_id, dir, sizeof(dir))) {
    (void)internal_pack_text3(diagnostic, "  export ", chap_id, ".");
    (void)internal_pack_text3(diagnostic, "", ext, " path rejected, skipped\n");
    return 1U;
  }

  const mdl_export_meta_t m = internal_pack_metadata(storage, meta, dir);

  if (mdl_format_is_dir_output(format)) {
    return internal_pack_dir_output(storage, format, dir, chap_id, ext, &m, ws, output, diagnostic);
  }
  char      leaf[k_pack_leaf_bytes];
  const int ln = snprintf(leaf, sizeof(leaf), "%s.%s", chap_id, ext);
  char      out[k_pack_dir_bytes];
  if (!internal_pack_snprintf_fit(ln, sizeof(leaf)) ||
      !mdl_path_join(series_dir, leaf, out, sizeof(out))) {
    ra8_err_t output_error = internal_pack_text3(diagnostic, "  export ", chap_id, ".");
    output_error           = priv_mdl_stream_text(output_error, diagnostic, ext);
    (void)priv_mdl_stream_text(output_error, diagnostic, " path rejected, skipped\n");
    return 1U;
  }
  const ra8_err_t rc = mdl_export_chapter_meta_ws(storage, format, dir, out, &m, ws);
  if (rc != k_ra8_ok) {
    internal_pack_report_failure(diagnostic, chap_id, ext, rc);
    return 1U;
  }
  ra8_err_t output_error = internal_pack_text3(output, "  packaged ", chap_id, ".");
  output_error           = priv_mdl_stream_text(output_error, output, ext);
  output_error           = priv_mdl_stream_text(output_error, output, "\n");
  return (output_error == k_ra8_ok) ? 0U : 1U;
}

size_t mdl_pack_one(mdl_storage_t*          storage,
                    ra8_mdl_format_t        format,
                    const char*             series_dir,
                    const char*             chap_id,
                    mdl_export_workspace_t* ws,
                    ra8_io_stream_t*        output,
                    ra8_io_stream_t*        diagnostic)
{
  return mdl_pack_one_meta(storage, format, series_dir, chap_id, nullptr, ws, output, diagnostic);
}

/**
 * @brief Package a combined chapter folder into the selected format
 * @details Composes guarded input/output paths, auto-loads metadata when
 *          needed, marks incomplete filenames, and handles directory-output
 *          formats without claiming that a container file was created.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] format Output format to write.
 * @param[in] series_dir Absolute series directory.
 * @param[in] combined_rel Sanitized combined-directory leaf.
 * @param[in] incomplete Whether the output filename must be marked incomplete.
 * @param[in] meta Metadata to embed, or NULL to auto-load it.
 * @param[in,out] ws Exclusive caller-owned exporter workspace.
 * @param[in,out] output Borrowed stream receiving the successful output path.
 * @param[in,out] diagnostic Borrowed stream receiving policy and failure diagnostics.
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
/**
 * @brief Report a combine-export failure with its error code.
 * @details Builds `"  combine export FAILED (err 0x<rc>)\n"` on @p diagnostic,
 *          threading the running stream status through every fragment so a
 *          mid-message write failure is never masked.
 * @param[in,out] diagnostic Borrowed stream receiving the failure diagnostic.
 * @param[in] rc The failure code being reported.
 * @return Nothing; the caller reports the packaging failure regardless.
 * @pre @p diagnostic is non-NULL.
 * @post One diagnostic line was appended to @p diagnostic, or the stream's
 *       existing error was preserved.
 * @note Thread safety follows ownership of the supplied stream.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_pack_report_combine_failure(ra8_io_stream_t* diagnostic,
                                                              ra8_err_t        rc)
{
  ra8_err_t output_error =
    priv_mdl_stream_text(k_ra8_ok, diagnostic, "  combine export FAILED (err 0x");
  output_error = priv_mdl_stream_hex(output_error, diagnostic, (uint32_t)rc);
  (void)priv_mdl_stream_text(output_error, diagnostic, ")\n");
}

/**
 * @brief Combine-package one directory into a directory-output format.
 * @details Directory-output formats write per-page siblings directly into
 *          the combined directory rather than one container file, so success
 *          is reported against that directory rather than a container name
 *          that was never created.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] format Output format to write; must be a directory-output format.
 * @param[in] dir Combined directory, used as both source and destination.
 * @param[in] ext Format's canonical extension, for diagnostics.
 * @param[in] mark `" (INCOMPLETE)"` or an empty string, for diagnostics.
 * @param[in] meta Metadata to embed.
 * @param[in,out] ws Exclusive caller-owned exporter workspace.
 * @param[in,out] output Borrowed stream receiving the successful output path.
 * @param[in,out] diagnostic Borrowed stream receiving failure diagnostics.
 * @return Count of failures from this operation.
 * @retval 0U Packaging succeeded.
 * @retval 1U Export or diagnostic reporting failed.
 * @pre @p format is a directory-output format.
 * @pre String arguments are non-NULL, NUL-terminated, and stable.
 * @post Success leaves per-page siblings under @p dir.
 * @post Failure is diagnosed and never counted more than once.
 * @note Not thread-safe for a shared workspace or output directory.
 * @since Version 0.1.0
 */
RA8_INTERNAL static size_t internal_pack_combined_dir_output(mdl_storage_t*           storage,
                                                             ra8_mdl_format_t         format,
                                                             const char*              dir,
                                                             const char*              ext,
                                                             const char*              mark,
                                                             const mdl_export_meta_t* meta,
                                                             mdl_export_workspace_t*  ws,
                                                             ra8_io_stream_t*         output,
                                                             ra8_io_stream_t*         diagnostic)
{
  const ra8_err_t drc = mdl_export_chapter_meta_ws(storage, format, dir, dir, meta, ws);
  if (drc != k_ra8_ok) {
    internal_pack_report_combine_failure(diagnostic, drc);
    return 1U;
  }
  ra8_err_t output_error = internal_pack_text3(output, "  combined", mark, " -> ");
  output_error           = priv_mdl_stream_text(output_error, output, dir);
  output_error           = priv_mdl_stream_text(output_error, output, "/*.");
  output_error           = priv_mdl_stream_text(output_error, output, ext);
  output_error           = priv_mdl_stream_text(output_error, output, "\n");
  return (output_error == k_ra8_ok) ? 0U : 1U;
}

RA8_INTERNAL static size_t internal_pack_combined_dir(mdl_storage_t*           storage,
                                                      ra8_mdl_format_t         format,
                                                      const char*              series_dir,
                                                      const char*              combined_rel,
                                                      bool                     incomplete,
                                                      const mdl_export_meta_t* meta,
                                                      mdl_export_workspace_t*  ws,
                                                      ra8_io_stream_t*         output,
                                                      ra8_io_stream_t*         diagnostic)
{
  const char* ext  = mdl_format_ext(format);
  const char* mark = incomplete ? " (INCOMPLETE)" : "";
  char        dir[k_pack_dir_bytes];
  if (!mdl_path_join(series_dir, combined_rel, dir, sizeof(dir))) {
    (void)
      internal_pack_text3(diagnostic, "  combine export path rejected under ", series_dir, "\n");
    return 1U;
  }

  const mdl_export_meta_t m = internal_pack_metadata(storage, meta, dir);

  if (mdl_format_is_dir_output(format)) {
    return internal_pack_combined_dir_output(storage,
                                             format,
                                             dir,
                                             ext,
                                             mark,
                                             &m,
                                             ws,
                                             output,
                                             diagnostic);
  }
  char      leaf[k_pack_leaf_bytes];
  const int ln = incomplete ? snprintf(leaf, sizeof(leaf), "%s.INCOMPLETE.%s", combined_rel, ext)
                            : snprintf(leaf, sizeof(leaf), "%s.%s", combined_rel, ext);
  char      out[k_pack_dir_bytes];
  if (!internal_pack_snprintf_fit(ln, sizeof(leaf)) ||
      !mdl_path_join(series_dir, leaf, out, sizeof(out))) {
    (void)
      internal_pack_text3(diagnostic, "  combine export path rejected under ", series_dir, "\n");
    return 1U;
  }
  const ra8_err_t rc = mdl_export_chapter_meta_ws(storage, format, dir, out, &m, ws);
  if (rc != k_ra8_ok) {
    internal_pack_report_combine_failure(diagnostic, rc);
    return 1U;
  }
  ra8_err_t output_error = internal_pack_text3(output, "  combined", mark, " -> ");
  output_error           = priv_mdl_stream_text(output_error, output, out);
  output_error           = priv_mdl_stream_text(output_error, output, "\n");
  return (output_error == k_ra8_ok) ? 0U : 1U;
}

size_t mdl_pack_combined_meta(mdl_storage_t*           storage,
                              ra8_mdl_format_t         format,
                              bool                     allow_incomplete,
                              const char*              series_dir,
                              const char*              combined_rel,
                              const mdl_fetch_stats_t* stats,
                              const mdl_export_meta_t* meta,
                              mdl_export_workspace_t*  ws,
                              ra8_io_stream_t*         output,
                              ra8_io_stream_t*         diagnostic)
{
  if (stats->chapters_completed == 0U) {
    return 0U; /* nothing was fetched to package */
  }
  const bool incomplete = priv_mdl_fetch_run_incomplete(stats);
  if (incomplete) {
    if (!allow_incomplete) {
      ra8_err_t output_error = priv_mdl_stream_text(k_ra8_ok,
                                                    diagnostic,
                                                    "  combine: NOT packaged -- run is "
                                                    "incomplete (");
      output_error = priv_mdl_stream_u64(output_error, diagnostic, stats->chapters_failed);
      output_error = priv_mdl_stream_text(output_error, diagnostic, " chapter(s) / ");
      output_error = priv_mdl_stream_u64(output_error, diagnostic, stats->pages_failed);
      output_error = priv_mdl_stream_text(output_error,
                                          diagnostic,
                                          " page(s) failed); pass --allow-incomplete to force "
                                          "a marked archive\n");
      return (output_error == k_ra8_ok) ? 0U : 1U;
    }
  }
  return internal_pack_combined_dir(storage,
                                    format,
                                    series_dir,
                                    combined_rel,
                                    incomplete,
                                    meta,
                                    ws,
                                    output,
                                    diagnostic);
}

size_t mdl_pack_combined(mdl_storage_t*           storage,
                         ra8_mdl_format_t         format,
                         bool                     allow_incomplete,
                         const char*              series_dir,
                         const char*              combined_rel,
                         const mdl_fetch_stats_t* stats,
                         mdl_export_workspace_t*  ws,
                         ra8_io_stream_t*         output,
                         ra8_io_stream_t*         diagnostic)
{
  return mdl_pack_combined_meta(storage,
                                format,
                                allow_incomplete,
                                series_dir,
                                combined_rel,
                                stats,
                                nullptr,
                                ws,
                                output,
                                diagnostic);
}
