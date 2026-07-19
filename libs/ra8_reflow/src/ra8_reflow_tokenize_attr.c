/**
 * @file ra8_reflow_tokenize_attr.c
 * @brief Attribute and CSS-identity helpers for the no-heap XHTML tokenizer:
 *        quoted-attribute scanning, `<img src>` / `<a href>` capture, CSS
 *        element-identity + inline-style extraction, link interning, and
 *        embedded-face resolution.
 *
 * @details
 * Companion translation unit to ra8_reflow_tokenize.c. It groups the routines
 * that read a raw `<...>` start-tag span: locating a named quoted attribute,
 * copying its value into the engine text pool, building the
 * `ra8_css_element_t` identity (tag + id + class) the cascade matches against,
 * parsing the inline `style="..."` declaration, interning an `<a href>` into
 * the link-target table, detecting a `<link rel>` stylesheet, and mapping a
 * cascaded font-family + emphasis to an embedded face slot. None of these
 * touch the tokenizer's mutable `tok_ctx_t`; the markup handlers in
 * ra8_reflow_tokenize.c call the cross-TU helpers promoted into
 * ra8_reflow_tokenize_internal.h. Pure forward-pass scanning -- no recursion
 * (NASA P10 Rule 1) and bounded loops (Rule 2).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
#include "ra8_reflow_tokenize_internal.h"

/* ===========================================================================
 * Quoted-attribute scanning + value capture
 * ===========================================================================
 */

/**
 * @brief True iff attribute @p name (case-insensitive) begins at `tag[i]`.
 *
 * @details Matches @p name with a non-name byte (or the leading '<')
 * immediately before it, so a substring like "xsrc" does not match "src" and
 * "xid" does not match "id".
 *
 * @param[in] tag      Raw tag span.
 * @param[in] i        Candidate start offset (`i + name_len` must be in range).
 * @param[in] name     Lower-case attribute name to match.
 * @param[in] name_len Length of @p name, bytes.
 * @return true iff `tag[i..i+name_len)` equals @p name as an attribute name.
 * @retval true  Match.
 * @retval false No match.
 * @pre `tag`, `name` are non-null and `tag[i..i+name_len)` are readable.
 * @pre `i + name_len <= tag span length`.
 * @post No state mutated.
 * @post Return value depends solely on the inputs.
 * @note Pure function; @p name must already be lower-case.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_attr_name_at(const uint8_t* tag, size_t i, const char* name, size_t name_len)
{
  for (size_t k = 0U; k < name_len; ++k) {
    if ((tag[i + k] | 0x20U) != (uint8_t)name[k]) {
      return false;
    }
  }
  const char prev = (char)((i == 0U) ? '<' : tag[i - 1U]);
  return !(((prev >= 'a') && (prev <= 'z')) || ((prev >= 'A') && (prev <= 'Z')));
}

/**
 * @brief Parse `= "value"` after an attribute name; return the value span.
 *
 * @details Skips whitespace, requires `=`, skips whitespace, requires a quote,
 * then scans to the matching quote. The span excludes the quotes.
 *
 * @param[in]  tag     Raw tag span.
 * @param[in]  tag_len Length of @p tag, bytes.
 * @param[in]  pos     Offset just past the attribute name.
 * @param[out] vstart  Receives the value start offset.
 * @param[out] vlen    Receives the value length, bytes.
 * @return true iff a quoted value was found.
 * @retval true  `*vstart` / `*vlen` describe the value.
 * @retval false No `= "..."` followed the name.
 * @pre `tag`, `vstart`, `vlen` are non-null.
 * @pre `pos <= tag_len`.
 * @post On true, `[*vstart, *vstart+*vlen)` lies within the tag span.
 * @post On false, the outputs are unspecified (caller ignores them).
 * @note Pure read of @p tag.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool
priv_attr_quoted_value(const uint8_t* tag, size_t tag_len, size_t pos, size_t* vstart, size_t* vlen)
{
  size_t j = pos;
  while ((j < tag_len) && ra8_reflow_tok_is_xml_whitespace((char)tag[j])) {
    ++j;
  }
  if ((j >= tag_len) || (tag[j] != '=')) {
    return false;
  }
  ++j;
  while ((j < tag_len) && ra8_reflow_tok_is_xml_whitespace((char)tag[j])) {
    ++j;
  }
  if ((j >= tag_len) || ((tag[j] != '"') && (tag[j] != '\''))) {
    return false;
  }
  const uint8_t quote = tag[j];
  ++j;
  *vstart = j;
  while ((j < tag_len) && (tag[j] != quote)) {
    ++j;
  }
  *vlen = j - *vstart;
  return true;
}

/**
 * @brief Locate a named attribute's quoted value span within a tag (no copy).
 *
 * @details The non-copying core of ra8_reflow_tok_capture_attr(): returns the value's
 * offset + length *within @p tag* so callers can either copy it to the pool or
 * scan it in place (e.g. an inline `style`).
 *
 * @param[in]  tag      Raw tag span starting at '<'.
 * @param[in]  tag_len  Length of @p tag, bytes.
 * @param[in]  name     Lower-case attribute name to find.
 * @param[in]  name_len Length of @p name, bytes.
 * @param[out] out_voff Receives the value offset within @p tag.
 * @param[out] out_vlen Receives the value length, bytes.
 * @return true iff a quoted @p name value was found.
 * @retval true  `*out_voff` / `*out_vlen` describe the value within @p tag.
 * @retval false Attribute absent or malformed.
 * @pre `tag`, `name`, `out_voff`, `out_vlen` are non-null.
 * @pre `tag_len >= name_len` (zero-length searches return false immediately).
 * @post No state mutated.
 * @post On false, `*out_voff` and `*out_vlen` are unspecified.
 * @note Pure read of @p tag.
 * @since 0.1.0
 */
