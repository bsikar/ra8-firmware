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
 * @brief Cascade specificity weights (packed id > class > type > universal).
 *
 * @details A rule's specificity is the sum of one weight per present constraint
 * (id + class + type), each at most 1 in our compound model; the packed value
 * orders the cascade. Inheritance is the lowest priority (weight 0); a universal
 * selector (no constraints) is also 0 but beats inheritance on source order.
 */
typedef enum : uint16_t {
  k_priv_rank_inherited = 0U,     /**< Inheritance / universal weight.    */
  k_priv_spec_type      = 1U,     /**< A type constraint adds this.       */
  k_priv_spec_class     = 100U,   /**< A class constraint adds this.      */
  k_priv_spec_id        = 10000U, /**< An id constraint adds this.        */
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
 * @enum priv_css_face_t
 * @brief `@font-face` parsing constants.
 */
typedef enum : uint8_t {
  k_priv_url_len = 4U, /**< Length of the `url(` token prefix. */
} priv_css_face_t;

/**
 * @enum priv_css_scan_t
 * @brief Token lengths used by the top-level stylesheet scanner.
 *
 * @details Stand-ins for the `/` + `*` comment delimiters so the parse loop
 * carries no bare numeric literals when stepping over comments and blocks.
 *
 * @invariant k_priv_cmt_marker is the byte count of `/` + `*` (or `*` + `/`).
 * @see ra_css_parse
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_priv_cmt_marker = 2U, /**< Byte length of a CSS comment delimiter pair. */
} priv_css_scan_t;

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

/** @brief Parse the optional leading type / `*` of a compound selector. */
static bool priv_parse_sel_type(const char* s, size_t len, size_t* i, ra_css_rule_t* rule)
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
  const ra_reflow_html_tag_t tag = ra_reflow_tok_classify(&s[start], *i - start);
  if (tag == k_ra_reflow_tag_unknown) {
    return false; /* unrecognised tag -> drop the rule */
  }
  rule->sel_tag = (uint8_t)tag;
  return true;
}

/** @brief Parse one `.class` / `#id` part at @p s[*i] into @p rule; advance @p i. */
static bool priv_parse_sel_part(ra_css_sheet_t* sheet,
                                const char*     s,
                                size_t          len,
                                size_t*         i,
                                ra_css_rule_t*  rule)
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
 */
static bool
priv_parse_selector(ra_css_sheet_t* sheet, const char* s, size_t len, ra_css_rule_t* rule)
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
 * @brief Parse a (possibly descendant) selector into @p rule.
 *
 * @details Splits @p s on whitespace into compound parts; the LAST part is the
 * subject (folded into @p rule's `sel_tag` / `class` / `id`), the earlier parts
 * are ancestor constraints stored in `rule->anc` (selector order, outermost
 * first). Returns false (the caller drops the rule) on an unsupported compound,
 * or on more than ::k_ra_css_max_anc ancestor parts.
 */
/**
 * @brief Split @p s into whitespace-separated compound spans.
 *
 * @return The number of compound parts written to @p part_p / @p part_n, or -1
 *         if there are more than (1 subject + ::k_ra_css_max_anc) parts.
 */
static int32_t priv_split_compounds(const char* s, size_t len, const char** part_p, size_t* part_n)
{
  size_t nparts = 0U;
  size_t i      = 0U;
  /* Bounded: i advances to len; each pass consumes >=1 char after the ws skip. */
  while (i < len) {
    while ((i < len) && priv_is_ws(s[i])) {
      ++i;
    }
    if (i >= len) {
      break;
    }
    if (nparts > (size_t)k_ra_css_max_anc) {
      return -1; /* more than one subject + k_ra_css_max_anc ancestor parts */
    }
    const size_t start = i;
    while ((i < len) && !priv_is_ws(s[i])) {
      ++i;
    }
    part_p[nparts] = &s[start];
    part_n[nparts] = i - start;
    ++nparts;
  }
  return (int32_t)nparts;
}

static bool
priv_parse_complex_selector(ra_css_sheet_t* sheet, const char* s, size_t len, ra_css_rule_t* rule)
{
  const char*   part_p[(size_t)k_ra_css_max_anc + 1U] = {};
  size_t        part_n[(size_t)k_ra_css_max_anc + 1U] = {};
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
    ra_css_rule_t tmp = {};
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
    if (priv_parse_complex_selector(sheet, one, one_len, &rule)) {
      priv_push_rule(sheet, &rule);
    }
    i = comma + 1U;
  }
}

