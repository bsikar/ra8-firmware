/**
 * @file ra8_reflow_css_rules.c
 * @brief Content-CSS selector + `@font-face` rule parsing (#111).
 *
 * @details
 * The selector grammar (type / class / id / descendant compounds) and the
 * `@font-face` descriptor parser for the v1 CSS subset documented in
 * `ra8_reflow_css.h`. Parsed selectors and faces are interned into the
 * caller-owned ::ra8_css_sheet_t name pool and rule / face tables. The top-level
 * block dispatcher ::ra8_reflow_css_parse_one_block lives here because it drives
 * the selector and `@font-face` parsers; it is invoked by the stylesheet scanner
 * ::ra8_css_parse in a sibling translation unit. No MMIO, no heap.
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
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_reflow.h" /* ra8_reflow_html_tag_t */
#include "ra8_reflow_css.h"
#include "ra8_reflow_css_internal.h"
#include "ra8_reflow_tokenize_internal.h" /* ra8_reflow_tok_classify */

/* ===========================================================================
 * Internal constants (no magic numbers)
 * ===========================================================================
 */

/**
 * @enum priv_css_face_t
 * @brief `@font-face` parsing constants.
 */
typedef enum : uint8_t {
  k_priv_url_len = 4U, /**< Length of the `url(` token prefix. */
} priv_css_face_t;

/* ===========================================================================
 * Selector parsing
 * ===========================================================================
 */

/**
 * @brief True if @p c can appear inside a bare type / class / id name.
 *
 * @details Accepts ASCII letters ('a'-'z', 'A'-'Z'), decimal digits ('0'-'9'),
 * the hyphen ('-'), and the underscore ('_'), which are the characters allowed
 * in CSS identifiers per the CSS specification. Used by priv_parse_sel_type()
 * and priv_parse_sel_part() to delimit token boundaries in selector text.
 *
 * @param[in] c Byte to classify.
 *
 * @return bool Classification result.
 * @retval true  @p c is a valid CSS identifier character.
 * @retval false @p c is not a valid CSS identifier character.
 *
 * @pre No precondition on @p c; all byte values are valid inputs.
 * @pre Caller uses the return value to decide whether to advance a cursor.
 * @post No state is mutated; the function is pure.
 * @post @p c is not modified.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_is_name_char(char c)
{
  return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) ||
         (c == '-') || (c == '_');
}

/**
 * @brief Intern a class/id name into the sheet pool; false if it does not fit.
 *
 * @details Copies @p len bytes of @p s into @p sheet->names at offset
 * @p sheet->names_used, then advances names_used by @p len. On success, @p *off
 * receives the offset at which the name was stored so that a rule or face can
 * reference it by (off, len) rather than by pointer. Returns false without
 * mutation when @p len is 0, exceeds k_ra8_css_name_max, or would cause names_used
 * to overflow k_ra8_css_name_pool.
 *
 * @param[in,out] sheet Sheet whose name pool receives the interned bytes.
 * @param[in]     s     Byte span to intern (need not be NUL-terminated).
 * @param[in]     len   Number of bytes to copy.
 * @param[out]    off   Receives the pool offset of the interned name on success.
 *
 * @return bool Whether the name was successfully interned.
 * @retval true  @p *off holds the start offset; @p sheet->names_used increased by @p len.
 * @retval false @p len is 0 or too large, or the pool has insufficient space.
 *
 * @pre @p sheet and @p off are non-NULL.
 * @pre @p s points to at least @p len readable bytes.
 * @post On true, @p sheet->names[@p *off .. @p *off + @p len) contains the copied name.
 * @post On false, @p sheet is not modified.
 *
 * @note Thread-safe with respect to distinct @p sheet instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_intern_name(ra8_css_sheet_t* sheet, const char* s, size_t len, uint16_t* off)
{
  /* mcdc-deactivated: every caller guards the length before interning -- (nlen == 0U) reject at the selector site, (n > 0U) at the @font-face/font-family sites, and priv_extract_url rejects an empty url() -- so (len == 0U) is unreachable; only (len > k_ra8_css_name_max) varies. */
  if ((len == 0U) || (len > (size_t)k_ra8_css_name_max)) {
    return false;
  }
  if (((size_t)sheet->names_used + len) > (size_t)k_ra8_css_name_pool) {
    return false;
  }
  *off = sheet->names_used;
  (void)memcpy(&sheet->names[sheet->names_used], s, len);
  sheet->names_used = (uint16_t)(sheet->names_used + len);
  return true;
}

