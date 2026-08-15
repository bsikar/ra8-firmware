/**
 * @file ra8_reflow_css_cascade.c
 * @brief Content-CSS stylesheet scanner, cascade, and face lookup (#111).
 *
 * @details
 * The top-level stylesheet scanner (::ra8_css_parse, comment + block skipping),
 * the selector-matching and specificity cascade, and the `@font-face` lookup
 * API for the v1 CSS subset documented in `ra8_reflow_css.h`. The scanner walks
 * `selector { ... }` blocks and hands each to ::priv_ra8_reflow_css_parse_one_block in
 * a sibling translation unit; the cascade resolves inherited / rule / inline
 * sources into a single ::ra8_css_style_t. No MMIO, no heap; all state lives in
 * the caller-owned ::ra8_css_sheet_t.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_reflow.h" /* k_ra8_reflow_tag_unknown, style/align enums */
#include "ra8_reflow_css.h"
#include "ra8_reflow_css_internal.h"

/* ===========================================================================
 * Internal constants (no magic numbers)
 * ===========================================================================
 */

/**
 * @enum priv_css_consts_t
 * @brief Cascade specificity weights (packed id > class > type > universal).
 *
 * @details A rule's specificity is the sum of one weight per present constraint
 * (id + class + type), each at most 1 in our compound model; the packed value
 * orders the cascade. Inheritance is the lowest priority (weight 0); a universal
 * selector (no constraints) is also 0 but beats inheritance on source order.
 */
typedef enum : uint16_t {
  k_priv_rank_inherited = 0U,     /**< Inheritance / universal weight. */
  k_priv_spec_type      = 1U,     /**< A type constraint adds this.    */
  k_priv_spec_class     = 100U,   /**< A class constraint adds this.   */
  k_priv_spec_id        = 10000U, /**< An id constraint adds this.     */
} priv_css_consts_t;

/**
 * @enum priv_css_scan_t
 * @brief Token lengths used by the top-level stylesheet scanner.
 *
 * @details Stand-ins for the `/` + `*` comment delimiters so the parse loop
 * carries no bare numeric literals when stepping over comments and blocks.
 *
 * @invariant k_priv_cmt_marker is the byte count of `/` + `*` (or `*` + `/`).
 * @see ra8_css_parse
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_priv_cmt_marker = 2U, /**< Byte length of a CSS comment delimiter pair. */
} priv_css_scan_t;

/* ===========================================================================
 * Public API -- stylesheet scanner
 * ===========================================================================
 */

ra8_err_t ra8_css_sheet_reset(ra8_css_sheet_t* sheet)
{
  if (sheet == nullptr) {
    return k_ra8_err_null_ptr;
  }
  sheet->rule_count = 0U;
  sheet->face_count = 0U;
  sheet->next_order = 0U;
  sheet->names_used = 0U;
  return k_ra8_ok;
}

/**
 * @brief Step over a `C`-style comment starting at @p start.
 *
 * @details Assumes @p css[start..start+1] is the open delimiter and scans to
 * the matching close delimiter, returning the offset just past it (clamped to
 * @p len when the comment is unterminated).
 *
 * @param[in] css   Stylesheet text (non-NULL, validated by the caller).
 * @param[in] len   Total length of @p css, bytes.
 * @param[in] start Offset of the comment open delimiter.
 *
 * @return Offset of the first byte after the comment, in `[start, len]`.
 * @retval len The comment ran to the end of the buffer (unterminated).
 *
 * @pre @p css is non-NULL.
 * @pre @p start < @p len.
 * @post The return value is in `[start, len]`.
 * @post No state is mutated (pure function).
 * @note Thread-safe; operates on caller-owned memory only.
 * @since 0.1.0
 */
RA8_INTERNAL
static size_t internal_skip_comment(const char* css, size_t len, size_t start)
{
  const char open_a = '/';
  const char open_b = '*';
  size_t     j      = start + (size_t)k_priv_cmt_marker;
  /* Bounded: j strictly increases each pass; capped at len. */
  while (((j + 1U) < len) && !((css[j] == open_b) && (css[j + 1U] == open_a))) {
    ++j;
  }
  const size_t past = j + (size_t)k_priv_cmt_marker;
  return (past <= len) ? past : len;
}

