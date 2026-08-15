/**
 * @file mdl_export_zip.c
 * @brief Provide bounded miniz storage and the CBZ container writer.
 *
 * @details Adapts miniz allocation callbacks to caller-owned workspace, types
 * external cover assets, and emits deterministic CBZ archives.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "mdl_export.h"
#include "mdl_export_internal.h"
#include "mdl_urlname.h"
#include "miniz.h"
#include "ra8_attributes.h"

/**
 * @brief Round a byte count up to maximum host alignment.
 * @details Performs the power-of-two rounding only after overflow checks.
 * @param[in] bytes Requested payload byte count.
 * @param[out] out Rounded result.
 * @return Whether the aligned size is representable.
 * @retval true @p out contains the aligned size.
 * @retval false @p out is NULL or rounding would overflow.
 * @pre @p bytes is an untrusted bounded allocation request.
 * @pre Maximum alignment is a nonzero power of two.
 * @post Success writes a value not smaller than @p bytes.
 * @post Failure does not write through @p out.
 * @note Thread-safe and side-effect free.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_zip_align_size(size_t bytes, size_t* out)
{
  const size_t alignment = _Alignof(max_align_t);
  const size_t mask      = alignment - 1U;
  if ((out == nullptr) || (bytes > (SIZE_MAX - mask))) {
    return false;
  }
  *out = (bytes + mask) & ~mask;
  return true;
}

/**
 * @brief Find an allocator block by its payload address.
 * @details Walks only aligned blocks created after the saved writer floor and
 *          never follows a pointer outside the current workspace high edge.
 * @param[in] alloc Active callback adapter.
 * @param[in] address Candidate payload address.
 * @return Matching block header, or NULL.
 * @retval non-NULL @p address names a block from this adapter.
 * @retval NULL Arguments are invalid or no block matches.
 * @pre @p alloc is NULL or owns a well-formed current block chain.
 * @pre @p address is treated only as an opaque comparison value.
 * @post No allocator block or workspace counter is modified.
 * @post A returned header lies within the active workspace range.
 * @note Not thread-safe with concurrent allocation from the same adapter.
 * @since 0.1.0
 */
RA8_INTERNAL static mdl_zip_block_t* internal_zip_find_block(mdl_zip_allocator_t* alloc,
                                                             const void*          address)
{
  if ((alloc == nullptr) || (alloc->ws == nullptr) || (address == nullptr)) {
    return nullptr;
  }
  uint8_t* const end = alloc->ws->data + alloc->ws->used;
  for (mdl_zip_block_t* block = alloc->first;
       (block != nullptr) && ((uint8_t*)block < end) && (block->span >= sizeof(*block));
       block = ((uint8_t*)block + block->span < end)
                 ? (mdl_zip_block_t*)((uint8_t*)block + block->span)
                 : nullptr) {
    if ((const void*)(block + 1) == address) {
      return block;
    }
  }
  return nullptr;
}

