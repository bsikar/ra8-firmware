/**
 * @file mdl_export.c
 * @brief Coordinate bounded chapter export and validated publication.
 *
 * @details Owns format selection, page discovery, workspace accounting, and
 * dispatch to the format-specific writer translation units. Each writer keeps
 * its own format rules while this unit preserves one publication policy.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "mdl_export.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "mdl_export_internal.h"
#include "ra8_attributes.h"

/** @brief Mixed-radix civil timestamp geometry used for stable ordering. */
typedef enum : uint8_t {
  k_export_month_radix        = 13U, /**< One-based month field radix. */
  k_export_hours_per_day      = 24U, /**< Civil hours per day.         */
  k_export_minutes_per_hour   = 60U, /**< Civil minutes per hour.      */
  k_export_seconds_per_minute = 60U, /**< Civil seconds per minute.    */
} mdl_export_time_radix_t;

mdl_format_t mdl_format_from_str(const char* s)
{
  if ((s == nullptr) || (strcmp(s, "loose") == 0)) {
    return k_mdl_fmt_loose;
  }
  if (strcmp(s, "cbz") == 0) {
    return k_mdl_fmt_cbz;
  }
  if (strcmp(s, "cbt") == 0) {
    return k_mdl_fmt_cbt;
  }
  if (strcmp(s, "cbt.gz") == 0) {
    return k_mdl_fmt_cbt_gz;
  }
  if (strcmp(s, "epub") == 0) {
    return k_mdl_fmt_epub;
  }
  if (strcmp(s, "jof") == 0) {
    return k_mdl_fmt_jof;
  }
  return k_mdl_fmt_invalid;
}

const char* mdl_format_ext(mdl_format_t fmt)
{
  switch (fmt) {
    case k_mdl_fmt_cbz:
      return "cbz";
    case k_mdl_fmt_cbt:
      return "cbt";
    case k_mdl_fmt_cbr:
      return "cbr";
    case k_mdl_fmt_cbt_xz:
      return "cbt.xz";
    case k_mdl_fmt_cbt_gz:
      return "cbt.gz";
    case k_mdl_fmt_epub:
      return "epub";
    case k_mdl_fmt_jof:
      return "jof";
    case k_mdl_fmt_rabook:
      return "rabook";
    case k_mdl_fmt_loose:
    case k_mdl_fmt_invalid:
    default:
      return "";
  }
}

bool mdl_format_is_dir_output(mdl_format_t fmt)
{
  /* JOF is inherently per-page: one `.jof` band atlas is written beside each
   * source image, so a chapter is a directory of atlases rather than a single
   * container file at out_path. Every other archive format produces one file.
   */
  return fmt == k_mdl_fmt_jof;
}

void mdl_export_workspace_init(mdl_export_workspace_t* ws, void* data, size_t cap)
{
  if (ws == nullptr) {
    return;
  }
  ws->data       = (uint8_t*)data;
  ws->cap        = (data == nullptr) ? 0U : cap;
  ws->used       = 0U;
  ws->high_water = 0U;
}

