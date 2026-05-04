/**
 * @file ra_reflow_xml_shim.cpp
 * @brief C++ -> C shim that walks an XHTML document with tinyxml2.
 *
 * @details
 * The rest of `libs/ra_reflow` is plain C. Tinyxml2 only exposes a
 * C++ API, so this is the one TU that reaches the parser. Everything
 * callable from the rest of the codebase is `extern "C"` and operates
 * on the engine's caller-owned pools (`engine->tokens[]` /
 * `engine->text_pool[]`), so no C++ ABI escapes the boundary.
 *
 * The walker is recursive but bounded: tinyxml2 caps document nesting
 * depth in its own parser, and the engine's token / text pools are
 * fixed-size, so any pathological input fails with `k_ra_err_no_mem`
 * rather than running unbounded.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ra_err.h"
#include "ra_reflow.h"
#include "ra_reflow_xml_shim_internal.hpp"
#include "tinyxml2.h"

namespace ra_reflow_xml_shim_internal {

std::size_t lowercase_truncated_copy(const char* src, char* dst, std::size_t dst_cap)
{
  // mcdc-deactivated: TU-local helper lowercase_truncated_copy -- defensive NULL+cap guard duplicates the public-API entry-point check, which has already rejected NULL/zero-cap on every reachable call path.
  if (dst == nullptr || dst_cap == 0U) {
    return 0U;
  }
  std::size_t i = 0U;
  if (src != nullptr) {
    while (src[i] != '\0' && i + 1U < dst_cap) {
      char c = src[i];
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c + ('a' - 'A'));
      }
      dst[i] = c;
      ++i;
    }
  }
  dst[i] = '\0';
  return i;
}

bool is_xml_whitespace(char c)
{
  return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
}

bool should_reject_null_args(bool engine_is_null, bool xhtml_buf_is_null)
{
  return engine_is_null || xhtml_buf_is_null;
}

} /* namespace ra_reflow_xml_shim_internal */