/**
 * @brief Allocate reusable miniz storage from a bounded exporter arena.
 * @details Rejects multiplication overflow, reuses a released first-fit block,
 *          or appends one aligned block through ::mdl_export_workspace_take.
 * @param[in,out] opaque Pointer to the active ::mdl_zip_allocator_t.
 * @param[in] items Element count requested by miniz.
 * @param[in] size Bytes per requested element.
 * @return Maximum-aligned payload storage, or NULL.
 * @retval non-NULL The complete bounded request was satisfied.
 * @retval NULL Arguments overflowed or caller capacity was exhausted.
 * @pre @p opaque points to an active exclusive writer adapter.
 * @pre Its workspace data remains live and writable.
 * @post Success records exact requested bytes in one live block.
 * @post Failure sets the adapter exhaustion flag and performs no system allocation.
 * @note Not thread-safe for a shared adapter.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_zip_workspace_alloc(void* opaque, size_t items, size_t size)
{
  mdl_zip_allocator_t* alloc = (mdl_zip_allocator_t*)opaque;
  if ((alloc == nullptr) || (alloc->ws == nullptr) || (items == 0U) || (size == 0U) ||
      (items > (SIZE_MAX / size))) {
    if (alloc != nullptr) {
      alloc->exhausted = true;
    }
    return nullptr;
  }
  const size_t bytes = items * size;
  size_t       payload_span;
  if (!internal_zip_align_size(bytes, &payload_span) ||
      (payload_span > (SIZE_MAX - sizeof(mdl_zip_block_t)))) {
    alloc->exhausted = true;
    return nullptr;
  }
  uint8_t* const end = alloc->ws->data + alloc->ws->used;
  for (mdl_zip_block_t* block = alloc->first;
       (block != nullptr) && ((uint8_t*)block < end) && (block->span >= sizeof(*block));
       block = ((uint8_t*)block + block->span < end)
                 ? (mdl_zip_block_t*)((uint8_t*)block + block->span)
                 : nullptr) {
    if (block->free && ((block->span - sizeof(*block)) >= bytes)) {
      block->bytes = bytes;
      block->free  = false;
      return block + 1;
    }
  }
  const size_t     block_span = sizeof(mdl_zip_block_t) + payload_span;
  mdl_zip_block_t* block =
    (mdl_zip_block_t*)mdl_export_workspace_take(alloc->ws, block_span, _Alignof(max_align_t));
  if (block == nullptr) {
    alloc->exhausted = true;
    return nullptr;
  }
  block->span  = block_span;
  block->bytes = bytes;
  block->free  = false;
  if (alloc->first == nullptr) {
    alloc->first = block;
  }
  return block + 1;
}

/**
 * @brief Release a miniz block for reuse within the current writer.
 * @details Marks only blocks found in the current bounded adapter; no system
 *          deallocator or out-of-arena address is ever invoked.
 * @param[in,out] opaque Pointer to the active ::mdl_zip_allocator_t.
 * @param[in] address Payload returned by this adapter, or NULL.
 * @pre @p opaque is NULL or identifies the active writer allocator.
 * @pre @p address is NULL or was returned by the matching allocator.
 * @post A recognized block becomes available for first-fit reuse.
 * @post Workspace cursors and high-water accounting are unchanged.
 * @note Not thread-safe for a shared writer allocator.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_zip_workspace_free(void* opaque, void* address)
{
  mdl_zip_allocator_t* alloc = (mdl_zip_allocator_t*)opaque;
  mdl_zip_block_t*     block = internal_zip_find_block(alloc, address);
  if (block != nullptr) {
    block->free = true;
  }
}

/**
 * @brief Resize a miniz block within the bounded reusable arena.
 * @details Retains a sufficiently large block in place; otherwise obtains a
 *          replacement, copies the exact prior request, and releases the old block.
 * @param[in,out] opaque Pointer to the active ::mdl_zip_allocator_t.
 * @param[in] address Existing payload, or NULL for allocation semantics.
 * @param[in] items New element count.
 * @param[in] size Bytes per new element.
 * @return Resized payload storage, or NULL.
 * @retval non-NULL The request was satisfied with preserved prior bytes.
 * @retval NULL Input was invalid or bounded storage was exhausted.
 * @pre @p opaque points to an active exclusive writer adapter.
 * @pre Non-NULL @p address came from the matching adapter and is still live.
 * @post Success preserves the previous requested payload prefix.
 * @post Failure never invokes a system allocator or frees caller storage.
 * @note Not thread-safe for a shared adapter.
 * @since 0.1.0
 */
RA8_INTERNAL static void*
internal_zip_workspace_realloc(void* opaque, void* address, size_t items, size_t size)
{
  mdl_zip_allocator_t* alloc = (mdl_zip_allocator_t*)opaque;
  if (address == nullptr) {
    return internal_zip_workspace_alloc(opaque, items, size);
  }
  if ((alloc == nullptr) || (items == 0U) || (size == 0U) || (items > (SIZE_MAX / size))) {
    if (alloc != nullptr) {
      alloc->exhausted = true;
    }
    return nullptr;
  }
  mdl_zip_block_t* block = internal_zip_find_block(alloc, address);
  const size_t     bytes = items * size;
  if ((block == nullptr) || block->free) {
    alloc->exhausted = true;
    return nullptr;
  }
  if ((block->span - sizeof(*block)) >= bytes) {
    block->bytes = bytes;
    return address;
  }
  void* replacement = internal_zip_workspace_alloc(opaque, items, size);
  if (replacement == nullptr) {
    return nullptr;
  }
  memcpy(replacement, address, block->bytes);
  block->free = true;
  return replacement;
}

