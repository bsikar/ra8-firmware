/**
 * @file ra8_translate_ocr.h
 * @brief C23 ABI for deterministic OCR image cropping, rotation, bilinear resizing,
 * and affine int8 quantization.
 * @since 0.1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Pixel format supported by `ra8_translate` OCR preprocessing kernels.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_translate_ocr_format_gray8  = 1U, /**< 1 byte per pixel grayscale.   */
  k_ra8_translate_ocr_format_rgb888 = 3U, /**< 3 bytes per pixel packed RGB. */
} ra8_translate_ocr_format_t;

static_assert(sizeof(ra8_translate_ocr_format_t) == 1U, "ra8_translate_ocr_format_t width");

/**
 * @brief Orthogonal rotation applied after cropping and before destination resize.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_translate_ocr_rot_none   = 0U, /**< Identity orientation.                      */
  k_ra8_translate_ocr_rot_ccw_90 = 1U, /**< 90 deg CCW: vertical top-down -> LTR line. */
  k_ra8_translate_ocr_rot_cw_90  = 2U, /**< 90 deg clockwise rotation.                 */
  k_ra8_translate_ocr_rot_180    = 3U, /**< 180 deg inversion.                         */
} ra8_translate_ocr_rotation_t;

static_assert(sizeof(ra8_translate_ocr_rotation_t) == 1U, "ra8_translate_ocr_rotation_t width");

/**
 * @brief Coordinate mapping convention for bilinear source sample placement.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_translate_ocr_coord_half_pixel    = 0U, /**< (dst + 0.5) * src / dst_len - 0.5. */
  k_ra8_translate_ocr_coord_align_corners = 1U, /**< dst * (src - 1) / (dst_len - 1).   */
  k_ra8_translate_ocr_coord_asym_scale    = 2U, /**< dst * src / dst_len.               */
} ra8_translate_ocr_coord_mode_t;

static_assert(sizeof(ra8_translate_ocr_coord_mode_t) == 1U, "ra8_translate_ocr_coord_mode_t width");

/**
 * @brief Axis-aligned source crop rectangle in pixels.
 * @since 0.1.0
 */
typedef struct {
  uint16_t x;      /**< Left origin pixel coordinate. */
  uint16_t y;      /**< Top origin pixel coordinate.  */
  uint16_t width;  /**< Crop rectangle width (> 0).   */
  uint16_t height; /**< Crop rectangle height (> 0).  */
} ra8_translate_ocr_rect_t;

static_assert(sizeof(ra8_translate_ocr_rect_t) == 8U, "ra8_translate_ocr_rect_t size");
static_assert(alignof(ra8_translate_ocr_rect_t) == 2U, "ra8_translate_ocr_rect_t align");
static_assert(offsetof(ra8_translate_ocr_rect_t, x) == 0U, "ocr_rect x off");
static_assert(offsetof(ra8_translate_ocr_rect_t, y) == 2U, "ocr_rect y off");
static_assert(offsetof(ra8_translate_ocr_rect_t, width) == 4U, "ocr_rect width off");
static_assert(offsetof(ra8_translate_ocr_rect_t, height) == 6U, "ocr_rect height off");

/**
 * @brief Source image dimensions, byte stride, and pixel format.
 * @since 0.1.0
 */
typedef struct {
  uint32_t                   stride_bytes; /**< Bytes per source row (>= width * ch). */
  uint16_t                   width;        /**< Image width in pixels (> 0).          */
  uint16_t                   height;       /**< Image height in pixels (> 0).         */
  ra8_translate_ocr_format_t format;       /**< Source pixel layout.                  */
  uint8_t                    reserved[3];  /**< Reserved; must be zero.               */
} ra8_translate_ocr_image_spec_t;

static_assert(sizeof(ra8_translate_ocr_image_spec_t) == 12U, "ra8_translate_ocr_image_spec_t size");
static_assert(alignof(ra8_translate_ocr_image_spec_t) == 4U,
              "ra8_translate_ocr_image_spec_t align");
static_assert(offsetof(ra8_translate_ocr_image_spec_t, stride_bytes) == 0U,
              "ocr_image_spec stride off");
static_assert(offsetof(ra8_translate_ocr_image_spec_t, width) == 4U, "ocr_image_spec width off");
static_assert(offsetof(ra8_translate_ocr_image_spec_t, height) == 6U, "ocr_image_spec height off");
static_assert(offsetof(ra8_translate_ocr_image_spec_t, format) == 8U, "ocr_image_spec format off");
static_assert(offsetof(ra8_translate_ocr_image_spec_t, reserved) == 9U,
              "ocr_image_spec reserved off");

