/**
 * @file ra_reflow_css.c
 * @brief Implementation of the minimal content-CSS cascade (#111).
 *
 * @details
 * Pure, zero-allocation parser + matcher + cascade for the v1 CSS subset
 * documented in `ra_reflow_css.h`. No MMIO, no heap; every buffer is a
 * fixed-size field of the caller-owned ::ra_css_sheet_t.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include "ra_reflow_css.h"

#include <stddef.h>
#include <string.h>

#include "ra_reflow.h"                   /* ra_reflow_html_tag_t, style/align enums */
#include "ra_reflow_tokenize_internal.h" /* ra_reflow_tok_classify */

/* ===========================================================================
 * Internal constants (no magic numbers)
 * ===========================================================================
 */

/**
 * @enum priv_css_consts_t
 * @brief Cascade specificity ranks (inheritance lowest, id highest).
 */
typedef enum : uint16_t {
  k_priv_rank_inherited = 0U, /**< Inheritance is the lowest priority. */
  k_priv_rank_universal = 1U, /**< `*` selector rank.                  */
  k_priv_rank_type      = 2U, /**< `tag` selector rank.                */
  k_priv_rank_class     = 3U, /**< `.class` selector rank.             */
  k_priv_rank_id        = 4U, /**< `#id` selector rank.                */
} priv_css_consts_t;

/**
 * @enum priv_css_hex_t
 * @brief Hex-colour parsing constants (`#rgb` / `#rrggbb`).
 */
typedef enum : uint8_t {
  k_priv_hex_base = 16U, /**< Base + "not a hex digit" sentinel. */
  k_priv_hex_a10  = 10U, /**< Value of hex 'a'/'A'.             */
  k_priv_hex3_len = 3U,  /**< `#rgb` short-form digit count.    */
  k_priv_hex6_len = 6U,  /**< `#rrggbb` digit count.            */
  k_priv_hex_nib  = 4U,  /**< Bits per hex nibble.              */
  k_priv_hex_chan = 8U,  /**< Bits per colour channel.          */
} priv_css_hex_t;

/**
 * @enum priv_css_fs_t
 * @brief `font-size` parsing constants (decimal + unit scaling).
 */
typedef enum : uint16_t {
  k_priv_fs_dec  = 10U,   /**< Decimal base.                          */
  k_priv_fs_pct1 = 100U,  /**< Hundredths scale; 1em in percent.      */
  k_priv_fs_frac = 2U,    /**< Fractional digits kept.                */
  k_priv_fs_max  = 9999U, /**< Clamp for a parsed font-size number.   */
  k_priv_fs_ulen = 2U,    /**< Length of the "px" / "em" unit suffix. */
} priv_css_fs_t;

/**
 * @enum priv_css_color_t
 * @brief Common named CSS colours + the "not a colour" sentinel.
 */
typedef enum : uint32_t {
  k_priv_col_black   = 0x000000U,   /**< CSS `black`.                       */
  k_priv_col_white   = 0xFFFFFFU,   /**< CSS `white`.                       */
  k_priv_col_red     = 0xFF0000U,   /**< CSS `red`.                         */
  k_priv_col_green   = 0x008000U,   /**< CSS `green`.                       */
  k_priv_col_blue    = 0x0000FFU,   /**< CSS `blue`.                        */
  k_priv_col_gray    = 0x808080U,   /**< CSS `gray` / `grey`.               */
  k_priv_col_silver  = 0xC0C0C0U,   /**< CSS `silver`.                      */
  k_priv_col_maroon  = 0x800000U,   /**< CSS `maroon`.                      */
  k_priv_col_navy    = 0x000080U,   /**< CSS `navy`.                        */
  k_priv_col_invalid = 0xFFFFFFFFU, /**< Sentinel: not a parseable colour. */
} priv_css_color_t;

/* ===========================================================================
 * Small pure string helpers
 * ===========================================================================
 */

