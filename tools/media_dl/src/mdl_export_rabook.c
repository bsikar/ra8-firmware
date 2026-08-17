/**
 * @file mdl_export_rabook.c
 * @brief Bounded fixed-layout EPUB-to-RBKC writer for media chapters.
 * @details Reuses the production EPUB writer and firmware RABOOK compiler under
 *          one caller-owned 96 MiB profile, then strictly validates the staged
 *          RBKC container before atomic publication.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <string.h>

#include "mdl_export_rabook_internal.h"
#include "miniz.h"
#include "ra8_book.h"
#include "ra8_rabook_container.h"
#include "ra8_rabook_pipeline.h"

/** @brief Fixed production conversion profile within the 96 MiB CLI arena. */
typedef enum : uint32_t {
  k_rabook_page_cap          = 47U,                 /**< EPUB manifest-safe page count. */
  k_rabook_chapter_cap       = 48U,                 /**< Builder chapter rows.          */
  k_rabook_node_cap          = 512U,                /**< Fixed-page XHTML DOM nodes.    */
  k_rabook_attr_cap          = 256U,                /**< Fixed-page XHTML attributes.   */
  k_rabook_style_cap         = 4U,                  /**< Stylesheet descriptors.        */
  k_rabook_image_cap         = 48U,                 /**< Pages plus one external cover. */
  k_rabook_string_bytes      = 256U * 1024U,        /**< Interned metadata/DOM strings. */
  k_rabook_image_pool_bytes  = 24U * 1024U * 1024U, /**< 48 gray4 1024-square rasters.  */
  k_rabook_flat_bytes        = 26U * 1024U * 1024U, /**< Flat RABOOK1 output.           */
  k_rabook_xhtml_bytes       = 64U * 1024U,         /**< One page XHTML member.         */
  k_rabook_image_raw_bytes   = 8U * 1024U * 1024U,  /**< One encoded image member.      */
  k_rabook_image_arena_bytes = 20U * 1024U * 1024U, /**< One stb grayscale decode.      */
  k_rabook_gray_bytes        = 1024U * 1024U,       /**< 1024-square downscale buffer.  */
  k_rabook_css_bytes         = 64U * 1024U,         /**< One stylesheet member.         */
  k_rabook_max_image_edge    = 1024U,               /**< Output raster long-edge clamp. */
  k_rabook_chunk_bytes       = 1024U * 1024U,       /**< Independent RBKC chunk size.   */
  k_rabook_compressed_bytes  = k_rabook_chunk_bytes + (64U * 1024U),
  /**< One complete zlib stream. */
  k_rabook_offset_entries = 64U, /**< RBKC offset table entries. */
} mdl_rabook_profile_limit_t;

/** @brief All fixed-profile views carved from the export workspace. */
typedef struct {
  ra8_epub_book_t*                 book;        /**< Streamed intermediate EPUB.  */
  ra8_rabook_buffers_t             builder;     /**< Flat-book builder arenas.    */
  ra8_rabook_pipeline_scratch_t    pipeline;    /**< EPUB compiler scratch.       */
  ra8_img_arena_t                  image_arena; /**< stb decode arena descriptor. */
  ra8_rabook_container_workspace_t container;   /**< RBKC writer workspace.       */
} mdl_rabook_profile_t;

