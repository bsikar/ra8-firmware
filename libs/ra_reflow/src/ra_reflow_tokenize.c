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
  k_priv_style_mask    = ((uint32_t)k_ra_reflow_style_bold | (uint32_t)k_ra_reflow_style_italic |
                          (uint32_t)k_ra_reflow_style_underline), /**< Run-style bits. */
} priv_tok_consts_t;

/**
 * @struct tok_ctx_t
 * @brief Mutable tokenizer state threaded through the scan helpers.
 */
typedef struct {
  ra_reflow_t*     engine;      /**< Target engine pools.        */
  uint8_t          style;       /**< Current inline-style bits.  */
  uint8_t          active_link; /**< 1-based link id (0 = none).  */
  uint32_t         color;       /**< Current CSS colour / inherit.*/
  uint16_t         css_font_px; /**< Inherited CSS font px (0=none).*/
  uint16_t         family_off;  /**< Inherited font-family slice off.*/
  uint16_t         family_len;  /**< Inherited font-family len (0=none).*/
  uint8_t          face_slot;   /**< Resolved embedded face (0=default).*/
  ra_css_element_t stack_el
    [k_priv_max_depth]; /**< Open-element identities (tag+id+class) for the cascade + descendant match. */
  uint8_t  stack_style[k_priv_max_depth];   /**< Style to restore on pop.    */
  uint8_t  stack_link[k_priv_max_depth];    /**< Link id to restore on pop.  */
  uint32_t stack_color[k_priv_max_depth];   /**< Colour to restore on pop.   */
  uint16_t stack_font[k_priv_max_depth];    /**< CSS font px to restore on pop.*/
  uint16_t stack_fam_off[k_priv_max_depth]; /**< font-family off to restore.  */
  uint16_t stack_fam_len[k_priv_max_depth]; /**< font-family len to restore.  */
  uint8_t  stack_face[k_priv_max_depth];    /**< Face slot to restore on pop. */
  uint32_t sp;                              /**< Stack depth.                */
  uint32_t suppress_sp;                     /**< display:none subtree depth (0=off).*/
  bool     saw_element;                     /**< At least one element seen.  */
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
    {"link", k_ra_reflow_tag_link},
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
 * @brief Test whether the tokenizer is inside a `display:none` subtree.
 *
 * @details Returns true when `ctx->suppress_sp` is nonzero, meaning the scan
 * entered an element whose cascaded style declared `display:none` (#140). All
 * token and text-pool emits are suppressed until the matching close tag pops
 * the stack back above the recorded depth.
 *
 * @param[in] ctx Tokenizer context to query.
 * @return true while inside a `display:none` subtree; false otherwise.
 * @retval true  Emission is suppressed for the current position.
 * @retval false Emission proceeds normally.
 * @pre `ctx` is non-null.
 * @pre `ctx->suppress_sp` is a valid depth value (0 or a previous `sp`).
 * @post No state is modified (pure read of `ctx`).
 * @post `ctx->suppress_sp` is unchanged.
 * @note Pure function.
 * @since 0.1.0
 */
static bool priv_suppressed(const tok_ctx_t* ctx)
{
  return ctx->suppress_sp != 0U;
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
  tok->color             = (uint32_t)k_ra_reflow_color_inherit;
  tok->css_font_px       = 0U;
  tok->reserved16        = 0U;
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

/**
 * @brief Return the index of the first occurrence of `lit` (its start), or len.
 *
 * @details Like ::priv_skip_past but returns the literal's START offset, so the
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
static size_t priv_find_lit(const uint8_t* buf, size_t i, size_t len, const char* lit)
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
  if ((ctx->sp > 0U) && !priv_suppressed(ctx)) {
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
    if (tlen > 0U) {
      ctx->engine->tokens[ctx->engine->token_count - 1U].reserved16 = (uint16_t)ctx->face_slot;
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
  const ra_reflow_html_tag_t tag = (ra_reflow_html_tag_t)ctx->stack_el[ctx->sp].tag;
  ctx->style                     = ctx->stack_style[ctx->sp];
  ctx->active_link               = ctx->stack_link[ctx->sp];
  ctx->color                     = ctx->stack_color[ctx->sp];
  ctx->css_font_px               = ctx->stack_font[ctx->sp];
  ctx->family_off                = ctx->stack_fam_off[ctx->sp];
  ctx->family_len                = ctx->stack_fam_len[ctx->sp];
  ctx->face_slot                 = ctx->stack_face[ctx->sp];
  if (priv_is_block(tag) && !priv_suppressed(ctx) &&
      !priv_emit(ctx->engine, k_ra_reflow_tok_block_end, tag, ctx->style, 0U, 0U)) {
    return k_ra_err_no_mem;
  }
  /* Exited the display:none subtree once we pop back above its depth. */
  if ((ctx->suppress_sp != 0U) && (ctx->sp < ctx->suppress_sp)) {
    ctx->suppress_sp = 0U;
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
 * @pre `tag_len >= name_len` (zero-length searches return false immediately).
 * @post No state mutated.
 * @post On false, `*out_voff` and `*out_vlen` are unspecified.
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
 * @pre `engine->text_pool_used <= k_ra_reflow_text_pool_bytes`.
 * @post `*out_len > 0` iff a quoted @p name value was stored in the pool.
 * @post The pool grows by `*out_len` bytes when a value is stored.
 * @note Not thread-safe.
 * @since 0.1.0
 */
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
 * @return The element identity for ra_css_cascade().
 * @retval ra_css_element_t Fully populated struct; absent attributes have NULL pointer and zero length.
 * @pre `tag` is non-null and holds @p span bytes.
 * @pre `kind` is a valid `ra_reflow_html_tag_t` value.
 * @post No state mutated; returned pointers alias @p tag.
 * @post `el.tag == (uint8_t)kind`.
 * @note Pure read of @p tag.
 * @since 0.1.0
 */
static ra_css_element_t priv_css_element(ra_reflow_html_tag_t kind, const uint8_t* tag, size_t span)
{
  ra_css_element_t el = {};
  el.tag              = (uint8_t)kind;
  size_t off          = 0U;
  size_t len          = 0U;
  if (priv_find_attr(tag, span, "id", sizeof("id") - 1U, &off, &len)) {
    el.id     = (const char*)&tag[off];
    el.id_len = (uint16_t)len;
  }
  if (priv_find_attr(tag, span, "class", sizeof("class") - 1U, &off, &len)) {
    el.class_str = (const char*)&tag[off];
    el.class_len = (uint16_t)len;
  }
  return el;
}

/**
 * @brief Parse a just-opened tag's inline `style="..."` into a CSS declaration.
 *
 * @details Locates the `style` attribute within the raw tag span and delegates
 * to `ra_css_parse_inline()` to parse its value into a declaration struct.
 * When the attribute is absent or the parser fails the returned struct has
 * `set == 0`, indicating no inline declarations override the cascade.
 *
 * @param[in] tag  Raw tag span starting at '<'.
 * @param[in] span Length of @p tag, bytes.
 * @return The inline CSS declaration for this element.
 * @retval ra_css_style_t Struct with `set == 0` if no `style` attribute is present.
 * @pre `tag` is non-null and holds @p span bytes.
 * @pre `span > 0` (a zero-span tag cannot carry attributes).
 * @post No state mutated.
 * @post Returned struct `set` field is 0 when no `style` attribute was found.
 * @note Pure read of @p tag.
 * @since 0.1.0
 */
static ra_css_style_t priv_css_inline(const uint8_t* tag, size_t span)
{
  ra_css_style_t inl = {};
  size_t         off = 0U;
  size_t         len = 0U;
  if (priv_find_attr(tag, span, "style", sizeof("style") - 1U, &off, &len)) {
    (void)ra_css_parse_inline((const char*)&tag[off], (uint32_t)len, &inl);
  }
  return inl;
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
 * @retval 1..k_ra_reflow_max_links The newly assigned 1-based link id.
 * @pre `engine` is non-null.
 * @pre `engine->link_target_count <= k_ra_reflow_max_links`.
 * @post On a non-zero return the table grew by one entry.
 * @post `engine->link_target_count` is unchanged on a zero return.
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
 * @param[in]     comp Cascaded style (supplies the block alignment).
 * @param[in]     css_font_px Block CSS font px to stamp (0 = UA default).
 * @return k_ra_ok, or k_ra_err_no_mem on a token-pool overflow.
 * @retval k_ra_err_no_mem Block-start token pool full.
 * @pre `ctx` and `tag` are non-null.
 * @pre `ctx->sp > 0` (the tag was already pushed).
 * @post For a block tag a block-start token is emitted (id slice or 0,0).
 * @post For `<a>` `ctx->active_link` reflects the interned href (0 if none).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_open_attrs(tok_ctx_t*            ctx,
                                const uint8_t*        tag,
                                size_t                span,
                                ra_reflow_html_tag_t  kind,
                                bool                  block,
                                const ra_css_style_t* comp,
                                uint16_t              css_font_px)
{
  if (priv_suppressed(ctx)) {
    return k_ra_ok; /* inside a display:none subtree -> emit nothing */
  }
  if (block) {
    uint32_t id_off = 0U;
    uint32_t id_len = 0U;
    priv_capture_attr(ctx->engine, tag, span, "id", sizeof("id") - 1U, &id_off, &id_len);
    if (!priv_emit(ctx->engine, k_ra_reflow_tok_block_start, kind, ctx->style, id_off, id_len)) {
      return k_ra_err_no_mem;
    }
    /* Stash the block's cascaded alignment + CSS font px on the block-start token
     * (left + 0 = unstyled defaults, so unstyled blocks lay out byte-identically). */
    const uint8_t      align = ((comp->set & (uint8_t)k_ra_css_set_align) != 0U)
                                 ? comp->align
                                 : (uint8_t)k_ra_reflow_align_left;
    ra_reflow_token_t* bst   = &ctx->engine->tokens[ctx->engine->token_count - 1U];
    bst->reserved            = align;
    bst->css_font_px         = css_font_px;
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
static bool priv_rel_is_stylesheet(const uint8_t* rel, size_t len)
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
 * @brief Resolve a `<link rel="stylesheet" href>` via the bound CSS loader.
 *
 * @details Best-effort. When a CSS loader is bound and the tag is a stylesheet
 * link carrying an href, fetches the bytes and parses them into the chapter
 * sheet at this document position (so a later inline `<style>` / `style=`
 * overrides them). Any missing attribute / loader failure is silently ignored
 * -- the chapter renders with whatever rules are present.
 *
 * @param[in,out] ctx    Tokenizer context (carries the engine + its CSS sheet).
 * @param[in]     buf    Source buffer.
 * @param[in]     tag_lt Index of the tag's '<'.
 * @param[in]     end    One past the tag's '>'.
 * @return Nothing.
 * @pre `ctx`, `buf` non-null; `end >= tag_lt`.
 * @pre `ctx->engine` is non-null and its CSS sheet is initialised.
 * @post On a resolved stylesheet, its rules are appended to `ctx->engine->css`.
 * @post When no loader is bound or attributes are absent, the sheet is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_handle_link(tok_ctx_t* ctx, const uint8_t* buf, size_t tag_lt, size_t end)
{
  if (ctx->engine->css_loader == nullptr) {
    return;
  }
  const uint8_t* tagbuf   = &buf[tag_lt];
  const size_t   span     = (end > tag_lt) ? (end - tag_lt) : 0U;
  size_t         rel_off  = 0U;
  size_t         rel_len  = 0U;
  size_t         href_off = 0U;
  size_t         href_len = 0U;
  if (!priv_find_attr(tagbuf, span, "rel", sizeof("rel") - 1U, &rel_off, &rel_len) ||
      !priv_find_attr(tagbuf, span, "href", sizeof("href") - 1U, &href_off, &href_len) ||
      !priv_rel_is_stylesheet(&tagbuf[rel_off], rel_len)) {
    return;
  }
  const uint8_t* css_bytes = nullptr;
  size_t         css_len   = 0U;
  if ((ctx->engine->css_loader(ctx->engine->css_loader_ctx,
                               (const char*)&tagbuf[href_off],
                               (uint32_t)href_len,
                               &css_bytes,
                               &css_len) == k_ra_ok) &&
      (css_bytes != nullptr) && (css_len > 0U)) {
    (void)ra_css_parse(&ctx->engine->css, (const char*)css_bytes, (uint32_t)css_len);
  }
}

/**
 * @brief Emit the single token for a void tag (`<br>` / `<hr>` / `<img>`),
 *        or resolve a `<link>` stylesheet.
 *
 * @details `<br>` -> break, `<hr>` -> rule, `<img>` -> image (capturing its
 * `src`), `<link rel=stylesheet>` -> external CSS load (no token). Returns
 * whether @p tag was a void tag so the caller can fall through to ordinary
 * open-tag handling otherwise.
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
 * @post On false neither the token pool nor `*out_err` are modified.
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
  if (tag == k_ra_reflow_tag_link) {
    priv_handle_link(ctx, buf, tag_lt, end); /* external stylesheet (no token) */
    *out_err = k_ra_ok;
    return true;
  }
  const bool is_void =
    (tag == k_ra_reflow_tag_br) || (tag == k_ra_reflow_tag_hr) || (tag == k_ra_reflow_tag_img);
  if (is_void && priv_suppressed(ctx)) {
    *out_err = k_ra_ok; /* void tag inside a display:none subtree -> drop */
    return true;
  }
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
/**
 * @brief Resolve an element's effective CSS font px from the cascade + inherited.
 *
 * @details Returns the inherited size (0 = none -> UA default) when no font-size
 * is declared, else resolves the declared value: an absolute `px`, or a `%`/`em`
 * of the parent size (the inherited px, or the body size when no ancestor set
 * one). The result clamps to [k_ra_reflow_min_font_px, k_ra_reflow_max_font_px].
 *
 * @param[in] ctx  Tokenizer context (carries the inherited font px + body size).
 * @param[in] comp The element's cascaded style.
 * @return The element's effective CSS font px, or 0 for "use the UA default".
 * @retval 0 No font-size declared for this element and no ancestor has set one.
 * @pre `ctx` and `comp` are non-null.
 * @pre `ctx->engine->font_px` is the body font size.
 * @post No state mutated.
 * @post Return value is clamped to [k_ra_reflow_min_font_px, k_ra_reflow_max_font_px] when non-zero.
 * @note Pure aside from reading @p ctx.
 * @since 0.1.0
 */
static uint16_t priv_css_font_px(const tok_ctx_t* ctx, const ra_css_style_t* comp)
{
  if ((comp->set & (uint8_t)k_ra_css_set_fontsize) == 0U) {
    return ctx->css_font_px; /* inherit (0 = none -> UA default) */
  }
  const uint16_t parent = (ctx->css_font_px != 0U) ? ctx->css_font_px : ctx->engine->font_px;
  uint32_t px = ((ra_css_font_unit_t)comp->font_unit == k_ra_css_font_pct)
                  ? (((uint32_t)parent * (uint32_t)comp->font_val) / (uint32_t)k_ra_reflow_pct_full)
                  : (uint32_t)comp->font_val;
  if (px < (uint32_t)k_ra_reflow_min_font_px) {
    px = (uint32_t)k_ra_reflow_min_font_px;
  }
  if (px > (uint32_t)k_ra_reflow_max_font_px) {
    px = (uint32_t)k_ra_reflow_max_font_px;
  }
  return (uint16_t)px;
}

/**
 * @brief Run the content-CSS cascade for a just-pushed element and apply it.
 *
 * @details Builds the element's CSS identity + inherited run style + colour,
 * cascades author `<style>` rules + inline style over it (#111 / #140), emits
 * the block-start (via priv_open_attrs) carrying the cascaded alignment + font
 * size, and updates the context run style + colour + font px for descendant
 * content. Unstyled content lays out byte-identically to the pre-CSS engine.
 * Factored out of priv_handle_start to keep each function within the NASA Rule 4
 * size budget.
 *
 * @param[in,out] ctx    Tokenizer context (element already pushed).
 * @param[in]     tagbuf Raw tag span starting at '<'.
 * @param[in]     span   Length of @p tagbuf, bytes.
 * @param[in]     tag    Classified tag.
 * @param[in]     block  True iff @p tag is a block element.
 * @return k_ra_ok, or k_ra_err_no_mem on a block-start token-pool overflow.
 * @retval k_ra_err_no_mem Block-start token pool full.
 * @pre `ctx`, `tagbuf` are non-null; `ctx->sp > 0` (element already pushed).
 * @pre `span` spans the element's `<...>` start tag.
 * @post `ctx->style` / `ctx->color` reflect the element's computed style.
 * @post A block element emitted its block-start with the cascaded alignment.
 * @note Not thread-safe.
 * @since 0.1.0
 */
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
 * @pre `engine->face_count <= k_ra_reflow_max_faces`.
 * @post No state is modified.
 * @post Return value is in [0, engine->face_count].
 * @note Pure; not thread-safe only insofar as it reads engine state.
 * @since 0.1.0
 */
static uint8_t priv_resolve_face_slot(const ra_reflow_t* engine, const ra_css_style_t* comp)
{
  if ((engine->face_count == 0U) || ((comp->set & (uint8_t)k_ra_css_set_family) == 0U) ||
      (comp->family_len == 0U)) {
    return 0U;
  }
  const char*   family = (const char*)&engine->css.names[comp->family_off];
  const bool    bold   = (comp->style & (uint8_t)k_ra_reflow_style_bold) != 0U;
  const bool    italic = (comp->style & (uint8_t)k_ra_reflow_style_italic) != 0U;
  const int16_t ci     = ra_css_match_face(&engine->css, family, comp->family_len, bold, italic);
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

/**
 * @brief Run the CSS cascade for a just-pushed element and apply the result.
 *
 * @details Builds the element's CSS identity and inherited run style, runs the
 * author `<style>` rules + inline `style` attribute through `ra_css_cascade_ctx()`
 * (#111 / #140), emits the block-start via `priv_open_attrs()` carrying the
 * cascaded alignment + font size, and updates `ctx->style`, `ctx->color`,
 * `ctx->css_font_px`, `ctx->family_off`/`len`, and `ctx->face_slot` for
 * descendant content. Unstyled content lays out byte-identically to the
 * pre-CSS engine. Factored out of `priv_handle_start()` to stay within the
 * NASA Rule 4 function-length budget.
 *
 * @param[in,out] ctx    Tokenizer context (element already pushed onto stack).
 * @param[in]     tagbuf Raw tag span starting at '<'.
 * @param[in]     span   Length of @p tagbuf, bytes.
 * @param[in]     tag    Classified tag.
 * @param[in]     block  True iff @p tag is a block element.
 * @return k_ra_ok, or k_ra_err_no_mem on a block-start token-pool overflow.
 * @retval k_ra_ok        Cascade applied; context updated.
 * @retval k_ra_err_no_mem Token pool full when emitting the block-start token.
 * @pre `ctx` and `tagbuf` are non-null.
 * @pre `ctx->sp > 0` (the element has already been pushed by the caller).
 * @post `ctx->style` / `ctx->color` / `ctx->css_font_px` reflect the cascaded style.
 * @post A block element emitted its block-start token with cascaded alignment.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_open_styled(tok_ctx_t*           ctx,
                                 const uint8_t*       tagbuf,
                                 size_t               span,
                                 ra_reflow_html_tag_t tag,
                                 bool                 block)
{
  const uint8_t          base = (uint8_t)(ctx->style | priv_style_for(tag));
  const ra_css_element_t el   = ctx->stack_el[ctx->sp - 1U]; /* pushed by the caller */
  ra_css_style_t         inh  = {.set = (uint8_t)k_priv_style_mask, .style = base};
  if (ctx->color != (uint32_t)k_ra_reflow_color_inherit) {
    inh.set   = (uint8_t)(inh.set | (uint8_t)k_ra_css_set_color);
    inh.color = ctx->color;
  }
  /* font-family inherits (#109): seed the cascade with the parent's resolved
   * family so a child without its own `font-family` keeps the ancestor's face. */
  if (ctx->family_len != 0U) {
    inh.set        = (uint8_t)(inh.set | (uint8_t)k_ra_css_set_family);
    inh.family_off = ctx->family_off;
    inh.family_len = ctx->family_len;
  }
  const ra_css_style_t inl = priv_css_inline(tagbuf, span);
  /* Ancestors = the open-element stack below this one (stack_el[0 .. sp-2]). */
  const uint8_t        n_anc = (uint8_t)(ctx->sp - 1U);
  const ra_css_style_t comp =
    ra_css_cascade_ctx(&ctx->engine->css, &el, inh, inl, ctx->stack_el, n_anc);
  const uint16_t fpx = priv_css_font_px(ctx, &comp);
  /* display:none (#140): begin suppressing this element + its subtree at the
   * current depth; priv_open_attrs and the emit sites then drop every token
   * until the matching close pops back above this depth. */
  const bool hidden = ((comp.set & (uint8_t)k_ra_css_set_display) != 0U) && (comp.display != 0U);
  if (hidden && (ctx->suppress_sp == 0U)) {
    ctx->suppress_sp = ctx->sp;
  }
  const ra_err_t aerr = priv_open_attrs(ctx, tagbuf, span, tag, block, &comp, fpx);
  if (aerr != k_ra_ok) {
    return aerr;
  }
  ctx->style       = (uint8_t)(comp.style & (uint8_t)k_priv_style_mask);
  ctx->color       = ((comp.set & (uint8_t)k_ra_css_set_color) != 0U)
                       ? comp.color
                       : (uint32_t)k_ra_reflow_color_inherit;
  ctx->css_font_px = fpx;
  /* Resolve the embedded face for this element from its cascaded family +
   * emphasis (#109). Children inherit the family slice; text runs stamp the
   * resolved slot. Both are 0 / unchanged for content without `@font-face`. */
  if ((comp.set & (uint8_t)k_ra_css_set_family) != 0U) {
    ctx->family_off = comp.family_off;
    ctx->family_len = comp.family_len;
  }
  ctx->face_slot = priv_resolve_face_slot(ctx->engine, &comp);
  return k_ra_ok;
}

/**
 * @brief Handle an opening, void, or self-closing start tag.
 *
 * @details Parses the tag name and self-close flag, then dispatches: void tags
 * (br / hr / img) emit a single token and return; self-closing block tags emit
 * an empty block-start / block-end pair; ordinary tags push the element onto
 * the style stack (after bounds-checking depth) and call `priv_open_styled()`
 * to cascade + apply CSS and emit any block-start token.
 *
 * @param[in,out] ctx Tokenizer context.
 * @param[in]     buf Source buffer.
 * @param[in,out] pi  Cursor at '<'; advanced past '>'.
 * @param[in]     len Total buffer length.
 * @return k_ra_ok, k_ra_err_no_mem, or k_ra_err_validation_failed.
 * @retval k_ra_ok        Tag processed successfully.
 * @retval k_ra_err_no_mem Token pool or nesting-depth bound exceeded.
 * @pre `ctx`, `buf`, `pi` are non-null.
 * @pre `buf[*pi] == '<'` and the tag is not an end-tag, comment, or declaration.
 * @post `*pi` advances past the tag (or to `len` on truncation).
 * @post The element stack and token pool grow as required by the tag kind.
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
    if (block && !priv_suppressed(ctx)) {
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
  const size_t span = (*pi > tag_lt) ? (*pi - tag_lt) : 0U;
  /* Record the element's full identity (tag + id + class, aliasing the chapter
   * buffer) so the cascade reads it back and descendant selectors can match it
   * as an ancestor of a later element. */
  ctx->stack_el[ctx->sp]      = priv_css_element(tag, &buf[tag_lt], span);
  ctx->stack_style[ctx->sp]   = ctx->style;
  ctx->stack_link[ctx->sp]    = ctx->active_link;
  ctx->stack_color[ctx->sp]   = ctx->color;
  ctx->stack_font[ctx->sp]    = ctx->css_font_px;
  ctx->stack_fam_off[ctx->sp] = ctx->family_off;
  ctx->stack_fam_len[ctx->sp] = ctx->family_len;
  ctx->stack_face[ctx->sp]    = ctx->face_slot;
  ctx->sp++;
  return priv_open_styled(ctx, &buf[tag_lt], span, tag, block);
}

/**
 * @brief True iff `<name` starts at `buf[i]` and is followed by a tag delimiter.
 *
 * @details Distinguishes `<style ...>` / `<script>` from look-alikes such as a
 * hypothetical `<styled>` by requiring `>`, `/`, whitespace, or end-of-buffer
 * after the name.
 *
 * @param[in] buf  Source buffer.
 * @param[in] i    Offset of the leading '<'.
 * @param[in] len  Total buffer length.
 * @param[in] name Literal element open including '<' (e.g. "<style").
 * @return true iff the element name matches with a following delimiter.
 * @retval false No match, or a longer name continues past @p name.
 * @pre `buf` and `name` are non-null.
 * @pre `i <= len` (caller guarantees the cursor is within bounds).
 * @post No state is modified (pure).
 * @post Return value depends solely on the inputs.
 * @note Pure function.
 * @since 0.1.0
 */
static bool priv_tag_is(const uint8_t* buf, size_t i, size_t len, const char* name)
{
  if (!priv_starts_with(buf, i, len, name)) {
    return false;
  }
  const size_t n = strlen(name);
  if ((i + n) >= len) {
    return true;
  }
  const char c = (char)buf[i + n];
  return (c == '>') || (c == '/') || ra_reflow_tok_is_xml_whitespace(c);
}

/**
 * @brief Consume a raw-text element (`<style>` / `<script>`) without emitting.
 *
 * @details Per HTML raw-text rules the content runs verbatim (no nested
 * elements) to the matching lowercase close tag. `<style>` content is parsed
 * into the engine stylesheet (#111); `<script>` content is discarded. A missing
 * close tag consumes to end-of-buffer. Lowercase close literals match valid
 * XHTML (XML is case-sensitive and requires lowercase element names).
 *
 * @param[in,out] ctx       Tokenizer context (engine stylesheet).
 * @param[in]     buf       Source buffer.
 * @param[in,out] pi        Cursor at '<'; advanced past the close tag.
 * @param[in]     len       Total buffer length.
 * @param[in]     close_lit Lowercase close tag literal (e.g. "</style>").
 * @param[in]     is_style  True to parse the content as CSS, false to discard.
 * @pre `ctx`, `buf`, `pi`, `close_lit` are non-null.
 * @pre `buf[*pi] == '<'`.
 * @post `*pi` advances past the element (or to `len`).
 * @post For `<style>` any parsed rules are appended to `ctx->engine->css`.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_handle_raw_text(tok_ctx_t*     ctx,
                                 const uint8_t* buf,
                                 size_t*        pi,
                                 size_t         len,
                                 const char*    close_lit,
                                 bool           is_style)
{
  ctx->saw_element      = true;
  const size_t open_end = priv_skip_past(buf, *pi, len, ">");
  const size_t close_at = priv_find_lit(buf, open_end, len, close_lit);
  if (is_style && (close_at > open_end)) {
    (void)ra_css_parse(&ctx->engine->css,
                       (const char*)&buf[open_end],
                       (uint32_t)(close_at - open_end));
  }
  *pi = (close_at < len) ? (close_at + strlen(close_lit)) : len;
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
  if (priv_tag_is(buf, *pi, len, "<style")) {
    priv_handle_raw_text(ctx, buf, pi, len, "</style>", true);
    return k_ra_ok;
  }
  if (priv_tag_is(buf, *pi, len, "<script")) {
    priv_handle_raw_text(ctx, buf, pi, len, "</script>", false);
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
 * @post On `ctx->sp == 0` or inside `display:none` the pools are unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool priv_emit_text_run(tok_ctx_t* ctx, const uint8_t* buf, size_t run, size_t end)
{
  if ((ctx->sp == 0U) || priv_suppressed(ctx)) {
    return true; /* outside any element, or inside display:none -> drop */
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
  ctx->engine->tokens[ctx->engine->token_count - 1U].reserved   = ctx->active_link;
  ctx->engine->tokens[ctx->engine->token_count - 1U].color      = ctx->color;
  ctx->engine->tokens[ctx->engine->token_count - 1U].reserved16 = (uint16_t)ctx->face_slot;
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

  (void)ra_css_sheet_reset(&engine->css); /* fresh CSS rules per chapter (#111) */

  tok_ctx_t ctx = {};
  ctx.engine    = engine;
  ctx.style     = (uint8_t)k_ra_reflow_style_normal;
  ctx.color     = (uint32_t)k_ra_reflow_color_inherit;

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
