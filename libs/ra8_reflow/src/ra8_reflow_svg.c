/**
 * @file ra8_reflow_svg.c
 * @brief Scan / attribute / colour-parsing core of the minimal SVG subset (#112).
 *
 * @details Pure string-scanning leaf helpers for the SVG subset: XML whitespace
 * and ASCII-fold classifiers, case-insensitive literal scans, integer parsing,
 * attribute slicing, colour-keyword / hex-paint parsing, and the `fill` /
 * gradient-reference resolver. The transform, shape, path, and document-walk
 * stages live in the sibling ra8_reflow_svg_*.c files. No DOM, no heap. See
 * ra8_reflow_svg.h for the public scope and ra8_reflow_svg_internal.h for the
 * shared geometry types and cross-TU helper contracts.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_reflow_svg.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_reflow_svg_internal.h"

/* ===========================================================================
 * Pure string / number helpers
 * ===========================================================================
 */

/**
 * @brief Test whether a character is XML whitespace.
 *
 * @details Classifies @p c as an XML whitespace character according to the XML
 * 1.0 specification: space (0x20), horizontal tab (0x09), line feed (0x0A),
 * carriage return (0x0D), or form feed (0x0C). Used throughout the parser to
 * skip inter-token gaps and attribute boundaries without invoking libc locale.
 *
 * @param[in] c Character to test.
 *
 * @return bool Classification result.
 * @retval true  @p c is one of the five XML whitespace characters.
 * @retval false @p c is not an XML whitespace character.
 *
 * @pre  @p c is any value representable by @c char (no range restriction).
 * @pre  No state or side-effects are required before calling this function.
 * @post The return value is one of exactly {true, false}.
 * @post No external state is modified by this function.
 *
 * @note Not thread-safe in isolation; all callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
bool ra8_svgp_ws(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r') || (c == '\f');
}

/**
 * @brief ASCII-fold a character to lower case.
 *
 * @details Converts uppercase ASCII letters ('A'-'Z') to their lowercase
 * equivalents by adding the ('a' - 'A') offset. All other characters,
 * including non-ASCII bytes, digits, punctuation, and control characters,
 * are returned unchanged. Used by case-insensitive string comparisons
 * throughout the SVG parser (element names, attribute names, colour names).
 *
 * @param[in] c Character to fold.
 *
 * @return char The lowercase equivalent of @p c, or @p c unchanged.
 * @retval 'a'..'z' When @p c was an uppercase ASCII letter.
 * @retval c        When @p c was already lowercase or not an ASCII letter.
 *
 * @pre  @p c is any value representable by @c char.
 * @pre  No module state is required before calling this function.
 * @post The return value is an ASCII lowercase letter when @p c was an uppercase letter.
 * @post No external state is modified by this function.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
char ra8_svgp_lc(char c)
{
  return (char)(((c >= 'A') && (c <= 'Z')) ? (c + ('a' - 'A')) : c);
}

/**
 * @brief Test whether a byte span starts with a literal at a given offset,
 *        case-insensitively.
 *
 * @details Computes the length of @p lit via @c strlen, then verifies that
 * @p s[@p at .. @p at+n) matches @p lit character-for-character after folding
 * both sides through @c ra8_svgp_lc. Returns false immediately when the remaining
 * span @p s[@p at .. @p len) is shorter than @p lit, avoiding any out-of-bounds
 * access. Matching is done byte-by-byte with no locale dependence.
 *
 * @param[in] s   Byte buffer to search; must not be NULL.
 * @param[in] len Total number of valid bytes in @p s.
 * @param[in] at  Byte offset within @p s at which to start the comparison.
 * @param[in] lit NUL-terminated ASCII literal to match; must not be NULL.
 *
 * @return bool Result of the case-insensitive prefix test.
 * @retval true  @p s[@p at .. @p at+strlen(lit)) matches @p lit case-insensitively.
 * @retval false @p at + strlen(@p lit) > @p len, or any character differs.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p lit is a valid NUL-terminated ASCII string.
 *
 * @post @p s and @p lit are not modified.
 * @post @p at and @p len are not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
bool ra8_svgp_starts_ci(const uint8_t* s, size_t len, size_t at, const char* lit)
{
  const size_t n = strlen(lit);
  if ((at + n) > len) {
    return false;
  }
  for (size_t k = 0U; k < n; ++k) {
    if (ra8_svgp_lc((char)s[at + k]) != ra8_svgp_lc(lit[k])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Find the first case-insensitive occurrence of a literal in a byte span.
 *
 * @details Scans @p s[@p from .. @p len) for the first position where
 * @c ra8_svgp_starts_ci returns true for @p lit. If @p lit is empty (zero-length),
 * returns @p len immediately as a defined sentinel rather than matching at every
 * position. The inner loop advances the position by one byte per iteration;
 * the loop bound is @p len - strlen(@p lit) + 1, so the scan is O(len * len(lit)).
 *
 * @param[in] s    Byte buffer to search; must not be NULL.
 * @param[in] len  Total valid bytes in @p s.
 * @param[in] from Starting byte offset; must be <= @p len.
 * @param[in] lit  NUL-terminated ASCII literal to find; must not be NULL.
 *
 * @return size_t Byte offset of the first match, or @p len if not found.
 * @retval [from..len) Offset of the first case-insensitive occurrence of @p lit.
 * @retval len         @p lit was not found, or @p lit is empty.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p from is <= @p len.
 *
 * @post @p s and @p lit are not modified.
 * @post The return value is always in the closed range [@p from, @p len].
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
size_t ra8_svgp_find_ci(const uint8_t* s, size_t len, size_t from, const char* lit)
{
  const size_t n = strlen(lit);
  if (n == 0U) {
    return len;
  }
  size_t i = from;
  /* Bounded: i advances by 1 each step, capped by len - n + 1. */
  while ((i + n) <= len) {
    if (ra8_svgp_starts_ci(s, len, i, lit)) {
      return i;
    }
    ++i;
  }
  return len;
}