/**
 * @brief Locate the `{ ... }` block beginning at selector offset @p i.
 *
 * @details Scans forward for the block-open byte, then the block-close byte,
 * writing both offsets out. Returns false when no block-open is present, which
 * the caller treats as end-of-input.
 *
 * @param[in]  css       Stylesheet text (non-NULL, validated by the caller).
 * @param[in]  len       Total length of @p css, bytes.
 * @param[in]  i         Offset of the selector list start.
 * @param[out] out_open  Offset of the block-open byte (valid only on true).
 * @param[out] out_close Offset of the block-close byte, or @p len if missing.
 *
 * @return True iff a block-open byte was found at or after @p i.
 * @retval true  A block-open was found; @p out_open / @p out_close are set.
 * @retval false No block-open exists in `[i, len)`.
 *
 * @pre @p css, @p out_open, @p out_close are non-NULL.
 * @pre @p i <= @p len.
 * @post On true, `*out_open < len` and `*out_close <= len`.
 * @post No state other than the out-params is mutated.
 * @note Thread-safe; operates on caller-owned memory only.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool
internal_find_block(const char* css, size_t len, size_t i, size_t* out_open, size_t* out_close)
{
  const char open_c  = '{';
  const char close_c = '}';
  size_t     brace   = i;
  /* Bounded: brace strictly increases each pass; capped at len. */
  while ((brace < len) && (css[brace] != open_c)) {
    ++brace;
  }
  if (brace >= len) {
    return false;
  }
  size_t close = brace + 1U;
  /* Bounded: close strictly increases each pass; capped at len. */
  while ((close < len) && (css[close] != close_c)) {
    ++close;
  }
  *out_open  = brace;
  *out_close = close;
  return true;
}

