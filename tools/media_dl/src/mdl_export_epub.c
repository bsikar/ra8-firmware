/**
 * @file mdl_export_epub.c
 * @brief Emit deterministic fixed-layout EPUB3 chapter containers.
 *
 * @details Owns EPUB media typing, page XHTML, cover integration, and final
 * ZIP assembly over bounded caller storage. The package document, navigation
 * document, and publication identifier are rendered by mdl_export_epub_meta.c.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "mdl_export.h"
#include "mdl_export_epub_internal.h"
#include "mdl_export_internal.h"
#include "mdl_sanitize.h"
#include "mdl_urlname.h"
#include "miniz.h"
#include "ra8_attributes.h"

/* --- EPUB (self-contained: a valid EPUB3 of the page images via miniz) ---- */

/** @brief OCF container pointing at the OPF package (fixed). */
static const char* const s_epub_container_xml =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<container version=\"1.0\" "
  "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
  "<rootfile full-path=\"OEBPS/content.opf\" "
  "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

/**
 * @brief Classify one page's image media type from sniffed portable bytes.
 * @details Joins the canonical path and sniffs the file's magic through
 *          injected storage. An unrecognized (but cleanly read) signature is
 *          reported as ::k_ra8_err_not_found rather than an error, so the
 *          caller can fall back to the suffix classifier.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] dir Canonical chapter directory.
 * @param[in] name Bounded page leaf name.
 * @param[out] out Borrowed canonical MIME pointer, valid only on k_ra8_ok.
 * @return Sniff classification or filesystem status.
 * @retval k_ra8_ok A magic-derived MIME was selected; @p out is valid.
 * @retval k_ra8_err_not_found The signature was read cleanly but unrecognized.
 * @retval k_ra8_err_invalid_size The composed path exceeded its bound.
 * @retval other A portable open, read, or close failure propagated.
 * @pre All pointers are non-NULL and paths are canonical under @p storage.
 * @pre @p name is NUL-terminated and fits the exporter filename contract.
 * @post The page file is consumed only through ::fw_fs_file_t.
 * @post Any status other than k_ra8_ok leaves @p out unchanged.
 * @note Not thread-safe for a shared storage workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_epub_media_type_from_sniff(mdl_storage_t* storage,
                                                                  const char*    dir,
                                                                  const char*    name,
                                                                  const char**   out)
{
  char src[k_fw_fs_path_cap];
  if (priv_mdl_export_path_join(src, sizeof(src), dir, name) != k_ra8_ok) {
    return k_ra8_err_invalid_size;
  }
  char            mime[64];
  const ra8_err_t sniff =
    mdl_urlname_sniff_file(storage, src, nullptr, nullptr, 0U, mime, sizeof(mime));
  if (sniff != k_ra8_ok) {
    return (sniff == k_ra8_err_validation_failed) ? k_ra8_err_not_found : sniff;
  }
  if (strcmp(mime, "image/png") == 0) {
    *out = "image/png";
    return k_ra8_ok;
  }
  if (strcmp(mime, "image/gif") == 0) {
    *out = "image/gif";
    return k_ra8_ok;
  }
  if (strcmp(mime, "image/webp") == 0) {
    *out = "image/webp";
    return k_ra8_ok;
  }
  if (strcmp(mime, "image/jpeg") == 0) {
    *out = "image/jpeg";
    return k_ra8_ok;
  }
  return k_ra8_err_not_found;
}

/**
 * @brief Classify one page's image media type from its filename suffix.
 * @details Falls back to JPEG when no recognized suffix is present, since
 *          the exporter must always publish some MIME for a manifest entry.
 * @param[in] name Bounded page leaf name.
 * @param[out] out Borrowed canonical MIME pointer.
 * @return Nothing; a MIME is always selected.
 * @pre @p name is NUL-terminated.
 * @pre @p out is non-NULL and addresses one writable pointer.
 * @post @p out addresses process-lifetime constant storage.
 * @post No file is opened and @p name is unchanged.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_epub_media_type_from_suffix(const char* name, const char** out)
{
  const char* dot = strrchr(name, '.');
  if (dot != nullptr) {
    if ((strcmp(dot, ".png") == 0) || (strcmp(dot, ".PNG") == 0)) {
      *out = "image/png";
      return;
    }
    if ((strcmp(dot, ".gif") == 0) || (strcmp(dot, ".GIF") == 0)) {
      *out = "image/gif";
      return;
    }
    if ((strcmp(dot, ".webp") == 0) || (strcmp(dot, ".WEBP") == 0)) {
      *out = "image/webp";
      return;
    }
    if ((strcmp(dot, ".bmp") == 0) || (strcmp(dot, ".BMP") == 0)) {
      *out = "image/bmp";
      return;
    }
  }
  *out = "image/jpeg";
}

/**
 * @brief Resolve one page's image media type from portable bytes then suffix.
 * @details Prefers recognized image magic read through the injected storage;
 *          only a clean unsupported signature falls back to the page suffix.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] dir Canonical chapter directory.
 * @param[in] name Bounded page leaf name.
 * @param[out] out Borrowed canonical MIME pointer.
 * @return Classification or filesystem status.
 * @retval k_ra8_ok A magic-derived or suffix fallback MIME was selected.
 * @retval k_ra8_err_invalid_size The composed path exceeded its bound.
 * @retval other A portable open, read, or close failure propagated.
 * @pre All pointers are non-NULL and paths are canonical under @p storage.
 * @pre @p name is NUL-terminated and fits the exporter filename contract.
 * @post Success initializes @p out to process-lifetime constant storage.
 * @post The page file is consumed only through ::fw_fs_file_t.
 * @note Not thread-safe for a shared storage workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_epub_media_type(mdl_storage_t* storage,
                                                       const char*    dir,
                                                       const char*    name,
                                                       const char**   out)
{
  if (dir != nullptr) {
    const ra8_err_t sniffed = internal_epub_media_type_from_sniff(storage, dir, name, out);
    if (sniffed == k_ra8_ok) {
      return k_ra8_ok;
    }
    if (sniffed != k_ra8_err_not_found) {
      return sniffed;
    }
  }
  internal_epub_media_type_from_suffix(name, out);
  return k_ra8_ok;
}

RA8_PRIV bool priv_mdl_epub_str_cat(char* dst, size_t cap, const char* text)
{
  const size_t cur = strlen(dst);
  const size_t add = strlen(text);
  if (cur + add + 1U > cap) {
    return false;
  }
  memcpy(dst + cur, text, add + 1U);
  return true;
}

RA8_PRIV bool priv_mdl_epub_add_str(mz_zip_archive* zip, const char* name, const char* body)
{
  return priv_mdl_export_zip_add_memory(zip,
                                        name,
                                        (const uint8_t*)body,
                                        strlen(body),
                                        MZ_NO_COMPRESSION) == k_ra8_ok;
}

/**
 * @brief Store and declare a validated external EPUB cover
 * @details Writes the file under its canonical OEBPS path and appends one
 *          cover-image manifest item using byte-derived MIME data.
 * @param[in,out] zip Initialized EPUB ZIP writer.
 * @param[in,out] storage Injected portable cover reader.
 * @param[in] cover Prepared cover descriptor.
 * @param[in,out] mani NUL-terminated manifest accumulator.
 * @param[in] cap Capacity of @p mani.
 * @return Cover-addition status.
 * @retval k_ra8_ok No external cover was requested or it was added.
 * @retval k_ra8_fail A path, ZIP, or manifest operation failed.
 * @pre All pointers are non-NULL and @p mani is terminated within @p cap.
 * @pre An external descriptor has already passed content validation.
 * @post Success adds at most one canonical cover member and declaration.
 * @post The trusted host source path is not exposed in the manifest.
 * @note Not thread-safe for a shared ZIP writer or manifest buffer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_epub_add_external_cover(mz_zip_archive*             zip,
                                                               mdl_storage_t*              storage,
                                                               const mdl_external_cover_t* cover,
                                                               char*                       mani,
                                                               size_t                      cap)
{
  if (!cover->external) {
    return k_ra8_ok;
  }
  char      archive_entry[k_epub_entry_max];
  const int entry_len = snprintf(archive_entry, sizeof(archive_entry), "OEBPS/%s", cover->entry);
  if (!priv_mdl_export_snprintf_fit(entry_len, sizeof(archive_entry)) ||
      (priv_mdl_export_zip_add_file(zip,
                                    storage,
                                    archive_entry,
                                    cover->source,
                                    MZ_NO_COMPRESSION) != k_ra8_ok)) {
    return k_ra8_fail;
  }
  char      frag[k_epub_frag_max];
  const int frag_len = snprintf(frag,
                                sizeof(frag),
                                "<item id=\"cover-image\" href=\"%s\" media-type=\"%s\" "
                                "properties=\"cover-image\"/>",
                                cover->entry,
                                cover->mime);
  return priv_mdl_export_snprintf_fit(frag_len, sizeof(frag)) &&
             priv_mdl_epub_str_cat(mani, cap, frag)
           ? k_ra8_ok
           : k_ra8_fail;
}

/**
 * @brief Append one page's manifest, spine, and navigation fragments
 * @details Builds all three bounded fragments from an escaped filename and
 *          marks the image manifest item when it is the logical cover.
 * @param[in,out] mani Manifest accumulator.
 * @param[in,out] spine Spine accumulator.
 * @param[in,out] nav Navigation accumulator.
 * @param[in] cap Capacity shared by all accumulators.
 * @param[in] esc_name XML-escaped image filename.
 * @param[in] media Image MIME string.
 * @param[in] idx Zero-based manifest identifier index.
 * @param[in] n One-based displayed page number.
 * @param[in] is_cover Whether to emit the cover-image property.
 * @return Fragment append status.
 * @retval k_ra8_ok All fragments fit.
 * @retval k_ra8_fail Formatting or any accumulator overflowed.
 * @pre All strings and accumulators are NUL-terminated within their bounds.
 * @pre @p mani, @p spine, and @p nav reference distinct writable buffers.
 * @post Success appends coherent identifiers across all three documents.
 * @post Failure is explicit and no truncated fragment is reported as valid.
 * @note Thread-safe across distinct accumulator sets.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_epub_append_frags(char*       mani,
                                                         char*       spine,
                                                         char*       nav,
                                                         size_t      cap,
                                                         const char* esc_name,
                                                         const char* media,
                                                         size_t      idx,
                                                         unsigned    n,
                                                         bool        is_cover)
{
  char        frag[k_epub_frag_max];
  const char* prop_attr = is_cover ? " properties=\"cover-image\"" : "";
  const int   fn        = snprintf(frag,
                                   sizeof(frag),
                                   "<item id=\"pg%zu\" href=\"page_%03u.xhtml\" "
                                   "media-type=\"application/xhtml+xml\"/>"
                                   "<item id=\"img%zu\" href=\"images/%s\" media-type=\"%s\"%s/>",
                                   idx,
                                   n,
                                   idx,
                                   esc_name,
                                   media,
                                   prop_attr);
  if (!priv_mdl_export_snprintf_fit(fn, sizeof(frag))) {
    return k_ra8_fail;
  }
  if (!priv_mdl_epub_str_cat(mani, cap, frag)) {
    return k_ra8_fail;
  }
  (void)snprintf(frag, sizeof(frag), "<itemref idref=\"pg%zu\"/>", idx);
  if (!priv_mdl_epub_str_cat(spine, cap, frag)) {
    return k_ra8_fail;
  }
  (void)snprintf(frag, sizeof(frag), "<li><a href=\"page_%03u.xhtml\">Page %u</a></li>", n, n);
  if (!priv_mdl_epub_str_cat(nav, cap, frag)) {
    return k_ra8_fail;
  }
  return k_ra8_ok;
}

/**
 * @brief Is one exported image the publication's declared cover?
 * @details Matches by declared cover index first, then by declared cover
 *          path name, so either identification method marks the same image.
 * @param[in] meta Resolved metadata controlling cover selection, or NULL.
 * @param[in] name Image file name being exported.
 * @param[in] idx Zero-based export position of @p name.
 * @return Whether this image is the declared cover.
 * @retval true @p idx matches the declared cover index, or @p name matches
 *         the declared cover path.
 * @retval false @p meta is NULL, or neither identification method matches.
 * @pre @p name is non-NULL and NUL-terminated.
 * @pre @p meta is NULL or holds a bounded NUL-terminated cover path.
 * @post No state is modified.
 * @post The result depends only on the supplied arguments.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_epub_page_is_cover(const mdl_export_meta_t* meta, const char* name, size_t idx)
{
  if (meta == nullptr) {
    return false;
  }
  if ((meta->cover_index >= 0) && ((size_t)meta->cover_index == idx)) {
    return true;
  }
  return (meta->cover_path[0] != '\0') && (strcmp(name, meta->cover_path) == 0);
}

/**
 * @brief Escape a page name and write its XHTML wrapper into the ZIP.
 * @details Escapes the source name for XML use, renders the fixed page
 *          markup, and writes it under the page's OEBPS entry name.
 * @param[in,out] zip Initialized EPUB ZIP writer.
 * @param[in] name Page filename.
 * @param[in] n One-based page number.
 * @param[out] esc Receives the escaped @p name for later fragment use.
 * @param[in] esc_cap Capacity of @p esc.
 * @return Page-XHTML write status.
 * @retval k_ra8_ok The escaped name and XHTML entry were written.
 * @retval k_ra8_fail Escaping, formatting, or ZIP writing failed.
 * @pre @p name is a NUL-terminated untrusted filename.
 * @pre @p esc addresses @p esc_cap writable bytes.
 * @post Success writes exactly one XHTML ZIP member.
 * @post Failure adds no member and @p esc holds no usable name.
 * @note Not thread-safe for a shared ZIP writer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_epub_write_page_xhtml(mz_zip_archive* zip,
                                                             const char*     name,
                                                             unsigned        n,
                                                             char*           esc,
                                                             size_t          esc_cap)
{
  if (!mdl_xml_escape(name, esc, esc_cap)) {
    return k_ra8_fail; /* untrusted filename must not break the container XML */
  }
  char      xhtml[k_epub_xhtml_max];
  const int xn = snprintf(xhtml,
                          sizeof(xhtml),
                          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Page %u"
                          "</title></head><body><img src=\"images/%s\" alt=\"Page %u\"/>"
                          "</body></html>",
                          n,
                          esc,
                          n);
  if (!priv_mdl_export_snprintf_fit(xn, sizeof(xhtml))) {
    return k_ra8_fail;
  }
  char entry[k_epub_entry_max];
  (void)snprintf(entry, sizeof(entry), "OEBPS/page_%03u.xhtml", n);
  if (!priv_mdl_epub_add_str(zip, entry, xhtml)) {
    return k_ra8_fail;
  }
  return k_ra8_ok;
}