/** @brief True for CSS whitespace (space / tab / newline / CR / FF). */
static bool priv_is_ws(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r') || (c == '\f');
}

/** @brief ASCII-fold one byte to lower case. */
static char priv_lower(char c)
{
  return ((c >= 'A') && (c <= 'Z')) ? (char)(c + ('a' - 'A')) : c;
}

/** @brief Case-insensitive compare of span @p s[0..len) against NUL literal @p lit. */
static bool priv_ci_eq(const char* s, size_t len, const char* lit)
{
  if ((s == nullptr) || (lit == nullptr)) {
    return false;
  }
  size_t k = 0U;
  for (; (k < len) && (lit[k] != '\0'); ++k) {
    if (priv_lower(s[k]) != priv_lower(lit[k])) {
      return false;
    }
  }
  return (k == len) && (lit[k] == '\0');
}

/** @brief Case-insensitive "span contains substring @p sub" (bounded scan). */
static bool priv_ci_contains(const char* s, size_t len, const char* sub)
{
  const size_t sl = strlen(sub);
  if ((s == nullptr) || (sl == 0U) || (sl > len)) {
    return false;
  }
  /* Bounded: at most (len - sl + 1) windows; i advances by 1 each step. */
  for (size_t i = 0U; (i + sl) <= len; ++i) {
    bool hit = true;
    /* Bounded: j < sl, the NUL-terminated literal's strlen. */
    for (size_t j = 0U; j < sl; ++j) {
      if (priv_lower(s[i + j]) != priv_lower(sub[j])) {
        hit = false;
        break;
      }
    }
    if (hit) {
      return true;
    }
  }
  return false;
}

/** @brief Trim leading + trailing whitespace from @p s[0..*len); returns start. */
static const char* priv_trim(const char* s, size_t* len)
{
  size_t a = 0U;
  size_t b = *len;
  while ((a < b) && priv_is_ws(s[a])) {
    ++a;
  }
  while ((b > a) && priv_is_ws(s[b - 1U])) {
    --b;
  }
  *len = b - a;
  return &s[a];
}

/* ===========================================================================
 * Declaration-body parsing (shared by rules + inline)
 * ===========================================================================
 */

/** @brief Hex value of one ASCII digit, or k_priv_hex_base if not a hex digit. */
static uint8_t priv_hex_val(char c)
{
  if ((c >= '0') && (c <= '9')) {
    return (uint8_t)(c - '0');
  }
  const char l = priv_lower(c);
  if ((l >= 'a') && (l <= 'f')) {
    return (uint8_t)((l - 'a') + (int)k_priv_hex_a10);
  }
  return (uint8_t)k_priv_hex_base;
}

/** @brief Parse `rgb` / `rrggbb` hex digits (no `#`) to 0xRRGGBB, or invalid. */
static uint32_t priv_parse_hex_color(const char* s, size_t len)
{
  uint32_t rgb = 0U;
  if (len == (size_t)k_priv_hex6_len) {
    /* Bounded: exactly k_priv_hex6_len digits. */
    for (size_t i = 0U; i < len; ++i) {
      const uint8_t v = priv_hex_val(s[i]);
      if (v >= (uint8_t)k_priv_hex_base) {
        return (uint32_t)k_priv_col_invalid;
      }
      rgb = (rgb << (uint32_t)k_priv_hex_nib) | (uint32_t)v;
    }
    return rgb;
  }
  if (len == (size_t)k_priv_hex3_len) {
    /* Bounded: exactly k_priv_hex3_len digits; each expands d -> dd. */
    for (size_t i = 0U; i < len; ++i) {
      const uint8_t v = priv_hex_val(s[i]);
      if (v >= (uint8_t)k_priv_hex_base) {
        return (uint32_t)k_priv_col_invalid;
      }
      rgb = (rgb << (uint32_t)k_priv_hex_chan) | ((uint32_t)v << (uint32_t)k_priv_hex_nib) |
            (uint32_t)v;
    }
    return rgb;
  }
  return (uint32_t)k_priv_col_invalid;
}

