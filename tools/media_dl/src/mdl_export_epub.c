/**
 * @file mdl_export_epub.c
 * @brief Emit deterministic fixed-layout EPUB3 chapter containers.
 *
 * @details Owns EPUB media typing, XHTML, OPF, navigation, UUID derivation,
 * cover integration, and final ZIP assembly over bounded caller storage.
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
#include "mdl_export_internal.h"
#include "mdl_sanitize.h"
#include "mdl_urlname.h"
#include "miniz.h"
#include "ra8_attributes.h"

/** @brief Bounded EPUB text expansion sizes. */
typedef enum : uint16_t {
  k_epub_creator_factor = 12U,  /**< Writer plus artist expansion factor. */
  k_epub_fragment_slack = 64U,  /**< Fixed bytes around an XML fragment.  */
  k_epub_creator_slack  = 128U, /**< Fixed bytes around all creators.     */
  k_epub_uuid_byte_bits = 8U,   /**< Bits consumed per UUID hash byte.    */
} mdl_epub_text_bounds_t;

/** @brief FNV-1a values used to derive deterministic publication UUIDs. */
typedef enum : uint64_t {
  k_uuid_fnv_prime = UINT64_C(1099511628211),        /**< FNV-1a 64-bit prime.      */
  k_uuid_seed_one  = UINT64_C(14695981039346656037), /**< Primary FNV offset basis. */
  k_uuid_seed_two  = UINT64_C(7809847782465536322),  /**< Independent second seed.  */
} mdl_uuid_hash_t;

/** @brief RFC 4122 UUID byte layout, masks, and version/variant bits. */
typedef enum : uint8_t {
  k_uuid_separator       = 0xFFU, /**< Separator mixed between hash fields. */
  k_uuid_byte_count      = 16U,   /**< Bytes in an RFC 4122 UUID.           */
  k_uuid_half_bytes      = 8U,    /**< Bytes contributed by each hash.      */
  k_uuid_top_shift       = 56U,   /**< Shift selecting the top hash byte.   */
  k_uuid_version_byte    = 6U,    /**< UUID version field byte index.       */
  k_uuid_version_mask    = 0x0FU, /**< Mask retaining non-version bits.     */
  k_uuid_version_five    = 0x50U, /**< RFC 4122 version-five field bits.    */
  k_uuid_variant_byte    = 8U,    /**< UUID variant field byte index.       */
  k_uuid_variant_mask    = 0x3FU, /**< Mask retaining non-variant bits.     */
  k_uuid_variant_rfc4122 = 0x80U, /**< RFC 4122 variant field bits.         */
  k_uuid_node_offset     = 10U,   /**< First byte of the UUID node field.   */
  k_uuid_last_byte       = 15U,   /**< Final UUID byte index.               */
} mdl_uuid_layout_t;