/**
 * @brief Convert a single hexadecimal digit character to its numeric value.
 *
 * @details Accepts '0'-'9', 'a'-'f', and 'A'-'F'. The alphabetic check is
 * performed after ASCII-folding via @c ra8_svgp_lc so both cases are handled
 * uniformly. Returns the sentinel ::k_svg_hex_base (16) for any character
 * that is not a valid hexadecimal digit, allowing callers to detect and
 * propagate parse failures without a separate validity flag.
 *
 * @param[in] c Character to convert; expected to be a hex digit.
 *
 * @return uint8_t Numeric value of the hex digit, or the sentinel on failure.
 * @retval 0..9                For '0'-'9'.
 * @retval 10..15              For 'a'-'f' or 'A'-'F'.
 * @retval (uint8_t)k_svg_hex_base  @p c is not a hexadecimal digit.
 *
 * @pre  @p c is any value representable by @c char.
 * @pre  Callers must check the return value against ::k_svg_hex_base before use.
 *
 * @post The return value is in [0, k_svg_hex_base].
 * @post @p c is not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_hex(char c)
{
  if ((c >= '0') && (c <= '9')) {
    return (uint8_t)(c - '0');
  }
  const char l = ra8_svgp_lc(c);
  if ((l >= 'a') && (l <= 'f')) {
    return (uint8_t)((l - 'a') + (int)k_svg_hex_a10);
  }
  return (uint8_t)k_svg_hex_base;
}

/**
 * @brief Parse the integer part of one SVG number at @p s[*i], skipping leading
 *        separators (whitespace / commas); truncates any fraction.
 *
 * @details First advances @p *i past any leading whitespace or comma
 * separators. Then reads an optional sign ('+'/'-') followed by consecutive
 * decimal digit characters to form an @c int32_t. If a decimal point is found
 * immediately after the integer digits, the fractional digits are consumed and
 * discarded so that the next call begins at the correct position. On return
 * @p *i points one past the last character consumed.
 *
 * @param[in]     s   Byte buffer holding the SVG text; must not be NULL.
 * @param[in]     len Total valid bytes in @p s.
 * @param[in,out] i   Current parse cursor; updated to one past the consumed number.
 *
 * @return int32_t The signed integer value parsed from the current cursor position.
 * @retval 0  No digit characters were found after the sign (or no sign either).
 * @retval n  The value of the decimal integer scanned from @p s[*i].
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p i is not NULL, and @p *i is <= @p len.
 *
 * @post @p *i is advanced past any whitespace, sign, integer digits, and fraction.
 * @post @p *i <= @p len.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
int32_t ra8_svgp_num(const uint8_t* s, size_t len, size_t* i)
{
  while ((*i < len) && (ra8_svgp_ws((char)s[*i]) || (s[*i] == ','))) {
    ++(*i);
  }
  int32_t sign = 1;
  if ((*i < len) && ((s[*i] == '-') || (s[*i] == '+'))) {
    sign = (s[*i] == '-') ? -1 : 1;
    ++(*i);
  }
  int32_t v = 0;
  while ((*i < len) && (s[*i] >= '0') && (s[*i] <= '9')) {
    v = (v * (int32_t)k_svg_dec) + (int32_t)(s[*i] - '0');
    ++(*i);
  }
  /* Skip a fractional part (truncated). */
  if ((*i < len) && (s[*i] == '.')) {
    ++(*i);
    while ((*i < len) && (s[*i] >= '0') && (s[*i] <= '9')) {
      ++(*i);
    }
  }
  return v * sign;
}

