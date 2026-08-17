/**
 * @file mdl_export_epub_meta.c
 * @brief Render the EPUB3 package document, navigation, and identifier.
 * @details Derives one deterministic RFC 4122 publication identifier, escapes
 *          the Dublin Core metadata text, and renders the bounded OPF plus
 *          navigation documents that finalize a staged EPUB archive.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdio.h>
#include <string.h>

#include "mdl_export.h"
#include "mdl_export_epub_internal.h"
#include "mdl_sanitize.h"
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
    (void)priv_mdl_epub_str_cat(text->creators, sizeof(text->creators), fragment);
  }
  if (meta->artist[0] != '\0') {
    char escaped[k_mdl_meta_name_max * 6U];
    char fragment[k_mdl_meta_name_max * 6U + k_epub_fragment_slack];
    (void)mdl_xml_escape(meta->artist, escaped, sizeof(escaped));
    (void)
      snprintf(fragment, sizeof(fragment), "<dc:creator opf:role=\"art\">%s</dc:creator>", escaped);
    (void)priv_mdl_epub_str_cat(text->creators, sizeof(text->creators), fragment);
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
                     priv_mdl_epub_add_str(zip, "OEBPS/content.opf", opf) &&
                     priv_mdl_epub_add_str(zip, "OEBPS/nav.xhtml", navdoc) &&
                     (mz_zip_writer_finalize_archive(zip) != MZ_FALSE);
  return ok ? k_ra8_ok : k_ra8_fail;
}

RA8_PRIV ra8_err_t priv_mdl_epub_add_meta(mz_zip_archive*          zip,
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