/**
 * @brief Mix one NUL-terminated metadata field into a UUID hash
 * @details Applies 64-bit FNV-1a and a separator byte so adjacent fields cannot
 *          collapse into the same concatenated hash stream.
 * @param[in] hash Incoming hash state.
 * @param[in] text NUL-terminated metadata field.
 * @return Updated hash state after the field and separator.
 * @retval uint64_t Deterministic updated FNV state.
 * @pre @p text is non-NULL and NUL-terminated.
 * @pre @p hash is the state for all preceding canonical fields.
 * @post @p text is not modified.
 * @post Equal input states and text produce equal output.
 * @note Thread-safe: this is a pure hash helper.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_uuid_hash_text(uint64_t hash, const char* text)
{
  while (*text != '\0') {
    hash ^= (uint8_t)*text++;
    hash *= (uint64_t)k_uuid_fnv_prime;
  }
  hash ^= (uint8_t)k_uuid_separator;
  return hash * (uint64_t)k_uuid_fnv_prime;
}

/**
 * @brief Derive a stable RFC-4122-shaped identifier from canonical metadata
 * @details Hashes canonical fields in both directions and sets version-five and
 *          RFC variant bits before formatting a URN into caller storage.
 * @param[out] out Destination UUID string buffer.
 * @param[in] cap Writable capacity of @p out.
 * @param[in] meta Metadata to hash, or NULL for defaults.
 * @pre @p out is non-NULL and addresses @p cap writable bytes.
 * @pre @p meta is NULL or contains bounded NUL-terminated fields.
 * @post @p out contains a deterministic NUL-terminated UUID URN when capacity permits.
 * @post Input metadata remains unchanged.
 * @note Thread-safe across distinct output buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_generate_uuid(char* out, size_t cap, const mdl_export_meta_t* meta)
{
  mdl_export_meta_t m;
  if (meta != nullptr) {
    m = *meta;
  } else {
    mdl_meta_init(&m);
  }
  const char* fields[] =
    {m.series_title, m.chapter_title, m.writer, m.artist, m.language, m.source_url, m.cover_path};
  uint64_t h1 = (uint64_t)k_uuid_seed_one;
  uint64_t h2 = (uint64_t)k_uuid_seed_two;
  for (size_t i = 0U; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
    h1 = internal_uuid_hash_text(h1, fields[i]);
    h2 = internal_uuid_hash_text(h2, fields[(sizeof(fields) / sizeof(fields[0])) - 1U - i]);
  }
  uint8_t b[k_uuid_byte_count];
  for (size_t i = 0U; i < (size_t)k_uuid_half_bytes; ++i) {
    b[i] = (uint8_t)(h1 >> ((uint8_t)k_uuid_top_shift - (i * (size_t)k_epub_uuid_byte_bits)));
    b[i + (size_t)k_uuid_half_bytes] =
      (uint8_t)(h2 >> ((uint8_t)k_uuid_top_shift - (i * (size_t)k_epub_uuid_byte_bits)));
  }
  b[k_uuid_version_byte] =
    (uint8_t)((b[k_uuid_version_byte] & k_uuid_version_mask) | k_uuid_version_five);
  b[k_uuid_variant_byte] =
    (uint8_t)((b[k_uuid_variant_byte] & k_uuid_variant_mask) | k_uuid_variant_rfc4122);
  (void)snprintf(out,
                 cap,
                 "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%"
                 "02x%02x%02x%02x%02x",
                 b[0],
                 b[1],
                 b[2],
                 b[3],
                 b[4],
                 b[5],
                 b[6],
                 b[7],
                 b[8],
                 b[9],
                 b[k_uuid_node_offset],
                 b[k_uuid_node_offset + 1U],
                 b[k_uuid_node_offset + 2U],
                 b[k_uuid_node_offset + 3U],
                 b[k_uuid_node_offset + 4U],
                 b[k_uuid_last_byte]);
}

/* --- EPUB (self-contained: a valid EPUB3 of the page images via miniz) ---- */

/**
 * @brief EPUB string-buffer sizing (grows with the page count).
 * @details Sized for the WORST case, not the typical `page_NNN.jpg`: a page
 *          filename may be up to ::k_name_max bytes and, XML-escaped, expand
 *          6x (a name of all `&quot;`). That escaped name is embedded once in
 *          the page's manifest fragment and once in its xhtml document, so both
 *          the fragment buffer and the per-page accumulator budget must exceed
 *          the fixed template text plus ::k_epub_name_esc_max. Undersizing here
 *          does not truncate silently -- ::internal_str_cat and ::priv_mdl_export_snprintf_fit report it
 *          and the export fails -- but correct sizing is what lets a legitimate
 *          long-name chapter package rather than error.
 */
typedef enum : uint32_t {
  k_epub_name_esc_max   = 1536U, /**< XML-escaped page name (k_name_max * 6).       */
  k_epub_frag_max       = 2048U, /**< One manifest fragment (fixed + escaped name). */
  k_epub_xhtml_max      = 2048U, /**< One page's xhtml document (embeds the name).  */
  k_epub_entry_max      = 320U,  /**< A zip entry path ("OEBPS/images/" + name).    */
  k_epub_base_bytes     = 4096U, /**< Fixed opf/nav overhead.                       */
  k_epub_per_page_bytes = 2048U, /**< Per-page opf/nav accumulator growth.          */
  k_epub_workspace_cap  = k_epub_base_bytes + (k_max_pages * k_epub_per_page_bytes),
  /**< Maximum bounded XML accumulator bytes. */
} mdl_epub_size_t;

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
 * @post The page file is consumed only through ::fw_fs_file_t.
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
 * @post @p out addresses process-lifetime constant storage.
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