/* ===========================================================================
 * Attribute + colour parsing
 * ===========================================================================
 */

/**
 * @brief Test whether an attribute keyword begins at @p s[@p at] on a word boundary.
 *
 * @details Uses @c ra8_svgp_starts_ci to confirm the case-insensitive match of @p name
 * at position @p at, then validates two boundary conditions: the character
 * immediately before @p at (when @p at > 0) must be XML whitespace so that
 * a substring of a longer name is not accepted, and the character immediately
 * after the matched name must be '=' or XML whitespace. These three checks
 * together ensure that only a correctly delimited attribute keyword matches.
 *
 * @param[in] s    Byte buffer containing the tag text; must not be NULL.
 * @param[in] len  Total valid bytes in @p s.
 * @param[in] at   Byte offset at which to test for the attribute name.
 * @param[in] name NUL-terminated ASCII attribute name to match; must not be NULL.
 *
 * @return bool Result of the word-boundary attribute name test.
 * @retval true  @p name matches at @p at with valid surrounding delimiters.
 * @retval false @p name does not match, or a boundary condition fails.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p name is a valid NUL-terminated ASCII string.
 *
 * @post @p s, @p len, @p at, and @p name are not modified.
 * @post No bytes in @p s are written.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_attr_at(const uint8_t* s, size_t len, size_t at, const char* name)
{
  const size_t n = strlen(name);
  if (!ra8_svgp_starts_ci(s, len, at, name)) {
    return false;
  }
  if ((at > 0U) && !ra8_svgp_ws((char)s[at - 1U])) {
    return false; /* must be preceded by whitespace */
  }
  const size_t after = at + n;
  if (after >= len) {
    return false;
  }
  return (s[after] == '=') || ra8_svgp_ws((char)s[after]);
}

/**
 * @brief Find attribute @p name in tag span @p s[0..len); return its value slice.
 *
 * @details Scans the tag byte span for the first occurrence of @p name that
 * passes the word-boundary check via @c priv_attr_at. Once located, the
 * scanner advances past the '=' separator and any surrounding whitespace to
 * find the opening quote character ('"' or "'"). The matching closing quote is
 * then located and the slice [@p *voff, @p *voff + @p *vlen) is set to the
 * attribute value content between the two quote characters. Returns false if
 * the name is not found, no '=' follows, or no opening quote is present.
 *
 * @param[in]  s     Byte buffer containing the tag text; must not be NULL.
 * @param[in]  len   Total valid bytes in @p s.
 * @param[in]  name  NUL-terminated ASCII attribute name to find; must not be NULL.
 * @param[out] voff  Set to the byte offset of the first value character when found.
 * @param[out] vlen  Set to the byte length of the value when found.
 *
 * @return bool Whether the attribute was found and parsed.
 * @retval true  Attribute @p name found; @p *voff and @p *vlen are valid.
 * @retval false Attribute not found, missing '=', or missing opening quote.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p voff and @p vlen are valid non-NULL output pointers.
 *
 * @post On true: @p *voff < @p len and @p *voff + @p *vlen <= @p len.
 * @post On false: @p *voff and @p *vlen are indeterminate and must not be used.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
bool ra8_svgp_attr(const uint8_t* s, size_t len, const char* name, size_t* voff, size_t* vlen)
{
  /* Bounded: i advances by 1 each step, capped by len. */
  for (size_t i = 0U; i < len; ++i) {
    if (!priv_attr_at(s, len, i, name)) {
      continue;
    }
    size_t j = i + strlen(name);
    while ((j < len) && (s[j] != '=')) {
      ++j;
    }
    ++j; /* past '=' */
    while ((j < len) && ra8_svgp_ws((char)s[j])) {
      ++j;
    }
    if ((j >= len) || ((s[j] != '"') && (s[j] != '\''))) {
      return false;
    }
    const char q = (char)s[j];
    ++j;
    const size_t start = j;
    while ((j < len) && (s[j] != (uint8_t)q)) {
      ++j;
    }
    *voff = start;
    *vlen = j - start;
    return true;
  }
  return false;
}

