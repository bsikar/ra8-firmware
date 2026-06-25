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

/**
 * @brief True for CSS whitespace (space / tab / newline / CR / FF).
 *
 * @details Classifies a single byte as a CSS white-space character per the CSS
 * specification: space (0x20), horizontal tab (0x09), newline (0x0A), carriage
 * return (0x0D), and form feed (0x0C). Used throughout the parser to skip or
 * trim tokens.
 *
 * @param[in] c Byte to classify (any value).
 *
 * @return bool Classification result.
 * @retval true  @p c is a CSS whitespace byte.
 * @retval false @p c is not a CSS whitespace byte.
 *
 * @pre No precondition on @p c; all byte values are valid inputs.
 * @pre Caller must be in a context where character classification is meaningful.
 * @post Return value is determined solely by @p c; no state is mutated.
 * @post The function has no side effects.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
static bool priv_is_ws(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r') || (c == '\f');
}

/**
 * @brief ASCII-fold one byte to lower case.
 *
 * @details Converts an ASCII upper-case letter ('A'-'Z') to its lower-case
 * equivalent by adding the fixed offset between 'A' and 'a'. All other byte
 * values are returned unchanged. This is used for case-insensitive CSS keyword
 * and identifier comparisons throughout the parser.
 *
 * @param[in] c Byte to fold; may be any value.
 *
 * @return char The lower-cased byte.
 * @retval c + ('a' - 'A') When @p c is in the range 'A'..'Z'.
 * @retval c               For all other byte values.
 *
 * @pre No precondition on @p c; all byte values are valid inputs.
 * @pre Caller must be performing a case-insensitive comparison or scan.
 * @post Return value differs from @p c only when @p c is an ASCII upper-case letter.
 * @post No state is mutated; the function is pure.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
static char priv_lower(char c)
{
  return (char)(((c >= 'A') && (c <= 'Z')) ? (c + ('a' - 'A')) : c);
}

/**
 * @brief Case-insensitive compare of span @p s[0..len) against NUL literal @p lit.
 *
 * @details Compares exactly @p len bytes of @p s against the NUL-terminated
 * string @p lit using ASCII case folding via priv_lower(). Returns true only
 * when the span and the literal have equal length and all bytes fold equal.
 * Either null pointer causes an immediate false return without dereferencing.
 *
 * @param[in] s   Pointer to the byte span (need not be NUL-terminated).
 * @param[in] len Number of bytes in @p s to compare.
 * @param[in] lit NUL-terminated reference string; must not be NULL for a true result.
 *
 * @return bool Comparison result.
 * @retval true  @p s[0..len) equals @p lit case-insensitively and lengths match.
 * @retval false Lengths differ, any byte differs, or a null pointer was passed.
 *
 * @pre @p len is the exact byte count of the span to compare (may be 0).
 * @pre @p lit is a valid NUL-terminated string pointer or NULL.
 * @post No state is mutated; the function is pure.
 * @post @p s and @p lit are not modified.
 *
 * @note Thread-safe; no shared mutable state.
 * @since 0.1.0
 */
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

/**
 * @brief Case-insensitive "span contains substring @p sub" (bounded scan).
 *
 * @details Slides a window of strlen(@p sub) bytes across @p s[0..len) and
 * checks for a case-insensitive match at each position via priv_lower(). The
 * outer loop is bounded by (len - sl + 1) windows; the inner loop is bounded
 * by sl, the NUL-terminated substring length. Returns false immediately if
 * @p s is NULL, @p sub is empty, or @p sub is longer than @p len.
 *
 * @param[in] s   Byte span to search (need not be NUL-terminated).
 * @param[in] len Number of bytes in @p s.
 * @param[in] sub NUL-terminated substring to search for.
 *
 * @return bool Search result.
 * @retval true  A case-insensitive occurrence of @p sub was found within @p s.
 * @retval false @p s is NULL, @p sub is empty, @p sub is longer than @p len, or
 *               no occurrence was found.
 *
 * @pre @p sub is a valid NUL-terminated string (length determinable via strlen).
 * @pre @p len accurately reflects the number of valid bytes reachable via @p s.
 * @post No state is mutated; the function is pure.
 * @post @p s and @p sub are not modified.
 *
 * @note Thread-safe; no shared mutable state.
 * @since 0.1.0
 */
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