/** @brief Parse a CSS colour value (`#rgb` / `#rrggbb` / a named keyword). */
static uint32_t priv_parse_color(const char* s, size_t len)
{
  if ((s == nullptr) || (len == 0U)) {
    return (uint32_t)k_priv_col_invalid;
  }
  if (s[0] == '#') {
    return priv_parse_hex_color(&s[1], len - 1U);
  }
  if (priv_ci_eq(s, len, "black")) {
    return (uint32_t)k_priv_col_black;
  }
  if (priv_ci_eq(s, len, "white")) {
    return (uint32_t)k_priv_col_white;
  }
  if (priv_ci_eq(s, len, "red")) {
    return (uint32_t)k_priv_col_red;
  }
  if (priv_ci_eq(s, len, "green")) {
    return (uint32_t)k_priv_col_green;
  }
  if (priv_ci_eq(s, len, "blue")) {
    return (uint32_t)k_priv_col_blue;
  }
  if (priv_ci_eq(s, len, "gray") || priv_ci_eq(s, len, "grey")) {
    return (uint32_t)k_priv_col_gray;
  }
  if (priv_ci_eq(s, len, "silver")) {
    return (uint32_t)k_priv_col_silver;
  }
  if (priv_ci_eq(s, len, "maroon")) {
    return (uint32_t)k_priv_col_maroon;
  }
  if (priv_ci_eq(s, len, "navy")) {
    return (uint32_t)k_priv_col_navy;
  }
  return (uint32_t)k_priv_col_invalid;
}

/** @brief Scan a decimal number into hundredths (e.g. "1.2" -> 120); advance @p i. */
static uint32_t priv_scan_hundredths(const char* s, size_t len, size_t* i, bool* any)
{
  uint32_t hund = 0U;
  /* Bounded: integer digits, i advances by 1 each step, capped by len. */
  while ((*i < len) && (s[*i] >= '0') && (s[*i] <= '9')) {
    hund = (hund * (uint32_t)k_priv_fs_dec) + (uint32_t)(s[*i] - '0');
    *any = true;
    ++(*i);
  }
  hund *= (uint32_t)k_priv_fs_pct1; /* integer part -> hundredths */
  if ((*i < len) && (s[*i] == '.')) {
    ++(*i);
    uint32_t place = (uint32_t)k_priv_fs_pct1 / (uint32_t)k_priv_fs_dec; /* tenths */
    size_t   fd    = 0U;
    /* Bounded: at most k_priv_fs_frac fractional digits. */
    while ((*i < len) && (s[*i] >= '0') && (s[*i] <= '9') && (fd < (size_t)k_priv_fs_frac)) {
      hund += (uint32_t)(s[*i] - '0') * place;
      place /= (uint32_t)k_priv_fs_dec;
      *any = true;
      ++fd;
      ++(*i);
    }
    /* Bounded: skip any remaining fractional digits, i capped by len. */
    while ((*i < len) && (s[*i] >= '0') && (s[*i] <= '9')) {
      ++(*i);
    }
  }
  return hund;
}

/** @brief Clamp a hundredths value to a whole number in [0, k_priv_fs_max]. */
static uint16_t priv_fs_whole(uint32_t hund)
{
  const uint32_t whole = hund / (uint32_t)k_priv_fs_pct1;
  return (uint16_t)((whole > (uint32_t)k_priv_fs_max) ? (uint32_t)k_priv_fs_max : whole);
}

