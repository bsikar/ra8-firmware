/**
 * @file ra8_npu_quant.h
 * @brief Affine (scale + zero-point) tensor quantization for the NPU runtime
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Input/output quantization helpers for the Ethos-U55 inference runtime. An
 * Ethos-U55 command stream operates on INT8 / UINT8 quantized tensors; the
 * application holds its data as real-valued `float`. These four functions bridge
 * the two with the standard TFLite affine mapping
 *
 *     real  = scale * (quantized - zero_point)
 *     quantized = clamp(round(real / scale) + zero_point, qmin, qmax)
 *
 * so a runtime can `ra8_npu_quantize_*()` a float input arena into the INT8 /
 * UINT8 tensor the NPU reads, then `ra8_npu_dequantize_*()` the NPU output back
 * to float for the application.
 *
 * ## Why this is pure integer/float math (no libm)
 *
 * Rounding is done with float arithmetic and an explicit cast rather than
 * `lroundf()`, so this translation unit pulls in NO `libm` dependency. That
 * matters because it is compiled into every app (it lives in the always-built
 * `ra8_hal` source set); linking `libm` into apps that never quantize would be
 * an unwanted cost. Unreferenced, `--gc-sections` drops it entirely.
 *
 * ## Not device-gated
 *
 * Unlike `ra8_npu.h` this header is NOT behind `RA8_HAS_NPU`: the mapping is
 * plain arithmetic with no MMIO, useful (and host-testable) on any device, and
 * the NPU-only driver stays the single owner of the `RA8_HAS_NPU` gate.
 *
 * ## Threading
 *
 * Re-entrant and stateless: every function reads only its arguments and writes
 * only the caller's output buffer. Safe to call from any context.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_npu_quant_range_t
 * @brief Saturation bounds of the supported quantized element types.
 *
 * @details The quantize helpers clamp the rounded result to the closed range of
 *          the destination element type so an out-of-range real value saturates
 *          instead of wrapping. INT8 spans [-128, 127]; UINT8 spans [0, 255].
 *
 * @invariant `k_ra8_npu_quant_i8_min <= k_ra8_npu_quant_i8_max`.
 * @invariant `k_ra8_npu_quant_u8_min <= k_ra8_npu_quant_u8_max`.
 * @see ra8_npu_quantize_i8
 * @see ra8_npu_quantize_u8
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_ra8_npu_quant_i8_min = -128, /**< INT8 lower saturation bound.  */
  k_ra8_npu_quant_i8_max = 127,  /**< INT8 upper saturation bound.  */
  k_ra8_npu_quant_u8_min = 0,    /**< UINT8 lower saturation bound. */
  k_ra8_npu_quant_u8_max = 255,  /**< UINT8 upper saturation bound. */
} ra8_npu_quant_range_t;

/**
 * @brief Quantize a float arena into an INT8 tensor (affine, saturating).
 *
 * @details
 * Maps each `in[i]` to `clamp(round(in[i] / scale) + zero_point, -128, 127)`.
 * Rounding is half-away-from-zero. The intermediate is bounded before the cast
 * so an extreme real value saturates rather than triggering undefined
 * float-to-int conversion.
 *
 * @param[in]  in         Source float arena of @p count elements (non-NULL).
 * @param[out] out        Destination INT8 arena of @p count elements (non-NULL).
 * @param[in]  count      Number of elements to convert (0 is a valid no-op).
 * @param[in]  scale      Quantization scale, strictly positive (> 0).
 * @param[in]  zero_point Quantization zero-point (added after rounding).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok All @p count elements quantized into @p out.
 * @retval k_ra8_err_null_ptr `in` or `out` was nullptr.
 * @retval k_ra8_err_invalid_arg `scale` was not strictly positive.
 *
 * @pre `in` and `out` are non-NULL and each address @p count elements.
 * @pre `scale` is strictly positive.
 * @post On success each `out[i]` holds the saturated INT8 quantization of `in[i]`.
 * @post On any error @p out is left unmodified.
 *
 * @note Re-entrant; no shared state.
 * @see ra8_npu_dequantize_i8
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: 2 preconditions, 2 postconditions
 * - Rule 7: returns ra8_err_t, marked [[nodiscard]]
 */
[[nodiscard]] ra8_err_t
ra8_npu_quantize_i8(const float* in, int8_t* out, size_t count, float scale, int32_t zero_point);