bool ra8_reflow_tok_find_attr(const uint8_t* tag,
                              size_t         tag_len,
                              const char*    name,
                              size_t         name_len,
                              size_t*        out_voff,
                              size_t*        out_vlen)
{
  if (tag_len < name_len) {
    return false;
  }
  for (size_t i = 0U; (i + name_len) <= tag_len; ++i) {
    if (priv_attr_name_at(tag, i, name, name_len) &&
        priv_attr_quoted_value(tag, tag_len, i + name_len, out_voff, out_vlen)) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Copy a named attribute's quoted value from a tag span into the text pool.
 *
 * @details Scans the raw `<...>` span for attribute @p name (case-insensitive,
 * not preceded by another name character) and copies its quoted value verbatim
 * into the engine text pool. Used for `<img src>` / `<a href>` / element `id`.
 * On no match, a malformed attribute, an empty value, or a full pool the outputs
 * are zeroed, which callers treat as "attribute absent".
 *
 * @param[in,out] engine   Engine whose text pool receives the value bytes.
 * @param[in]     tag      Raw tag span starting at '<'.
 * @param[in]     tag_len  Length of @p tag, bytes.
 * @param[in]     name     Lower-case attribute name to find.
 * @param[in]     name_len Length of @p name, bytes.
 * @param[out]    out_off  Receives the text-pool offset of the value (0 if none).
 * @param[out]    out_len  Receives the value byte length (0 if none).
 * @return Nothing.
 * @pre `engine`, `tag`, `name`, `out_off`, `out_len` are non-null.
 * @pre `engine->text_pool_used <= k_ra8_reflow_text_pool_bytes`.
 * @post `*out_len > 0` iff a quoted @p name value was stored in the pool.
 * @post The pool grows by `*out_len` bytes when a value is stored.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void ra8_reflow_tok_capture_attr(ra8_reflow_t*  engine,
                                 const uint8_t* tag,
                                 size_t         tag_len,
                                 const char*    name,
                                 size_t         name_len,
                                 uint32_t*      out_off,
                                 uint32_t*      out_len)
{
  *out_off      = 0U;
  *out_len      = 0U;
  size_t vstart = 0U;
  size_t vlen   = 0U;
  if (!ra8_reflow_tok_find_attr(tag, tag_len, name, name_len, &vstart, &vlen)) {
    return;
  }
  if ((vlen == 0U) ||
      ((size_t)engine->text_pool_used + vlen > (size_t)k_ra8_reflow_text_pool_bytes)) {
    return;
  }
  const uint32_t off = engine->text_pool_used;
  memcpy(&engine->text_pool[off], &tag[vstart], vlen);
  engine->text_pool_used += (uint32_t)vlen;
  *out_off = off;
  *out_len = (uint32_t)vlen;
}

/**
 * @brief Build a CSS element identity from a just-opened tag's attributes.
 *
 * @details Locates the `id` and `class` attribute value spans in place (no
 * copy) so the cascade can match `#id` / `.class` selectors; the returned
 * pointers alias @p tag. A missing attribute leaves its pointer NULL and its
 * length zero. The element's tag enum is stored so type selectors also work.
 *
 * @param[in] kind Classified tag.
 * @param[in] tag  Raw tag span starting at '<'.
 * @param[in] span Length of @p tag, bytes.
 * @return The element identity for ra8_css_cascade().
 * @retval ra8_css_element_t Fully populated struct; absent attributes have NULL pointer and zero length.
 * @pre `tag` is non-null and holds @p span bytes.
 * @pre `kind` is a valid `ra8_reflow_html_tag_t` value.
 * @post No state mutated; returned pointers alias @p tag.
 * @post `el.tag == (uint8_t)kind`.
 * @note Pure read of @p tag.
 * @since 0.1.0
 */
ra8_css_element_t
ra8_reflow_tok_css_element(ra8_reflow_html_tag_t kind, const uint8_t* tag, size_t span)
{
  ra8_css_element_t el = {};
  el.tag               = (uint8_t)kind;
  size_t off           = 0U;
  size_t len           = 0U;
  if (ra8_reflow_tok_find_attr(tag, span, "id", sizeof("id") - 1U, &off, &len)) {
    el.id     = (const char*)&tag[off];
    el.id_len = (uint16_t)len;
  }
  if (ra8_reflow_tok_find_attr(tag, span, "class", sizeof("class") - 1U, &off, &len)) {
    el.class_str = (const char*)&tag[off];
    el.class_len = (uint16_t)len;
  }
  return el;
}

/**
 * @brief Parse a just-opened tag's inline `style="..."` into a CSS declaration.
 *
 * @details Locates the `style` attribute within the raw tag span and delegates
 * to `ra8_css_parse_inline()` to parse its value into a declaration struct.
 * When the attribute is absent or the parser fails the returned struct has
 * `set == 0`, indicating no inline declarations override the cascade.
 *
 * @param[in] tag  Raw tag span starting at '<'.
 * @param[in] span Length of @p tag, bytes.
 * @return The inline CSS declaration for this element.
 * @retval ra8_css_style_t Struct with `set == 0` if no `style` attribute is present.
 * @pre `tag` is non-null and holds @p span bytes.
 * @pre `span > 0` (a zero-span tag cannot carry attributes).
 * @post No state mutated.
 * @post Returned struct `set` field is 0 when no `style` attribute was found.
 * @note Pure read of @p tag.
 * @since 0.1.0
 */
ra8_css_style_t ra8_reflow_tok_css_inline(const uint8_t* tag, size_t span)
{
  ra8_css_style_t inl = {};
  size_t          off = 0U;
  size_t          len = 0U;
  if (ra8_reflow_tok_find_attr(tag, span, "style", sizeof("style") - 1U, &off, &len)) {
    (void)ra8_css_parse_inline((const char*)&tag[off], (uint32_t)len, &inl);
  }
  return inl;
}

/* ===========================================================================
 * Link interning + stylesheet-rel detection + face resolution
 * ===========================================================================
 */

/**
 * @brief Intern an `<a>` href slice into the link-target table.
 *
 * @details Appends a {href_off, href_len} entry and returns its 1-based id
 * (stored in subsequent text tokens' `reserved` byte). An empty href or a full
 * table returns 0 (the run renders as plain underlined text -- not tappable).
 *
 * @param[in,out] engine   Engine whose link-target table grows.
 * @param[in]     href_off Href text-pool offset.
 * @param[in]     href_len Href byte length.
 * @return 1-based link id, or 0 if not interned.
 * @retval 0 Empty href or the table is full.
 * @retval 1..k_ra8_reflow_max_links The newly assigned 1-based link id.
 * @pre `engine` is non-null.
 * @pre `engine->link_target_count <= k_ra8_reflow_max_links`.
 * @post On a non-zero return the table grew by one entry.
 * @post `engine->link_target_count` is unchanged on a zero return.
 * @note Not thread-safe.
 * @since 0.1.0
 */
uint8_t ra8_reflow_tok_intern_link(ra8_reflow_t* engine, uint32_t href_off, uint32_t href_len)
{
  if ((href_len == 0U) || (engine->link_target_count >= (uint32_t)k_ra8_reflow_max_links)) {
    return 0U;
  }
  const uint32_t idx                 = engine->link_target_count;
  engine->link_targets[idx].href_off = href_off;
  engine->link_targets[idx].href_len = href_len;
  engine->link_target_count          = idx + 1U;
  return (uint8_t)(idx + 1U); /* 1-based: 0 means "no link" */
}

/**
 * @brief True if a `rel` attribute value contains the `stylesheet` token.
 *
 * @details Case-sensitive substring scan (EPUB `<link>` rels are authored
 * lowercase; this also accepts the `alternate stylesheet` form). Bounded by the
 * value length. The inner loop iterates at most `len` times so the scan is
 * O(len) with no dynamic allocation.
 *
 * @param[in] rel Attribute-value bytes (not NUL-terminated).
 * @param[in] len Length of @p rel, bytes.
 * @return true if `stylesheet` occurs in @p rel.
 * @retval true  The substring "stylesheet" was found in @p rel.
 * @retval false The substring is absent or @p len is too short to contain it.
 * @pre @p rel addresses @p len readable bytes.
 * @pre @p rel is non-null (a zero @p len is a valid empty-value case).
 * @post No state mutated.
 * @post Return value depends solely on the inputs.
 * @note Pure.
 * @since 0.1.0
 */
bool ra8_reflow_tok_rel_is_stylesheet(const uint8_t* rel, size_t len)
{
  static const char k_kw[] = "stylesheet";
  const size_t      k_klen = sizeof(k_kw) - 1U;
  for (size_t i = 0U; (i + k_klen) <= len; i++) {
    size_t j = 0U;
    for (; (j < k_klen) && (rel[i + j] == (uint8_t)k_kw[j]); j++) {
    }
    if (j == k_klen) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Map a cascaded run's (font-family + emphasis) to an embedded face slot.
 *
 * @details Returns 0 (the engine's default bound face) when no embedded face is
 * registered, the run has no resolved `font-family`, no `@font-face` matches, or
 * the matched `@font-face` was never registered (its `src` had no manifest font).
 * Otherwise returns `1 + the registry index`, which the render pass uses to pick
 * the matching `stbtt_fontinfo`.
 *
 * @param[in] engine Engine whose face registry + parsed sheet are consulted.
 * @param[in] comp   The element's cascaded style (family + emphasis bits).
 * @return 0 for the default face, else `1 + engine->faces[] index`.
 * @retval 0 No registered face matches the cascaded family + emphasis combination.
 * @pre @p engine and @p comp are non-null.
 * @pre `engine->face_count <= k_ra8_reflow_max_faces`.
 * @post No state is modified.
 * @post Return value is in [0, engine->face_count].
 * @note Pure; not thread-safe only insofar as it reads engine state.
 * @since 0.1.0
 */
uint8_t ra8_reflow_tok_resolve_face_slot(const ra8_reflow_t* engine, const ra8_css_style_t* comp)
{
  if ((engine->face_count == 0U) || ((comp->set & (uint8_t)k_ra8_css_set_family) == 0U) ||
      (comp->family_len == 0U)) {
    return 0U;
  }
  const char*   family = (const char*)&engine->css.names[comp->family_off];
  const bool    bold   = (comp->style & (uint8_t)k_ra8_reflow_style_bold) != 0U;
  const bool    italic = (comp->style & (uint8_t)k_ra8_reflow_style_italic) != 0U;
  const int16_t ci     = ra8_css_match_face(&engine->css, family, comp->family_len, bold, italic);
  if (ci < 0) {
    return 0U;
  }
  for (uint8_t k = 0U; k < engine->face_count; ++k) {
    if (engine->faces[k].css_face_idx == (uint8_t)ci) {
      return (uint8_t)(k + 1U);
    }
  }
  return 0U;
}
