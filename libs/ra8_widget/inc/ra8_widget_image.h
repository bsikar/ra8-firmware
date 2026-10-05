/**
 * @file ra8_widget_image.h
 * @brief Greyscale image leaf widget for the ra8_widget tree.
 * @ingroup grp_ereader
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "ra8_err.h"
#include "ra8_widget.h"

typedef enum : uint8_t {
  k_ra8_widget_image_fit = 0,
  k_ra8_widget_image_fill = 1,
} ra8_widget_image_scale_t;

/** Caller-owned decoded greyscale bitmap. Pixels remain valid while rendered. */
typedef struct ra8_widget_image {
  const ra8_widget_paint_t* paint;
  const uint8_t* pixels;
  uint32_t width;
  uint32_t height;
  ra8_widget_image_scale_t scale;
  uint8_t placeholder_fill;
  uint8_t placeholder_border;
  uint8_t reserved;
  int32_t placeholder_border_width;
} ra8_widget_image_t;

const ra8_widget_vtable_t* ra8_widget_image_vtable(void);
[[nodiscard]] ra8_err_t ra8_widget_image_init(ra8_widget_t* widget, ra8_widget_image_t* image);

#ifdef __cplusplus
}
#endif