/**
 * @brief Hex value of one ASCII digit, or k_priv_hex_base if not a hex digit.
 *
 * @details Maps an ASCII hexadecimal character ('0'-'9', 'a'-'f', 'A'-'F') to
 * its numeric value (0-15). The digit is first lower-cased via priv_lower() so
 * 'A'-'F' and 'a'-'f' are treated identically. Any byte outside those ranges
 * returns k_priv_hex_base (16) as a sentinel indicating "not a hex digit".
 *
 * @param[in] c ASCII character to convert.
 *
 * @return uint8_t Numeric hex value or sentinel.
 * @retval 0..9  For '0'..'9'.
 * @retval 10..15 For 'a'..'f' or 'A'..'F'.
 * @retval k_priv_hex_base (16) For any non-hex character.
 *
 * @pre No precondition on @p c; all byte values are valid inputs.
 * @pre Caller checks return value against k_priv_hex_base before use.
 * @post Return value is in [0, k_priv_hex_base]; no state is mutated.
 * @post The function is pure; @p c is not modified.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
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

/**
 * @brief Parse `rgb` / `rrggbb` hex digits (no `#`) to 0xRRGGBB, or invalid.
 *
 * @details Accepts exactly k_priv_hex3_len (3) or k_priv_hex6_len (6) hex
 * digits in @p s[0..len) with no leading `#`. For a 3-digit shorthand each
 * nibble @p d is expanded to @p dd (i.e., channel = (d << 4) | d). For 6-digit
 * form each pair is packed left-to-right into bits 23..0 of the result. Returns
 * k_priv_col_invalid when any digit is non-hex or the length is neither 3 nor 6.
 *
 * @param[in] s   Pointer to the hex digit span (no leading '#').
 * @param[in] len Number of bytes in @p s (must equal 3 or 6 for a valid colour).
 *
 * @return uint32_t Packed RGB value or invalid sentinel.
 * @retval 0x000000..0xFFFFFF Parsed colour in 0xRRGGBB format on success.
 * @retval k_priv_col_invalid When @p len is not 3 or 6, or a non-hex byte occurs.
 *
 * @pre @p s points to at least @p len readable bytes.
 * @pre @p len equals k_priv_hex3_len or k_priv_hex6_len for a valid result.
 * @post No state is mutated; the function is pure.
 * @post @p s is not modified.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
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

/**
 * @brief Parse a CSS colour value (`#rgb` / `#rrggbb` / a named keyword).
 *
 * @details Accepts the following forms in @p s[0..len):
 *   - `#rgb` and `#rrggbb` hex notations, delegated to priv_parse_hex_color().
 *   - CSS Level 1/2 named colours: black, white, red, green, blue, gray/grey,
 *     silver, maroon, navy (case-insensitive).
 * Returns k_priv_col_invalid when @p s is NULL, @p len is zero, the leading
 * `#` is followed by an invalid hex span, or the keyword is unrecognised.
 *
 * @param[in] s   Pointer to the colour value span (need not be NUL-terminated).
 * @param[in] len Number of bytes in @p s.
 *
 * @return uint32_t Parsed colour or invalid sentinel.
 * @retval 0x000000..0xFFFFFF Parsed colour in 0xRRGGBB format.
 * @retval k_priv_col_invalid When @p s is NULL, @p len is 0, or parsing fails.
 *
 * @pre @p s points to at least @p len readable bytes, or is NULL.
 * @pre @p len accurately reflects the span length.
 * @post No state is mutated; the function is pure.
 * @post @p s is not modified.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
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

/**
 * @brief Scan a decimal number into hundredths (e.g. "1.2" -> 120); advance @p i.
 *
 * @details Reads ASCII digits from @p s starting at @p *i, accumulating the
 * integer part multiplied by k_priv_fs_pct1 (100). If a decimal point follows,
 * up to k_priv_fs_frac (2) fractional digits are accumulated; further fractional
 * digits are consumed and discarded so the cursor is always left past the number.
 * Sets @p *any to true whenever at least one digit is consumed. On return @p *i
 * points to the first byte after all consumed digits.
 *
 * @param[in]     s   Byte span containing the decimal number.
 * @param[in]     len Total length of @p s.
 * @param[in,out] i   Cursor into @p s; advanced past all consumed digit/dot bytes.
 * @param[in,out] any Set to true when any digit is consumed; never reset to false.
 *
 * @return uint32_t Accumulated value in hundredths (value * 100).
 * @retval 0 When no digit bytes are present at @p *i.
 * @retval >0 The integer value scaled to hundredths, including fractional part.
 *
 * @pre @p s points to at least @p len readable bytes.
 * @pre @p i and @p any are non-NULL; @p *i <= @p len on entry.
 * @post @p *i is in [@p *i_entry, @p len] on return.
 * @post @p *any is unchanged if no digit was consumed, else it equals true.
 *
 * @note Thread-safe; operates only on caller-owned state.
 * @since 0.1.0
 */
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