/** @brief Parse `Npx` / `N%` / `Nem` (N may be fractional) into value + unit. */
static bool priv_parse_fontsize(const char* s, size_t len, uint16_t* out_val, uint8_t* out_unit)
{
  size_t   i    = 0U;
  bool     any  = false;
  uint32_t hund = priv_scan_hundredths(s, len, &i, &any);
  if (!any) {
    return false;
  }
  const size_t rem = len - i;
  if ((rem == (size_t)k_priv_fs_ulen) && priv_ci_eq(&s[i], rem, "px")) {
    *out_val  = priv_fs_whole(hund);
    *out_unit = (uint8_t)k_ra_css_font_px;
    return true;
  }
  if ((rem == 1U) && (s[i] == '%')) {
    *out_val  = priv_fs_whole(hund);
    *out_unit = (uint8_t)k_ra_css_font_pct;
    return true;
  }
  if ((rem == (size_t)k_priv_fs_ulen) && priv_ci_eq(&s[i], rem, "em")) {
    /* 1em = 100%; `hund` is value*100, which is already the percent. */
    *out_val  = (uint16_t)((hund > (uint32_t)k_priv_fs_max) ? (uint32_t)k_priv_fs_max : hund);
    *out_unit = (uint8_t)k_ra_css_font_pct;
    return true;
  }
  return false;
}

/** @brief Apply one trimmed `prop:value` pair to @p out (sets a `set` bit). */
/** @brief Set / clear @p stylebit in @p out, marking @p setbit present. */
static void priv_set_emphasis(ra_css_style_t* out, uint8_t setbit, uint8_t stylebit, bool on)
{
  out->set   = (uint8_t)(out->set | setbit);
  out->style = (uint8_t)(on ? (out->style | stylebit) : (out->style & (uint8_t)~stylebit));
}

/** @brief Apply a boolean-emphasis property (font-weight/style/decoration); false if other. */
static bool priv_apply_emphasis(const char*     prop,
                                size_t          plen,
                                const char*     val,
                                size_t          vlen,
                                ra_css_style_t* out)
{
  if (priv_ci_eq(prop, plen, "font-weight")) {
    const bool on = priv_ci_eq(val, vlen, "bold") || priv_ci_eq(val, vlen, "bolder") ||
                    priv_ci_eq(val, vlen, "600") || priv_ci_eq(val, vlen, "700") ||
                    priv_ci_eq(val, vlen, "800") || priv_ci_eq(val, vlen, "900");
    priv_set_emphasis(out, (uint8_t)k_ra_css_set_bold, (uint8_t)k_ra_reflow_style_bold, on);
    return true;
  }
  if (priv_ci_eq(prop, plen, "font-style")) {
    const bool on = priv_ci_eq(val, vlen, "italic") || priv_ci_eq(val, vlen, "oblique");
    priv_set_emphasis(out, (uint8_t)k_ra_css_set_italic, (uint8_t)k_ra_reflow_style_italic, on);
    return true;
  }
  if (priv_ci_eq(prop, plen, "text-decoration") || priv_ci_eq(prop, plen, "text-decoration-line")) {
    const bool on = priv_ci_contains(val, vlen, "underline");
    priv_set_emphasis(out,
                      (uint8_t)k_ra_css_set_underline,
                      (uint8_t)k_ra_reflow_style_underline,
                      on);
    return true;
  }
  return false;
}

/** @brief Apply a parsed `text-align` value to @p out. */
static void priv_apply_align(const char* val, size_t vlen, ra_css_style_t* out)
{
  out->set = (uint8_t)(out->set | (uint8_t)k_ra_css_set_align);
  if (priv_ci_eq(val, vlen, "right")) {
    out->align = (uint8_t)k_ra_reflow_align_right;
  } else if (priv_ci_eq(val, vlen, "center")) {
    out->align = (uint8_t)k_ra_reflow_align_center;
  } else if (priv_ci_eq(val, vlen, "justify")) {
    out->align = (uint8_t)k_ra_reflow_align_justify;
  } else {
    out->align = (uint8_t)k_ra_reflow_align_left;
  }
}