/**
 * @brief Destination tensor geometry, format, rotation, and resize grid policy.
 * @since 0.1.0
 */
typedef struct {
  uint16_t                       dst_width;  /**< Destination tensor width (> 0).  */
  uint16_t                       dst_height; /**< Destination tensor height (> 0). */
  ra8_translate_ocr_format_t     dst_format; /**< Target pixel format.             */
  ra8_translate_ocr_rotation_t   rotation;   /**< Rotation before scaling.         */
  ra8_translate_ocr_coord_mode_t coord_mode; /**< Bilinear sample coordinate grid. */
  uint8_t                        reserved;   /**< Reserved; must be zero.          */
} ra8_translate_ocr_preprocess_cfg_t;

static_assert(sizeof(ra8_translate_ocr_preprocess_cfg_t) == 8U,
              "ra8_translate_ocr_preprocess_cfg_t size");
static_assert(alignof(ra8_translate_ocr_preprocess_cfg_t) == 2U,
              "ra8_translate_ocr_preprocess_cfg_t align");
static_assert(offsetof(ra8_translate_ocr_preprocess_cfg_t, dst_width) == 0U,
              "ocr_cfg dst_width off");
static_assert(offsetof(ra8_translate_ocr_preprocess_cfg_t, dst_height) == 2U,
              "ocr_cfg dst_height off");
static_assert(offsetof(ra8_translate_ocr_preprocess_cfg_t, dst_format) == 4U,
              "ocr_cfg dst_format off");
static_assert(offsetof(ra8_translate_ocr_preprocess_cfg_t, rotation) == 5U, "ocr_cfg rotation off");
static_assert(offsetof(ra8_translate_ocr_preprocess_cfg_t, coord_mode) == 6U,
              "ocr_cfg coord_mode off");
static_assert(offsetof(ra8_translate_ocr_preprocess_cfg_t, reserved) == 7U, "ocr_cfg reserved off");

/**
 * @brief Floating-point normalization and TFLite int8 quantization parameters.
 * @since 0.1.0
 */
typedef struct {
  float   mean[3];     /**< Per-channel mean (channel 0 for gray8).       */
  float   inv_std[3];  /**< Per-channel reciprocal std (> 0, finite).     */
  float   input_scale; /**< Pixel pre-scale (e.g. 1/255.0f, > 0, finite). */
  float   quant_scale; /**< TFLite output scale (> 0, finite).            */
  int8_t  zero_point;  /**< TFLite int8 zero point (-128..127).           */
  uint8_t reserved[3]; /**< Reserved; must be zero.                       */
} ra8_translate_ocr_quant_t;

static_assert(sizeof(ra8_translate_ocr_quant_t) == 36U, "ra8_translate_ocr_quant_t size");
static_assert(alignof(ra8_translate_ocr_quant_t) == 4U, "ra8_translate_ocr_quant_t align");
static_assert(offsetof(ra8_translate_ocr_quant_t, mean) == 0U, "ocr_quant mean off");
static_assert(offsetof(ra8_translate_ocr_quant_t, inv_std) == 12U, "ocr_quant inv_std off");
static_assert(offsetof(ra8_translate_ocr_quant_t, input_scale) == 24U, "ocr_quant input_scale off");
static_assert(offsetof(ra8_translate_ocr_quant_t, quant_scale) == 28U, "ocr_quant quant_scale off");
static_assert(offsetof(ra8_translate_ocr_quant_t, zero_point) == 32U, "ocr_quant zero_point off");
static_assert(offsetof(ra8_translate_ocr_quant_t, reserved) == 33U, "ocr_quant reserved off");

