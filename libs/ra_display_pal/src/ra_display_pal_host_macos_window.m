/**
 * @file ra_display_pal_host_macos_window.m
 * @brief Cocoa/AppKit window shim for the host macOS display backend
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Real implementation of ``ra_display_pal_host_macos_window.h`` using
 * AppKit. Opens a single titled window, converts the backend's RGB565
 * framebuffer to BGRA on every present, and draws it through a
 * CoreGraphics image in the content view. Compiled only by the desktop
 * demo (``examples/host/floorplan``); the firmware / test build links
 * the headless ``..._window_sim.c`` instead.
 *
 * The whole TU is guarded by ``__APPLE__`` so it is inert if it is ever
 * swept into a non-macOS build.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#if defined(__APPLE__)

#include "ra_display_pal_host_macos_window.h"

#import <Cocoa/Cocoa.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_display_pal_host_macos_cgimage.h"
#include "ra_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_display_pal_host_macos_window";

/**
 * @enum ra_macos_window_key_t
 * @brief Virtual key codes the event pump acts on.
 *
 * @details
 * Carbon HIToolbox virtual key codes (independent of keyboard layout).
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_vk_escape = 53U, /**< kVK_Escape. */
} ra_macos_window_key_t;

/** @brief Window content-rect origin (x and y). Float, so const not enum. */
static const CGFloat k_macos_window_origin = 0.0;

/**
 * @struct ra_macos_window
 * @brief Opaque handle token. State lives in this TU's file-statics
 *        because AppKit only supports one app object per process.
 */
struct ra_macos_window {
  int reserved;
};

/** @brief Single handle token returned by ra_macos_window_open. */
static struct ra_macos_window s_handle;

/** @brief The app's window. nil before open / after close. */
static NSWindow* s_window = nil;

/** @brief Last frame as a CoreGraphics image; NULL until first present. */
static CGImageRef s_image = NULL;

/** @brief Set true once the user closes the window. */
static bool s_closed = false;

/** @brief Framebuffer-space column of the last unprocessed mouse click. */
static uint16_t s_click_x = 0U;

/** @brief Framebuffer-space row of the last unprocessed mouse click. */
static uint16_t s_click_y = 0U;

/** @brief Set on mouse-down; cleared when polled. */
static bool s_has_click = false;

/**
 * @brief Content view that blits the latest framebuffer image.
 */
@interface RaFloorView : NSView
@end

@implementation RaFloorView
- (void)drawRect:(NSRect)dirtyRect
{
  (void)dirtyRect;
  if (s_image == NULL) {
    return;
  }
  CGContextRef cg = [[NSGraphicsContext currentContext] CGContext];
  CGContextDrawImage(cg, self.bounds, s_image);
}
- (void)mouseDown:(NSEvent*)event
{
  /* Window coords -> framebuffer coords (top-left origin). The view is
   * not flipped, so invert y. */
  const NSPoint p  = [self convertPoint:event.locationInWindow fromView:nil];
  const CGFloat hh = self.bounds.size.height;
  CGFloat       fx = p.x;
  CGFloat       fy = hh - p.y;
  if (fx < 0.0) {
    fx = 0.0;
  }
  if (fy < 0.0) {
    fy = 0.0;
  }
  s_click_x   = (uint16_t)fx;
  s_click_y   = (uint16_t)fy;
  s_has_click = true;
}
@end

/**
 * @brief Window delegate that records the close request.
 */
@interface RaWinDelegate : NSObject <NSWindowDelegate>
@end

@implementation RaWinDelegate
- (void)windowWillClose:(NSNotification*)note
{
  (void)note;
  s_closed = true;
}
@end

/** @brief Strong reference to the delegate so ARC keeps it alive. */
static RaWinDelegate* s_delegate = nil;

/** @brief Strong reference to the content view. */
static RaFloorView* s_view = nil;