/**
 * @brief Parse the optional leading type / `*` of a compound selector.
 *
 * @details Reads the element type or universal selector at @p s[*i]:
 *   - If the current byte is '*', advances @p *i by 1 and returns true without
 *     recording a type constraint (universal selector).
 *   - Otherwise, consumes a run of priv_is_name_char() bytes and classifies the
 *     span via ra8_reflow_tok_classify(). On success, stores the tag in
 *     @p rule->sel_tag and returns true. Returns false if the name is not a
 *     recognised HTML tag (caller drops the rule).
 *
 * @param[in]     s    Selector text span.
 * @param[in]     len  Length of @p s in bytes.
 * @param[in,out] i    Cursor into @p s; advanced past the consumed type token.
 * @param[in,out] rule Rule record to update with the parsed tag.
 *
 * @return bool Whether the type token was parsed successfully.
 * @retval true  A recognised type or universal selector was consumed.
 * @retval false The name at @p s[*i] is not a recognised HTML element tag.
 *
 * @pre @p s points to at least @p len readable bytes.
 * @pre @p i and @p rule are non-NULL; @p *i < @p len on entry.
 * @post On true, @p *i is advanced past the consumed token.
 * @post On true, @p rule->sel_tag holds the classified tag (or remains unknown for '*').
 *
 * @note Thread-safe with respect to distinct @p rule instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_parse_sel_type(const char* s, size_t len, size_t* i, ra8_css_rule_t* rule)
{
  if (s[*i] == '*') {
    ++(*i); /* universal: no type constraint */
    return true;
  }
  size_t start = *i;
  /* Bounded: i advances over name chars, capped by len. */
  while ((*i < len) && priv_is_name_char(s[*i])) {
    ++(*i);
  }
  const ra8_reflow_html_tag_t tag = ra8_reflow_tok_classify(&s[start], *i - start);
  if (tag == k_ra8_reflow_tag_unknown) {
    return false; /* unrecognised tag -> drop the rule */
  }
  rule->sel_tag = (uint8_t)tag;
  return true;
}

/**
 * @brief Parse one `.class` / `#id` part at @p s[*i] into @p rule; advance @p i.
 *
 * @details Reads the '.' or '#' prefix at @p s[*i], then consumes a run of
 * priv_is_name_char() bytes as the name. Interns the name via priv_intern_name()
 * and stores the resulting (off, len) pair in the appropriate field of @p rule:
 *   - '.' -> @p rule->class_off / @p rule->class_len (only one class supported in v1)
 *   - '#' -> @p rule->id_off / @p rule->id_len (only one id supported in v1)
 * Returns false when the name is empty, fails to intern, or is a duplicate
 * of an already-stored class/id on the same rule.
 *
 * @param[in,out] sheet Sheet whose name pool may receive the interned name.
 * @param[in]     s     Selector text span.
 * @param[in]     len   Length of @p s in bytes.
 * @param[in,out] i     Cursor into @p s; advanced past '.' or '#' and the name.
 * @param[in,out] rule  Rule record to update with the class or id constraint.
 *
 * @return bool Whether the part was parsed and stored successfully.
 * @retval true  A class or id constraint was interned and stored in @p rule.
 * @retval false The name was empty, overflowed the pool, or was a duplicate.
 *
 * @pre @p sheet, @p i, and @p rule are non-NULL.
 * @pre @p s[@p *i] is '.' or '#' on entry.
 * @post On true, @p *i is advanced past the name; the matching rule field is set.
 * @post On false, @p rule and @p sheet state are left in an undefined partial state
 *       (the rule is discarded by the caller).
 *
 * @note Thread-safe with respect to distinct @p sheet and @p rule instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_parse_sel_part(ra8_css_sheet_t* sheet,
                                const char*      s,
                                size_t           len,
                                size_t*          i,
                                ra8_css_rule_t*  rule)
{
  const char kind = s[*i];
  ++(*i);
  const size_t start = *i;
  while ((*i < len) && priv_is_name_char(s[*i])) {
    ++(*i);
  }
  const size_t nlen = *i - start;
  uint16_t     off  = 0U;
  if ((nlen == 0U) || !priv_intern_name(sheet, &s[start], nlen, &off)) {
    return false;
  }
  if (kind == '.') {
    if (rule->class_len != 0U) {
      return false; /* a second class -> unsupported in v1 */
    }
    rule->class_off = off;
    rule->class_len = (uint16_t)nlen;
    return true;
  }
  if (rule->id_len != 0U) {
    return false; /* a second id */
  }
  rule->id_off = off;
  rule->id_len = (uint16_t)nlen;
  return true;
}