/**
 * @brief Clamp a hundredths value to a whole number in [0, k_priv_fs_max].
 *
 * @details Divides @p hund by k_priv_fs_pct1 (100) to convert from hundredths
 * back to whole units, then clamps the result to at most k_priv_fs_max (9999)
 * to keep it representable in a uint16_t font-size field.
 *
 * @param[in] hund Value expressed in hundredths (e.g. 120 represents 1.20).
 *
 * @return uint16_t Clamped whole-unit value.
 * @retval 0..k_priv_fs_max The integer quotient of (@p hund / k_priv_fs_pct1),
 *                           clamped to k_priv_fs_max.
 *
 * @pre @p hund is a value previously produced by priv_scan_hundredths().
 * @pre Caller intends to store the result in a uint16_t font-size field.
 * @post Return value is in [0, k_priv_fs_max]; no state is mutated.
 * @post The function is pure; @p hund is not modified.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
static uint16_t priv_fs_whole(uint32_t hund)
{
  const uint32_t whole = hund / (uint32_t)k_priv_fs_pct1;
  return (uint16_t)((whole > (uint32_t)k_priv_fs_max) ? (uint32_t)k_priv_fs_max : whole);
}

/**
 * @brief Parse `Npx` / `N%` / `Nem` (N may be fractional) into value + unit.
 *
 * @details Delegates numeric scanning to priv_scan_hundredths(), then classifies
 * the trailing unit suffix:
 *   - "px": writes the clamped whole value to @p *out_val and k_ra_css_font_px
 *     to @p *out_unit.
 *   - "%": identical whole-value rounding, unit set to k_ra_css_font_pct.
 *   - "em": the hundredths value is itself the percentage (1em = 100%); stored
 *     clamped to k_priv_fs_max and unit k_ra_css_font_pct.
 * Returns false if no digit was parsed, or the unit suffix is unrecognised.
 *
 * @param[in]  s        Byte span containing the font-size value and unit.
 * @param[in]  len      Number of bytes in @p s.
 * @param[out] out_val  Receives the parsed numeric value (valid only on true).
 * @param[out] out_unit Receives the unit constant (valid only on true).
 *
 * @return bool Parse result.
 * @retval true  The span was a recognised font-size token; @p *out_val and
 *               @p *out_unit have been written.
 * @retval false No digit was found, or the unit suffix was not px / % / em.
 *
 * @pre @p s points to at least @p len readable bytes.
 * @pre @p out_val and @p out_unit are non-NULL.
 * @post On true, @p *out_val is in [0, k_priv_fs_max] and @p *out_unit is a
 *       valid ra_css_font_unit value.
 * @post On false, @p *out_val and @p *out_unit are not written.
 *
 * @note Thread-safe; operates only on caller-owned state.
 * @since 0.1.0
 */
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