/**
 * @brief Add one fixed-layout EPUB page and its image
 * @details Escapes the source name, writes page XHTML and stored image members,
 *          derives the real MIME where possible, and appends package fragments.
 * @param[in,out] storage Injected portable file reader.
 * @param[in,out] zip Initialized EPUB ZIP writer.
 * @param[in] dir Chapter directory.
 * @param[in] name Page filename.
 * @param[in] idx Zero-based page index.
 * @param[in,out] mani Manifest accumulator.
 * @param[in,out] spine Spine accumulator.
 * @param[in,out] nav Navigation accumulator.
 * @param[in] cap Capacity shared by accumulators.
 * @param[in] meta Metadata controlling cover selection, or NULL.
 * @return Page-addition status.
 * @retval k_ra8_ok Image, XHTML, and fragments were added.
 * @retval k_ra8_fail Escaping, formatting, ZIP writing, or append failed.
 * @pre Paths and accumulators are valid NUL-terminated strings.
 * @pre @p zip is initialized and accumulator buffers are distinct.
 * @post Success adds one XHTML and one image member.
 * @post Success appends matching manifest, spine, and navigation references.
 * @note Not thread-safe for a shared ZIP writer or buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_epub_add_page(mdl_storage_t*           storage,
                                                     mz_zip_archive*          zip,
                                                     const char*              dir,
                                                     const char*              name,
                                                     size_t                   idx,
                                                     char*                    mani,
                                                     char*                    spine,
                                                     char*                    nav,
                                                     size_t                   cap,
                                                     const mdl_export_meta_t* meta)
{
  const unsigned  n = (unsigned)(idx + 1U);
  char            esc[k_epub_name_esc_max];
  const ra8_err_t xhtml_err = internal_epub_write_page_xhtml(zip, name, n, esc, sizeof(esc));
  if (xhtml_err != k_ra8_ok) {
    return xhtml_err;
  }
  char src[k_fw_fs_path_cap];
  if (priv_mdl_export_path_join(src, sizeof(src), dir, name) != k_ra8_ok) {
    return k_ra8_err_invalid_size;
  }
  char      entry[k_epub_entry_max];
  const int en = snprintf(entry, sizeof(entry), "OEBPS/images/%s", name);
  if (!priv_mdl_export_snprintf_fit(en, sizeof(entry))) {
    return k_ra8_fail;
  }
  const ra8_err_t source_err =
    priv_mdl_export_zip_add_file(zip, storage, entry, src, MZ_NO_COMPRESSION);
  if (source_err != k_ra8_ok) {
    return source_err;
  }
  const bool      is_cover   = internal_epub_page_is_cover(meta, name, idx);
  const char*     media_type = nullptr;
  const ra8_err_t media_err  = internal_epub_media_type(storage, dir, name, &media_type);
  if (media_err != k_ra8_ok) {
    return media_err;
  }
  return internal_epub_append_frags(mani, spine, nav, cap, esc, media_type, idx, n, is_cover);
}

/**
 * @brief Select page metadata after accounting for an external cover.
 * @details Copies metadata only when the external cover consumes the cover
 *          role, clearing the page cover fields without mutating caller input.
 * @param[in] cover Prepared canonical cover descriptor.
 * @param[in] meta Resolved caller metadata.
 * @param[out] page_meta Caller-owned adjusted metadata storage.
 * @return Metadata object to use for page manifest generation.
 * @retval meta No external cover adjustment is required.
 * @retval page_meta An adjusted caller-owned copy is ready.
 * @pre All pointers are non-NULL and stable for the call.
 * @pre @p page_meta does not alias @p meta or @p cover.
 * @post Caller metadata and cover descriptor remain unchanged.
 * @post An adjusted result has no page cover index or cover path.
 * @note Thread-safe across distinct arguments.
 * @since 0.1.0
 */
