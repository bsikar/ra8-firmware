/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
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
 * @brief Package `chapter_dir`'s contents into `out_path` as `fmt`.
 *
 * @param[in] fmt         Target container (must not be loose/invalid).
 * @param[in] chapter_dir Absolute path to the chapter's page folder.
 * @param[in] out_path    Absolute path of the archive to create.
 *
 * @retval k_ra8_ok               Archive written.
 * @retval k_ra8_err_invalid_arg  NULL/loose/invalid argument.
 * @retval k_ra8_err_not_supported The archiver tool is not on PATH.
 * @retval k_ra8_fail             The archiver ran but failed.
 */
ra8_err_t mdl_export_chapter(mdl_format_t fmt, const char* chapter_dir, const char* out_path);