static void
priv_apply_decl(const char* prop, size_t plen, const char* val, size_t vlen, ra_css_style_t* out)
{
  if (priv_apply_emphasis(prop, plen, val, vlen, out)) {
    return;
  }
  if (priv_ci_eq(prop, plen, "text-align")) {
    priv_apply_align(val, vlen, out);
  } else if (priv_ci_eq(prop, plen, "color")) {
    const uint32_t rgb = priv_parse_color(val, vlen);
    if (rgb != (uint32_t)k_priv_col_invalid) {
      out->set   = (uint8_t)(out->set | (uint8_t)k_ra_css_set_color);
      out->color = rgb;
    }
  } else if (priv_ci_eq(prop, plen, "font-size")) {
    uint16_t fv = 0U;
    uint8_t  fu = 0U;
    if (priv_parse_fontsize(val, vlen, &fv, &fu)) {
      out->set       = (uint8_t)(out->set | (uint8_t)k_ra_css_set_fontsize);
      out->font_val  = fv;
      out->font_unit = fu;
    }
  } else if (priv_ci_eq(prop, plen, "display")) {
    out->set     = (uint8_t)(out->set | (uint8_t)k_ra_css_set_display);
    out->display = priv_ci_eq(val, vlen, "none") ? 1U : 0U;
  } else {
    /* Unknown property -> ignore (no set bit). */
  }
}

/** @brief Parse a `prop:value; ...` declaration body (no braces) into @p out. */
static void priv_parse_decls(const char* s, size_t len, ra_css_style_t* out)
{
  size_t i = 0U;
  /* Bounded: each iteration consumes at least one byte via the ';' advance. */
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
      const char* prop = priv_trim(&s[i], &plen);
      size_t      vlen = semi - (colon + 1U);
      const char* val  = priv_trim(&s[colon + 1U], &vlen);
      if ((plen > 0U) && (vlen > 0U)) {
        priv_apply_decl(prop, plen, val, vlen, out);
      }
    }
    i = semi + 1U;
  }
}

/* ===========================================================================
 * Selector parsing
 * ===========================================================================
 */

/** @brief True if @p c can appear inside a bare type / class / id name. */
static bool priv_is_name_char(char c)
{
  return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) ||
         (c == '-') || (c == '_');
}

/** @brief Intern a class/id name into the sheet pool; false if it does not fit. */
static bool priv_intern_name(ra_css_sheet_t* sheet, const char* s, size_t len, uint16_t* off)
{
  if ((len == 0U) || (len > (size_t)k_ra_css_name_max)) {
    return false;
  }
  if (((size_t)sheet->names_used + len) > (size_t)k_ra_css_name_pool) {
    return false;
  }
  *off = sheet->names_used;
  (void)memcpy(&sheet->names[sheet->names_used], s, len);
  sheet->names_used = (uint16_t)(sheet->names_used + len);
  return true;
}

/**
 * @brief Parse ONE trimmed simple selector into @p rule; false if unsupported.
 *
 * @details Accepts `*`, `tag`, `.class`, `#id` only. Compound (`p.x`),
 * descendant (`a b`) and pseudo selectors fail (the caller drops the rule).
 */
static bool
priv_parse_selector(ra_css_sheet_t* sheet, const char* s, size_t len, ra_css_rule_t* rule)
{
  if (len == 0U) {
    return false;
  }
  if ((len == 1U) && (s[0] == '*')) {
    rule->sel_kind = (uint8_t)k_ra_css_sel_universal;
    return true;
  }
  const char first = s[0];
  if ((first == '.') || (first == '#')) {
    /* Class / id: the remainder must be a single bare name. */
    for (size_t k = 1U; k < len; ++k) {
      if (!priv_is_name_char(s[k])) {
        return false;
      }
    }
    uint16_t off = 0U;
    if (!priv_intern_name(sheet, &s[1], len - 1U, &off)) {
      return false;
    }
    rule->sel_kind = (uint8_t)((first == '.') ? k_ra_css_sel_class : k_ra_css_sel_id);
    rule->name_off = off;
    rule->name_len = (uint16_t)(len - 1U);
    return true;
  }
  /* Type selector: every byte must be a bare name char (rejects compounds). */
  for (size_t k = 0U; k < len; ++k) {
    if (!priv_is_name_char(s[k])) {
      return false;
    }
  }
  const ra_reflow_html_tag_t tag = ra_reflow_tok_classify(s, len);
  if (tag == k_ra_reflow_tag_unknown) {
    return false; /* cannot target an unrecognised tag reliably */
  }
  rule->sel_kind = (uint8_t)k_ra_css_sel_type;
  rule->sel_tag  = (uint8_t)tag;
  return true;
}

