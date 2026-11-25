/**
 * @file ra8_viewer_reader.c
 * @brief Requirements, workspace binding, source lifecycle, and engine dispatch.
 * @details Implements the device-portable JOF, comic, and reflow requirements-to-
 * bind lifecycle over caller-owned memory and injected ::ra8_fmt_source_t
 * callbacks, leaving POSIX descriptors in `ra8_viewer_host_fd.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_viewer_reader.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "jof.h"
#include "ra8_attributes.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "ra8_fmt_io.h"
#include "ra8_viewer_reader_internal.h"

/** @brief Mutable cursor used to calculate or bind one aligned layout. */
typedef struct {
  uint8_t* base;     /**< Workspace base, or NULL during sizing. */
  size_t   capacity; /**< Accessible extent during binding.      */
  size_t   used;     /**< First unused byte.                     */
  bool     valid;    /**< False after overflow/capacity failure. */
} viewer_layout_t;

/**
 * @brief Align @p value upward with overflow rejection.
 * @details Accepts power-of-two alignments and checks the rounding addition.
 * @param[in] value Unaligned extent.
 * @param[in] alignment Required alignment.
 * @param[out] out Aligned result.
 * @return Whether rounding succeeded.
 * @retval true @p out is populated.
 * @retval false Alignment was invalid or overflowed.
 * @pre @p out is writable.
 * @pre @p value is a workspace-relative extent.
 * @post Success publishes a result no smaller than @p value.
 * @post Failure mutates no workspace bytes.
 * @note Pure apart from @p out.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_align_up(size_t value, size_t alignment, size_t* out)
{
  const size_t mask = alignment - 1U;
  if ((alignment == 0U) || ((alignment & mask) != 0U) || (value > (SIZE_MAX - mask))) {
    return false;
  }
  *out = (value + mask) & ~mask;
  return true;
}

/**
 * @brief Take one aligned layout slice, or only charge it while sizing.
 * @details Advances the cursor after checked alignment and extent arithmetic.
 * @param[in,out] layout Mutable layout cursor.
 * @param[in] bytes Requested slice extent.
 * @param[in] alignment Required power-of-two alignment.
 * @return Slice base in binding mode, otherwise NULL.
 * @retval non-NULL The requested bound slice is available.
 * @retval NULL Sizing mode is active or the request failed.
 * @pre @p layout is non-NULL.
 * @pre @p layout was initialized by the caller.
 * @post Failure marks the cursor invalid.
 * @post Success advances used to the end of the slice.
 * @note Sizing mode charges space without touching workspace bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_take(viewer_layout_t* layout, size_t bytes, size_t alignment)
{
  size_t start = 0U;
  if (!layout->valid || !internal_align_up(layout->used, alignment, &start) ||
      (start > SIZE_MAX - bytes)) {
    layout->valid = false;
    return nullptr;
  }
  const size_t end = start + bytes;
  if ((layout->base != nullptr) && (end > layout->capacity)) {
    layout->valid = false;
    return nullptr;
  }
  void* result = (layout->base == nullptr) ? nullptr : &layout->base[start];
  layout->used = end;
  return result;
}

/**
 * @brief Take the complete JOF-specific workspace tail.
 * @details Charges or binds the one-cell cache metadata and compressed scratch.
 * @param[in,out] layout Active sizing or binding cursor.
 * @param[in] requirements Valid JOF requirements.
 * @return JOF pointer bundle; pointers are NULL in sizing mode.
 * @retval viewer_jof_t The complete bound or sizing-mode bundle.
 * @pre @p layout and @p requirements are valid.
 * @pre `requirements->engine == k_ra8_viewer_engine_jof`.
 * @post The cursor advances through every JOF-specific slice.
 * @post Failure marks the cursor invalid.
 * @note Performs no workspace writes.
 * @since 0.1.0
 */
RA8_INTERNAL static viewer_jof_t
internal_take_jof(viewer_layout_t* layout, const ra8_viewer_reader_requirements_t* requirements)
{
  viewer_jof_t jof = {};
  jof.cells = (uint8_t*)internal_take(layout, requirements->cell_bytes, alignof(max_align_t));
  jof.meta  = (ra8_keycache_cell_t*)internal_take(layout,
                                                  sizeof(ra8_keycache_cell_t),
                                                  alignof(ra8_keycache_cell_t));
  jof.keys =
    (ra8_tile_key_t*)internal_take(layout, sizeof(ra8_tile_key_t), alignof(ra8_tile_key_t));
  jof.dims =
    (ra8_tile_dims_t*)internal_take(layout, sizeof(ra8_tile_dims_t), alignof(ra8_tile_dims_t));
  jof.buckets  = (int32_t*)internal_take(layout,
                                         (size_t)k_viewer_jof_buckets * sizeof(int32_t),
                                         alignof(int32_t));
  jof.scratch  = (uint8_t*)internal_take(layout, requirements->scratch_bytes, alignof(max_align_t));
  jof.cell_cap = requirements->cell_bytes;
  jof.scratch_cap = requirements->scratch_bytes;
  return jof;
}

