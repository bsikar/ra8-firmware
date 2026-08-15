/**
 * @file mdl_state_decimal.c
 * @brief Exact locale-free conversion for legacy decimal state values.
 * @details Uses fixed-capacity integers to round bounded schema-v2 decimals to binary64.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "mdl_state_internal.h"
#include "ra8_attributes.h"

/** @brief Fixed storage is sufficient for 17 digits multiplied or divided by 10^400. */
typedef enum : uint8_t {
  k_state_big_words = 48, /**< Capacity of one fixed unsigned integer. */
} mdl_state_big_limit_t;

/** @brief Exact decimal and binary64 representation constants. */
typedef enum : int32_t {
  k_state_decimal_prime            = 5,     /**< Odd factor of decimal radix.       */
  k_state_decimal_scale_max        = 400,   /**< Accepted absolute decimal scale.   */
  k_state_divide_bits              = 64,    /**< Width of the bounded quotient.     */
  k_state_binary64_fraction_bits   = 52,    /**< Stored binary64 fraction width.    */
  k_state_binary64_precision_bits  = 53,    /**< Binary64 significand precision.    */
  k_state_binary64_sign_shift      = 63,    /**< Binary64 sign-bit position.        */
  k_state_binary64_exponent_bias   = 1023,  /**< Binary64 exponent bias.            */
  k_state_binary64_exponent_max    = 1023,  /**< Largest finite unbiased exponent.  */
  k_state_binary64_exponent_min    = -1022, /**< Smallest normal unbiased exponent. */
  k_state_binary64_subnormal_scale = 1074,  /**< Scale selecting subnormal units.   */
} mdl_state_decimal_limit_t;

/** @brief Little-endian base-2^32 unsigned integer. */
typedef struct {
  uint32_t word[k_state_big_words]; /**< Little-endian magnitude words. */
  uint8_t  used;                    /**< Significant word count.        */
} mdl_state_big_t;