/** @brief Append one fully-built rule to the sheet; drop silently if full. */
static void priv_push_rule(ra_css_sheet_t* sheet, const ra_css_rule_t* rule)
{
  if (sheet->rule_count >= (uint16_t)k_ra_css_max_rules) {
    return;
  }
  sheet->rules[sheet->rule_count]       = *rule;
  sheet->rules[sheet->rule_count].order = sheet->next_order;
  sheet->rule_count                     = (uint16_t)(sheet->rule_count + 1U);
  sheet->next_order                     = (uint16_t)(sheet->next_order + 1U);
}

/** @brief Parse a comma-grouped selector list sharing declaration @p decl. */
static void priv_parse_selector_list(ra_css_sheet_t* sheet,
                                     const char*     sel,
                                     size_t          sel_len,
                                     ra_css_style_t  decl)
{
  size_t i = 0U;
  while (i < sel_len) {
    size_t comma = i;
    while ((comma < sel_len) && (sel[comma] != ',')) {
      ++comma;
    }
    size_t        one_len = comma - i;
    const char*   one     = priv_trim(&sel[i], &one_len);
    ra_css_rule_t rule    = {};
    rule.decl             = decl;
    if (priv_parse_selector(sheet, one, one_len, &rule)) {
      priv_push_rule(sheet, &rule);
    }
    i = comma + 1U;
  }
}

/* ===========================================================================
 * Public API
 * ===========================================================================
 */

ra_err_t ra_css_sheet_reset(ra_css_sheet_t* sheet)
{
  if (sheet == nullptr) {
    return k_ra_err_null_ptr;
  }
  sheet->rule_count = 0U;
  sheet->next_order = 0U;
  sheet->names_used = 0U;
  sheet->pad        = 0U;
  return k_ra_ok;
}

ra_err_t ra_css_parse(ra_css_sheet_t* sheet, const char* css, uint32_t len)
{
  if ((sheet == nullptr) || (css == nullptr)) {
    return k_ra_err_null_ptr;
  }
  size_t i = 0U;
  /* Bounded: each iteration advances past a `{...}` block or breaks at EOF. */
  while (i < (size_t)len) {
    /* Skip a leading comment or whitespace before the selector. */
    if (((i + 1U) < (size_t)len) && (css[i] == '/') && (css[i + 1U] == '*')) {
      size_t j = i + 2U;
      while (((j + 1U) < (size_t)len) && !((css[j] == '*') && (css[j + 1U] == '/'))) {
        ++j;
      }
      i = (j + 2U <= (size_t)len) ? (j + 2U) : (size_t)len;
      continue;
    }
    if (priv_is_ws(css[i])) {
      ++i;
      continue;
    }
    /* Selector list runs up to '{'. */
    size_t brace = i;
    while ((brace < (size_t)len) && (css[brace] != '{')) {
      ++brace;
    }
    if (brace >= (size_t)len) {
      break; /* no block -> done */
    }
    size_t close = brace + 1U;
    while ((close < (size_t)len) && (css[close] != '}')) {
      ++close;
    }
    ra_css_style_t decl = {};
    priv_parse_decls(&css[brace + 1U], close - (brace + 1U), &decl);
    priv_parse_selector_list(sheet, &css[i], brace - i, decl);
    i = (close < (size_t)len) ? (close + 1U) : (size_t)len;
  }
  return k_ra_ok;
}

