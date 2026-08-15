/**
 * @file mdl_export.h
 * @brief Package a downloaded chapter folder into a reader-openable container.
 *
 * @details
 * The currently supported writers are CBZ (ZIP), CBT (tar), gzip-wrapped CBT,
 * EPUB, and the native JOF tile atlas. They use in-tree implementations and
 * caller-owned bounded storage so the host tool exercises the same no-heap
 * constraints as portable reader code. CBR, xz-wrapped CBT, and RABOOK enum
 * values remain reserved until in-tree bounded writers and validators exist.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_storage.h"
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
 *
 * @details Recognizes only formats the CLI may currently emit; NULL and the
 *          explicit `loose` token select loose pages, while reserved formats
 *          remain invalid until their writer and validator are available.
 * @param[in] s Format name, or NULL (treated as "loose").
 * @return The matching kind, or ::k_mdl_fmt_invalid.
 * @retval k_mdl_fmt_loose @p s is NULL or names loose output.
 * @retval k_mdl_fmt_invalid @p s is an unknown or reserved format name.
 * @pre @p s is NULL or points to a NUL-terminated string.
 * @pre The caller treats format names as case-sensitive CLI tokens.
 * @post No caller-owned memory is modified.
 * @post The return value is always a member of ::mdl_format_t.
 * @note Thread-safe: this is a pure string classifier.
 * @since 0.1.0
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
 * @pre The caller distinguishes directory output from writer availability.
 * @post No state is mutated (pure classifier).
 * @post Repeated calls with the same value return the same result.
 *
 * @note Thread-safe: pure function of its argument.
 * @see mdl_export_chapter_ws()
 * @since 0.1.0
 */
bool mdl_format_is_dir_output(mdl_format_t fmt);

/** @brief Sizing constants for metadata fields. */
typedef enum : uint16_t {
  k_mdl_meta_title_max   = 256,  /**< Title (series or chapter) buffer max bytes.   */
  k_mdl_meta_summary_max = 1024, /**< Summary description buffer max bytes.         */
  k_mdl_meta_name_max    = 128,  /**< Person name (writer/artist) buffer max bytes. */
  k_mdl_meta_url_max     = 512,  /**< Absolute source URL max bytes, including NUL. */
  k_mdl_meta_path_max    = 1024, /**< Bounded host cover path, including NUL.       */
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
  char   source_url[k_mdl_meta_url_max];      /**< Validated absolute HTTP(S) source URL.   */
  char   cover_path[k_mdl_meta_path_max];     /**< Trusted verified local cover path/name.  */
  int    cover_index;                         /**< Cover page index (0-based, -1 if unset). */
  char   language[k_mdl_meta_lang_max];       /**< BCP-47 language tag (default "en").      */
  mdl_reading_direction_t reading_direction;  /**< Fixed-layout page progression.           */
  char identifier[k_mdl_meta_id_max];         /**< Stable identifier; derived when empty.   */
  char modified[k_mdl_meta_date_max];         /**< Deterministic ISO-8601 modified time.    */
} mdl_export_meta_t;

/**
 * @brief Initialise a metadata struct to empty/default values.
 *
 * @details Clears all textual fields, selects English left-to-right defaults,
 *          and marks the cover index as unset for deterministic later export.
 * @param[out] meta Struct to clear (never NULL).
 * @pre @p meta points to writable storage for one ::mdl_export_meta_t.
 * @pre No other thread accesses @p meta during initialization.
 * @post Every string field is NUL-terminated and initially empty except language.
 * @post Language is `en`, direction is LTR, and cover index is negative.
 * @note Thread-safe across distinct metadata objects.
 * @since 0.1.0
 */
void mdl_meta_init(mdl_export_meta_t* meta);

/**
 * @brief Parse metadata key-value lines or XML text into a metadata struct.
 *
 * @details Accepts the documented aliases and ComicInfo fields, trims text,
 *          rejects malformed numeric/direction values, and refuses any field
 *          that would exceed its fixed destination instead of truncating it.
 * @param[in,out] meta Metadata struct to populate.
 * @param[in]     text Key-value string or XML document.
 * @return k_ra8_ok on success, k_ra8_err_invalid_arg if meta or text is NULL.
 * @retval k_ra8_ok Recognized fields were applied without truncation.
 * @retval k_ra8_err_invalid_arg A pointer or semantic field is invalid.
 * @retval k_ra8_err_invalid_size An input line or field exceeds a fixed bound.
 * @pre @p text is NUL-terminated when non-NULL.
 * @pre @p meta is initialized and exclusively owned during the call.
 * @post On success all populated strings remain NUL-terminated.
 * @post On failure no out-of-bounds write or silent truncation occurs.
 * @note Thread-safe across distinct metadata objects.
 * @since 0.1.0
 */
