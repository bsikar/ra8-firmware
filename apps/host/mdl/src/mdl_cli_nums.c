/**
 * @file mdl_cli_nums.c
 * @brief Strict bounded numeric-option parsing for mdl.
 * @details Converts the validated command's numeric strings into fixed-width
 *          run values and rejects signs, garbage, overflow, and invalid ranges.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "mdl_cli.h"
#include "mdl_cli_internal.h"
#include "ra8_attributes.h"

/** @brief Radix used by strict unsigned decimal parsing. */
typedef enum : uint8_t {
  k_cli_dec_base = 10, /**< Decimal conversion radix. */
} mdl_cli_parse_t;

/** @brief Default request time budget when `--timeout` is absent. */
typedef enum : uint32_t {
  k_req_timeout_def = 25000U, /**< Twenty-five seconds in milliseconds. */
} mdl_cli_timeout_t;

/** @brief Native-width numeric candidates retained until range validation. */
typedef struct {
  unsigned long timeout;   /**< Parsed timeout before uint32 narrowing.   */
  unsigned long chapters;  /**< Parsed chapter count before size_t use.   */
  unsigned long max_imgs;  /**< Parsed image cap before uint32 narrowing. */
  unsigned long pick;      /**< Parsed discovery selection before size_t. */
  uint64_t      max_bytes; /**< Parsed response byte cap.                 */
} mdl_cli_raw_nums_t;

/**
 * @brief Parse one complete unsigned-long decimal value.
 * @details Rejects NULL, empty, signed, whitespace-prefixed, overflowing, and
 *          trailing-garbage spellings before publishing the converted value.
 * @param[in] s Candidate NUL-terminated decimal text.
 * @param[out] out Receives the converted value on success.
 * @return Whether the complete spelling was accepted.
 * @retval true @p out was initialized with the exact value.
 * @retval false The spelling or range was invalid.
 * @pre @p out is non-NULL.
 * @pre @p s, when non-NULL, is NUL-terminated.
 * @post Success initializes @p out exactly once.
 * @post Failure does not publish a converted value.
 * @note Temporarily modifies `errno`; callers must not depend on its prior value.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_cli_parse_ul(const char* s, unsigned long* out)
{
  if (s == nullptr) {
    return false;
  }
  if (s[0] < '0') {
    return false;
  }
  if (s[0] > '9') {
    return false;
  }
  errno                   = 0;
  char*               end = nullptr;
  const unsigned long v   = strtoul(s, &end, k_cli_dec_base);
  if (errno != 0) {
    return false;
  }
  if (*end != '\0') {
    return false;
  }
  *out = v;
  return true;
}

/**
 * @brief Parse one complete unsigned 64-bit decimal value.
 * @details Applies the same strict spelling rules as ::internal_cli_parse_ul
 *          while preserving the full `uint64_t` option range.
 * @param[in] s Candidate NUL-terminated decimal text.
 * @param[out] out Receives the converted value on success.
 * @return Whether the complete spelling was accepted.
 * @retval true @p out was initialized with the exact value.
 * @retval false The spelling or range was invalid.
 * @pre @p out is non-NULL.
 * @pre @p s, when non-NULL, is NUL-terminated.
 * @post Success initializes @p out exactly once.
 * @post Failure does not publish a converted value.
 * @note Temporarily modifies `errno`; callers must not depend on its prior value.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_cli_parse_u64(const char* s, uint64_t* out)
{
  if (s == nullptr) {
    return false;
  }
  if (s[0] < '0') {
    return false;
  }
  if (s[0] > '9') {
    return false;
  }
  errno                        = 0;
  char*                    end = nullptr;
  const unsigned long long v   = strtoull(s, &end, k_cli_dec_base);
  if (errno != 0) {
    return false;
  }
  if (*end != '\0') {
    return false;
  }
  *out = (uint64_t)v;
  return true;
}

/**
 * @brief Parse one complete finite decimal chapter number for `--from`.
 * @details Uses the C numeric grammar exposed by `strtod`, then rejects
 *          overflow, missing conversion, trailing bytes, NaN, and infinity.
 * @param[in] s NUL-terminated option value.
 * @param[out] out Receives the finite chapter number.
 * @return Whether the complete value was accepted.
 * @retval true @p out received one finite number.
 * @retval false An argument or numeric spelling was invalid.
 * @pre @p s and @p out are non-NULL for success.
 * @pre The caller does not consume @p out after false.
 * @post On true, `isfinite(*out)` is true.
 * @post No global state other than the temporary `errno` value is retained.
 * @note Not thread-safe with code that concurrently depends on `errno`.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_cli_parse_chapter(const char* s, double* out)
{
  if ((s == nullptr) || (out == nullptr) || (s[0] == '\0')) {
    return false;
  }
  errno            = 0;
  char*        end = nullptr;
  const double v   = strtod(s, &end);
  if ((errno != 0) || (end == s) || (*end != '\0') || !isfinite(v)) {
    return false;
  }
  *out = v;
  return true;
}

/**
 * @brief Reject one option whose value is not an unsigned decimal integer.
 * @param[in,out] diagnostic Bound diagnostic stream.
 * @param[in] name Option name without leading dashes.
 * @param[in] value Rejected NUL-terminated spelling.
 * @return Invalid-argument after a complete write, or the stream failure.
 * @retval k_ra8_err_invalid_arg The complete diagnostic was accepted.
 * @retval other The stream rejected a fragment.
 * @pre All pointers are non-NULL and strings are NUL-terminated.
 * @pre @p diagnostic is exclusively owned for the call.
 * @post The rejected value is reproduced without formatting interpretation.
 * @post No parsed numeric output is modified.
 * @note Thread-safe across distinct streams.
 * @since 0.1.0

 * @details Converts complete decimal spellings into a local candidate.
 *          Caller-visible options change only after every range check succeeds.
 */