ra_err_t ra_css_parse_inline(const char* decls, uint32_t len, ra_css_style_t* out)
{
  if ((decls == nullptr) || (out == nullptr)) {
    return k_ra_err_null_ptr;
  }
  *out = (ra_css_style_t){};
  priv_parse_decls(decls, (size_t)len, out);
  return k_ra_ok;
}

/** @brief True iff a space-separated class list contains @p name exactly. */
static bool priv_class_list_has(const char* list, size_t list_len, const char* name, size_t nlen)
{
  size_t i = 0U;
  /* Bounded: each pass skips >=0 ws then a token, advancing i to list_len. */
  while (i < list_len) {
    while ((i < list_len) && priv_is_ws(list[i])) {
      ++i;
    }
    size_t start = i;
    while ((i < list_len) && !priv_is_ws(list[i])) {
      ++i;
    }
    const size_t tlen = i - start;
    if ((tlen == nlen) && (memcmp(&list[start], name, nlen) == 0)) {
      return true;
    }
  }
  return false;
}

bool ra_css_rule_matches(const ra_css_rule_t*    rule,
                         const ra_css_element_t* el,
                         const ra_css_sheet_t*   sheet)
{
  if ((rule == nullptr) || (el == nullptr) || (sheet == nullptr)) {
    return false;
  }
  switch ((ra_css_sel_kind_t)rule->sel_kind) {
    case k_ra_css_sel_universal:
      return true;
    case k_ra_css_sel_type:
      return el->tag == rule->sel_tag;
    case k_ra_css_sel_id: {
      const char* nm = (const char*)&sheet->names[rule->name_off];
      return (el->id != nullptr) && (el->id_len == rule->name_len) &&
             (memcmp(el->id, nm, rule->name_len) == 0);
    }
    case k_ra_css_sel_class: {
      const char* nm = (const char*)&sheet->names[rule->name_off];
      return (el->class_str != nullptr) &&
             priv_class_list_has(el->class_str, el->class_len, nm, rule->name_len);
    }
    default:
      return false;
  }
}

/** @brief Specificity rank (+1 over inheritance) for a rule's selector kind. */
static uint16_t priv_rule_rank(uint8_t sel_kind)
{
  switch ((ra_css_sel_kind_t)sel_kind) {
    case k_ra_css_sel_type:
      return (uint16_t)k_priv_rank_type;
    case k_ra_css_sel_class:
      return (uint16_t)k_priv_rank_class;
    case k_ra_css_sel_id:
      return (uint16_t)k_priv_rank_id;
    case k_ra_css_sel_universal:
    default:
      return (uint16_t)k_priv_rank_universal;
  }
}

/**
 * @brief Resolve the winning declaration for one property across all sources.
 *
 * @return Pointer to the winning style (inherited / a rule decl / inline), or
 *         NULL if no source declares @p setbit.
 */