/**
 * @brief Read an SVG attribute as a signed integer, returning a default when absent.
 *
 * @details Calls @c ra8_svgp_attr to locate the attribute value slice, then forwards
 * that slice to @c ra8_svgp_num starting at offset zero. If the attribute is absent,
 * returns @p def without modifying any output. Fractional parts of the attribute
 * value are truncated by @c ra8_svgp_num. Useful for reading integer-valued SVG
 * geometry attributes such as @c x, @c y, @c width, and @c height.
 *
 * @param[in] s    Byte buffer containing the SVG tag text; must not be NULL.
 * @param[in] len  Total valid bytes in @p s.
 * @param[in] name NUL-terminated ASCII attribute name; must not be NULL.
 * @param[in] def  Value to return when the attribute is absent.
 *
 * @return int32_t The parsed integer value, or @p def when the attribute is absent.
 * @retval def     Attribute @p name was not found in @p s.
 * @retval n       The signed integer parsed from the attribute value.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p name is a valid NUL-terminated ASCII string.
 *
 * @post @p s and @p name are not modified.
 * @post The return value equals @p def when @c ra8_svgp_attr returns false.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
int32_t ra8_svgp_attr_num(const uint8_t* s, size_t len, const char* name, int32_t def)
{
  size_t off = 0U;
  size_t vl  = 0U;
  if (!ra8_svgp_attr(s, len, name, &off, &vl)) {
    return def;
  }
  size_t k = 0U;
  return ra8_svgp_num(&s[off], vl, &k);
}

/**
 * @brief Case-insensitive equality test between a byte span and a NUL-terminated literal.
 *
 * @details Advances a shared index through both @p s and @p lit simultaneously,
 * comparing each pair of characters after folding through @c ra8_svgp_lc. The loop
 * terminates as soon as a mismatch is found or the NUL terminator in @p lit is
 * reached. Returns true only when the index equals @p len at the same point that
 * @p lit[k] is '\0', ensuring both length equality and content equality.
 *
 * @param[in] s   Byte span to compare; must not be NULL.
 * @param[in] len Number of valid bytes in @p s.
 * @param[in] lit NUL-terminated ASCII string to compare against; must not be NULL.
 *
 * @return bool Whether @p s and @p lit are equal under case folding.
 * @retval true  @p s[@p 0..len) matches @p lit case-insensitively and lengths agree.
 * @retval false Any character differs, or the lengths do not match.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p lit is a valid NUL-terminated ASCII string.
 *
 * @post @p s and @p lit are not modified.
 * @post The return value is deterministic given fixed inputs.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_ci_eq(const uint8_t* s, size_t len, const char* lit)
{
  size_t k = 0U;
  for (; (k < len) && (lit[k] != '\0'); ++k) {
    if (ra8_svgp_lc((char)s[k]) != ra8_svgp_lc(lit[k])) {
      return false;
    }
  }
  return (k == len) && (lit[k] == '\0');
}

/**
 * @brief Parse a hex colour from raw digit bytes (no leading '#') into 0x00RRGGBB.
 *
 * @details Accepts exactly @c k_svg_hex3 (3) or @c k_svg_hex6 (6) digit bytes.
 * For a 6-digit input the bytes are read as pairs RR GG BB; each nibble is
 * decoded via @c priv_hex and shifted into the 24-bit result. For a 3-digit
 * input each nibble @c N is expanded to @c NN (i.e. the nibble value is placed
 * in both the high and low nibble of the channel byte) to conform to the CSS
 * shorthand rule. Any non-hex digit encountered causes an early return of
 * ::k_svg_no_paint. Any length other than 3 or 6 also returns ::k_svg_no_paint.
 *
 * @param[in] s   Pointer to the first hex digit byte (no '#'); must not be NULL.
 * @param[in] len Number of digit bytes available at @p s; must be 3 or 6 for success.
 *
 * @return uint32_t Parsed colour in 0x00RRGGBB format, or the sentinel on failure.
 * @retval 0x000000..0xFFFFFF  Valid 24-bit colour when @p len is 3 or 6 and all digits are hex.
 * @retval (uint32_t)k_svg_no_paint  @p len is neither 3 nor 6, or a non-hex digit is present.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p len equals @c k_svg_hex3 or @c k_svg_hex6 for a successful parse.
 *
 * @post @p s is not modified.
 * @post The return value has bits [31:24] clear (always 0x00RRGGBB).
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_hex_color(const uint8_t* s, size_t len)
{
  uint32_t rgb = 0U;
  if (len == (size_t)k_svg_hex6) {
    for (size_t i = 0U; i < len; ++i) {
      const uint8_t v = priv_hex((char)s[i]);
      if (v >= (uint8_t)k_svg_hex_base) {
        return (uint32_t)k_svg_no_paint;
      }
      rgb = (rgb << (uint32_t)k_svg_hex_nib) | (uint32_t)v;
    }
    return rgb;
  }
  if (len == (size_t)k_svg_hex3) {
    for (size_t i = 0U; i < len; ++i) {
      const uint8_t v = priv_hex((char)s[i]);
      if (v >= (uint8_t)k_svg_hex_base) {
        return (uint32_t)k_svg_no_paint;
      }
      rgb =
        (rgb << (uint32_t)k_svg_hex_chan) | ((uint32_t)v << (uint32_t)k_svg_hex_nib) | (uint32_t)v;
    }
    return rgb;
  }
  return (uint32_t)k_svg_no_paint;
}

/**
 * @brief Map a named SVG colour keyword to its 0x00RRGGBB value.
 *
 * @details Linearly searches a compile-time table of the twelve most common
 * SVG/CSS colour keywords (black, white, red, green, blue, gray, grey, silver,
 * maroon, navy, yellow, orange). Comparison is performed via @c priv_ci_eq so
 * the keyword is accepted in any mix of upper and lower case. Returns
 * ::k_svg_no_paint for the keyword "none" and for any name not present in the
 * table; callers treat this sentinel as "no paint / transparent".
 *
 * @param[in] s   Byte span holding the colour keyword; must not be NULL.
 * @param[in] len Number of valid bytes in @p s.
 *
 * @return uint32_t The 24-bit RGB colour, or the no-paint sentinel.
 * @retval 0x000000..0xFFFFFF  When @p s matches one of the known colour names.
 * @retval (uint32_t)k_svg_no_paint  When @p s is "none" or an unknown name.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p s contains only ASCII characters (colour keywords are ASCII-only).
 *
 * @post @p s is not modified.
 * @post The return value has bits [31:24] clear (always 0x00RRGGBB or the sentinel).
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_named_color(const uint8_t* s, size_t len)
{
  static const struct {
    const char* name; /**< Name. */
    uint32_t    rgb;  /**< RGB.  */
  } k_names[] = {
    {"black", 0x000000U},
    {"white", 0xFFFFFFU},
    {"red", 0xFF0000U},
    {"green", 0x008000U},
    {"blue", 0x0000FFU},
    {"gray", 0x808080U},
    {"grey", 0x808080U},
    {"silver", 0xC0C0C0U},
    {"maroon", 0x800000U},
    {"navy", 0x000080U},
    {"yellow", 0xFFFF00U},
    {"orange", 0xFFA500U},
  };
  for (size_t k = 0U; k < (sizeof(k_names) / sizeof(k_names[0])); ++k) {
    if (priv_ci_eq(s, len, k_names[k].name)) {
      return k_names[k].rgb;
    }
  }
  return (uint32_t)k_svg_no_paint;
}