ra8_err_t mdl_meta_parse(mdl_export_meta_t* meta, const char* text);

/**
 * @brief Load metadata from a directory by looking for metadata files.
 *
 * @details Initializes @p meta, then merges bounded recognized metadata files
 *          in deterministic candidate order and propagates parse overflow or
 *          semantic errors rather than accepting a truncated value.
 * @param[in,out] storage Injected portable file reader.
 * @param[out] meta Metadata struct to fill.
 * @param[in]  dir  Directory path to inspect.
 * @return k_ra8_ok on success, error code on invalid arg.
 * @retval k_ra8_ok No file existed or all discovered files parsed successfully.
 * @retval k_ra8_err_invalid_arg A pointer or discovered semantic value is invalid.
 * @retval k_ra8_err_invalid_size A discovered field exceeds its fixed bound.
 * @pre @p dir is NUL-terminated when non-NULL.
 * @pre @p meta points to writable exclusive storage.
 * @post @p meta contains defaults plus all successfully parsed fields.
 * @post A parse failure is reported instead of silently truncating metadata.
 * @note Thread-safe across distinct metadata objects and stable directories.
 * @since 0.1.0
 */
ra8_err_t mdl_meta_load_dir(mdl_storage_t* storage, mdl_export_meta_t* meta, const char* dir);

/**
 * @brief Generate ComicInfo.xml content from metadata.
 *
 * @details Escapes user text and delegates to the page-aware generator with a
 *          zero page count for callers that only need a metadata document.
 * @param[in]  meta Metadata struct (or NULL for default metadata).
 * @param[out] buf  Output buffer for XML string.
 * @param[in]  cap  Capacity of @p buf.
 * @return k_ra8_ok on success, error code if buffer too small or NULL arg.
 * @retval k_ra8_ok A complete NUL-terminated XML document was written.
 * @retval k_ra8_err_invalid_arg @p buf is NULL or @p cap is zero.
 * @retval k_ra8_err_invalid_size Escaped metadata does not fit @p buf.
 * @pre @p meta is NULL or points to a fully initialized metadata object.
 * @pre @p buf addresses @p cap writable bytes when non-NULL.
 * @post Success leaves a complete NUL-terminated ComicInfo document.
 * @post Failure never reports a truncated document as successful.
 * @note Thread-safe across distinct input and output objects.
 * @since 0.1.0
 */
ra8_err_t mdl_export_build_comicinfo(const mdl_export_meta_t* meta, char* buf, size_t cap);

/**
 * @brief Generate ComicInfo.xml including page count and direction semantics
 *
 * @details Emits escaped metadata, deterministic defaults, page count, manga
 *          direction, and a FrontCover page declaration when its index is valid.
 * @param[in] meta Metadata to encode, or NULL for defaults.
 * @param[in] page_count Logical image count in the target comic archive.
 * @param[out] buf Destination for the NUL-terminated XML document.
 * @param[in] cap Writable capacity of @p buf in bytes.
 * @return XML generation status.
 * @retval k_ra8_ok A complete document was generated.
 * @retval k_ra8_err_invalid_arg @p buf is NULL or @p cap is zero.
 * @retval k_ra8_err_invalid_size Escaped output exceeds @p cap.
 * @pre @p meta is NULL or points to initialized bounded strings.
 * @pre @p buf addresses @p cap writable bytes when non-NULL.
 * @post Success writes one complete NUL-terminated document.
 * @post Failure is explicit and never treated as a valid truncated document.
 * @note Thread-safe across distinct metadata and output buffers.
 * @since 0.1.0
 */
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

/**
 * @brief Bind an exporter arena without allocating memory
 *
 * @details Initializes the caller-owned bump arena and clears usage counters;
 *          a NULL data pointer produces a zero-capacity workspace.
 * @param[out] ws Workspace descriptor to initialize, or NULL for no operation.
 * @param[in,out] data Caller-owned writable arena bytes, or NULL.
 * @param[in] cap Number of bytes available at @p data.
 * @pre @p ws is NULL or points to writable descriptor storage.
 * @pre Non-NULL @p data remains alive while the workspace is used.
 * @post A non-NULL workspace has zero used and high-water counters.
 * @post Capacity is @p cap only when @p data is non-NULL, otherwise zero.
 * @note Thread-safe across distinct workspaces; one workspace is not shareable.
 * @since 0.1.0
 */
void mdl_export_workspace_init(mdl_export_workspace_t* ws, void* data, size_t cap);

