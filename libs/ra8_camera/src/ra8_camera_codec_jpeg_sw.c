/**
 * @file ra8_camera_codec_jpeg_sw.c
 * @brief `ra8_jpeg_sw` adapter for the generic camera codec facade.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details Samples RGB888 or UYVY422 into the configured packed-RGB workspace,
 * delegates baseline-JPEG encoding, and translates the produced byte count
 * into a generic JPEG frame view.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_camera_codec_jpeg_sw.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_camera_internal.h"
#include "ra8_err.h"
#include "ra8_jpeg_sw.h"

/** @brief BT.601 limited-range fixed-point conversion constants. */
typedef enum : int32_t {
  k_camera_jpeg_luma_black = 16,  /**< Limited-range black level.        */
  k_camera_jpeg_chroma_mid = 128, /**< Neutral chroma level.             */
  k_camera_jpeg_luma_scale = 298, /**< Fixed-point luma scale.           */
  k_camera_jpeg_red_cr     = 409, /**< Cr contribution to red.           */
  k_camera_jpeg_green_cb   = 100, /**< Cb subtraction from green.        */
  k_camera_jpeg_green_cr   = 208, /**< Cr subtraction from green.        */
  k_camera_jpeg_blue_cb    = 516, /**< Cb contribution to blue.          */
  k_camera_jpeg_rounding   = 128, /**< Fixed-point rounding bias.        */
  k_camera_jpeg_shift      = 8,   /**< Fixed-point fractional-bit count. */
  k_camera_jpeg_u8_max     = 255, /**< Maximum packed RGB channel value. */
} camera_jpeg_color_t;

