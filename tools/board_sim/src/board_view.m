/**
 * @file board_view.m
 * @brief Cocoa implementation of the board_sim desktop window (see board_view.h)
 *
 * @details
 * A layer-backed NSView whose CALayer ``contents`` is set directly to a CGImage
 * built from the supplied RGB565 framebuffer. Driving the layer contents (rather
 * than drawing in ``drawRect:``) lets the window server composite the frame
 * independently of the run loop, so the picture stays put during the mouse-event
 * flood of a cursor drag -- the ``drawRect:`` + backing-store path otherwise
 * re-clears the view on those events and reads as flicker. The redraw policy is
 * pinned to Never so AppKit never discards the contents to call back into the
 * view. The app runs cooperatively: board_view_pump drains pending events from
 * the caller's loop rather than taking over with [NSApp run], so the emulator
 * keeps stepping between frames; a minimal main menu gives a working Cmd+Q.
 * Built with -fobjc-arc; CGImageRef (a CF type, not ARC-managed) is released by
 * hand after the layer takes its own retain.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "board_view.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <stdint.h>
#include <stdlib.h>

#include "board_input.h"

/**
 * @brief Layer-backed content view that shows the latest frame and records clicks.
 */
@interface BoardImageView : NSView
/** @brief Framebuffer width in pixels of the currently shown image. */
@property(nonatomic, assign) uint16_t fbWidth;
/** @brief Framebuffer height in pixels of the currently shown image. */
@property(nonatomic, assign) uint16_t fbHeight;
/** @brief Last unconsumed click column, framebuffer pixels (top-left origin). */
@property(nonatomic, assign) uint16_t clickX;
/** @brief Last unconsumed click row, framebuffer pixels (top-left origin). */
@property(nonatomic, assign) uint16_t clickY;
/** @brief Set on mouse-down; cleared when polled. */
@property(nonatomic, assign) BOOL hasClick;
/** @brief Last unconsumed drag column, framebuffer pixels (top-left origin). */
@property(nonatomic, assign) uint16_t dragX;
/** @brief Last unconsumed drag row, framebuffer pixels (top-left origin). */
@property(nonatomic, assign) uint16_t dragY;
/** @brief Set while the primary button is held + moved; cleared when polled. */
@property(nonatomic, assign) BOOL hasDrag;
/** @brief Set by mouseUp:; one-shot edge consumed by board_view_poll_release. */
@property(nonatomic, assign) BOOL hasRelease;
/** @brief Accumulated scroll-wheel notches since the last poll (+up / -down). */
@property(nonatomic, assign) int32_t scrollAccum;
@end

@implementation BoardImageView
/* Fully covered by the frame image (black layer background before the first
 * frame), so report opacity -- AppKit skips erasing the window background. */
- (BOOL)isOpaque
{
  return YES;
}
/* Must be the first responder for keyDown: to be delivered (NSView is NO by
 * default); board_view_open also makes this view the window's first responder. */
- (BOOL)acceptsFirstResponder
{
  return YES;
}
/* A typed key is just another keystroke source: push each accepted character
 * into the shared board_input FIFO, exactly as the --keys injector does, so the
 * run loop drains both through one path into the console UART RX. */
- (void)keyDown:(NSEvent*)event
{
  NSString* s = event.characters;
  for (NSUInteger i = 0U; i < s.length; i++) {
    unichar u = [s characterAtIndex:i];
    if (u == 0x7FU) {
      u = 0x08U; /* map the Delete key to ASCII backspace */
    }
    const BOOL printable = (u >= 0x20U) && (u < 0x7FU);
    const BOOL control =
      (u == (unichar)'\r') || (u == (unichar)'\n') || (u == (unichar)'\t') || (u == 0x08U);
    if (printable || control) {
      board_input_push_key((char)u);
    }
  }
}
/* Mouse-wheel / two-finger scroll drives the console scrollback: accumulate
 * notches (positive = scroll up into older history) for the run loop to drain
 * via board_view_poll_scroll. This is independent of keyDown (which feeds the
 * firmware's UART RX), so scrolling never collides with typing into the app. */
- (void)scrollWheel:(NSEvent*)event
{
  const CGFloat dy = event.scrollingDeltaY;
  if (dy > 0.5) {
    self.scrollAccum += 1; /* content moves down -> reveal older lines */
  } else if (dy < -0.5) {
    self.scrollAccum -= 1;
  }
}
/* Window coords -> framebuffer pixels (top-left origin). The view is not
 * flipped, so invert Y; the image fills self.bounds, so scale view points to
 * framebuffer pixels in case the window was resized away from 1:1. Returns NO
 * (no pixel) until a frame has set the framebuffer size. Shared by mouseDown:
 * (a press) and mouseDragged: (the button held + moved) so both map alike. */
