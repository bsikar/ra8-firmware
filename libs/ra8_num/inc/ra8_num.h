/**
 * @file ra8_num.h
 * @brief Exact, locale-free decimal to IEEE-754 binary64 conversion.
 * @ingroup grp_core
 *
 * @par Tag
 * [Ring 1 / Numeric] {World: NS}
 *
 * @details
 * Every consumer that parses a number out of a file and cannot call `strtod`
 * needs correctly-rounded decimal conversion: a config file, a descriptor, a
 * manifest, a wire format. Until this header the tree had exactly one correct
 * implementation of it and no supported way to reach it, because it lived
 * inside a media downloader's state-journal codec behind an `*_internal.h`.
 * A second consumer had to link that product library or write the bignum
 * again.
 *
 * ::ra8_num_decimal_to_binary64 is that primitive, and nothing else. It takes
 * a decimal value already split into an unsigned significand and a signed
 * power of ten, and answers the binary64 nearest to `mantissa * 10^scale`,
 * ties to even, using fixed-capacity integer arithmetic and no libc
 * conversion and no locale.
 *
 * @code
 * double v = 0.0;
 * if (ra8_num_decimal_to_binary64(31415926535897932U, -16, false, &v)) {
 *   // v is the binary64 nearest to 3.1415926535897932
 * }
 * @endcode
 *
 * ## What this is not
 *
 * It is not a parser. Splitting text into a significand, a scale and a sign
 * is the caller's grammar and stays the caller's, because a state journal, a
 * `robots.txt` directive and a command line disagree about what is
 * well-formed. This header only converts what that grammar produced.
 *
 * ## Toolchain requirement
 *
 * The conversion is meaningless on a target whose `double` is not IEC 60559
 * binary64, so the requirement is asserted here, where the platform declares
 * it, rather than wherever a product first happened to notice.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <float.h>
#include <stdint.h>

#if !defined(__STDC_IEC_559__) && !defined(__STDC_IEC_60559_BFP__) && !defined(__clang__) &&        \
  !defined(__GNUC__)
#error "ra8_num requires IEC 60559 binary floating-point"
#endif

static_assert(sizeof(double) == 8U, "ra8_num requires binary64 double");
static_assert(FLT_RADIX == 2, "ra8_num requires radix-2 floating-point");

/** @brief IEC 60559 binary64 parameters the conversion is defined against. */
typedef enum : int16_t {
  k_ra8_num_binary64_mantissa_bits = 53,    /**< Binary64 significand precision. */
  k_ra8_num_binary64_max_exponent  = 1024,  /**< Binary64 maximum exponent.      */
  k_ra8_num_binary64_min_exponent  = -1021, /**< Binary64 minimum exponent.      */
} ra8_num_binary64_parameter_t;

static_assert(DBL_MANT_DIG == k_ra8_num_binary64_mantissa_bits,
              "ra8_num requires 53-bit binary64 precision");
static_assert(DBL_MAX_EXP == k_ra8_num_binary64_max_exponent,
              "ra8_num requires binary64 exponent range");
static_assert(DBL_MIN_EXP == k_ra8_num_binary64_min_exponent,
              "ra8_num requires binary64 exponent range");

/** @brief Bounds the conversion accepts, and the reason each one exists. */
typedef enum : int16_t {
  k_ra8_num_decimal_scale_max  = 400, /**< Accepted absolute power of ten.     */
  k_ra8_num_decimal_digits_max = 17,  /**< Significand digits that round-trip. */
} ra8_num_decimal_bound_t;

/**
 * @brief Convert one exact bounded decimal rational to binary64.
 * @details Builds `mantissa * 5^scale` as a fixed-capacity rational, divides it with the
 * remaining power of two held explicitly, and rounds the 53-bit quotient to nearest with
 * ties to even. No libc conversion, no locale, no dynamic allocation, no floating-point
 * arithmetic on the way: the result is assembled from its bit pattern.
 * @param[in] mantissa Unsigned decimal significand, at most ::k_ra8_num_decimal_digits_max digits.
 * @param[in] decimal_scale Signed power of ten applied to @p mantissa.
 * @param[in] negative Whether to set the binary64 sign bit.
 * @param[out] out Converted finite binary64 value.
 * @return Whether the exact value rounds to a non-underflowing finite binary64.
 * @retval false @p out is NULL, the scale is outside ::k_ra8_num_decimal_scale_max, an
 * intermediate exceeds the fixed capacity, or the value overflows or underflows binary64.
 * @pre @p out is non-NULL.
 * @pre @p decimal_scale is within ::k_ra8_num_decimal_scale_max in absolute value.
 * @post Success is correctly rounded to nearest, ties to even.
 * @post Failure publishes no numeric result; @p out is left untouched.
 * @note A zero mantissa yields a signed zero and never fails on scale alone.
 * @note Infinity and NaN are never produced; an out-of-range value is a refusal.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_num_decimal_to_binary64(uint64_t mantissa,
                                               int32_t  decimal_scale,
                                               bool     negative,
                                               double*  out);
