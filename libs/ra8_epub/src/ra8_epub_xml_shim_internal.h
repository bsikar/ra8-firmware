/**
 * @file ra8_epub_xml_shim_internal.h
 * @brief Library-private contract for the bounded EPUB XML parser
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 3 / LIB] {World: NS}
 *
 * @details
 * ``ra8_epub_xml_shim.c`` implements four strict streamed entry points that
 * ``ra8_epub_open.c`` calls. Those four functions and the container-result
 * struct they share used to be restated by hand in the shim, in
 * ``ra8_epub_open.c`` and in three test translation units -- five copies
 * of one contract, none of which the compiler could check against
 * another. This header is the single copy.
 *
 * Everything declared here is library-private: production callers outside
 * ``libs/ra8_epub`` go through ``ra8_epub_open()``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_epub.h"
#include "ra8_err.h"

/**
 * @struct ra8_epub_container_result_t
 * @brief Out-parameter struct returned by `priv_ra8_epub_xml_parse_container()`.
 *
 * @details
 * Carries the rootfile path lifted out of ``META-INF/container.xml``. The
 * OPF path is always non-empty on success; the caller feeds it to the
 * second pass, `priv_ra8_epub_xml_parse_opf()`.
 *
 * @invariant `opf_path` is NUL-terminated whenever the parse returned
 *            `k_ra8_ok`.
 * @invariant `opf_path` is never longer than `k_ra8_epub_max_path_len - 1`.
 *
 * @par Example:
 * @code
 * ra8_epub_container_result_t res = {};
 * if (priv_ra8_epub_xml_parse_container(buf, len, &res) == k_ra8_ok) {
 *   // res.opf_path names the OPF document inside the archive
 * }
 * @endcode
 *
 * @see priv_ra8_epub_xml_parse_container()
 */
typedef struct {
  char opf_path[k_ra8_epub_max_path_len]; /**< Rootfile path, NUL-terminated. */
} ra8_epub_container_result_t;

