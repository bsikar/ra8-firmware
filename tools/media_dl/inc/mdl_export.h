/**
 * @file mdl_export.h
 * @brief Package a downloaded chapter folder into a reader-openable container.
 *
 * @details
 * The firmware readers open several containers -- CBZ (ZIP), CBR (RAR), CBT
 * (tar), and xz/gzip-wrapped variants -- all detected by file magic, plus the
 * custom JOF tile atlas and RABOOK formats. This module turns a chapter's
 * folder of page images into one of those so the output can be fed back to the
 * readers for testing.
 *
 * Archive formats are produced by spawning the system archiver (zip/tar/xz/rar)
 * -- these are host test-fixture generators, not firmware code, so reusing the
 * platform's archivers is the pragmatic choice. RAR has no open writer, so
 * `cbr` requires the proprietary `rar` tool on PATH (a clear error is printed
 * when it is absent). JOF/RABOOK (added separately) reuse the firmware's own
 * producers compiled host-side.
 *
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT

 */
#pragma once

#include <stdint.h>

#include "ra8_err.h"

/** @brief Output container selected by `--format`. */
typedef enum : uint8_t {
  k_mdl_fmt_loose   = 0,   /**< Leave loose page images (no archive).             */
  k_mdl_fmt_cbz     = 1,   /**< ZIP of images (`.cbz`).                           */
  k_mdl_fmt_cbt     = 2,   /**< tar of images (`.cbt`).                           */
  k_mdl_fmt_cbr     = 3,   /**< RAR of images (`.cbr`); needs external `rar`.     */
  k_mdl_fmt_cbt_xz  = 4,   /**< xz-compressed tar (`.cbt.xz`).                    */
  k_mdl_fmt_cbt_gz  = 5,   /**< gzip-compressed tar (`.cbt.gz`).                  */
  k_mdl_fmt_epub    = 6,   /**< EPUB of images (`.epub`).                         */
  k_mdl_fmt_jof     = 7,   /**< Native JOF tile atlas per page (`.jof`).          */
  k_mdl_fmt_rabook  = 8,   /**< Native RABOOK (`.rabook`); needs external python. */
  k_mdl_fmt_invalid = 255, /**< Unrecognised `--format` string.                   */
} mdl_format_t;

/**
 * @brief Map a `--format` string to a container kind.
 * @param[in] s Format name, or NULL (treated as "loose").
 * @return The matching kind, or ::k_mdl_fmt_invalid.
 */
mdl_format_t mdl_format_from_str(const char* s);

/** @brief File extension (without dot) for a format, e.g. "cbz" / "cbt.xz". */
const char* mdl_format_ext(mdl_format_t fmt);

/**
 * @brief Whether `fmt` writes per-page sibling files rather than one container.
 *
 * @details
 * JOF is inherently per-page: ::mdl_export_chapter writes one `.jof` band atlas
 * beside each source image inside the chapter directory, so a JOF "chapter" is a
 * directory of atlases, not a single archive at `out_path`. Every other archive
 * format produces exactly one file at `out_path`. Callers use this to report
 * what was actually written -- a success message must never name a container
 * file that a directory-output format did not create.
 *
 * @param[in] fmt Format to classify.
 *
 * @return Whether @p fmt produces per-page sibling files in the chapter dir.
 * @retval true  @p fmt is ::k_mdl_fmt_jof (per-page `.jof` siblings).
 * @retval false @p fmt produces a single container file at `out_path`.
 *
 * @pre @p fmt is a value of ::mdl_format_t.
 * @post No state is mutated (pure classifier).
 *
 * @note Thread-safe: pure function of its argument.
 * @see mdl_export_chapter()
 * @since 0.1.0
 */
bool mdl_format_is_dir_output(mdl_format_t fmt);

/**
 * @brief Package `chapter_dir`'s contents into `out_path` as `fmt`.
 *
 * @details
 * For single-container formats (CBZ/CBT/CBT.GZ/CBT.XZ/CBR/EPUB/RABOOK) the
 * archive is created at @p out_path. For a directory-output format (JOF, see
 * ::mdl_format_is_dir_output) the pages are written as `.jof` siblings inside
 * @p chapter_dir and @p out_path is unused. Fails rather than silently
 * truncating: a chapter with more than ::k_max_pages page images, a page name
 * that will not fit a ustar entry, or an EPUB accumulator overrun all return an
 * error instead of producing short or malformed output.
 *
 * @param[in] fmt         Target container (must not be loose/invalid).
 * @param[in] chapter_dir Absolute path to the chapter's page folder.
 * @param[in] out_path    Absolute path of the archive to create (single-file
 *                        formats); unused for directory-output formats but must
 *                        be non-NULL.
 *
 * @retval k_ra8_ok               Archive (or every `.jof` sibling) written.
 * @retval k_ra8_err_invalid_arg  NULL/loose/invalid argument.
 * @retval k_ra8_err_invalid_size Over-long page name, or more than
 *                                ::k_max_pages page images in @p chapter_dir.
 * @retval k_ra8_err_empty        No page images found in @p chapter_dir.
 * @retval k_ra8_err_not_supported The archiver tool is not on PATH.
 * @retval k_ra8_fail             The archiver ran but failed.
 */
#include "ra8_host_arena.h"

ra8_err_t mdl_export_chapter(ra8_arena_t* arena,
                             mdl_format_t fmt,
                             const char*  chapter_dir,
                             const char*  out_path);
