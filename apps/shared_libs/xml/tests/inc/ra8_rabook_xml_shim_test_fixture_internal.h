/**
 * @file ra8_rabook_xml_shim_test_fixture_internal.h
 * @brief Shared bounded fixture for the rabook XML shim host tests.
 *
 * @details
 * Provides identical private arenas and helpers to the DOM-behaviour and
 * resource-limit translation units. Each translation unit receives its own
 * file-local fixture type and static-member state; only the coherent limit-suite
 * runner is shared with the test executable's main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include "book.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_rabook_xml_shim.h"
#include "ra8_rabook_xml_shim_test_internal.h"
#include "rabook_compile.h"
}

/**
 * @brief Run the resource-limit and deep-nesting XML shim cases.
 * @details Executes the bounded sibling, attribute, arena, lifecycle, and
 *          reader-depth vectors in their own fixture translation unit.
 * @return Whether every focused assertion passed.
 * @retval true Every resource-limit and deep-nesting assertion passed.
 * @retval false At least one focused assertion failed.
 * @pre The production rabook XML shim is linked into the test executable.
 * @post Only the limit-suite translation unit's private fixture state changes.
 * @note Test-only interface shared by the two focused translation units.
 * @since Version 0.1.0
 */
RA8_TEST_HELPER [[nodiscard]] bool ra8_rabook_xml_shim_limits_tests_run();

/* -------------------------------------------------------------------------- */
/* Static test arenas */
/* -------------------------------------------------------------------------- */

namespace ra8_rabook_xml_shim_fixture_detail {
template <typename Owner>
struct test_fixture {
  // Each owner tag receives an independent static-member fixture arena.

  inline static ra8_rabook_xml_workspace_t s_xml_workspace = {}; /**< Parser scratch workspace. */

  /** @brief Provide the file-local parse chapter test helper. @details Implements the parse chapter fixture operation used only by this focused test executable. @param[in] bytes Fixture argument governed by the exercised interface contract. @param[in] length Fixture argument governed by the exercised interface contract. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] href Fixture argument governed by the exercised interface contract. @param[in] title Fixture argument governed by the exercised interface contract. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static ra8_err_t internal_parse_chapter(const uint8_t*    bytes,
                                                       size_t            length,
                                                       ra8_rabook_ctx_t* ctx,
                                                       const char*       href,
                                                       const char*       title)
  {
    return ra8_rabook_xml_parse_chapter(bytes, length, ctx, href, title, &s_xml_workspace);
  }

  /** @brief Call the production parser with an explicitly chosen workspace. @details The object-like macro below rewrites every later call site to the five-argument wrapper, so the workspace guard needs an entry point declared before it that still forwards the caller's own pointer. @param[in] bytes Immutable XHTML bytes. @param[in] length Readable extent of the source. @param[in,out] ctx Builder receiving one chapter. @param[in] href Chapter identity string. @param[in] title Chapter title string. @param[in,out] workspace Caller workspace, or NULL for the workspace guard. @return RA8 status from the production parser. @retval k_ra8_ok The chapter was appended. @retval k_ra8_err_null_ptr A required pointer was NULL. @pre Fixed-capacity fixture storage required by this operation is available. @pre Non-NULL arguments follow the production interface contract. @post The production result is returned unchanged. @post No file-local fixture state is modified by the wrapper. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static ra8_err_t internal_parse_chapter_ws(const uint8_t*              bytes,
                                                          size_t                      length,
                                                          ra8_rabook_ctx_t*           ctx,
                                                          const char*                 href,
                                                          const char*                 title,
                                                          ra8_rabook_xml_workspace_t* workspace)
  {
    return ra8_rabook_xml_parse_chapter(bytes, length, ctx, href, title, workspace);
  }

  static constexpr uint32_t k_chapter_cap = 8U;    /**< Maximum retained chapters.   */
  static constexpr uint32_t k_node_cap    = 512U;  /**< Room for the sibling scan.   */
  static constexpr uint32_t k_attr_cap    = 64U;   /**< Maximum retained attributes. */
  static constexpr uint32_t k_style_cap   = 4U;    /**< Maximum stylesheets.         */
  static constexpr uint32_t k_image_cap   = 4U;    /**< Maximum images.              */
  static constexpr uint32_t k_string_cap  = 4096U; /**< Emitted string bytes.        */
  static constexpr uint32_t k_imgpool_cap = 64U;   /**< Fixture image bytes.         */
  static constexpr uint32_t k_out_cap     = 8192U; /**< Compiled output bytes.       */

  static constexpr size_t   k_test_dummy_len = 10U; /**< Nonzero null-guard length. */
  static constexpr uint32_t k_nested_b_idx   = 5U;  /**< Index of nested "B" text.  */