/**
 * @brief Parse `META-INF/container.xml` and copy the rootfile path out.
 *
 * @details
 * Reads the ``<rootfiles>/<rootfile full-path=...>`` attribute of the
 * first rootfile entry. The complete immutable document is validated before
 * @p out changes. Decoded paths longer than the fixed result field are
 * truncated at a complete XML entity or UTF-8 sequence, matching the legacy
 * bounded-field contract.
 *
 * @param[in]  xml_bytes Container document bytes. Not NUL-terminated.
 * @param[in]  xml_len   Exact readable extent of @p xml_bytes in bytes.
 * @param[out] out       Receives the rootfile path.
 * @param[in,out] workspace Exclusive caller-owned XML scratch workspace.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Rootfile path copied into @p out.
 * @retval k_ra8_err_null_ptr      @p xml_bytes or @p out was NULL.
 * @retval k_ra8_err_invalid_size  @p xml_len is zero.
 * @retval k_ra8_err_validation_failed The document is malformed or no usable
 *                                      rootfile path exists.
 *
 * @pre @p xml_bytes is readable for @p xml_len bytes.
 * @pre @p out and @p workspace point at writable non-overlapping storage.
 * @post On success `out->opf_path` is NUL-terminated; on failure @p out is
 *       unchanged. Workspace bytes are scratch and unspecified on return.
 * @post The function performs no dynamic allocation and never changes the
 * source.
 * @note Thread-safe for distinct outputs and workspaces.
 *
 * @see priv_ra8_epub_xml_parse_opf()
 *
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_ra8_epub_xml_parse_container(const uint8_t*               xml_bytes,
                                                     size_t                       xml_len,
                                                     ra8_epub_container_result_t* out,
                                                     ra8_epub_xml_workspace_t*    workspace);

/**
 * @brief Parse the OPF document into metadata, cover and spine.
 *
 * @details
 * The complete document, required OPF shape, and spine capacity are checked
 * before semantic fields change. On success `chapter_count`, chapter paths,
 * metadata, manifest resources, cover, fonts, and TOC source are written from
 * the OPF ``<metadata>``, ``<manifest>`` and ``<spine>`` sections.
 *
 * @param[in]     xml_bytes OPF document bytes. Not NUL-terminated.
 * @param[in]     xml_len   Exact readable extent of @p xml_bytes in bytes.
 * @param[in,out] book      Book record to fill in.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Metadata, spine and cover recorded.
 * @retval k_ra8_err_null_ptr    @p xml_bytes or @p book was NULL.
 * @retval k_ra8_err_invalid_size @p xml_len is zero.
 * @retval k_ra8_err_validation_failed The XML or required OPF shape is invalid.
 * @retval k_ra8_err_no_mem      The spine exceeds the fixed chapter capacity.
 *
 * @pre @p xml_bytes is readable for @p xml_len bytes.
 * @pre @p book points at writable storage and its embedded XML workspace is
 *      not used by another live parse.
 * @post On documented input/shape/capacity failure, externally visible book
 *       fields are unchanged; embedded workspace scratch is unspecified.
 * @post The function performs no dynamic allocation and never changes the
 * source.
 * @note Thread-safe for distinct books.
 *
 * @see priv_ra8_epub_xml_parse_ncx()
 *
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_ra8_epub_xml_parse_opf(const uint8_t*   xml_bytes,
                                               size_t           xml_len,
                                               ra8_epub_book_t* book);

/**
 * @brief Parse an EPUB 2 NCX document into `book->toc`.
 *
 * @details
 * Walks ``<navMap>`` depth-first. Each ``<navPoint>`` contributes one
 * entry (``<navLabel><text>`` becomes the title, ``<content src>`` the
 * href) and nested ``<navPoint>`` elements increase the entry depth.
 *
 * @param[in]     xml_bytes NCX document bytes. Not NUL-terminated.
 * @param[in]     xml_len   Exact readable extent of @p xml_bytes in bytes.
 * @param[in,out] book      Book record whose `toc` / `toc_count` are set.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Table of contents recorded.
 * @retval k_ra8_err_null_ptr    @p xml_bytes or @p book was NULL.
 * @retval k_ra8_err_invalid_size @p xml_len is zero.
 * @retval k_ra8_err_validation_failed The XML is malformed or has no nav map.
 * @retval k_ra8_err_no_mem      The nav map exceeds the fixed TOC capacity.
 *
 * @pre @p xml_bytes is readable for @p xml_len bytes.
 * @pre `priv_ra8_epub_xml_parse_opf()` has already run for @p book.
 * @post On success `book->toc[0..toc_count)` is replaced. On failure existing
 *       TOC fields are unchanged; embedded workspace scratch is unspecified.
 * @post The function performs no dynamic allocation and never changes the
 * source.
 * @note Thread-safe for distinct books.
 *
 * @see priv_ra8_epub_xml_parse_nav()
 *
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_ra8_epub_xml_parse_ncx(const uint8_t*   xml_bytes,
                                               size_t           xml_len,
                                               ra8_epub_book_t* book);

/**
 * @brief Parse an EPUB 3 nav document into `book->toc`.
 *
 * @details
 * Locates the ``<nav epub:type="toc">`` element, falling back to the
 * first ``<nav>``, then walks its ``<ol>``/``<li>`` tree. Each ``<li>``
 * contributes one entry (``<a>`` text becomes the title, ``<a href>`` the
 * href) and nested ``<ol>`` elements increase the entry depth.
 *
 * @param[in]     xml_bytes Nav document bytes. Not NUL-terminated.
 * @param[in]     xml_len   Exact readable extent of @p xml_bytes in bytes.
 * @param[in,out] book      Book record whose `toc` / `toc_count` are set.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Table of contents recorded.
 * @retval k_ra8_err_null_ptr    @p xml_bytes or @p book was NULL.
 * @retval k_ra8_err_invalid_size @p xml_len is zero.
 * @retval k_ra8_err_validation_failed The XML is malformed or has no usable
 * nav.
 * @retval k_ra8_err_no_mem      The nav tree exceeds the fixed TOC capacity.
 *
 * @pre @p xml_bytes is readable for @p xml_len bytes.
 * @pre `priv_ra8_epub_xml_parse_opf()` has already run for @p book.
 * @post On success `book->toc[0..toc_count)` is replaced. On failure existing
 *       TOC fields are unchanged; embedded workspace scratch is unspecified.
 * @post The function performs no dynamic allocation and never changes the
 * source.
 * @note Thread-safe for distinct books.
 *
 * @see priv_ra8_epub_xml_parse_ncx()
 *
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_ra8_epub_xml_parse_nav(const uint8_t*   xml_bytes,
                                               size_t           xml_len,
                                               ra8_epub_book_t* book);

#ifdef __cplusplus
}
#endif
