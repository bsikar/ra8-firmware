/**
 * @file board_view.m
 * @brief Cocoa implementation of the ra8_emulator desktop window (see board_view.h)
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

#include "board_input.h"
#include "board_view_provider_internal.h"

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
  NSWindow*       window; /**< Framework-owned window.     */
  BoardImageView* view;   /**< Framework-owned image view. */
  uint16_t        width;  /**< Current surface width.      */
  uint16_t        height; /**< Current surface height.     */
};

static_assert(sizeof(board_view_t) <= sizeof(board_view_storage_t),
              "board view storage is too small");
static_assert(alignof(board_view_t) <= alignof(board_view_storage_t),
              "board view storage alignment is insufficient");

/**
 * @brief Install a minimal main menu so Cmd+Q (Quit) works under the manual pump.
 * @details Install a minimal main menu so cmd+q (quit) works under the manual pump; this step is contained within the board view model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for board view install menu. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board view model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
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

board_view_t* board_view_open(board_view_storage_t* storage,
                              uint16_t              width_px,
                              uint16_t              height_px,
                              const char*           title)
{
  if ((storage == nullptr) || (width_px == 0U) || (height_px == 0U)) {
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
    if (v == nil) {
      [win close];
      return nullptr;
    }
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

    board_view_t* bv = (board_view_t*)(void*)storage;
    bv->window       = win;
    bv->view         = v;
    bv->width        = width_px;
    bv->height       = height_px;
    return bv;
  }
}

/**
 * @brief Perform board view present for the board view model.
 * @details Perform board view present for the board view model; this step is contained within the board view model and uses bounded caller or module-owned storage.
 * @param[in,out] view Authoritative memory or presentation view accessed by the operation.
 * @param[in,out] presentation Descriptor-backed presentation workspace accessed by the operation.
 * @pre Arguments satisfy the ranges documented for board view present. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board view model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
void board_view_present(board_view_t* view, emu_presentation_workspace_t* presentation)
{
  if ((view == nullptr) || (presentation == nullptr) ||
      (presentation->composite_width != view->width) ||
      (presentation->composite_height != view->height)) {
    return;
  }
  @autoreleasepool {
    int snapshot_fd = -1;
    if (!emu_presentation_snapshot(presentation, &snapshot_fd)) {
      return;
    }
    const size_t provider_bytes =
      (presentation->surface_bytes / sizeof(uint16_t)) * sizeof(uint32_t);
    CGDataProviderRef provider = priv_board_view_provider_create(snapshot_fd, provider_bytes);
    if (provider == NULL) {
      return;
    }
    CGColorSpaceRef cs  = CGColorSpaceCreateDeviceRGB();
    CGImageRef      img = NULL;
    if (cs != NULL) {
      img = CGImageCreate((size_t)view->width,
                          (size_t)view->height,
                          8U,
                          32U,
                          (size_t)view->width * sizeof(uint32_t),
                          cs,
                          kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                          provider,
                          NULL,
                          false,
                          kCGRenderingIntentDefault);
      CGColorSpaceRelease(cs);
    }
    CGDataProviderRelease(provider);
    if (img != NULL) {
      view->view.fbWidth  = view->width;
      view->view.fbHeight = view->height;
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

/**
 * @brief Perform board view pump for the board view model.
 * @details Perform board view pump for the board view model; this step is contained within the board view model and uses bounded caller or module-owned storage.
 * @param[in,out] view Authoritative memory or presentation view accessed by the operation.
 * @return The board view pump result produced by the board view model.
 * @retval true The board view pump condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for board view pump. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board view model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
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

/**
 * @brief Perform board view poll click for the board view model.
 * @details Perform board view poll click for the board view model; this step is contained within the board view model and uses bounded caller or module-owned storage.
 * @param[in,out] view Authoritative memory or presentation view accessed by the operation.
 * @param[in,out] x Horizontal coordinate in pixels.
 * @param[in,out] y Vertical coordinate in pixels.
 * @return The board view poll click result produced by the board view model.
 * @retval true The board view poll click condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for board view poll click. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board view model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
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

/**
 * @brief Perform board view poll drag for the board view model.
 * @details Perform board view poll drag for the board view model; this step is contained within the board view model and uses bounded caller or module-owned storage.
 * @param[in,out] view Authoritative memory or presentation view accessed by the operation.
 * @param[in,out] x Horizontal coordinate in pixels.
 * @param[in,out] y Vertical coordinate in pixels.
 * @return The board view poll drag result produced by the board view model.
 * @retval true The board view poll drag condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for board view poll drag. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board view model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
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

/**
 * @brief Perform board view poll release for the board view model.
 * @details Perform board view poll release for the board view model; this step is contained within the board view model and uses bounded caller or module-owned storage.
 * @param[in,out] view Authoritative memory or presentation view accessed by the operation.
 * @return The board view poll release result produced by the board view model.
 * @retval true The board view poll release condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for board view poll release. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board view model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
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

/**
 * @brief Perform board view poll scroll for the board view model.
 * @details Perform board view poll scroll for the board view model; this step is contained within the board view model and uses bounded caller or module-owned storage.
 * @param[in,out] view Authoritative memory or presentation view accessed by the operation.
 * @return The board view poll scroll result produced by the board view model.
 * @retval value The operation-specific board view poll scroll value.
 * @pre Arguments satisfy the ranges documented for board view poll scroll. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board view model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
int32_t board_view_poll_scroll(board_view_t* view)
{
  if ((view == nullptr) || (view->view == nil)) {
    return 0;
  }
  const int32_t n        = view->view.scrollAccum;
  view->view.scrollAccum = 0;
  return n;
}

/**
 * @brief Perform board view close for the board view model.
 * @details Perform board view close for the board view model; this step is contained within the board view model and uses bounded caller or module-owned storage.
 * @param[in,out] view Authoritative memory or presentation view accessed by the operation.
 * @pre Arguments satisfy the ranges documented for board view close. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board view model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
void board_view_close(board_view_t* view)
{
  if (view == nullptr) {
    return;
  }
  @autoreleasepool {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    view->view.layer.contents = nil;
    [CATransaction commit];
    [CATransaction flush];
    [view->window close];
    view->window = nil;
    view->view   = nil;
    view->width  = 0U;
    view->height = 0U;
  }
}
