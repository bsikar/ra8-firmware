/**
 * @file ra8_viewer_view.m
 * @brief Cocoa implementation of the viewer window (see ra8_viewer_view.h).
 *
 * @details
 * A layer-backed NSView whose CALayer ``contents`` is a CGImage built directly
 * from the reader's RGB565 framebuffer -- the same contents-driven present as
 * board_sim's board_view.m, so a frame stays put during an event flood without a
 * ``drawRect:`` re-clear. The app runs cooperatively: ra8_viewer_view_pump drains
 * pending events from the caller's loop rather than taking over with [NSApp run],
 * so the reader stays in charge of when to render the next page. Arrow keys are
 * captured into two accumulators (page navigation and vertical scroll) the caller
 * drains each frame; a minimal main menu gives a working Cmd+Q. Built with
 * -fobjc-arc; CGImageRef (a CF type, not ARC-managed) is released by hand after
 * the layer takes its own retain.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "ra8_viewer_view.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * @enum ra8_viewer_key_t
 * @brief Cocoa arrow-key unichars captured by the content view (no magics).
 */
typedef enum : uint16_t {
  k_viewer_key_up    = 0xF700U, /**< NSUpArrowFunctionKey.    */
  k_viewer_key_down  = 0xF701U, /**< NSDownArrowFunctionKey.  */
  k_viewer_key_left  = 0xF702U, /**< NSLeftArrowFunctionKey.  */
  k_viewer_key_right = 0xF703U, /**< NSRightArrowFunctionKey. */
} ra8_viewer_key_t;

/**
 * @enum ra8_viewer_rgb_t
 * @brief RGB565 unpack constants for the framebuffer -> ARGB expansion.
 */
typedef enum : uint32_t {
  k_viewer_r_shift  = 11U,   /**< Red field shift in RGB565.      */
  k_viewer_g_shift  = 5U,    /**< Green field shift in RGB565.    */
  k_viewer_r_mask   = 0x1FU, /**< 5-bit red mask.                 */
  k_viewer_g_mask   = 0x3FU, /**< 6-bit green mask.               */
  k_viewer_b_mask   = 0x1FU, /**< 5-bit blue mask.                */
  k_viewer_argb_r   = 16U,   /**< Red byte shift in 0x00RRGGBB.   */
  k_viewer_argb_g   = 8U,    /**< Green byte shift in 0x00RRGGBB. */
  k_viewer_bpp_bits = 8U,    /**< Bits per channel in the bitmap. */
} ra8_viewer_rgb_t;

/**
 * @brief Layer-backed content view that shows the latest frame and records keys.
 */
@interface ViewerImageView : NSView
/** @brief Accumulated page-navigation delta (+right / -left) since last poll. */
@property(nonatomic, assign) int32_t navAccum;
/** @brief Accumulated vertical-scroll delta (+down / -up) since last poll. */
@property(nonatomic, assign) int32_t scrollAccum;
@end

@implementation ViewerImageView
/* Fully covered by the frame image (white page on a black pre-first-frame
 * layer), so report opacity and let AppKit skip erasing the window background. */
- (BOOL)isOpaque
{
  return YES;
}
/* Must be first responder for keyDown: to be delivered (NSView is NO by default);
 * ra8_viewer_view_open also makes this view the window's first responder. */
- (BOOL)acceptsFirstResponder
{
  return YES;
}
/* Left/Right arrows turn pages; Up/Down arrows scroll vertically (webtoon). Each
 * keystroke bumps the matching accumulator for the run loop to drain. */
- (void)keyDown:(NSEvent*)event
{
  NSString* s = event.characters;
  for (NSUInteger i = 0U; i < s.length; i++) {
    const unichar u = [s characterAtIndex:i];
    if (u == (unichar)k_viewer_key_right) {
      self.navAccum += 1;
    } else if (u == (unichar)k_viewer_key_left) {
      self.navAccum -= 1;
    } else if (u == (unichar)k_viewer_key_down) {
      self.scrollAccum += 1;
    } else if (u == (unichar)k_viewer_key_up) {
      self.scrollAccum -= 1;
    }
  }
}
/* Mouse-wheel / two-finger scroll drives the same vertical-scroll accumulator as
 * the Up/Down arrows, so a webtoon scrolls from either input. */
- (void)scrollWheel:(NSEvent*)event
{
  static const CGFloat k_viewer_scroll_deadzone = 0.5; /* ignore sub-notch jitter */
  const CGFloat        dy                       = event.scrollingDeltaY;
  if (dy > k_viewer_scroll_deadzone) {
    self.scrollAccum -= 1; /* content moves down -> reveal earlier rows */
  } else if (dy < -k_viewer_scroll_deadzone) {
    self.scrollAccum += 1;
  }
}
@end

struct ra8_viewer_view {
  NSWindow*        window;
  ViewerImageView* view;
};

/** @brief Install a minimal main menu so Cmd+Q (Quit) works under the manual pump. */
static void ra8_viewer_install_menu(void)
{
  if ([NSApp mainMenu] != nil) {
    return;
  }
  NSMenu*     menubar = [[NSMenu alloc] init];
  NSMenuItem* appItem = [[NSMenuItem alloc] init];
  [menubar addItem:appItem];
  NSMenu*     appMenu = [[NSMenu alloc] init];
  NSString*   name    = [[NSProcessInfo processInfo] processName];
  NSMenuItem* quit    = [[NSMenuItem alloc] initWithTitle:[@"Quit " stringByAppendingString:name]
                                                   action:@selector(terminate:)
                                            keyEquivalent:@"q"];
  [appMenu addItem:quit];
  [appItem setSubmenu:appMenu];
  [NSApp setMainMenu:menubar];
}