/**
 * @brief Clamp a signed RGB component to one byte.
 *
 * @details Saturates a fixed-point conversion result into packed RGB range.
 * @param[in] component Signed channel value.
 * @return Clamped channel byte.
 * @retval 0 Component was below the packed range.
 * @retval 255 Component was above the packed range.
 * @pre `component` is a completed fixed-point channel calculation.
 * @pre No global state is required.
 * @post The result is in the inclusive range 0..255.
 * @post No state is modified.
 * @note Thread-safe and deterministic.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_jpeg_sw_clamp(int32_t component)
{
  if (component < 0) {
    return 0U;
  }
  if (component > (int32_t)k_camera_jpeg_u8_max) {
    return (uint8_t)k_camera_jpeg_u8_max;
  }
  return (uint8_t)component;
}

/**
 * @brief Convert one limited-range YCbCr pixel to packed RGB888.
 *
 * @details Applies the integer BT.601 limited-range transform and saturation.
 * @param[in]  y      Luma sample.
 * @param[in]  cb     Blue-difference chroma sample.
 * @param[in]  cr     Red-difference chroma sample.
 * @param[out] output Receives packed red, green, and blue bytes.
 * @pre `output` addresses at least three writable bytes.
 * @pre Input samples are complete unsigned bytes.
 * @post Exactly three output bytes are written.
 * @post Every output channel is in the inclusive range 0..255.
 * @note Thread-safe; uses no mutable shared state.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_jpeg_sw_ycbcr_to_rgb(uint8_t y, uint8_t cb, uint8_t cr, uint8_t* output)
{
  int32_t luma = (int32_t)y - (int32_t)k_camera_jpeg_luma_black;
  if (luma < 0) {
    luma = 0;
  }
  const int32_t blue_delta = (int32_t)cb - (int32_t)k_camera_jpeg_chroma_mid;
  const int32_t red_delta  = (int32_t)cr - (int32_t)k_camera_jpeg_chroma_mid;
  output[0]                = internal_jpeg_sw_clamp(((int32_t)k_camera_jpeg_luma_scale * luma +
                                                     (int32_t)k_camera_jpeg_red_cr * red_delta +
                                                     (int32_t)k_camera_jpeg_rounding) >>
                                                    (int32_t)k_camera_jpeg_shift);
  output[1]                = internal_jpeg_sw_clamp(
    ((int32_t)k_camera_jpeg_luma_scale * luma - (int32_t)k_camera_jpeg_green_cb * blue_delta -
     (int32_t)k_camera_jpeg_green_cr * red_delta + (int32_t)k_camera_jpeg_rounding) >>
    (int32_t)k_camera_jpeg_shift);
  output[2] = internal_jpeg_sw_clamp(((int32_t)k_camera_jpeg_luma_scale * luma +
                                      (int32_t)k_camera_jpeg_blue_cb * blue_delta +
                                      (int32_t)k_camera_jpeg_rounding) >>
                                     (int32_t)k_camera_jpeg_shift);
}

/**
 * @brief Read one source pixel as RGB888.
 *
 * @details Copies RGB input directly or decodes the selected pixel from its
 *          shared UYVY chroma pair.
 * @param[in]  input  Valid RGB888 or UYVY422 frame.
 * @param[in]  x      Source column.
 * @param[in]  y      Source row.
 * @param[out] output Receives packed red, green, and blue bytes.
 * @pre `x < input->width` and `y < input->height`.
 * @pre `output` addresses at least three writable bytes.
 * @post Exactly three output bytes are written.
 * @post Input bytes are unchanged.
 * @note Thread-safe with respect to immutable input storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_jpeg_sw_read_rgb(const ra8_camera_frame_t* input, uint32_t x, uint32_t y, uint8_t* output)
{
  const uint32_t row = y * input->stride_bytes;
  if (input->format == k_ra8_camera_format_rgb888) {
    const uint32_t offset = row + (x * 3U);
    output[0]             = input->data[offset];
    output[1]             = input->data[offset + 1U];
    output[2]             = input->data[offset + 2U];
    return;
  }
  const uint32_t pair = row + ((x & ~1U) * 2U);
  const uint32_t yoff = ((x & 1U) == 0U) ? 1U : 3U;
  internal_jpeg_sw_ycbcr_to_rgb(input->data[pair + yoff],
                                input->data[pair],
                                input->data[pair + 2U],
                                output);
}

/**
 * @brief Nearest-neighbour sample input into the configured RGB workspace.
 *
 * @details Maps each configured output coordinate into the source frame and
 *          materializes a tightly packed RGB image for the JPEG encoder.
 * @param[in] state Backend state holding output geometry and workspace.
 * @param[in] input Valid RGB888 or UYVY422 input frame.
 * @pre State configuration passed backend initialization.
 * @pre Input storage remains readable throughout conversion.
 * @post The complete configured RGB workspace prefix is initialized.
 * @post Input storage and backend configuration are unchanged.
 * @note Not thread-safe with respect to the shared workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_jpeg_sw_prepare_rgb(const ra8_camera_codec_jpeg_sw_state_t* state,
                                                      const ra8_camera_frame_t*               input)
{
  const uint32_t out_width  = (uint32_t)state->cfg.output_width;
  const uint32_t out_height = (uint32_t)state->cfg.output_height;
  for (uint32_t y = 0U; y < out_height; y += 1U) {
    const uint32_t source_y = (y * (uint32_t)input->height) / out_height;
    for (uint32_t x = 0U; x < out_width; x += 1U) {
      const uint32_t source_x = (x * (uint32_t)input->width) / out_width;
      internal_jpeg_sw_read_rgb(input,
                                source_x,
                                source_y,
                                &state->cfg.rgb_workspace[(((size_t)y * out_width) + x) * 3U]);
    }
  }
}

/**
 * @brief Convert and encode one raw frame with `ra8_jpeg_sw`.
 *
 * @details Converts or samples the input into caller-owned RGB workspace, then
 *          writes a baseline JPEG into the per-call output buffer.
 * @param[in]  ctx           Bound software-JPEG state.
 * @param[in]  input         Valid RGB888 or UYVY422 frame.
 * @param[in]  output_buffer Caller-owned JPEG destination.
 * @param[out] out_frame     Receives the encoded JPEG view.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok JPEG encoded.
 * @retval k_ra8_err_null_ptr Required state, frame, or buffer pointer is NULL.
 * @retval k_ra8_err_not_supported Input format is not raw RGB888 or UYVY422.
 * @retval other Forwarded from ::ra8_jpeg_sw_encode.
 * @pre Input passes generic frame validation.
 * @pre Workspace and output buffer do not overlap readable input storage.
 * @post On success `out_frame->data == output_buffer->data`.
 * @post On success the returned dimensions match configured output geometry.
 * @note Not thread-safe with respect to state workspace or output buffer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_jpeg_sw_encode(void*                      ctx,
                                                      const ra8_camera_frame_t*  input,
                                                      const ra8_camera_buffer_t* output_buffer,
                                                      ra8_camera_frame_t*        out_frame)
{
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (input == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (output_buffer == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_frame == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (output_buffer->data == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (output_buffer->capacity == 0U) {
    return k_ra8_err_null_ptr;
  }
  if (input->format != k_ra8_camera_format_rgb888) {
    if (input->format != k_ra8_camera_format_uyvy422) {
      return k_ra8_err_not_supported;
    }
  }
  const ra8_camera_codec_jpeg_sw_state_t* state = (const ra8_camera_codec_jpeg_sw_state_t*)ctx;
  internal_jpeg_sw_prepare_rgb(state, input);
  uint32_t        produced = 0U;
  const ra8_err_t encoded  = ra8_jpeg_sw_encode(state->cfg.rgb_workspace,
                                                state->cfg.output_width,
                                                state->cfg.output_height,
                                                state->cfg.quality,
                                                output_buffer->data,
                                                output_buffer->capacity,
                                                &produced);
  if (encoded != k_ra8_ok) {
    return encoded;
  }
  *out_frame = (ra8_camera_frame_t){
    .data         = output_buffer->data,
    .bytes        = produced,
    .stride_bytes = 0U,
    .width        = state->cfg.output_width,
    .height       = state->cfg.output_height,
    .format       = k_ra8_camera_format_jpeg,
  };
  return k_ra8_ok;
}

/** @brief Software JPEG codec vtable. */
static const ra8_camera_codec_iface_t s_jpeg_sw_codec_iface = {
  .encode = internal_jpeg_sw_encode,
};

ra8_err_t ra8_camera_codec_jpeg_sw_init(ra8_camera_codec_t*                   codec,
                                        ra8_camera_codec_jpeg_sw_state_t*     state,
                                        const ra8_camera_codec_jpeg_sw_cfg_t* cfg)
{
  if (codec == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (state == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->rgb_workspace == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->quality < (uint8_t)k_ra8_jpeg_sw_quality_min) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->quality > (uint8_t)k_ra8_jpeg_sw_quality_max) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->output_width == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->output_height == 0U) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t row_bytes = (uint32_t)cfg->output_width * 3U;
  if ((uint32_t)cfg->output_height > (UINT32_MAX / row_bytes)) {
    return k_ra8_err_invalid_size;
  }
  if (cfg->rgb_workspace_capacity < (row_bytes * (uint32_t)cfg->output_height)) {
    return k_ra8_err_invalid_size;
  }
  state->cfg   = *cfg;
  codec->iface = &s_jpeg_sw_codec_iface;
  codec->ctx   = state;
  return k_ra8_ok;
}
