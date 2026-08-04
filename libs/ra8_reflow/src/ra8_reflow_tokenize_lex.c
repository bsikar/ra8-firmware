/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_reflow_tokenize_lex.c
 * @brief Lexical primitives for the no-heap streaming XHTML tokenizer:
 *        whitespace/entity/UTF-8 scanning, tag classification, and the
 *        text-pool / token-pool emit helpers.
 *
 * @details
 * Companion translation unit to ra8_reflow_tokenize.c. It collects the leaf
 * scanning routines that do not touch the tokenizer's mutable element-stack
 * context (`tok_ctx_t`): XHTML-whitespace classification, UTF-8 encoding,
 * named/numeric entity decoding, tag-name classification, the whitespace-
 * collapse text-pool feed, the entity-decoding run stash, the token-pool
 * emit, and the literal-search markup scanners. The markup handlers and the
 * cascade engine live in ra8_reflow_tokenize.c and call the cross-TU helpers
 * promoted into ra8_reflow_tokenize_internal.h. Pure forward-pass scanning --
 * no recursion (NASA P10 Rule 1) and bounded loops (Rule 2).
 *
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
 * Public (test-exposed) helpers -- documented in ra8_reflow_tokenize_internal.h
 * ===========================================================================
 */

bool ra8_reflow_tok_is_xml_whitespace(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r') || (c == '\f') || (c == '\v');
}

size_t ra8_reflow_tok_utf8_encode(uint32_t cp, uint8_t* dst)
{
  if (cp > (uint32_t)k_priv_uc_max) {
    cp = (uint32_t)k_priv_uc_max;
  }
  if (cp < (uint32_t)k_priv_uc_2byte) {
    dst[0] = (uint8_t)cp;
    return 1U;
  }
  if (cp < (uint32_t)k_priv_uc_3byte) {
    dst[0] = (uint8_t)((uint32_t)k_priv_utf8_lead2 | (cp >> (uint32_t)k_priv_utf8_sh6));
    dst[1] = (uint8_t)((uint32_t)k_priv_utf8_cont | (cp & (uint32_t)k_priv_utf8_mask));
    return 2U;
  }
  if (cp < (uint32_t)k_priv_uc_4byte) {
    dst[0] = (uint8_t)((uint32_t)k_priv_utf8_lead3 | (cp >> (uint32_t)k_priv_utf8_sh12));
    dst[1] = (uint8_t)((uint32_t)k_priv_utf8_cont |
                       ((cp >> (uint32_t)k_priv_utf8_sh6) & (uint32_t)k_priv_utf8_mask));
    dst[2] = (uint8_t)((uint32_t)k_priv_utf8_cont | (cp & (uint32_t)k_priv_utf8_mask));
    return 3U;
  }
  dst[0] = (uint8_t)((uint32_t)k_priv_utf8_lead4 | (cp >> (uint32_t)k_priv_utf8_sh18));
  dst[1] = (uint8_t)((uint32_t)k_priv_utf8_cont |
                     ((cp >> (uint32_t)k_priv_utf8_sh12) & (uint32_t)k_priv_utf8_mask));
  dst[2] = (uint8_t)((uint32_t)k_priv_utf8_cont |
                     ((cp >> (uint32_t)k_priv_utf8_sh6) & (uint32_t)k_priv_utf8_mask));
  dst[3] = (uint8_t)((uint32_t)k_priv_utf8_cont | (cp & (uint32_t)k_priv_utf8_mask));
  return 4U;
}

/**
 * @brief Lower-case ASCII and copy the local part of a tag name.
 *
 * @details Scans for the last ':' to drop any namespace prefix, then copies
 * the remaining local name, folding A-Z to a-z, truncating at the buffer
 * capacity. Always NUL-terminates.
 *
 * @param[in]  name Source name (may carry a `prefix:` namespace).
 * @param[in]  len  Bytes in `name`.
 * @param[out] dst  NUL-terminated lower-cased local name buffer.
 * @return Length written to `dst` (excluding the NUL).
 * @retval 0 Empty local name.
 * @pre `name` and `dst` are non-null.
 * @pre `dst` has room for k_priv_tag_name_cap bytes.
 * @post `dst` is NUL-terminated.
 * @post No input bytes are modified.
 * @note Pure aside from writing `dst`.
 * @since 0.1.0
 */