/**
 * @brief Set or clear @p stylebit in @p out, marking @p setbit as present.
 *
 * @details Records that the emphasis property identified by @p setbit has been
 * explicitly declared by setting that bit in @p out->set. If @p on is true the
 * corresponding @p stylebit is OR'd into @p out->style; otherwise @p stylebit
 * is cleared with a bitwise AND of the complement. This is the single mutation
 * point for bold, italic, and underline state in the cascade.
 *
 * @param[in,out] out      Style record to update; must not be NULL.
 * @param[in]     setbit   Bit in @p out->set that marks this property as set.
 * @param[in]     stylebit Bit in @p out->style that carries the on/off value.
 * @param[in]     on       True to set @p stylebit, false to clear it.
 *
 * @return Nothing.
 *
 * @pre @p out is non-NULL.
 * @pre @p setbit and @p stylebit are single-bit masks valid for their fields.
 * @post @p out->set has @p setbit OR'd in.
 * @post @p out->style has @p stylebit set when @p on is true, cleared otherwise.
 *
 * @note Thread-safe with respect to distinct @p out instances; not safe for
 *       concurrent writes to the same @p out.
 * @since 0.1.0
 */
static void priv_set_emphasis(ra_css_style_t* out, uint8_t setbit, uint8_t stylebit, bool on)
{
  out->set   = (uint8_t)(out->set | setbit);
  out->style = (uint8_t)(on ? (out->style | stylebit) : (out->style & (uint8_t)~stylebit));
}

/**
 * @brief Apply a boolean-emphasis property (font-weight/style/decoration); false if other.
 *
 * @details Handles three CSS properties that map to on/off style bits:
 *   - "font-weight": bold when value is bold/bolder/600..900.
 *   - "font-style": italic when value is italic or oblique.
 *   - "text-decoration" / "text-decoration-line": underline when value contains
 *     the substring "underline".
 * Delegates mutation to priv_set_emphasis(). Returns false for any other
 * property so the caller can try the next handler.
 *
 * @param[in]     prop Style property name span (not NUL-terminated).
 * @param[in]     plen Length of @p prop in bytes.
 * @param[in]     val  Style value span (not NUL-terminated).
 * @param[in]     vlen Length of @p val in bytes.
 * @param[in,out] out  Style record to update on a match; must not be NULL.
 *
 * @return bool Whether the property was recognised and handled.
 * @retval true  @p prop was one of the handled emphasis properties; @p out updated.
 * @retval false @p prop was not an emphasis property; @p out is unchanged.
 *
 * @pre @p prop points to at least @p plen readable bytes.
 * @pre @p val points to at least @p vlen readable bytes.
 * @pre @p out is non-NULL.
 * @post @p prop and @p val are not modified.
 * @post On true, the appropriate set/style bits in @p out have been updated.
 *
 * @note Thread-safe with respect to distinct @p out instances.
 * @since 0.1.0
 */
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