RA8_INTERNAL static const mdl_export_meta_t*
internal_epub_page_meta(const mdl_external_cover_t* cover,
                        const mdl_export_meta_t*    meta,
                        mdl_export_meta_t*          page_meta)
{
  if (!cover->external) {
    return meta;
  }
  *page_meta               = *meta;
  page_meta->cover_index   = -1;
  page_meta->cover_path[0] = '\0';
  return page_meta;
}

/**
 * @brief Carve the five bounded XML/ZIP accumulators from workspace arena.
 * @details Reserves one @p cap-byte block per accumulator and NUL-terminates
 *          the three accumulators the caller builds incrementally.
 * @param[in,out] ws Exclusive caller-owned workspace.
 * @param[in] cap Byte capacity reserved for each accumulator.
 * @param[out] mani Receives the manifest accumulator.
 * @param[out] spine Receives the spine accumulator.
 * @param[out] nav Receives the navigation accumulator.
 * @param[out] opf Receives the OPF render buffer.
 * @param[out] navdoc Receives the navigation-document render buffer.
 * @return Workspace-carve status.
 * @retval k_ra8_ok All five accumulators were reserved.
 * @retval k_ra8_err_invalid_size The workspace arena is exhausted.
 * @pre @p ws is exclusive and owns writable arena storage.
 * @pre The five out-pointers are non-NULL and address distinct storage.
 * @post On success every out-pointer is non-NULL and the three text
 *       accumulators are empty NUL-terminated strings.
 * @post Failure terminates no accumulator and publishes no usable block.
 * @note Not thread-safe for a shared workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_epub_carve_workspace(mdl_export_workspace_t* ws,
                                                            size_t                  cap,
                                                            char**                  mani,
                                                            char**                  spine,
                                                            char**                  nav,
                                                            char**                  opf,
                                                            char**                  navdoc)
{
  *mani   = (char*)mdl_export_workspace_take(ws, cap, 1U);
  *spine  = (char*)mdl_export_workspace_take(ws, cap, 1U);
  *nav    = (char*)mdl_export_workspace_take(ws, cap, 1U);
  *opf    = (char*)mdl_export_workspace_take(ws, cap, 1U);
  *navdoc = (char*)mdl_export_workspace_take(ws, cap, 1U);
  if ((*mani == nullptr) || (*spine == nullptr) || (*nav == nullptr) || (*opf == nullptr) ||
      (*navdoc == nullptr)) {
    return k_ra8_err_invalid_size;
  }
  (*mani)[0]  = '\0';
  (*spine)[0] = '\0';
  (*nav)[0]   = '\0';
  return k_ra8_ok;
}

/**
 * @brief End the ZIP writer and translate exhaustion/output faults to status.
 * @details Always ends an opened writer before releasing the workspace
 *          allocator, then maps a generic write failure to the more specific
 *          exhaustion or captured-output error when one is available.
 * @param[in,out] zip ZIP writer to end when @p zip_open.
 * @param[in] zip_open Whether @p zip was successfully initialized.
 * @param[in,out] zip_alloc Workspace allocator bound to @p zip.
 * @param[in] rc Status accumulated by the writer stages.
 * @param[in] output Validated-publication output whose captured error may
 *            refine a generic write failure.
 * @return Refined writer status.
 * @retval k_ra8_err_invalid_size @p rc was k_ra8_fail and the arena was exhausted.
 * @retval other @p rc, or @p output->error when it refines a generic failure.
 * @pre @p zip_alloc is bound to @p zip via ::priv_mdl_zip_workspace_bind.
 * @pre @p output is the validated-publication output bound to the writer.
 * @post The ZIP writer is ended and the workspace allocator is released.
 * @post A failing @p rc is never refined into a success status.
 * @note Not thread-safe for a shared workspace or writer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_epub_finish_writer(mz_zip_archive*            zip,
                                                          bool                       zip_open,
                                                          mdl_zip_allocator_t*       zip_alloc,
                                                          ra8_err_t                  rc,
                                                          const mdl_export_output_t* output)
{
  if (zip_open) {
    (void)mz_zip_writer_end(zip); /* alloc-allow: releases caller-arena blocks */
  }
  if ((rc == k_ra8_fail) && zip_alloc->exhausted) {
    rc = k_ra8_err_invalid_size;
  }
  if ((rc == k_ra8_fail) && (output->error != k_ra8_ok)) {
    rc = output->error;
  }
  priv_mdl_zip_workspace_release(zip_alloc);
  return rc;
}