RA8_INTERNAL
static size_t priv_local_lower(const char* name, size_t len, char* dst)
{
  size_t start = 0U;
  for (size_t k = 0U; k < len; ++k) {
    if (name[k] == ':') {
      start = k + 1U;
    }
  }
  size_t n = 0U;
  for (size_t k = start; (k < len) && (n + 1U < (size_t)k_priv_tag_name_cap); ++k) {
    char c = name[k];
    if ((c >= 'A') && (c <= 'Z')) {
      c = (char)(c + ('a' - 'A'));
    }
    dst[n] = c;
    ++n;
  }
  dst[n] = '\0';
  return n;
}

ra8_reflow_html_tag_t ra8_reflow_tok_classify(const char* name, size_t len)
{
  if (name == nullptr) {
    return k_ra8_reflow_tag_unknown;
  }
  char lower[k_priv_tag_name_cap];
  (void)priv_local_lower(name, len, lower);

  static const struct {
    const char*           word; /**< Word. */
    ra8_reflow_html_tag_t tag;  /**< Tag.  */
  } k_map[] = {
    {"p", k_ra8_reflow_tag_p},
    {"h1", k_ra8_reflow_tag_h1},
    {"h2", k_ra8_reflow_tag_h2},
    {"h3", k_ra8_reflow_tag_h3},
    {"h4", k_ra8_reflow_tag_h4},
    {"h5", k_ra8_reflow_tag_h5},
    {"h6", k_ra8_reflow_tag_h6},
    {"em", k_ra8_reflow_tag_em},
    {"strong", k_ra8_reflow_tag_strong},
    {"b", k_ra8_reflow_tag_b},
    {"i", k_ra8_reflow_tag_i},
    {"br", k_ra8_reflow_tag_br},
    {"hr", k_ra8_reflow_tag_hr},
    {"ul", k_ra8_reflow_tag_ul},
    {"ol", k_ra8_reflow_tag_ol},
    {"li", k_ra8_reflow_tag_li},
    {"blockquote", k_ra8_reflow_tag_blockquote},
    {"a", k_ra8_reflow_tag_a},
    {"img", k_ra8_reflow_tag_img},
    {"link", k_ra8_reflow_tag_link},
    {"table", k_ra8_reflow_tag_table},
    {"tr", k_ra8_reflow_tag_tr},
    {"td", k_ra8_reflow_tag_td},
    {"th", k_ra8_reflow_tag_th},
  };
  for (size_t k = 0U; k < (sizeof(k_map) / sizeof(k_map[0])); ++k) {
    if (strcmp(lower, k_map[k].word) == 0) {
      return k_map[k].tag;
    }
  }
  return k_ra8_reflow_tag_unknown;
}