/**
 * @brief Initialize a fixed integer from one uint64 value.
 * @details @param[out] big Destination integer. @param[in] value Initial magnitude.
 * @pre @p big is non-NULL. @pre Its storage has the declared word capacity.
 * @post Every unused word is zero. @post The used count is one or two.
 * @note Initialization is constant-time with respect to capacity. @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_state_big_init(mdl_state_big_t* big, uint64_t value)
{
  memset(big, 0, sizeof(*big));
  big->word[0] = (uint32_t)value;
  big->word[1] = (uint32_t)(value >> 32U);
  big->used    = (uint8_t)((big->word[1] != 0U) ? 2U : 1U);
}

/**
 * @brief Remove unused high words while retaining one zero word.
 * @details @param[in,out] big Integer to normalize.
 * @pre @p big is non-NULL. @pre Its used count is within capacity.
 * @post The high used word is nonzero unless the value is zero. @post Zero retains one word.
 * @note Magnitude words are otherwise unchanged. @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_state_big_trim(mdl_state_big_t* big)
{
  while ((big->used > 1U) && (big->word[big->used - 1U] == 0U)) {
    --big->used;
  }
}

/**
 * @brief Return the significant bit count of a fixed integer.
 * @details @param[in] big Normalized integer. @return Magnitude bit length. @retval 0 The integer is zero.
 * @pre @p big is non-NULL. @pre Its used count is normalized and in range.
 * @post The integer is unchanged. @post A nonzero result fits the fixed capacity.
 * @note Uses only defined unsigned shifts. @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_mdl_state_big_bits(const mdl_state_big_t* big)
{
  if ((big->used == 1U) && (big->word[0] == 0U)) {
    return 0U;
  }
  uint32_t high = big->word[big->used - 1U];
  uint8_t  bits = 0U;
  while (high != 0U) {
    high >>= 1U;
    ++bits;
  }
  return (uint16_t)(((uint16_t)(big->used - 1U) * 32U) + bits);
}

/**
 * @brief Multiply a fixed integer by one small factor.
 * @details @param[in,out] big Integer operand. @param[in] factor Small multiplier. @return Capacity result. @retval false The product exceeds fixed storage.
 * @pre @p big is non-NULL. @pre Its used count is normalized and in range.
 * @post Success stores the exact product. @post Failure never writes outside the array.
 * @note Carry arithmetic uses uint64. @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_big_mul(mdl_state_big_t* big, uint32_t factor)
{
  uint64_t carry = 0U;
  for (uint8_t i = 0U; i < big->used; ++i) {
    const uint64_t product = ((uint64_t)big->word[i] * factor) + carry;
    big->word[i]           = (uint32_t)product;
    carry                  = product >> 32U;
  }
  if (carry != 0U) {
    if (big->used >= (uint8_t)k_state_big_words) {
      return false;
    }
    big->word[big->used] = (uint32_t)carry;
    ++big->used;
  }
  return true;
}

/**
 * @brief Copy one fixed integer shifted left by an exact bit count.
 * @details @param[in] src Source integer. @param[in] shift Bit count. @param[out] dst Destination. @return Capacity result. @retval false The shifted value does not fit.
 * @pre Source and destination are non-NULL and distinct. @pre Source used count is valid.
 * @post Success stores the exact normalized shift. @post Failure writes no out-of-bounds word.
 * @note Zero remains normalized. @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mdl_state_big_shift(const mdl_state_big_t* src, uint16_t shift, mdl_state_big_t* dst)
{
  const uint16_t words = shift / 32U;
  const uint8_t  bits  = (uint8_t)(shift % 32U);
  if (((uint16_t)src->used + words + (bits != 0U ? 1U : 0U)) > k_state_big_words) {
    return false;
  }
  memset(dst, 0, sizeof(*dst));
  uint64_t carry = 0U;
  for (uint8_t i = 0U; i < src->used; ++i) {
    const uint64_t value           = ((uint64_t)src->word[i] << bits) | carry;
    dst->word[(uint16_t)i + words] = (uint32_t)value;
    carry                          = value >> 32U;
  }
  dst->used = (uint8_t)((uint16_t)src->used + words);
  if (carry != 0U) {
    dst->word[dst->used] = (uint32_t)carry;
    ++dst->used;
  }
  internal_mdl_state_big_trim(dst);
  return true;
}

/**
 * @brief Compare two normalized fixed integers.
 * @details @param[in] lhs Left operand. @param[in] rhs Right operand. @return Three-way ordering. @retval 0 Magnitudes are equal.
 * @pre Both pointers are non-NULL. @pre Both used counts are normalized.
 * @post Both operands are unchanged. @post The sign of the result matches unsigned ordering.
 * @note Comparison starts at the highest word. @since 0.1.0
 */
RA8_INTERNAL static int internal_mdl_state_big_compare(const mdl_state_big_t* lhs,
                                                       const mdl_state_big_t* rhs)
{
  if (lhs->used != rhs->used) {
    return lhs->used > rhs->used ? 1 : -1;
  }
  for (uint8_t i = lhs->used; i > 0U; --i) {
    if (lhs->word[i - 1U] != rhs->word[i - 1U]) {
      return lhs->word[i - 1U] > rhs->word[i - 1U] ? 1 : -1;
    }
  }
  return 0;
}

/**
 * @brief Subtract a no-larger fixed integer from another.
 * @details @param[in,out] lhs Minuend and result. @param[in] rhs Subtrahend.
 * @pre Both pointers are non-NULL. @pre @p lhs is at least @p rhs.
 * @post @p lhs stores the exact normalized difference. @post @p rhs is unchanged.
 * @note Borrow arithmetic remains unsigned. @since 0.1.0
 */
RA8_INTERNAL static void internal_mdl_state_big_subtract(mdl_state_big_t*       lhs,
                                                         const mdl_state_big_t* rhs)
{
  uint64_t borrow = 0U;
  for (uint8_t i = 0U; i < lhs->used; ++i) {
    const uint64_t subtrahend = (uint64_t)(i < rhs->used ? rhs->word[i] : 0U) + borrow;
    const uint64_t current    = lhs->word[i];
    lhs->word[i]              = (uint32_t)(current - subtrahend);
    borrow                    = current < subtrahend ? 1U : 0U;
  }
  internal_mdl_state_big_trim(lhs);
}

