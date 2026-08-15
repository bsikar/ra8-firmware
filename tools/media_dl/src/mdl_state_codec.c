/**
 * @file mdl_state_codec.c
 * @brief Portable parser for persistent series state payloads.
 * @details Streams exact schema-v1/v2/v3/v4 records through bounded fw_fs reads.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "mdl_state.h"
#include "mdl_state_internal.h"
#include "ra8_attributes.h"

static_assert(sizeof(long) <= sizeof(int64_t), "legacy state requires long no wider than int64");

/** @brief Parser limits and radices. */
typedef enum : uint16_t {
  k_state_max_flds           = 10,  /**< Max TAB fields split from a line. */
  k_state_hex_base           = 16,  /**< Radix for the hex hash fields.    */
  k_state_dec_base           = 10,  /**< Radix for the decimal fields.     */
  k_state_hex_alpha_base     = 10,  /**< Value represented by ASCII `a`.   */
  k_state_decimal_digits_max = 17,  /**< Legacy writer precision bound.    */
  k_state_decimal_exp_max    = 400, /**< Bounded legacy decimal scale.     */
} mdl_state_parse_t;

/** @brief Field index of each column on a `C` (chapter) record line. */
typedef enum : uint8_t {
  k_c_id        = 1,  /**< Chapter identifier.               */
  k_c_known     = 2,  /**< Explicit parsed-number flag (v2). */
  k_c_number    = 3,  /**< Parsed chapter number (v2).       */
  k_c_done      = 4,  /**< Complete flag (0/1).              */
  k_c_pages     = 5,  /**< Total page count.                 */
  k_c_ready     = 6,  /**< Pages fetched + verified.         */
  k_c_epoch     = 7,  /**< Fetch time (epoch s).             */
  k_c_url       = 8,  /**< Source URL.                       */
  k_c_title     = 9,  /**< Display title (v2).               */
  k_c_fields_v2 = 10, /**< Fields a v2 `C` line needs.       */
  k_c_v1_number = 2,  /**< Integral parsed chapter number.   */
  k_c_v1_done   = 3,  /**< Complete flag (0/1).              */
  k_c_v1_pages  = 4,  /**< Total page count.                 */
  k_c_v1_ready  = 5,  /**< Pages fetched + verified.         */
  k_c_v1_epoch  = 6,  /**< Fetch time (epoch s).             */
  k_c_v1_url    = 7,  /**< Source URL.                       */
  k_c_fields_v1 = 8,  /**< Fields a legacy `C` line needs.   */
} mdl_c_col_t;

/** @brief Field index of each column on a `P` (page) record line. */
typedef enum : uint8_t {
  k_p_urlhash   = 1, /**< Source-URL hash (hex).           */
  k_p_content   = 2, /**< Content hash (hex).              */
  k_p_relpath   = 3, /**< Path under the series.           */
  k_p_etag      = 4, /**< Cached ETag (optional).          */
  k_p_lastmod   = 5, /**< Cached Last-Modified (optional). */
  k_p_epoch     = 6, /**< Most recent fetch epoch (v4).    */
  k_p_status    = 7, /**< Most recent HTTP status (v4).    */
  k_p_fields    = 4, /**< Minimum legacy `P` fields.       */
  k_p_fields_v4 = 8, /**< Exact fields a v4 `P` needs.     */
} mdl_p_col_t;

/* ---- parsing ------------------------------------------------------------- */

/**
 * @brief Split one record in place at TAB delimiters.
 * @details @param[in,out] line Mutable record. @param[out] fld Field pointers. @param[in] max Pointer capacity. @return Field count. @retval `max + 1` Too many fields.
 * @pre All pointers are non-NULL. @pre @p max is positive.
 * @post Returned fields alias @p line. @post Excess fields are reported, not truncated silently.
 * @note Does not allocate. @since 0.1.0
 */
RA8_INTERNAL static size_t internal_mdl_state_split_tabs(char* line, char* fld[], size_t max)
{
  size_t n = 0U;
  fld[n]   = line;
  ++n;
  for (char* c = line; (*c != '\0') && (n < max); ++c) {
    if (*c == '\t') {
      *c     = '\0';
      fld[n] = c + 1;
      ++n;
    }
  }
  if ((n == max) && (strchr(fld[max - 1U], '\t') != nullptr)) {
    return max + 1U;
  }
  return n;
}