/**
 * @brief Bind one zeroed miniz archive to a bounded allocator.
 * @details Records the current workspace floor and installs allocation,
 *          release, and resize callbacks before miniz initialization.
 * @param[out] zip Archive descriptor to initialize.
 * @param[out] alloc Callback adapter with writer lifetime.
 * @param[in,out] ws Exclusive initialized exporter workspace.
 * @pre All pointer arguments are non-NULL.
 * @pre @p ws owns live writable storage through writer teardown.
 * @post @p zip has no default miniz allocator callback.
 * @post @p alloc records the cursor restored by ::priv_mdl_zip_workspace_release.
 * @note Not thread-safe for a shared workspace.
 * @since 0.1.0
 */
RA8_PRIV void priv_mdl_zip_workspace_bind(mz_zip_archive*         zip,
                                          mdl_zip_allocator_t*    alloc,
                                          mdl_export_workspace_t* ws)
{
  memset(zip, 0, sizeof(*zip));
  *alloc               = (mdl_zip_allocator_t){.ws = ws, .floor = ws->used};
  zip->m_pAlloc        = internal_zip_workspace_alloc;
  zip->m_pFree         = internal_zip_workspace_free;
  zip->m_pRealloc      = internal_zip_workspace_realloc;
  zip->m_pAlloc_opaque = alloc;
}

/**
 * @brief Release all miniz allocations while preserving high-water.
 * @details Restores the bump cursor to its pre-writer floor after miniz has
 *          ended, making transient writer storage reusable by later phases.
 * @param[in,out] alloc Active callback adapter, or NULL.
 * @pre @p alloc is NULL or its writer has ended or failed initialization.
 * @pre The associated workspace is not concurrently accessed.
 * @post A valid workspace cursor equals the saved floor.
 * @post `high_water` retains the largest observed allocation edge.
 * @note Not thread-safe for a shared workspace.
 * @since 0.1.0
 */
RA8_PRIV void priv_mdl_zip_workspace_release(mdl_zip_allocator_t* alloc)
{
  if ((alloc != nullptr) && (alloc->ws != nullptr)) {
    alloc->ws->used = alloc->floor;
  }
}