/**
 * @brief Parse ONE trimmed compound selector into @p rule; false if unsupported.
 *
 * @details Accepts one type + one class + one id in CSS order, e.g. `*`, `p`,
 * `.note`, `#x`, `p.note`, `p#x`, `.note#x`. Two classes (`.a.b`), descendant
 * combinators (`div p`) and pseudo selectors fail (the caller drops the rule).
 * Delegates to priv_parse_sel_type() and priv_parse_sel_part() for each token.
 *
 * @param[in,out] sheet Sheet whose name pool may receive interned names.
 * @param[in]     s     Trimmed compound selector span (no surrounding whitespace).
 * @param[in]     len   Length of @p s in bytes.
 * @param[in,out] rule  Rule record to populate with selector constraints.
 *
 * @return bool Whether the selector was fully parsed.
 * @retval true  The entire span was consumed as a supported compound selector.
 * @retval false @p len is 0, an unsupported combinator or pseudo was encountered,
 *               or an inner parse helper returned false.
 *
 * @pre @p sheet and @p rule are non-NULL.
 * @pre @p s points to at least @p len readable bytes (may be NULL when len == 0).
 * @post On true, @p rule contains all parsed constraints; @p sheet->names_used
 *       may have grown.
 * @post On false, @p rule is left in a partial state and must be discarded.
 *
 * @note Thread-safe with respect to distinct @p sheet and @p rule instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool
priv_parse_selector(ra8_css_sheet_t* sheet, const char* s, size_t len, ra8_css_rule_t* rule)
{
  if (len == 0U) {
    return false;
  }
  size_t i   = 0U;
  bool   any = false;
  if (priv_is_name_char(s[0]) || (s[0] == '*')) {
    if (!priv_parse_sel_type(s, len, &i, rule)) {
      return false;
    }
    any = true;
  }
  /* Then a run of `.class` / `#id` parts (at most one of each). */
  while (i < len) {
    if (((s[i] != '.') && (s[i] != '#')) || !priv_parse_sel_part(sheet, s, len, &i, rule)) {
      return false; /* combinator / pseudo / dup / bad name -> unsupported */
    }
    any = true;
  }
  return any;
}

/**
 * @brief Split @p s into whitespace-separated compound spans.
 *
 * @details Iterates over @p s[0..len), skipping runs of ra8_reflow_css_is_ws() characters
 * and recording the start and length of each non-whitespace token in the parallel
 * @p part_p / @p part_n arrays. Returns -1 early when the part count would exceed
 * (k_ra8_css_max_anc + 1), enforcing the maximum ancestor depth. Both output
 * arrays must have capacity for at least (k_ra8_css_max_anc + 1) entries.
 *
 * @param[in]  s       Selector text span (need not be NUL-terminated).
 * @param[in]  len     Number of bytes in @p s.
 * @param[out] part_p  Array of pointers; each entry points into @p s at the
 *                     start of the corresponding compound token.
 * @param[out] part_n  Array of sizes; each entry is the byte length of the
 *                     corresponding compound token.
 *
 * @return int32_t Number of parts found, or -1 on overflow.
 * @retval 0  The span is empty or contains only whitespace.
 * @retval >0 The number of whitespace-separated compound tokens found.
 * @retval -1 More than (k_ra8_css_max_anc + 1) tokens were encountered.
 *
 * @pre @p s points to at least @p len readable bytes.
 * @pre @p part_p and @p part_n are non-NULL with capacity >= (k_ra8_css_max_anc + 1).
 * @post On a non-negative return, @p part_p[0..ret) and @p part_n[0..ret) are set.
 * @post @p s is not modified; entries beyond the return count are unspecified.
 *
 * @note Thread-safe; operates only on caller-owned arrays.
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t priv_split_compounds(const char* s, size_t len, const char** part_p, size_t* part_n)
{
  size_t nparts = 0U;
  size_t i      = 0U;
  /* Bounded: i advances to len; each pass consumes >=1 char after the ws skip. */
  while (i < len) {
    /* mcdc-deactivated: priv_split_compounds is only ever called with a whitespace-trimmed selector (ra8_reflow_css_trim in priv_parse_selector_list and the block dispatcher), so there is no trailing whitespace for this inner skip to consume up to len; the (i < len) false arm is unreachable on any public path. */
    while ((i < len) && ra8_reflow_css_is_ws(s[i])) {
      ++i;
    }
    if (i >= len) {
      break;
    }
    if (nparts > (size_t)k_ra8_css_max_anc) {
      return -1; /* more than one subject + k_ra8_css_max_anc ancestor parts */
    }
    const size_t start = i;
    while ((i < len) && !ra8_reflow_css_is_ws(s[i])) {
      ++i;
    }
    part_p[nparts] = &s[start];
    part_n[nparts] = i - start;
    ++nparts;
  }
  return (int32_t)nparts;
}

