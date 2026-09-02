/**
 * @file mdl_export_epub_internal.h
 * @brief Private bounded EPUB3 writer contracts for the media downloader.
 * @details Separates fixed-layout page and ZIP assembly from the package
 *          metadata writer, and fixes the bounded XML accumulator sizing that
 *          both translation units budget against.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_export_internal.h"
#include "miniz.h"

/**
 * @brief EPUB string-buffer sizing (grows with the page count).
 * @details Sized for the WORST case, not the typical `page_NNN.jpg`: a page
 *          filename may be up to ::k_name_max bytes and, XML-escaped, expand
 *          6x (a name of all `&quot;`). That escaped name is embedded once in
 *          the page's manifest fragment and once in its xhtml document, so both
 *          the fragment buffer and the per-page accumulator budget must exceed
 *          the fixed template text plus ::k_epub_name_esc_max. Undersizing here
 *          does not truncate silently -- ::priv_mdl_epub_str_cat and
 *          ::priv_mdl_export_snprintf_fit report it and the export fails --
 *          but correct sizing is what lets a legitimate long-name chapter
 *          package rather than error.
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
RA8_PRIV bool priv_mdl_epub_str_cat(char* dst, size_t cap, const char* text);

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
RA8_PRIV bool priv_mdl_epub_add_str(mz_zip_archive* zip, const char* name, const char* body);

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
RA8_PRIV ra8_err_t priv_mdl_epub_add_meta(mz_zip_archive*          zip,
                                          const char*              mani,
                                          const char*              spine,
                                          const char*              nav,
                                          size_t                   page_count,
                                          const mdl_export_meta_t* meta,
                                          char*                    opf,
                                          size_t                   opf_buf_cap,
                                          char*                    navdoc,
                                          size_t                   nav_buf_cap);
