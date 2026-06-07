/**
 * @file board_view.m
 * @brief Cocoa implementation of the board_sim desktop window (see board_view.h)
 *
 * @details
 * A custom NSView holds a CGImage built from the supplied RGB565 framebuffer
 * and blits it (nearest-neighbour, top-left origin) to fill the window. The
 * app runs cooperatively: board_view_pump drains pending events from the
 * caller's loop rather than taking over with [NSApp run], so the emulator
 * keeps stepping between frames. Built with -fobjc-arc; the CGImageRef (a CF
 * type, not ARC-managed) is released by hand.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "board_view.h"

#import <Cocoa/Cocoa.h>

#include <stdint.h>
#include <stdlib.h>

/** @brief Content view that draws the latest emulated frame. */
@interface BoardImageView : NSView
@property(nonatomic, assign) CGImageRef image;
@end

@implementation BoardImageView
- (void)drawRect:(NSRect)dirty
{
  (void)dirty;
  if (self.image == NULL) {
    return;
  }
  CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
  const CGFloat w  = self.bounds.size.width;
  const CGFloat h  = self.bounds.size.height;
  /* Flip Y so framebuffer row 0 lands at the top of the window. */
  CGContextSaveGState(ctx);
  CGContextTranslateCTM(ctx, 0.0, h);
  CGContextScaleCTM(ctx, 1.0, -1.0);
  CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
  CGContextDrawImage(ctx, CGRectMake(0.0, 0.0, w, h), self.image);
  CGContextRestoreGState(ctx);
}
- (void)dealloc
{
  if (_image != NULL) {
    CGImageRelease(_image);
  }
}
@end

struct board_view {
  NSWindow*       window;
  BoardImageView* view;
};

board_view_t* board_view_open(uint16_t width_px, uint16_t height_px, const char* title)
{
  if ((width_px == 0U) || (height_px == 0U)) {
    return nullptr;
  }
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    const NSRect    rect = NSMakeRect(0.0, 0.0, (CGFloat)width_px, (CGFloat)height_px);
    const NSUInteger style =
      NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    NSWindow* win = [[NSWindow alloc] initWithContentRect:rect
                                               styleMask:style
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    if (win == nil) {
      return nullptr;
    }
    [win setReleasedWhenClosed:NO];
    if (title != nullptr) {
      [win setTitle:[NSString stringWithUTF8String:title]];
    }
    BoardImageView* v = [[BoardImageView alloc] initWithFrame:rect];
    [win setContentView:v];
    [win center];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    board_view_t* bv = (board_view_t*)calloc(1U, sizeof(*bv));
    if (bv == nullptr) {
      return nullptr;
    }
    bv->window = win;
    bv->view   = v;
    return bv;
  }
}

void board_view_present(board_view_t* view,
                        const uint16_t* rgb565,
                        uint16_t        width_px,
                        uint16_t        height_px)
{
  if ((view == nullptr) || (rgb565 == nullptr) || (width_px == 0U) || (height_px == 0U)) {
    return;
  }
  @autoreleasepool {
    const size_t n    = (size_t)width_px * (size_t)height_px;
    uint32_t*    argb = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (argb == nullptr) {
      return;
    }
    for (size_t i = 0U; i < n; i++) {
      const uint16_t p  = rgb565[i];
      const uint32_t r5 = (uint32_t)((p >> 11) & 0x1FU);
      const uint32_t g6 = (uint32_t)((p >> 5) & 0x3FU);
      const uint32_t b5 = (uint32_t)(p & 0x1FU);
      const uint32_t r  = (r5 << 3) | (r5 >> 2);
      const uint32_t g  = (g6 << 2) | (g6 >> 4);
      const uint32_t b  = (b5 << 3) | (b5 >> 2);
      argb[i]           = (r << 16) | (g << 8) | b; /* 0x00RRGGBB, drawn opaque */
    }
    CGColorSpaceRef cs  = CGColorSpaceCreateDeviceRGB();
    CGContextRef    bmp = CGBitmapContextCreate(argb,
                                             (size_t)width_px,
                                             (size_t)height_px,
                                             8,
                                             (size_t)width_px * 4U,
                                             cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = (bmp != NULL) ? CGBitmapContextCreateImage(bmp) : NULL;
    if (bmp != NULL) {
      CGContextRelease(bmp);
    }
    CGColorSpaceRelease(cs);
    free(argb);
    if (img != NULL) {
      CGImageRef old        = view->view.image;
      view->view.image      = img;
      if (old != NULL) {
        CGImageRelease(old);
      }
      [view->view setNeedsDisplay:YES];
    }
  }
}

bool board_view_pump(board_view_t* view)
{
  if (view == nullptr) {
    return true;
  }
  @autoreleasepool {
    NSEvent* ev = nil;
    while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                    untilDate:[NSDate distantPast]
                                       inMode:NSDefaultRunLoopMode
                                      dequeue:YES]) != nil) {
      [NSApp sendEvent:ev];
    }
    return ![view->window isVisible];
  }
}

void board_view_close(board_view_t* view)
{
  if (view == nullptr) {
    return;
  }
  @autoreleasepool {
    [view->window close];
    view->window = nil;
    view->view   = nil;
  }
  free(view);
}