/**
 * @brief Apply a parsed `text-align` value to @p out.
 *
 * @details Sets the k_ra_css_set_align bit in @p out->set and assigns
 * @p out->align to the matching ra_reflow_align constant:
 *   - "right"   -> k_ra_reflow_align_right
 *   - "center"  -> k_ra_reflow_align_center
 *   - "justify" -> k_ra_reflow_align_justify
 *   - any other -> k_ra_reflow_align_left (CSS default)
 * Comparison is case-insensitive via priv_ci_eq().
 *
 * @param[in]     val  Value span for the text-align property.
 * @param[in]     vlen Length of @p val in bytes.
 * @param[in,out] out  Style record to update; must not be NULL.
 *
 * @return Nothing.
 *
 * @pre @p val points to at least @p vlen readable bytes.
 * @pre @p out is non-NULL.
 * @post @p out->set has k_ra_css_set_align OR'd in.
 * @post @p out->align holds the parsed alignment constant.
 *
 * @note Thread-safe with respect to distinct @p out instances.
 * @since 0.1.0
 */
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

/**
 * @brief Apply one trimmed `prop:value` pair to @p out, setting the matching bit.
 *
 * @details Routes a single CSS declaration to the appropriate handler:
 *   - Emphasis properties (font-weight, font-style, text-decoration) via
 *     priv_apply_emphasis().
 *   - "text-align" via priv_apply_align().
 *   - "color": parsed via priv_parse_color(); applied only on a valid result.
 *   - "font-size": parsed via priv_parse_fontsize(); applied only on success.
 *   - "display": stored as 1 for "none", 0 for any other value.
 *   - Unrecognised properties are silently ignored; no set bit is touched.
 *
 * @param[in]     prop Style property name span (not NUL-terminated).
 * @param[in]     plen Length of @p prop in bytes.
 * @param[in]     val  Style value span (not NUL-terminated).
 * @param[in]     vlen Length of @p val in bytes.
 * @param[in,out] out  Style record to update; must not be NULL.
 *
 * @return Nothing.
 *
 * @pre @p prop points to at least @p plen readable bytes.
 * @pre @p val points to at least @p vlen readable bytes.
 * @pre @p out is non-NULL.
 * @post The relevant set bit and data field in @p out are updated when the
 *       property is recognised and the value parses successfully.
 * @post @p prop and @p val are not modified.
 *
 * @note Thread-safe with respect to distinct @p out instances.
 * @since 0.1.0
 */
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

/**
 * @brief Parse a `prop:value; ...` declaration body (no braces) into @p out.
 *
 * @details Iterates over semicolon-delimited declarations in @p s[0..len).
 * For each declaration, it locates the colon separator, trims whitespace from
 * both property name and value, and delegates to priv_apply_decl(). Declarations
 * with an empty property or value after trimming are skipped silently. The loop
 * is bounded by @p len; each iteration advances past the next semicolon.
 *
 * @param[in]     s   Byte span of the declaration block (no surrounding braces).
 * @param[in]     len Number of bytes in @p s.
 * @param[in,out] out Style record to accumulate parsed declarations into.
 *
 * @return Nothing.
 *
 * @pre @p s points to at least @p len readable bytes (may be NULL only when len == 0).
 * @pre @p out is non-NULL and zero-initialised or partially filled by the caller.
 * @post @p out reflects all successfully parsed declarations from @p s.
 * @post @p s is not modified.
 *
 * @note Thread-safe with respect to distinct @p out instances.
 * @since 0.1.0
 */
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
 * mutation when @p len is 0, exceeds k_ra_css_name_max, or would cause names_used
 * to overflow k_ra_css_name_pool.
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
 * @brief Parse the optional leading type / `*` of a compound selector.
 *
 * @details Reads the element type or universal selector at @p s[*i]:
 *   - If the current byte is '*', advances @p *i by 1 and returns true without
 *     recording a type constraint (universal selector).
 *   - Otherwise, consumes a run of priv_is_name_char() bytes and classifies the
 *     span via ra_reflow_tok_classify(). On success, stores the tag in
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
 * @brief Split @p s into whitespace-separated compound spans.
 *
 * @details Iterates over @p s[0..len), skipping runs of priv_is_ws() characters
 * and recording the start and length of each non-whitespace token in the parallel
 * @p part_p / @p part_n arrays. Returns -1 early when the part count would exceed
 * (k_ra_css_max_anc + 1), enforcing the maximum ancestor depth. Both output
 * arrays must have capacity for at least (k_ra_css_max_anc + 1) entries.
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
 * @retval -1 More than (k_ra_css_max_anc + 1) tokens were encountered.
 *
 * @pre @p s points to at least @p len readable bytes.
 * @pre @p part_p and @p part_n are non-NULL with capacity >= (k_ra_css_max_anc + 1).
 * @post On a non-negative return, @p part_p[0..ret) and @p part_n[0..ret) are set.
 * @post @p s is not modified; entries beyond the return count are unspecified.
 *
 * @note Thread-safe; operates only on caller-owned arrays.
 * @since 0.1.0
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