/**
 * @brief Convert one canonical lowercase ASCII digit.
 * @details @param[in] byte Candidate byte. @param[in] base Radix. @param[out] out Digit value. @return Whether the byte belongs to the radix. @retval false Locale/case variant or out-of-range digit.
 * @pre @p out is non-NULL. @pre @p base is decimal or hexadecimal.
 * @post Success initializes @p out. @post Failure does not accept locale-dependent text.
 * @note Uppercase hexadecimal is intentionally noncanonical. @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_digit(char byte, int base, uint8_t* out)
{
  uint8_t digit = UINT8_MAX;
  if ((byte >= '0') && (byte <= '9')) {
    digit = (uint8_t)(byte - '0');
  } else if ((byte >= 'a') && (byte <= 'f')) {
    digit = (uint8_t)(k_state_hex_alpha_base + (byte - 'a'));
  }
  if ((digit == UINT8_MAX) || ((int)digit >= base)) {
    return false;
  }
  *out = digit;
  return true;
}

/**
 * @brief Parse one exact unsigned decimal or hexadecimal field.
 * @details @param[in] text Canonical digits. @param[in] base Radix. @param[out] out Parsed value. @return Parse result. @retval false Empty, malformed, or overflowing input.
 * @pre @p out is non-NULL. @pre @p base is decimal or hexadecimal.
 * @post Success initializes @p out exactly. @post Failure never wraps arithmetic.
 * @note Parsing is locale and libc-width independent. @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_state_parse_u64_field(const char* text, int base, uint64_t* out)
{
  if ((text == nullptr) || (text[0] == '\0') ||
      ((base != (int)k_state_dec_base) && (base != (int)k_state_hex_base))) {
    return false;
  }
  uint64_t value = 0U;
  for (const char* p = text; *p != '\0'; ++p) {
    uint8_t digit = 0U;
    if (!internal_mdl_state_digit(*p, base, &digit) ||
        (value > ((UINT64_MAX - (uint64_t)digit) / (uint64_t)base))) {
      return false;
    }
    value = (value * (uint64_t)base) + (uint64_t)digit;
  }
  *out = value;
  return true;
}

/**
 * @brief Parse one exact signed 64-bit decimal including INT64_MIN.
 * @details @param[in] text Canonical signed digits. @param[out] out Parsed value. @return Parse result. @retval false Invalid syntax or overflow.
 * @pre @p text and @p out are non-NULL. @pre A plus sign and whitespace are forbidden.
 * @post Success initializes the full int64 range. @post Failure never invokes signed overflow.
 * @note Accumulation uses an unsigned magnitude. @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_parse_i64_field(const char* text, int64_t* out)
{
  if ((text == nullptr) || (text[0] == '\0') || (text[0] == '+')) {
    return false;
  }
  const bool  negative = text[0] == '-';
  const char* digits   = negative ? &text[1] : text;
  if (digits[0] == '\0') {
    return false;
  }
  uint64_t magnitude = 0U;
  if (!internal_mdl_state_parse_u64_field(digits, (int)k_state_dec_base, &magnitude)) {
    return false;
  }
  const uint64_t limit = negative ? ((uint64_t)INT64_MAX + 1U) : (uint64_t)INT64_MAX;
  if (magnitude > limit) {
    return false;
  }
  *out = (negative && (magnitude == limit)) ? INT64_MIN
                                            : (negative ? -(int64_t)magnitude : (int64_t)magnitude);
  return true;
}

/**
 * @brief Parse a target-width legacy signed long field exactly.
 * @details @param[in] text Canonical signed digits. @param[out] out Parsed target value. @return Parse result. @retval false The value is outside `long` on this target.
 * @pre @p text and @p out are non-NULL. @pre The int64 parser contract applies.
 * @post Success initializes @p out. @post Failure performs no narrowing conversion.
 * @note Preserves v1 semantics on ILP32 and LP64. @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_parse_long_field(const char* text, long* out)
{
  int64_t value = 0;
  if (!internal_mdl_state_parse_i64_field(text, &value) || (value < (int64_t)LONG_MIN) ||
      (value > (int64_t)LONG_MAX)) {
    return false;
  }
  *out = (long)value;
  return true;
}

/**
 * @brief Parse one bounded ASCII decimal exponent.
 * @details @param[in,out] cursor Exponent cursor. @param[out] out_exp Signed exponent. @return Parse result. @retval false Missing, trailing, or oversized exponent text.
 * @pre Both pointers are non-NULL. @pre The cursor begins after `e` or `E`.
 * @post Success advances to the terminator. @post Work is bounded by the configured exponent cap.
 * @note A sign is allowed only within the exponent. @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_parse_decimal_exp(const char** cursor, int32_t* out_exp)
{
  const char* p     = *cursor;
  bool        neg   = false;
  int32_t     value = 0;
  if ((*p == '+') || (*p == '-')) {
    neg = *p == '-';
    ++p;
  }
  if ((*p < '0') || (*p > '9')) {
    return false;
  }
  while ((*p >= '0') && (*p <= '9')) {
    if (value > (int32_t)k_state_decimal_exp_max) {
      return false;
    }
    value = (value * (int32_t)k_state_dec_base) + (int32_t)(*p - '0');
    ++p;
  }
  if ((value > (int32_t)k_state_decimal_exp_max) || (*p != '\0')) {
    return false;
  }
  *cursor  = p;
  *out_exp = neg ? -value : value;
  return true;
}

/**
 * @brief Parse one complete locale-free legacy decimal binary64 value.
 * @details @param[in] text Schema-v2 decimal. @param[out] out Correctly rounded value. @return Parse result. @retval false Noncanonical, overlong, overflowed, or underflowed input.
 * @pre @p text and @p out are non-NULL. @pre The target satisfies the binary64 assertions.
 * @post Success initializes the exact nearest-even result. @post Failure accepts no trailing bytes.
 * @note Conversion delegates to the fixed rational engine. @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_parse_double_field(const char* text, double* out)
{
  if ((text == nullptr) || (text[0] == '\0')) {
    return false;
  }
  const char* p        = text;
  const bool  negative = *p == '-';
  if (*p == '-') {
    ++p;
  }
  uint64_t mantissa    = 0U;
  int32_t  fractional  = 0;
  bool     digit       = false;
  bool     nonzero     = false;
  bool     decimal     = false;
  uint8_t  significant = 0U;
  while (((*p >= '0') && (*p <= '9')) || ((*p == '.') && !decimal)) {
    if (*p == '.') {
      decimal = true;
    } else {
      digit   = true;
      nonzero = nonzero || (*p != '0');
      significant += nonzero ? 1U : 0U;
      if (significant > (uint8_t)k_state_decimal_digits_max) {
        return false;
      }
      mantissa = (mantissa * (uint64_t)k_state_dec_base) + (uint64_t)(*p - '0');
      fractional += decimal ? 1 : 0;
      if (fractional > (int32_t)k_state_decimal_exp_max) {
        return false;
      }
    }
    ++p;
  }
  int32_t exponent = 0;
  if ((*p == 'e') || (*p == 'E')) {
    ++p;
    if (!internal_mdl_state_parse_decimal_exp(&p, &exponent)) {
      return false;
    }
  }
  const int32_t scale = exponent - fractional;
  if (!digit || (*p != '\0') || (scale < -(int32_t)k_state_decimal_exp_max) ||
      (scale > (int32_t)k_state_decimal_exp_max) ||
      !priv_mdl_state_decimal_to_binary64(mantissa, scale, negative, out)) {
    return false;
  }
  return true;
}

/**
 * @brief Parse one exact-width canonical binary64 bit identity.
 * @details @param[in] text Sixteen lowercase hexadecimal digits. @param[out] out Decoded value. @return Parse result. @retval false Width, case, syntax, or finiteness violation.
 * @pre @p text and @p out are non-NULL. @pre The target satisfies the binary64 assertions.
 * @post Success preserves every finite bit. @post NaN and infinity remain rejected.
 * @note Decoding uses memcpy rather than type punning. @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_parse_binary64(const char* text, double* out)
{
  if ((text == nullptr) || (strlen(text) != 16U)) {
    return false;
  }
  for (size_t i = 0U; i < 16U; ++i) {
    const bool digit = (text[i] >= '0') && (text[i] <= '9');
    const bool lower = (text[i] >= 'a') && (text[i] <= 'f');
    if (!digit && !lower) {
      return false;
    }
  }
  uint64_t bits = 0U;
  if (!internal_mdl_state_parse_u64_field(text, (int)k_state_hex_base, &bits)) {
    return false;
  }
  memcpy(out, &bits, sizeof(bits));
  return isfinite(*out);
}

/**
 * @brief Apply validated common chapter fields.
 * @details Enforces page/completion invariants and chapter-id uniqueness before
 *          appending a fully populated migrated/current record.
 * @param[in,out] st           State receiving the chapter.
 * @param[in]     fld          Split record fields.
 * @param[in]     number       Parsed chapter number.
 * @param[in]     number_known Explicit number-presence flag.
 * @param[in]     done         Parsed completion flag.
 * @param[in]     pages        Parsed page count.
 * @param[in]     ready        Parsed completed-page count.
 * @param[in]     epoch        Parsed fetch epoch.
 * @param[in]     url_col      Source-URL column index.
 * @param[in]     title        Bounded display title.
 * @return Whether the complete record was accepted.
 * @retval true  One valid chapter was appended.
 * @retval false A field/invariant failed or capacity was exhausted.
 * @pre @p st, @p fld, and @p title are non-NULL.
 * @pre @p url_col addresses an element of @p fld.
 * @post On true, chapter count grows by one.
 * @post On false, no duplicate chapter is appended.
 * @note Not thread-safe: mutates @p st.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_apply_chapter_values(mdl_state_t* st,
                                                                 char*        fld[],
                                                                 double       number,
                                                                 bool         number_known,
                                                                 uint64_t     done,
                                                                 uint64_t     pages,
                                                                 uint64_t     ready,
                                                                 int64_t      epoch,
                                                                 size_t       url_col,
                                                                 const char*  title)
{
  if ((done > 1U) || (pages > UINT16_MAX) || (ready > UINT16_MAX) || (ready > pages) ||
      ((done != 0U) && ((pages == 0U) || (ready != pages))) ||
      (mdl_state_find_chapter(st, fld[k_c_id]) != nullptr)) {
    return false;
  }
  mdl_chapter_rec_t* rec =
    mdl_state_add_chapter_numbered(st, fld[k_c_id], fld[url_col], number, number_known);
  if ((rec == nullptr) || !mdl_state_set_chapter_metadata(rec, title, number, number_known)) {
    return false;
  }
  rec->complete   = done != 0U;
  rec->page_count = (uint16_t)pages;
  rec->pages_done = (uint16_t)ready;
  rec->fetched_at = epoch;
  return true;
}

/**
 * @brief Parse and apply one legacy v1 chapter record.
 * @details Migrates integral nonzero presence semantics into the v2 tuple.
 * @param[in,out] st  State receiving the record.
 * @param[in]     fld Split TAB fields.
 * @param[in]     nf  Number of fields.
 * @return Whether the exact v1 record was accepted.
 * @retval true  A valid migrated chapter was appended.
 * @retval false Field count, numeric syntax, invariants, or capacity failed.
 * @pre @p st and @p fld are non-NULL.
 * @pre @p nf describes accessible entries in @p fld.
 * @post On true, the chapter is represented using v2 in-memory semantics.
 * @post On false, parsing stops without reporting success.
 * @note Not thread-safe: mutates @p st.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_state_apply_chapter_v1(mdl_state_t* st, char* fld[], size_t nf)
{
  if (nf != (size_t)k_c_fields_v1) {
    return false;
  }
  long     number = 0L;
  uint64_t done   = 0U;
  uint64_t pages  = 0U;
  uint64_t ready  = 0U;
  int64_t  epoch  = 0;
  if (!internal_mdl_state_parse_long_field(fld[k_c_v1_number], &number) ||
      !internal_mdl_state_parse_u64_field(fld[k_c_v1_done], (int)k_state_dec_base, &done) ||
      !internal_mdl_state_parse_u64_field(fld[k_c_v1_pages], (int)k_state_dec_base, &pages) ||
      !internal_mdl_state_parse_u64_field(fld[k_c_v1_ready], (int)k_state_dec_base, &ready) ||
      !internal_mdl_state_parse_i64_field(fld[k_c_v1_epoch], &epoch)) {
    return false;
  }
  return internal_mdl_state_apply_chapter_values(st,
                                                 fld,
                                                 (double)number,
                                                 number != 0L,
                                                 done,
                                                 pages,
                                                 ready,
                                                 epoch,
                                                 (size_t)k_c_v1_url,
                                                 "");
}

/**
 * @brief Parse and apply one current v2 chapter record.
 * @details Requires exact field count, Boolean flags, and a finite double.
 * @param[in,out] st  State receiving the record.
 * @param[in]     fld Split TAB fields.
 * @param[in]     nf  Number of fields.
 * @return Whether the exact v2 record was accepted.
 * @retval true  A valid chapter was appended.
 * @retval false Field count, syntax, invariants, or capacity failed.
 * @pre @p st and @p fld are non-NULL.
 * @pre @p nf describes accessible entries in @p fld.
 * @post On true, explicit presence and fractional number are preserved.
 * @post On false, parsing stops without reporting success.
 * @note Not thread-safe: mutates @p st.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_state_apply_chapter_v2(mdl_state_t* st, char* fld[], size_t nf)
{
  if (nf != (size_t)k_c_fields_v2) {
    return false;
  }
  uint64_t known  = 0U;
  double   number = 0.0;
  uint64_t done   = 0U;
  uint64_t pages  = 0U;
  uint64_t ready  = 0U;
  int64_t  epoch  = 0;
  if (!internal_mdl_state_parse_u64_field(fld[k_c_known], (int)k_state_dec_base, &known) ||
      !internal_mdl_state_parse_double_field(fld[k_c_number], &number) ||
      !internal_mdl_state_parse_u64_field(fld[k_c_done], (int)k_state_dec_base, &done) ||
      !internal_mdl_state_parse_u64_field(fld[k_c_pages], (int)k_state_dec_base, &pages) ||
      !internal_mdl_state_parse_u64_field(fld[k_c_ready], (int)k_state_dec_base, &ready) ||
      !internal_mdl_state_parse_i64_field(fld[k_c_epoch], &epoch) || (known > 1U)) {
    return false;
  }
  return internal_mdl_state_apply_chapter_values(st,
                                                 fld,
                                                 number,
                                                 known != 0U,
                                                 done,
                                                 pages,
                                                 ready,
                                                 epoch,
                                                 (size_t)k_c_url,
                                                 fld[k_c_title]);
}

/**
 * @brief Parse and apply one current-schema chapter record.
 * @details @param[in,out] st Destination state. @param[in,out] fld Split fields. @param[in] nf Field count. @return Application result. @retval false Any v3 field or invariant is invalid.
 * @pre @p st and @p fld are non-NULL. @pre The version record selected schema v3.
 * @post Success appends one complete chapter. @post Failure appends no partial chapter.
 * @note The chapter identity is canonical binary64 hex. @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_state_apply_chapter_v3(mdl_state_t* st, char* fld[], size_t nf)
{
  if (nf != (size_t)k_c_fields_v2) {
    return false;
  }
  uint64_t known  = 0U;
  double   number = 0.0;
  uint64_t done   = 0U;
  uint64_t pages  = 0U;
  uint64_t ready  = 0U;
  int64_t  epoch  = 0;
  if (!internal_mdl_state_parse_u64_field(fld[k_c_known], (int)k_state_dec_base, &known) ||
      !internal_mdl_state_parse_binary64(fld[k_c_number], &number) ||
      !internal_mdl_state_parse_u64_field(fld[k_c_done], (int)k_state_dec_base, &done) ||
      !internal_mdl_state_parse_u64_field(fld[k_c_pages], (int)k_state_dec_base, &pages) ||
      !internal_mdl_state_parse_u64_field(fld[k_c_ready], (int)k_state_dec_base, &ready) ||
      !internal_mdl_state_parse_i64_field(fld[k_c_epoch], &epoch) || (known > 1U)) {
    return false;
  }
  return internal_mdl_state_apply_chapter_values(st,
                                                 fld,
                                                 number,
                                                 known != 0U,
                                                 done,
                                                 pages,
                                                 ready,
                                                 epoch,
                                                 (size_t)k_c_url,
                                                 fld[k_c_title]);
}

/**
 * @brief Parse and apply one page record.
 * @details @param[in,out] st Destination state. @param[in,out] fld Split fields. @param[in] nf Field count. @param[in] schema_version Active payload schema. @return Application result. @retval false The record is malformed or over capacity.
 * @pre @p st and @p fld are non-NULL. @pre A supported version was already parsed.
 * @post Success appends exactly one page. @post Failure leaves no partial page.
 * @note Optional cache validators default to empty. @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_state_apply_page(mdl_state_t* st, char* fld[], size_t nf, uint16_t schema_version)
{
  const bool current = schema_version == (uint16_t)k_mdl_state_version;
  if ((current && (nf != (size_t)k_p_fields_v4)) ||
      (!current && ((nf < (size_t)k_p_fields) || (nf > ((size_t)k_p_lastmod + 1U))))) {
    return false;
  }
  uint64_t uh = 0U;
  uint64_t ch = 0U;
  if (!internal_mdl_state_parse_u64_field(fld[k_p_urlhash], (int)k_state_hex_base, &uh) ||
      !internal_mdl_state_parse_u64_field(fld[k_p_content], (int)k_state_hex_base, &ch)) {
    return false;
  }
  const char* etag    = (nf > (size_t)k_p_etag) ? fld[k_p_etag] : "";
  const char* lastmod = (nf > (size_t)k_p_lastmod) ? fld[k_p_lastmod] : "";
  int64_t     epoch   = 0;
  uint64_t    status  = 0U;
  if (current &&
      (!internal_mdl_state_parse_i64_field(fld[k_p_epoch], &epoch) ||
       !internal_mdl_state_parse_u64_field(fld[k_p_status], (int)k_state_dec_base, &status) ||
       (status > UINT16_MAX))) {
    return false;
  }
  return mdl_state_add_page(st, uh, ch, fld[k_p_relpath], etag, lastmod, epoch, (uint16_t)status);
}

/**
 * @brief Store one exact bounded key/value record.
 * @details Requires exactly two TAB fields and copies without truncation.
 * @param[out] dst Destination fixed field.
 * @param[in]  cap Destination capacity.
 * @param[in]  fld Split record fields.
 * @param[in]  nf  Number of fields.
 * @return Whether the record was valid and copied.
 * @retval true  The value was copied exactly.
 * @retval false Field count or bounded-field validation failed.
 * @pre @p dst and @p fld are non-NULL.
 * @pre @p cap is greater than zero.
 * @post On true, @p dst equals `fld[1]`.
 * @post On false, @p dst is unchanged.
 * @note Not thread-safe: writes caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_apply_kv(char* dst, size_t cap, char* fld[], size_t nf)
{
  if ((nf != 2U) || !priv_mdl_state_field_valid(fld[1], cap)) {
    return false;
  }
  priv_mdl_state_set_opt(dst, cap, fld[1]);
  return true;
}

/**
 * @brief Apply one current-schema rich-metadata record.
 * @details Dispatches one metadata type to its exact bounded destination.
 * @param[in,out] st State receiving the field.
 * @param[in] type One of D, W, A, O, K, or L.
 * @param[in] fld Split record fields.
 * @param[in] nf Number of accessible fields.
 * @return Whether the exact record was recognized and accepted.
 * @retval false Unknown type, invalid path, field count, or field contents.
 * @pre @p st and @p fld are non-NULL.
 * @pre @p nf describes every accessible field pointer.
 * @post Success updates exactly one bounded metadata field.
 * @post Failure leaves the selected field unchanged.
 * @note Cover paths additionally enforce relative non-traversing syntax.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_state_apply_metadata(mdl_state_t* st, char type, char* fld[], size_t nf)
{
  switch (type) {
    case 'D':
      return internal_mdl_state_apply_kv(st->summary, sizeof(st->summary), fld, nf);
    case 'W':
      return internal_mdl_state_apply_kv(st->writer, sizeof(st->writer), fld, nf);
    case 'A':
      return internal_mdl_state_apply_kv(st->artist, sizeof(st->artist), fld, nf);
    case 'O':
      return internal_mdl_state_apply_kv(st->cover_url, sizeof(st->cover_url), fld, nf);
    case 'K':
      return (nf == 2U) && priv_mdl_state_relative_path_valid(fld[1], sizeof(st->cover_path)) &&
             internal_mdl_state_apply_kv(st->cover_path, sizeof(st->cover_path), fld, nf);
    case 'L':
      return internal_mdl_state_apply_kv(st->language, sizeof(st->language), fld, nf);
    default:
      return false;
  }
}

/**
 * @brief Accept one exact supported schema-version record.
 * @details @param[in,out] fld Split fields. @param[in] nf Field count. @param[out] schema_version Selected schema. @return Parse result. @retval false Duplicate, unsupported, or malformed version.
 * @pre @p fld and @p schema_version are non-NULL. @pre The caller initialized the version to zero.
 * @post Success selects v1, v2, v3, or v4. @post Failure does not accept a partial numeric token.
 * @note Exactly one version record is permitted. @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_state_apply_version(char* fld[], size_t nf, uint16_t* schema_version)
{
  uint64_t version = 0U;
  if ((fld[0][0] != 'V') || (nf != 2U) ||
      !internal_mdl_state_parse_u64_field(fld[1], (int)k_state_dec_base, &version) ||
      ((version != (uint64_t)k_mdl_state_version_v1) &&
       (version != (uint64_t)k_mdl_state_version_v2) &&
       (version != (uint64_t)k_mdl_state_version_v3) &&
       (version != (uint64_t)k_mdl_state_version))) {
    return false;
  }
  *schema_version = (uint16_t)version;
  return true;
}

/**
 * @brief Apply one already-split state record.
 * @details Dispatches by record type and enforces schema-specific grammar.
 * @param[in,out] st             State receiving the record.
 * @param[in]     fld            Split TAB fields.
 * @param[in]     nf             Number of fields.
 * @param[in,out] schema_version Zero before V; accepted schema afterward.
 * @return Whether the record is valid in sequence and schema.
 * @retval true  The version was accepted or the record was applied.
 * @retval false The record is malformed, unsupported, duplicated, or misplaced.
 * @pre All pointer arguments are non-NULL.
 * @pre @p nf is nonzero and describes accessible @p fld entries.
 * @post On first success, @p schema_version becomes v1, v2, v3, or v4.
 * @post On false, the caller rejects the complete stream.
 * @note Not thread-safe: mutates parser state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_state_apply_line(mdl_state_t* st, char* fld[], size_t nf, uint16_t* schema_version)
{
  const char type = fld[0][0];
  if ((fld[0][1] != '\0')) {
    return false; /* a record type is exactly one character */
  }
  if (*schema_version == 0U) {
    return internal_mdl_state_apply_version(fld, nf, schema_version);
  }
  switch (type) {
    case 'V':
      return false; /* duplicate headers make corruption/concatenation visible */
    case 'S':
      return internal_mdl_state_apply_kv(st->series_url, sizeof(st->series_url), fld, nf);
    case 'T':
      return internal_mdl_state_apply_kv(st->series_title, sizeof(st->series_title), fld, nf);
    case 'N':
      return internal_mdl_state_apply_kv(st->site_name, sizeof(st->site_name), fld, nf);
    case 'H':
      return internal_mdl_state_apply_kv(st->site_host, sizeof(st->site_host), fld, nf);
    case 'G':
      return internal_mdl_state_apply_kv(st->config_path, sizeof(st->config_path), fld, nf);
    case 'D':
    case 'W':
    case 'A':
    case 'O':
    case 'K':
    case 'L':
      return (*schema_version != (uint16_t)k_mdl_state_version_v1) &&
             internal_mdl_state_apply_metadata(st, type, fld, nf);
    case 'R': {
      uint64_t direction = 0U;
      if ((*schema_version == (uint16_t)k_mdl_state_version_v1) || (nf != 2U) ||
          !internal_mdl_state_parse_u64_field(fld[1], (int)k_state_dec_base, &direction) ||
          (direction > (uint64_t)k_mdl_state_read_rtl)) {
        return false;
      }
      st->reading_direction = (mdl_state_reading_direction_t)direction;
      return true;
    }
    case 'C':
      if (*schema_version == (uint16_t)k_mdl_state_version_v1) {
        return internal_mdl_state_apply_chapter_v1(st, fld, nf);
      }
      return (*schema_version == (uint16_t)k_mdl_state_version_v2)
               ? internal_mdl_state_apply_chapter_v2(st, fld, nf)
               : internal_mdl_state_apply_chapter_v3(st, fld, nf);
    case 'P':
      return internal_mdl_state_apply_page(st, fld, nf, *schema_version);
    default:
      return false;
  }
}