RA8_INTERNAL static ra8_err_t
internal_cli_reject_integer(ra8_io_stream_t* diagnostic, const char* name, const char* value)
{
  const char* const parts[] = {"mdl: --",
                               name,
                               " expects a non-negative integer, got '",
                               value,
                               "'\n"};
  return priv_mdl_cli_reject_parts(diagnostic, parts, sizeof(parts) / sizeof(parts[0]));
}

/**
 * @brief Write a rejection line containing one unsigned limit value.
 * @param[in,out] diagnostic Bound diagnostic stream.
 * @param[in] prefix Text before the decimal value.
 * @param[in] value Limit rendered as canonical unsigned decimal.
 * @param[in] suffix Text after the decimal value, including the newline.
 * @return Invalid-argument after a complete write, or the stream failure.
 * @retval k_ra8_err_invalid_arg The complete diagnostic was accepted.
 * @retval other The stream rejected text or digits.
 * @pre All pointers are non-NULL and strings are NUL-terminated.
 * @pre @p diagnostic is exclusively owned for the call.
 * @post Success-path rejection emits @p prefix, @p value, then @p suffix.
 * @post No numeric parser output is modified.
 * @note Thread-safe across distinct streams.
 * @since 0.1.0

 * @details Converts complete decimal spellings into a local candidate.
 *          Caller-visible options change only after every range check succeeds.
 */
RA8_INTERNAL static ra8_err_t internal_cli_reject_limit(ra8_io_stream_t* diagnostic,
                                                        const char*      prefix,
                                                        uint64_t         value,
                                                        const char*      suffix)
{
  const char* const first[] = {prefix};
  ra8_err_t         err     = priv_mdl_cli_put_parts(diagnostic, first, 1U);
  if (err == k_ra8_ok) {
    err = ra8_io_stream_put_u64(diagnostic, value);
  }
  if (err == k_ra8_ok) {
    const char* const last[] = {suffix};
    err                      = priv_mdl_cli_reject_parts(diagnostic, last, 1U);
  }
  return err;
}

/**
 * @brief Parse one optional unsigned-long option or emit its rejection.
 * @param[in,out] diagnostic Bound diagnostic stream.
 * @param[in] name Option name used in diagnostics.
 * @param[in] text Optional NUL-terminated option value.
 * @param[in] dflt Value used when @p text is null.
 * @param[out] out Receives the default or parsed value on success.
 * @return Canonical parse or stream status.
 * @retval k_ra8_ok @p out was initialized.
 * @retval k_ra8_err_invalid_arg The invalid spelling was reported.
 * @retval other The diagnostic stream rejected output.
 * @pre @p diagnostic, @p name, and @p out are non-NULL.
 * @pre Non-null @p text is NUL-terminated.
 * @post Success initializes @p out exactly once.
 * @post Failure leaves @p out unchanged.
 * @note No pointer ownership is transferred.
 * @since 0.1.0

 * @details Converts complete decimal spellings into a local candidate.
 *          Caller-visible options change only after every range check succeeds.
 */
RA8_INTERNAL static ra8_err_t internal_cli_opt_ul(ra8_io_stream_t* diagnostic,
                                                  const char*      name,
                                                  const char*      text,
                                                  unsigned long    dflt,
                                                  unsigned long*   out)
{
  if (text == nullptr) {
    *out = dflt;
    return k_ra8_ok;
  }
  unsigned long candidate = 0UL;
  if (!internal_cli_parse_ul(text, &candidate)) {
    return internal_cli_reject_integer(diagnostic, name, text);
  }
  *out = candidate;
  return k_ra8_ok;
}