ra8_viewer_view_t* ra8_viewer_view_open(uint16_t width_px, uint16_t height_px, const char* title)
{
  if ((width_px == 0U) || (height_px == 0U)) {
    return nullptr;
  }
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    ra8_viewer_install_menu();

    const NSRect     rect = NSMakeRect(0.0, 0.0, (CGFloat)width_px, (CGFloat)height_px);
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
    ViewerImageView* v          = [[ViewerImageView alloc] initWithFrame:rect];
    v.wantsLayer                = YES;
    v.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    v.layer.magnificationFilter = kCAFilterNearest;
    v.layer.minificationFilter  = kCAFilterNearest;
    v.layer.opaque              = YES;
    v.layer.backgroundColor     = CGColorGetConstantColor(kCGColorBlack);
    [win setContentView:v];
    [win center];
    [win makeKeyAndOrderFront:nil];
    [win makeFirstResponder:v];
    [NSApp activateIgnoringOtherApps:YES];

    ra8_viewer_view_t* vv = (ra8_viewer_view_t*)calloc(1U, sizeof(*vv));
    if (vv == nullptr) {
      return nullptr;
    }
    vv->window = win;
    vv->view   = v;
    return vv;
  }
}

/**
 * @brief Build an ARGB CGImage from an RGB565 frame (caller releases it).
 * @param[in] rgb565    Row-major RGB565 pixels.
 * @param[in] width_px  Frame width.
 * @param[in] height_px Frame height.
 * @return A retained CGImageRef, or NULL on allocation failure.
 */
static CGImageRef
ra8_viewer_image_from_rgb565(const uint16_t* rgb565, uint16_t width_px, uint16_t height_px)
{
  const size_t n    = (size_t)width_px * (size_t)height_px;
  uint32_t*    argb = (uint32_t*)malloc(n * sizeof(uint32_t));
  if (argb == nullptr) {
    return NULL;
  }
  for (size_t i = 0U; i < n; i++) {
    const uint16_t p  = rgb565[i];
    const uint32_t r5 = (uint32_t)((p >> k_viewer_r_shift) & k_viewer_r_mask);
    const uint32_t g6 = (uint32_t)((p >> k_viewer_g_shift) & k_viewer_g_mask);
    const uint32_t b5 = (uint32_t)(p & k_viewer_b_mask);
    const uint32_t r  = (r5 << 3) | (r5 >> 2);
    const uint32_t g  = (g6 << 2) | (g6 >> 4);
    const uint32_t b  = (b5 << 3) | (b5 >> 2);
    argb[i]           = (r << k_viewer_argb_r) | (g << k_viewer_argb_g) | b;
  }
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef bmp = CGBitmapContextCreate(argb,
                                           (size_t)width_px,
                                           (size_t)height_px,
                                           (size_t)k_viewer_bpp_bits,
                                           (size_t)width_px * sizeof(uint32_t),
                                           cs,
                                           kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
  CGImageRef   img = (bmp != NULL) ? CGBitmapContextCreateImage(bmp) : NULL;
  if (bmp != NULL) {
    CGContextRelease(bmp);
  }
  CGColorSpaceRelease(cs);
  free(argb);
  return img;
}

void ra8_viewer_view_present(ra8_viewer_view_t* view,
                             const uint16_t*    rgb565,
                             uint16_t           width_px,
                             uint16_t           height_px)
{
  if ((view == nullptr) || (rgb565 == nullptr) || (width_px == 0U) || (height_px == 0U)) {
    return;
  }
  @autoreleasepool {
    CGImageRef img = ra8_viewer_image_from_rgb565(rgb565, width_px, height_px);
    if (img != NULL) {
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      view->view.layer.contents = (__bridge id)img;
      [CATransaction commit];
      CGImageRelease(img);
    }
  }
}

bool ra8_viewer_view_pump(ra8_viewer_view_t* view)
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

int32_t ra8_viewer_view_poll_nav(ra8_viewer_view_t* view)
{
  if ((view == nullptr) || (view->view == nil)) {
    return 0;
  }
  const int32_t n     = view->view.navAccum;
  view->view.navAccum = 0;
  return n;
}

int32_t ra8_viewer_view_poll_scroll(ra8_viewer_view_t* view)
{
  if ((view == nullptr) || (view->view == nil)) {
    return 0;
  }
  const int32_t n        = view->view.scrollAccum;
  view->view.scrollAccum = 0;
  return n;
}

bool ra8_viewer_view_save_png(ra8_viewer_view_t* view, const char* path)
{
  if ((view == nullptr) || (view->view == nil) || (path == nullptr)) {
    return false;
  }
  @autoreleasepool {
    CGImageRef img = (__bridge CGImageRef)view->view.layer.contents;
    if (img == NULL) {
      return false;
    }
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    NSData*           png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if (png == nil) {
      return false;
    }
    NSString* nspath = [NSString stringWithUTF8String:path];
    return [png writeToFile:nspath atomically:YES] == YES;
  }
}

void ra8_viewer_view_close(ra8_viewer_view_t* view)
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