/* ===========================================================================
 * @font-face + font-family parsing
 * ===========================================================================
 */

/** @brief Case-insensitive equality of two byte spans. */
static bool priv_ci_eq_span(const char* a, size_t alen, const char* b, size_t blen)
{
  if ((a == nullptr) || (b == nullptr) || (alen != blen)) {
    return false;
  }
  /* Bounded: k < alen (== blen); one byte folded per step. */
  for (size_t k = 0U; k < alen; ++k) {
    if (priv_lower(a[k]) != priv_lower(b[k])) {
      return false;
    }
  }
  return true;
}

/** @brief Strip one layer of matching `'`/`"` quotes from span @p s[0..*len). */
static const char* priv_strip_quotes(const char* s, size_t* len)
{
  if ((*len >= 2U) && ((s[0] == '"') || (s[0] == '\'')) && (s[*len - 1U] == s[0])) {
    *len -= 2U;
    return &s[1];
  }
  return s;
}

/** @brief Extract the path inside the first `url(...)` of a `src` value. */
static bool priv_extract_url(const char* val, size_t vlen, const char** url, size_t* ulen)
{
  /* Bounded: at most (vlen - 3) windows; i advances by 1 each step. */
  for (size_t i = 0U; (i + (size_t)k_priv_url_len) <= vlen; ++i) {
    if (!priv_ci_eq(&val[i], (size_t)k_priv_url_len, "url(")) {
      continue;
    }
    size_t a = i + (size_t)k_priv_url_len;
    size_t b = a;
    while ((b < vlen) && (val[b] != ')')) {
      ++b;
    }
    size_t      n = b - a;
    const char* p = priv_trim(&val[a], &n);
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

/** @brief True for a `font-weight` keyword that selects the bold face. */
static bool priv_is_bold_kw(const char* val, size_t vlen)
{
  return priv_ci_eq(val, vlen, "bold") || priv_ci_eq(val, vlen, "bolder") ||
         priv_ci_eq(val, vlen, "600") || priv_ci_eq(val, vlen, "700") ||
         priv_ci_eq(val, vlen, "800") || priv_ci_eq(val, vlen, "900");
}

/** @brief True for a `font-style` keyword that selects the italic face. */
static bool priv_is_italic_kw(const char* val, size_t vlen)
{
  return priv_ci_eq(val, vlen, "italic") || priv_ci_eq(val, vlen, "oblique");
}

/** @brief Apply one `@font-face` declaration to the face being built. */
static void priv_face_apply(ra_css_sheet_t*    sheet,
                            const char*        prop,
                            size_t             plen,
                            const char*        val,
                            size_t             vlen,
                            ra_css_fontface_t* face)
{
  if (priv_ci_eq(prop, plen, "font-family")) {
    size_t      n   = vlen;
    const char* fam = priv_strip_quotes(val, &n);
    uint16_t    off = 0U;
    if ((n > 0U) && priv_intern_name(sheet, fam, n, &off)) {
      face->family_off = off;
      face->family_len = (uint16_t)n;
    }
  } else if (priv_ci_eq(prop, plen, "font-weight")) {
    face->weight_bold = priv_is_bold_kw(val, vlen) ? 1U : 0U;
  } else if (priv_ci_eq(prop, plen, "font-style")) {
    face->style_italic = priv_is_italic_kw(val, vlen) ? 1U : 0U;
  } else if (priv_ci_eq(prop, plen, "src")) {
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

/** @brief Iterate `prop:value;` pairs in a declaration block (no braces). */
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
      const char* prop = priv_trim(&s[i], &plen);
      size_t      vlen = semi - (colon + 1U);
      const char* val  = priv_trim(&s[colon + 1U], &vlen);
      if ((plen > 0U) && (vlen > 0U)) {
        cb(ctx, prop, plen, val, vlen);
      }
    }
    i = semi + 1U;
  }
}

/** @brief Context threading a sheet + the face being built through the loop. */
typedef struct {
  ra_css_sheet_t*    sheet; /**< Sheet receiving interned name bytes. */
  ra_css_fontface_t* face;  /**< Face accumulating descriptors.       */
} priv_face_ctx_t;

/** @brief ::priv_decl_fn adapter that routes one descriptor to ::priv_face_apply. */
static void priv_face_cb(void* ctx, const char* prop, size_t plen, const char* val, size_t vlen)
{
  priv_face_ctx_t* c = (priv_face_ctx_t*)ctx;
  priv_face_apply(c->sheet, prop, plen, val, vlen, c->face);
}

/** @brief Parse one `@font-face { ... }` block; append it if family + src set. */
static void priv_parse_fontface(ra_css_sheet_t* sheet, const char* block, size_t len)
{
  if (sheet->face_count >= (uint16_t)k_ra_css_max_faces) {
    return;
  }
  ra_css_fontface_t face = {};
  priv_face_ctx_t   ctx  = {.sheet = sheet, .face = &face};
  priv_for_each_decl(block, len, priv_face_cb, &ctx);
  if ((face.family_len != 0U) && (face.src_len != 0U)) {
    sheet->faces[sheet->face_count] = face;
    sheet->face_count               = (uint16_t)(sheet->face_count + 1U);
  }
}

/** @brief Context threading a sheet + the rule declaration through the loop. */
typedef struct {
  ra_css_sheet_t* sheet; /**< Sheet receiving the interned family name. */
  ra_css_style_t* decl;  /**< Declaration receiving the family slice.   */
} priv_family_ctx_t;

/** @brief ::priv_decl_fn adapter that interns a rule's `font-family` value. */
static void priv_family_cb(void* ctx, const char* prop, size_t plen, const char* val, size_t vlen)
{
  priv_family_ctx_t* c = (priv_family_ctx_t*)ctx;
  if (!priv_ci_eq(prop, plen, "font-family")) {
    return;
  }
  size_t      n   = vlen;
  const char* fam = priv_strip_quotes(val, &n);
  uint16_t    off = 0U;
  if ((n > 0U) && priv_intern_name(c->sheet, fam, n, &off)) {
    c->decl->set        = (uint8_t)(c->decl->set | (uint8_t)k_ra_css_set_family);
    c->decl->family_off = off;
    c->decl->family_len = (uint16_t)n;
  }
}

/** @brief Scan a rule's declaration block for `font-family`, interning it. */
static void
priv_extract_family(ra_css_sheet_t* sheet, const char* block, size_t len, ra_css_style_t* decl)
{
  priv_family_ctx_t ctx = {.sheet = sheet, .decl = decl};
  priv_for_each_decl(block, len, priv_family_cb, &ctx);
}

/** @brief Route an at-rule: parse `@font-face`; skip every other `@`-rule. */
static void priv_parse_at_rule(ra_css_sheet_t* sheet,
                               const char*     sel,
                               size_t          sel_len,
                               const char*     block,
                               size_t          block_len)
{
  if (priv_ci_eq(sel, sel_len, "@font-face")) {
    priv_parse_fontface(sheet, block, block_len);
  }
  /* @media / @import / @page / ... are out of v1 scope -> skipped. */
}

/** @brief True iff face @p f's family equals @p family (case-insensitive). */
static bool priv_family_eq(const ra_css_sheet_t*    sheet,
                           const ra_css_fontface_t* f,
                           const char*              family,
                           size_t                   family_len)
{
  return priv_ci_eq_span((const char*)&sheet->names[f->family_off],
                         (size_t)f->family_len,
                         family,
                         family_len);
}

/** @brief Dispatch one `selector|@rule { block }`: at-rule vs. style rule. */
static void priv_parse_one_block(ra_css_sheet_t* sheet,
                                 const char*     sel,
                                 size_t          sel_len,
                                 const char*     block,
                                 size_t          block_len)
{
  size_t      tlen = sel_len;
  const char* tsel = priv_trim(sel, &tlen);
  if ((tlen > 0U) && (tsel[0] == '@')) {
    priv_parse_at_rule(sheet, tsel, tlen, block, block_len);
    return;
  }
  ra_css_style_t decl = {};
  priv_parse_decls(block, block_len, &decl);
  priv_extract_family(sheet, block, block_len, &decl);
  priv_parse_selector_list(sheet, sel, sel_len, decl);
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
  sheet->face_count = 0U;
  sheet->next_order = 0U;
  sheet->names_used = 0U;
  return k_ra_ok;
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
static size_t priv_skip_comment(const char* css, size_t len, size_t start)
{
  const char open_a  = '/';
  const char open_b  = '*';
  size_t     j       = start + (size_t)k_priv_cmt_marker;
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
static bool
priv_find_block(const char* css, size_t len, size_t i, size_t* out_open, size_t* out_close)
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

ra_err_t ra_css_parse(ra_css_sheet_t* sheet, const char* css, uint32_t len)
{
  if ((sheet == nullptr) || (css == nullptr)) {
    return k_ra_err_null_ptr;
  }
  const char open_a = '/';
  const char open_b = '*';
  size_t     i      = 0U;
  /* Bounded: each iteration advances past a block or breaks at EOF. */
  while (i < (size_t)len) {
    if (((i + 1U) < (size_t)len) && (css[i] == open_a) && (css[i + 1U] == open_b)) {
      i = priv_skip_comment(css, (size_t)len, i);
      continue;
    }
    if (priv_is_ws(css[i])) {
      ++i;
      continue;
    }
    size_t brace = 0U;
    size_t close = 0U;
    if (!priv_find_block(css, (size_t)len, i, &brace, &close)) {
      break; /* no block -> done */
    }
    priv_parse_one_block(sheet, &css[i], brace - i, &css[brace + 1U], close - (brace + 1U));
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
  /* Every present constraint must match (no constraint = universal). */
  if ((rule->sel_tag != (uint8_t)k_ra_reflow_tag_unknown) && (el->tag != rule->sel_tag)) {
    return false;
  }
  if (rule->class_len != 0U) {
    const char* nm = (const char*)&sheet->names[rule->class_off];
    if ((el->class_str == nullptr) ||
        !priv_class_list_has(el->class_str, el->class_len, nm, rule->class_len)) {
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

/** @brief True if descendant ancestor part @p anc matches element @p el. */
static bool
priv_anc_matches(const ra_css_anc_t* anc, const ra_css_element_t* el, const ra_css_sheet_t* sheet)
{
  if ((anc->tag != (uint8_t)k_ra_reflow_tag_unknown) && (el->tag != anc->tag)) {
    return false;
  }
  if (anc->class_len != 0U) {
    const char* nm = (const char*)&sheet->names[anc->class_off];
    if ((el->class_str == nullptr) ||
        !priv_class_list_has(el->class_str, el->class_len, nm, anc->class_len)) {
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
 * @details The subject must match @p el; then each ancestor part must match some
 * element of @p ancestors (outermost first), in selector order, with the CSS
 * descendant combinator (any depth between parts). A simple rule (no ancestor
 * parts) reduces to ::ra_css_rule_matches.
 */
static bool priv_rule_matches_ctx(const ra_css_rule_t*    rule,
                                  const ra_css_element_t* el,
                                  const ra_css_element_t* ancestors,
                                  uint8_t                 n_anc,
                                  const ra_css_sheet_t*   sheet)
{
  if (!ra_css_rule_matches(rule, el, sheet)) {
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
    if (priv_anc_matches(&rule->anc[ai], &ancestors[si], sheet)) {
      --ai;
    }
    --si;
  }
  return ai < 0;
}

/** @brief Packed specificity (id*10000 + class*100 + type), summing all parts. */
static uint16_t priv_rule_rank(const ra_css_rule_t* rule)
{
  uint16_t spec = 0U;
  if (rule->sel_tag != (uint8_t)k_ra_reflow_tag_unknown) {
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
    if (rule->anc[a].tag != (uint8_t)k_ra_reflow_tag_unknown) {
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
    const uint16_t rank = priv_rule_rank(&sheet->rules[i]);
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

/** @brief Resolve the inherited `font-family` slice into @p out. */
static void priv_cascade_family(ra_css_style_t*       out,
                                const ra_css_sheet_t* sheet,
                                const bool*           matched,
                                const ra_css_style_t* inherited,
                                const ra_css_style_t* inl)
{
  const ra_css_style_t* win =
    priv_resolve((uint8_t)k_ra_css_set_family, inherited, sheet, matched, inl);
  if (win != nullptr) {
    out->set        = (uint8_t)(out->set | (uint8_t)k_ra_css_set_family);
    out->family_off = win->family_off;
    out->family_len = win->family_len;
  }
}

ra_css_style_t ra_css_cascade_ctx(const ra_css_sheet_t*   sheet,
                                  const ra_css_element_t* el,
                                  ra_css_style_t          inherited,
                                  ra_css_style_t          inline_decl,
                                  const ra_css_element_t* ancestors,
                                  uint8_t                 n_anc)
{
  if ((sheet == nullptr) || (el == nullptr)) {
    return inherited;
  }
  bool matched[k_ra_css_max_rules] = {};
  /* Bounded: rule_count <= k_ra_css_max_rules; i advances by 1 each step. */
  for (uint16_t i = 0U; i < sheet->rule_count; ++i) {
    matched[i] = priv_rule_matches_ctx(&sheet->rules[i], el, ancestors, n_anc, sheet);
  }
  ra_css_style_t out = {};
  priv_cascade_emphasis(&out, sheet, matched, &inherited, &inline_decl);
  priv_cascade_scalars(&out, sheet, matched, &inherited, &inline_decl);
  priv_cascade_family(&out, sheet, matched, &inherited, &inline_decl);
  return out;
}

ra_css_style_t ra_css_cascade(const ra_css_sheet_t*   sheet,
                              const ra_css_element_t* el,
                              ra_css_style_t          inherited,
                              ra_css_style_t          inline_decl)
{
  return ra_css_cascade_ctx(sheet, el, inherited, inline_decl, nullptr, 0U);
}

int16_t ra_css_match_face(const ra_css_sheet_t* sheet,
                          const char*           family,
                          uint16_t              family_len,
                          bool                  want_bold,
                          bool                  want_italic)
{
  if ((sheet == nullptr) || (family == nullptr) || (family_len == 0U)) {
    return (int16_t)k_ra_css_no_face;
  }
  int16_t fallback = (int16_t)k_ra_css_no_face;
  /* Bounded: face_count <= k_ra_css_max_faces; i advances by 1 each step. */
  for (uint16_t i = 0U; i < sheet->face_count; ++i) {
    const ra_css_fontface_t* f = &sheet->faces[i];
    if (!priv_family_eq(sheet, f, family, (size_t)family_len)) {
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

bool ra_css_face_src(const ra_css_sheet_t* sheet,
                     uint16_t              idx,
                     const char**          out_src,
                     uint16_t*             out_len)
{
  if ((sheet == nullptr) || (out_src == nullptr) || (out_len == nullptr) ||
      (idx >= sheet->face_count)) {
    return false;
  }
  *out_src = (const char*)&sheet->names[sheet->faces[idx].src_off];
  *out_len = sheet->faces[idx].src_len;
  return true;
}
