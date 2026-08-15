/**
 * @file ra8_viewer_reader.c
 * @brief Requirements, workspace binding, descriptor lifecycle, and dispatch.
 * @details Implements the JOF-only requirements-to-bind lifecycle and keeps
 * host descriptor ownership at this composition edge.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_viewer_reader.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_viewer_reader_internal.h"

#ifndef O_CLOEXEC
/** @brief No-op close-on-exec fallback for hosts lacking the flag. */
#define O_CLOEXEC (0)
#endif

/** @brief Recognised filename classes at the host composition edge. */
typedef enum : uint8_t {
  k_viewer_fmt_unknown = 0U, /**< Unrecognised extension.                */
  k_viewer_fmt_jof     = 1U, /**< Supported streamed JOF.                */
  k_viewer_fmt_comic   = 2U, /**< Comic blocked on streaming codec APIs. */
  k_viewer_fmt_reflow  = 3U, /**< EPUB/RABOOK engine not wired.          */
} viewer_fmt_t;

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

/**
 * @brief Classify a path without opening it.
 * @details Recognises JOF, comic/wrapper, and reflow extension families.
 * @param[in] path NUL-terminated path.
 * @return Recognised filename class.
 * @retval k_viewer_fmt_unknown No recognised extension matched.
 * @pre @p path is non-NULL.
 * @pre @p path is NUL-terminated.
 * @post No descriptor is opened.
 * @post No state is mutated.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static viewer_fmt_t internal_classify(const char* path)
{
  if (internal_ends_with(path, ".jof")) {
    return k_viewer_fmt_jof;
  }
  if (internal_ends_with(path, ".cbz") || internal_ends_with(path, ".cbr") ||
      internal_ends_with(path, ".cbt") || internal_ends_with(path, ".gz") ||
      internal_ends_with(path, ".xz")) {
    return k_viewer_fmt_comic;
  }
  if (internal_ends_with(path, ".epub") || internal_ends_with(path, ".epb") ||
      internal_ends_with(path, ".rabook") || internal_ends_with(path, ".rbk")) {
    return k_viewer_fmt_reflow;
  }
  return k_viewer_fmt_unknown;
}

/**
 * @brief Best-effort exact diagnostic fragment over standard error.
 * @details Advances across short writes and retries interrupted writes.
 * @param[in] text NUL-terminated diagnostic fragment.
 * @pre @p text is non-NULL.
 * @pre Standard error may accept descriptor writes.
 * @post The complete fragment was attempted.
 * @post Reader state remains unchanged.
 * @note Concurrent fragments may interleave.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_stderr_write(const char* text)
{
  size_t       offset = 0U;
  const size_t length = strlen(text);
  while (offset < length) {
    const ssize_t written = write(STDERR_FILENO, &text[offset], length - offset);
    if ((written < 0) && (errno == EINTR)) {
      continue;
    }
    if (written <= 0) {
      return;
    }
    offset += (size_t)written;
  }
}

/**
 * @brief Emit the truthful unsupported-format reason.
 * @details Distinguishes codec-contract, reflow-wiring, and unknown formats.
 * @param[in] format Classified path family.
 * @param[in] path NUL-terminated path for the diagnostic.
 * @return Always unsupported.
 * @retval k_ra8_err_not_supported The format cannot open in this checkpoint.
 * @pre @p path is non-NULL and terminated.
 * @pre @p format came from ::internal_classify.
 * @post A reason naming @p path was attempted.
 * @post No reader state is mutated.
 * @note Descriptor output is best-effort.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_reject(viewer_fmt_t format, const char* path)
{
  internal_stderr_write("ra8_viewer: '");
  internal_stderr_write(path);
  if (format == k_viewer_fmt_comic) {
    internal_stderr_write("' recognised but comic rendering requires streaming codec "
                          "source/sink/spool APIs (contiguous stb arena, encoded-page "
                          "pointer, and whole-output gzip/XZ remain)");
  } else if (format == k_viewer_fmt_reflow) {
    internal_stderr_write("' recognised but its reflow reader engine "
                          "(font + image loader) is not wired into the viewer yet");
  } else {
    internal_stderr_write("' has an unsupported file type");
  }
  internal_stderr_write("\n");
  return k_ra8_err_not_supported;
}

/**
 * @brief Open and size one non-empty regular host file.
 * @details Acquires a close-on-exec descriptor and records its stat size.
 * @param[out] file Descriptor context.
 * @param[in] path NUL-terminated host path.
 * @return Open status.
 * @retval k_ra8_ok A non-empty regular file is owned by @p file.
 * @retval k_ra8_err_not_found Open or validation failed.
 * @pre @p file is writable.
 * @pre @p path is non-NULL and terminated.
 * @post Success publishes one owned descriptor.
 * @post Failure leaves no acquired descriptor.
 * @note The caller must close success.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_file_open(viewer_file_ctx_t* file, const char* path)
{
  const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return k_ra8_err_not_found;
  }
  struct stat metadata = {};
  if ((fstat(descriptor, &metadata) != 0) || !S_ISREG(metadata.st_mode) ||
      (metadata.st_size <= 0)) {
    (void)close(descriptor);
    return k_ra8_err_not_found;
  }
  *file =
    (viewer_file_ctx_t){.size = (uint64_t)metadata.st_size, .fd = descriptor, .is_open = true};
  return k_ra8_ok;
}

ra8_err_t
priv_viewer_pread(void* ctx, uint64_t offset, uint8_t* buffer, size_t length, size_t* out_read)
{
  viewer_file_ctx_t* file = (viewer_file_ctx_t*)ctx;
  if (out_read == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_read = 0U;
  if ((file == nullptr) || (buffer == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!file->is_open || (offset > (uint64_t)INT64_MAX)) {
    return k_ra8_err_invalid_state;
  }
  if (offset >= file->size) {
    return k_ra8_ok;
  }
  const uint64_t available = file->size - offset;
  size_t         requested = ((uint64_t)length > available) ? (size_t)available : length;
  size_t         completed = 0U;
  while (completed < requested) {
    size_t pass = requested - completed;
    if (pass > (size_t)SSIZE_MAX) {
      pass = (size_t)SSIZE_MAX;
    }
    const ssize_t got =
      pread(file->fd, &((uint8_t*)buffer)[completed], pass, (off_t)(offset + (uint64_t)completed));
    if ((got < 0) && (errno == EINTR)) {
      continue;
    }
    if (got <= 0) {
      if (got == 0) {
        *out_read = completed;
        return k_ra8_ok;
      }
      return k_ra8_err_not_found;
    }
    completed += (size_t)got;
  }
  *out_read = completed;
  return k_ra8_ok;
}

/**
 * @brief Charge the complete deterministic reader layout.
 * @details Walks state, framebuffer, dimensions, one cache cell, and scratch.
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
  (void)internal_take(&layout, need->cell_bytes, alignof(max_align_t));
  (void)internal_take(&layout, sizeof(ra8_keycache_cell_t), alignof(ra8_keycache_cell_t));
  (void)internal_take(&layout, sizeof(ra8_tile_key_t), alignof(ra8_tile_key_t));
  (void)internal_take(&layout, sizeof(ra8_tile_dims_t), alignof(ra8_tile_dims_t));
  (void)internal_take(&layout, (size_t)k_viewer_jof_buckets * sizeof(int32_t), alignof(int32_t));
  (void)internal_take(&layout, need->scratch_bytes, alignof(max_align_t));
  return layout;
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
      (need->cell_bytes == 0U) || (need->scratch_bytes == 0U)) {
    return false;
  }
  const viewer_layout_t layout = internal_layout(need, nullptr, 0U);
  return layout.valid && (layout.used == need->required_bytes);
}

ra8_err_t ra8_viewer_reader_requirements(const char* path, ra8_viewer_reader_requirements_t* out)
{
  if ((path == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out                      = (ra8_viewer_reader_requirements_t){};
  const viewer_fmt_t format = internal_classify(path);
  if (format != k_viewer_fmt_jof) {
    return internal_reject(format, path);
  }
  viewer_file_ctx_t file  = {.fd = -1};
  ra8_err_t         error = internal_file_open(&file, path);
  ra8_jof_info_t    info  = {};
  if (error == k_ra8_ok) {
    error = ra8_jof_parse(priv_viewer_pread, &file, file.size, &info);
  }
  if (file.is_open) {
    (void)close(file.fd);
  }
  const uint64_t band = (uint64_t)info.tile_w * (uint64_t)info.tile_h * (uint64_t)info.bpp;
  if ((error == k_ra8_ok) && (band == 0U)) {
    error = k_ra8_err_invalid_size;
  }
  const ra8_decomp_limits_t limits = ra8_decomp_limits_default();
  if (error == k_ra8_ok) {
    error = ra8_decomp_check_declared(&limits, file.size, band);
  }
  if (error != k_ra8_ok) {
    return error;
  }
  const uint32_t tiles    = ((uint32_t)info.height + (uint32_t)k_ra8_viewer_fb_height - 1U) /
                            (uint32_t)k_ra8_viewer_fb_height;
  out->required_alignment = alignof(max_align_t);
  out->framebuffer_bytes =
    (size_t)k_ra8_viewer_fb_width * (size_t)k_ra8_viewer_fb_height * sizeof(uint16_t);
  out->dimensions_bytes        = (size_t)tiles * 2U * sizeof(uint32_t);
  out->cell_bytes              = (size_t)band;
  out->scratch_bytes           = (info.codec == (uint8_t)k_ra8_jof_codec_raw)
                                   ? 1U
                                   : (size_t)ra8_jof_stored_bound((uint32_t)band);
  out->tile_count              = tiles;
  out->layout_version          = (uint32_t)k_viewer_layout_version;
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
  const size_t dimension_bytes = requirements->dimensions_bytes / 2U;
  uint32_t*    widths  = (uint32_t*)internal_take(&layout, dimension_bytes, alignof(uint32_t));
  uint32_t*    heights = (uint32_t*)internal_take(&layout, dimension_bytes, alignof(uint32_t));
  uint8_t* cells = (uint8_t*)internal_take(&layout, requirements->cell_bytes, alignof(max_align_t));
  ra8_keycache_cell_t* meta =
    (ra8_keycache_cell_t*)internal_take(&layout, sizeof(*meta), alignof(ra8_keycache_cell_t));
  ra8_tile_key_t* keys =
    (ra8_tile_key_t*)internal_take(&layout, sizeof(*keys), alignof(ra8_tile_key_t));
  ra8_tile_dims_t* dims =
    (ra8_tile_dims_t*)internal_take(&layout, sizeof(*dims), alignof(ra8_tile_dims_t));
  int32_t* buckets = (int32_t*)internal_take(&layout,
                                             (size_t)k_viewer_jof_buckets * sizeof(*buckets),
                                             alignof(int32_t));
  uint8_t* scratch =
    (uint8_t*)internal_take(&layout, requirements->scratch_bytes, alignof(max_align_t));
  if (!layout.valid || (layout.used != requirements->required_bytes)) {
    return k_ra8_err_invalid_size;
  }
  *reader = (ra8_viewer_reader_t){.file     = {.fd = -1},
                                  .is_bound = true,
                                  .fb       = framebuffer,
                                  .tile_wpx = widths,
                                  .tile_hpx = heights,
                                  .tile_cap = requirements->tile_count,
                                  .rt565    = framebuffer,
                                  .rt_w     = (uint32_t)k_ra8_viewer_fb_width,
                                  .rt_h     = (uint32_t)k_ra8_viewer_fb_height,
                                  .jof      = {.cells       = cells,
                                               .meta        = meta,
                                               .keys        = keys,
                                               .dims        = dims,
                                               .buckets     = buckets,
                                               .scratch     = scratch,
                                               .cell_cap    = requirements->cell_bytes,
                                               .scratch_cap = requirements->scratch_bytes}};
  *out    = reader;
  return k_ra8_ok;
}

ra8_err_t ra8_viewer_open(ra8_viewer_reader_t* reader, const char* path)
{
  if ((reader == nullptr) || (path == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!reader->is_bound || reader->is_open || reader->file.is_open) {
    return k_ra8_err_invalid_state;
  }
  const viewer_fmt_t format = internal_classify(path);
  if (format != k_viewer_fmt_jof) {
    return internal_reject(format, path);
  }
  ra8_err_t error = internal_file_open(&reader->file, path);
  if (error == k_ra8_ok) {
    error = priv_viewer_open_jof(reader);
  }
  if (error == k_ra8_ok) {
    reader->tile_n =
      ((uint32_t)reader->jof.dctx.info.height + (uint32_t)k_ra8_viewer_fb_height - 1U) /
      (uint32_t)k_ra8_viewer_fb_height;
    if (reader->tile_n > reader->tile_cap) {
      error = k_ra8_err_invalid_size;
    }
  }
  if (error == k_ra8_ok) {
    priv_viewer_size_jof_tiles(reader, reader->tile_n);
    reader->is_open = true;
    return k_ra8_ok;
  }
  if (reader->file.is_open) {
    (void)close(reader->file.fd);
  }
  reader->file   = (viewer_file_ctx_t){.fd = -1};
  reader->tile_n = 0U;
  return error;
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
  return priv_viewer_render_jof(reader, page);
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
  const ra8_err_t render =
    priv_viewer_tile_jof(reader, index, (uint16_t*)workspace, workspace_bytes, width, height);
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
  if (reader->file.is_open) {
    (void)close(reader->file.fd);
  }
  reader->file    = (viewer_file_ctx_t){.fd = -1};
  reader->is_open = false;
  reader->tile_n  = 0U;
  reader->rt565   = reader->fb;
  reader->rt_w    = (uint32_t)k_ra8_viewer_fb_width;
  reader->rt_h    = (uint32_t)k_ra8_viewer_fb_height;
}
