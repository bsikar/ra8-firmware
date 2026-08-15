/**
 * @file ra8_reflow_css.c
 * @brief Content-CSS primitives + declaration-body parsing (#111).
 *
 * @details
 * The string-classification primitives and the `prop:value;` declaration parser
 * for the v1 CSS subset documented in `ra8_reflow_css.h`. This translation unit
 * owns the small pure leaf helpers shared across the split (white-space test,
 * ASCII case-fold, case-insensitive compare, span trim) and the declaration
 * value parsers (colour, font-size, emphasis, align). It also implements the
 * public inline-declaration entry point ::ra8_css_parse_inline. No MMIO, no heap;
 * every buffer is a fixed-size field of the caller-owned ::ra8_css_sheet_t.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_reflow_css.h"

#include <stddef.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_reflow.h" /* style/align enums, k_ra8_css_font_* units */
#include "ra8_reflow_css_internal.h"

/* ===========================================================================
 * Internal constants (no magic numbers)
 * ===========================================================================
 */

/**
 * @enum priv_css_hex_t
 * @brief Hex-colour parsing constants (`#rgb` / `#rrggbb`).
 */
typedef enum : uint8_t {
  k_priv_hex_base = 16U, /**< Base + "not a hex digit" sentinel. */
  k_priv_hex_a10  = 10U, /**< Value of hex 'a'/'A'.              */
  k_priv_hex3_len = 3U,  /**< `#rgb` short-form digit count.     */
  k_priv_hex6_len = 6U,  /**< `#rrggbb` digit count.             */
  k_priv_hex_nib  = 4U,  /**< Bits per hex nibble.               */
  k_priv_hex_chan = 8U,  /**< Bits per colour channel.           */
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
  k_priv_col_black   = 0x000000U,   /**< CSS `black`.                      */
  k_priv_col_white   = 0xFFFFFFU,   /**< CSS `white`.                      */
  k_priv_col_red     = 0xFF0000U,   /**< CSS `red`.                        */
  k_priv_col_green   = 0x008000U,   /**< CSS `green`.                      */
  k_priv_col_blue    = 0x0000FFU,   /**< CSS `blue`.                       */
  k_priv_col_gray    = 0x808080U,   /**< CSS `gray` / `grey`.              */
  k_priv_col_silver  = 0xC0C0C0U,   /**< CSS `silver`.                     */
  k_priv_col_maroon  = 0x800000U,   /**< CSS `maroon`.                     */
  k_priv_col_navy    = 0x000080U,   /**< CSS `navy`.                       */
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
bool priv_ra8_reflow_css_is_ws(char c)
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
char priv_ra8_reflow_css_lower(char c)
{
  return (char)(((c >= 'A') && (c <= 'Z')) ? (c + ('a' - 'A')) : c);
}

/**
 * @brief Case-insensitive compare of span @p s[0..len) against NUL literal @p lit.
 *
 * @details Compares exactly @p len bytes of @p s against the NUL-terminated
 * string @p lit using ASCII case folding via priv_ra8_reflow_css_lower(). Returns true only
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
bool priv_ra8_reflow_css_ci_eq(const char* s, size_t len, const char* lit)
{
  if ((s == nullptr) || (lit == nullptr)) {
    return false;
  }
  size_t k = 0U;
  for (; (k < len) && (lit[k] != '\0'); ++k) {
    if (priv_ra8_reflow_css_lower(s[k]) != priv_ra8_reflow_css_lower(lit[k])) {
      return false;
    }
  }
  return (k == len) && (lit[k] == '\0');
}

/**
 * @brief Case-insensitive "span contains substring @p sub" (bounded scan).
 *
 * @details Slides a window of strlen(@p sub) bytes across @p s[0..len) and
 * checks for a case-insensitive match at each position via priv_ra8_reflow_css_lower(). The
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
RA8_INTERNAL
static bool internal_ci_contains(const char* s, size_t len, const char* sub)
{
  const size_t sl = strlen(sub);
  /* mcdc-deactivated: the sole caller passes internal_ci_contains(val, vlen, "underline") -- val is non-NULL (from priv_ra8_reflow_css_trim) and sub is a compile-time literal so sl == 9 (never 0); (s == nullptr) and (sl == 0U) are unreachable, only (sl > len) varies. */
  if ((s == nullptr) || (sl == 0U) || (sl > len)) {
    return false;
  }
  /* Bounded: at most (len - sl + 1) windows; i advances by 1 each step. */
  for (size_t i = 0U; (i + sl) <= len; ++i) {
    bool hit = true;
    /* Bounded: j < sl, the NUL-terminated literal's strlen. */
    for (size_t j = 0U; j < sl; ++j) {
      if (priv_ra8_reflow_css_lower(s[i + j]) != priv_ra8_reflow_css_lower(sub[j])) {
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
const char* priv_ra8_reflow_css_trim(const char* s, size_t* len)
{
  size_t a = 0U;
  size_t b = *len;
  while ((a < b) && priv_ra8_reflow_css_is_ws(s[a])) {
    ++a;
  }
  while ((b > a) && priv_ra8_reflow_css_is_ws(s[b - 1U])) {
    --b;
  }
  *len = b - a;
  return &s[a];
}

/* ===========================================================================
 * Declaration-body parsing (colour / font-size / emphasis / align)
 * ===========================================================================
 */

/**
 * @brief Hex value of one ASCII digit, or k_priv_hex_base if not a hex digit.
 *
 * @details Maps an ASCII hexadecimal character ('0'-'9', 'a'-'f', 'A'-'F') to
 * its numeric value (0-15). The digit is first lower-cased via priv_ra8_reflow_css_lower() so
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
RA8_INTERNAL
static uint8_t internal_hex_val(char c)
{
  if ((c >= '0') && (c <= '9')) {
    return (uint8_t)(c - '0');
  }
  const char l = priv_ra8_reflow_css_lower(c);
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
RA8_INTERNAL
static uint32_t internal_parse_hex_color(const char* s, size_t len)
{
  uint32_t rgb = 0U;
  if (len == (size_t)k_priv_hex6_len) {
    /* Bounded: exactly k_priv_hex6_len digits. */
    for (size_t i = 0U; i < len; ++i) {
      const uint8_t v = internal_hex_val(s[i]);
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
      const uint8_t v = internal_hex_val(s[i]);
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
 *   - `#rgb` and `#rrggbb` hex notations, delegated to internal_parse_hex_color().
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
RA8_INTERNAL
static uint32_t internal_parse_color(const char* s, size_t len)
{
  /* mcdc-deactivated: the sole caller internal_apply_decl passes val (non-NULL, from priv_ra8_reflow_css_trim) and vlen > 0 (gated by (plen > 0U) && (vlen > 0U) in priv_ra8_reflow_css_parse_decls); (s == nullptr) and (len == 0U) are unreachable on any public path. */
  if ((s == nullptr) || (len == 0U)) {
    return (uint32_t)k_priv_col_invalid;
  }
  if (s[0] == '#') {
    return internal_parse_hex_color(&s[1], len - 1U);
  }
  if (priv_ra8_reflow_css_ci_eq(s, len, "black")) {
    return (uint32_t)k_priv_col_black;
  }
  if (priv_ra8_reflow_css_ci_eq(s, len, "white")) {
    return (uint32_t)k_priv_col_white;
  }
  if (priv_ra8_reflow_css_ci_eq(s, len, "red")) {
    return (uint32_t)k_priv_col_red;
  }
  if (priv_ra8_reflow_css_ci_eq(s, len, "green")) {
    return (uint32_t)k_priv_col_green;
  }
  if (priv_ra8_reflow_css_ci_eq(s, len, "blue")) {
    return (uint32_t)k_priv_col_blue;
  }
  if (priv_ra8_reflow_css_ci_eq(s, len, "gray") || priv_ra8_reflow_css_ci_eq(s, len, "grey")) {
    return (uint32_t)k_priv_col_gray;
  }
  if (priv_ra8_reflow_css_ci_eq(s, len, "silver")) {
    return (uint32_t)k_priv_col_silver;
  }
  if (priv_ra8_reflow_css_ci_eq(s, len, "maroon")) {
    return (uint32_t)k_priv_col_maroon;
  }
  if (priv_ra8_reflow_css_ci_eq(s, len, "navy")) {
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
RA8_INTERNAL
static uint32_t internal_scan_hundredths(const char* s, size_t len, size_t* i, bool* any)
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
 * @pre @p hund is a value previously produced by internal_scan_hundredths().
 * @pre Caller intends to store the result in a uint16_t font-size field.
 * @post Return value is in [0, k_priv_fs_max]; no state is mutated.
 * @post The function is pure; @p hund is not modified.
 *
 * @note Thread-safe; pure function with no shared state.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_fs_whole(uint32_t hund)
{
  const uint32_t whole = hund / (uint32_t)k_priv_fs_pct1;
  return (uint16_t)((whole > (uint32_t)k_priv_fs_max) ? (uint32_t)k_priv_fs_max : whole);
}

/**
 * @brief Parse `Npx` / `N%` / `Nem` (N may be fractional) into value + unit.
 *
 * @details Delegates numeric scanning to internal_scan_hundredths(), then classifies
 * the trailing unit suffix:
 *   - "px": writes the clamped whole value to @p *out_val and k_ra8_css_font_px
 *     to @p *out_unit.
 *   - "%": identical whole-value rounding, unit set to k_ra8_css_font_pct.
 *   - "em": the hundredths value is itself the percentage (1em = 100%); stored
 *     clamped to k_priv_fs_max and unit k_ra8_css_font_pct.
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
 *       valid ra8_css_font_unit value.
 * @post On false, @p *out_val and @p *out_unit are not written.
 *
 * @note Thread-safe; operates only on caller-owned state.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_parse_fontsize(const char* s, size_t len, uint16_t* out_val, uint8_t* out_unit)
{
  size_t   i    = 0U;
  bool     any  = false;
  uint32_t hund = internal_scan_hundredths(s, len, &i, &any);
  if (!any) {
    return false;
  }
  const size_t rem = len - i;
  if ((rem == (size_t)k_priv_fs_ulen) && priv_ra8_reflow_css_ci_eq(&s[i], rem, "px")) {
    *out_val  = internal_fs_whole(hund);
    *out_unit = (uint8_t)k_ra8_css_font_px;
    return true;
  }
  if ((rem == 1U) && (s[i] == '%')) {
    *out_val  = internal_fs_whole(hund);
    *out_unit = (uint8_t)k_ra8_css_font_pct;
    return true;
  }
  if ((rem == (size_t)k_priv_fs_ulen) && priv_ra8_reflow_css_ci_eq(&s[i], rem, "em")) {
    /* 1em = 100%; `hund` is value*100, which is already the percent. */
    *out_val  = (uint16_t)((hund > (uint32_t)k_priv_fs_max) ? (uint32_t)k_priv_fs_max : hund);
    *out_unit = (uint8_t)k_ra8_css_font_pct;
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
RA8_INTERNAL
static void internal_set_emphasis(ra8_css_style_t* out, uint8_t setbit, uint8_t stylebit, bool on)
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
 * Delegates mutation to internal_set_emphasis(). Returns false for any other
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
RA8_INTERNAL
static bool internal_apply_emphasis(const char*      prop,
                                    size_t           plen,
                                    const char*      val,
                                    size_t           vlen,
                                    ra8_css_style_t* out)
{
  if (priv_ra8_reflow_css_ci_eq(prop, plen, "font-weight")) {
    const bool on =
      priv_ra8_reflow_css_ci_eq(val, vlen, "bold") ||
      priv_ra8_reflow_css_ci_eq(val, vlen, "bolder") ||
      priv_ra8_reflow_css_ci_eq(val, vlen, "600") || priv_ra8_reflow_css_ci_eq(val, vlen, "700") ||
      priv_ra8_reflow_css_ci_eq(val, vlen, "800") || priv_ra8_reflow_css_ci_eq(val, vlen, "900");
    internal_set_emphasis(out, (uint8_t)k_ra8_css_set_bold, (uint8_t)k_ra8_reflow_style_bold, on);
    return true;
  }
  if (priv_ra8_reflow_css_ci_eq(prop, plen, "font-style")) {
    const bool on = priv_ra8_reflow_css_ci_eq(val, vlen, "italic") ||
                    priv_ra8_reflow_css_ci_eq(val, vlen, "oblique");
    internal_set_emphasis(out,
                          (uint8_t)k_ra8_css_set_italic,
                          (uint8_t)k_ra8_reflow_style_italic,
                          on);
    return true;
  }
  if (priv_ra8_reflow_css_ci_eq(prop, plen, "text-decoration") ||
      priv_ra8_reflow_css_ci_eq(prop, plen, "text-decoration-line")) {
    const bool on = internal_ci_contains(val, vlen, "underline");
    internal_set_emphasis(out,
                          (uint8_t)k_ra8_css_set_underline,
                          (uint8_t)k_ra8_reflow_style_underline,
                          on);
    return true;
  }
  return false;
}

/**
 * @brief Apply a parsed `text-align` value to @p out.
 *
 * @details Sets the k_ra8_css_set_align bit in @p out->set and assigns
 * @p out->align to the matching ra8_reflow_align constant:
 *   - "right"   -> k_ra8_reflow_align_right
 *   - "center"  -> k_ra8_reflow_align_center
 *   - "justify" -> k_ra8_reflow_align_justify
 *   - any other -> k_ra8_reflow_align_left (CSS default)
 * Comparison is case-insensitive via priv_ra8_reflow_css_ci_eq().
 *
 * @param[in]     val  Value span for the text-align property.
 * @param[in]     vlen Length of @p val in bytes.
 * @param[in,out] out  Style record to update; must not be NULL.
 *
 * @return Nothing.
 *
 * @pre @p val points to at least @p vlen readable bytes.
 * @pre @p out is non-NULL.
 * @post @p out->set has k_ra8_css_set_align OR'd in.
 * @post @p out->align holds the parsed alignment constant.
 *
 * @note Thread-safe with respect to distinct @p out instances.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_apply_align(const char* val, size_t vlen, ra8_css_style_t* out)
{
  out->set = (uint8_t)(out->set | (uint8_t)k_ra8_css_set_align);
  if (priv_ra8_reflow_css_ci_eq(val, vlen, "right")) {
    out->align = (uint8_t)k_ra8_reflow_align_right;
  } else if (priv_ra8_reflow_css_ci_eq(val, vlen, "center")) {
    out->align = (uint8_t)k_ra8_reflow_align_center;
  } else if (priv_ra8_reflow_css_ci_eq(val, vlen, "justify")) {
    out->align = (uint8_t)k_ra8_reflow_align_justify;
  } else {
    out->align = (uint8_t)k_ra8_reflow_align_left;
  }
}

/**
 * @brief Apply one trimmed `prop:value` pair to @p out, setting the matching bit.
 *
 * @details Routes a single CSS declaration to the appropriate handler:
 *   - Emphasis properties (font-weight, font-style, text-decoration) via
 *     internal_apply_emphasis().
 *   - "text-align" via internal_apply_align().
 *   - "color": parsed via internal_parse_color(); applied only on a valid result.
 *   - "font-size": parsed via internal_parse_fontsize(); applied only on success.
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
RA8_INTERNAL
static void internal_apply_decl(const char*      prop,
                                size_t           plen,
                                const char*      val,
                                size_t           vlen,
                                ra8_css_style_t* out)
{
  if (internal_apply_emphasis(prop, plen, val, vlen, out)) {
    return;
  }
  if (priv_ra8_reflow_css_ci_eq(prop, plen, "text-align")) {
    internal_apply_align(val, vlen, out);
  } else if (priv_ra8_reflow_css_ci_eq(prop, plen, "color")) {
    const uint32_t rgb = internal_parse_color(val, vlen);
    if (rgb != (uint32_t)k_priv_col_invalid) {
      out->set   = (uint8_t)(out->set | (uint8_t)k_ra8_css_set_color);
      out->color = rgb;
    }
  } else if (priv_ra8_reflow_css_ci_eq(prop, plen, "font-size")) {
    uint16_t fv = 0U;
    uint8_t  fu = 0U;
    if (internal_parse_fontsize(val, vlen, &fv, &fu)) {
      out->set       = (uint8_t)(out->set | (uint8_t)k_ra8_css_set_fontsize);
      out->font_val  = fv;
      out->font_unit = fu;
    }
  } else if (priv_ra8_reflow_css_ci_eq(prop, plen, "display")) {
    out->set     = (uint8_t)(out->set | (uint8_t)k_ra8_css_set_display);
    out->display = priv_ra8_reflow_css_ci_eq(val, vlen, "none") ? 1U : 0U;
  } else {
    /* Unknown property -> ignore (no set bit). */
  }
}

/**
 * @brief Parse a `prop:value; ...` declaration body (no braces) into @p out.
 *
 * @details Iterates over semicolon-delimited declarations in @p s[0..len).
 * For each declaration, it locates the colon separator, trims whitespace from
 * both property name and value, and delegates to internal_apply_decl(). Declarations
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
void priv_ra8_reflow_css_parse_decls(const char* s, size_t len, ra8_css_style_t* out)
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
      const char* prop = priv_ra8_reflow_css_trim(&s[i], &plen);
      size_t      vlen = semi - (colon + 1U);
      const char* val  = priv_ra8_reflow_css_trim(&s[colon + 1U], &vlen);
      if ((plen > 0U) && (vlen > 0U)) {
        internal_apply_decl(prop, plen, val, vlen, out);
      }
    }
    i = semi + 1U;
  }
}

/* ===========================================================================
 * Public API -- inline declarations
 * ===========================================================================
 */

ra8_err_t ra8_css_parse_inline(const char* decls, uint32_t len, ra8_css_style_t* out)
{
  if ((decls == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = (ra8_css_style_t){};
  priv_ra8_reflow_css_parse_decls(decls, (size_t)len, out);
  return k_ra8_ok;
}
