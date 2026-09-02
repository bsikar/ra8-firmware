/**
 * @file reflow_css_test_util.h
 * @brief Shared sheet fixture for the test_reflow_css_*_mcdc.c siblings.
 *
 * @details
 * Header-only test fixture providing the shared parsed-stylesheet instance,
 * the load / element-identity / inline-declaration helpers, and the named
 * literals shared by test_reflow_css_parse_mcdc.c and
 * test_reflow_css_select_mcdc.c. Everything here has internal linkage,
 * so each including test executable owns a private sheet.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <string.h>

#include "reflow.h"
#include "reflow_css.h"
#include "unity_minimal.h"

/** @brief Shared parsed stylesheet (large -- keep off the test stack). */
static ra8_css_sheet_t s_sheet;

static inline bool css_test_bytes_equal(const uint8_t* bytes, const char* text, size_t len)
{
  bool equal = true;
  for (size_t i = 0U; (i < len) && equal; ++i) {
    equal = bytes[i] == (uint8_t)text[i];
  }
  return equal;
}

static inline size_t css_test_append(char* dst, size_t cap, size_t pos, const char* text)
{
  size_t i = 0U;
  while (((pos + i + 1U) < cap) && (text[i] != '\0')) {
    dst[pos + i] = text[i];
    ++i;
  }
  dst[pos + i] = '\0';
  return pos + i;
}

/**
 * @enum css_test_consts_t
 * @brief Small named literals for the supplemental CSS MC/DC tests.
 */
typedef enum : uint16_t {
  k_css_name_overflow = 70U,  /**< > k_ra8_css_name_max (64) -> intern rejects. */
  k_css_buf_cap       = 256U, /**< Scratch CSS / declaration buffer capacity.   */
  k_css_big_buf       = 512U, /**< Larger scratch buffer for multi-rule CSS.    */
  k_css_fs_156        = 156U, /**< 1.567em -> 156% (3rd frac digit dropped).    */
  k_css_fs_150        = 150U, /**< 1.5em -> 150%.                               */
  k_css_face_one      = 1U,   /**< Expected single-face count.                  */
  k_css_face_two      = 2U,   /**< Expected two-face count.                     */
  k_css_rule_one      = 1U,   /**< Expected single-rule count.                  */
} css_test_consts_t;

/**
 * @enum css_test_color_t
 * @brief Expected 0xRRGGBB results for the named-colour assertions.
 */
typedef enum : uint32_t {
  k_css_color_navy = 0x000080U, /**< Expected RGB of the CSS `navy` keyword. */
  k_css_color_gray = 0x808080U, /**< Expected RGB of `gray` / `grey`.        */
  k_css_color_red  = 0xFF0000U, /**< Expected RGB of the CSS `red` keyword.  */
  k_css_color_blue = 0x0000FFU, /**< Expected RGB of the CSS `blue` keyword. */
} css_test_color_t;

/** @brief Parse @p css into the shared sheet (reset first); assert success. */
static inline void load(const char* css)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_css_sheet_reset(&s_sheet));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_css_parse(&s_sheet, css, (uint32_t)strlen(css)));
}

/** @brief Build an element identity (NULL id/class allowed). */
static inline ra8_css_element_t elem(reflow_html_tag_t tag, const char* id, const char* cls)
{
  ra8_css_element_t e = {};
  e.tag               = (uint8_t)tag;
  e.id                = id;
  e.id_len            = (id != nullptr) ? (uint16_t)strlen(id) : 0U;
  e.class_str         = cls;
  e.class_len         = (cls != nullptr) ? (uint16_t)strlen(cls) : 0U;
  return e;
}

/** @brief The empty inline declaration (nothing set). */
static inline ra8_css_style_t no_inline(void)
{
  return (ra8_css_style_t){};
}

/** @brief Parse an inline `prop: value` body and return the declaration. */
static inline ra8_css_style_t inl(const char* decls)
{
  ra8_css_style_t d = {};
  (void)ra8_css_parse_inline(decls, (uint32_t)strlen(decls), &d);
  return d;
}