/**
 * @brief Append one fully-built rule to the sheet; drop silently if full.
 *
 * @details Copies @p *rule into @p sheet->rules[sheet->rule_count], stamps
 * the stored rule's order field with @p sheet->next_order, then increments
 * both @p sheet->rule_count and @p sheet->next_order. When the sheet has
 * already reached k_ra_css_max_rules, the function returns immediately without
 * modifying any field, silently discarding the rule.
 *
 * @param[in,out] sheet Sheet that receives the appended rule.
 * @param[in]     rule  Fully-parsed rule to append; must not be NULL.
 *
 * @return Nothing.
 *
 * @pre @p sheet and @p rule are non-NULL.
 * @pre @p rule is fully populated (selector and declaration fields valid).
 * @post When @p sheet->rule_count < k_ra_css_max_rules, rule_count and next_order
 *       are each incremented by 1 and the rule is stored.
 * @post When @p sheet->rule_count == k_ra_css_max_rules, no field is modified.
 *
 * @note Thread-safe with respect to distinct @p sheet instances.
 * @since 0.1.0
 */
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

/**
 * @brief Parse a comma-grouped selector list sharing declaration @p decl.
 *
 * @details Iterates over comma-delimited selectors in @p sel[0..sel_len), trims
 * each individual selector, constructs a fresh ra_css_rule_t carrying @p decl,
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
 *       @p sheet (subject to k_ra_css_max_rules capacity).
 * @post @p sel is not modified.
 *
 * @note Thread-safe with respect to distinct @p sheet instances.
 * @since 0.1.0
 */
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

/**
 * @brief Case-insensitive equality of two byte spans.
 *
 * @details Compares @p a[0..alen) and @p b[0..blen) byte-by-byte after
 * ASCII case-folding via priv_lower(). Returns false immediately when either
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

/**
 * @brief True for a `font-weight` keyword that selects the bold face.
 *
 * @details Returns true when @p val[0..vlen) matches (case-insensitively) any of
 * the CSS bold-weight keywords: "bold", "bolder", "600", "700", "800", or "900".
 * All comparisons are delegated to priv_ci_eq().
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
static bool priv_is_bold_kw(const char* val, size_t vlen)
{
  return priv_ci_eq(val, vlen, "bold") || priv_ci_eq(val, vlen, "bolder") ||
         priv_ci_eq(val, vlen, "600") || priv_ci_eq(val, vlen, "700") ||
         priv_ci_eq(val, vlen, "800") || priv_ci_eq(val, vlen, "900");
}

/**
 * @brief True for a `font-style` keyword that selects the italic face.
 *
 * @details Returns true when @p val[0..vlen) matches (case-insensitively) either
 * "italic" or "oblique" via priv_ci_eq(). All other values (e.g., "normal")
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
static bool priv_is_italic_kw(const char* val, size_t vlen)
{
  return priv_ci_eq(val, vlen, "italic") || priv_ci_eq(val, vlen, "oblique");
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
 * font-style, and src into a local ra_css_fontface_t. The face is appended to
 * @p sheet->faces[] only when both family_len and src_len are non-zero. If the
 * sheet is already at k_ra_css_max_faces capacity, the function returns without
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

/**
 * @brief priv_decl_fn adapter that interns a rule's `font-family` value.
 *
 * @details Casts @p ctx to a priv_family_ctx_t pointer. Ignores the callback
 * unless @p prop matches "font-family". When it does, strips quotes from @p val
 * via priv_strip_quotes() and interns the result into the sheet's name pool.
 * On success, sets the k_ra_css_set_family bit in @p decl->set and stores
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
 *       family fields set and k_ra_css_set_family OR'd into @p ctx->decl->set.
 * @post @p prop and @p val are not modified.
 *
 * @note Thread-safety matches that of priv_intern_name().
 * @since 0.1.0
 */
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
 *       k_ra_css_set_family set and family_off/family_len populated.
 * @post @p block is not modified.
 *
 * @note Thread-safe with respect to distinct @p sheet and @p decl instances.
 * @since 0.1.0
 */