/**
 * @brief Report whether this build targets ARMv8.1-M Helium MVE instructions.
 *
 * @details Calling context: `task-safe-reentrant`. Reads a compile-time target
 * feature constant without hardware access or mutation.
 *
 * @param[out] out_has_helium Non-null boolean receiving Helium MVE capability flag.
 * @return ::k_ra8_ok on success, or ::k_ra8_err_null_ptr if @p out_has_helium is NULL.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_ocr_has_helium(bool* out_has_helium);

/**
 * @brief Compute packed byte count required for an OCR output tensor buffer.
 *
 * @details Calling context: `task-safe-reentrant`. Computes `dst_width * dst_height * channels`
 * with checked overflow and validates @p dst_format.
 *
 * @param[in] dst_width Destination tensor width in pixels (> 0).
 * @param[in] dst_height Destination tensor height in pixels (> 0).
 * @param[in] dst_format Target pixel format (`gray8` or `rgb888`).
 * @param[out] out_bytes Non-null storage receiving required byte count.
 * @return ::k_ra8_ok on success, or explicit error status leaving @p out_bytes untouched.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_ocr_required_bytes(uint16_t                   dst_width,
                                           uint16_t                   dst_height,
                                           ra8_translate_ocr_format_t dst_format,
                                           size_t*                    out_bytes);

/**
 * @brief Crop, rotate, convert channels, and bilinearly resize a source image to 8-bit pixels.
 *
 * @details Calling context: `task-safe-reentrant`. Samples strictly inside @p crop without
 * heap allocation and leaves caller output storage unmodified on validation error.
 *
 * @param[in] src Source pixel buffer described by @p src_spec.
 * @param[in] src_len Readable byte length of @p src.
 * @param[in] src_spec Source geometry, byte stride, and pixel format.
 * @param[in] crop Source rectangle bounds to extract before rotation.
 * @param[in] cfg Target tensor dimensions, format, rotation, and grid mode.
 * @param[out] out_dst Caller buffer receiving tightly packed 8-bit pixels.
 * @param[in] out_cap Capacity of @p out_dst in bytes.
 * @param[out] out_written Non-null storage receiving bytes written.
 * @return ::k_ra8_ok on success, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_ocr_resize_u8(const uint8_t*                            src,
                                      size_t                                    src_len,
                                      const ra8_translate_ocr_image_spec_t*     src_spec,
                                      const ra8_translate_ocr_rect_t*           crop,
                                      const ra8_translate_ocr_preprocess_cfg_t* cfg,
                                      uint8_t*                                  out_dst,
                                      size_t                                    out_cap,
                                      size_t*                                   out_written);

/**
 * @brief Normalize and affine-quantize tightly packed 8-bit pixels into int8 tensor values.
 *
 * @details Calling context: `task-safe-reentrant`. Uses Helium MVE vector instructions on
 * Cortex-M85 builds and a bit-identical scalar fallback on other targets.
 *
 * @param[in] src Packed 8-bit pixel slice (`src_len > 0`, divisible by channels).
 * @param[in] src_len Byte length of @p src.
 * @param[in] format Pixel format (`gray8` or `rgb888`).
 * @param[in] quant Validated normalization and quantization parameters.
 * @param[out] out_dst Caller int8 buffer receiving quantized activations (`[-128, 127]`).
 * @param[in] out_cap Element capacity of @p out_dst (`>= src_len`).
 * @param[out] out_written Non-null storage receiving element count written.
 * @return ::k_ra8_ok on success, or explicit error status with outputs untouched.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_ocr_normalize_i8(const uint8_t*                   src,
                                         size_t                           src_len,
                                         ra8_translate_ocr_format_t       format,
                                         const ra8_translate_ocr_quant_t* quant,
                                         int8_t*                          out_dst,
                                         size_t                           out_cap,
                                         size_t*                          out_written);

/**
 * @brief Fused crop, rotate, bilinear resize, channel convert, and int8 quantize pipeline.
 *
 * @details Calling context: `task-safe-reentrant`. Streams cropped/resized pixels through
 * a bounded stack buffer directly into int8 quantization using Helium MVE when available.
 *
 * @param[in] src Source image pixels described by @p src_spec.
 * @param[in] src_len Readable byte length of @p src.
 * @param[in] src_spec Source geometry, byte stride, and pixel format.
 * @param[in] crop Crop rectangle inside @p src_spec dimensions.
 * @param[in] cfg Target tensor geometry, pixel format, rotation, and coordinate mode.
 * @param[in] quant Validated normalization and int8 quantization parameters.
 * @param[out] out_dst Caller int8 buffer receiving packed quantized tensor bytes.
 * @param[in] out_cap Element capacity of @p out_dst.
 * @param[out] out_written Non-null storage receiving int8 count written.
 * @return ::k_ra8_ok on success, or explicit error status with outputs untouched.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_ocr_preprocess_i8(const uint8_t*                            src,
                                          size_t                                    src_len,
                                          const ra8_translate_ocr_image_spec_t*     src_spec,
                                          const ra8_translate_ocr_rect_t*           crop,
                                          const ra8_translate_ocr_preprocess_cfg_t* cfg,
                                          const ra8_translate_ocr_quant_t*          quant,
                                          int8_t*                                   out_dst,
                                          size_t                                    out_cap,
                                          size_t*                                   out_written);

#ifdef __cplusplus
}
#endif