/**
 * @brief Compare a scaled rational against one exact power of two.
 * @details @param[in] numerator Rational numerator. @param[in] denominator Rational denominator. @param[in] binary_scale Numerator power-of-two scale. @param[in] exponent Compared exponent. @return Three-way ordering. @retval 0 Values are equal.
 * @pre Integer pointers are non-NULL. @pre Denominator is positive and normalized.
 * @post Inputs remain unchanged. @post Capacity overflow returns the mathematically forced ordering.
 * @note At most one temporary fixed integer is used. @since 0.1.0
 */
RA8_INTERNAL static int internal_mdl_state_compare_power(const mdl_state_big_t* numerator,
                                                         const mdl_state_big_t* denominator,
                                                         int32_t                binary_scale,
                                                         int32_t                exponent)
{
  mdl_state_big_t shifted = {};
  const int32_t   shift   = binary_scale - exponent;
  if (shift >= 0) {
    if (!internal_mdl_state_big_shift(numerator, (uint16_t)shift, &shifted)) {
      return 1;
    }
    return internal_mdl_state_big_compare(&shifted, denominator);
  }
  if (!internal_mdl_state_big_shift(denominator, (uint16_t)-shift, &shifted)) {
    return -1;
  }
  return internal_mdl_state_big_compare(numerator, &shifted);
}

/**
 * @brief Divide one scaled rational and round to nearest-even.
 * @details @param[in] numerator Numerator. @param[in] denominator Denominator. @param[in] binary_shift Numerator binary scale. @param[out] out Rounded quotient. @return Capacity result. @retval false Intermediate or quotient width exceeds bounds.
 * @pre All pointers are non-NULL. @pre Denominator is positive and normalized.
 * @post Success initializes @p out exactly. @post Inputs remain unchanged.
 * @note Long division emits at most 64 quotient bits. @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_divide(const mdl_state_big_t* numerator,
                                                   const mdl_state_big_t* denominator,
                                                   int32_t                binary_shift,
                                                   uint64_t*              out)
{
  mdl_state_big_t work    = {};
  mdl_state_big_t divisor = {};
  if (binary_shift >= 0) {
    if (!internal_mdl_state_big_shift(numerator, (uint16_t)binary_shift, &work)) {
      return false;
    }
    divisor = *denominator;
  } else {
    work = *numerator;
    if (!internal_mdl_state_big_shift(denominator, (uint16_t)-binary_shift, &divisor)) {
      return false;
    }
  }
  uint64_t      quotient = 0U;
  const int32_t top =
    (int32_t)internal_mdl_state_big_bits(&work) - (int32_t)internal_mdl_state_big_bits(&divisor);
  if (top >= (int32_t)k_state_divide_bits) {
    return false;
  }
  for (int32_t bit = top; bit >= 0; --bit) {
    mdl_state_big_t trial = {};
    if (!internal_mdl_state_big_shift(&divisor, (uint16_t)bit, &trial)) {
      return false;
    }
    if (internal_mdl_state_big_compare(&work, &trial) >= 0) {
      internal_mdl_state_big_subtract(&work, &trial);
      quotient |= UINT64_C(1) << (uint32_t)bit;
    }
  }
  mdl_state_big_t twice = {};
  if (!internal_mdl_state_big_shift(&work, 1U, &twice)) {
    return false;
  }
  const int comparison = internal_mdl_state_big_compare(&twice, &divisor);
  if ((comparison > 0) || ((comparison == 0) && ((quotient & 1U) != 0U))) {
    ++quotient;
  }
  *out = quotient;
  return true;
}

/**
 * @brief Build the odd rational factors for a decimal value.
 * @details @param[in] mantissa Decimal significand. @param[in] decimal_scale Signed power of ten. @param[out] numerator Numerator factor. @param[out] denominator Denominator factor. @return Capacity result. @retval false A factor exceeds fixed storage.
 * @pre Output pointers are non-NULL and distinct. @pre Decimal scale is within the public bound.
 * @post Success represents `mantissa * 5^scale`. @post The remaining `2^scale` stays explicit.
 * @note Exactly one side is multiplied by five. @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_build_rational(uint64_t         mantissa,
                                                           int32_t          decimal_scale,
                                                           mdl_state_big_t* numerator,
                                                           mdl_state_big_t* denominator)
{
  internal_mdl_state_big_init(numerator, mantissa);
  internal_mdl_state_big_init(denominator, 1U);
  const uint32_t   count  = (uint32_t)(decimal_scale < 0 ? -decimal_scale : decimal_scale);
  mdl_state_big_t* factor = decimal_scale < 0 ? denominator : numerator;
  for (uint32_t i = 0U; i < count; ++i) {
    if (!internal_mdl_state_big_mul(factor, (uint32_t)k_state_decimal_prime)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Encode one rounded quotient as finite binary64.
 * @details @param[in] quotient Rounded significand. @param[in] exponent Unbiased exponent. @param[in] negative Sign. @param[in] normal Normal/subnormal selector. @param[out] out Encoded value. @return Encoding result. @retval false Overflow, underflow, or invalid quotient.
 * @pre @p out is non-NULL. @pre Quotient and exponent came from exact division.
 * @post Success initializes finite binary64 bits. @post Rounding carry is normalized once.
 * @note Bit transfer uses memcpy. @since 0.1.0
 */
