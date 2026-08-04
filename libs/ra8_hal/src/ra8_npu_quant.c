/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_npu_quant.c
 * @brief Affine (scale + zero-point) tensor quantization for the NPU runtime
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Implementation of the `ra8_npu_quant.h` contract. The core rounding + clamp is
 * factored into a single helper (`internal_quant_round_clamp`) shared by the
 * INT8 and UINT8 quantizers so the saturation logic is written and tested once.
 * No hardware is touched, so this file carries no HUM / TRM citation, and no
 * `libm` symbol is referenced -- rounding is float arithmetic plus an explicit,
 * range-bounded cast.
 *
 * @since 0.1.0
 */

#include "ra8_npu_quant.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"

/**
 * @var s_tag
 * @brief Log component tag for this module.
 * @details Passed to the `RA8_*` validation macros as the source tag.
 * @note Read-only literal; never modified.
 * @since 0.1.0
 */
static const char* s_tag = "NPUQ";

/**
 * @var s_quant_round_cap
 * @brief Magnitude the pre-cast rounded value is clamped to (float, no libm).
 * @details Bounds `round(real / scale)` well inside the 32-bit integer range so
 *          the float-to-int cast in ::internal_quant_round_clamp is always
 *          defined; any value this large saturates at the element bound anyway.
 * @note Read-only; never modified.
 * @since 0.1.0
 */
static const float s_quant_round_cap = 1.0e6F;

/**
 * @var s_quant_half
 * @brief The half-step used for half-away-from-zero rounding without `libm`.
 * @details Held as a `const float` because an enum cannot carry a float; this is
 *          the named constant that keeps the rounding step out of the code as a
 *          magic number.
 * @note Read-only; never modified.
 * @since 0.1.0
 */
static const float s_quant_half = 0.5F;

/**
 * @brief Round `scaled` half-away-from-zero, add the zero-point, and clamp.
 *
 * @details
 * Computes `clamp((int32)round(scaled) + zero_point, qmin, qmax)` using only
 * float arithmetic and one bounded cast (no `libm`). `scaled` is the real value
 * already divided by the quantization scale; the rounded magnitude is clamped to
 * ::s_quant_round_cap before the cast so the conversion is always in range.
 *
 * @param[in] scaled     Real value divided by the quantization scale.
 * @param[in] zero_point Quantization zero-point added after rounding.
 * @param[in] qmin       Lower saturation bound of the destination type.
 * @param[in] qmax       Upper saturation bound of the destination type.
 *
 * @return The saturated integer quantized value in `[qmin, qmax]`.
 * @retval qmin `scaled` mapped below the destination range.
 * @retval qmax `scaled` mapped above the destination range.
 *
 * @pre `qmin <= qmax`.
 * @pre `scaled` is a finite value.
 * @post The result is within `[qmin, qmax]`.
 * @post No argument is modified.
 *
 * @note Re-entrant; no shared state.
 * @since 0.1.0
 */
static int32_t
internal_quant_round_clamp(float scaled, int32_t zero_point, int32_t qmin, int32_t qmax)
{
  float rounded = (scaled >= 0.0F) ? (scaled + s_quant_half) : (scaled - s_quant_half);
  if (rounded > s_quant_round_cap) {
    rounded = s_quant_round_cap;
  }
  if (rounded < -s_quant_round_cap) {
    rounded = -s_quant_round_cap;
  }
  int32_t q = (int32_t)rounded + zero_point;
  if (q < qmin) {
    q = qmin;
  }
  if (q > qmax) {
    q = qmax;
  }
  return q;
}

ra8_err_t
ra8_npu_quantize_i8(const float* in, int8_t* out, size_t count, float scale, int32_t zero_point)
{
  RA8_CHECK_NULL_PTR(in, s_tag, "quantize_i8: in must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "quantize_i8: out must not be nullptr");
  if (scale <= 0.0F) {
    ra8_log_error(s_tag, "quantize_i8: scale must be > 0");
    return k_ra8_err_invalid_arg;
  }
  for (size_t i = 0U; i < count; i++) {
    const int32_t q = internal_quant_round_clamp(in[i] / scale,
                                                 zero_point,
                                                 (int32_t)k_ra8_npu_quant_i8_min,
                                                 (int32_t)k_ra8_npu_quant_i8_max);
    out[i]          = (int8_t)q;
  }
  return k_ra8_ok;
}

ra8_err_t
ra8_npu_dequantize_i8(const int8_t* in, float* out, size_t count, float scale, int32_t zero_point)
{
  RA8_CHECK_NULL_PTR(in, s_tag, "dequantize_i8: in must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "dequantize_i8: out must not be nullptr");
  for (size_t i = 0U; i < count; i++) {
    out[i] = scale * (float)((int32_t)in[i] - zero_point);
  }
  return k_ra8_ok;
}

ra8_err_t
ra8_npu_quantize_u8(const float* in, uint8_t* out, size_t count, float scale, int32_t zero_point)
{
  RA8_CHECK_NULL_PTR(in, s_tag, "quantize_u8: in must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "quantize_u8: out must not be nullptr");
  if (scale <= 0.0F) {
    ra8_log_error(s_tag, "quantize_u8: scale must be > 0");
    return k_ra8_err_invalid_arg;
  }
  for (size_t i = 0U; i < count; i++) {
    const int32_t q = internal_quant_round_clamp(in[i] / scale,
                                                 zero_point,
                                                 (int32_t)k_ra8_npu_quant_u8_min,
                                                 (int32_t)k_ra8_npu_quant_u8_max);
    out[i]          = (uint8_t)q;
  }
  return k_ra8_ok;
}

ra8_err_t
ra8_npu_dequantize_u8(const uint8_t* in, float* out, size_t count, float scale, int32_t zero_point)
{
  RA8_CHECK_NULL_PTR(in, s_tag, "dequantize_u8: in must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "dequantize_u8: out must not be nullptr");
  for (size_t i = 0U; i < count; i++) {
    out[i] = scale * (float)((int32_t)in[i] - zero_point);
  }
  return k_ra8_ok;
}