/**
 * @brief Reserve aligned bytes from an exporter arena.
 * @details Advances the caller-owned bump cursor only after overflow and
 *          capacity checks and records the largest successfully used offset.
 * @param[in,out] ws Initialized exporter workspace.
 * @param[in] bytes Nonzero number of bytes to reserve.
 * @param[in] alignment Nonzero power-of-two alignment.
 * @return Pointer to aligned caller-owned bytes, or NULL on invalid input/exhaustion.
 * @retval non-NULL A complete aligned reservation was made.
 * @retval NULL Arguments were invalid or bounded capacity was exhausted.
 * @pre @p ws is non-NULL and was initialized by ::mdl_export_workspace_init.
 * @pre @p alignment is a power of two.
 * @post Success advances `used` and may increase `high_water`.
 * @post Failure leaves all workspace counters unchanged.
 * @note Thread-safe across distinct workspaces; one workspace is exclusive.
 * @since 0.1.0
 */
void* mdl_export_workspace_take(mdl_export_workspace_t* ws, size_t bytes, size_t alignment);

/**
 * @brief Package a chapter with explicit metadata and caller-owned workspace
 *
 * @details Enumerates bounded page names, derives deterministic timestamps,
 *          validates any external cover, independently verifies a borrowed
 *          transaction stage, and publishes @p out_path only after the
 *          selected writer succeeds. JOF publishes one validated sibling per
 *          page and therefore reports partial progress on a later-page failure.
 * @param[in,out] storage Injected portable storage used for image classification.
 * @param[in] fmt Target output format.
 * @param[in] chapter_dir Directory containing verified page images.
 * @param[in] out_path Container path, or chapter directory for JOF output.
 * @param[in] meta Metadata to embed, or NULL for deterministic defaults.
 * @param[in,out] ws Exclusive bounded exporter workspace.
 * @return Export status.
 * @retval k_ra8_ok All requested output was completed.
 * @retval k_ra8_err_invalid_arg A pointer, workspace, or format is invalid.
 * @retval k_ra8_err_invalid_size A bound or workspace capacity was exceeded.
 * @retval k_ra8_err_empty No qualifying page image exists.
 * @retval k_ra8_err_validation_failed An external cover is not a recognized image.
 * @retval k_ra8_err_not_supported The reserved writer is unavailable.
 * @retval k_ra8_fail File or container writing failed.
 * @pre @p storage is initialized, exclusive, and all paths are canonical beneath it.
 * @pre String arguments are NUL-terminated and stable for the call.
 * @pre @p ws owns writable storage and is not shared concurrently.
 * @post Success leaves a complete validated container or all requested JOF siblings.
 * @post A pre-publication container failure preserves the prior destination.
 * @post A JOF failure preserves the failed and later siblings while earlier
 *       successfully published pages remain visible.
 * @note Power-loss durability depends on the selected storage adapter's capabilities.
 * @note Not thread-safe for the same workspace or output path.
 * @since 0.1.0
 */
ra8_err_t mdl_export_chapter_meta_ws(mdl_storage_t*           storage,
                                     mdl_format_t             fmt,
                                     const char*              chapter_dir,
                                     const char*              out_path,
                                     const mdl_export_meta_t* meta,
                                     mdl_export_workspace_t*  ws);

/**
 * @brief Package a chapter after auto-loading bounded local metadata
 *
 * @details Loads the supported metadata files from @p chapter_dir and delegates
 *          to ::mdl_export_chapter_meta_ws with the same publication guarantees.
 * @param[in,out] storage Injected portable storage used for image classification.
 * @param[in] fmt Target output format.
 * @param[in] chapter_dir Directory containing pages and optional metadata.
 * @param[in] out_path Container path, or chapter directory for JOF output.
 * @param[in,out] ws Exclusive bounded exporter workspace.
 * @return Metadata-load or export status.
 * @retval k_ra8_ok Output was completed.
 * @retval k_ra8_err_invalid_arg A pointer, workspace, or format is invalid.
 * @retval k_ra8_err_invalid_size Metadata or exporter bounds were exceeded.
 * @retval k_ra8_err_empty No qualifying pages exist.
 * @retval k_ra8_fail File or container writing failed.
 * @pre @p storage is initialized, exclusive, and all paths are canonical beneath it.
 * @pre String arguments are NUL-terminated and stable for the call.
 * @pre @p ws owns writable storage and is not shared concurrently.
 * @post Success leaves complete output for the selected format.
 * @post Failure does not publish a partial container.
 * @note Not thread-safe for the same workspace or output path.
 * @since 0.1.0
 */
ra8_err_t mdl_export_chapter_ws(mdl_storage_t*          storage,
                                mdl_format_t            fmt,
                                const char*             chapter_dir,
                                const char*             out_path,
                                mdl_export_workspace_t* ws);