/**
 * @brief Take the complete comic-specific workspace tail.
 * @details Charges or binds page index, names, encoded-page, and decode-arena slices.
 * @param[in,out] layout Active sizing or binding cursor.
 * @param[in] requirements Valid comic requirements.
 * @return Comic pointer bundle; pointers are NULL in sizing mode.
 * @retval viewer_comic_t The complete bound or sizing-mode bundle.
 * @pre @p layout and @p requirements are valid.
 * @pre `requirements->engine == k_ra8_viewer_engine_comic`.
 * @post The cursor advances through every comic-specific slice.
 * @post Failure marks the cursor invalid.
 * @note Performs no workspace writes.
 * @since 0.1.0
 */
RA8_INTERNAL static viewer_comic_t
internal_take_comic(viewer_layout_t* layout, const ra8_viewer_reader_requirements_t* requirements)
{
  viewer_comic_t comic = {};
  comic.pages =
    (comic_page_t*)internal_take(layout, requirements->comic_pages_bytes, alignof(comic_page_t));
  comic.names = (char*)internal_take(layout, requirements->comic_names_bytes, alignof(char));
  comic.page =
    (uint8_t*)internal_take(layout, requirements->comic_page_bytes, alignof(max_align_t));
  comic.arena =
    (uint8_t*)internal_take(layout, requirements->comic_arena_bytes, alignof(max_align_t));
  comic.page_cap  = requirements->comic_page_bytes;
  comic.arena_cap = requirements->comic_arena_bytes;
  return comic;
}

/**
 * @brief Carve fixed-size reflow slices from the validated workspace.
 * @details Advances @p layout in alignment order identical to
 * ::internal_layout for the reflow engine branch.
 * @param[in,out] layout Workspace cursor.
 * @param[in] requirements Validated reflow requirements object.
 * @return Populated reflow slice pointers and capacities.
 * @pre @p layout holds enough remaining space for every requested slice.
 * @post Layout cursor advances across all reflow buffers.
 * @note Pure pointer carving; acquires no memory.
 * @since 0.1.0
 */
RA8_INTERNAL static viewer_reflow_t
internal_take_reflow(viewer_layout_t* layout, const ra8_viewer_reader_requirements_t* requirements)
{
  viewer_reflow_t reflow = {.loaded_chapter = UINT32_MAX};
  reflow.epub =
    (epub_book_t*)internal_take(layout, requirements->reflow_epub_bytes, alignof(epub_book_t));
  reflow.engine =
    (reflow_t*)internal_take(layout, requirements->reflow_engine_bytes, alignof(reflow_t));
  reflow.pages = (viewer_reflow_page_t*)internal_take(layout,
                                                      requirements->reflow_pages_bytes,
                                                      alignof(viewer_reflow_page_t));
  reflow.xhtml =
    (uint8_t*)internal_take(layout, requirements->reflow_xhtml_bytes, alignof(max_align_t));
  reflow.file_buf =
    (uint8_t*)internal_take(layout, requirements->reflow_file_bytes, alignof(max_align_t));
  reflow.book_buf =
    (uint8_t*)internal_take(layout, requirements->reflow_book_bytes, alignof(max_align_t));
  reflow.xhtml_cap = requirements->reflow_xhtml_bytes;
  reflow.file_cap  = requirements->reflow_file_bytes;
  reflow.book_cap  = requirements->reflow_book_bytes;
  return reflow;
}