namespace {

using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;
using tinyxml2::XMLNode;
using tinyxml2::XMLText;

/**
 * @brief Internal-only sizing constants for the shim.
 */
enum : uint32_t {
  k_priv_max_recursion_depth = 64U, /**< Hard cap on DOM nesting depth. */
};

/**
 * @brief Map a tag name (case-insensitive ASCII) to `ra_reflow_html_tag_t`.
 *
 * @details
 * Tinyxml2 stores tag names with their namespace prefix glued in
 * (e.g. `"html:p"`). We compare against the trailing local-name
 * portion. Unrecognised tags map to `k_ra_reflow_tag_unknown`, which
 * is treated as a transparent flow-pass-through by the walker.
 */
ra_reflow_html_tag_t classify_tag(const char* name)
{
  if (name == nullptr) {
    return k_ra_reflow_tag_unknown;
  }
  const char* colon = std::strrchr(name, ':');
  const char* tail  = (colon != nullptr) ? (colon + 1) : name;

  /* Lower-case copy to keep the compare branch-free. */
  char lower[16];
  (void)ra_reflow_xml_shim_internal::lowercase_truncated_copy(tail, lower, sizeof(lower));

  if (std::strcmp(lower, "p") == 0) {
    return k_ra_reflow_tag_p;
  }
  if (std::strcmp(lower, "h1") == 0) {
    return k_ra_reflow_tag_h1;
  }
  if (std::strcmp(lower, "h2") == 0) {
    return k_ra_reflow_tag_h2;
  }
  if (std::strcmp(lower, "h3") == 0) {
    return k_ra_reflow_tag_h3;
  }
  if (std::strcmp(lower, "h4") == 0) {
    return k_ra_reflow_tag_h4;
  }
  if (std::strcmp(lower, "h5") == 0) {
    return k_ra_reflow_tag_h5;
  }
  if (std::strcmp(lower, "h6") == 0) {
    return k_ra_reflow_tag_h6;
  }
  if (std::strcmp(lower, "em") == 0) {
    return k_ra_reflow_tag_em;
  }
  if (std::strcmp(lower, "strong") == 0) {
    return k_ra_reflow_tag_strong;
  }
  if (std::strcmp(lower, "b") == 0) {
    return k_ra_reflow_tag_b;
  }
  if (std::strcmp(lower, "i") == 0) {
    return k_ra_reflow_tag_i;
  }
  if (std::strcmp(lower, "br") == 0) {
    return k_ra_reflow_tag_br;
  }
  if (std::strcmp(lower, "hr") == 0) {
    return k_ra_reflow_tag_hr;
  }
  if (std::strcmp(lower, "ul") == 0) {
    return k_ra_reflow_tag_ul;
  }
  if (std::strcmp(lower, "ol") == 0) {
    return k_ra_reflow_tag_ol;
  }
  if (std::strcmp(lower, "li") == 0) {
    return k_ra_reflow_tag_li;
  }
  if (std::strcmp(lower, "blockquote") == 0) {
    return k_ra_reflow_tag_blockquote;
  }
  if (std::strcmp(lower, "a") == 0) {
    return k_ra_reflow_tag_a;
  }
  if (std::strcmp(lower, "img") == 0) {
    return k_ra_reflow_tag_img;
  }
  return k_ra_reflow_tag_unknown;
}

/**
 * @brief True for tags that act as block-flow containers in our
 *        v1 subset.
 */
bool is_block_tag(ra_reflow_html_tag_t tag)
{
  switch (tag) {
    case k_ra_reflow_tag_p:
    case k_ra_reflow_tag_h1:
    case k_ra_reflow_tag_h2:
    case k_ra_reflow_tag_h3:
    case k_ra_reflow_tag_h4:
    case k_ra_reflow_tag_h5:
    case k_ra_reflow_tag_h6:
    case k_ra_reflow_tag_ul:
    case k_ra_reflow_tag_ol:
    case k_ra_reflow_tag_li:
    case k_ra_reflow_tag_blockquote:
      return true;
    default:
      return false;
  }
}

/**
 * @brief Inline tags that flip a font-style bit while inside their
 *        subtree.
 *
 * @return The style flag to OR in, or `0` if no style change.
 */
uint8_t style_for_tag(ra_reflow_html_tag_t tag)
{
  switch (tag) {
    case k_ra_reflow_tag_b:
    case k_ra_reflow_tag_strong:
      return static_cast<uint8_t>(k_ra_reflow_style_bold);
    case k_ra_reflow_tag_i:
    case k_ra_reflow_tag_em:
      return static_cast<uint8_t>(k_ra_reflow_style_italic);
    case k_ra_reflow_tag_a:
      return static_cast<uint8_t>(k_ra_reflow_style_underline);
    default:
      return 0U;
  }
}

/**
 * @brief Append one token to `engine->tokens[]`. Returns false on pool
 *        overflow.
 */
bool push_token(ra_reflow_t*           engine,
                ra_reflow_token_kind_t kind,
                ra_reflow_html_tag_t   tag,
                uint8_t                style,
                uint32_t               text_off,
                uint32_t               text_len)
{
  if (engine->token_count >= k_ra_reflow_max_tokens) {
    return false;
  }
  ra_reflow_token_t* tok = &engine->tokens[engine->token_count];
  tok->kind              = static_cast<uint8_t>(kind);
  tok->tag               = static_cast<uint8_t>(tag);
  tok->style             = style;
  tok->reserved          = 0U;
  tok->text_off          = text_off;
  tok->text_len          = text_len;
  engine->token_count++;
  return true;
}

/**
 * @brief Copy a slice of normalised whitespace-collapsed text into the
 *        engine's text pool, returning the offset / length.
 *
 * @details
 * HTML whitespace rules: collapse any run of ASCII whitespace to a
 * single space; drop leading whitespace if it would be the first
 * character of an empty pool slot. The caller emits a token only if
 * `out_len > 0`.
 */
bool stash_text(ra_reflow_t* engine, const char* src, uint32_t* out_off, uint32_t* out_len)
{
  *out_off = engine->text_pool_used;
  *out_len = 0U;
  if (src == nullptr) {
    return true;
  }
  uint32_t pool_used = engine->text_pool_used;
  bool     last_ws   = true; /* Treat leading whitespace as already-consumed. */
  for (std::size_t i = 0U; src[i] != '\0'; ++i) {
    char c     = src[i];
    bool is_ws = ra_reflow_xml_shim_internal::is_xml_whitespace(c);
    if (is_ws) {
      if (last_ws) {
        continue;
      }
      c       = ' ';
      last_ws = true;
    } else {
      last_ws = false;
    }
    if (pool_used >= k_ra_reflow_text_pool_bytes) {
      return false;
    }
    engine->text_pool[pool_used] = static_cast<uint8_t>(c);
    ++pool_used;
  }
  *out_len               = pool_used - *out_off;
  engine->text_pool_used = pool_used;
  return true;
}

/* Forward declaration so walk_node() can recurse through walk_element(). */
ra_err_t walk_node(ra_reflow_t* engine, const XMLNode* node, uint8_t style, uint32_t depth);

/**
 * @brief Visit one element: emit block-start / inline tokens for it,
 *        recurse into children, emit block-end.
 */
ra_err_t walk_element(ra_reflow_t* engine, const XMLElement* el, uint8_t style, uint32_t depth)
{
  const ra_reflow_html_tag_t tag = classify_tag(el->Name());

  /* Void elements: emit one token then return without recursing. */
  if (tag == k_ra_reflow_tag_br) {
    return push_token(engine, k_ra_reflow_tok_break, tag, style, 0U, 0U) ? k_ra_ok
                                                                         : k_ra_err_no_mem;
  }
  if (tag == k_ra_reflow_tag_hr) {
    return push_token(engine, k_ra_reflow_tok_rule, tag, style, 0U, 0U) ? k_ra_ok : k_ra_err_no_mem;
  }
  if (tag == k_ra_reflow_tag_img) {
    return push_token(engine, k_ra_reflow_tok_image, tag, style, 0U, 0U) ? k_ra_ok
                                                                         : k_ra_err_no_mem;
  }

  const bool block = is_block_tag(tag);
  if (block) {
    if (!push_token(engine, k_ra_reflow_tok_block_start, tag, style, 0U, 0U)) {
      return k_ra_err_no_mem;
    }
  }

  const uint8_t inner_style = static_cast<uint8_t>(style | style_for_tag(tag));

  for (const XMLNode* child = el->FirstChild(); child != nullptr; child = child->NextSibling()) {
    ra_err_t err = walk_node(engine, child, inner_style, depth + 1U);
    if (err != k_ra_ok) {
      return err;
    }
  }

  if (block) {
    if (!push_token(engine, k_ra_reflow_tok_block_end, tag, style, 0U, 0U)) {
      return k_ra_err_no_mem;
    }
  }
  return k_ra_ok;
}

ra_err_t walk_node(ra_reflow_t* engine, const XMLNode* node, uint8_t style, uint32_t depth)
{
  if (depth >= k_priv_max_recursion_depth) {
    return k_ra_err_no_mem;
  }
  const XMLText* text = node->ToText();
  if (text != nullptr) {
    uint32_t    off = 0U;
    uint32_t    len = 0U;
    const char* str = text->Value();
    if (!stash_text(engine, str, &off, &len)) {
      return k_ra_err_no_mem;
    }
    if (len > 0U) {
      if (!push_token(engine, k_ra_reflow_tok_text, k_ra_reflow_tag_unknown, style, off, len)) {
        return k_ra_err_no_mem;
      }
    }
    return k_ra_ok;
  }
  const XMLElement* el = node->ToElement();
  if (el != nullptr) {
    return walk_element(engine, el, style, depth);
  }
  /* Comments / declarations / unknowns: skip silently. */
  return k_ra_ok;
}

} /* namespace */

extern "C" ra_err_t
priv_reflow_xml_walk(ra_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len)
{
  if (ra_reflow_xml_shim_internal::should_reject_null_args(engine == nullptr,
                                                           xhtml_buf == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (xhtml_len == 0U) {
    return k_ra_err_invalid_size;
  }

  XMLDocument doc;
  /* Tinyxml2 happily accepts both well-formed XHTML and the
   * looser "<html>...<p>...</html>" subset we expect from EPUB
   * chapters. Anything malformed enough to defeat its parser yields
   * k_ra_err_validation_failed. */
  if (doc.Parse(reinterpret_cast<const char*>(xhtml_buf), xhtml_len) != tinyxml2::XML_SUCCESS) {
    return k_ra_err_validation_failed;
  }
  const XMLElement* root = doc.RootElement();
  if (root == nullptr) {
    return k_ra_err_validation_failed;
  }
  return walk_element(engine, root, static_cast<uint8_t>(k_ra_reflow_style_normal), 0U);
}