void* mdl_export_workspace_take(mdl_export_workspace_t* ws, size_t bytes, size_t alignment)
{
  if ((ws == nullptr) || (ws->data == nullptr) || (bytes == 0U) || (alignment == 0U) ||
      ((alignment & (alignment - 1U)) != 0U)) {
    return nullptr;
  }
  const size_t mask = alignment - 1U;
  if (ws->used > (SIZE_MAX - mask)) {
    return nullptr;
  }
  const size_t start = (ws->used + mask) & ~mask;
  if ((start > ws->cap) || (bytes > (ws->cap - start))) {
    return nullptr;
  }
  ws->used = start + bytes;
  if (ws->used > ws->high_water) {
    ws->high_water = ws->used;
  }
  return &ws->data[start];
}
/**
 * @brief Test a filename suffix without ASCII case sensitivity
 * @details Compares only the tail and rejects a suffix longer than the name.
 * @param[in] name NUL-terminated filename.
 * @param[in] suffix NUL-terminated suffix.
 * @return Whether the complete suffix matches.
 * @retval true The suffix matches ignoring ASCII case.
 * @retval false Length or bytes differ.
 * @pre Both inputs are non-NULL and NUL-terminated.
 * @pre Inputs remain stable during the comparison.
 * @post Neither input is modified.
 * @post The result depends only on the inputs.
 * @note Thread-safe: this is a pure predicate.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_ends_with_ci(const char* name, const char* suffix)
{
  const size_t nl = strlen(name);
  const size_t sl = strlen(suffix);
  if (sl > nl) {
    return false;
  }
  const char* tail = name + (nl - sl);
  for (size_t i = 0U; i < sl; ++i) {
    if (tolower((unsigned char)tail[i]) != tolower((unsigned char)suffix[i])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief True if `name` is a raster page image a reader engine can decode.
 *
 * @details
 * Packaging must include ONLY page images. A chapter folder often also holds
 * this tool's own prior output (a sibling `.jof`/`.cbz`, a `.tar.tmp`) or OS
 * junk; folding those into an archive makes the reader choke when it decodes a
 * non-image "page" (the 0x107 that bit re-runs). Filtering by extension keeps
 * packaging idempotent -- re-running any format on a folder is safe.
 * @param[in] name NUL-terminated directory-entry name.
 * @return Whether the suffix names a supported raster page.
 * @retval true A supported raster suffix matched.
 * @retval false The entry is not a page image.
 * @pre @p name is non-NULL and NUL-terminated.
 * @pre Extension classification is sufficient at this enumeration stage.
 * @post @p name is unchanged.
 * @post The result is deterministic for the same name.
 * @note Thread-safe: this is a pure classifier.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_is_page_image(const char* name)
{
  static const char* const k_img_exts[] = {".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp"};
  for (size_t i = 0U; i < (sizeof(k_img_exts) / sizeof(k_img_exts[0])); ++i) {
    if (internal_ends_with_ci(name, k_img_exts[i])) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Sort a bounded page-name table lexically in place.
 * @details Uses stable insertion sort so the caller needs no allocator,
 *          comparator callback, or hidden global sort context.
 * @param[in,out] names Fixed-width table of terminated page filenames.
 * @param[in] count Number of readable and writable rows.
 * @pre @p names is non-NULL and contains @p count complete rows.
 * @pre Every readable row is NUL-terminated within ::k_name_max.
 * @post Rows are in nondecreasing strcmp order.
 * @post The table contains exactly the same names and multiplicities.
 * @note Not thread-safe for a shared name table.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_sort_pages(char names[][k_name_max], size_t count)
{
  for (size_t i = 1U; i < count; ++i) {
    char   held[k_name_max];
    size_t cursor = i;
    memcpy(held, names[i], sizeof(held));
    while ((cursor > 0U) && (strcmp(names[cursor - 1U], held) > 0)) {
      memcpy(names[cursor], names[cursor - 1U], k_name_max);
      --cursor;
    }
    memcpy(names[cursor], held, sizeof(held));
  }
}

/**
 * @brief List and sort regular chapter image entries through a portable cursor
 * @details Uses the injected directory cursor and caller arena, rejects
 *          symlink/nonregular qualifying entries, and closes before sorting.
 * @param[in,out] storage Bound portable filesystem.
 * @param[in] dir Canonical chapter directory.
 * @param[out] names Fixed-width page table.
 * @param[in] cap Available page rows.
 * @param[in,out] ws Export workspace providing the directory backend state.
 * @param[out] out_count Number of page rows written.
 * @return Enumeration, capacity, or close status.
 * @retval k_ra8_ok A complete sorted page table was produced.
 * @retval k_ra8_err_invalid_size A name, count, or workspace bound was exceeded.
 * @pre All pointers are valid and @p ws is exclusively owned.
 * @pre @p names has @p cap writable rows of ::k_name_max bytes.
 * @post Success returns a complete lexically sorted regular-image set.
 * @post Every opened directory cursor is closed and workspace use is restored.
 * @note Not thread-safe against concurrent directory mutation.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_list_pages(mdl_storage_t*          storage,
                                                  const char*             dir,
                                                  char                    names[][k_name_max],
                                                  size_t                  cap,
                                                  mdl_export_workspace_t* ws,
                                                  size_t*                 out_count)
{
  fw_fs_caps_t caps          = {};
  ra8_err_t    err           = fw_fs_get_caps(storage->fs, &caps);
  const size_t floor         = ws->used;
  void*        dir_workspace = nullptr;
  if (err == k_ra8_ok) {
    dir_workspace =
      mdl_export_workspace_take(ws, caps.directory_workspace_bytes, caps.directory_workspace_align);
    if (dir_workspace == nullptr) {
      err = k_ra8_err_invalid_size;
    }
  }
  fw_fs_dir_t directory = {};
  if (err == k_ra8_ok) {
    err = fw_fs_dir_open(&storage->fs->names,
                         dir,
                         &directory,
                         dir_workspace,
                         caps.directory_workspace_bytes);
  }
  size_t count = 0U;
  bool   entry = true;
  while ((err == k_ra8_ok) && entry) {
    fw_fs_dirent_value_t value = {};
    err                        = fw_fs_dir_next(&directory, &value, &entry);
    if ((err == k_ra8_ok) && entry && (value.name[0] != '.') &&
        internal_is_page_image(value.name)) {
      if (value.type != k_fw_fs_node_file) {
        err = k_ra8_err_invalid_arg;
      } else if ((value.name_bytes >= (uint16_t)k_name_max) || (count >= cap)) {
        err = k_ra8_err_invalid_size;
      } else {
        memcpy(names[count], value.name, (size_t)value.name_bytes + 1U);
        ++count;
      }
    }
  }
  if (directory.is_open) {
    const ra8_err_t closed = fw_fs_dir_close(&directory);
    if ((err == k_ra8_ok) && (closed != k_ra8_ok)) {
      err = closed;
    }
  }
  ws->used = floor;
  if (err == k_ra8_ok) {
    internal_sort_pages(names, count);
    *out_count = count;
  }
  return err;
}

/**
 * @brief Map one civil timestamp to a stable ordering key
 * @details Packs validated calendar fields with mixed-radix multiplication so
 *          lexical civil-time order becomes unsigned integer order.
 * @param[in] value Portable civil-time value.
 * @return Monotonic key for valid calendar fields.
 * @retval uint64_t Packed within-volume civil timestamp key.
 * @pre @p value comes from a valid filesystem timestamp.
 * @pre Every calendar field lies within the adapter's documented range.
 * @post No state is changed.
 * @post Equal civil fields produce equal keys on every host architecture.
 * @note UTC offsets are formatted but do not affect this within-volume order.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_timestamp_key(const fw_fs_datetime_t* value)
{
  uint64_t key = value->year;
  key          = (key * k_export_month_radix) + value->month;
  key          = (key * 32U) + value->day;
  key          = (key * k_export_hours_per_day) + value->hour;
  key          = (key * k_export_minutes_per_hour) + value->minute;
  return (key * k_export_seconds_per_minute) + value->second;
}
/**
 * @brief Derive a deterministic UTC modified timestamp from source pages
 * @details Stats every selected page and formats the newest mtime as ISO-8601 UTC.
 * @param[in,out] storage Injected portable metadata reader.
 * @param[in,out] meta Metadata object receiving the timestamp.
 * @param[in] dir NUL-terminated chapter directory.
 * @param[in] names Sorted page-name table.
 * @param[in] count Number of readable name rows.
 * @return Whether every page was statted and formatted.
 * @retval true `meta->modified` contains the derived timestamp.
 * @retval false A path, stat, clock conversion, or formatting step failed.
 * @pre Inputs are non-NULL and every name is NUL-terminated.
 * @pre @p meta is exclusively owned during the call.
 * @post Success writes one NUL-terminated UTC timestamp.
 * @post Failure is visible and never reported as a valid timestamp.
 * @note Not thread-safe against concurrent page replacement.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_metadata_set_page_timestamp(mdl_storage_t*     storage,
                                                              mdl_export_meta_t* meta,
                                                              const char*        dir,
                                                              const char names[][k_name_max],
                                                              size_t     count)
{
  fw_fs_timestamp_t latest     = {};
  uint64_t          latest_key = 0U;
  for (size_t i = 0U; i < count; ++i) {
    char path[k_fw_fs_path_cap];
    if (priv_mdl_export_path_join(path, sizeof(path), dir, names[i]) != k_ra8_ok) {
      return false;
    }
    fw_fs_stat_t stat = {};
    if ((fw_fs_stat(&storage->fs->names, path, &stat) != k_ra8_ok) || !stat.exists ||
        (stat.type != k_fw_fs_node_file)) {
      return false;
    }
    if (stat.modified.valid) {
      const uint64_t key = internal_timestamp_key(&stat.modified.value);
      if (!latest.valid || (key > latest_key)) {
        latest     = stat.modified;
        latest_key = key;
      }
    }
  }
  if (!latest.valid) {
    return snprintf(meta->modified, sizeof(meta->modified), "1970-01-01T00:00:00Z") > 0;
  }
  const fw_fs_datetime_t* value = &latest.value;
  if (latest.utc_offset_valid && (value->utc_offset_min != 0)) {
    const int offset   = value->utc_offset_min;
    const int absolute = (offset < 0) ? -offset : offset;
    return snprintf(meta->modified,
                    sizeof(meta->modified),
                    "%04u-%02u-%02uT%02u:%02u:%02u%c%02d:%02d",
                    value->year,
                    value->month,
                    value->day,
                    value->hour,
                    value->minute,
                    value->second,
                    (offset < 0) ? '-' : '+',
                    absolute / k_export_minutes_per_hour,
                    absolute % k_export_minutes_per_hour) > 0;
  }
  return snprintf(meta->modified,
                  sizeof(meta->modified),
                  "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  value->year,
                  value->month,
                  value->day,
                  value->hour,
                  value->minute,
                  value->second) > 0;
}
/**
 * @brief Dispatch one selected container writer
 * @details Centralizes format-to-writer mapping while preserving the shared
 *          metadata and caller-workspace contract.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] fmt Selected output format.
 * @param[in] dir Chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Page count.
 * @param[in,out] output Active validated-publication output.
 * @param[in] meta Metadata to embed, or NULL.
 * @param[in,out] ws Exclusive caller-owned workspace.
 * @return Selected writer status.
 * @retval k_ra8_ok The selected writer completed.
 * @retval k_ra8_err_invalid_arg The format has no container writer here.
 * @retval k_ra8_err_not_supported An optional writer is unavailable.
 * @retval k_ra8_fail The selected writer failed.
 * @pre Pointer arguments are valid for the selected format.
 * @pre @p ws is exclusive and owns writable arena bytes.
 * @post At most one writer is invoked.
 * @post The selected writer's status is returned unchanged.
 * @note Not thread-safe for shared output or workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_export_dispatch(mdl_storage_t*           storage,
                                                       mdl_format_t             fmt,
                                                       const char*              dir,
                                                       char                     names[][k_name_max],
                                                       size_t                   count,
                                                       mdl_export_output_t*     output,
                                                       const mdl_export_meta_t* meta,
                                                       mdl_export_workspace_t*  ws)
{
  switch (fmt) {
    case k_mdl_fmt_cbz:
      return priv_mdl_export_cbz(storage, dir, names, count, output, meta, ws);
    case k_mdl_fmt_cbt:
      return priv_mdl_export_tar(storage, dir, names, count, output, meta);
    case k_mdl_fmt_cbt_gz:
      return priv_mdl_export_tar_gzip(storage, dir, names, count, output, meta, ws);
    case k_mdl_fmt_epub:
      return priv_mdl_export_epub(storage, dir, names, count, output, meta, ws);
    case k_mdl_fmt_cbr:
    case k_mdl_fmt_cbt_xz:
    case k_mdl_fmt_rabook:
      return k_ra8_err_not_supported;
    case k_mdl_fmt_jof:
    case k_mdl_fmt_loose:
    case k_mdl_fmt_invalid:
    default:
      return k_ra8_err_invalid_arg;
  }
}

/**
 * @brief Build `out_path` through one storage transaction.
 * @details The single publication seam for every container format, rather than
 *          making each writer remember it. A re-export that fails part-way because
 *          caller storage is exhausted, metadata is invalid, or output I/O
 *          fails cannot truncate the previously-good archive: the destination
 *          is untouched until a complete, independently verified stage exists.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] fmt Selected output format.
 * @param[in] dir Chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Page count.
 * @param[in] out_path Final destination path.
 * @param[in] meta Metadata to embed, or NULL.
 * @param[in,out] ws Exclusive caller-owned workspace.
 * @return Validated-publication status.
 * @retval k_ra8_ok The completed stage was published.
 * @retval k_ra8_fail Stage reservation, writer, verification, or commit failed.
 * @retval k_ra8_err_invalid_size A selected writer exceeded a bound.
 * @retval k_ra8_err_validation_failed Cover or container validation failed.
 * @pre Paths and page rows are valid and stable.
 * @pre @p ws is exclusive and remains alive through commit.
 * @post Failure aborts the reserved temp and preserves prior output.
 * @post Success publishes exactly one complete container.
 * @note Power-loss durability depends on the storage adapter's capabilities.
 * @note Not thread-safe for the same destination or workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_export_transaction(mdl_storage_t* storage,
                                                          mdl_format_t   fmt,
                                                          const char*    dir,
                                                          char           names[][k_name_max],
                                                          size_t         count,
                                                          const char*    out_path,
                                                          const mdl_export_meta_t* meta,
                                                          mdl_export_workspace_t*  ws)
{
  mdl_export_output_t output = {};
  ra8_err_t           rc     = priv_mdl_export_output_begin(&output, storage, out_path, fmt);
  if (rc == k_ra8_ok) {
    rc = internal_export_dispatch(storage, fmt, dir, names, count, &output, meta, ws);
  }
  if ((rc != k_ra8_ok) && output.writer.transaction.active) {
    const ra8_err_t aborted = priv_mdl_export_output_abort(&output);
    return (aborted == k_ra8_ok) ? rc : aborted;
  }
  bool published = false;
  return (rc == k_ra8_ok) ? priv_mdl_export_output_commit(&output, ws, &published) : rc;
}

ra8_err_t mdl_export_chapter_meta_ws(mdl_storage_t*           storage,
                                     mdl_format_t             fmt,
                                     const char*              chapter_dir,
                                     const char*              out_path,
                                     const mdl_export_meta_t* meta,
                                     mdl_export_workspace_t*  ws)
{
  if ((storage == nullptr) || (chapter_dir == nullptr) || (out_path == nullptr) ||
      (fmt == k_mdl_fmt_loose) || (fmt == k_mdl_fmt_invalid) || (ws == nullptr) ||
      (ws->data == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  ws->used = 0U;
  if ((fmt == k_mdl_fmt_cbr) || (fmt == k_mdl_fmt_cbt_xz) || (fmt == k_mdl_fmt_rabook)) {
    return k_ra8_err_not_supported;
  }

  ws->high_water            = 0U;
  char (*names)[k_name_max] = mdl_export_workspace_take(ws,
                                                        (size_t)k_max_pages * (size_t)k_name_max,
                                                        _Alignof(char[k_name_max]));
  if (names == nullptr) {
    return k_ra8_err_invalid_size;
  }

  size_t          count = 0U;
  const ra8_err_t listed =
    internal_list_pages(storage, chapter_dir, names, (size_t)k_max_pages, ws, &count);
  if (listed != k_ra8_ok) {
    return listed;
  }
  if (count == 0U) {
    return k_ra8_err_empty;
  }
  mdl_export_meta_t resolved;
  if (meta != nullptr) {
    resolved = *meta;
  } else {
    mdl_meta_init(&resolved);
  }
  const ra8_err_t source_rc = priv_mdl_export_validate_source_url(resolved.source_url);
  if (source_rc != k_ra8_ok) {
    return source_rc;
  }
  if ((resolved.modified[0] == '\0') &&
      !internal_metadata_set_page_timestamp(storage, &resolved, chapter_dir, names, count)) {
    return k_ra8_fail;
  }

  if (fmt == k_mdl_fmt_jof) {
    /* JOF writes one `.jof` sibling per page into chapter_dir; out_path names
     * no single container (see mdl_format_is_dir_output), so there is no single
     * file to rename into place -- priv_mdl_export_jof commits each page itself. */
    return priv_mdl_export_jof(storage, chapter_dir, names, count, ws);
  }
  return internal_export_transaction(storage,
                                     fmt,
                                     chapter_dir,
                                     names,
                                     count,
                                     out_path,
                                     &resolved,
                                     ws);
}

ra8_err_t mdl_export_chapter_ws(mdl_storage_t*          storage,
                                mdl_format_t            fmt,
                                const char*             chapter_dir,
                                const char*             out_path,
                                mdl_export_workspace_t* ws)
{
  mdl_export_meta_t meta;
  if (chapter_dir != nullptr) {
    (void)mdl_meta_load_dir(storage, &meta, chapter_dir);
  } else {
    mdl_meta_init(&meta);
  }
  return mdl_export_chapter_meta_ws(storage, fmt, chapter_dir, out_path, &meta, ws);
}