/** @brief Bounded reader over one exact payload extent. */
typedef struct {
  mdl_storage_t* storage;   /**< Caller-owned stream scratch. */
  fw_fs_file_t*  file;      /**< Open portable source file.   */
  uint64_t       remaining; /**< Bytes not yet delivered.     */
  uint32_t       cursor;    /**< Next buffered byte.          */
  uint32_t       available; /**< Buffered byte count.         */
} mdl_state_reader_t;

/**
 * @brief Refill a payload reader without crossing its declared extent.
 * @details @param[in,out] reader Bounded reader. @return Portable I/O status. @retval k_ra8_err_invalid_state Zero progress before the declared end.
 * @pre @p reader and its storage/file are valid. @pre Remaining length is authoritative.
 * @post Success exposes at least one byte when bytes remain. @post No read crosses the payload extent.
 * @note Read attempts are globally bounded. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_reader_refill(mdl_state_reader_t* reader)
{
  if (reader->remaining == 0U) {
    reader->cursor    = 0U;
    reader->available = 0U;
    return k_ra8_ok;
  }
  const uint32_t  wanted = (reader->remaining < (uint64_t)reader->storage->io_buffer_bytes)
                             ? (uint32_t)reader->remaining
                             : reader->storage->io_buffer_bytes;
  uint32_t        count  = 0U;
  const ra8_err_t err    = fw_fs_read(reader->file, reader->storage->io_buffer, wanted, &count);
  if (err != k_ra8_ok) {
    return err;
  }
  if (count == 0U) {
    return k_ra8_err_invalid_state;
  }
  reader->remaining -= count;
  reader->cursor    = 0U;
  reader->available = count;
  return k_ra8_ok;
}

/**
 * @brief Deliver one byte from a bounded payload reader.
 * @details @param[in,out] reader Bounded reader. @param[out] out Byte. @param[out] out_eof End marker. @return Portable I/O status. @retval k_ra8_ok Byte or clean EOF delivered.
 * @pre All pointers are non-NULL. @pre Reader counters are internally consistent.
 * @post EOF initializes @p out_eof true. @post A byte decrements the remaining extent once.
 * @note Refill failures propagate unchanged. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mdl_state_reader_byte(mdl_state_reader_t* reader, uint8_t* out, bool* out_eof)
{
  if (reader->cursor == reader->available) {
    const ra8_err_t err = internal_mdl_state_reader_refill(reader);
    if (err != k_ra8_ok) {
      return err;
    }
    if (reader->available == 0U) {
      *out_eof = true;
      return k_ra8_ok;
    }
  }
  *out     = reader->storage->io_buffer[reader->cursor];
  *out_eof = false;
  reader->cursor += 1U;
  return k_ra8_ok;
}

/**
 * @brief Read one bounded text record.
 * @details @param[in,out] reader Bounded reader. @param[out] line Record buffer. @param[in] capacity Buffer size. @param[out] out_has_line Presence flag. @return Portable parse/I/O status. @retval k_ra8_err_invalid_state Record exceeds capacity.
 * @pre All pointers are non-NULL. @pre @p capacity permits a terminator.
 * @post A present line is NUL-terminated. @post Final unterminated records remain accepted.
 * @note Embedded NUL bytes are rejected later as malformed fields. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mdl_state_reader_line(mdl_state_reader_t* reader,
                                                             char*               line,
                                                             size_t              capacity,
                                                             bool*               out_has_line)
{
  size_t used = 0U;
  for (;;) {
    uint8_t   byte = 0U;
    bool      eof  = false;
    ra8_err_t err  = internal_mdl_state_reader_byte(reader, &byte, &eof);
    if (err != k_ra8_ok) {
      return err;
    }
    if (eof || (byte == (uint8_t)'\n')) {
      if ((used > 0U) && (line[used - 1U] == '\r')) {
        --used;
      }
      line[used]    = '\0';
      *out_has_line = !eof || (used > 0U);
      return k_ra8_ok;
    }
    if ((used + 1U) >= capacity) {
      return k_ra8_err_invalid_state;
    }
    line[used] = (char)byte;
    ++used;
  }
}

/**
 * @brief Apply one non-comment payload record.
 * @details @param[in,out] st Destination state. @param[in,out] line Mutable record. @param[in,out] schema_version Active schema. @return Application result. @retval false Unknown or malformed record.
 * @pre All pointers are non-NULL. @pre @p line is NUL-terminated.
 * @post Success applies one whole record. @post Failure never accepts excess fields.
 * @note Comment and blank filtering occurs in the caller. @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_state_parse_record(mdl_state_t* st, char* line, uint16_t* schema_version)
{
  if ((line[0] == '\0') || (line[0] == '#')) {
    return true;
  }
  char*        fields[k_state_max_flds];
  const size_t count = internal_mdl_state_split_tabs(line, fields, (size_t)k_state_max_flds);
  return (count <= (size_t)k_state_max_flds) &&
         internal_mdl_state_apply_line(st, fields, count, schema_version);
}

RA8_PRIV ra8_err_t priv_mdl_state_parse_file(mdl_storage_t* storage,
                                             fw_fs_file_t*  file,
                                             uint64_t       offset,
                                             uint64_t       length,
                                             uint16_t       max_schema_version,
                                             mdl_state_t*   st)
{
  if ((storage == nullptr) || (file == nullptr) || (st == nullptr) ||
      (storage->io_buffer == nullptr) || (storage->io_buffer_bytes == 0U) ||
      (max_schema_version == 0U) || (max_schema_version > (uint16_t)k_mdl_state_version)) {
    return k_ra8_err_invalid_arg;
  }
  mdl_state_init(st);
  ra8_err_t err = fw_fs_seek(file, offset);
  if (err != k_ra8_ok) {
    return err;
  }
  mdl_state_reader_t reader = {.storage   = storage,
                               .file      = file,
                               .remaining = length,
                               .cursor    = 0U,
                               .available = 0U};
  char               line[k_mdl_state_line_max];
  uint16_t           schema_version = 0U;
  bool               has_line       = true;
  while ((err == k_ra8_ok) && has_line) {
    err = internal_mdl_state_reader_line(&reader, line, sizeof(line), &has_line);
    if ((err == k_ra8_ok) && has_line &&
        !internal_mdl_state_parse_record(st, line, &schema_version)) {
      err = k_ra8_err_invalid_state;
    }
  }
  if ((err == k_ra8_ok) && ((schema_version == 0U) || (schema_version > max_schema_version) ||
                            !priv_mdl_state_valid(st))) {
    err = k_ra8_err_invalid_state;
  }
  if (err != k_ra8_ok) {
    mdl_state_init(st);
  }
  return err;
}

/* ---- coverage ------------------------------------------------------------ */

/**
 * @brief Convert a known integral chapter number for gap reporting.
 * @details Rejects absent, fractional, and values outside the `long` domain.
 * @param[in]  chapter Chapter whose number is inspected.
 * @param[out] out     Integral result destination.
 * @return Whether the chapter has a representable integral number.
 * @retval true  @p out received the exact integral number.
 * @retval false The number is absent, fractional, or out of range.
 * @pre @p chapter and @p out are non-NULL.
 * @pre @p chapter satisfies the persisted finite-number invariant.
 * @post On true, converting @p out back to double reproduces the number.
 * @post @p chapter is unchanged.
 * @note Thread-safe: reads only caller storage.
 * @since 0.1.0
 */