/**
 * @brief Append `text` to NUL-terminated `dst`; report whether it fully fit.
 * @details Never truncates: if the append (plus its NUL) would not fit in
 *          @p cap it leaves @p dst unchanged and returns false, so the caller
 *          can fail loudly rather than emit a manifest cut off mid-element.
 * @return true when the whole of @p text was appended, false if it would
 * overrun.
 * @param[in,out] dst Existing NUL-terminated accumulator.
 * @param[in] cap Total writable capacity of @p dst.
 * @param[in] text NUL-terminated text to append.
 * @retval true The complete text and terminator fit.
 * @retval false Capacity is insufficient and @p dst is unchanged.
 * @pre @p dst and @p text are non-NULL and do not overlap.
 * @pre @p dst is NUL-terminated within @p cap.
 * @post Success appends the complete source string.
 * @post Failure preserves the original destination.
 * @note Thread-safe across distinct accumulators.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_str_cat(char* dst, size_t cap, const char* text)
{
  const size_t cur = strlen(dst);
  const size_t add = strlen(text);
  if (cur + add + 1U > cap) {
    return false;
  }
  memcpy(dst + cur, text, add + 1U);
  return true;
}

/**
 * @brief Add an in-memory string as a stored ZIP entry
 * @details Passes the complete string length to miniz without compression.
 * @param[in,out] zip Initialized ZIP writer.
 * @param[in] name Safe NUL-terminated member name.
 * @param[in] body NUL-terminated member body.
 * @return Whether miniz accepted the complete member.
 * @retval true The entry was added.
 * @retval false The ZIP writer rejected it.
 * @pre All pointers are non-NULL and strings are NUL-terminated.
 * @pre @p zip remains initialized for writing.
 * @post Success adds exactly one stored member.
 * @post Input strings remain unchanged.
 * @note Not thread-safe for a shared ZIP writer.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_epub_add_str(mz_zip_archive* zip, const char* name, const char* body)
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
  return priv_mdl_export_snprintf_fit(frag_len, sizeof(frag)) && internal_str_cat(mani, cap, frag)
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
  if (!internal_str_cat(mani, cap, frag)) {
    return k_ra8_fail;
  }
  (void)snprintf(frag, sizeof(frag), "<itemref idref=\"pg%zu\"/>", idx);
  if (!internal_str_cat(spine, cap, frag)) {
    return k_ra8_fail;
  }
  (void)snprintf(frag, sizeof(frag), "<li><a href=\"page_%03u.xhtml\">Page %u</a></li>", n, n);
  if (!internal_str_cat(nav, cap, frag)) {
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
 * @pre @p name is non-NULL.
 * @post No state is modified.
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
  if (!internal_epub_add_str(zip, entry, xhtml)) {
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

/** @brief Escaped and rendered EPUB publication metadata. */
typedef struct {
  char identifier[k_mdl_meta_id_max * 6U]; /**< Escaped identifier.        */
  char title[k_mdl_meta_title_max * 6U];   /**< Escaped display title.     */
  char language[k_mdl_meta_lang_max * 6U]; /**< Escaped language.          */
  char modified[k_mdl_meta_date_max * 6U]; /**< Escaped modification date. */
  char creators[k_mdl_meta_name_max * k_epub_creator_factor + k_epub_creator_slack];
  /**< Complete escaped `<dc:creator>` fragments. */
  char description[k_mdl_meta_summary_max * 6U + k_epub_fragment_slack];
  /**< Optional escaped `<dc:description>` fragment. */
  char source[k_mdl_meta_url_max * 6U + k_epub_fragment_slack];
  /**< Optional escaped `<dc:source>` fragment. */
  const char* progression; /**< OPF page progression token. */
} mdl_epub_meta_text_t;

