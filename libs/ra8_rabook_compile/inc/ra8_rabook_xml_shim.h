/**
 * @file ra8_rabook_xml_shim.h
 * @brief Bounded XHTML-to-RABOOK streaming builder.
 * @ingroup grp_ereader
 *
 * @details Converts immutable XHTML into the canonical RABOOK DOM using a
 * strict no-heap XML pull reader and explicit caller-owned workspace.
 *
 * [Ring 4 / EPUB Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_book.h"
#include "ra8_err.h"
#include "ra8_rabook_compile.h"
#include "ra8_xml.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief XHTML consumer limits matching the established format contract. */
typedef enum : uint16_t {
  k_ra8_rabook_xml_max_attributes = 32U,  /**< Attributes accepted per element.   */
  k_ra8_rabook_xml_body_siblings  = 256U, /**< Direct children searched for body. */
} ra8_rabook_xml_limits_t;

/** @brief Caller-owned storage for one parser invocation. */
typedef struct {
  ra8_xml_workspace_t xml; /**< Strict reader nesting stack (4096 bytes). */
  /** Element node at each selected-subtree depth. */
  uint32_t element_nodes[k_ra8_xml_workspace_frames];
  /** Last emitted child at each selected-subtree depth. */
  uint32_t last_children[k_ra8_xml_workspace_frames];
  /** Source-order attributes for the element currently emitted. */
  ra8_book_attr_t attributes[k_ra8_rabook_xml_max_attributes];
} ra8_rabook_xml_workspace_t;

/**
 * @brief Parse one XHTML chapter and append it to @p ctx.
 * @details Validates all immutable source bytes before modifying @p ctx. A
 * direct `<body>` child among at most 256 root element children is emitted.
 * A document exceeding that bounded search is rejected; when the complete
 * bounded set has no body, the document root is emitted. Text and attributes
 * are entity-decoded; inter-element whitespace is preserved; comments, PIs,
 * and CDATA are skipped.
 * @param[in] xhtml_bytes Immutable complete XHTML byte sequence.
 * @param[in] xhtml_len Exact readable extent of @p xhtml_bytes in bytes.
 * @param[in,out] ctx Initialised builder receiving one chapter on success.
 * @param[in] chapter_href NUL-terminated immutable chapter identity.
 * @param[in] chapter_title NUL-terminated immutable chapter title.
 * @param[in,out] workspace Exclusive caller-owned parser and builder scratch.
 * @retval k_ra8_ok Chapter appended.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_size @p xhtml_len is zero or exceeds UINT32_MAX.
 * @retval k_ra8_err_validation_failed XML is malformed, too deeply nested, or
 *                                      exceeds the bounded body search.
 * @retval k_ra8_err_no_mem Builder or attribute capacity is exhausted.
 * @pre Every source/string argument remains readable and immutable for the
 * call.
 * @pre @p ctx is initialized; its storage and @p workspace are writable,
 *      non-overlapping, and not used by another live operation.
 * @post Any failure restores @p ctx to its entry state; bytes written beyond
 *       the restored active counts and all workspace bytes are unspecified
 * scratch.
 * @post On success the chapter is canonical and all input bytes remain
 * unchanged.
 * @post The function performs no dynamic allocation.
 * @note Thread-safe for distinct contexts and workspaces.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rabook_xml_parse_chapter(const uint8_t*              xhtml_bytes,
                                                     size_t                      xhtml_len,
                                                     ra8_rabook_ctx_t*           ctx,
                                                     const char*                 chapter_href,
                                                     const char*                 chapter_title,
                                                     ra8_rabook_xml_workspace_t* workspace);

#ifdef __cplusplus
} /* extern "C" */
#endif