/**
 * @brief Decode a `&#dec;` / `&#xhex;` numeric character reference.
 *
 * @details Assumes `src` begins with "&#". Reads an optional `x`/`X` for
 * hexadecimal, then base-appropriate digits up to a terminating ';'.
 *
 * @param[in]  src      Buffer positioned at the '&' of "&#...".
 * @param[in]  avail    Bytes available from `src`.
 * @param[out] out_cp   Decoded code point on success.
 * @param[out] out_used Bytes consumed (through ';') on success.
 * @return true if a complete numeric reference was parsed.
 * @retval false No digits, bad digit, or missing terminator.
 * @pre `src`, `out_cp`, `out_used` are non-null.
 * @pre `src[0..1]` are "&#".
 * @post On false the output params are unspecified.
 * @post On true *out_used is the index just past ';'.
 * @note Pure aside from writing the output params.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_decode_numeric(const char* src, size_t avail, uint32_t* out_cp, size_t* out_used)
{
  size_t   i    = 2U; /* past "&#" */
  uint32_t base = (uint32_t)k_priv_base_dec;
  /* mcdc-deactivated: the sole caller ra8_reflow_tok_decode_entity guarantees window >= k_priv_entity_min (4) before delegating, so i == 2 < avail always holds; the (i < avail) bound cannot be flipped false on any public path. */
  if ((i < avail) && ((src[i] == 'x') || (src[i] == 'X'))) {
    base = (uint32_t)k_priv_base_hex;
    ++i;
  }
  uint32_t cp     = 0U;
  size_t   digits = 0U;
  while ((i < avail) && (src[i] != ';')) {
    char     c = src[i];
    uint32_t d; /* assigned on every non-returning path below */
    if ((c >= '0') && (c <= '9')) {
      d = (uint32_t)(c - '0');
    } else if ((base == (uint32_t)k_priv_base_hex) && (c >= 'a') && (c <= 'f')) {
      d = (uint32_t)(c - 'a') + (uint32_t)k_priv_hex_offset;
    } else if ((base == (uint32_t)k_priv_base_hex) && (c >= 'A') && (c <= 'F')) {
      d = (uint32_t)(c - 'A') + (uint32_t)k_priv_hex_offset;
    } else {
      return false;
    }
    cp = (cp * base) + d;
    ++digits;
    ++i;
  }
  /* mcdc-deactivated: the scan loop above exits with i < avail only when src[i] == ';', so (src[i] != ';') is co-determined by (i >= avail) and can never independently flip; its independence pair is structurally unreachable. */
  if ((digits == 0U) || (i >= avail) || (src[i] != ';')) {
    return false;
  }
  *out_cp   = cp;
  *out_used = i + 1U;
  return true;
}

bool ra8_reflow_tok_decode_entity(const char* src, size_t avail, uint32_t* out_cp, size_t* out_used)
{
  const size_t window =
    (avail < (size_t)k_priv_entity_window) ? avail : (size_t)k_priv_entity_window;
  if ((window < (size_t)k_priv_entity_min) || (src[1] == '\0')) {
    return false;
  }
  if (src[1] == '#') {
    return priv_decode_numeric(src, window, out_cp, out_used);
  }
  static const struct {
    const char* word; /**< Word. */
    uint32_t    cp;   /**< Cp.   */
  } k_named[] = {
    {"amp", (uint32_t)'&'},
    {"lt", (uint32_t)'<'},
    {"gt", (uint32_t)'>'},
    {"quot", (uint32_t)'"'},
    {"apos", (uint32_t)'\''},
  };
  for (size_t e = 0U; e < (sizeof(k_named) / sizeof(k_named[0])); ++e) {
    const size_t wlen = strlen(k_named[e].word);
    if (((wlen + 2U) <= window) && (src[1U + wlen] == ';') &&
        (strncmp(&src[1], k_named[e].word, wlen) == 0)) {
      *out_cp   = k_named[e].cp;
      *out_used = wlen + 2U;
      return true;
    }
  }
  return false;
}

/* ===========================================================================
 * Block / inline-style classification
 * ===========================================================================
 */

/**
 * @brief Test whether a tag is a block-flow container in the v1 subset.
 *
 * @details Block tags emit block-start / block-end tokens; all others do
 * not introduce a block box.
 *
 * @param[in] tag Classified tag.
 * @return true for p / h1-h6 / ul / ol / li / blockquote.
 * @retval false Any inline, void, or unknown tag.
 * @pre None.
 * @pre None.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function.
 * @since 0.1.0
 */
