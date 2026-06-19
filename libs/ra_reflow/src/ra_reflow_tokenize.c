/**
 * @file ra_reflow_tokenize.c
 * @brief No-heap streaming XHTML tokenizer for ra_reflow (replaces the
 *        tinyxml2 DOM shim).
 *
 * @details
 * Scans the XHTML byte buffer in a single forward pass and populates the
 * engine's fixed token / text pools (`engine->tokens[]`,
 * `engine->text_pool[]`) directly -- no DOM, no heap. This is the on-target
 * parse path: the firmware traps `_sbrk`, and tinyxml2's `MemPoolT` grows
 * via `new`, so the DOM walk could not run on the device.
 *
 * Output is byte-for-byte equivalent to the former tinyxml2 walk for the
 * v1 conformance subset: a pre-order traversal that emits block-start /
 * block-end tokens for block elements, single tokens for the void
 * elements (br/hr/img), no token for inline-style elements (their style
 * bit is OR'd into descendant text), and pass-through for unknown tags.
 * Text runs are entity-decoded and whitespace-collapsed per text node
 * (HTML rules: runs of ASCII whitespace collapse to one space; a node's
 * leading whitespace is dropped). Inline style (bold/italic/underline)
 * propagates down a bounded explicit stack -- no recursion (NASA P10
 * Rule 1) and bounded loops (Rule 2).
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

#include "ra_err.h"
#include "ra_reflow.h"
#include "ra_reflow_tokenize_internal.h"

/* ===========================================================================
 * Internal sizing constants (no magic numbers).
 * ===========================================================================
 */

/**
 * @enum priv_tok_consts_t
 * @brief Tokenizer bounds and UTF-8 / numeric-entity encoding constants.
 */
typedef enum : uint32_t {
  k_priv_max_depth     = 64U,       /**< Max element nesting (matches old shim). */
  k_priv_tag_name_cap  = 16U,       /**< Lower-cased tag-name buffer size.       */
  k_priv_entity_window = 12U,       /**< Max bytes scanned for one '&...;'.      */
  k_priv_entity_min    = 4U,        /**< Shortest valid reference ("&lt;").      */
  k_priv_uc_2byte      = 0x80U,     /**< Code points >= need >= 2 UTF-8 bytes.   */
  k_priv_uc_3byte      = 0x800U,    /**< Code points >= need >= 3 UTF-8 bytes.   */
  k_priv_uc_4byte      = 0x10000U,  /**< Code points >= need 4 UTF-8 bytes.      */
  k_priv_uc_max        = 0x10FFFFU, /**< Highest valid Unicode code point.       */
  k_priv_utf8_lead2    = 0xC0U,     /**< 2-byte sequence lead-byte prefix.       */
  k_priv_utf8_lead3    = 0xE0U,     /**< 3-byte sequence lead-byte prefix.       */
  k_priv_utf8_lead4    = 0xF0U,     /**< 4-byte sequence lead-byte prefix.       */
  k_priv_utf8_cont     = 0x80U,     /**< Continuation-byte prefix.               */
  k_priv_utf8_mask     = 0x3FU,     /**< Low 6 bits per continuation byte.       */
  k_priv_utf8_sh6      = 6U,        /**< Shift for one continuation byte.        */
  k_priv_utf8_sh12     = 12U,       /**< Shift for two continuation bytes.       */
  k_priv_utf8_sh18     = 18U,       /**< Shift for three continuation bytes.     */
  k_priv_base_dec      = 10U,       /**< Decimal numeric-entity base.            */
  k_priv_base_hex      = 16U,       /**< Hexadecimal numeric-entity base.        */
  k_priv_hex_offset    = 10U,       /**< Value of hex 'a'/'A' minus the letter.  */
} priv_tok_consts_t;

/**
 * @struct tok_ctx_t
 * @brief Mutable tokenizer state threaded through the scan helpers.
 */
typedef struct {
  ra_reflow_t* engine;                        /**< Target engine pools.        */
  uint8_t      style;                         /**< Current inline-style bits.  */
  uint8_t      active_link;                   /**< 1-based link id (0 = none).  */
  uint8_t      stack_tag[k_priv_max_depth];   /**< Open-element tag stack.      */
  uint8_t      stack_style[k_priv_max_depth]; /**< Style to restore on pop.    */
  uint8_t      stack_link[k_priv_max_depth];  /**< Link id to restore on pop.  */
  uint32_t     sp;                            /**< Stack depth.                */
  bool         saw_element;                   /**< At least one element seen.  */
} tok_ctx_t;