/**
 * @brief Parse a (possibly descendant) selector string into @p rule.
 *
 * @details Splits @p s on whitespace via priv_split_compounds() to isolate the
 * subject compound (last token) and ancestor compounds (preceding tokens). The
 * subject is parsed into @p rule's sel_tag, class, and id fields via
 * priv_parse_selector(). Each ancestor is parsed into a temporary rule and its
 * constraints are stored in @p rule->anc[] in selector order (outermost first),
 * with @p rule->anc_count set to the number of ancestor parts. Returns false
 * when priv_split_compounds() returns <= 0, or any compound fails to parse.
 *
 * @param[in,out] sheet Sheet whose name pool receives interned class/id names.
 * @param[in]     s     Selector text (may include whitespace for descendants).
 * @param[in]     len   Number of bytes in @p s.
 * @param[in,out] rule  Rule record to populate; must be zero-initialised on entry.
 *
 * @return bool Whether the full selector was parsed.
 * @retval true  All compounds parsed; @p rule holds the complete selector.
 * @retval false @p s is empty, has too many parts, or any compound is unsupported.
 *
 * @pre @p sheet and @p rule are non-NULL.
 * @pre @p s points to at least @p len readable bytes.
 * @post On true, @p rule->anc_count and @p rule->anc[] reflect the ancestor chain.
 * @post On false, @p rule is in a partial state and must be discarded.
 *
 * @note Thread-safe with respect to distinct @p sheet and @p rule instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool
priv_parse_complex_selector(ra8_css_sheet_t* sheet, const char* s, size_t len, ra8_css_rule_t* rule)
{
  const char*   part_p[(size_t)k_ra8_css_max_anc + 1U] = {};
  size_t        part_n[(size_t)k_ra8_css_max_anc + 1U] = {};
  const int32_t nparts = priv_split_compounds(s, len, part_p, part_n);
  if (nparts <= 0) {
    return false;
  }
  const size_t np = (size_t)nparts;
  /* Last part = subject compound. */
  if (!priv_parse_selector(sheet, part_p[np - 1U], part_n[np - 1U], rule)) {
    return false;
  }
  /* Earlier parts = ancestor constraints (outermost first). */
  rule->anc_count = (uint8_t)(np - 1U);
  for (size_t a = 0U; (a + 1U) < np; ++a) {
    ra8_css_rule_t tmp = {};
    if (!priv_parse_selector(sheet, part_p[a], part_n[a], &tmp)) {
      return false;
    }
    rule->anc[a].tag       = tmp.sel_tag;
    rule->anc[a].class_off = tmp.class_off;
    rule->anc[a].class_len = tmp.class_len;
    rule->anc[a].id_off    = tmp.id_off;
    rule->anc[a].id_len    = tmp.id_len;
  }
  return true;
}

/**
 * @brief Append one fully-built rule to the sheet; drop silently if full.
 *
 * @details Copies @p *rule into @p sheet->rules[sheet->rule_count], stamps
 * the stored rule's order field with @p sheet->next_order, then increments
 * both @p sheet->rule_count and @p sheet->next_order. When the sheet has
 * already reached k_ra8_css_max_rules, the function returns immediately without
 * modifying any field, silently discarding the rule.
 *
 * @param[in,out] sheet Sheet that receives the appended rule.
 * @param[in]     rule  Fully-parsed rule to append; must not be NULL.
 *
 * @return Nothing.
 *
 * @pre @p sheet and @p rule are non-NULL.
 * @pre @p rule is fully populated (selector and declaration fields valid).
 * @post When @p sheet->rule_count < k_ra8_css_max_rules, rule_count and next_order
 *       are each incremented by 1 and the rule is stored.
 * @post When @p sheet->rule_count == k_ra8_css_max_rules, no field is modified.
 *
 * @note Thread-safe with respect to distinct @p sheet instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_push_rule(ra8_css_sheet_t* sheet, const ra8_css_rule_t* rule)
{
  if (sheet->rule_count >= (uint16_t)k_ra8_css_max_rules) {
    return;
  }
  sheet->rules[sheet->rule_count]       = *rule;
  sheet->rules[sheet->rule_count].order = sheet->next_order;
  sheet->rule_count                     = (uint16_t)(sheet->rule_count + 1U);
  sheet->next_order                     = (uint16_t)(sheet->next_order + 1U);
}

/**
 * @brief Parse a comma-grouped selector list sharing declaration @p decl.
 *
 * @details Iterates over comma-delimited selectors in @p sel[0..sel_len), trims
 * each individual selector, constructs a fresh ra8_css_rule_t carrying @p decl,
 * and attempts to parse the selector via priv_parse_complex_selector(). Rules
 * that parse successfully are appended to @p sheet via priv_push_rule(); those
 * that fail to parse are silently discarded. The loop is bounded by @p sel_len;
 * each iteration advances past the next comma.
 *
 * @param[in,out] sheet   Sheet that receives the generated rules.
 * @param[in]     sel     Comma-separated selector list span.
 * @param[in]     sel_len Number of bytes in @p sel.
 * @param[in]     decl    Shared declaration applied to every generated rule.
 *
 * @return Nothing.
 *
 * @pre @p sheet is non-NULL.
 * @pre @p sel points to at least @p sel_len readable bytes (may be NULL when 0).
 * @post Zero or more rules derived from @p sel and @p decl have been appended to
 *       @p sheet (subject to k_ra8_css_max_rules capacity).
 * @post @p sel is not modified.
 *
 * @note Thread-safe with respect to distinct @p sheet instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_parse_selector_list(ra8_css_sheet_t* sheet,
                                     const char*      sel,
                                     size_t           sel_len,
                                     ra8_css_style_t  decl)
{
  size_t i = 0U;
  while (i < sel_len) {
    size_t comma = i;
    while ((comma < sel_len) && (sel[comma] != ',')) {
      ++comma;
    }
    size_t         one_len = comma - i;
    const char*    one     = ra8_reflow_css_trim(&sel[i], &one_len);
    ra8_css_rule_t rule    = {};
    rule.decl              = decl;
    if (priv_parse_complex_selector(sheet, one, one_len, &rule)) {
      priv_push_rule(sheet, &rule);
    }
    i = comma + 1U;
  }
}

/* ===========================================================================
 * @font-face + font-family parsing + block dispatch
 * ===========================================================================
 */