ra_err_t ra_macos_window_open(uint16_t            width_px,
                              uint16_t            height_px,
                              const char*         title,
                              ra_macos_window_t** out_win)
{
  RA_CHECK_NULL_PTR(title, s_tag, "title");
  RA_CHECK_NULL_PTR(out_win, s_tag, "out_win");
  if (width_px == 0U || height_px == 0U) {
    return k_ra_err_invalid_arg;
  }

  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    const NSRect            frame = NSMakeRect(k_macos_window_origin,
                                               k_macos_window_origin,
                                               (CGFloat)width_px,
                                               (CGFloat)height_px);
    const NSWindowStyleMask mask =
      NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;

    s_window = [[NSWindow alloc] initWithContentRect:frame
                                           styleMask:mask
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    if (s_window == nil) {
      return k_ra_err_hw_init_failed;
    }
    [s_window setTitle:[NSString stringWithUTF8String:title]];

    s_view = [[RaFloorView alloc] initWithFrame:frame];
    [s_window setContentView:s_view];

    s_delegate = [[RaWinDelegate alloc] init];
    [s_window setDelegate:s_delegate];

    [s_window center];
    [s_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp finishLaunching];
    s_closed = false;
  }

  *out_win = &s_handle;
  return k_ra_ok;
}

ra_err_t ra_macos_window_present(ra_macos_window_t* win,
                                 const void*        rgb565,
                                 uint16_t           width_px,
                                 uint16_t           height_px,
                                 uint32_t           stride_bytes)
{
  RA_CHECK_NULL_PTR(win, s_tag, "win");
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

  @autoreleasepool {
    if (s_image != NULL) {
      CGImageRelease(s_image);
    }
    s_image = img;
    [s_view setNeedsDisplay:YES];
  }
  return k_ra_ok;
}

ra_err_t ra_macos_window_pump(ra_macos_window_t* win, bool* out_should_close)
{
  RA_CHECK_NULL_PTR(win, s_tag, "win");
  RA_CHECK_NULL_PTR(out_should_close, s_tag, "out_should_close");
  @autoreleasepool {
    for (;;) {
      NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES];
      if (ev == nil) {
        break;
      }
      /* This app has no main menu, so the standard Cmd+Q / Cmd+W key
       * equivalents are never auto-dispatched. Detect them here (plus
       * Esc) and request a clean close through the run loop. */
      if ([ev type] == NSEventTypeKeyDown) {
        const bool cmd      = ([ev modifierFlags] & NSEventModifierFlagCommand) != 0U;
        NSString*  chars    = [ev charactersIgnoringModifiers];
        const bool quit_key = cmd && ([chars isEqualToString:@"q"] || [chars isEqualToString:@"w"]);
        const bool esc_key  = ([ev keyCode] == (uint16_t)k_vk_escape);
        if (quit_key || esc_key) {
          s_closed = true;
        }
      }
      [NSApp sendEvent:ev];
    }
  }
  *out_should_close = s_closed;
  return k_ra_ok;
}

ra_err_t ra_macos_window_poll_click(ra_macos_window_t* win,
                                    bool*              out_has_click,
                                    uint16_t*          out_x,
                                    uint16_t*          out_y)
{
  RA_CHECK_NULL_PTR(win, s_tag, "win");
  RA_CHECK_NULL_PTR(out_has_click, s_tag, "out_has_click");
  RA_CHECK_NULL_PTR(out_x, s_tag, "out_x");
  RA_CHECK_NULL_PTR(out_y, s_tag, "out_y");
  *out_has_click = s_has_click;
  *out_x         = s_click_x;
  *out_y         = s_click_y;
  s_has_click    = false;
  return k_ra_ok;
}

ra_err_t ra_macos_window_close(ra_macos_window_t* win)
{
  RA_CHECK_NULL_PTR(win, s_tag, "win");
  @autoreleasepool {
    if (s_window != nil) {
      [s_window setDelegate:nil];
      [s_window close];
    }
    s_window   = nil;
    s_view     = nil;
    s_delegate = nil;
    if (s_image != NULL) {
      CGImageRelease(s_image);
      s_image = NULL;
    }
    s_closed = true;
  }
  return k_ra_ok;
}

#endif /* defined(__APPLE__) */