/**
 * @brief Parse an SVG paint value ('#rgb'/'#rrggbb'/name/'none') to 0x00RRGGBB.
 *
 * @details Routes the span to either @c priv_hex_color (when the first byte is
 * '#', strip it first) or @c priv_named_color for keyword colours. An empty
 * span immediately returns ::k_svg_no_paint. The keyword "none" is handled by
 * @c priv_named_color returning ::k_svg_no_paint so callers skip the paint.
 *
 * @param[in] s   Byte span containing the paint value; must not be NULL.
 * @param[in] len Number of valid bytes in @p s.
 *
 * @return uint32_t The 24-bit RGB colour, or ::k_svg_no_paint on failure.
 * @retval 0x000000..0xFFFFFF  Successfully parsed solid colour.
 * @retval (uint32_t)k_svg_no_paint  Empty span, "none", or unrecognised value.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p len is the exact byte length of the attribute value to parse.
 *
 * @post @p s is not modified.
 * @post The return value has bits [31:24] clear.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
uint32_t ra8_svgp_paint(const uint8_t* s, size_t len)
{
  if (len == 0U) {
    return (uint32_t)k_svg_no_paint;
  }
  if (s[0] == '#') {
    return priv_hex_color(&s[1], len - 1U);
  }
  return priv_named_color(s, len);
}

/**
 * @brief Read an SVG attribute as a paint colour, returning a default when absent.
 *
 * @details Calls @c ra8_svgp_attr to locate the attribute value slice, then
 * forwards that slice to @c ra8_svgp_paint. If the attribute is absent, returns
 * @p def unchanged. Useful for reading 'fill' and 'stroke' attributes where
 * a sentinel of ::k_svg_no_paint means the shape uses no paint for that role.
 *
 * @param[in] s    Byte buffer containing the SVG tag text; must not be NULL.
 * @param[in] len  Total valid bytes in @p s.
 * @param[in] name NUL-terminated ASCII attribute name; must not be NULL.
 * @param[in] def  Default paint value returned when the attribute is absent.
 *
 * @return uint32_t The parsed paint colour, or @p def when the attribute is absent.
 * @retval def                       Attribute @p name was not found in @p s.
 * @retval 0x000000..0xFFFFFF        Successfully parsed solid paint colour.
 * @retval (uint32_t)k_svg_no_paint  Attribute present but parses to no-paint.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p name is a valid NUL-terminated ASCII string.
 *
 * @post @p s and @p name are not modified.
 * @post The return value equals @p def when the attribute is absent.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
uint32_t ra8_svgp_attr_paint(const uint8_t* s, size_t len, const char* name, uint32_t def)
{
  size_t off = 0U;
  size_t vl  = 0U;
  if (!ra8_svgp_attr(s, len, name, &off, &vl)) {
    return def;
  }
  return ra8_svgp_paint(&s[off], vl);
}

/** @brief Implementation of `priv_match_grad()` -- linear id search over the document gradient set. */
int32_t priv_match_grad(const svg_grads_t* grads, const uint8_t* val, size_t vlen)
{
  if ((grads == nullptr) || !ra8_svgp_starts_ci(val, vlen, 0U, "url(#")) {
    return -1;
  }
  const size_t idoff = strlen("url(#");
  size_t       idend = idoff;
  while ((idend < vlen) && (val[idend] != ')')) {
    ++idend;
  }
  const size_t idlen = idend - idoff;
  /* Bounded: <= grads->n (<= k_svg_grad_max) gradients. */
  for (int32_t i = 0; i < grads->n; ++i) {
    const char* gid = grads->g[i].id;
    if ((strlen(gid) == idlen) && (memcmp(gid, &val[idoff], idlen) == 0)) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Resolve a shape's 'fill': a solid colour, or a gradient index via @p gi.
 *
 * @details Sets @p *gi to the matched gradient index (>= 0) for a
 * 'fill="url(#id)"' that resolves in @p t->grads, returning ::k_svg_no_paint
 * so the solid path is skipped. A 'url(#id)' with no match also returns
 * ::k_svg_no_paint with @p *gi == -1 (graceful skip). An absent 'fill'
 * returns @p def; otherwise the solid colour parsed from the attribute.
 *
 * @param[in]  s    Byte buffer containing the SVG tag text; must not be NULL.
 * @param[in]  len  Total valid bytes in @p s.
 * @param[in]  t    Active coordinate transform carrying the gradient set pointer.
 * @param[in]  def  Default solid colour when the 'fill' attribute is absent.
 * @param[out] gi   Set to the matched gradient index, or -1 when not a gradient.
 *
 * @return uint32_t The resolved solid colour, or ::k_svg_no_paint for gradient fills.
 * @retval def                       'fill' attribute is absent.
 * @retval (uint32_t)k_svg_no_paint  'fill' is a 'url(#id)' reference (gradient).
 * @retval 0x000000..0xFFFFFF        Solid colour from the 'fill' attribute.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p gi is a valid non-NULL output pointer.
 *
 * @post @p *gi is always written before return.
 * @post When @p *gi >= 0, the return value is ::k_svg_no_paint.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
uint32_t
ra8_svgp_resolve_fill(const uint8_t* s, size_t len, const svg_xform_t* t, uint32_t def, int32_t* gi)
{
  *gi        = -1;
  size_t off = 0U;
  size_t vl  = 0U;
  if (!ra8_svgp_attr(s, len, "fill", &off, &vl)) {
    return def;
  }
  if (ra8_svgp_starts_ci(&s[off], vl, 0U, "url(#")) {
    *gi = priv_match_grad(t->grads, &s[off], vl);
    return (uint32_t)k_svg_no_paint;
  }
  return ra8_svgp_paint(&s[off], vl);
}