/** @brief Strip one layer of matching `'`/`"` quotes from span @p s[0..*len). */
RA8_INTERNAL
static const char* priv_strip_quotes(const char* s, size_t* len)
{
  if ((*len >= 2U) && ((s[0] == '"') || (s[0] == '\'')) && (s[*len - 1U] == s[0])) {
    *len -= 2U;
    return &s[1];
  }
  return s;
}

/**
 * @brief Extract the path inside the first `url(...)` of a `src` value.
 *
 * @details Scans @p val[0..vlen) for the first case-insensitive "url(" prefix,
 * then locates the matching closing ')' and trims whitespace and one layer of
 * matching single or double quotes from the content via priv_strip_quotes().
 * On success, @p *url and @p *ulen receive the path span (pointing into @p val)
 * and the function returns true. Returns false when no "url(" is found or the
 * extracted path is empty after trimming.
 *
 * @param[in]  val  CSS `src` value span (e.g. `url('foo.ttf') format('truetype')`).
 * @param[in]  vlen Number of bytes in @p val.
 * @param[out] url  Receives a pointer into @p val at the start of the path (valid only on true).
 * @param[out] ulen Receives the length of the path in bytes (valid only on true).
 *
 * @return bool Whether a non-empty URL path was extracted.
 * @retval true  @p *url and @p *ulen point to the unquoted path inside "url(...)".
 * @retval false No "url(" was found, or the extracted path was empty.
 *
 * @pre @p val points to at least @p vlen readable bytes.
 * @pre @p url and @p ulen are non-NULL.
 * @post On true, @p *url points into @p val and @p *ulen > 0.
 * @post @p val is not modified.
 *
 * @note Thread-safe; operates only on caller-owned memory.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_extract_url(const char* val, size_t vlen, const char** url, size_t* ulen)
{
  /* Bounded: at most (vlen - 3) windows; i advances by 1 each step. */
  for (size_t i = 0U; (i + (size_t)k_priv_url_len) <= vlen; ++i) {
    if (!ra8_reflow_css_ci_eq(&val[i], (size_t)k_priv_url_len, "url(")) {
      continue;
    }
    size_t a = i + (size_t)k_priv_url_len;
    size_t b = a;
    while ((b < vlen) && (val[b] != ')')) {
      ++b;
    }
    size_t      n = b - a;
    const char* p = ra8_reflow_css_trim(&val[a], &n);
    p             = priv_strip_quotes(p, &n);
    if (n == 0U) {
      return false;
    }
    *url  = p;
    *ulen = n;
    return true;
  }
  return false;
}