RA8_INTERNAL static bool internal_mdl_state_encode(uint64_t quotient,
                                                   int32_t  exponent,
                                                   bool     negative,
                                                   bool     normal,
                                                   double*  out)
{
  uint64_t bits = negative ? (UINT64_C(1) << (uint32_t)k_state_binary64_sign_shift) : 0U;
  if (normal) {
    if (quotient == (UINT64_C(1) << (uint32_t)k_state_binary64_precision_bits)) {
      quotient >>= 1U;
      ++exponent;
    }
    if ((exponent > (int32_t)k_state_binary64_exponent_max) ||
        (quotient < (UINT64_C(1) << (uint32_t)k_state_binary64_fraction_bits))) {
      return false;
    }
    bits |= (uint64_t)(exponent + (int32_t)k_state_binary64_exponent_bias)
            << (uint32_t)k_state_binary64_fraction_bits;
    bits |= quotient - (UINT64_C(1) << (uint32_t)k_state_binary64_fraction_bits);
  } else {
    if ((quotient == 0U) ||
        (quotient > (UINT64_C(1) << (uint32_t)k_state_binary64_fraction_bits))) {
      return false;
    }
    if (quotient == (UINT64_C(1) << (uint32_t)k_state_binary64_fraction_bits)) {
      bits |= UINT64_C(1) << (uint32_t)k_state_binary64_fraction_bits;
    } else {
      bits |= quotient;
    }
  }
  memcpy(out, &bits, sizeof(bits));
  return true;
}

RA8_PRIV bool priv_mdl_state_decimal_to_binary64(uint64_t mantissa,
                                                 int32_t  decimal_scale,
                                                 bool     negative,
                                                 double*  out)
{
  if ((out == nullptr) || (decimal_scale < -(int32_t)k_state_decimal_scale_max) ||
      (decimal_scale > (int32_t)k_state_decimal_scale_max)) {
    return false;
  }
  if (mantissa == 0U) {
    const uint64_t bits = negative ? (UINT64_C(1) << (uint32_t)k_state_binary64_sign_shift) : 0U;
    memcpy(out, &bits, sizeof(bits));
    return true;
  }
  mdl_state_big_t numerator   = {};
  mdl_state_big_t denominator = {};
  if (!internal_mdl_state_build_rational(mantissa, decimal_scale, &numerator, &denominator)) {
    return false;
  }
  int32_t exponent = (int32_t)internal_mdl_state_big_bits(&numerator) -
                     (int32_t)internal_mdl_state_big_bits(&denominator) + decimal_scale;
  if (internal_mdl_state_compare_power(&numerator, &denominator, decimal_scale, exponent) < 0) {
    --exponent;
  }
  if (exponent > (int32_t)k_state_binary64_exponent_max) {
    return false;
  }
  const bool    normal   = exponent >= (int32_t)k_state_binary64_exponent_min;
  const int32_t shift    = normal
                             ? (decimal_scale + (int32_t)k_state_binary64_fraction_bits - exponent)
                             : (decimal_scale + (int32_t)k_state_binary64_subnormal_scale);
  uint64_t      quotient = 0U;
  if (!internal_mdl_state_divide(&numerator, &denominator, shift, &quotient)) {
    return false;
  }
  return internal_mdl_state_encode(quotient, exponent, negative, normal, out);
}
