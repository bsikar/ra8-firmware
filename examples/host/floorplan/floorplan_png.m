/**
 * @file floorplan_png.m
 * @brief Headless PNG export for the floor-plan demo
 *
 * @details
 * Builds a CGImage from the RGB565 framebuffer via the shared
 * ``ra_macos_framebuffer_to_cgimage`` factory, then encodes it as a PNG
 * with NSBitmapImageRep and writes it to disk. No NSApplication / window
 * is created, so this runs in a headless shell. Guarded by ``__APPLE__``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#if defined(__APPLE__)

#include "floorplan_png.h"

#import <Cocoa/Cocoa.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_display_pal_host_macos_cgimage.h"
#include "ra_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "floorplan_png";

ra_err_t floorplan_write_png(const char* path,
                             const void* rgb565,
                             uint16_t    width_px,
                             uint16_t    height_px,
                             uint32_t    stride_bytes)
{
  RA_CHECK_NULL_PTR(path, s_tag, "path");
  RA_CHECK_NULL_PTR(rgb565, s_tag, "rgb565");
  if (width_px == 0U || height_px == 0U || stride_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }

  CGImageRef     img = NULL;
  const ra_err_t ce =
    ra_macos_framebuffer_to_cgimage(rgb565, width_px, height_px, stride_bytes, &img);
  if (ce != k_ra_ok) {
    return ce;
  }

  ra_err_t rc = k_ra_ok;
  @autoreleasepool {
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    NSData*           png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSString*         p   = [NSString stringWithUTF8String:path];
    if (png == nil || p == nil || ![png writeToFile:p atomically:YES]) {
      rc = k_ra_err_hw_error;
    }
  }
  CGImageRelease(img);
  return rc;
}

#endif /* defined(__APPLE__) */