ra8_err_t ra8_css_parse(ra8_css_sheet_t* sheet, const char* css, uint32_t len)
{
  if ((sheet == nullptr) || (css == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const char open_a = '/';
  const char open_b = '*';
  size_t     i      = 0U;
  /* Bounded: each iteration advances past a block or breaks at EOF. */
  while (i < (size_t)len) {
    if (((i + 1U) < (size_t)len) && (css[i] == open_a) && (css[i + 1U] == open_b)) {
      i = internal_skip_comment(css, (size_t)len, i);
      continue;
    }
    if (priv_ra8_reflow_css_is_ws(css[i])) {
      ++i;
      continue;
    }
    size_t brace = 0U;
    size_t close = 0U;
    if (!internal_find_block(css, (size_t)len, i, &brace, &close)) {
      break; /* no block -> done */
    }
    priv_ra8_reflow_css_parse_one_block(sheet,
                                        &css[i],
                                        brace - i,
                                        &css[brace + 1U],
                                        close - (brace + 1U));
    i = (close < (size_t)len) ? (close + 1U) : (size_t)len;
  }
  return k_ra8_ok;
}

/* ===========================================================================
 * Selector matching + cascade + face lookup
 * ===========================================================================
 */

/**
 * @brief True iff a space-separated class list contains @p name exactly.
 *
 * @details Tokenises @p list[0..list_len) on whitespace and performs a byte-exact
 * (not case-folded) comparison of each token against @p name[0..nlen) via memcmp.
 * The outer loop is bounded by @p list_len; each pass skips leading whitespace,
 * extracts the next token, and compares it. Returns true on the first matching
 * token; false if no token matches.
 *
 * @param[in] list     Space-separated class attribute value span.
 * @param[in] list_len Number of bytes in @p list.
 * @param[in] name     Interned class name to search for.
 * @param[in] nlen     Length of @p name in bytes.
 *
 * @return bool Search result.
 * @retval true  At least one whitespace-delimited token in @p list equals @p name.
 * @retval false No token in @p list matches @p name, or @p list is empty.
 *
 * @pre @p list points to at least @p list_len readable bytes.
 * @pre @p name points to at least @p nlen readable bytes.
 * @post No state is mutated; the function is pure.
 * @post @p list and @p name are not modified.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool
internal_class_list_has(const char* list, size_t list_len, const char* name, size_t nlen)
{
  size_t i = 0U;
  /* Bounded: each pass skips >=0 ws then a token, advancing i to list_len. */
  while (i < list_len) {
    while ((i < list_len) && priv_ra8_reflow_css_is_ws(list[i])) {
      ++i;
    }
    size_t start = i;
    while ((i < list_len) && !priv_ra8_reflow_css_is_ws(list[i])) {
      ++i;
    }
    const size_t tlen = i - start;
    if ((tlen == nlen) && (memcmp(&list[start], name, nlen) == 0)) {
      return true;
    }
  }
  return false;
}

bool ra8_css_rule_matches(const ra8_css_rule_t*    rule,
                          const ra8_css_element_t* el,
                          const ra8_css_sheet_t*   sheet)
{
  if ((rule == nullptr) || (el == nullptr) || (sheet == nullptr)) {
    return false;
  }
  /* Every present constraint must match (no constraint = universal). */
  if ((rule->sel_tag != (uint8_t)k_ra8_reflow_tag_unknown) && (el->tag != rule->sel_tag)) {
    return false;
  }
  if (rule->class_len != 0U) {
    const char* nm = (const char*)&sheet->names[rule->class_off];
    if ((el->class_str == nullptr) ||
        !internal_class_list_has(el->class_str, el->class_len, nm, rule->class_len)) {
      return false;
    }
  }
  if (rule->id_len != 0U) {
    const char* nm = (const char*)&sheet->names[rule->id_off];
    if ((el->id == nullptr) || (el->id_len != rule->id_len) ||
        (memcmp(el->id, nm, rule->id_len) != 0)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief True if descendant ancestor part @p anc matches element @p el.
 *
 * @details Checks each non-zero constraint field of @p anc against @p el:
 *   - tag:       @p el->tag must equal @p anc->tag when the tag is not unknown.
 *   - class_len: @p el->class_str must contain the interned class name via
 *                internal_class_list_has().
 *   - id_len:    @p el->id must equal the interned id via memcmp.
 * Returns true only when all present constraints match. An @p anc with all fields
 * zero (universal) always returns true.
 *
 * @param[in] anc   Ancestor constraint part to test.
 * @param[in] el    Element to test the constraint against.
 * @param[in] sheet Sheet owning the name pool for interned class/id names.
 *
 * @return bool Whether @p anc matches @p el.
 * @retval true  All constraints in @p anc are satisfied by @p el.
 * @retval false At least one constraint in @p anc is not satisfied.
 *
 * @pre @p anc, @p el, and @p sheet are non-NULL.
 * @pre Class/id offsets in @p anc are valid indices into @p sheet->names.
 * @post No state is mutated; the function is pure.
 * @post @p anc, @p el, and @p sheet are not modified.
 *
 * @note Thread-safe; pure function with no shared mutable state.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_anc_matches(const ra8_css_anc_t*     anc,
                                 const ra8_css_element_t* el,
                                 const ra8_css_sheet_t*   sheet)
{
  if ((anc->tag != (uint8_t)k_ra8_reflow_tag_unknown) && (el->tag != anc->tag)) {
    return false;
  }
  if (anc->class_len != 0U) {
    const char* nm = (const char*)&sheet->names[anc->class_off];
    if ((el->class_str == nullptr) ||
        !internal_class_list_has(el->class_str, el->class_len, nm, anc->class_len)) {
      return false;
    }
  }
  if (anc->id_len != 0U) {
    const char* nm = (const char*)&sheet->names[anc->id_off];
    if ((el->id == nullptr) || (el->id_len != anc->id_len) ||
        (memcmp(el->id, nm, anc->id_len) != 0)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Full match of a (possibly descendant) rule against @p el + ancestors.
 *
 * @details The subject must match @p el via ra8_css_rule_matches(); when
 * @p rule->anc_count is 0 that check is sufficient. For rules with ancestor
 * constraints, a greedy right-to-left scan walks the ancestor stack from innermost
 * (@p ancestors[n_anc-1]) to outermost (@p ancestors[0]), consuming one rule
 * ancestor part per stack element that matches via internal_anc_matches(). The
 * descendant combinator allows any depth between parts. Returns true only when
 * all ancestor parts are consumed (ai < 0 at loop exit).
 *
 * @param[in] rule      Rule to match (subject + ancestor constraints).
 * @param[in] el        Subject element to match the rule's subject compound.
 * @param[in] ancestors Array of ancestor elements; index 0 is outermost.
 * @param[in] n_anc     Number of elements in @p ancestors.
 * @param[in] sheet     Sheet owning the name pool for interned names.
 *
 * @return bool Whether the rule matches the element in context.
 * @retval true  The rule's subject and all ancestor constraints are satisfied.
 * @retval false The subject does not match @p el, or at least one ancestor
 *               constraint has no matching ancestor in the stack.
 *
 * @pre @p rule, @p el, and @p sheet are non-NULL.
 * @pre @p ancestors points to at least @p n_anc readable elements, or is NULL
 *      when @p n_anc is 0.
 * @post No state is mutated; the function is pure.
 * @post @p rule, @p el, @p ancestors, and @p sheet are not modified.
 *
 * @note Thread-safe; pure function with no shared mutable state.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_rule_matches_ctx(const ra8_css_rule_t*    rule,
                                      const ra8_css_element_t* el,
                                      const ra8_css_element_t* ancestors,
                                      uint8_t                  n_anc,
                                      const ra8_css_sheet_t*   sheet)
{
  if (!ra8_css_rule_matches(rule, el, sheet)) {
    return false;
  }
  if (rule->anc_count == 0U) {
    return true;
  }
  /* Greedy right-to-left: match each ancestor part (innermost first) walking up
   * the stack from the parent. The descendant combinator allows any depth. */
  int32_t ai = (int32_t)rule->anc_count - 1;
  int32_t si = (int32_t)n_anc - 1;
  /* Bounded: si strictly decreases each pass; ends when si < 0 or ai < 0. */
  while ((ai >= 0) && (si >= 0)) {
    if (internal_anc_matches(&rule->anc[ai], &ancestors[si], sheet)) {
      --ai;
    }
    --si;
  }
  return ai < 0;
}

/**
 * @brief Packed specificity (id*10000 + class*100 + type), summing all parts.
 *
 * @details Computes the CSS cascade specificity of @p rule by summing weights
 * for each present constraint across the subject and all ancestor parts:
 *   - k_priv_spec_id (10000) for each non-empty id field.
 *   - k_priv_spec_class (100) for each non-empty class field.
 *   - k_priv_spec_type (1) for each non-unknown tag.
 * The ancestor loop is bounded by @p rule->anc_count. The result is used by
 * internal_resolve() to determine which rule wins during the cascade.
 *
 * @param[in] rule Rule whose specificity to compute.
 *
 * @return uint16_t Packed specificity value.
 * @retval 0 When no type, class, or id constraints are present (universal).
 * @retval >0 The summed specificity per CSS cascade rules.
 *
 * @pre @p rule is non-NULL.
 * @pre @p rule->anc_count is accurate (within bounds of @p rule->anc[]).
 * @post No state is mutated; the function is pure.
 * @post @p rule is not modified.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_rule_rank(const ra8_css_rule_t* rule)
{
  uint16_t spec = 0U;
  if (rule->sel_tag != (uint8_t)k_ra8_reflow_tag_unknown) {
    spec = (uint16_t)(spec + (uint16_t)k_priv_spec_type);
  }
  if (rule->class_len != 0U) {
    spec = (uint16_t)(spec + (uint16_t)k_priv_spec_class);
  }
  if (rule->id_len != 0U) {
    spec = (uint16_t)(spec + (uint16_t)k_priv_spec_id);
  }
  /* Each descendant ancestor part adds to specificity per CSS. */
  for (uint8_t a = 0U; a < rule->anc_count; ++a) {
    if (rule->anc[a].tag != (uint8_t)k_ra8_reflow_tag_unknown) {
      spec = (uint16_t)(spec + (uint16_t)k_priv_spec_type);
    }
    if (rule->anc[a].class_len != 0U) {
      spec = (uint16_t)(spec + (uint16_t)k_priv_spec_class);
    }
    if (rule->anc[a].id_len != 0U) {
      spec = (uint16_t)(spec + (uint16_t)k_priv_spec_id);
    }
  }
  return spec;
}

/**
 * @brief Resolve the winning declaration for one property across all sources.
 *
 * @return Pointer to the winning style (inherited / a rule decl / inline), or
 *         NULL if no source declares @p setbit.
 */
RA8_INTERNAL
static const ra8_css_style_t* internal_resolve(uint8_t                setbit,
                                               const ra8_css_style_t* inherited,
                                               const ra8_css_sheet_t* sheet,
                                               const bool*            matched,
                                               const ra8_css_style_t* inl)
{
  const ra8_css_style_t* win        = nullptr;
  uint16_t               best_rank  = 0U;
  uint16_t               best_order = 0U;
  bool                   have       = false;
  if ((inherited->set & setbit) != 0U) {
    win       = inherited;
    best_rank = (uint16_t)k_priv_rank_inherited;
    have      = true;
  }
  /* Bounded: rule_count <= k_ra8_css_max_rules; i advances by 1 each step. */
  for (uint16_t i = 0U; i < sheet->rule_count; ++i) {
    if (!matched[i] || ((sheet->rules[i].decl.set & setbit) == 0U)) {
      continue;
    }
    const uint16_t rank = internal_rule_rank(&sheet->rules[i]);
    /* mcdc-deactivated: rules[].order is assigned monotonically in source order (internal_push_rule increments next_order) and this loop scans rules in that same order, so a later same-rank rule always has order > best_order; (order >= best_order) is invariantly true and its false arm is unreachable. */
    if ((!have) || (rank > best_rank) ||
        ((rank == best_rank) && (sheet->rules[i].order >= best_order))) {
      win        = &sheet->rules[i].decl;
      best_rank  = rank;
      best_order = sheet->rules[i].order;
      have       = true;
    }
  }
  if ((inl->set & setbit) != 0U) {
    win = inl;
  }
  return win;
}

/**
 * @brief Resolve the bold / italic / underline emphasis bits into @p out.
 *
 * @details Iterates over a fixed table of three emphasis properties (bold,
 * italic, underline) and calls internal_resolve() for each to find the winning
 * style source. When a winner is found, the corresponding set bit is OR'd into
 * @p out->set and the style bit is conditionally OR'd into @p out->style based
 * on the winner's style field. The table has a statically known size of 3,
 * making the loop provably bounded.
 *
 * @param[in,out] out       Style record to write the resolved emphasis bits into.
 * @param[in]     sheet     Sheet providing the matching rules.
 * @param[in]     matched   Boolean array parallel to @p sheet->rules[]; true for
 *                          each rule that matched the current element.
 * @param[in]     inherited Style inherited from the parent element.
 * @param[in]     inl       Inline style from the element's `style` attribute.
 *
 * @return Nothing.
 *
 * @pre @p out, @p sheet, @p matched, @p inherited, and @p inl are non-NULL.
 * @pre @p matched has at least @p sheet->rule_count elements.
 * @post @p out->set and @p out->style have the resolved emphasis bits written.
 * @post @p inherited and @p inl are not modified.
 *
 * @note Thread-safe with respect to distinct @p out instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_cascade_emphasis(ra8_css_style_t*       out,
                                      const ra8_css_sheet_t* sheet,
                                      const bool*            matched,
                                      const ra8_css_style_t* inherited,
                                      const ra8_css_style_t* inl)
{
  static const struct {
    uint8_t setbit;   /**< Setbit.   */
    uint8_t stylebit; /**< Stylebit. */
  } k_props[3] = {
    {(uint8_t)k_ra8_css_set_bold, (uint8_t)k_ra8_reflow_style_bold},
    {(uint8_t)k_ra8_css_set_italic, (uint8_t)k_ra8_reflow_style_italic},
    {(uint8_t)k_ra8_css_set_underline, (uint8_t)k_ra8_reflow_style_underline},
  };
  for (size_t p = 0U; p < (sizeof(k_props) / sizeof(k_props[0])); ++p) {
    const ra8_css_style_t* win =
      internal_resolve(k_props[p].setbit, inherited, sheet, matched, inl);
    if (win != nullptr) {
      out->set = (uint8_t)(out->set | k_props[p].setbit);
      if ((win->style & k_props[p].stylebit) != 0U) {
        out->style = (uint8_t)(out->style | k_props[p].stylebit);
      }
    }
  }
}

/**
 * @brief Resolve the scalar properties (align / colour / font-size / display).
 *
 * @details Calls internal_resolve() for each of the four scalar CSS properties
 * (text-align, color, font-size, display) and, when a winner is found, copies
 * the winning value into @p out and OR's the corresponding set bit. font-size
 * and display are resolved from rules and inline styles only; inheritance for
 * those is handled by the caller to avoid double-application of percentage
 * font-size values.
 *
 * @param[in,out] out       Style record to write the resolved scalar values into.
 * @param[in]     sheet     Sheet providing the matching rules.
 * @param[in]     matched   Boolean array parallel to @p sheet->rules[]; true for
 *                          each rule that matched the current element.
 * @param[in]     inherited Style inherited from the parent element.
 * @param[in]     inl       Inline style from the element's `style` attribute.
 *
 * @return Nothing.
 *
 * @pre @p out, @p sheet, @p matched, @p inherited, and @p inl are non-NULL.
 * @pre @p matched has at least @p sheet->rule_count elements.
 * @post @p out reflects the winning scalar values for all four properties.
 * @post @p inherited and @p inl are not modified.
 *
 * @note Thread-safe with respect to distinct @p out instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_cascade_scalars(ra8_css_style_t*       out,
                                     const ra8_css_sheet_t* sheet,
                                     const bool*            matched,
                                     const ra8_css_style_t* inherited,
                                     const ra8_css_style_t* inl)
{
  const ra8_css_style_t* awin =
    internal_resolve((uint8_t)k_ra8_css_set_align, inherited, sheet, matched, inl);
  if (awin != nullptr) {
    out->set   = (uint8_t)(out->set | (uint8_t)k_ra8_css_set_align);
    out->align = awin->align;
  }
  const ra8_css_style_t* cwin =
    internal_resolve((uint8_t)k_ra8_css_set_color, inherited, sheet, matched, inl);
  if (cwin != nullptr) {
    out->set   = (uint8_t)(out->set | (uint8_t)k_ra8_css_set_color);
    out->color = cwin->color;
  }
  /* font-size + display resolve from rules + inline only -- not inherited via
   * this pure pass (a `%` is applied by the caller against the parent's resolved
   * px, so seeding `inherited` would double-apply it). */
  const ra8_css_style_t* fwin =
    internal_resolve((uint8_t)k_ra8_css_set_fontsize, inherited, sheet, matched, inl);
  if (fwin != nullptr) {
    out->set       = (uint8_t)(out->set | (uint8_t)k_ra8_css_set_fontsize);
    out->font_val  = fwin->font_val;
    out->font_unit = fwin->font_unit;
  }
  const ra8_css_style_t* dwin =
    internal_resolve((uint8_t)k_ra8_css_set_display, inherited, sheet, matched, inl);
  if (dwin != nullptr) {
    out->set     = (uint8_t)(out->set | (uint8_t)k_ra8_css_set_display);
    out->display = dwin->display;
  }
}

/**
 * @brief Resolve the inherited `font-family` slice into @p out.
 *
 * @details Calls internal_resolve() for the k_ra8_css_set_family property and, when
 * a winning style is found, copies (family_off, family_len) from the winner into
 * @p out and OR's k_ra8_css_set_family into @p out->set. The family name itself
 * lives in the sheet's name pool and is referenced by offset and length; no
 * string copy is performed.
 *
 * @param[in,out] out       Style record to write the resolved family reference into.
 * @param[in]     sheet     Sheet providing the matching rules and name pool.
 * @param[in]     matched   Boolean array parallel to @p sheet->rules[]; true for
 *                          each rule that matched the current element.
 * @param[in]     inherited Style inherited from the parent element.
 * @param[in]     inl       Inline style from the element's `style` attribute.
 *
 * @return Nothing.
 *
 * @pre @p out, @p sheet, @p matched, @p inherited, and @p inl are non-NULL.
 * @pre @p matched has at least @p sheet->rule_count elements.
 * @post When a winning font-family source exists, @p out->family_off and
 *       @p out->family_len are set and k_ra8_css_set_family is OR'd into @p out->set.
 * @post @p inherited and @p inl are not modified.
 *
 * @note Thread-safe with respect to distinct @p out instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_cascade_family(ra8_css_style_t*       out,
                                    const ra8_css_sheet_t* sheet,
                                    const bool*            matched,
                                    const ra8_css_style_t* inherited,
                                    const ra8_css_style_t* inl)
{
  const ra8_css_style_t* win =
    internal_resolve((uint8_t)k_ra8_css_set_family, inherited, sheet, matched, inl);
  if (win != nullptr) {
    out->set        = (uint8_t)(out->set | (uint8_t)k_ra8_css_set_family);
    out->family_off = win->family_off;
    out->family_len = win->family_len;
  }
}

ra8_css_style_t ra8_css_cascade_ctx(const ra8_css_sheet_t*   sheet,
                                    const ra8_css_element_t* el,
                                    ra8_css_style_t          inherited,
                                    ra8_css_style_t          inline_decl,
                                    const ra8_css_element_t* ancestors,
                                    uint8_t                  n_anc)
{
  if ((sheet == nullptr) || (el == nullptr)) {
    return inherited;
  }
  bool matched[k_ra8_css_max_rules] = {};
  /* Bounded: rule_count <= k_ra8_css_max_rules; i advances by 1 each step. */
  for (uint16_t i = 0U; i < sheet->rule_count; ++i) {
    matched[i] = internal_rule_matches_ctx(&sheet->rules[i], el, ancestors, n_anc, sheet);
  }
  ra8_css_style_t out = {};
  internal_cascade_emphasis(&out, sheet, matched, &inherited, &inline_decl);
  internal_cascade_scalars(&out, sheet, matched, &inherited, &inline_decl);
  internal_cascade_family(&out, sheet, matched, &inherited, &inline_decl);
  return out;
}

ra8_css_style_t ra8_css_cascade(const ra8_css_sheet_t*   sheet,
                                const ra8_css_element_t* el,
                                ra8_css_style_t          inherited,
                                ra8_css_style_t          inline_decl)
{
  return ra8_css_cascade_ctx(sheet, el, inherited, inline_decl, nullptr, 0U);
}

/**
 * @brief Case-insensitive equality of two byte spans.
 *
 * @details Compares @p a[0..alen) and @p b[0..blen) byte-by-byte after
 * ASCII case-folding via priv_ra8_reflow_css_lower(). Returns false immediately when either
 * pointer is NULL or the lengths differ. The comparison loop is bounded by
 * @p alen (which equals @p blen when lengths match).
 *
 * @param[in] a    First byte span (need not be NUL-terminated).
 * @param[in] alen Length of @p a in bytes.
 * @param[in] b    Second byte span (need not be NUL-terminated).
 * @param[in] blen Length of @p b in bytes.
 *
 * @return bool Comparison result.
 * @retval true  Both spans have equal length and compare equal case-insensitively.
 * @retval false Either pointer is NULL, lengths differ, or any byte differs.
 *
 * @pre @p a points to at least @p alen readable bytes, or is NULL.
 * @pre @p b points to at least @p blen readable bytes, or is NULL.
 * @post No state is mutated; the function is pure.
 * @post @p a and @p b are not modified.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_ci_eq_span(const char* a, size_t alen, const char* b, size_t blen)
{
  /* mcdc-deactivated: internal_ci_eq_span is reached only via internal_family_eq from ra8_css_match_face, which rejects a NULL sheet and a NULL family at its entry, so both name pointers are non-NULL here; (a == nullptr) and (b == nullptr) are unreachable, only (alen != blen) varies. */
  if ((a == nullptr) || (b == nullptr) || (alen != blen)) {
    return false;
  }
  /* Bounded: k < alen (== blen); one byte folded per step. */
  for (size_t k = 0U; k < alen; ++k) {
    if (priv_ra8_reflow_css_lower(a[k]) != priv_ra8_reflow_css_lower(b[k])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief True iff face @p f's family equals @p family (case-insensitive).
 *
 * @details Retrieves the interned family name from @p sheet->names using
 * @p f->family_off and @p f->family_len, then delegates to internal_ci_eq_span()
 * for a case-insensitive comparison against @p family[0..family_len).
 *
 * @param[in] sheet      Sheet owning the name pool referenced by @p f.
 * @param[in] f          Font face whose interned family name to compare.
 * @param[in] family     Byte span of the family name to match.
 * @param[in] family_len Number of bytes in @p family.
 *
 * @return bool Comparison result.
 * @retval true  @p f's interned family name equals @p family case-insensitively.
 * @retval false The names differ in length or any byte differs.
 *
 * @pre @p sheet and @p f are non-NULL.
 * @pre @p f->family_off + @p f->family_len <= @p sheet->names_used.
 * @post No state is mutated; the function is pure relative to @p sheet.
 * @post @p family is not modified.
 *
 * @note Thread-safe; no shared mutable state.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_family_eq(const ra8_css_sheet_t*    sheet,
                               const ra8_css_fontface_t* f,
                               const char*               family,
                               size_t                    family_len)
{
  return internal_ci_eq_span((const char*)&sheet->names[f->family_off],
                             (size_t)f->family_len,
                             family,
                             family_len);
}

int16_t ra8_css_match_face(const ra8_css_sheet_t* sheet,
                           const char*            family,
                           uint16_t               family_len,
                           bool                   want_bold,
                           bool                   want_italic)
{
  if ((sheet == nullptr) || (family == nullptr) || (family_len == 0U)) {
    return (int16_t)k_ra8_css_no_face;
  }
  int16_t fallback = (int16_t)k_ra8_css_no_face;
  /* Bounded: face_count <= k_ra8_css_max_faces; i advances by 1 each step. */
  for (uint16_t i = 0U; i < sheet->face_count; ++i) {
    const ra8_css_fontface_t* f = &sheet->faces[i];
    if (!internal_family_eq(sheet, f, family, (size_t)family_len)) {
      continue;
    }
    if (((f->weight_bold != 0U) == want_bold) && ((f->style_italic != 0U) == want_italic)) {
      return (int16_t)i;
    }
    if ((f->weight_bold == 0U) && (f->style_italic == 0U) && (fallback < 0)) {
      fallback = (int16_t)i;
    }
  }
  return fallback;
}

bool ra8_css_face_src(const ra8_css_sheet_t* sheet,
                      uint16_t               idx,
                      const char**           out_src,
                      uint16_t*              out_len)
{
  if ((sheet == nullptr) || (out_src == nullptr) || (out_len == nullptr) ||
      (idx >= sheet->face_count)) {
    return false;
  }
  *out_src = (const char*)&sheet->names[sheet->faces[idx].src_off];
  *out_len = sheet->faces[idx].src_len;
  return true;
}
