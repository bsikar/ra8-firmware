/**
 * @file ra_display_pal_host_macos_cgimage.m
 * @brief RGB565 framebuffer -> CGImage factory (shared by window + PNG)
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Single implementation of the "convert + wrap as CGImage" step used by
 * the live-window shim and the headless PNG exporter. Compiled only on
 * macOS (the ``.m`` is excluded from the firmware / unit-test glob).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#if defined(__APPLE__)

#include "ra_display_pal_host_macos_cgimage.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "ra_check.h"
#include "ra_display_pal_host_macos_pixels.h"
#include "ra_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_display_pal_host_macos_cgimage";

ra_err_t ra_macos_framebuffer_to_cgimage(const void* rgb565,
                                         uint16_t    width_px,
                                         uint16_t    height_px,
                                         uint32_t    stride_bytes,
                                         CGImageRef* out_image)
{
  RA_CHECK_NULL_PTR(rgb565, s_tag, "rgb565");
  RA_CHECK_NULL_PTR(out_image, s_tag, "out_image");
  if (width_px == 0U || height_px == 0U || stride_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }

  const size_t count = (size_t)width_px * (size_t)height_px;
  uint32_t*    argb  = (uint32_t*)malloc(count * sizeof(uint32_t));
  if (argb == NULL) {
    return k_ra_err_no_mem;
  }
  const ra_err_t ce = ra_macos_rgb565_to_argb8888(argb, rgb565, width_px, height_px, stride_bytes);
  if (ce != k_ra_ok) {
    free(argb);
    return ce;
  }

  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef bmp = CGBitmapContextCreate(argb,
                                           (size_t)width_px,
                                           (size_t)height_px,
                                           (size_t)k_macos_argb_bits_per_component,
                                           (size_t)width_px * (size_t)k_macos_argb_bytes_per_pixel,
                                           cs,
                                           kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
  CGImageRef   img = (bmp != NULL) ? CGBitmapContextCreateImage(bmp) : NULL;
  if (bmp != NULL) {
    CGContextRelease(bmp);
  }
  CGColorSpaceRelease(cs);
  free(argb); /* CGBitmapContextCreateImage took its own copy. */

  if (img == NULL) {
    return k_ra_err_no_mem;
  }
  *out_image = img;
  return k_ra_ok;
}

#endif /* defined(__APPLE__) */
