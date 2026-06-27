/**
 * @file ra_rabook_xml_shim.h
 * @brief C-callable interface to the XHTML -> ra_rabook DOM parser (#149).
 *
 * @details
 * Thin extern "C" boundary over the C++ tinyxml2 shim that walks an XHTML
 * chapter file and populates a @ref ra_rabook_ctx_t builder with the body's
 * element/text nodes, matching the DFS pre-order the desktop tool
 * `tools/epub_compile/epub_compile.py` emits.
 *
 * @note The implementation (ra_rabook_xml_shim.cpp) is the only C++ TU in
 *       this library.  All types that cross the boundary use plain C and no
 *       C++ ABI leaks out of this header.
 * @note Not thread-safe.
 * @see ra_rabook_compile.h  Builder context populated by this parser.
 * @since Version 0.1.0
 *
 * [Ring 4 / EPUB Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_rabook_compile.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse one XHTML chapter and add it to the builder context.
 *
 * @details
 * Parses @p xhtml_bytes with tinyxml2, locates the `<body>` subtree (or
 * falls back to the document root), and performs an iterative DFS pre-order
 * walk that mirrors the desktop tool's `DomBuilder.add_element()` recursion:
 *
 *  - Element nodes become @ref ra_rabook_add_element calls (name + attrs).
 *  - Text nodes become @ref ra_rabook_add_text calls (interned string).
 *  - Comments, PIs, and CDATA are silently skipped.
 *
 * On return, `ra_rabook_add_chapter(ctx, title_off, href_off, root_idx)` has
 * been called and the chapter is recorded in the builder.
 *
 * @par NASA Rule 1 (no recursion)
 * The DFS uses an explicit caller-stack of depth @c k_ra_rabook_xhtml_max_depth
 * (no C recursion; documented with @c RA_NO_RECURSION).
 *
 * @par NASA Rule 3 (no dynamic allocation) -- documented deviation
 * `tinyxml2::XMLDocument::Parse()` heap-allocates its internal node pool.
 * The document is local to this function and freed on return, so no allocation
 * outlives the call.  This is the same exemption accepted by
 * `libs/ra_epub/src/ra_epub_xml_shim.cpp`.
 *
 * @param[in]     xhtml_bytes    Raw XHTML file bytes (need not be NUL-terminated).
 * @param[in]     xhtml_len      Length of @p xhtml_bytes in bytes.
 * @param[in,out] ctx            Builder context (non-NULL, initialised).
 * @param[in]     chapter_href   Manifest href of this chapter (non-NULL).
 * @param[in]     chapter_title  TOC label for this chapter ("" if unknown).
 *
 * @return Error code.
 * @retval k_ra_ok           Chapter parsed and added to the builder.
 * @retval k_ra_err_null_ptr @p xhtml_bytes, @p ctx, @p chapter_href, or
 *                           @p chapter_title is NULL.
 * @retval k_ra_err_no_mem   tinyxml2 failed to parse (malformed XML) or the
 *                           builder arena is full (ctx->failed latched).
 *
 * @pre @p ctx was initialised by @ref ra_rabook_compile_init.
 * @pre @p xhtml_len > 0.
 * @post On success a new chapter entry references the body element node.
 * @post On failure ctx->failed may be latched; the builder is still usable
 *       for subsequent calls (sticky-fail model).
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
ra_err_t ra_rabook_xml_parse_chapter(const uint8_t*   xhtml_bytes,
                                     size_t           xhtml_len,
                                     ra_rabook_ctx_t* ctx,
                                     const char*      chapter_href,
                                     const char*      chapter_title);

#ifdef __cplusplus
} /* extern "C" */
#endif