  inline static book_chapter_t    s_chapters[k_chapter_cap]; /**< Parsed chapter table.      */
  inline static book_node_t       s_nodes[k_node_cap];       /**< Parsed node table.         */
  inline static book_attr_t       s_attrs[k_attr_cap];       /**< Parsed attribute table.    */
  inline static book_stylesheet_t s_styles[k_style_cap];     /**< Parsed stylesheet table.   */
  inline static book_image_t      s_images[k_image_cap];     /**< Parsed image table.        */
  inline static char              s_strpool[k_string_cap];   /**< Parsed string storage.     */
  inline static uint8_t           s_imgpool[k_imgpool_cap];  /**< Parsed image-byte storage. */
  inline static uint8_t           s_out[k_out_cap];          /**< Compiled output storage.   */

  /**
 * @brief Node arena for the deep-nesting fixtures (#625).
 * @details The worst-case document at the reader depth cap emits nearly twice
 *          the shared @ref k_node_cap, so those tests get their own table.
 */
  static constexpr uint32_t k_deep_node_cap = 1024U; /**< Covers 996 nodes. */

  inline static book_node_t s_deep_nodes[k_deep_node_cap]; /**< Deep-nesting node table. */

  inline static uint32_t s_total = 0U; /**< Number of assertions evaluated.   */
  inline static uint32_t s_pass  = 0U; /**< Number of assertions that passed. */

  /** @brief Provide the file-local check test helper. @details Implements the check fixture operation used only by this focused test executable. @param[in] cond Fixture argument governed by the exercised interface contract. @param[in] name Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static void internal_check(bool cond, const char* name)
  {
    (void)name;
    ++s_total;
    if (cond) {
      ++s_pass;
    }
  }

  /* Prepare a fresh builder context over the static arenas. */
  /** @brief Prepare the fixture's make ctx state. @details Implements the make ctx fixture operation used only by this focused test executable. @return The value computed by the fixture helper. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static ra8_rabook_ctx_t internal_make_ctx()
  {
    const ra8_rabook_buffers_t bufs = {
      .chapters       = s_chapters,
      .nodes          = s_nodes,
      .attrs          = s_attrs,
      .stylesheets    = s_styles,
      .images         = s_images,
      .string_pool    = s_strpool,
      .image_pool     = s_imgpool,
      .out            = s_out,
      .chapter_cap    = k_chapter_cap,
      .node_cap       = k_node_cap,
      .attr_cap       = k_attr_cap,
      .stylesheet_cap = k_style_cap,
      .image_cap      = k_image_cap,
      .string_cap     = k_string_cap,
      .image_pool_cap = k_imgpool_cap,
      .out_cap        = k_out_cap,
    };
    ra8_rabook_ctx_t ctx = {};
    (void)rabook_compile_init(&ctx, &bufs);
    return ctx;
  }

  /* Return the string-pool content at offset @p off. */
  RA8_INTERNAL static const char* internal_pool_str(const ra8_rabook_ctx_t& ctx, uint32_t off)
  {
    if (off == k_book_nil || off >= ctx.string_size) {
      return "<NIL>";
    }
    return ctx.buf.string_pool + off;
  }

  /** @brief Compare all logical builder state that a failed parse must restore. @details Implements the same ctx state fixture operation used only by this focused test executable. @param[in] lhs Fixture argument governed by the exercised interface contract. @param[in] rhs Fixture argument governed by the exercised interface contract. @return Whether the named fixture condition holds. @retval true The named fixture condition holds. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
  RA8_INTERNAL static bool internal_same_ctx_state(const ra8_rabook_ctx_t& lhs,
                                                   const ra8_rabook_ctx_t& rhs)
  {
    return (lhs.chapter_count == rhs.chapter_count) && (lhs.node_count == rhs.node_count) &&
           (lhs.attr_count == rhs.attr_count) && (lhs.stylesheet_count == rhs.stylesheet_count) &&
           (lhs.image_count == rhs.image_count) && (lhs.string_size == rhs.string_size) &&
           (lhs.image_pool_size == rhs.image_pool_size) && (lhs.title_off == rhs.title_off) &&
           (lhs.author_off == rhs.author_off) && (lhs.language_off == rhs.language_off) &&
           (lhs.identifier_off == rhs.identifier_off) &&
           (lhs.cover_image_index == rhs.cover_image_index) && (lhs.flags == rhs.flags) &&
           (lhs.image_pool_mode == rhs.image_pool_mode) && (lhs.failed == rhs.failed);
  }
};
} // namespace ra8_rabook_xml_shim_fixture_detail