/**
 * @brief Allocate the builder tables and resident flat-book pools.
 * @details Carves each typed table and byte pool in declaration order, then
 *          publishes capacities only after every required span exists.
 * @param[in,out] workspace Exclusive export arena.
 * @param[out] profile Profile receiving builder views.
 * @return Capacity status.
 * @retval k_ra8_ok Every builder arena was reserved.
 * @retval k_ra8_err_invalid_size The export arena is too small.
 * @pre Pointers are non-NULL and @p profile is zero-initialized.
 * @pre @p workspace is exclusively mutable.
 * @post Success initializes every ::ra8_rabook_buffers_t member.
 * @post Failure publishes no usable builder profile.
 * @note Allocations are monotonic and released only by workspace reset.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_allocate_builder(mdl_export_workspace_t* workspace,
                                                        mdl_rabook_profile_t*   profile)
{
  ra8_rabook_buffers_t* const builder = &profile->builder;
  builder->chapters = mdl_export_workspace_take(workspace,
                                                k_rabook_chapter_cap * sizeof(*builder->chapters),
                                                _Alignof(ra8_book_chapter_t));
  builder->nodes    = mdl_export_workspace_take(workspace,
                                                k_rabook_node_cap * sizeof(*builder->nodes),
                                                _Alignof(ra8_book_node_t));
  builder->attrs    = mdl_export_workspace_take(workspace,
                                                k_rabook_attr_cap * sizeof(*builder->attrs),
                                                _Alignof(ra8_book_attr_t));
  builder->stylesheets =
    mdl_export_workspace_take(workspace,
                              k_rabook_style_cap * sizeof(*builder->stylesheets),
                              _Alignof(ra8_book_stylesheet_t));
  builder->images      = mdl_export_workspace_take(workspace,
                                                   k_rabook_image_cap * sizeof(*builder->images),
                                                   _Alignof(ra8_book_image_t));
  builder->string_pool = mdl_export_workspace_take(workspace, k_rabook_string_bytes, 1U);
  builder->image_pool =
    mdl_export_workspace_take(workspace, k_rabook_image_pool_bytes, _Alignof(max_align_t));
  builder->out = mdl_export_workspace_take(workspace, k_rabook_flat_bytes, _Alignof(max_align_t));
  if ((builder->chapters == nullptr) || (builder->nodes == nullptr) ||
      (builder->attrs == nullptr) || (builder->stylesheets == nullptr) ||
      (builder->images == nullptr) || (builder->string_pool == nullptr) ||
      (builder->image_pool == nullptr) || (builder->out == nullptr)) {
    return k_ra8_err_invalid_size;
  }
  builder->chapter_cap    = k_rabook_chapter_cap;
  builder->node_cap       = k_rabook_node_cap;
  builder->attr_cap       = k_rabook_attr_cap;
  builder->stylesheet_cap = k_rabook_style_cap;
  builder->image_cap      = k_rabook_image_cap;
  builder->string_cap     = k_rabook_string_bytes;
  builder->image_pool_cap = k_rabook_image_pool_bytes;
  builder->out_cap        = k_rabook_flat_bytes;
  return k_ra8_ok;
}

/**
 * @brief Allocate the streamed EPUB parser and image-conversion scratch.
 * @details Reserves one reusable member buffer, stb arena, gray raster, CSS
 *          buffer, and XML workspace for the fixed grayscale compiler profile.
 * @param[in,out] workspace Exclusive export arena.
 * @param[out] profile Profile receiving compiler views.
 * @return Capacity status.
 * @retval k_ra8_ok Every parser and raster arena was reserved.
 * @retval k_ra8_err_invalid_size The export arena is too small.
 * @pre Pointers are non-NULL and builder allocation already succeeded.
 * @pre Profile and workspace remain exclusively mutable.
 * @post Success configures a gray4 1024-long-edge compiler profile.
 * @post Failure publishes no usable pipeline profile.
 * @note The profile intentionally rejects larger source working sets.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_allocate_pipeline(mdl_export_workspace_t* workspace,
                                                         mdl_rabook_profile_t*   profile)
{
  profile->book =
    mdl_export_workspace_take(workspace, sizeof(*profile->book), _Alignof(ra8_epub_book_t));
  uint8_t* xhtml = mdl_export_workspace_take(workspace, k_rabook_xhtml_bytes, 1U);
  uint8_t* raw =
    mdl_export_workspace_take(workspace, k_rabook_image_raw_bytes, _Alignof(max_align_t));
  uint8_t* arena =
    mdl_export_workspace_take(workspace, k_rabook_image_arena_bytes, _Alignof(max_align_t));
  uint8_t* gray = mdl_export_workspace_take(workspace, k_rabook_gray_bytes, _Alignof(max_align_t));
  char*    css  = mdl_export_workspace_take(workspace, k_rabook_css_bytes, 1U);
  ra8_rabook_xml_workspace_t* xml =
    mdl_export_workspace_take(workspace, sizeof(*xml), _Alignof(ra8_rabook_xml_workspace_t));
  if ((profile->book == nullptr) || (xhtml == nullptr) || (raw == nullptr) || (arena == nullptr) ||
      (gray == nullptr) || (css == nullptr) || (xml == nullptr)) {
    return k_ra8_err_invalid_size;
  }
  profile->image_arena = (ra8_img_arena_t){.base = arena, .cap = k_rabook_image_arena_bytes};
  profile->pipeline    = (ra8_rabook_pipeline_scratch_t){.xhtml          = xhtml,
                                                         .xhtml_cap      = k_rabook_xhtml_bytes,
                                                         .image_raw      = raw,
                                                         .image_cap      = k_rabook_image_raw_bytes,
                                                         .img_arena      = &profile->image_arena,
                                                         .gray           = gray,
                                                         .gray_cap       = k_rabook_gray_bytes,
                                                         .css            = css,
                                                         .css_cap        = k_rabook_css_bytes,
                                                         .max_image_edge = k_rabook_max_image_edge,
                                                         .pixel_format   = k_ra8_book_pixfmt_gray4,
                                                         .xml_workspace  = xml};
  (void)memset(profile->book, 0, sizeof(*profile->book));
  return k_ra8_ok;
}

/**
 * @brief Allocate the independent-chunk RBKC writer workspace.
 * @details Reserves one input chunk, one worst-case compressed stream, the
 *          miniz compressor object, and the bounded chunk-offset table.
 * @param[in,out] workspace Exclusive export arena.
 * @param[out] profile Profile receiving container views.
 * @return Capacity status.
 * @retval k_ra8_ok Every container span was reserved.
 * @retval k_ra8_err_invalid_size The export arena is too small.
 * @pre Pointers are non-NULL and earlier profile allocations succeeded.
 * @pre Container spans must remain non-overlapping and exclusively mutable.
 * @post Success initializes every container workspace member.
 * @post Failure publishes no usable container workspace.
 * @note The offset table covers the fixed flat-output cap.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_allocate_container(mdl_export_workspace_t* workspace,
                                                          mdl_rabook_profile_t*   profile)
{
  ra8_rabook_container_workspace_t* const container = &profile->container;
  container->input =
    mdl_export_workspace_take(workspace, k_rabook_chunk_bytes, _Alignof(max_align_t));
  container->compressed =
    mdl_export_workspace_take(workspace, k_rabook_compressed_bytes, _Alignof(max_align_t));
  container->compressor =
    mdl_export_workspace_take(workspace, sizeof(tdefl_compressor), _Alignof(max_align_t));
  container->offsets =
    mdl_export_workspace_take(workspace,
                              k_rabook_offset_entries * sizeof(*container->offsets),
                              _Alignof(uint64_t));
  if ((container->input == nullptr) || (container->compressed == nullptr) ||
      (container->compressor == nullptr) || (container->offsets == nullptr)) {
    return k_ra8_err_invalid_size;
  }
  container->input_cap      = k_rabook_chunk_bytes;
  container->compressed_cap = k_rabook_compressed_bytes;
  container->compressor_cap = sizeof(tdefl_compressor);
  container->offset_cap     = k_rabook_offset_entries;
  return k_ra8_ok;
}

/**
 * @brief Carve the complete fixed conversion profile from caller storage.
 * @details Sequences builder, streamed-EPUB, raster, and RBKC reservations so
 *          later stages never allocate or silently reduce an earlier bound.
 * @param[in,out] workspace Exclusive reset export arena.
 * @param[out] profile Complete profile on success.
 * @return First builder, pipeline, or container capacity failure.
 * @retval k_ra8_ok The complete fixed profile fits.
 * @retval k_ra8_err_invalid_size A required span does not fit.
 * @pre Pointers are non-NULL and @p workspace has used equal to zero.
 * @pre @p profile is exclusively writable.
 * @post Success leaves all profile pointers within the caller arena.
 * @post Failure never falls back to dynamic allocation.
 * @note The caller may inspect workspace high-water after the operation.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_allocate_profile(mdl_export_workspace_t* workspace,
                                                        mdl_rabook_profile_t*   profile)
{
  *profile        = (mdl_rabook_profile_t){};
  ra8_err_t error = internal_allocate_builder(workspace, profile);
  if (error == k_ra8_ok) {
    error = internal_allocate_pipeline(workspace, profile);
  }
  if (error == k_ra8_ok) {
    error = internal_allocate_container(workspace, profile);
  }
  return error;
}

/**
 * @brief Produce and validate one private fixed-layout EPUB intermediate.
 * @details Uses create-new sibling naming, the production EPUB writer, and its
 *          strict transaction verifier before exposing the intermediate path.
 * @param[in,out] storage Exclusive portable storage binding.
 * @param[in] directory Canonical source chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Page count.
 * @param[in] destination Final path used to salt the private name.
 * @param[in] meta Resolved metadata.
 * @param[in,out] workspace Export/validation arena.
 * @param[out] temp_path Chosen canonical intermediate path.
 * @param[out] temp_exists Whether a committed temporary file must be removed.
 * @return EPUB write, validation, or publication status.
 * @retval k_ra8_ok A strictly validated temporary EPUB is visible.
 * @retval other A create, write, validate, commit, or abort error propagated.
 * @pre All pointers and rows are valid and stable.
 * @pre The storage transaction workspace is free.
 * @post Success sets @p temp_exists true and names the temporary EPUB.
 * @post Failure reports truthful publication state for cleanup.
 * @note The final destination is never modified here.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_write_temp_epub(mdl_storage_t*           storage,
                                                       const char*              directory,
                                                       char                     names[][k_name_max],
                                                       size_t                   count,
                                                       const char*              destination,
                                                       const mdl_export_meta_t* meta,
                                                       mdl_export_workspace_t*  workspace,
                                                       char*                    temp_path,
                                                       bool*                    temp_exists)
{
  *temp_exists               = false;
  mdl_export_output_t output = {};
  ra8_err_t           error  = priv_mdl_rabook_temp_begin(&output,
                                                          storage,
                                                          directory,
                                                          destination,
                                                          temp_path,
                                                          k_fw_fs_path_cap);
  if (error == k_ra8_ok) {
    error = priv_mdl_export_epub(storage, directory, names, count, &output, meta, workspace);
  }
  if ((error != k_ra8_ok) && output.writer.transaction.active) {
    const ra8_err_t aborted = priv_mdl_export_output_abort(&output);
    return (aborted == k_ra8_ok) ? error : aborted;
  }
  if (error == k_ra8_ok) {
    error = priv_mdl_export_output_commit(&output, workspace, temp_exists);
  }
  return error;
}

/**
 * @brief Close both compile-time EPUB readers, preserving the first error.
 * @details Closes the resident streamed book view first, then the portable
 *          EPUB source reader, regardless of any prior pipeline failure. A
 *          close failure only replaces @p error when the pipeline had
 *          otherwise succeeded, so the earliest real failure always wins.
 * @param[in] error Status accumulated by the compile pipeline before this
 *                   close step; k_ra8_ok if every prior step succeeded.
 * @param[in,out] book Resident streamed EPUB view; closed only if in use.
 * @param[in,out] source Portable EPUB source reader; closed only if open.
 * @return The first non-ok status among @p error and both close calls.
 * @retval k_ra8_ok Every prior step and both closes succeeded.
 * @retval other The first failure among @p error and the two closes.
 * @pre @p book and @p source describe the same compile-time EPUB.
 * @pre Both readers are safe to close exactly once from this call.
 * @post Both readers are closed regardless of the returned status.
 * @post @p error is never weakened: an existing failure survives an
 *       independent close failure on either reader.
 * @note Not thread-safe; the caller retains exclusive ownership of both
 *       readers for the duration of this call.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_close_compile_sources(ra8_err_t                 error,
                                                             ra8_epub_book_t*          book,
                                                             mdl_rabook_epub_source_t* source)
{
  if ((book != nullptr) && (book->in_use != 0U)) {
    const ra8_err_t closed = ra8_epub_close(book);
    if ((error == k_ra8_ok) && (closed != k_ra8_ok)) {
      error = closed;
    }
  }
  if (source->file.is_open) {
    const ra8_err_t closed = priv_mdl_rabook_epub_close(source);
    if ((error == k_ra8_ok) && (closed != k_ra8_ok)) {
      error = closed;
    }
  }
  return error;
}

/**
 * @brief Confirm the compiled flat book matches the source page count.
 * @details Compares the RABOOK1 chapter and image counts against the
 *          source EPUB's expected page count, tolerating exactly one
 *          synthesized cover image beyond the page count.
 * @param[in] blob Validated RABOOK1 buffer (already passed
 *                  ::ra8_book_validate).
 * @param[in] page_count Expected source spine/image minimum.
 * @return Whether the compiled counts fall within the expected range.
 * @retval k_ra8_ok Chapter and image counts both match expectations.
 * @retval k_ra8_err_validation_failed A page or image was lost or added.
 * @pre @p blob has already passed ::ra8_book_validate.
 * @pre @p page_count is representable as a uint32_t.
 * @post No state is mutated; only @p blob's header is read.
 * @note Not thread-safe against a concurrent writer of @p blob.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_check_compiled_page_count(const void* blob,
                                                                 size_t      page_count)
{
  const ra8_book_header_t* header         = ra8_book_header(blob);
  const uint32_t           expected_pages = (uint32_t)page_count;
  if ((header->chapter_count != expected_pages) || (header->image_count < expected_pages) ||
      (header->image_count > (expected_pages + 1U))) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Compile one temporary EPUB into a validated resident flat RABOOK1.
 * @details Opens the EPUB through portable random reads, invokes the production
 *          compiler, closes both readers, and checks exact page/image counts.
 * @param[in,out] storage Exclusive portable storage binding.
 * @param[in] temp_path Canonical validated EPUB path.
 * @param[in] page_count Expected spine/image minimum.
 * @param[in,out] workspace Reset caller export arena.
 * @param[out] profile Complete retained compiler/container profile.
 * @param[out] flat Resident flat-book source view.
 * @return Open, compiler, close, or structural status.
 * @retval k_ra8_ok A valid flat book has exact chapter and image counts.
 * @retval k_ra8_err_validation_failed Compiler output lost or added a page image.
 * @pre Pointers are non-NULL and the temporary EPUB is immutable.
 * @pre @p workspace is exclusively mutable and large enough for the profile.
 * @post Success initializes @p flat within @p workspace.
 * @post Every opened EPUB and portable source is closed on return.
 * @note The retained profile remains valid until workspace reset.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_compile_temp(mdl_storage_t*            storage,
                                                    const char*               temp_path,
                                                    size_t                    page_count,
                                                    mdl_export_workspace_t*   workspace,
                                                    mdl_rabook_profile_t*     profile,
                                                    mdl_rabook_flat_source_t* flat)
{
  workspace->used                 = 0U;
  ra8_err_t                error  = internal_allocate_profile(workspace, profile);
  mdl_rabook_epub_source_t source = {};
  if (error == k_ra8_ok) {
    error = priv_mdl_rabook_epub_open(&source, storage, temp_path);
  }
  ra8_epub_stream_media_t media = {.read = priv_mdl_rabook_epub_read,
                                   .ctx  = &source,
                                   .size = source.size_bytes};
  if (error == k_ra8_ok) {
    error = ra8_epub_open_streamed(&media, temp_path, profile->book);
  }
  const void* blob   = nullptr;
  uint32_t    length = 0U;
  if (error == k_ra8_ok) {
    error = ra8_rabook_compile_from_epub_to_buffer(profile->book,
                                                   &profile->builder,
                                                   &profile->pipeline,
                                                   &blob,
                                                   &length);
  }
  if ((error == k_ra8_ok) && (source.error != k_ra8_ok)) {
    error = source.error;
  }
  error = internal_close_compile_sources(error, profile->book, &source);
  if (error == k_ra8_ok) {
    error = ra8_book_validate(blob, length);
  }
  if (error == k_ra8_ok) {
    error = internal_check_compiled_page_count(blob, page_count);
  }
  if (error == k_ra8_ok) {
    *flat = (mdl_rabook_flat_source_t){.bytes = blob, .size_bytes = length};
  }
  return error;
}

/**
 * @brief Stream one flat book into a strictly validated final RBKC transaction.
 * @details Feeds the resident RABOOK1 blob through independent zlib chunks and
 *          relies on the shared transaction verifier before atomic publication.
 * @param[in,out] storage Exclusive portable storage binding.
 * @param[in] destination Canonical final RABOOK path.
 * @param[in] flat Resident validated flat-book source.
 * @param[in,out] profile Retained container workspace.
 * @param[in,out] workspace Export/strict-validation arena.
 * @return Container write, validation, or publication status.
 * @retval k_ra8_ok A complete strict RBKC artifact was published.
 * @retval other A transaction, compressor, validator, commit, or abort error.
 * @pre All pointers are non-NULL and profile spans remain alive.
 * @pre No temporary source file remains open.
 * @post Failure before publication preserves an existing destination.
 * @post Success publishes exactly one reader-openable `.rabook` file.
 * @note Power-loss durability follows the injected transaction backend.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_emit_container(mdl_storage_t*            storage,
                                                      const char*               destination,
                                                      mdl_rabook_flat_source_t* flat,
                                                      mdl_rabook_profile_t*     profile,
                                                      mdl_export_workspace_t*   workspace)
{
  mdl_export_output_t output = {};
  ra8_err_t           error =
    priv_mdl_export_output_begin(&output, storage, destination, k_ra8_mdl_format_rabook);
  uint64_t container_length = 0U;
  if (error == k_ra8_ok) {
    error = ra8_rabook_container_write(priv_mdl_rabook_flat_read,
                                       flat,
                                       flat->size_bytes,
                                       k_rabook_chunk_bytes,
                                       priv_mdl_export_output_write_at,
                                       &output,
                                       &profile->container,
                                       &container_length);
  }
  if ((error == k_ra8_ok) && (container_length != output.extent)) {
    error = k_ra8_err_invalid_size;
  }
  if ((error != k_ra8_ok) && output.writer.transaction.active) {
    const ra8_err_t aborted = priv_mdl_export_output_abort(&output);
    return (aborted == k_ra8_ok) ? error : aborted;
  }
  bool published = false;
  return (error == k_ra8_ok) ? priv_mdl_export_output_commit(&output, workspace, &published)
                             : error;
}

RA8_PRIV ra8_err_t priv_mdl_export_rabook(mdl_storage_t*           storage,
                                          const char*              directory,
                                          char                     names[][k_name_max],
                                          size_t                   count,
                                          const char*              destination,
                                          const mdl_export_meta_t* meta,
                                          mdl_export_workspace_t*  workspace)
{
  if ((storage == nullptr) || (directory == nullptr) || (names == nullptr) ||
      (destination == nullptr) || (meta == nullptr) || (workspace == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if ((count == 0U) || (count > (size_t)k_rabook_page_cap)) {
    return k_ra8_err_invalid_size;
  }
  char                     temp_path[k_fw_fs_path_cap] = {};
  bool                     temp_exists                 = false;
  ra8_err_t                error                       = internal_write_temp_epub(storage,
                                                                                  directory,
                                                                                  names,
                                                                                  count,
                                                                                  destination,
                                                                                  meta,
                                                                                  workspace,
                                                                                  temp_path,
                                                                                  &temp_exists);
  mdl_rabook_profile_t     profile                     = {};
  mdl_rabook_flat_source_t flat                        = {};
  if (error == k_ra8_ok) {
    error = internal_compile_temp(storage, temp_path, count, workspace, &profile, &flat);
  }
  if (temp_exists) {
    const ra8_err_t removed = fw_fs_unlink(&storage->fs->names, temp_path);
    if (removed != k_ra8_ok) {
      return removed;
    }
  }
  if (error == k_ra8_ok) {
    error = internal_emit_container(storage, destination, &flat, &profile, workspace);
  }
  return error;
}