/**
 * @brief Test one case-insensitive ASCII filename suffix.
 * @details Folds only ASCII uppercase characters in the path tail.
 * @param[in] path NUL-terminated path.
 * @param[in] suffix Lowercase NUL-terminated suffix.
 * @return Whether the suffix matches.
 * @retval true The path ends with @p suffix ignoring ASCII case.
 * @retval false It is shorter or differs.
 * @pre @p path is non-NULL and terminated.
 * @pre @p suffix is non-NULL, lowercase, and terminated.
 * @post No state is mutated.
 * @post The result depends only on inputs.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_ends_with(const char* path, const char* suffix)
{
  const size_t path_len   = strlen(path);
  const size_t suffix_len = strlen(suffix);
  if (path_len < suffix_len) {
    return false;
  }
  const char* tail = &path[path_len - suffix_len];
  for (size_t index = 0U; index < suffix_len; ++index) {
    char value = tail[index];
    if ((value >= 'A') && (value <= 'Z')) {
      value = (char)(value + ('a' - 'A'));
    }
    if (value != suffix[index]) {
      return false;
    }
  }
  return true;
}

ra8_viewer_format_t ra8_viewer_classify_path(const char* path)
{
  if (path == nullptr) {
    return k_ra8_viewer_format_unknown;
  }
  if (internal_ends_with(path, ".jof")) {
    return k_ra8_viewer_format_jof;
  }
  if (internal_ends_with(path, ".cbz") || internal_ends_with(path, ".cbr") ||
      internal_ends_with(path, ".cbt")) {
    return k_ra8_viewer_format_comic;
  }
  if (internal_ends_with(path, ".gz") || internal_ends_with(path, ".xz")) {
    return k_ra8_viewer_format_comic_wrap;
  }
  if (internal_ends_with(path, ".epub") || internal_ends_with(path, ".epb")) {
    return k_ra8_viewer_format_epub;
  }
  if (internal_ends_with(path, ".rabook") || internal_ends_with(path, ".rbk")) {
    return k_ra8_viewer_format_rabook;
  }
  return k_ra8_viewer_format_unknown;
}

ra8_err_t
priv_viewer_pread(void* ctx, uint64_t offset, uint8_t* buffer, size_t length, size_t* out_read)
{
  const ra8_fmt_source_t* source = (const ra8_fmt_source_t*)ctx;
  if (out_read == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_read = 0U;
  if ((source == nullptr) || (source->read_at == nullptr) ||
      ((buffer == nullptr) && (length != 0U))) {
    return k_ra8_err_null_ptr;
  }
  if ((offset >= source->size) || (length == 0U)) {
    return k_ra8_ok;
  }
  const uint64_t available = source->size - offset;
  const size_t   requested = ((uint64_t)length > available) ? (size_t)available : length;
  size_t         completed = 0U;
  while (completed < requested) {
    size_t          pass  = 0U;
    const ra8_err_t error = source->read_at(source->ctx,
                                            offset + (uint64_t)completed,
                                            &buffer[completed],
                                            requested - completed,
                                            &pass);
    if (error != k_ra8_ok) {
      return error;
    }
    if (pass == 0U) {
      break;
    }
    if (pass > (requested - completed)) {
      return k_ra8_err_invalid_size;
    }
    completed += pass;
  }
  *out_read = completed;
  return k_ra8_ok;
}

/**
 * @brief Charge the complete deterministic reader layout.
 * @details Walks common state then the selected JOF or comic engine tail.
 * @param[in] need Valid requirements fields.
 * @param[in,out] base Backing base, or NULL for sizing only.
 * @param[in] capacity Accessible extent when @p base is non-NULL.
 * @return Completed layout cursor.
 * @retval viewer_layout_t Cursor with valid false on any failure.
 * @pre @p need is non-NULL.
 * @pre @p base is aligned when non-NULL.
 * @post Sizing mode mutates no bytes.
 * @post Binding mode only calculates addresses; callers publish later.
 * @note Pure in sizing mode.
 * @since 0.1.0
 */
RA8_INTERNAL static viewer_layout_t
internal_layout(const ra8_viewer_reader_requirements_t* need, uint8_t* base, size_t capacity)
{
  viewer_layout_t layout = {.base = base, .capacity = capacity, .used = 0U, .valid = true};
  (void)internal_take(&layout, sizeof(ra8_viewer_reader_t), alignof(max_align_t));
  (void)internal_take(&layout, need->framebuffer_bytes, alignof(uint16_t));
  const size_t dimension_bytes = need->dimensions_bytes / 2U;
  (void)internal_take(&layout, dimension_bytes, alignof(uint32_t));
  (void)internal_take(&layout, dimension_bytes, alignof(uint32_t));
  if (need->engine == (uint32_t)k_ra8_viewer_engine_jof) {
    (void)internal_take_jof(&layout, need);
  } else if (need->engine == (uint32_t)k_ra8_viewer_engine_comic) {
    (void)internal_take_comic(&layout, need);
  } else if (need->engine == (uint32_t)k_ra8_viewer_engine_reflow) {
    (void)internal_take_reflow(&layout, need);
  } else {
    layout.valid = false;
  }
  return layout;
}