/**
 * @brief Map a miniz failure to bounded exhaustion when applicable.
 * @details Distinguishes an arena callback refusal from ordinary ZIP I/O or
 *          format failures so callers can size their explicit workspace.
 * @param[in] alloc Active callback adapter, or NULL.
 * @return Exporter error matching the recorded allocator state.
 * @retval k_ra8_err_invalid_size Bounded callback capacity was exhausted.
 * @retval k_ra8_fail No allocator exhaustion was recorded.
 * @pre @p alloc is NULL or remains alive through result classification.
 * @pre The writer has reported a failure before this helper is called.
 * @post No workspace counter or block is modified.
 * @post Identical allocator state produces an identical result.
 * @note Thread-safe for immutable distinct adapters.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_zip_workspace_error(const mdl_zip_allocator_t* alloc)
{
  return ((alloc != nullptr) && alloc->exhausted) ? k_ra8_err_invalid_size : k_ra8_fail;
}

/**
 * @brief Validate and canonicalize a cover not already present as a page
 * @details Recognizes an in-chapter page by exact name; otherwise requires a
 *          stable regular image file, sniffs its bytes, and hides the trusted
 *          host path behind `cover/cover.<actual-type>`.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] meta Metadata carrying the trusted cover path, or NULL.
 * @param[in] names Sorted chapter page rows.
 * @param[in] count Number of readable rows.
 * @param[out] cover Canonical external-cover descriptor.
 * @return Cover classification status.
 * @retval k_ra8_ok No external cover is needed or one was validated.
 * @retval k_ra8_err_invalid_arg The fixed path lacks a terminating NUL.
 * @retval k_ra8_err_invalid_size The canonical member path does not fit.
 * @retval k_ra8_err_validation_failed The source is absent, nonregular, or not an image.
 * @pre @p cover and @p names are valid for @p count rows.
 * @pre @p meta is NULL or exclusively stable for the call.
 * @post Success fully initializes @p cover.
 * @post An external source path is never copied into an archive member name.
 * @note Not thread-safe against concurrent replacement of the cover file.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_prepare_cover(mdl_storage_t*           storage,
                                                 const mdl_export_meta_t* meta,
                                                 char                     names[][k_name_max],
                                                 size_t                   count,
                                                 mdl_external_cover_t*    cover)
{
  memset(cover, 0, sizeof(*cover));
  if ((meta == nullptr) || (meta->cover_path[0] == '\0')) {
    return k_ra8_ok;
  }
  if (strnlen(meta->cover_path, sizeof(meta->cover_path)) == sizeof(meta->cover_path)) {
    return k_ra8_err_invalid_arg;
  }
  for (size_t i = 0U; i < count; ++i) {
    if (strcmp(meta->cover_path, names[i]) == 0) {
      return k_ra8_ok;
    }
  }

  fw_fs_stat_t    stat = {};
  const ra8_err_t err  = fw_fs_stat(&storage->fs->names, meta->cover_path, &stat);
  if ((err != k_ra8_ok) || !stat.exists || (stat.type != k_fw_fs_node_file) ||
      (stat.size_bytes == 0U)) {
    return k_ra8_err_validation_failed;
  }
  char ext[k_cover_ext_bytes];
  if (mdl_urlname_sniff_file(storage,
                             meta->cover_path,
                             nullptr,
                             ext,
                             sizeof(ext),
                             cover->mime,
                             sizeof(cover->mime)) != k_ra8_ok) {
    return k_ra8_err_validation_failed;
  }
  const int written = snprintf(cover->entry, sizeof(cover->entry), "cover/cover.%s", ext);
  if (!priv_mdl_export_snprintf_fit(written, sizeof(cover->entry))) {
    return k_ra8_err_invalid_size;
  }
  cover->source   = meta->cover_path;
  cover->external = true;
  return k_ra8_ok;
}

/**
 * @brief Add every discovered page to an initialized CBZ writer.
 * @details Builds each source path in bounded storage and stores the page
 * without recompression.
 * @param[in,out] zip Initialized ZIP writer.
 * @param[in,out] storage Injected portable page reader.
 * @param[in] dir Chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Number of page rows.
 * @return Whether all page members were added.
 * @retval true Every page was accepted by the writer.
 * @retval false At least one page could not be added.
 * @pre Paths and rows are valid, stable, and NUL-terminated.
 * @pre @p zip is initialized.
 * @post Success adds exactly @p count page members.
 * @post Failure leaves writer cleanup to the caller.
 * @note Not thread-safe for a shared writer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cbz_add_pages(mz_zip_archive* zip,
                                                     mdl_storage_t*  storage,
                                                     const char*     dir,
                                                     char            names[][k_name_max],
                                                     size_t          count)
{
  for (size_t i = 0U; i < count; ++i) {
    char      source[k_fw_fs_path_cap];
    ra8_err_t err = priv_mdl_export_path_join(source, sizeof(source), dir, names[i]);
    if (err == k_ra8_ok) {
      err = priv_mdl_export_zip_add_file(zip, storage, names[i], source, MZ_NO_COMPRESSION);
    }
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Build and append ComicInfo metadata to a CBZ writer.
 * @details Rebases an external cover to logical page zero before rendering.
 * @param[in,out] zip Initialized ZIP writer.
 * @param[in] meta Metadata to render, or NULL.
 * @param[in] count Number of discovered pages.
 * @param[in] cover Prepared external-cover state.
 * @param[in] allocator ZIP workspace allocator for error translation.
 * @return Metadata/member-addition status.
 * @retval k_ra8_ok ComicInfo.xml was added.
 * @retval k_ra8_err_invalid_size Metadata did not fit.
 * @retval k_ra8_fail ZIP member addition failed.
 * @pre @p zip, @p cover, and @p allocator are non-NULL.
 * @pre An external cover implies non-NULL @p meta.
 * @post Success adds one ComicInfo.xml member.
 * @post Failure leaves writer cleanup to the caller.
 * @note Not thread-safe for a shared writer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cbz_add_metadata(mz_zip_archive*             zip,
                                                        const mdl_export_meta_t*    meta,
                                                        size_t                      count,
                                                        const mdl_external_cover_t* cover,
                                                        const mdl_zip_allocator_t*  allocator)
{
  mdl_export_meta_t        cover_meta;
  const mdl_export_meta_t* comic_meta  = meta;
  size_t                   comic_pages = count;
  if (cover->external) {
    cover_meta             = *meta;
    cover_meta.cover_index = 0;
    comic_meta             = &cover_meta;
    ++comic_pages;
  }
  char            comic_xml[4096];
  const ra8_err_t rc =
    mdl_export_build_comicinfo_pages(comic_meta, comic_pages, comic_xml, sizeof(comic_xml));
  if (rc != k_ra8_ok) {
    return rc;
  }
  const ra8_err_t added = priv_mdl_export_zip_add_memory(zip,
                                                         "ComicInfo.xml",
                                                         (const uint8_t*)comic_xml,
                                                         strlen(comic_xml),
                                                         MZ_NO_COMPRESSION);
  return (added == k_ra8_ok) ? k_ra8_ok : priv_mdl_zip_workspace_error(allocator);
}

/**
 * @brief Write a CBZ with pages, canonical cover, and ComicInfo metadata
 * @details Stores each page without recompression, inserts an external cover
 *          first when present, and finalizes only after ComicInfo succeeds.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] dir NUL-terminated chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Number of pages to archive.
 * @param[in,out] output Active validated-publication output.
 * @param[in] meta Metadata to embed, or NULL.
 * @param[in,out] ws Exclusive caller-owned workspace.
 * @return CBZ writer status.
 * @retval k_ra8_ok The ZIP central directory finalized successfully.
 * @retval k_ra8_err_validation_failed An external cover is invalid.
 * @retval k_ra8_err_invalid_size Metadata or a bounded name does not fit.
 * @retval k_ra8_fail ZIP creation, member writing, or finalization failed.
 * @pre Paths and name rows are valid and stable.
 * @pre @p output owns an active transaction.
 * @post Success leaves a finalized CBZ with ComicInfo.
 * @post External cover paths are represented only by canonical member names.
 * @note Not thread-safe for the same output or changing source files.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_cbz(mdl_storage_t*           storage,
                                       const char*              dir,
                                       char                     names[][k_name_max],
                                       size_t                   count,
                                       mdl_export_output_t*     output,
                                       const mdl_export_meta_t* meta,
                                       mdl_export_workspace_t*  ws)
{
  mdl_external_cover_t cover;
  ra8_err_t            rc = priv_mdl_export_prepare_cover(storage, meta, names, count, &cover);
  if (rc != k_ra8_ok) {
    return rc;
  }
  mz_zip_archive      zip;
  mdl_zip_allocator_t zip_alloc;
  priv_mdl_zip_workspace_bind(&zip, &zip_alloc, ws);
  zip.m_pWrite     = priv_mdl_export_zip_write;
  zip.m_pIO_opaque = output;
  if (mz_zip_writer_init(&zip, 0) /* alloc-allow: callbacks use caller arena */ == MZ_FALSE) {
    const ra8_err_t zip_rc = priv_mdl_zip_workspace_error(&zip_alloc);
    priv_mdl_zip_workspace_release(&zip_alloc);
    return zip_rc;
  }
  if (cover.external) {
    rc = priv_mdl_export_zip_add_file(&zip, storage, cover.entry, cover.source, MZ_NO_COMPRESSION);
    if ((rc == k_ra8_fail) && zip_alloc.exhausted) {
      rc = k_ra8_err_invalid_size;
    }
  }
  if (rc == k_ra8_ok) {
    rc = internal_cbz_add_pages(&zip, storage, dir, names, count);
    if ((rc == k_ra8_fail) && zip_alloc.exhausted) {
      rc = k_ra8_err_invalid_size;
    }
  }
  if (rc == k_ra8_ok) {
    rc = internal_cbz_add_metadata(&zip, meta, count, &cover, &zip_alloc);
  }
  if ((rc == k_ra8_ok) &&
      (mz_zip_writer_finalize_archive(&zip) /* alloc-allow: callbacks use caller arena */ ==
       MZ_FALSE)) {
    rc = priv_mdl_zip_workspace_error(&zip_alloc);
  }
  (void)mz_zip_writer_end(&zip); /* alloc-allow: releases caller-arena blocks */
  priv_mdl_zip_workspace_release(&zip_alloc);
  if ((rc == k_ra8_fail) && (output->error != k_ra8_ok)) {
    rc = output->error;
  }
  return rc;
}