/**
 * @brief Parse one optional uint64 option or emit its rejection.
 * @param[in,out] diagnostic Bound diagnostic stream.
 * @param[in] name Option name used in diagnostics.
 * @param[in] text Optional NUL-terminated option value.
 * @param[in] dflt Value used when @p text is null.
 * @param[out] out Receives the default or parsed value on success.
 * @return Canonical parse or stream status.
 * @retval k_ra8_ok @p out was initialized.
 * @retval k_ra8_err_invalid_arg The invalid spelling was reported.
 * @retval other The diagnostic stream rejected output.
 * @pre @p diagnostic, @p name, and @p out are non-NULL.
 * @pre Non-null @p text is NUL-terminated.
 * @post Success initializes @p out exactly once.
 * @post Failure leaves @p out unchanged.
 * @note No pointer ownership is transferred.
 * @since 0.1.0

 * @details Converts complete decimal spellings into a local candidate.
 *          Caller-visible options change only after every range check succeeds.
 */
RA8_INTERNAL static ra8_err_t internal_cli_opt_u64(ra8_io_stream_t* diagnostic,
                                                   const char*      name,
                                                   const char*      text,
                                                   uint64_t         dflt,
                                                   uint64_t*        out)
{
  if (text == nullptr) {
    *out = dflt;
    return k_ra8_ok;
  }
  uint64_t candidate = 0U;
  if (!internal_cli_parse_u64(text, &candidate)) {
    return internal_cli_reject_integer(diagnostic, name, text);
  }
  *out = candidate;
  return k_ra8_ok;
}

/**
 * @brief Parse all numeric spellings into a failure-atomic candidate.
 * @param[in] a Parsed argument strings.
 * @param[in,out] diagnostic Bound rejection stream.
 * @param[out] out Candidate populated only while parsing succeeds.
 * @param[out] raw Native-width values retained for range validation.
 * @return Canonical parse or stream status.
 * @retval k_ra8_ok Every spelling was accepted.
 * @retval k_ra8_err_invalid_arg One invalid spelling was reported.
 * @retval other The diagnostic stream rejected output.
 * @pre All pointers are non-NULL.
 * @pre @p a came from ::mdl_cli_parse.
 * @post Success initializes every field in @p out and @p raw.
 * @post Failure leaves publication to the caller disabled.
 * @note Thread-safe across distinct arguments and streams.
 * @since 0.1.0

 * @details Converts complete decimal spellings into a local candidate.
 *          Caller-visible options change only after every range check succeeds.
 */
RA8_INTERNAL static ra8_err_t internal_cli_parse_num_values(const mdl_args_t*   a,
                                                            ra8_io_stream_t*    diagnostic,
                                                            mdl_nums_t*         out,
                                                            mdl_cli_raw_nums_t* raw)
{
  ra8_err_t err = internal_cli_opt_ul(diagnostic,
                                      "timeout",
                                      a->timeout,
                                      (unsigned long)k_req_timeout_def,
                                      &raw->timeout);
  if (err == k_ra8_ok) {
    err = internal_cli_opt_ul(diagnostic, "chapters", a->chapters, 1UL, &raw->chapters);
  }
  if (err == k_ra8_ok) {
    err = internal_cli_opt_ul(diagnostic, "max", a->max, 0UL, &raw->max_imgs);
  }
  if (err == k_ra8_ok) {
    err = internal_cli_opt_u64(diagnostic, "seed", a->seed, 1U, &out->seed);
  }
  if (err == k_ra8_ok) {
    err = internal_cli_opt_u64(diagnostic, "max-bytes", a->max_bytes, 0U, &raw->max_bytes);
  }
  return err;
}

/**
 * @brief Parse chapter and pick values into the candidate numeric record.
 * @param[in] a Parsed argument strings.
 * @param[in,out] diagnostic Bound rejection stream.
 * @param[in,out] out Candidate numeric record.
 * @param[out] raw Native-width numeric candidates receiving the pick value.
 * @return Canonical parse or stream status.
 * @retval k_ra8_ok Both values were accepted.
 * @retval k_ra8_err_invalid_arg One invalid spelling was reported.
 * @retval other The diagnostic stream rejected output.
 * @pre All pointers are non-NULL.
 * @pre @p out contains the scalar values parsed earlier.
 * @post Success initializes chapter-presence, chapter-number, and `raw->pick`.
 * @post Failure leaves public output unpublished.
 * @note Thread-safe across distinct arguments and streams.
 * @since 0.1.0

 * @details Converts complete decimal spellings into a local candidate.
 *          Caller-visible options change only after every range check succeeds.
 */