bool ra8_reflow_tok_is_block(ra8_reflow_html_tag_t tag)
{
  switch (tag) {
    case k_ra8_reflow_tag_p:
    case k_ra8_reflow_tag_h1:
    case k_ra8_reflow_tag_h2:
    case k_ra8_reflow_tag_h3:
    case k_ra8_reflow_tag_h4:
    case k_ra8_reflow_tag_h5:
    case k_ra8_reflow_tag_h6:
    case k_ra8_reflow_tag_ul:
    case k_ra8_reflow_tag_ol:
    case k_ra8_reflow_tag_li:
    case k_ra8_reflow_tag_blockquote:
    case k_ra8_reflow_tag_table:
    case k_ra8_reflow_tag_tr:
    case k_ra8_reflow_tag_td:
    case k_ra8_reflow_tag_th:
      return true;
    default:
      return false;
  }
}

/**
 * @brief Return the style bit an inline tag contributes to its subtree.
 *
 * @details b/strong add bold, i/em add italic, a adds underline; every
 * other tag contributes nothing.
 *
 * @param[in] tag Classified tag.
 * @return The ra8_reflow_style bit to OR in, or 0.
 * @retval 0 The tag carries no inline style.
 * @pre None.
 * @pre None.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function.
 * @since 0.1.0
 */
uint8_t ra8_reflow_tok_style_for(ra8_reflow_html_tag_t tag)
{
  switch (tag) {
    case k_ra8_reflow_tag_b:
    case k_ra8_reflow_tag_strong:
      return (uint8_t)k_ra8_reflow_style_bold;
    case k_ra8_reflow_tag_i:
    case k_ra8_reflow_tag_em:
      return (uint8_t)k_ra8_reflow_style_italic;
    case k_ra8_reflow_tag_a:
      return (uint8_t)k_ra8_reflow_style_underline;
    default:
      return 0U;
  }
}

/* ===========================================================================
 * Token-pool emit + text-pool feed / stash + literal scanners
 * ===========================================================================
 */