/**
 * @brief Dequantize an INT8 tensor into a float arena (affine).
 *
 * @details Maps each `in[i]` to `scale * (in[i] - zero_point)`.
 *
 * @param[in]  in         Source INT8 arena of @p count elements (non-NULL).
 * @param[out] out        Destination float arena of @p count elements (non-NULL).
 * @param[in]  count      Number of elements to convert (0 is a valid no-op).
 * @param[in]  scale      Quantization scale (the same value used to quantize).
 * @param[in]  zero_point Quantization zero-point (subtracted before scaling).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok All @p count elements dequantized into @p out.
 * @retval k_ra8_err_null_ptr `in` or `out` was nullptr.
 *
 * @pre `in` and `out` are non-NULL and each address @p count elements.
 * @pre `scale` matches the value used to quantize @p in.
 * @post On success each `out[i]` holds `scale * (in[i] - zero_point)`.
 * @post On any error @p out is left unmodified.
 *
 * @note Re-entrant; no shared state.
 * @see ra8_npu_quantize_i8
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: 2 preconditions, 2 postconditions
 * - Rule 7: returns ra8_err_t, marked [[nodiscard]]
 */
[[nodiscard]] ra8_err_t
ra8_npu_dequantize_i8(const int8_t* in, float* out, size_t count, float scale, int32_t zero_point);

/**
 * @brief Quantize a float arena into a UINT8 tensor (affine, saturating).
 *
 * @details
 * Maps each `in[i]` to `clamp(round(in[i] / scale) + zero_point, 0, 255)`.
 * Rounding is half-away-from-zero. The intermediate is bounded before the cast
 * so an extreme real value saturates rather than triggering undefined
 * float-to-int conversion.
 *
 * @param[in]  in         Source float arena of @p count elements (non-NULL).
 * @param[out] out        Destination UINT8 arena of @p count elements (non-NULL).
 * @param[in]  count      Number of elements to convert (0 is a valid no-op).
 * @param[in]  scale      Quantization scale, strictly positive (> 0).
 * @param[in]  zero_point Quantization zero-point (added after rounding).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok All @p count elements quantized into @p out.
 * @retval k_ra8_err_null_ptr `in` or `out` was nullptr.
 * @retval k_ra8_err_invalid_arg `scale` was not strictly positive.
 *
 * @pre `in` and `out` are non-NULL and each address @p count elements.
 * @pre `scale` is strictly positive.
 * @post On success each `out[i]` holds the saturated UINT8 quantization of `in[i]`.
 * @post On any error @p out is left unmodified.
 *
 * @note Re-entrant; no shared state.
 * @see ra8_npu_dequantize_u8
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: 2 preconditions, 2 postconditions
 * - Rule 7: returns ra8_err_t, marked [[nodiscard]]
 */
[[nodiscard]] ra8_err_t
ra8_npu_quantize_u8(const float* in, uint8_t* out, size_t count, float scale, int32_t zero_point);

/**
 * @brief Dequantize a UINT8 tensor into a float arena (affine).
 *
 * @details Maps each `in[i]` to `scale * (in[i] - zero_point)`.
 *
 * @param[in]  in         Source UINT8 arena of @p count elements (non-NULL).
 * @param[out] out        Destination float arena of @p count elements (non-NULL).
 * @param[in]  count      Number of elements to convert (0 is a valid no-op).
 * @param[in]  scale      Quantization scale (the same value used to quantize).
 * @param[in]  zero_point Quantization zero-point (subtracted before scaling).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok All @p count elements dequantized into @p out.
 * @retval k_ra8_err_null_ptr `in` or `out` was nullptr.
 *
 * @pre `in` and `out` are non-NULL and each address @p count elements.
 * @pre `scale` matches the value used to quantize @p in.
 * @post On success each `out[i]` holds `scale * (in[i] - zero_point)`.
 * @post On any error @p out is left unmodified.
 *
 * @note Re-entrant; no shared state.
 * @see ra8_npu_quantize_u8
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: 2 preconditions, 2 postconditions
 * - Rule 7: returns ra8_err_t, marked [[nodiscard]]
 */
[[nodiscard]] ra8_err_t
ra8_npu_dequantize_u8(const uint8_t* in, float* out, size_t count, float scale, int32_t zero_point);

#ifdef __cplusplus
}
#endif