- (BOOL)fbPointFromEvent:(NSEvent*)event x:(uint16_t*)ox y:(uint16_t*)oy
{
  const NSPoint p  = [self convertPoint:event.locationInWindow fromView:nil];
  const CGFloat vw = self.bounds.size.width;
  const CGFloat vh = self.bounds.size.height;
  if ((vw <= 0.0) || (vh <= 0.0) || (self.fbWidth == 0U) || (self.fbHeight == 0U)) {
    return NO;
  }
  CGFloat fx = (p.x / vw) * (CGFloat)self.fbWidth;
  CGFloat fy = ((vh - p.y) / vh) * (CGFloat)self.fbHeight;
  if (fx < 0.0) {
    fx = 0.0;
  }
  if (fy < 0.0) {
    fy = 0.0;
  }
  if (fx > (CGFloat)(self.fbWidth - 1U)) {
    fx = (CGFloat)(self.fbWidth - 1U);
  }
  if (fy > (CGFloat)(self.fbHeight - 1U)) {
    fy = (CGFloat)(self.fbHeight - 1U);
  }
  *ox = (uint16_t)fx;
  *oy = (uint16_t)fy;
  return YES;
}
- (void)mouseDown:(NSEvent*)event
{
  uint16_t fx = 0U;
  uint16_t fy = 0U;
  if ([self fbPointFromEvent:event x:&fx y:&fy]) {
    self.clickX   = fx;
    self.clickY   = fy;
    self.hasClick = YES;
  }
}
/* A held-button drag: publish the current position so the run loop can follow it
 * (the battery slider tracks the cursor). Independent of hasClick so the press is
 * still routed once on mouseDown; subsequent motion only updates the drag fields. */
- (void)mouseDragged:(NSEvent*)event
{
  uint16_t fx = 0U;
  uint16_t fy = 0U;
  if ([self fbPointFromEvent:event x:&fx y:&fy]) {
    self.dragX   = fx;
    self.dragY   = fy;
    self.hasDrag = YES;
  }
}
/* Primary button released: latch a one-shot edge (no coordinate -- a release is
 * just the end of the press). The run loop reads it via board_view_poll_release
 * to release a held momentary push-button (SW1/SW2) and drop a slider grab. */
- (void)mouseUp:(NSEvent*)event
{
  (void)event;
  self.hasRelease = YES;
}
@end

struct board_view {
  NSWindow*       window;
  BoardImageView* view;
};

/** @brief Install a minimal main menu so Cmd+Q (Quit) works under the manual pump. */
static void board_view_install_menu(void)
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

board_view_t* board_view_open(uint16_t width_px, uint16_t height_px, const char* title)
{
  if ((width_px == 0U) || (height_px == 0U)) {
    return nullptr;
  }
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    board_view_install_menu();

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
    BoardImageView* v = [[BoardImageView alloc] initWithFrame:rect];
    /* Layer-backed, contents-driven: the window server composites the frame, so
     * a cursor drag's event flood cannot make it flicker. Never redraw means
     * AppKit leaves the contents we set alone; nearest filter keeps pixels crisp
     * if the window is not 1:1; black background covers the pre-first-frame gap. */
    v.wantsLayer                = YES;
    v.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    v.layer.magnificationFilter = kCAFilterNearest;
    v.layer.minificationFilter  = kCAFilterNearest;
    v.layer.opaque              = YES;
    v.layer.backgroundColor     = CGColorGetConstantColor(kCGColorBlack);
    [win setContentView:v];
    [win center];
    [win makeKeyAndOrderFront:nil];
    [win makeFirstResponder:v]; /* route keyDown: to the content view (UART typing) */
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

void board_view_present(board_view_t*   view,
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
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef    bmp =
      CGBitmapContextCreate(argb,
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
      view->view.fbWidth  = width_px;
      view->view.fbHeight = height_px;
      /* Drive the layer contents directly. Disable implicit actions so the
       * contents swap is immediate (no default cross-fade) and commit now so the
       * new frame shows without waiting on the run loop. The layer retains the
       * image, so our reference can be released straight after. */
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      view->view.layer.contents = (__bridge id)img;
      [CATransaction commit];
      CGImageRelease(img);
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

bool board_view_poll_click(board_view_t* view, uint16_t* x, uint16_t* y)
{
  if ((view == nullptr) || (view->view == nil) || (x == nullptr) || (y == nullptr)) {
    return false;
  }
  if (!view->view.hasClick) {
    return false;
  }
  *x                  = view->view.clickX;
  *y                  = view->view.clickY;
  view->view.hasClick = NO;
  return true;
}

bool board_view_poll_drag(board_view_t* view, uint16_t* x, uint16_t* y)
{
  if ((view == nullptr) || (view->view == nil) || (x == nullptr) || (y == nullptr)) {
    return false;
  }
  if (!view->view.hasDrag) {
    return false;
  }
  *x                 = view->view.dragX;
  *y                 = view->view.dragY;
  view->view.hasDrag = NO;
  return true;
}

bool board_view_poll_release(board_view_t* view)
{
  if ((view == nullptr) || (view->view == nil)) {
    return false;
  }
  if (!view->view.hasRelease) {
    return false;
  }
  view->view.hasRelease = NO;
  return true;
}

int32_t board_view_poll_scroll(board_view_t* view)
{
  if ((view == nullptr) || (view->view == nil)) {
    return 0;
  }
  const int32_t n        = view->view.scrollAccum;
  view->view.scrollAccum = 0;
  return n;
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