/**
 * @brief Append one token to the engine's token pool.
 *
 * @details Fills the next ra8_reflow_token_t and advances token_count, or
 * fails if the fixed pool is full.
 *
 * @param[in,out] engine Engine whose token pool is appended to.
 * @param[in]     kind   Token kind.
 * @param[in]     tag    Associated tag (or k_ra8_reflow_tag_unknown).
 * @param[in]     style  Inline-style bits for the token.
 * @param[in]     off    Text-pool offset (text tokens only).
 * @param[in]     len    Text-pool length (text tokens only).
 * @return true on success, false on pool overflow.
 * @retval false The token pool is full.
 * @pre `engine` is non-null.
 * @pre `engine->token_count <= k_ra8_reflow_max_tokens`.
 * @post On success token_count is incremented by one.
 * @post On failure the pool is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool ra8_reflow_tok_emit(ra8_reflow_t*           engine,
                         ra8_reflow_token_kind_t kind,
                         ra8_reflow_html_tag_t   tag,
                         uint8_t                 style,
                         uint32_t                off,
                         uint32_t                len)
{
  if (engine->token_count >= (uint32_t)k_ra8_reflow_max_tokens) {
    return false;
  }
  ra8_reflow_token_t* tok = &engine->tokens[engine->token_count];
  tok->kind               = (uint8_t)kind;
  tok->tag                = (uint8_t)tag;
  tok->style              = style;
  tok->reserved           = 0U;
  tok->text_off           = off;
  tok->text_len           = len;
  tok->color              = (uint32_t)k_ra8_reflow_color_inherit;
  tok->css_font_px        = 0U;
  tok->reserved16         = 0U;
  engine->token_count++;
  return true;
}

/**
 * @brief Write one byte through the whitespace-collapse state machine.
 *
 * @details Runs of ASCII whitespace collapse to a single space; a run that
 * begins with `*last_ws` true (start of node) drops its leading whitespace.
 *
 * @param[in,out] engine  Engine whose text pool is appended to.
 * @param[in]     ch      Byte to write.
 * @param[in,out] last_ws Collapse state (true if the prior byte was space).
 * @return true on success, false on text-pool overflow.
 * @retval false The text pool is full.
 * @pre `engine` and `last_ws` are non-null.
 * @pre `engine->text_pool_used <= k_ra8_reflow_text_pool_bytes`.
 * @post On a written byte text_pool_used is incremented.
 * @post Collapsed (skipped) whitespace leaves the pool unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool ra8_reflow_tok_feed(ra8_reflow_t* engine, char ch, bool* last_ws)
{
  if (ra8_reflow_tok_is_xml_whitespace(ch)) {
    if (*last_ws) {
      return true;
    }
    ch       = ' ';
    *last_ws = true;
  } else {
    *last_ws = false;
  }
  if (engine->text_pool_used >= (uint32_t)k_ra8_reflow_text_pool_bytes) {
    return false;
  }
  engine->text_pool[engine->text_pool_used] = (uint8_t)ch;
  engine->text_pool_used++;
  return true;
}

/**
 * @brief Feed a code point's UTF-8 bytes through the whitespace-collapse.
 *
 * @details Encodes `cp` to 1-4 bytes and feeds each via ra8_reflow_tok_feed().
 *
 * @param[in,out] engine  Engine whose text pool is appended to.
 * @param[in]     cp      Code point to encode.
 * @param[in,out] last_ws Whitespace-collapse state.
 * @return true on success, false on text-pool overflow.
 * @retval false The text pool is full.
 * @pre `engine` and `last_ws` are non-null.
 * @pre `cp` is a Unicode code point (clamped by the encoder).
 * @post On success 1-4 bytes are appended to the pool.
 * @post On failure the pool may be partially written.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_feed_utf8(ra8_reflow_t* engine, uint32_t cp, bool* last_ws)
{
  uint8_t      enc[4];
  const size_t n = ra8_reflow_tok_utf8_encode(cp, enc);
  for (size_t b = 0U; b < n; ++b) {
    if (!ra8_reflow_tok_feed(engine, (char)enc[b], last_ws)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Stash one source position of a text run (entity or literal byte).
 *
 * @details If `buf[i]` starts a recognised entity it is decoded and emitted
 * as UTF-8; otherwise the single byte (including an unrecognised '&') is
 * fed through the whitespace-collapse.
 *
 * @param[in,out] engine   Engine whose text pool is appended to.
 * @param[in]     buf      Source buffer.
 * @param[in]     i        Current offset within the run.
 * @param[in]     end      End offset of the run (exclusive).
 * @param[in,out] last_ws  Whitespace-collapse state.
 * @param[out]    consumed Source bytes consumed (1, or the entity length).
 * @return true on success, false on text-pool overflow.
 * @retval false The text pool is full.
 * @pre `engine`, `buf`, `last_ws`, `consumed` are non-null.
 * @pre `i < end`.
 * @post On success *consumed is in [1, end - i].
 * @post On failure the pool may be partially written.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_stash_one(ra8_reflow_t*  engine,
                           const uint8_t* buf,
                           size_t         i,
                           size_t         end,
                           bool*          last_ws,
                           size_t*        consumed)
{
  if (buf[i] == '&') {
    uint32_t cp   = 0U;
    size_t   used = 0U;
    if (ra8_reflow_tok_decode_entity((const char*)&buf[i], end - i, &cp, &used)) {
      *consumed = used;
      return priv_feed_utf8(engine, cp, last_ws);
    }
  }
  *consumed = 1U;
  return ra8_reflow_tok_feed(engine, (char)buf[i], last_ws);
}

/**
 * @brief Entity-decode and whitespace-collapse a text run into the pool.
 *
 * @details Walks the run one source position at a time via priv_stash_one(),
 * which decodes a recognised entity to UTF-8 (else passes '&' through) and
 * collapses whitespace.
 *
 * @param[in,out] engine  Engine whose text pool is appended to.
 * @param[in]     buf     Source buffer.
 * @param[in]     start   Start offset of the run (inclusive).
 * @param[in]     end     End offset of the run (exclusive).
 * @param[out]    out_off Text-pool offset of the stored run.
 * @param[out]    out_len Number of bytes stored.
 * @return true on success, false on text-pool overflow.
 * @retval false The text pool is full.
 * @pre `engine`, `buf`, `out_off`, `out_len` are non-null.
 * @pre `start <= end`.
 * @post On success *out_off / *out_len describe the stored slice.
 * @post On failure the pool may be partially written and is abandoned.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool ra8_reflow_tok_stash_run(ra8_reflow_t*  engine,
                              const uint8_t* buf,
                              size_t         start,
                              size_t         end,
                              uint32_t*      out_off,
                              uint32_t*      out_len)
{
  *out_off       = engine->text_pool_used;
  bool   last_ws = true; /* drop leading whitespace */
  size_t i       = start;
  while (i < end) {
    size_t consumed = 0U;
    if (!priv_stash_one(engine, buf, i, end, &last_ws, &consumed)) {
      return false;
    }
    i += consumed;
  }
  *out_len = engine->text_pool_used - *out_off;
  return true;
}