/**
 * @brief Render the EPUB creator fragments with their original fallback.
 * @details Builds epub prepare creators within caller-owned bounded workspace and reports capacity, encoding, or I/O failure without transferring workspace ownership.
 * @param[in] meta Resolved export metadata.
 * @param[out] text Prepared EPUB text storage.
 * @pre Both pointers are non-NULL.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post @p text contains at least one creator fragment.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note Thread-safe across distinct objects.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_epub_prepare_creators(const mdl_export_meta_t* meta,
                                                        mdl_epub_meta_text_t*    text)
{
  text->creators[0] = '\0';
  if (meta->writer[0] != '\0') {
    char escaped[k_mdl_meta_name_max * 6U];
    char fragment[k_mdl_meta_name_max * 6U + k_epub_fragment_slack];
    (void)mdl_xml_escape(meta->writer, escaped, sizeof(escaped));
    (void)
      snprintf(fragment, sizeof(fragment), "<dc:creator opf:role=\"aut\">%s</dc:creator>", escaped);
    (void)internal_str_cat(text->creators, sizeof(text->creators), fragment);
  }
  if (meta->artist[0] != '\0') {
    char escaped[k_mdl_meta_name_max * 6U];
    char fragment[k_mdl_meta_name_max * 6U + k_epub_fragment_slack];
    (void)mdl_xml_escape(meta->artist, escaped, sizeof(escaped));
    (void)
      snprintf(fragment, sizeof(fragment), "<dc:creator opf:role=\"art\">%s</dc:creator>", escaped);
    (void)internal_str_cat(text->creators, sizeof(text->creators), fragment);
  }
  if (text->creators[0] == '\0') {
    (void)snprintf(text->creators, sizeof(text->creators), "<dc:creator>media_dl</dc:creator>");
  }
}

/**
 * @brief Render optional EPUB description and source fragments.
 * @details Builds epub prepare optional within caller-owned bounded workspace and reports capacity, encoding, or I/O failure without transferring workspace ownership.
 * @param[in] meta Resolved and URL-validated metadata.
 * @param[out] text Prepared EPUB text storage.
 * @return Optional-fragment status.
 * @retval k_ra8_ok Every present optional value fit.
 * @retval k_ra8_err_invalid_size Escaped source storage was insufficient.
 * @pre Both pointers are non-NULL.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post Absent fields leave empty fragments.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note Thread-safe across distinct objects.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_epub_prepare_optional(const mdl_export_meta_t* meta,
                                                             mdl_epub_meta_text_t*    text)
{
  text->description[0] = '\0';
  if (meta->summary[0] != '\0') {
    char escaped[k_mdl_meta_summary_max * 6U];
    (void)mdl_xml_escape(meta->summary, escaped, sizeof(escaped));
    (void)snprintf(text->description,
                   sizeof(text->description),
                   "<dc:description>%s</dc:description>",
                   escaped);
  }
  text->source[0] = '\0';
  if (meta->source_url[0] != '\0') {
    char escaped[k_mdl_meta_url_max * 6U];
    if (!mdl_xml_escape(meta->source_url, escaped, sizeof(escaped))) {
      return k_ra8_err_invalid_size;
    }
    const int written =
      snprintf(text->source, sizeof(text->source), "<dc:source>%s</dc:source>", escaped);
    if (!priv_mdl_export_snprintf_fit(written, sizeof(text->source))) {
      return k_ra8_err_invalid_size;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Prepare all escaped EPUB metadata text.
 * @details Builds epub prepare text within caller-owned bounded workspace and reports capacity, encoding, or I/O failure without transferring workspace ownership.
 * @param[in] meta Resolved and URL-validated metadata.
 * @param[out] text Prepared EPUB text storage.
 * @return Text-preparation status.
 * @retval k_ra8_ok Required and optional fields fit.
 * @retval k_ra8_err_invalid_size Escaped bounded storage was insufficient.
 * @pre Both pointers are non-NULL.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post Success initializes every field of @p text.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note Thread-safe across distinct objects.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_epub_prepare_text(const mdl_export_meta_t* meta,
                                                         mdl_epub_meta_text_t*    text)
{
  char raw_id[k_mdl_meta_id_max];
  if (meta->identifier[0] != '\0') {
    (void)snprintf(raw_id, sizeof(raw_id), "%s", meta->identifier);
  } else {
    internal_generate_uuid(raw_id, sizeof(raw_id), meta);
  }
  if (!mdl_xml_escape(raw_id, text->identifier, sizeof(text->identifier)) ||
      !mdl_xml_escape(meta->language, text->language, sizeof(text->language)) ||
      !mdl_xml_escape(meta->modified, text->modified, sizeof(text->modified))) {
    return k_ra8_err_invalid_size;
  }
  const char* raw_title;
  if (meta->chapter_title[0] != '\0') {
    raw_title = meta->chapter_title;
  } else if (meta->series_title[0] != '\0') {
    raw_title = meta->series_title;
  } else {
    raw_title = "chapter";
  }
  if (!mdl_xml_escape(raw_title, text->title, sizeof(text->title))) {
    (void)snprintf(text->title, sizeof(text->title), "chapter");
  }
  text->progression = (meta->reading_direction == k_mdl_read_rtl) ? "rtl" : "ltr";
  internal_epub_prepare_creators(meta, text);
  return internal_epub_prepare_optional(meta, text);
}

/**
 * @brief Render and append prepared EPUB package documents.
 * @details Builds epub render meta within caller-owned bounded workspace and reports capacity, encoding, or I/O failure without transferring workspace ownership.
 * @param[in,out] zip Initialized EPUB writer.
 * @param[in] mani Manifest fragments.
 * @param[in] spine Spine fragments.
 * @param[in] nav Navigation fragments.
 * @param[in] page_count Logical page count.
 * @param[in] text Prepared metadata text.
 * @param[out] opf OPF workspace.
 * @param[in] opf_cap OPF workspace capacity.
 * @param[out] navdoc Navigation workspace.
 * @param[in] nav_cap Navigation workspace capacity.
 * @return Document/finalization status.
 * @retval k_ra8_ok Documents were added and archive finalized.
 * @retval k_ra8_err_invalid_size Required workspace was insufficient.
 * @retval k_ra8_fail Rendering, member addition, or finalization failed.
 * @pre All pointers are non-NULL and input strings are NUL-terminated.
 * @pre Supplied capacities cover their referenced bounded buffers.
 * @post Success finalizes the EPUB central directory.
 * @post Result status and outputs describe one completed synchronous attempt.
 * @note Not thread-safe for a shared writer or buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_epub_render_meta(mz_zip_archive*             zip,
                                                        const char*                 mani,
                                                        const char*                 spine,
                                                        const char*                 nav,
                                                        size_t                      page_count,
                                                        const mdl_epub_meta_text_t* text,
                                                        char*                       opf,
                                                        size_t                      opf_cap,
                                                        char*                       navdoc,
                                                        size_t                      nav_cap)
{
  const size_t opf_need = strlen(mani) + strlen(spine) + strlen(text->creators) +
                          strlen(text->description) + strlen(text->source) +
                          (size_t)k_epub_base_bytes;
  const size_t nav_need = strlen(nav) + (size_t)k_epub_base_bytes;
  if ((opf_need > opf_cap) || (nav_need > nav_cap)) {
    return k_ra8_err_invalid_size;
  }
  const int opf_n =
    snprintf(opf,
             opf_need,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
             "unique-identifier=\"bookid\"><metadata "
             "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
             "xmlns:opf=\"http://www.idpf.org/2007/opf\">"
             "<dc:identifier id=\"bookid\">%s</dc:identifier>"
             "<dc:title>%s</dc:title>%s%s%s<dc:language>%s</dc:language>"
             "<meta property=\"dcterms:modified\">%s</meta>"
             "<meta property=\"schema:numberOfPages\">%zu</meta>"
             "</metadata><manifest><item id=\"nav\" href=\"nav.xhtml\" "
             "media-type=\"application/xhtml+xml\" properties=\"nav\"/>%s</manifest>"
             "<spine page-progression-direction=\"%s\">%s</spine></package>",
             text->identifier,
             text->title,
             text->creators,
             text->description,
             text->source,
             text->language,
             text->modified,
             page_count,
             mani,
             text->progression,
             spine);
  const int  nav_n = snprintf(navdoc,
                              nav_need,
                              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                              "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
                              "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>Contents"
                              "</title></head><body><nav epub:type=\"toc\"><ol>%s</ol></nav>"
                              "</body></html>",
                              nav);
  const bool ok    = priv_mdl_export_snprintf_fit(opf_n, opf_need) &&
                     priv_mdl_export_snprintf_fit(nav_n, nav_need) &&
                     internal_epub_add_str(zip, "OEBPS/content.opf", opf) &&
                     internal_epub_add_str(zip, "OEBPS/nav.xhtml", navdoc) &&
                     (mz_zip_writer_finalize_archive(zip) != MZ_FALSE);
  return ok ? k_ra8_ok : k_ra8_fail;
}

/**
 * @brief Build EPUB package metadata and finalize the archive
 * @details Escapes deterministic metadata, composes bounded OPF/navigation
 *          documents, adds them to the ZIP, and writes the central directory.
 * @param[in,out] zip Initialized EPUB ZIP writer.
 * @param[in] mani Complete manifest fragments.
 * @param[in] spine Complete spine fragments.
 * @param[in] nav Complete navigation fragments.
 * @param[in] page_count Logical reading-order page count.
 * @param[in] meta Metadata to encode, or NULL.
 * @param[out] opf Caller workspace for content.opf.
 * @param[in] opf_buf_cap Capacity of @p opf.
 * @param[out] navdoc Caller workspace for nav.xhtml.
 * @param[in] nav_buf_cap Capacity of @p navdoc.
 * @return Metadata/finalization status.
 * @retval k_ra8_ok Both documents were added and ZIP finalized.
 * @retval k_ra8_err_invalid_size Required bounded XML storage is insufficient.
 * @retval k_ra8_fail Formatting, member addition, or finalization failed.
 * @pre All document strings are NUL-terminated and pointers are non-NULL.
 * @pre Output buffers are distinct and @p zip is initialized.
 * @post Success leaves a finalized EPUB central directory.
 * @post Failure is explicit and the caller owns temp cleanup.
 * @note Not thread-safe for a shared ZIP writer or buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_epub_add_meta(mz_zip_archive*          zip,
                                                     const char*              mani,
                                                     const char*              spine,
                                                     const char*              nav,
                                                     size_t                   page_count,
                                                     const mdl_export_meta_t* meta,
                                                     char*                    opf,
                                                     size_t                   opf_buf_cap,
                                                     char*                    navdoc,
                                                     size_t                   nav_buf_cap)
{
  mdl_export_meta_t resolved;
  if (meta != nullptr) {
    resolved = *meta;
  } else {
    mdl_meta_init(&resolved);
  }
  ra8_err_t            rc   = priv_mdl_export_validate_source_url(resolved.source_url);
  mdl_epub_meta_text_t text = {};
  if (rc == k_ra8_ok) {
    rc = internal_epub_prepare_text(&resolved, &text);
  }
  if (rc != k_ra8_ok) {
    return rc;
  }
  return internal_epub_render_meta(zip,
                                   mani,
                                   spine,
                                   nav,
                                   page_count,
                                   &text,
                                   opf,
                                   opf_buf_cap,
                                   navdoc,
                                   nav_buf_cap);
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
 * @post On success every out-pointer is non-NULL and the three text
 *       accumulators are empty NUL-terminated strings.
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
 * @post The ZIP writer is ended and the workspace allocator is released.
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
      (!internal_epub_add_str(&zip, "mimetype", "application/epub+zip") ||
       !internal_epub_add_str(&zip, "META-INF/container.xml", s_epub_container_xml))) {
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
    rc = internal_epub_add_meta(&zip, mani, spine, nav, count, meta, opf, cap, navdoc, cap);
  }
  /* The coordinator aborts this stage unless writer completion and verification pass. */
  return internal_epub_finish_writer(&zip, zip_open, &zip_alloc, rc, output);
}