RA8_INTERNAL static ra8_err_t internal_cli_parse_chapter_pick(const mdl_args_t*   a,
                                                              ra8_io_stream_t*    diagnostic,
                                                              mdl_nums_t*         out,
                                                              mdl_cli_raw_nums_t* raw)
{
  out->from_present = a->from != nullptr;
  out->from_num     = 0.0;
  if ((a->from != nullptr) && !internal_cli_parse_chapter(a->from, &out->from_num)) {
    const char* const parts[] = {"mdl: --from expects a finite chapter number, got '",
                                 a->from,
                                 "'\n"};
    return priv_mdl_cli_reject_parts(diagnostic, parts, sizeof(parts) / sizeof(parts[0]));
  }
  return internal_cli_opt_ul(diagnostic, "pick", a->pick, 0UL, &raw->pick);
}

/**
 * @brief Enforce numeric option ranges after complete spelling conversion.
 * @param[in] a Parsed argument strings used for presence-sensitive ranges.
 * @param[in,out] diagnostic Bound rejection stream.
 * @param[in] raw Parsed native-width candidates before narrowing.
 * @return Canonical validation or stream status.
 * @retval k_ra8_ok Every converted value lies in its public range.
 * @retval k_ra8_err_invalid_arg One invalid range was reported.
 * @retval other The diagnostic stream rejected output.
 * @pre All pointers are non-NULL and values were parsed completely.
 * @pre @p diagnostic is exclusively owned for the call.
 * @post Success authorizes all later narrowing conversions.
 * @post Failure leaves public output unpublished.
 * @note Thread-safe across distinct arguments and streams.
 * @since 0.1.0

 * @details Converts complete decimal spellings into a local candidate.
 *          Caller-visible options change only after every range check succeeds.
 */
RA8_INTERNAL static ra8_err_t internal_cli_validate_num_ranges(const mdl_args_t*         a,
                                                               ra8_io_stream_t*          diagnostic,
                                                               const mdl_cli_raw_nums_t* raw)
{
  if ((raw->timeout == 0UL) || (raw->timeout > (unsigned long)UINT32_MAX)) {
    return internal_cli_reject_limit(diagnostic, "mdl: --timeout must be in 1..", UINT32_MAX, "\n");
  }
  if ((raw->chapters == 0UL) || ((uintmax_t)raw->chapters > (uintmax_t)SIZE_MAX)) {
    return internal_cli_reject_limit(diagnostic,
                                     "mdl: --chapters must be in 1..",
                                     (uint64_t)SIZE_MAX,
                                     "\n");
  }
  if (raw->max_imgs > (unsigned long)UINT32_MAX) {
    return internal_cli_reject_limit(diagnostic, "mdl: --max must not exceed ", UINT32_MAX, "\n");
  }
  if ((a->max_bytes != nullptr) && (raw->max_bytes == 0U)) {
    const char* const parts[] = {"mdl: --max-bytes must be greater than zero\n"};
    return priv_mdl_cli_reject_parts(diagnostic, parts, 1U);
  }
  if ((a->pick != nullptr) &&
      ((raw->pick == 0UL) || ((uintmax_t)raw->pick > (uintmax_t)SIZE_MAX))) {
    return internal_cli_reject_limit(diagnostic,
                                     "mdl: --pick must be in 1..",
                                     (uint64_t)SIZE_MAX,
                                     "\n");
  }
  return k_ra8_ok;
}

ra8_err_t mdl_cli_parse_nums(const mdl_args_t* a, ra8_io_stream_t* diagnostic, mdl_nums_t* n)
{
  if ((a == nullptr) || (diagnostic == nullptr) || (n == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  mdl_nums_t         candidate = {};
  mdl_cli_raw_nums_t raw       = {};
  ra8_err_t          err       = internal_cli_parse_num_values(a, diagnostic, &candidate, &raw);
  if (err == k_ra8_ok) {
    err = internal_cli_parse_chapter_pick(a, diagnostic, &candidate, &raw);
  }
  if (err == k_ra8_ok) {
    err = internal_cli_validate_num_ranges(a, diagnostic, &raw);
  }
  if (err == k_ra8_ok) {
    candidate.timeout  = (uint32_t)raw.timeout;
    candidate.chapters = (size_t)raw.chapters;
    candidate.max_imgs = (uint32_t)raw.max_imgs;
    candidate.pick     = (size_t)raw.pick;
    *n                 = candidate;
  }
  return err;
}