/**
 * @brief Test whether `buf` at `i` begins with the literal `lit`.
 *
 * @details Bounds-checks that `lit` fits within `[i, len)` before comparing.
 *
 * @param[in] buf Source buffer.
 * @param[in] i   Offset to test from.
 * @param[in] len Total buffer length.
 * @param[in] lit NUL-terminated literal to match.
 * @return true if the literal fits and matches at `i`.
 * @retval false Out of range or mismatch.
 * @pre `buf` and `lit` are non-null.
 * @pre `i <= len`.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function.
 * @since 0.1.0
 */
bool ra8_reflow_tok_starts_with(const uint8_t* buf, size_t i, size_t len, const char* lit)
{
  const size_t n = strlen(lit);
  if ((i + n) > len) {
    return false;
  }
  return memcmp(&buf[i], lit, n) == 0;
}

/**
 * @brief Return the index just past the first occurrence of `lit`.
 *
 * @details Linear bounded scan; used to skip comments / PIs / declarations.
 *
 * @param[in] buf Source buffer.
 * @param[in] i   Offset to search from.
 * @param[in] len Total buffer length.
 * @param[in] lit NUL-terminated literal to find.
 * @return Index just past `lit`, or `len` if not found.
 * @retval len The literal does not occur in `[i, len)`.
 * @pre `buf` and `lit` are non-null.
 * @pre `i <= len`.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function.
 * @since 0.1.0
 */
size_t ra8_reflow_tok_skip_past(const uint8_t* buf, size_t i, size_t len, const char* lit)
{
  const size_t n = strlen(lit);
  while ((i + n) <= len) {
    if (memcmp(&buf[i], lit, n) == 0) {
      return i + n;
    }
    ++i;
  }
  return len;
}

/**
 * @brief Return the index of the first occurrence of `lit` (its start), or len.
 *
 * @details Like ::ra8_reflow_tok_skip_past but returns the literal's START offset, so the
 * caller can both bound the preceding content and resume past the literal.
 *
 * @param[in] buf Source buffer.
 * @param[in] i   Offset to search from.
 * @param[in] len Total buffer length.
 * @param[in] lit NUL-terminated literal to find.
 * @return Start index of `lit`, or `len` if absent.
 * @retval len The literal does not occur in `[i, len)`.
 * @pre `buf` and `lit` are non-null.
 * @pre `i <= len`.
 * @post No state is modified (pure).
 * @post Return value is in `[i, len]`.
 * @note Pure function.
 * @since 0.1.0
 */
size_t ra8_reflow_tok_find_lit(const uint8_t* buf, size_t i, size_t len, const char* lit)
{
  const size_t n = strlen(lit);
  while ((i + n) <= len) {
    if (memcmp(&buf[i], lit, n) == 0) {
      return i;
    }
    ++i;
  }
  return len;
}