/**
 * @brief Return whether every reflow slice requirement is zero.
 * @param[in] need Candidate requirements object.
 * @return True if all six reflow extent fields are zero.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_reflow_fields_zero(const ra8_viewer_reader_requirements_t* need)
{
  return (need->reflow_epub_bytes == 0U) && (need->reflow_engine_bytes == 0U) &&
         (need->reflow_pages_bytes == 0U) && (need->reflow_xhtml_bytes == 0U) &&
         (need->reflow_file_bytes == 0U) && (need->reflow_book_bytes == 0U);
}

/**
 * @brief Validate engine-specific requirement fields.
 * @details Requires exact fixed capacities or mutually exclusive JOF slices.
 * @param[in] need Candidate requirements object.
 * @return Whether its selected engine fields are canonical.
 * @retval true The engine and all exclusive fields match policy.
 * @retval false The object is stale, forged, or internally inconsistent.
 * @pre @p need is non-NULL.
 * @pre Common fields are checked separately.
 * @post No state is mutated.
 * @post Exactly one engine shape can pass.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_engine_requirements_valid(const ra8_viewer_reader_requirements_t* need)
{
  if (need->engine == (uint32_t)k_ra8_viewer_engine_jof) {
    return (need->cell_bytes != 0U) && (need->scratch_bytes != 0U) &&
           (need->comic_pages_bytes == 0U) && (need->comic_names_bytes == 0U) &&
           (need->comic_page_bytes == 0U) && (need->comic_arena_bytes == 0U) &&
           internal_reflow_fields_zero(need);
  }
  if (need->engine == (uint32_t)k_ra8_viewer_engine_comic) {
    return (need->cell_bytes == 0U) && (need->scratch_bytes == 0U) &&
           (need->tile_count == (uint32_t)k_viewer_comic_page_cap) &&
           (need->comic_pages_bytes == ((size_t)k_viewer_comic_page_cap * sizeof(comic_page_t))) &&
           (need->comic_names_bytes == (size_t)k_viewer_comic_name_bytes) &&
           (need->comic_page_bytes == (size_t)k_viewer_comic_page_bytes) &&
           (need->comic_arena_bytes == (size_t)k_viewer_comic_arena_bytes) &&
           internal_reflow_fields_zero(need);
  }
  if (need->engine == (uint32_t)k_ra8_viewer_engine_reflow) {
    return (need->cell_bytes == 0U) && (need->scratch_bytes == 0U) &&
           (need->comic_pages_bytes == 0U) && (need->comic_names_bytes == 0U) &&
           (need->comic_page_bytes == 0U) && (need->comic_arena_bytes == 0U) &&
           (need->tile_count == (uint32_t)k_viewer_reflow_page_cap) &&
           (need->reflow_epub_bytes == sizeof(epub_book_t)) &&
           (need->reflow_engine_bytes == sizeof(reflow_t)) &&
           (need->reflow_pages_bytes ==
            ((size_t)k_viewer_reflow_page_cap * sizeof(viewer_reflow_page_t))) &&
           (need->reflow_xhtml_bytes == (size_t)k_viewer_reflow_xhtml_bytes) &&
           (need->reflow_file_bytes == (size_t)k_viewer_reflow_file_bytes) &&
           (need->reflow_book_bytes == (size_t)k_viewer_reflow_book_bytes);
  }
  return false;
}

/**
 * @brief Validate a requirements object before workspace mutation.
 * @details Checks ABI, fixed extents, non-zero variable slices, and recomputed total.
 * @param[in] need Candidate requirements.
 * @return Whether bind may consume the layout.
 * @retval true Every invariant and exact total matched.
 * @retval false The object was stale, corrupt, or inconsistent.
 * @pre @p need is non-NULL.
 * @pre @p need was supplied to a bind request.
 * @post No workspace byte is modified.
 * @post The result is deterministic for @p need.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_requirements_valid(const ra8_viewer_reader_requirements_t* need)
{
  if ((need->layout_version != (uint32_t)k_viewer_layout_version) ||
      (need->required_alignment != alignof(max_align_t)) || (need->tile_count == 0U) ||
      (need->framebuffer_bytes !=
       ((size_t)k_ra8_viewer_fb_width * (size_t)k_ra8_viewer_fb_height * sizeof(uint16_t))) ||
      (need->dimensions_bytes != ((size_t)need->tile_count * 2U * sizeof(uint32_t))) ||
      !internal_engine_requirements_valid(need)) {
    return false;
  }
  const viewer_layout_t layout = internal_layout(need, nullptr, 0U);
  return layout.valid && (layout.used == need->required_bytes);
}

/**
 * @brief Populate JOF-specific requirements from one positioned source.
 * @details Parses geometry, validates the declared decoded band, and sizes one
 * cache cell plus the exact compressed-band staging bound.
 * @param[in] source Active positioned JOF source.
 * @param[out] out Requirements object with common fields initialized.
 * @return Parse or geometry status.
 * @retval k_ra8_ok Canonical JOF requirements were published.
 * @retval k_ra8_err_invalid_size Decoded band geometry is invalid.
 * @pre @p source is non-empty and valid.
 * @pre @p out is writable and initialized.
 * @post Success publishes a canonical JOF engine shape.
 * @post Failure acquires no memory.
 * @note The caller retains source ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_require_jof(const ra8_fmt_source_t*           source,
                                                   ra8_viewer_reader_requirements_t* out)
{
  ra8_fmt_source_t bounded = *source;
  jof_info_t       info    = {};
  ra8_err_t        error   = jof_parse(priv_viewer_pread, &bounded, bounded.size, &info);
  const uint64_t   band    = (uint64_t)info.tile_w * (uint64_t)info.tile_h * (uint64_t)info.bpp;
  if ((error == k_ra8_ok) && (band == 0U)) {
    error = k_ra8_err_invalid_size;
  }
  const ra8_decomp_limits_t limits = ra8_decomp_limits_default();
  if (error == k_ra8_ok) {
    error = ra8_decomp_check_declared(&limits, bounded.size, band);
  }
  if (error != k_ra8_ok) {
    return error;
  }
  const uint32_t tiles  = ((uint32_t)info.height + (uint32_t)k_ra8_viewer_fb_height - 1U) /
                          (uint32_t)k_ra8_viewer_fb_height;
  out->dimensions_bytes = (size_t)tiles * 2U * sizeof(uint32_t);
  out->cell_bytes       = (size_t)band;
  out->scratch_bytes =
    (info.codec == (uint8_t)k_jof_codec_raw) ? 1U : (size_t)jof_stored_bound((uint32_t)band);
  out->tile_count = tiles;
  out->engine     = (uint32_t)k_ra8_viewer_engine_jof;
  return k_ra8_ok;
}

/**
 * @brief Populate the fixed bounded bare-comic requirements shape.
 * @details Reserves maximum page metadata plus explicit encoded/decode slices;
 * archive parsing happens only after those caller-owned regions are bound.
 * @param[out] out Requirements object with common fields initialized.
 * @pre @p out is writable and zero-initialized.
 * @pre Fixed comic capacities fit size_t.
 * @post A canonical comic engine shape is published.
 * @post No file bytes or workspace bytes are touched.
 * @note Pure apart from @p out.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_require_comic(ra8_viewer_reader_requirements_t* out)
{
  out->tile_count        = (uint32_t)k_viewer_comic_page_cap;
  out->dimensions_bytes  = (size_t)out->tile_count * 2U * sizeof(uint32_t);
  out->comic_pages_bytes = (size_t)out->tile_count * sizeof(comic_page_t);
  out->comic_names_bytes = (size_t)k_viewer_comic_name_bytes;
  out->comic_page_bytes  = (size_t)k_viewer_comic_page_bytes;
  out->comic_arena_bytes = (size_t)k_viewer_comic_arena_bytes;
  out->engine            = (uint32_t)k_ra8_viewer_engine_comic;
}

/**
 * @brief Populate fixed reflow requirements for EPUB and RABOOK books.
 * @param[out] out Partially populated requirements struct.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_require_reflow(ra8_viewer_reader_requirements_t* out)
{
  out->tile_count          = (uint32_t)k_viewer_reflow_page_cap;
  out->dimensions_bytes    = (size_t)out->tile_count * 2U * sizeof(uint32_t);
  out->reflow_epub_bytes   = sizeof(epub_book_t);
  out->reflow_engine_bytes = sizeof(reflow_t);
  out->reflow_pages_bytes  = (size_t)out->tile_count * sizeof(viewer_reflow_page_t);
  out->reflow_xhtml_bytes  = (size_t)k_viewer_reflow_xhtml_bytes;
  out->reflow_file_bytes   = (size_t)k_viewer_reflow_file_bytes;
  out->reflow_book_bytes   = (size_t)k_viewer_reflow_book_bytes;
  out->engine              = (uint32_t)k_ra8_viewer_engine_reflow;
}

ra8_err_t ra8_viewer_reader_requirements_source(ra8_viewer_format_t               format,
                                                const ra8_fmt_source_t*           source,
                                                ra8_viewer_reader_requirements_t* out)
{
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out = (ra8_viewer_reader_requirements_t){};
  if ((source == nullptr) || (source->read_at == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((format != k_ra8_viewer_format_jof) && (format != k_ra8_viewer_format_comic) &&
      (format != k_ra8_viewer_format_epub) && (format != k_ra8_viewer_format_rabook)) {
    return k_ra8_err_not_supported;
  }
  if (source->size == 0U) {
    return k_ra8_err_invalid_size;
  }
  out->required_alignment = alignof(max_align_t);
  out->framebuffer_bytes =
    (size_t)k_ra8_viewer_fb_width * (size_t)k_ra8_viewer_fb_height * sizeof(uint16_t);
  out->layout_version = (uint32_t)k_viewer_layout_version;
  ra8_err_t error     = k_ra8_ok;
  if (format == k_ra8_viewer_format_jof) {
    error = internal_require_jof(source, out);
  } else if (format == k_ra8_viewer_format_comic) {
    internal_require_comic(out);
  } else {
    internal_require_reflow(out);
  }
  if (error != k_ra8_ok) {
    *out = (ra8_viewer_reader_requirements_t){};
    return error;
  }
  const viewer_layout_t layout = internal_layout(out, nullptr, 0U);
  if (!layout.valid) {
    *out = (ra8_viewer_reader_requirements_t){};
    return k_ra8_err_invalid_size;
  }
  out->required_bytes = layout.used;
  return k_ra8_ok;
}

ra8_err_t ra8_viewer_reader_bind(ra8_viewer_reader_t**                   out,
                                 void*                                   workspace,
                                 size_t                                  workspace_bytes,
                                 const ra8_viewer_reader_requirements_t* requirements,
                                 ra8_viewer_workspace_report_t*          report)
{
  if ((out == nullptr) || (requirements == nullptr) || (report == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out    = nullptr;
  *report = (ra8_viewer_workspace_report_t){.required_bytes = requirements->required_bytes,
                                            .supplied_bytes = workspace_bytes};
  if ((workspace == nullptr) || !internal_requirements_valid(requirements) ||
      (((uintptr_t)workspace % requirements->required_alignment) != 0U) ||
      (workspace_bytes < requirements->required_bytes)) {
    return k_ra8_err_invalid_size;
  }
  viewer_layout_t      layout = {.base     = (uint8_t*)workspace,
                                 .capacity = workspace_bytes,
                                 .used     = 0U,
                                 .valid    = true};
  ra8_viewer_reader_t* reader =
    (ra8_viewer_reader_t*)internal_take(&layout, sizeof(*reader), alignof(max_align_t));
  uint16_t* framebuffer =
    (uint16_t*)internal_take(&layout, requirements->framebuffer_bytes, alignof(uint16_t));
  const size_t    dimension_bytes = requirements->dimensions_bytes / 2U;
  uint32_t*       widths  = (uint32_t*)internal_take(&layout, dimension_bytes, alignof(uint32_t));
  uint32_t*       heights = (uint32_t*)internal_take(&layout, dimension_bytes, alignof(uint32_t));
  viewer_jof_t    jof     = {};
  viewer_comic_t  comic   = {};
  viewer_reflow_t reflow  = {};
  if (requirements->engine == (uint32_t)k_ra8_viewer_engine_jof) {
    jof = internal_take_jof(&layout, requirements);
  } else if (requirements->engine == (uint32_t)k_ra8_viewer_engine_comic) {
    comic = internal_take_comic(&layout, requirements);
  } else {
    reflow = internal_take_reflow(&layout, requirements);
  }
  if (!layout.valid || (layout.used != requirements->required_bytes)) {
    return k_ra8_err_invalid_size;
  }
  *reader = (ra8_viewer_reader_t){.host_file = {.fd = -1},
                                  .is_bound  = true,
                                  .engine    = requirements->engine,
                                  .limits    = ra8_decomp_limits_default(),
                                  .fb        = framebuffer,
                                  .tile_wpx  = widths,
                                  .tile_hpx  = heights,
                                  .tile_cap  = requirements->tile_count,
                                  .rt565     = framebuffer,
                                  .rt_w      = (uint32_t)k_ra8_viewer_fb_width,
                                  .rt_h      = (uint32_t)k_ra8_viewer_fb_height,
                                  .jof       = jof,
                                  .comic     = comic,
                                  .reflow    = reflow};
  *out    = reader;
  return k_ra8_ok;
}

/**
 * @brief Open and size the reader's selected engine.
 * @details Dispatches only from the immutable engine recorded at bind time and
 * populates the common tile count/dimension arrays before publication.
 * @param[in,out] reader Bound reader with an open descriptor.
 * @return Engine open, geometry, or capacity status.
 * @retval k_ra8_ok The selected engine and all tile dimensions are ready.
 * @retval k_ra8_err_invalid_state The bound engine is unknown.
 * @pre `reader->file` is open and matches the bound engine's path class.
 * @pre Every engine-specific workspace slice is bound.
 * @post Success publishes a non-zero tile count no greater than tile capacity.
 * @post Failure leaves `reader->is_open` false.
 * @note Not thread-safe; drives the selected parser.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_open_selected(ra8_viewer_reader_t* reader)
{
  ra8_err_t error = k_ra8_err_invalid_state;
  if (reader->engine == (uint32_t)k_ra8_viewer_engine_jof) {
    error = priv_viewer_open_jof(reader);
    if (error == k_ra8_ok) {
      reader->tile_n =
        ((uint32_t)reader->jof.dctx.info.height + (uint32_t)k_ra8_viewer_fb_height - 1U) /
        (uint32_t)k_ra8_viewer_fb_height;
    }
    if ((error == k_ra8_ok) && ((reader->tile_n == 0U) || (reader->tile_n > reader->tile_cap))) {
      error = k_ra8_err_invalid_size;
    }
    if (error == k_ra8_ok) {
      priv_viewer_size_jof_tiles(reader, reader->tile_n);
    }
  } else if (reader->engine == (uint32_t)k_ra8_viewer_engine_comic) {
    error = priv_viewer_open_comic(reader);
    if (error == k_ra8_ok) {
      reader->tile_n = comic_page_count(&reader->comic.archive);
    }
    if ((error == k_ra8_ok) && ((reader->tile_n == 0U) || (reader->tile_n > reader->tile_cap))) {
      error = k_ra8_err_invalid_size;
    }
    if (error == k_ra8_ok) {
      error = priv_viewer_size_comic_tiles(reader);
    }
  } else if (reader->engine == (uint32_t)k_ra8_viewer_engine_reflow) {
    error = priv_viewer_open_reflow(reader);
  }
  return error;
}

/**
 * @brief Whether a path class matches the engine frozen at bind time.
 * @details Prevents a requirements object calculated for one engine from being
 * reused to open a differently laid-out document.
 * @param[in] reader Bound reader.
 * @param[in] format Classified requested path.
 * @return Whether the pair is compatible.
 * @retval true The path and workspace select the same engine.
 * @retval false They differ or the engine is unknown.
 * @pre @p reader is non-NULL and bound.
 * @pre @p format came from ::internal_classify.
 * @post No state is mutated.
 * @post A wrapped or reflow format never matches.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
bool priv_viewer_engine_matches(const ra8_viewer_reader_t* reader, ra8_viewer_format_t format)
{
  if (reader == nullptr) {
    return false;
  }
  return ((reader->engine == (uint32_t)k_ra8_viewer_engine_jof) &&
          (format == k_ra8_viewer_format_jof)) ||
         ((reader->engine == (uint32_t)k_ra8_viewer_engine_comic) &&
          (format == k_ra8_viewer_format_comic)) ||
         ((reader->engine == (uint32_t)k_ra8_viewer_engine_reflow) &&
          ((format == k_ra8_viewer_format_epub) || (format == k_ra8_viewer_format_rabook)));
}

ra8_err_t priv_viewer_open_bound_source(ra8_viewer_reader_t*    reader,
                                        ra8_viewer_format_t     format,
                                        const ra8_fmt_source_t* source,
                                        viewer_source_close_fn  close_fn)
{
  if ((reader == nullptr) || (source == nullptr) || (source->read_at == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!reader->is_bound || reader->is_open || (reader->source.read_at != nullptr)) {
    return k_ra8_err_invalid_state;
  }
  if (source->size == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (!priv_viewer_engine_matches(reader, format)) {
    return k_ra8_err_invalid_state;
  }
  reader->source         = *source;
  reader->source_close   = close_fn;
  reader->reflow.is_epub = (format == k_ra8_viewer_format_epub);
  const ra8_err_t error  = internal_open_selected(reader);
  if (error == k_ra8_ok) {
    reader->is_open = true;
    return k_ra8_ok;
  }
  if (reader->engine == (uint32_t)k_ra8_viewer_engine_comic) {
    (void)comic_close(&reader->comic.archive);
  } else if (reader->engine == (uint32_t)k_ra8_viewer_engine_reflow) {
    priv_viewer_close_reflow(reader);
  }
  reader->source       = (ra8_fmt_source_t){};
  reader->source_close = nullptr;
  reader->tile_n       = 0U;
  return error;
}

ra8_err_t ra8_viewer_open_source(ra8_viewer_reader_t*    reader,
                                 ra8_viewer_format_t     format,
                                 const ra8_fmt_source_t* source)
{
  if ((reader == nullptr) || (source == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  return priv_viewer_open_bound_source(reader, format, source, nullptr);
}

uint32_t ra8_viewer_page_count(const ra8_viewer_reader_t* reader)
{
  return ((reader == nullptr) || !reader->is_open) ? 0U : reader->tile_n;
}

ra8_err_t ra8_viewer_render_page(ra8_viewer_reader_t* reader, uint32_t page)
{
  if (reader == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!reader->is_open) {
    return k_ra8_err_invalid_state;
  }
  if (page >= reader->tile_n) {
    return k_ra8_err_out_of_range;
  }
  if (reader->engine == (uint32_t)k_ra8_viewer_engine_comic) {
    return priv_viewer_render_comic(reader, page);
  }
  if (reader->engine == (uint32_t)k_ra8_viewer_engine_reflow) {
    return priv_viewer_render_reflow(reader, page);
  }
  return priv_viewer_render_jof(reader, page);
}

ra8_err_t
ra8_viewer_page_container(ra8_viewer_reader_t* reader, uint32_t page, ra8_imgdec_name_t* out)
{
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out = (ra8_imgdec_name_t){};
  if (reader == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!reader->is_open) {
    return k_ra8_err_invalid_state;
  }
  if (page >= reader->tile_n) {
    return k_ra8_err_out_of_range;
  }
  if (reader->engine != (uint32_t)k_ra8_viewer_engine_comic) {
    return k_ra8_err_not_supported;
  }
  return priv_viewer_comic_container(reader, page, out);
}

uint32_t ra8_viewer_tile_count(const ra8_viewer_reader_t* reader)
{
  return ra8_viewer_page_count(reader);
}

ra8_err_t ra8_viewer_tile_size(const ra8_viewer_reader_t* reader,
                               uint32_t                   index,
                               uint32_t*                  width,
                               uint32_t*                  height)
{
  if ((reader == nullptr) || (width == nullptr) || (height == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!reader->is_open) {
    return k_ra8_err_invalid_state;
  }
  if (index >= reader->tile_n) {
    return k_ra8_err_out_of_range;
  }
  *width  = reader->tile_wpx[index];
  *height = reader->tile_hpx[index];
  return k_ra8_ok;
}

ra8_err_t ra8_viewer_tile_requirements(const ra8_viewer_reader_t* reader,
                                       uint32_t                   index,
                                       size_t*                    out_bytes,
                                       size_t*                    out_alignment)
{
  uint32_t        width  = 0U;
  uint32_t        height = 0U;
  const ra8_err_t error  = ra8_viewer_tile_size(reader, index, &width, &height);
  if ((out_bytes == nullptr) || (out_alignment == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (error != k_ra8_ok) {
    return error;
  }
  *out_bytes     = (size_t)width * (size_t)height * sizeof(uint16_t);
  *out_alignment = alignof(uint16_t);
  return k_ra8_ok;
}

ra8_err_t ra8_viewer_render_tile565(ra8_viewer_reader_t*           reader,
                                    uint32_t                       index,
                                    void*                          workspace,
                                    size_t                         workspace_bytes,
                                    uint32_t*                      width,
                                    uint32_t*                      height,
                                    uint16_t**                     out_pixels,
                                    ra8_viewer_workspace_report_t* report)
{
  if ((reader == nullptr) || (width == nullptr) || (height == nullptr) || (out_pixels == nullptr) ||
      (report == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_pixels               = nullptr;
  size_t          required  = 0U;
  size_t          alignment = 0U;
  const ra8_err_t error     = ra8_viewer_tile_requirements(reader, index, &required, &alignment);
  *report =
    (ra8_viewer_workspace_report_t){.required_bytes = required, .supplied_bytes = workspace_bytes};
  if (error != k_ra8_ok) {
    return error;
  }
  if ((workspace == nullptr) || (((uintptr_t)workspace % alignment) != 0U) ||
      (workspace_bytes < required)) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t render = k_ra8_err_invalid_state;
  if (reader->engine == (uint32_t)k_ra8_viewer_engine_comic) {
    render =
      priv_viewer_tile_comic(reader, index, (uint16_t*)workspace, workspace_bytes, width, height);
  } else if (reader->engine == (uint32_t)k_ra8_viewer_engine_reflow) {
    render =
      priv_viewer_tile_reflow(reader, index, (uint16_t*)workspace, workspace_bytes, width, height);
  } else {
    render =
      priv_viewer_tile_jof(reader, index, (uint16_t*)workspace, workspace_bytes, width, height);
  }
  if (render == k_ra8_ok) {
    *out_pixels = (uint16_t*)workspace;
  }
  return render;
}

void ra8_viewer_close(ra8_viewer_reader_t* reader)
{
  if (reader == nullptr) {
    return;
  }
  if (reader->is_open) {
    if (reader->engine == (uint32_t)k_ra8_viewer_engine_comic) {
      (void)comic_close(&reader->comic.archive);
    } else if (reader->engine == (uint32_t)k_ra8_viewer_engine_reflow) {
      priv_viewer_close_reflow(reader);
    }
    if (reader->source_close != nullptr) {
      reader->source_close(reader->source.ctx);
    }
  }
  reader->source       = (ra8_fmt_source_t){};
  reader->source_close = nullptr;
  reader->host_file    = (viewer_file_ctx_t){.fd = -1};
  reader->is_open      = false;
  reader->tile_n       = 0U;
  reader->rt565        = reader->fb;
  reader->rt_w         = (uint32_t)k_ra8_viewer_fb_width;
  reader->rt_h         = (uint32_t)k_ra8_viewer_fb_height;
}