/**
 * @brief True for a `font-weight` keyword that selects the bold face.
 *
 * @details Returns true when @p val[0..vlen) matches (case-insensitively) any of
 * the CSS bold-weight keywords: "bold", "bolder", "600", "700", "800", or "900".
 * All comparisons are delegated to ra8_reflow_css_ci_eq().
 *
 * @param[in] val  CSS font-weight value span (not NUL-terminated).
 * @param[in] vlen Number of bytes in @p val.
 *
 * @return bool Whether the value selects the bold face.
 * @retval true  @p val is a bold-selecting keyword or numeric weight.
 * @retval false @p val is a non-bold weight (e.g., "normal", "400").
 *
 * @pre @p val points to at least @p vlen readable bytes.
 * @pre @p vlen accurately reflects the span length.
 * @post No state is mutated; the function is pure.
 * @post @p val is not modified.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_is_bold_kw(const char* val, size_t vlen)
{
  return ra8_reflow_css_ci_eq(val, vlen, "bold") || ra8_reflow_css_ci_eq(val, vlen, "bolder") ||
         ra8_reflow_css_ci_eq(val, vlen, "600") || ra8_reflow_css_ci_eq(val, vlen, "700") ||
         ra8_reflow_css_ci_eq(val, vlen, "800") || ra8_reflow_css_ci_eq(val, vlen, "900");
}

/**
 * @brief True for a `font-style` keyword that selects the italic face.
 *
 * @details Returns true when @p val[0..vlen) matches (case-insensitively) either
 * "italic" or "oblique" via ra8_reflow_css_ci_eq(). All other values (e.g., "normal")
 * return false.
 *
 * @param[in] val  CSS font-style value span (not NUL-terminated).
 * @param[in] vlen Number of bytes in @p val.
 *
 * @return bool Whether the value selects the italic face.
 * @retval true  @p val is "italic" or "oblique".
 * @retval false @p val is any other font-style value.
 *
 * @pre @p val points to at least @p vlen readable bytes.
 * @pre @p vlen accurately reflects the span length.
 * @post No state is mutated; the function is pure.
 * @post @p val is not modified.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_is_italic_kw(const char* val, size_t vlen)
{
  return ra8_reflow_css_ci_eq(val, vlen, "italic") || ra8_reflow_css_ci_eq(val, vlen, "oblique");
}

/**
 * @brief Apply one `@font-face` declaration to the face being built.
 *
 * @details Handles the four recognised `@font-face` descriptors:
 *   - "font-family": strips quotes and interns the family name into @p sheet;
 *     stores (off, len) in @p face->family_off / family_len.
 *   - "font-weight": sets @p face->weight_bold via priv_is_bold_kw().
 *   - "font-style": sets @p face->style_italic via priv_is_italic_kw().
 *   - "src": extracts the first url() path via priv_extract_url() and interns it;
 *     stores (off, len) in @p face->src_off / src_len.
 * All other descriptors (e.g., unicode-range) are silently ignored.
 *
 * @param[in,out] sheet Sheet whose name pool receives interned strings.
 * @param[in]     prop  Descriptor property name span (not NUL-terminated).
 * @param[in]     plen  Length of @p prop in bytes.
 * @param[in]     val   Descriptor value span (not NUL-terminated).
 * @param[in]     vlen  Length of @p val in bytes.
 * @param[in,out] face  Font-face record being accumulated.
 *
 * @return Nothing.
 *
 * @pre @p sheet and @p face are non-NULL.
 * @pre @p prop points to at least @p plen readable bytes.
 * @post On a recognised descriptor, the matching field of @p face is updated.
 * @post @p prop and @p val are not modified.
 *
 * @note Thread-safe with respect to distinct @p sheet and @p face instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_face_apply(ra8_css_sheet_t*    sheet,
                            const char*         prop,
                            size_t              plen,
                            const char*         val,
                            size_t              vlen,
                            ra8_css_fontface_t* face)
{
  if (ra8_reflow_css_ci_eq(prop, plen, "font-family")) {
    size_t      n   = vlen;
    const char* fam = priv_strip_quotes(val, &n);
    uint16_t    off = 0U;
    if ((n > 0U) && priv_intern_name(sheet, fam, n, &off)) {
      face->family_off = off;
      face->family_len = (uint16_t)n;
    }
  } else if (ra8_reflow_css_ci_eq(prop, plen, "font-weight")) {
    face->weight_bold = priv_is_bold_kw(val, vlen) ? 1U : 0U;
  } else if (ra8_reflow_css_ci_eq(prop, plen, "font-style")) {
    face->style_italic = priv_is_italic_kw(val, vlen) ? 1U : 0U;
  } else if (ra8_reflow_css_ci_eq(prop, plen, "src")) {
    const char* url  = nullptr;
    size_t      ulen = 0U;
    uint16_t    off  = 0U;
    if (priv_extract_url(val, vlen, &url, &ulen) && priv_intern_name(sheet, url, ulen, &off)) {
      face->src_off = off;
      face->src_len = (uint16_t)ulen;
    }
  } else {
    /* Other @font-face descriptors (unicode-range, ...) -> ignored. */
  }
}

/** @brief Callback invoked by ::priv_for_each_decl on each `prop:value` pair. */
typedef void (
  *priv_decl_fn)(void* ctx, const char* prop, size_t plen, const char* val, size_t vlen);

/**
 * @brief Iterate `prop:value;` pairs in a declaration block (no braces).
 *
 * @details Scans @p s[0..len) for semicolon-delimited declarations and invokes
 * @p cb(ctx, prop, plen, val, vlen) for each pair that has a non-empty property
 * and value after whitespace trimming. The outer loop advances past each semicolon
 * and is bounded by @p len; the inner colon search is bounded by the semicolon
 * position. Pairs missing a colon are skipped silently.
 *
 * @param[in] s   Declaration block text (no surrounding braces).
 * @param[in] len Number of bytes in @p s.
 * @param[in] cb  Callback invoked once per valid prop:value pair; must not be NULL.
 * @param[in] ctx Opaque context pointer forwarded to @p cb unchanged.
 *
 * @return Nothing.
 *
 * @pre @p s points to at least @p len readable bytes (may be NULL when len == 0).
 * @pre @p cb is non-NULL.
 * @post @p cb has been called for each valid declaration found in @p s.
 * @post @p s is not modified.
 *
 * @note Thread-safety depends on @p cb and @p ctx; the scanner itself is stateless.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_for_each_decl(const char* s, size_t len, priv_decl_fn cb, void* ctx)
{
  size_t i = 0U;
  /* Bounded: each pass advances past the next ';' (or to len). */
  while (i < len) {
    size_t semi = i;
    while ((semi < len) && (s[semi] != ';')) {
      ++semi;
    }
    size_t colon = i;
    while ((colon < semi) && (s[colon] != ':')) {
      ++colon;
    }
    if (colon < semi) {
      size_t      plen = colon - i;
      const char* prop = ra8_reflow_css_trim(&s[i], &plen);
      size_t      vlen = semi - (colon + 1U);
      const char* val  = ra8_reflow_css_trim(&s[colon + 1U], &vlen);
      if ((plen > 0U) && (vlen > 0U)) {
        cb(ctx, prop, plen, val, vlen);
      }
    }
    i = semi + 1U;
  }
}

/** @brief Context threading a sheet + the face being built through the loop. */
typedef struct {
  ra8_css_sheet_t*    sheet; /**< Sheet receiving interned name bytes. */
  ra8_css_fontface_t* face;  /**< Face accumulating descriptors.       */
} priv_face_ctx_t;