/* ===========================================================================
 * Public (test-exposed) helpers -- documented in ra_reflow_tokenize_internal.h
 * ===========================================================================
 */

bool ra_reflow_tok_is_xml_whitespace(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r') || (c == '\f') || (c == '\v');
}

size_t ra_reflow_tok_utf8_encode(uint32_t cp, uint8_t* dst)
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

ra_reflow_html_tag_t ra_reflow_tok_classify(const char* name, size_t len)
{
  if (name == nullptr) {
    return k_ra_reflow_tag_unknown;
  }
  char lower[k_priv_tag_name_cap];
  (void)priv_local_lower(name, len, lower);

  static const struct {
    const char*          word;
    ra_reflow_html_tag_t tag;
  } k_map[] = {
    {"p", k_ra_reflow_tag_p},
    {"h1", k_ra_reflow_tag_h1},
    {"h2", k_ra_reflow_tag_h2},
    {"h3", k_ra_reflow_tag_h3},
    {"h4", k_ra_reflow_tag_h4},
    {"h5", k_ra_reflow_tag_h5},
    {"h6", k_ra_reflow_tag_h6},
    {"em", k_ra_reflow_tag_em},
    {"strong", k_ra_reflow_tag_strong},
    {"b", k_ra_reflow_tag_b},
    {"i", k_ra_reflow_tag_i},
    {"br", k_ra_reflow_tag_br},
    {"hr", k_ra_reflow_tag_hr},
    {"ul", k_ra_reflow_tag_ul},
    {"ol", k_ra_reflow_tag_ol},
    {"li", k_ra_reflow_tag_li},
    {"blockquote", k_ra_reflow_tag_blockquote},
    {"a", k_ra_reflow_tag_a},
    {"img", k_ra_reflow_tag_img},
    {"table", k_ra_reflow_tag_table},
    {"tr", k_ra_reflow_tag_tr},
    {"td", k_ra_reflow_tag_td},
    {"th", k_ra_reflow_tag_th},
  };
  for (size_t k = 0U; k < (sizeof(k_map) / sizeof(k_map[0])); ++k) {
    if (strcmp(lower, k_map[k].word) == 0) {
      return k_map[k].tag;
    }
  }
  return k_ra_reflow_tag_unknown;
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
static bool priv_decode_numeric(const char* src, size_t avail, uint32_t* out_cp, size_t* out_used)
{
  size_t   i    = 2U; /* past "&#" */
  uint32_t base = (uint32_t)k_priv_base_dec;
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
  if ((digits == 0U) || (i >= avail) || (src[i] != ';')) {
    return false;
  }
  *out_cp   = cp;
  *out_used = i + 1U;
  return true;
}

bool ra_reflow_tok_decode_entity(const char* src, size_t avail, uint32_t* out_cp, size_t* out_used)
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
    const char* word;
    uint32_t    cp;
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
 * Internal flow helpers
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
static bool priv_is_block(ra_reflow_html_tag_t tag)
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
    case k_ra_reflow_tag_table:
    case k_ra_reflow_tag_tr:
    case k_ra_reflow_tag_td:
    case k_ra_reflow_tag_th:
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
 * @return The ra_reflow_style bit to OR in, or 0.
 * @retval 0 The tag carries no inline style.
 * @pre None.
 * @pre None.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function.
 * @since 0.1.0
 */
static uint8_t priv_style_for(ra_reflow_html_tag_t tag)
{
  switch (tag) {
    case k_ra_reflow_tag_b:
    case k_ra_reflow_tag_strong:
      return (uint8_t)k_ra_reflow_style_bold;
    case k_ra_reflow_tag_i:
    case k_ra_reflow_tag_em:
      return (uint8_t)k_ra_reflow_style_italic;
    case k_ra_reflow_tag_a:
      return (uint8_t)k_ra_reflow_style_underline;
    default:
      return 0U;
  }
}

/**
 * @brief Append one token to the engine's token pool.
 *
 * @details Fills the next ra_reflow_token_t and advances token_count, or
 * fails if the fixed pool is full.
 *
 * @param[in,out] engine Engine whose token pool is appended to.
 * @param[in]     kind   Token kind.
 * @param[in]     tag    Associated tag (or k_ra_reflow_tag_unknown).
 * @param[in]     style  Inline-style bits for the token.
 * @param[in]     off    Text-pool offset (text tokens only).
 * @param[in]     len    Text-pool length (text tokens only).
 * @return true on success, false on pool overflow.
 * @retval false The token pool is full.
 * @pre `engine` is non-null.
 * @pre `engine->token_count <= k_ra_reflow_max_tokens`.
 * @post On success token_count is incremented by one.
 * @post On failure the pool is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool priv_emit(ra_reflow_t*           engine,
                      ra_reflow_token_kind_t kind,
                      ra_reflow_html_tag_t   tag,
                      uint8_t                style,
                      uint32_t               off,
                      uint32_t               len)
{
  if (engine->token_count >= (uint32_t)k_ra_reflow_max_tokens) {
    return false;
  }
  ra_reflow_token_t* tok = &engine->tokens[engine->token_count];
  tok->kind              = (uint8_t)kind;
  tok->tag               = (uint8_t)tag;
  tok->style             = style;
  tok->reserved          = 0U;
  tok->text_off          = off;
  tok->text_len          = len;
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
 * @pre `engine->text_pool_used <= k_ra_reflow_text_pool_bytes`.
 * @post On a written byte text_pool_used is incremented.
 * @post Collapsed (skipped) whitespace leaves the pool unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool priv_feed(ra_reflow_t* engine, char ch, bool* last_ws)
{
  if (ra_reflow_tok_is_xml_whitespace(ch)) {
    if (*last_ws) {
      return true;
    }
    ch       = ' ';
    *last_ws = true;
  } else {
    *last_ws = false;
  }
  if (engine->text_pool_used >= (uint32_t)k_ra_reflow_text_pool_bytes) {
    return false;
  }
  engine->text_pool[engine->text_pool_used] = (uint8_t)ch;
  engine->text_pool_used++;
  return true;
}

/**
 * @brief Entity-decode and whitespace-collapse a text run into the pool.
 *
 * @details Recognised entities are decoded to UTF-8 then collapsed with the
 * surrounding text; unrecognised '&' sequences pass through literally.
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
/**
 * @brief Feed a code point's UTF-8 bytes through the whitespace-collapse.
 *
 * @details Encodes `cp` to 1-4 bytes and feeds each via priv_feed().
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
static bool priv_feed_utf8(ra_reflow_t* engine, uint32_t cp, bool* last_ws)
{
  uint8_t      enc[4];
  const size_t n = ra_reflow_tok_utf8_encode(cp, enc);
  for (size_t b = 0U; b < n; ++b) {
    if (!priv_feed(engine, (char)enc[b], last_ws)) {
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
static bool priv_stash_one(ra_reflow_t*   engine,
                           const uint8_t* buf,
                           size_t         i,
                           size_t         end,
                           bool*          last_ws,
                           size_t*        consumed)
{
  if (buf[i] == '&') {
    uint32_t cp   = 0U;
    size_t   used = 0U;
    if (ra_reflow_tok_decode_entity((const char*)&buf[i], end - i, &cp, &used)) {
      *consumed = used;
      return priv_feed_utf8(engine, cp, last_ws);
    }
  }
  *consumed = 1U;
  return priv_feed(engine, (char)buf[i], last_ws);
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
static bool priv_stash_run(ra_reflow_t*   engine,
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
static bool priv_starts_with(const uint8_t* buf, size_t i, size_t len, const char* lit)
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
static size_t priv_skip_past(const uint8_t* buf, size_t i, size_t len, const char* lit)
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

/* ===========================================================================
 * Markup handlers (advance *pi)
 * ===========================================================================
 */

/**
 * @brief Handle a CDATA section, emitting its inner text verbatim.
 *
 * @details Inner bytes are whitespace-collapsed but NOT entity-decoded
 * (matching XML CDATA semantics). Emits a text token when inside an element
 * and the collapsed run is non-empty.
 *
 * @param[in,out] ctx Tokenizer context.
 * @param[in]     buf Source buffer.
 * @param[in,out] pi  Cursor at "<![CDATA["; advanced past "]]>".
 * @param[in]     len Total buffer length.
 * @return k_ra_ok, or k_ra_err_no_mem on pool overflow.
 * @retval k_ra_ok Section consumed.
 * @pre `ctx`, `buf`, `pi` are non-null.
 * @pre `buf[*pi]` begins "<![CDATA[".
 * @post `*pi` advances past the section (or to `len`).
 * @post Token / text pools may grow by one text run.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_handle_cdata(tok_ctx_t* ctx, const uint8_t* buf, size_t* pi, size_t len)
{
  const size_t inner = *pi + strlen("<![CDATA[");
  size_t       close = inner;
  while (((close + 3U) <= len) && (memcmp(&buf[close], "]]>", 3U) != 0)) {
    ++close;
  }
  if (ctx->sp > 0U) {
    uint32_t off     = ctx->engine->text_pool_used;
    bool     last_ws = true;
    for (size_t k = inner; k < close; ++k) {
      if (!priv_feed(ctx->engine, (char)buf[k], &last_ws)) {
        return k_ra_err_no_mem;
      }
    }
    const uint32_t tlen = ctx->engine->text_pool_used - off;
    if ((tlen > 0U) && !priv_emit(ctx->engine,
                                  k_ra_reflow_tok_text,
                                  k_ra_reflow_tag_unknown,
                                  ctx->style,
                                  off,
                                  tlen)) {
      return k_ra_err_no_mem;
    }
  }
  *pi = ((close + 3U) <= len) ? (close + 3U) : len;
  return k_ra_ok;
}

/**
 * @brief Handle a closing tag, popping the stack and emitting block-end.
 *
 * @details Restores the style saved at the matching open and, if that
 * element was a block, emits its block-end token.
 *
 * @param[in,out] ctx Tokenizer context.
 * @param[in]     buf Source buffer.
 * @param[in,out] pi  Cursor at "</"; advanced past '>'.
 * @param[in]     len Total buffer length.
 * @return k_ra_ok, k_ra_err_no_mem, or k_ra_err_validation_failed.
 * @retval k_ra_err_validation_failed Stray end tag (empty stack).
 * @pre `ctx`, `buf`, `pi` are non-null.
 * @pre `buf[*pi..*pi+1]` are "</".
 * @post `*pi` advances past the tag (or to `len`).
 * @post The element stack shrinks by one on success.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_handle_end(tok_ctx_t* ctx, const uint8_t* buf, size_t* pi, size_t len)
{
  size_t i = *pi + 2U; /* past "</" */
  while ((i < len) && (buf[i] != '>')) {
    ++i;
  }
  *pi = (i < len) ? (i + 1U) : len;
  if (ctx->sp == 0U) {
    return k_ra_err_validation_failed;
  }
  ctx->sp--;
  const ra_reflow_html_tag_t tag = (ra_reflow_html_tag_t)ctx->stack_tag[ctx->sp];
  ctx->style                     = ctx->stack_style[ctx->sp];
  ctx->active_link               = ctx->stack_link[ctx->sp];
  if (priv_is_block(tag) &&
      !priv_emit(ctx->engine, k_ra_reflow_tok_block_end, tag, ctx->style, 0U, 0U)) {
    return k_ra_err_no_mem;
  }
  return k_ra_ok;
}