/**
 * @brief Package chapter pages into a valid fixed-layout EPUB3
 * @details Carves all XML accumulators from caller storage, adds OCF roots,
 *          canonical cover, page members, metadata, and finalizes the staged archive.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] dir Chapter directory.
 * @param[in] names Sorted page-name rows.
 * @param[in] count Number of page rows.
 * @param[in,out] output Active validated-publication output.
 * @param[in] meta Metadata to encode, or NULL.
 * @param[in,out] ws Exclusive caller-owned workspace.
 * @return EPUB writer status.
 * @retval k_ra8_ok A complete finalized EPUB was written.
 * @retval k_ra8_err_invalid_size Workspace or metadata bounds were exceeded.
 * @retval k_ra8_err_validation_failed External cover validation failed.
 * @retval k_ra8_fail ZIP, XHTML, or metadata writing failed.
 * @pre Paths and rows are valid, stable, and NUL-terminated.
 * @pre @p ws is exclusive and owns writable arena storage.
 * @post The ZIP writer is ended on every initialized path.
 * @post Success includes required mimetype, container, OPF, and navigation members.
 * @note Not thread-safe for shared output or workspace.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_export_epub(mdl_storage_t*           storage,
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
  const size_t cap = (size_t)k_epub_base_bytes + (count * (size_t)k_epub_per_page_bytes);
  char*        mani;
  char*        spine;
  char*        nav;
  char*        opf;
  char*        navdoc;
  rc = internal_epub_carve_workspace(ws, cap, &mani, &spine, &nav, &opf, &navdoc);
  if (rc != k_ra8_ok) {
    return rc;
  }
  mz_zip_archive      zip;
  mdl_zip_allocator_t zip_alloc;
  priv_mdl_zip_workspace_bind(&zip, &zip_alloc, ws);
  zip.m_pWrite        = priv_mdl_export_zip_write;
  zip.m_pIO_opaque    = output;
  const bool zip_open = (mz_zip_writer_init(&zip, 0) != MZ_FALSE);
  rc                  = zip_open ? k_ra8_ok : priv_mdl_zip_workspace_error(&zip_alloc);
  if ((rc == k_ra8_ok) &&
      (!priv_mdl_epub_add_str(&zip, "mimetype", "application/epub+zip") ||
       !priv_mdl_epub_add_str(&zip, "META-INF/container.xml", s_epub_container_xml))) {
    rc = k_ra8_fail;
  }
  if (rc == k_ra8_ok) {
    rc = internal_epub_add_external_cover(&zip, storage, &cover, mani, cap);
  }
  mdl_export_meta_t        page_meta;
  const mdl_export_meta_t* page_meta_ptr = internal_epub_page_meta(&cover, meta, &page_meta);
  for (size_t i = 0U; (rc == k_ra8_ok) && (i < count); ++i) {
    rc =
      internal_epub_add_page(storage, &zip, dir, names[i], i, mani, spine, nav, cap, page_meta_ptr);
  }
  if (rc == k_ra8_ok) {
    rc = priv_mdl_epub_add_meta(&zip, mani, spine, nav, count, meta, opf, cap, navdoc, cap);
  }
  /* The coordinator aborts this stage unless writer completion and verification pass. */
  return internal_epub_finish_writer(&zip, zip_open, &zip_alloc, rc, output);
}