/**
 * @brief priv_decl_fn adapter that routes one descriptor to priv_face_apply().
 *
 * @details Casts @p ctx to a priv_face_ctx_t pointer and forwards the property
 * and value spans to priv_face_apply() with the sheet and face from the context.
 * This function is designed to be passed as the @p cb argument of
 * priv_for_each_decl() when parsing an `@font-face` block.
 *
 * @param[in] ctx  Pointer to a priv_face_ctx_t (sheet + face); must not be NULL.
 * @param[in] prop Property name span (not NUL-terminated).
 * @param[in] plen Length of @p prop in bytes.
 * @param[in] val  Property value span (not NUL-terminated).
 * @param[in] vlen Length of @p val in bytes.
 *
 * @return Nothing.
 *
 * @pre @p ctx is a non-NULL priv_face_ctx_t pointer with valid sheet and face fields.
 * @pre @p prop points to at least @p plen readable bytes.
 * @post priv_face_apply() has been called with the unwrapped context fields.
 * @post @p prop and @p val are not modified.
 *
 * @note Thread-safety matches that of priv_face_apply().
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_face_cb(void* ctx, const char* prop, size_t plen, const char* val, size_t vlen)
{
  priv_face_ctx_t* c = (priv_face_ctx_t*)ctx;
  priv_face_apply(c->sheet, prop, plen, val, vlen, c->face);
}

/**
 * @brief Parse one `@font-face { ... }` block; append it if family + src set.
 *
 * @details Iterates over the declaration block via priv_for_each_decl() with
 * priv_face_cb() as the callback to accumulate font-family, font-weight,
 * font-style, and src into a local ra8_css_fontface_t. The face is appended to
 * @p sheet->faces[] only when both family_len and src_len are non-zero. If the
 * sheet is already at k_ra8_css_max_faces capacity, the function returns without
 * parsing.
 *
 * @param[in,out] sheet Sheet that may receive the parsed font face.
 * @param[in]     block Declaration block text (no surrounding braces).
 * @param[in]     len   Number of bytes in @p block.
 *
 * @return Nothing.
 *
 * @pre @p sheet is non-NULL.
 * @pre @p block points to at least @p len readable bytes (may be NULL when len == 0).
 * @post When family and src were set, @p sheet->face_count is incremented by 1
 *       and the face is stored in @p sheet->faces[].
 * @post @p block is not modified.
 *
 * @note Thread-safe with respect to distinct @p sheet instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_parse_fontface(ra8_css_sheet_t* sheet, const char* block, size_t len)
{
  if (sheet->face_count >= (uint16_t)k_ra8_css_max_faces) {
    return;
  }
  ra8_css_fontface_t face = {};
  priv_face_ctx_t    ctx  = {.sheet = sheet, .face = &face};
  priv_for_each_decl(block, len, priv_face_cb, &ctx);
  if ((face.family_len != 0U) && (face.src_len != 0U)) {
    sheet->faces[sheet->face_count] = face;
    sheet->face_count               = (uint16_t)(sheet->face_count + 1U);
  }
}

/** @brief Context threading a sheet + the rule declaration through the loop. */
typedef struct {
  ra8_css_sheet_t* sheet; /**< Sheet receiving the interned family name. */
  ra8_css_style_t* decl;  /**< Declaration receiving the family slice.   */
} priv_family_ctx_t;