static const ra_css_style_t* priv_resolve(uint8_t               setbit,
                                          const ra_css_style_t* inherited,
                                          const ra_css_sheet_t* sheet,
                                          const bool*           matched,
                                          const ra_css_style_t* inl)
{
  const ra_css_style_t* win        = nullptr;
  uint16_t              best_rank  = 0U;
  uint16_t              best_order = 0U;
  bool                  have       = false;
  if ((inherited->set & setbit) != 0U) {
    win       = inherited;
    best_rank = (uint16_t)k_priv_rank_inherited;
    have      = true;
  }
  /* Bounded: rule_count <= k_ra_css_max_rules; i advances by 1 each step. */
  for (uint16_t i = 0U; i < sheet->rule_count; ++i) {
    if (!matched[i] || ((sheet->rules[i].decl.set & setbit) == 0U)) {
      continue;
    }
    const uint16_t rank = priv_rule_rank(sheet->rules[i].sel_kind);
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

/** @brief Resolve the bold / italic / underline emphasis bits into @p out. */
static void priv_cascade_emphasis(ra_css_style_t*       out,
                                  const ra_css_sheet_t* sheet,
                                  const bool*           matched,
                                  const ra_css_style_t* inherited,
                                  const ra_css_style_t* inl)
{
  static const struct {
    uint8_t setbit;
    uint8_t stylebit;
  } k_props[3] = {
    {(uint8_t)k_ra_css_set_bold, (uint8_t)k_ra_reflow_style_bold},
    {(uint8_t)k_ra_css_set_italic, (uint8_t)k_ra_reflow_style_italic},
    {(uint8_t)k_ra_css_set_underline, (uint8_t)k_ra_reflow_style_underline},
  };
  for (size_t p = 0U; p < (sizeof(k_props) / sizeof(k_props[0])); ++p) {
    const ra_css_style_t* win = priv_resolve(k_props[p].setbit, inherited, sheet, matched, inl);
    if (win != nullptr) {
      out->set = (uint8_t)(out->set | k_props[p].setbit);
      if ((win->style & k_props[p].stylebit) != 0U) {
        out->style = (uint8_t)(out->style | k_props[p].stylebit);
      }
    }
  }
}

/** @brief Resolve the scalar properties (align / colour / font-size / display). */
static void priv_cascade_scalars(ra_css_style_t*       out,
                                 const ra_css_sheet_t* sheet,
                                 const bool*           matched,
                                 const ra_css_style_t* inherited,
                                 const ra_css_style_t* inl)
{
  const ra_css_style_t* awin =
    priv_resolve((uint8_t)k_ra_css_set_align, inherited, sheet, matched, inl);
  if (awin != nullptr) {
    out->set   = (uint8_t)(out->set | (uint8_t)k_ra_css_set_align);
    out->align = awin->align;
  }
  const ra_css_style_t* cwin =
    priv_resolve((uint8_t)k_ra_css_set_color, inherited, sheet, matched, inl);
  if (cwin != nullptr) {
    out->set   = (uint8_t)(out->set | (uint8_t)k_ra_css_set_color);
    out->color = cwin->color;
  }
  /* font-size + display resolve from rules + inline only -- not inherited via
   * this pure pass (a `%` is applied by the caller against the parent's resolved
   * px, so seeding `inherited` would double-apply it). */
  const ra_css_style_t* fwin =
    priv_resolve((uint8_t)k_ra_css_set_fontsize, inherited, sheet, matched, inl);
  if (fwin != nullptr) {
    out->set       = (uint8_t)(out->set | (uint8_t)k_ra_css_set_fontsize);
    out->font_val  = fwin->font_val;
    out->font_unit = fwin->font_unit;
  }
  const ra_css_style_t* dwin =
    priv_resolve((uint8_t)k_ra_css_set_display, inherited, sheet, matched, inl);
  if (dwin != nullptr) {
    out->set     = (uint8_t)(out->set | (uint8_t)k_ra_css_set_display);
    out->display = dwin->display;
  }
}

ra_css_style_t ra_css_cascade(const ra_css_sheet_t*   sheet,
                              const ra_css_element_t* el,
                              ra_css_style_t          inherited,
                              ra_css_style_t          inline_decl)
{
  if ((sheet == nullptr) || (el == nullptr)) {
    return inherited;
  }
  bool matched[k_ra_css_max_rules] = {};
  /* Bounded: rule_count <= k_ra_css_max_rules; i advances by 1 each step. */
  for (uint16_t i = 0U; i < sheet->rule_count; ++i) {
    matched[i] = ra_css_rule_matches(&sheet->rules[i], el, sheet);
  }
  ra_css_style_t out = {};
  priv_cascade_emphasis(&out, sheet, matched, &inherited, &inline_decl);
  priv_cascade_scalars(&out, sheet, matched, &inherited, &inline_decl);
  return out;
}
