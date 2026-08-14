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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/** @brief Output container selected by `--format`. */
typedef enum : uint8_t {
  k_mdl_fmt_loose   = 0,   /**< Leave loose page images (no archive).    */
  k_mdl_fmt_cbz     = 1,   /**< ZIP of images (`.cbz`).                  */
  k_mdl_fmt_cbt     = 2,   /**< tar of images (`.cbt`).                  */
  k_mdl_fmt_cbr     = 3,   /**< Reserved; not accepted by the CLI.       */
  k_mdl_fmt_cbt_xz  = 4,   /**< Reserved; not accepted by the CLI.       */
  k_mdl_fmt_cbt_gz  = 5,   /**< gzip-compressed tar (`.cbt.gz`).         */
  k_mdl_fmt_epub    = 6,   /**< EPUB of images (`.epub`).                */
  k_mdl_fmt_jof     = 7,   /**< Native JOF tile atlas per page (`.jof`). */
  k_mdl_fmt_rabook  = 8,   /**< Reserved; not accepted by the CLI.       */
  k_mdl_fmt_invalid = 255, /**< Unrecognised `--format` string.          */
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

/** @brief Sizing constants for metadata fields. */
typedef enum : uint16_t {
  k_mdl_meta_title_max   = 256,  /**< Title (series or chapter) buffer max bytes.   */
  k_mdl_meta_summary_max = 1024, /**< Summary description buffer max bytes.         */
  k_mdl_meta_name_max    = 128,  /**< Person name (writer/artist) buffer max bytes. */
  k_mdl_meta_path_max    = 256,  /**< Cover image path buffer max bytes.            */
  k_mdl_meta_lang_max    = 16,   /**< BCP-47 language tag buffer max bytes.         */
  k_mdl_meta_id_max      = 96,   /**< Stable publication identifier max bytes.      */
  k_mdl_meta_date_max    = 32,   /**< ISO-8601 modified timestamp max bytes.        */
} mdl_meta_size_t;

/** @brief Logical page progression used by fixed-layout readers. */
typedef enum : uint8_t {
  k_mdl_read_ltr = 0, /**< Left-to-right page progression. */
  k_mdl_read_rtl = 1, /**< Right-to-left page progression. */
} mdl_reading_direction_t;

/**
 * @struct mdl_export_meta_t
 * @brief Rich series and chapter metadata for export containers (ComicInfo.xml, EPUB OPF).
 */
typedef struct {
  char   series_title[k_mdl_meta_title_max];  /**< Series title.                            */
  char   summary[k_mdl_meta_summary_max];     /**< Summary / description.                   */
  char   writer[k_mdl_meta_name_max];         /**< Writer / Author.                         */
  char   artist[k_mdl_meta_name_max];         /**< Artist / Illustrator.                    */
  char   chapter_title[k_mdl_meta_title_max]; /**< Chapter title.                           */
  double chapter_number;                      /**< Chapter number (0.0 if unnumbered).      */
  char   cover_path[k_mdl_meta_path_max];     /**< Cover image filename/path.               */
  int    cover_index;                         /**< Cover page index (0-based, -1 if unset). */
  char   language[k_mdl_meta_lang_max];       /**< BCP-47 language tag (default "en").      */
  mdl_reading_direction_t reading_direction;  /**< Fixed-layout page progression.           */
  char identifier[k_mdl_meta_id_max];         /**< Stable identifier; derived when empty.   */
  char modified[k_mdl_meta_date_max];         /**< Deterministic ISO-8601 modified time.    */
} mdl_export_meta_t;

/**
 * @brief Initialise a metadata struct to empty/default values.
 * @param[out] meta Struct to clear (never NULL).
 */
void mdl_meta_init(mdl_export_meta_t* meta);

/**
 * @brief Parse metadata key-value lines or XML text into a metadata struct.
 * @param[in,out] meta Metadata struct to populate.
 * @param[in]     text Key-value string or XML document.
 * @return k_ra8_ok on success, k_ra8_err_invalid_arg if meta or text is NULL.
 */
ra8_err_t mdl_meta_parse(mdl_export_meta_t* meta, const char* text);

/**
 * @brief Load metadata from a directory by looking for metadata files.
 * @param[out] meta Metadata struct to fill.
 * @param[in]  dir  Directory path to inspect.
 * @return k_ra8_ok on success, error code on invalid arg.
 */
ra8_err_t mdl_meta_load_dir(mdl_export_meta_t* meta, const char* dir);

/**
 * @brief Generate ComicInfo.xml content from metadata.
 * @param[in]  meta Metadata struct (or NULL for default metadata).
 * @param[out] buf  Output buffer for XML string.
 * @param[in]  cap  Capacity of @p buf.
 * @return k_ra8_ok on success, error code if buffer too small or NULL arg.
 */
ra8_err_t mdl_export_build_comicinfo(const mdl_export_meta_t* meta, char* buf, size_t cap);

/** @brief Generate ComicInfo.xml including page count/direction semantics. */
ra8_err_t mdl_export_build_comicinfo_pages(const mdl_export_meta_t* meta,
                                           size_t                   page_count,
                                           char*                    buf,
                                           size_t                   cap);

/** @brief Caller-owned bounded arena for all exporter scratch state. */
typedef struct mdl_export_workspace {
  uint8_t* data;       /**< Writable arena bytes.         */
  size_t   cap;        /**< Total arena capacity.         */
  size_t   used;       /**< Current allocation high edge. */
  size_t   high_water; /**< Largest used value observed.  */
} mdl_export_workspace_t;

/** @brief Bind an exporter arena; no allocation occurs. */
void mdl_export_workspace_init(mdl_export_workspace_t* ws, void* data, size_t cap);

/** @brief Reserve aligned bytes from an exporter arena, or NULL when bounded capacity is exhausted. */
void* mdl_export_workspace_take(mdl_export_workspace_t* ws, size_t bytes, size_t alignment);

/** @brief Package a chapter using only caller-owned workspace. */
ra8_err_t mdl_export_chapter_meta_ws(mdl_format_t             fmt,
                                     const char*              chapter_dir,
                                     const char*              out_path,
                                     const mdl_export_meta_t* meta,
                                     mdl_export_workspace_t*  ws);

/** @brief Metadata-autoloading workspace variant. */
ra8_err_t mdl_export_chapter_ws(mdl_format_t            fmt,
                                const char*             chapter_dir,
                                const char*             out_path,
                                mdl_export_workspace_t* ws);

/**
 * @brief Package `chapter_dir`'s contents into `out_path` as `fmt` with rich metadata.
 *
 * @param[in] fmt         Target container (must not be loose/invalid).
 * @param[in] chapter_dir Absolute path to the chapter's page folder.
 * @param[in] out_path    Absolute path of the archive to create.
 * @param[in] meta        Rich metadata (may be NULL).
 *
 * @retval k_ra8_ok               Archive written.
 * @retval k_ra8_err_invalid_arg  NULL/loose/invalid argument.
 * @retval k_ra8_err_invalid_size Over-long page name or cap overflow.
 * @retval k_ra8_err_empty        No page images found in @p chapter_dir.
 * @retval k_ra8_err_not_supported The archiver tool is not on PATH.
 * @retval k_ra8_fail             The archiver ran but failed.
 */