/**
 * @brief priv_decl_fn adapter that interns a rule's `font-family` value.
 *
 * @details Casts @p ctx to a priv_family_ctx_t pointer. Ignores the callback
 * unless @p prop matches "font-family". When it does, strips quotes from @p val
 * via priv_strip_quotes() and interns the result into the sheet's name pool.
 * On success, sets the k_ra8_css_set_family bit in @p decl->set and stores
 * (off, len) in @p decl->family_off / family_len. This function is designed to
 * be used as the @p cb argument of priv_for_each_decl().
 *
 * @param[in] ctx  Pointer to a priv_family_ctx_t (sheet + decl); must not be NULL.
 * @param[in] prop Property name span (not NUL-terminated).
 * @param[in] plen Length of @p prop in bytes.
 * @param[in] val  Property value span (not NUL-terminated).
 * @param[in] vlen Length of @p val in bytes.
 *
 * @return Nothing.
 *
 * @pre @p ctx is a non-NULL priv_family_ctx_t with valid sheet and decl fields.
 * @pre @p prop points to at least @p plen readable bytes.
 * @post When @p prop is "font-family" and intern succeeds, @p ctx->decl has the
 *       family fields set and k_ra8_css_set_family OR'd into @p ctx->decl->set.
 * @post @p prop and @p val are not modified.
 *
 * @note Thread-safety matches that of priv_intern_name().
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_family_cb(void* ctx, const char* prop, size_t plen, const char* val, size_t vlen)
{
  priv_family_ctx_t* c = (priv_family_ctx_t*)ctx;
  if (!ra8_reflow_css_ci_eq(prop, plen, "font-family")) {
    return;
  }
  size_t      n   = vlen;
  const char* fam = priv_strip_quotes(val, &n);
  uint16_t    off = 0U;
  if ((n > 0U) && priv_intern_name(c->sheet, fam, n, &off)) {
    c->decl->set        = (uint8_t)(c->decl->set | (uint8_t)k_ra8_css_set_family);
    c->decl->family_off = off;
    c->decl->family_len = (uint16_t)n;
  }
}

/**
 * @brief Scan a rule's declaration block for `font-family`, interning it.
 *
 * @details Calls priv_for_each_decl() with priv_family_cb() to find and intern
 * the "font-family" property from @p block into @p sheet->names and record it in
 * @p decl. This is a thin convenience wrapper that sets up a priv_family_ctx_t
 * and delegates all logic to the callback.
 *
 * @param[in,out] sheet Sheet whose name pool may receive the interned family name.
 * @param[in]     block Declaration block text (no surrounding braces).
 * @param[in]     len   Number of bytes in @p block.
 * @param[in,out] decl  Declaration to update with the font-family slice.
 *
 * @return Nothing.
 *
 * @pre @p sheet and @p decl are non-NULL.
 * @pre @p block points to at least @p len readable bytes (may be NULL when len == 0).
 * @post If "font-family" is present in @p block and intern succeeds, @p decl has
 *       k_ra8_css_set_family set and family_off/family_len populated.
 * @post @p block is not modified.
 *
 * @note Thread-safe with respect to distinct @p sheet and @p decl instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
priv_extract_family(ra8_css_sheet_t* sheet, const char* block, size_t len, ra8_css_style_t* decl)
{
  priv_family_ctx_t ctx = {.sheet = sheet, .decl = decl};
  priv_for_each_decl(block, len, priv_family_cb, &ctx);
}

/**
 * @brief Route an at-rule: parse `@font-face`; skip every other `@`-rule.
 *
 * @details Compares the trimmed at-keyword @p sel against "@font-face"
 * (case-insensitive). When it matches, delegates parsing of the block to
 * priv_parse_fontface(). All other at-rules (`@media`, `@import`, `@page`,
 * etc.) are out of the v1 scope and are silently discarded.
 *
 * @param[in,out] sheet     Sheet that may receive the parsed font face.
 * @param[in]     sel       At-keyword span (e.g. "@font-face"), trimmed.
 * @param[in]     sel_len   Length of @p sel in bytes.
 * @param[in]     block     Block body text (no surrounding braces).
 * @param[in]     block_len Number of bytes in @p block.
 *
 * @return Nothing.
 *
 * @pre @p sheet is non-NULL.
 * @pre @p sel points to at least @p sel_len readable bytes.
 * @post When @p sel is "@font-face", priv_parse_fontface() has been called.
 * @post @p sel and @p block are not modified.
 *
 * @note Thread-safe with respect to distinct @p sheet instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_parse_at_rule(ra8_css_sheet_t* sheet,
                               const char*      sel,
                               size_t           sel_len,
                               const char*      block,
                               size_t           block_len)
{
  if (ra8_reflow_css_ci_eq(sel, sel_len, "@font-face")) {
    priv_parse_fontface(sheet, block, block_len);
  }
  /* @media / @import / @page / ... are out of v1 scope -> skipped. */
}

/**
 * @brief Dispatch one `selector|@rule { block }`: at-rule vs. style rule.
 *
 * @details Trims @p sel to obtain the effective keyword. When the first byte is
 * '@', routes to priv_parse_at_rule(). Otherwise, parses the declaration block
 * via ra8_reflow_css_parse_decls() and priv_extract_family(), then distributes the
 * resulting style to every comma-separated selector in @p sel via
 * priv_parse_selector_list().
 *
 * @param[in,out] sheet     Sheet receiving parsed rules or font faces.
 * @param[in]     sel       Selector or at-keyword text.
 * @param[in]     sel_len   Number of bytes in @p sel.
 * @param[in]     block     Declaration block body (no surrounding braces).
 * @param[in]     block_len Number of bytes in @p block.
 *
 * @return Nothing.
 *
 * @pre @p sheet is non-NULL.
 * @pre @p sel points to at least @p sel_len readable bytes.
 * @post @p sheet has been updated with all rules or faces derived from this block.
 * @post @p sel and @p block are not modified.
 *
 * @note Thread-safe with respect to distinct @p sheet instances.
 * @since 0.1.0
 */
void ra8_reflow_css_parse_one_block(ra8_css_sheet_t* sheet,
                                    const char*      sel,
                                    size_t           sel_len,
                                    const char*      block,
                                    size_t           block_len)
{
  size_t      tlen = sel_len;
  const char* tsel = ra8_reflow_css_trim(sel, &tlen);
  if ((tlen > 0U) && (tsel[0] == '@')) {
    priv_parse_at_rule(sheet, tsel, tlen, block, block_len);
    return;
  }
  ra8_css_style_t decl = {};
  ra8_reflow_css_parse_decls(block, block_len, &decl);
  priv_extract_family(sheet, block, block_len, &decl);
  priv_parse_selector_list(sheet, sel, sel_len, decl);
}