static void
priv_extract_family(ra_css_sheet_t* sheet, const char* block, size_t len, ra_css_style_t* decl)
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

/**
 * @brief True iff face @p f's family equals @p family (case-insensitive).
 *
 * @details Retrieves the interned family name from @p sheet->names using
 * @p f->family_off and @p f->family_len, then delegates to priv_ci_eq_span()
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

/**
 * @brief Dispatch one `selector|@rule { block }`: at-rule vs. style rule.
 *
 * @details Trims @p sel to obtain the effective keyword. When the first byte is
 * '@', routes to priv_parse_at_rule(). Otherwise, parses the declaration block
 * via priv_parse_decls() and priv_extract_family(), then distributes the
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

/**
 * @brief True if descendant ancestor part @p anc matches element @p el.
 *
 * @details Checks each non-zero constraint field of @p anc against @p el:
 *   - tag:       @p el->tag must equal @p anc->tag when the tag is not unknown.
 *   - class_len: @p el->class_str must contain the interned class name via
 *                priv_class_list_has().
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
 * @details The subject must match @p el via ra_css_rule_matches(); when
 * @p rule->anc_count is 0 that check is sufficient. For rules with ancestor
 * constraints, a greedy right-to-left scan walks the ancestor stack from innermost
 * (@p ancestors[n_anc-1]) to outermost (@p ancestors[0]), consuming one rule
 * ancestor part per stack element that matches via priv_anc_matches(). The
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

/**
 * @brief Packed specificity (id*10000 + class*100 + type), summing all parts.
 *
 * @details Computes the CSS cascade specificity of @p rule by summing weights
 * for each present constraint across the subject and all ancestor parts:
 *   - k_priv_spec_id (10000) for each non-empty id field.
 *   - k_priv_spec_class (100) for each non-empty class field.
 *   - k_priv_spec_type (1) for each non-unknown tag.
 * The ancestor loop is bounded by @p rule->anc_count. The result is used by
 * priv_resolve() to determine which rule wins during the cascade.
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

/**
 * @brief Resolve the bold / italic / underline emphasis bits into @p out.
 *
 * @details Iterates over a fixed table of three emphasis properties (bold,
 * italic, underline) and calls priv_resolve() for each to find the winning
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

/**
 * @brief Resolve the scalar properties (align / colour / font-size / display).
 *
 * @details Calls priv_resolve() for each of the four scalar CSS properties
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

/**
 * @brief Resolve the inherited `font-family` slice into @p out.
 *
 * @details Calls priv_resolve() for the k_ra_css_set_family property and, when
 * a winning style is found, copies (family_off, family_len) from the winner into
 * @p out and OR's k_ra_css_set_family into @p out->set. The family name itself
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
 *       @p out->family_len are set and k_ra_css_set_family is OR'd into @p out->set.
 * @post @p inherited and @p inl are not modified.
 *
 * @note Thread-safe with respect to distinct @p out instances.
 * @since 0.1.0
 */
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