/**
 * @brief Parse a start tag's name and self-close flag, advancing past '>'.
 *
 * @details Skips attributes quote-aware so a '>' inside an attribute value
 * does not end the tag.
 *
 * @param[in]     buf       Source buffer.
 * @param[in,out] pi        Cursor at '<'; advanced past '>'.
 * @param[in]     len       Total buffer length.
 * @param[out]    selfclose Set true for a "/>"-terminated tag.
 * @return The classified tag.
 * @retval k_ra_reflow_tag_unknown Unrecognised element name.
 * @pre `buf`, `pi`, `selfclose` are non-null.
 * @pre `buf[*pi] == '<'`.
 * @post `*pi` advances past the tag (or to `len`).
 * @post `*selfclose` reflects the "/>" terminator.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_reflow_html_tag_t
priv_parse_start(const uint8_t* buf, size_t* pi, size_t len, bool* selfclose)
{
  size_t       i      = *pi + 1U; /* past '<' */
  const size_t nstart = i;
  while ((i < len) && (buf[i] != '>') && (buf[i] != '/') &&
         !ra_reflow_tok_is_xml_whitespace((char)buf[i])) {
    ++i;
  }
  const ra_reflow_html_tag_t tag = ra_reflow_tok_classify((const char*)&buf[nstart], i - nstart);
  *selfclose                     = false;
  while ((i < len) && (buf[i] != '>')) {
    const char c = (char)buf[i];
    if ((c == '"') || (c == '\'')) {
      ++i;
      while ((i < len) && (buf[i] != (uint8_t)c)) {
        ++i;
      }
    } else if ((c == '/') && ((i + 1U) < len) && (buf[i + 1U] == '>')) {
      *selfclose = true;
    }
    ++i;
  }
  *pi = (i < len) ? (i + 1U) : len;
  return tag;
}

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
static bool priv_attr_name_at(const uint8_t* tag, size_t i, const char* name, size_t name_len)
{
  for (size_t k = 0U; k < name_len; ++k) {
    if ((tag[i + k] | 0x20U) != (uint8_t)name[k]) {
      return false;
    }
  }
  const char prev = (i == 0U) ? '<' : (char)tag[i - 1U];
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
static bool
priv_attr_quoted_value(const uint8_t* tag, size_t tag_len, size_t pos, size_t* vstart, size_t* vlen)
{
  size_t j = pos;
  while ((j < tag_len) && ra_reflow_tok_is_xml_whitespace((char)tag[j])) {
    ++j;
  }
  if ((j >= tag_len) || (tag[j] != '=')) {
    return false;
  }
  ++j;
  while ((j < tag_len) && ra_reflow_tok_is_xml_whitespace((char)tag[j])) {
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
 * @brief Extract a named attribute's quoted value from a tag span into the pool.
 *
 * @details Scans the raw `<...>` span for attribute @p name (case-insensitive,
 * not preceded by another name character) and copies its quoted value verbatim
 * into the engine text pool. Used for `<img src>` / `<a href>` / element `id`.
 * On no match, a malformed attribute, or a full pool the outputs are zeroed,
 * which callers treat as "attribute absent".
 *
 * @param[in,out] engine   Engine whose text pool receives the value bytes.
 * @param[in]     tag      Raw tag span starting at '<'.
 * @param[in]     tag_len  Length of @p tag, bytes.
 * @param[in]     name     Lower-case attribute name to find.
 * @param[in]     name_len Length of @p name, bytes.
 * @param[out]    out_off  Receives the text-pool offset of the value (0 if none).
 * @param[out]    out_len  Receives the value byte length (0 if none).
 * @return None.
 * @pre `engine`, `tag`, `name`, `out_off`, `out_len` are non-null.
 * @pre `engine->text_pool_used <= k_ra_reflow_text_pool_bytes`.
 * @post `*out_len > 0` iff a quoted @p name value was stored in the pool.
 * @post The pool grows by `*out_len` bytes when a value is stored.
 * @note Not thread-safe.
 * @since 0.1.0
 */
/**
 * @brief Locate a named attribute's quoted value span within a tag (no copy).
 *
 * @details The non-copying core of priv_capture_attr(): returns the value's
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
 * @post No state mutated.
 * @note Pure read of @p tag.
 * @since 0.1.0
 */
static bool priv_find_attr(const uint8_t* tag,
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

static void priv_capture_attr(ra_reflow_t*   engine,
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
  if (!priv_find_attr(tag, tag_len, name, name_len, &vstart, &vlen)) {
    return;
  }
  if ((vlen == 0U) ||
      ((size_t)engine->text_pool_used + vlen > (size_t)k_ra_reflow_text_pool_bytes)) {
    return;
  }
  const uint32_t off = engine->text_pool_used;
  memcpy(&engine->text_pool[off], &tag[vstart], vlen);
  engine->text_pool_used += (uint32_t)vlen;
  *out_off = off;
  *out_len = (uint32_t)vlen;
}

/**
 * @brief Parse a `text-align` value out of an inline `style` attribute value.
 *
 * @details Scans @p style for "text-align", skips to past the ':', and maps the
 * first value letter: j->justify, c->centre, r->right, anything else->left.
 * Bounded, case-insensitive, zero-alloc.
 *
 * @param[in] style Inline style value bytes (the part inside the quotes).
 * @param[in] len   Length of @p style, bytes.
 * @return The parsed alignment, or ::k_ra_reflow_align_left if unset.
 * @retval k_ra_reflow_align_left No usable `text-align` found.
 * @pre `style` is non-null and holds @p len bytes.
 * @post No state mutated.
 * @note Pure function.
 * @since 0.1.0
 */
static ra_reflow_align_t priv_parse_text_align(const uint8_t* style, size_t len)
{
  static const char k_key[] = "text-align";
  const size_t      klen    = sizeof(k_key) - 1U;
  for (size_t i = 0U; (i + klen) <= len; ++i) {
    if (!priv_attr_name_at(style, i, k_key, klen)) {
      continue;
    }
    size_t j = i + klen;
    while ((j < len) && (style[j] != ':')) {
      ++j;
    }
    ++j; /* past ':' */
    while ((j < len) && ra_reflow_tok_is_xml_whitespace((char)style[j])) {
      ++j;
    }
    if (j >= len) {
      return k_ra_reflow_align_left;
    }
    const char value = (char)(style[j] | 0x20U);
    if (value == 'j') {
      return k_ra_reflow_align_justify;
    }
    if (value == 'c') {
      return k_ra_reflow_align_center;
    }
    if (value == 'r') {
      return k_ra_reflow_align_right;
    }
    return k_ra_reflow_align_left;
  }
  return k_ra_reflow_align_left;
}

/**
 * @brief Resolve a block element's text alignment from its inline `style`.
 *
 * @param[in] tag  Raw tag span starting at '<'.
 * @param[in] span Length of @p tag, bytes.
 * @return The block's alignment (left when no `style="text-align:..."`).
 * @retval k_ra_reflow_align_left No alignment declared.
 * @pre `tag` is non-null.
 * @post No state mutated.
 * @note Pure read of @p tag.
 * @since 0.1.0
 */
static ra_reflow_align_t priv_block_align(const uint8_t* tag, size_t span)
{
  size_t voff = 0U;
  size_t vlen = 0U;
  if (!priv_find_attr(tag, span, "style", sizeof("style") - 1U, &voff, &vlen)) {
    return k_ra_reflow_align_left;
  }
  return priv_parse_text_align(&tag[voff], vlen);
}

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
 * @pre `engine` is non-null.
 * @pre `engine->link_target_count <= k_ra_reflow_max_links`.
 * @post On a non-zero return the table grew by one entry.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static uint8_t priv_intern_link(ra_reflow_t* engine, uint32_t href_off, uint32_t href_len)
{
  if ((href_len == 0U) || (engine->link_target_count >= (uint32_t)k_ra_reflow_max_links)) {
    return 0U;
  }
  const uint32_t idx                 = engine->link_target_count;
  engine->link_targets[idx].href_off = href_off;
  engine->link_targets[idx].href_len = href_len;
  engine->link_target_count          = idx + 1U;
  return (uint8_t)(idx + 1U); /* 1-based: 0 means "no link" */
}

/**
 * @brief Capture a just-opened tag's `id` (block) or `href` (`<a>`).
 *
 * @details For a block tag, captures `id="..."` and emits the block-start token
 * carrying the id slice (so layout can record an anchor position). For an `<a>`,
 * captures `href="..."`, interns it, and sets the active link id on @p ctx.
 *
 * @param[in,out] ctx  Tokenizer context (active link + engine pools).
 * @param[in]     tag  Raw tag span starting at '<'.
 * @param[in]     span Length of @p tag, bytes.
 * @param[in]     kind Classified tag.
 * @param[in]     block True iff @p kind is a block element.
 * @return k_ra_ok, or k_ra_err_no_mem on a token-pool overflow.
 * @retval k_ra_err_no_mem Block-start token pool full.
 * @pre `ctx` and `tag` are non-null.
 * @pre `ctx->sp > 0` (the tag was already pushed).
 * @post For a block tag a block-start token is emitted (id slice or 0,0).
 * @post For `<a>` `ctx->active_link` reflects the interned href (0 if none).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_open_attrs(tok_ctx_t*           ctx,
                                const uint8_t*       tag,
                                size_t               span,
                                ra_reflow_html_tag_t kind,
                                bool                 block)
{
  if (block) {
    uint32_t id_off = 0U;
    uint32_t id_len = 0U;
    priv_capture_attr(ctx->engine, tag, span, "id", sizeof("id") - 1U, &id_off, &id_len);
    if (!priv_emit(ctx->engine, k_ra_reflow_tok_block_start, kind, ctx->style, id_off, id_len)) {
      return k_ra_err_no_mem;
    }
    /* Stash the block's text alignment in the block-start token's reserved byte
     * (left = 0, so unstyled blocks lay out byte-identically). */
    const ra_reflow_align_t align                               = priv_block_align(tag, span);
    ctx->engine->tokens[ctx->engine->token_count - 1U].reserved = (uint8_t)align;
    return k_ra_ok;
  }
  if (kind == k_ra_reflow_tag_a) {
    uint32_t href_off = 0U;
    uint32_t href_len = 0U;
    priv_capture_attr(ctx->engine, tag, span, "href", sizeof("href") - 1U, &href_off, &href_len);
    ctx->active_link = priv_intern_link(ctx->engine, href_off, href_len);
  }
  return k_ra_ok;
}

/**
 * @brief Emit the single token for a void tag (`<br>` / `<hr>` / `<img>`).
 *
 * @details `<br>` -> break, `<hr>` -> rule, `<img>` -> image (capturing its
 * `src`). Returns whether @p tag was a void tag so the caller can fall through
 * to ordinary open-tag handling otherwise.
 *
 * @param[in,out] ctx     Tokenizer context.
 * @param[in]     buf     Source buffer.
 * @param[in]     tag_lt  Index of the tag's '<'.
 * @param[in]     end     One past the tag's '>'.
 * @param[in]     tag     Classified tag.
 * @param[out]    out_err Receives the emit result when handled.
 * @return true iff @p tag was a void tag (then @p *out_err is set).
 * @retval true  Void tag handled; check @p *out_err.
 * @retval false Not a void tag; caller continues.
 * @pre `ctx`, `buf`, `out_err` are non-null.
 * @pre `end >= tag_lt`.
 * @post On true a single token was emitted (or the pool overflowed).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool priv_handle_void(tok_ctx_t*           ctx,
                             const uint8_t*       buf,
                             size_t               tag_lt,
                             size_t               end,
                             ra_reflow_html_tag_t tag,
                             ra_err_t*            out_err)
{
  if (tag == k_ra_reflow_tag_br) {
    *out_err = priv_emit(ctx->engine, k_ra_reflow_tok_break, tag, ctx->style, 0U, 0U)
                 ? k_ra_ok
                 : k_ra_err_no_mem;
    return true;
  }
  if (tag == k_ra_reflow_tag_hr) {
    *out_err = priv_emit(ctx->engine, k_ra_reflow_tok_rule, tag, ctx->style, 0U, 0U)
                 ? k_ra_ok
                 : k_ra_err_no_mem;
    return true;
  }
  if (tag == k_ra_reflow_tag_img) {
    uint32_t     src_off = 0U;
    uint32_t     src_len = 0U;
    const size_t span    = (end > tag_lt) ? (end - tag_lt) : 0U;
    priv_capture_attr(ctx->engine,
                      &buf[tag_lt],
                      span,
                      "src",
                      sizeof("src") - 1U,
                      &src_off,
                      &src_len);
    *out_err = priv_emit(ctx->engine, k_ra_reflow_tok_image, tag, ctx->style, src_off, src_len)
                 ? k_ra_ok
                 : k_ra_err_no_mem;
    return true;
  }
  return false;
}

/**
 * @brief Handle an opening / void / self-closing start tag.
 *
 * @details Void tags (br/hr/img) emit a single token; self-closing block
 * tags emit an empty block-start/block-end pair; ordinary tags push the
 * style stack (and a block-start for block tags).
 *
 * @param[in,out] ctx Tokenizer context.
 * @param[in]     buf Source buffer.
 * @param[in,out] pi  Cursor at '<'; advanced past '>'.
 * @param[in]     len Total buffer length.
 * @return k_ra_ok, k_ra_err_no_mem, or validation error.
 * @retval k_ra_err_no_mem Token pool or nesting-depth bound exceeded.
 * @pre `ctx`, `buf`, `pi` are non-null.
 * @pre `buf[*pi] == '<'` and not an end / comment / decl.
 * @post `*pi` advances past the tag.
 * @post The element stack / token pool grow as the tag requires.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_handle_start(tok_ctx_t* ctx, const uint8_t* buf, size_t* pi, size_t len)
{
  const size_t               tag_lt    = *pi; /* index of '<' before parse */
  bool                       selfclose = false;
  const ra_reflow_html_tag_t tag       = priv_parse_start(buf, pi, len, &selfclose);
  ctx->saw_element                     = true;

  ra_err_t verr = k_ra_ok;
  if (priv_handle_void(ctx, buf, tag_lt, *pi, tag, &verr)) {
    return verr;
  }

  const bool block = priv_is_block(tag);
  if (selfclose) {
    if (block) {
      if (!priv_emit(ctx->engine, k_ra_reflow_tok_block_start, tag, ctx->style, 0U, 0U)) {
        return k_ra_err_no_mem;
      }
      if (!priv_emit(ctx->engine, k_ra_reflow_tok_block_end, tag, ctx->style, 0U, 0U)) {
        return k_ra_err_no_mem;
      }
    }
    return k_ra_ok;
  }

  if (ctx->sp >= (uint32_t)k_priv_max_depth) {
    return k_ra_err_no_mem;
  }
  ctx->stack_tag[ctx->sp]   = (uint8_t)tag;
  ctx->stack_style[ctx->sp] = ctx->style;
  ctx->stack_link[ctx->sp]  = ctx->active_link;
  ctx->sp++;
  const size_t   span = (*pi > tag_lt) ? (*pi - tag_lt) : 0U;
  const ra_err_t aerr = priv_open_attrs(ctx, &buf[tag_lt], span, tag, block);
  if (aerr != k_ra_ok) {
    return aerr;
  }
  ctx->style = (uint8_t)(ctx->style | priv_style_for(tag));
  return k_ra_ok;
}

/**
 * @brief Dispatch the markup construct beginning at `buf[*pi] == '<'`.
 *
 * @details Routes to the comment / CDATA / processing-instruction /
 * declaration skip, or to the end-tag / start-tag handlers.
 *
 * @param[in,out] ctx Tokenizer context.
 * @param[in]     buf Source buffer.
 * @param[in,out] pi  Cursor at '<'; advanced past the construct.
 * @param[in]     len Total buffer length.
 * @return Result of the selected handler (k_ra_ok on a skip).
 * @retval k_ra_ok Comment / PI / declaration consumed.
 * @pre `ctx`, `buf`, `pi` are non-null.
 * @pre `buf[*pi] == '<'`.
 * @post `*pi` advances past the construct.
 * @post Token / text pools may grow per the selected handler.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_handle_lt(tok_ctx_t* ctx, const uint8_t* buf, size_t* pi, size_t len)
{
  if (priv_starts_with(buf, *pi, len, "<!--")) {
    *pi = priv_skip_past(buf, *pi + 4U, len, "-->");
    return k_ra_ok;
  }
  if (priv_starts_with(buf, *pi, len, "<![CDATA[")) {
    return priv_handle_cdata(ctx, buf, pi, len);
  }
  if (priv_starts_with(buf, *pi, len, "<?")) {
    *pi = priv_skip_past(buf, *pi + 2U, len, "?>");
    return k_ra_ok;
  }
  if (priv_starts_with(buf, *pi, len, "<!")) {
    *pi = priv_skip_past(buf, *pi + 2U, len, ">");
    return k_ra_ok;
  }
  if (((*pi + 1U) < len) && (buf[*pi + 1U] == '/')) {
    return priv_handle_end(ctx, buf, pi, len);
  }
  return priv_handle_start(ctx, buf, pi, len);
}

/**
 * @brief Stash + emit a character-data run, tagging it with the active link id.
 *
 * @details Text outside any open element is dropped. Otherwise the run is
 * entity-decoded + whitespace-collapsed into the text pool and emitted as a text
 * token whose `reserved` byte carries the active `<a>` link id (0 = none).
 *
 * @param[in,out] ctx Tokenizer context.
 * @param[in]     buf Source buffer.
 * @param[in]     run Run start offset (inclusive).
 * @param[in]     end Run end offset (exclusive).
 * @return true on success, false on a pool overflow.
 * @retval false Text or token pool full.
 * @pre `ctx` and `buf` are non-null; `run <= end`.
 * @pre `ctx->engine->token_count` is consistent.
 * @post On a non-empty stored run a text token is appended + link-tagged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool priv_emit_text_run(tok_ctx_t* ctx, const uint8_t* buf, size_t run, size_t end)
{
  if (ctx->sp == 0U) {
    return true;
  }
  uint32_t off = 0U;
  uint32_t tln = 0U;
  if (!priv_stash_run(ctx->engine, buf, run, end, &off, &tln)) {
    return false;
  }
  if (tln == 0U) {
    return true;
  }
  if (!priv_emit(ctx->engine,
                 k_ra_reflow_tok_text,
                 k_ra_reflow_tag_unknown,
                 ctx->style,
                 off,
                 tln)) {
    return false;
  }
  ctx->engine->tokens[ctx->engine->token_count - 1U].reserved = ctx->active_link;
  return true;
}

/* ===========================================================================
 * Entry point (called by ra_reflow_parse.c)
 * ===========================================================================
 */

ra_err_t priv_reflow_xml_walk(ra_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len);

ra_err_t priv_reflow_xml_walk(ra_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len)
{
  if ((engine == nullptr) || (xhtml_buf == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (xhtml_len == 0U) {
    return k_ra_err_invalid_size;
  }

  tok_ctx_t ctx = {};
  ctx.engine    = engine;
  ctx.style     = (uint8_t)k_ra_reflow_style_normal;

  size_t i = 0U;
  while (i < xhtml_len) {
    if (xhtml_buf[i] == '<') {
      const ra_err_t err = priv_handle_lt(&ctx, xhtml_buf, &i, xhtml_len);
      if (err != k_ra_ok) {
        return err;
      }
      continue;
    }
    const size_t run = i;
    while ((i < xhtml_len) && (xhtml_buf[i] != '<')) {
      ++i;
    }
    if (!priv_emit_text_run(&ctx, xhtml_buf, run, i)) {
      return k_ra_err_no_mem;
    }
  }

  if (!ctx.saw_element) {
    return k_ra_err_validation_failed;
  }
  if (ctx.sp != 0U) {
    return k_ra_err_validation_failed;
  }
  return k_ra_ok;
}
