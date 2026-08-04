/**
 * @file ra8_viewer_view.m
 * @brief Cocoa continuous-scroll reader window (see ra8_viewer_view.h).
 *
 * @details
 * A native `NSScrollView` whose document view (::ReaderDocumentView) reads the
 * document as one vertical strip of tiles pulled from the reader. Each tile is
 * scaled to the current window width and stacked, so the whole series scrolls
 * smoothly and endlessly (trackpad / wheel / scrollbar / Page keys) and re-fits
 * to width on resize. Tiles are rendered lazily as they approach the viewport and
 * released when they scroll far away, bounding memory on a long chapter. A tile
 * that fails to render (e.g. a page taller than the whole-image decoder's cap)
 * draws as a labelled placeholder rather than sinking the document.
 *
 * The app runs cooperatively: ::ra8_viewer_view_pump drains pending events from
 * the caller's loop rather than taking over with `[NSApp run]`. Built with
 * -fobjc-arc; CGImageRef (a CF type, not ARC-managed) is released by hand.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "ra8_viewer_view.h"

#import <Cocoa/Cocoa.h>
#include <stdint.h>
#include <stdlib.h>

#include "ra8_err.h"
#include "ra8_viewer_reader.h"

/**
 * @enum ra8_viewer_rgb_t
 * @brief RGB565 unpack constants for the tile -> ARGB expansion.
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
 * @enum ra8_viewer_layout_t
 * @brief Reader layout knobs (no magic numbers).
 */
typedef enum : uint32_t {
  k_viewer_prefetch = 2U,   /**< Tiles to render ahead/behind the viewport. */
  k_viewer_keep     = 4U,   /**< Tiles kept resident either side of view.   */
  k_viewer_min_win  = 240U, /**< Minimum window content edge, px.           */
  k_viewer_dflt_w   = 760U, /**< Default reading column width, px.          */
} ra8_viewer_layout_t;

/**
 * @brief Build an ARGB CGImage from an RGB565 tile (caller releases it).
 * @param[in] rgb565 Row-major RGB565 pixels.
 * @param[in] w      Tile width.
 * @param[in] h      Tile height.
 * @return A retained CGImageRef, or NULL on allocation failure.
 */
static CGImageRef ra8_viewer_cgimage_from_565(const uint16_t* rgb565, uint32_t w, uint32_t h)
{
  const size_t n    = (size_t)w * (size_t)h;
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
                                           (size_t)w,
                                           (size_t)h,
                                           (size_t)k_viewer_bpp_bits,
                                           (size_t)w * sizeof(uint32_t),
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

/**
 * @brief Flipped document view: one vertical strip of fit-to-width tiles.
 */
@interface ReaderDocumentView : NSView {
  ra8_viewer_reader_t*                      _reader;  /**< Borrowed tile source.                  */
  uint32_t                                  _count;   /**< Tile count.                            */
  uint32_t*                                 _tw;      /**< Native tile widths (_count).           */
  uint32_t*                                 _th;      /**< Native tile heights (_count).          */
  CGFloat*                                  _yTop;    /**< Laid-out top y per tile (_count).      */
  CGFloat*                                  _onH;     /**< Laid-out on-screen height per tile.    */
  CGFloat                                   _layoutW; /**< Width the current layout was made for. */
  CGFloat                                   _totalH;  /**< Laid-out document height.              */
  NSMutableDictionary<NSNumber*, NSImage*>* _cache;   /**< tile -> rendered image.                */
  NSMutableIndexSet*                        _failed;  /**< tiles that won't render.               */
}
@end

@implementation ReaderDocumentView

- (instancetype)initWithReader:(ra8_viewer_reader_t*)reader
{
  self = [super initWithFrame:NSMakeRect(0, 0, (CGFloat)k_viewer_dflt_w, (CGFloat)k_viewer_dflt_w)];
  if (self == nil) {
    return nil;
  }
  _reader = reader;
  _count  = ra8_viewer_tile_count(reader);
  _cache  = [NSMutableDictionary dictionary];
  _failed = [NSMutableIndexSet indexSet];
  if (_count > 0U) {
    _tw   = (uint32_t*)calloc(_count, sizeof(uint32_t));
    _th   = (uint32_t*)calloc(_count, sizeof(uint32_t));
    _yTop = (CGFloat*)calloc(_count, sizeof(CGFloat));
    _onH  = (CGFloat*)calloc(_count, sizeof(CGFloat));
    for (uint32_t i = 0U; i < _count; i++) {
      uint32_t w = 0U;
      uint32_t h = 0U;
      if (ra8_viewer_tile_size(reader, i, &w, &h) == k_ra8_ok) {
        _tw[i] = w;
        _th[i] = h;
      }
    }
  }
  return self;
}

- (void)dealloc
{
  free(_tw);
  free(_th);
  free(_yTop);
  free(_onH);
}

/* Top-left origin, y downward: tile 0 at the top, natural reading order. */
- (BOOL)isFlipped
{
  return YES;
}
- (BOOL)acceptsFirstResponder
{
  return YES;
}

/**
 * @brief Recompute per-tile heights/offsets for a content width and resize self.
 * @param[in] width New content width (== clip-view width).
 */
- (void)layoutForWidth:(CGFloat)width
{
  if (width < 1.0) {
    width = 1.0;
  }
  _layoutW  = width;
  CGFloat y = 0.0;
  for (uint32_t i = 0U; i < _count; i++) {
    /* A tile that never probed (0x0) still gets a slot so the reader can show a
     * placeholder in its place; give it a square-ish stand-in height. */
    const CGFloat tw = (_tw[i] > 0U) ? (CGFloat)_tw[i] : width;
    const CGFloat th = (_th[i] > 0U) ? (CGFloat)_th[i] : width;
    const CGFloat oh = (th * width) / tw;
    _yTop[i]         = y;
    _onH[i]          = oh;
    y += oh;
  }
  _totalH = (_count > 0U) ? y : 1.0;
  [self setFrameSize:NSMakeSize(width, _totalH)];
  [self setNeedsDisplay:YES];
}

/** @brief Index of the tile whose band contains document-y @p yy (clamped). */
- (uint32_t)tileAtY:(CGFloat)yy
{
  if ((_count == 0U) || (yy <= 0.0)) {
    return 0U;
  }
  for (uint32_t i = 0U; i < _count; i++) {
    if (yy < (_yTop[i] + _onH[i])) {
      return i;
    }
  }
  return _count - 1U;
}

/** @brief Render + cache tile @p i, or mark it failed. Returns nil on failure. */
- (NSImage*)imageForTile:(uint32_t)i
{
  NSNumber* key = @(i);
  NSImage*  im  = _cache[key];
  if (im != nil) {
    return im;
  }
  if ([_failed containsIndex:(NSUInteger)i]) {
    return nil;
  }
  uint32_t  w   = 0U;
  uint32_t  h   = 0U;
  uint16_t* buf = nullptr;
  if (ra8_viewer_render_tile565(_reader, i, &w, &h, &buf) != k_ra8_ok || buf == nullptr) {
    [_failed addIndex:(NSUInteger)i];
    return nil;
  }
  CGImageRef cg = ra8_viewer_cgimage_from_565(buf, w, h);
  free(buf);
  if (cg == NULL) {
    [_failed addIndex:(NSUInteger)i];
    return nil;
  }
  im = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize((CGFloat)w, (CGFloat)h)];
  CGImageRelease(cg);
  _cache[key] = im;
  return im;
}

/** @brief Drop cached images for tiles outside [lo, hi] to bound memory. */
- (void)evictOutsideLo:(uint32_t)lo hi:(uint32_t)hi
{
  NSMutableArray<NSNumber*>* drop = [NSMutableArray array];
  for (NSNumber* k in _cache) {
    const uint32_t i = (uint32_t)k.unsignedIntValue;
    if ((i < lo) || (i > hi)) {
      [drop addObject:k];
    }
  }
  [_cache removeObjectsForKeys:drop];
}

- (void)drawRect:(NSRect)dirty
{
  [[NSColor colorWithWhite:0.11 alpha:1.0] set];
  NSRectFill(dirty);
  if (_count == 0U) {
    return;
  }
  /* Re-fit if the width drifted from the last layout (belt-and-suspenders with
   * the clip-resize notification). */
  if (fabs(self.bounds.size.width - _layoutW) > 0.5) {
    [self layoutForWidth:self.bounds.size.width];
  }

  const NSRect   vis = self.visibleRect;
  const uint32_t vlo = [self tileAtY:NSMinY(vis)];
  const uint32_t vhi = [self tileAtY:NSMaxY(vis)];
  const uint32_t plo =
    (vlo > (uint32_t)k_viewer_prefetch) ? (vlo - (uint32_t)k_viewer_prefetch) : 0U;
  uint32_t phi = vhi + (uint32_t)k_viewer_prefetch;
  if (phi >= _count) {
    phi = _count - 1U;
  }
  const uint32_t klo = (vlo > (uint32_t)k_viewer_keep) ? (vlo - (uint32_t)k_viewer_keep) : 0U;
  uint32_t       khi = vhi + (uint32_t)k_viewer_keep;
  if (khi >= _count) {
    khi = _count - 1U;
  }
  [self evictOutsideLo:klo hi:khi];

  const CGFloat w = self.bounds.size.width;
  for (uint32_t i = plo; i <= phi; i++) {
    const NSRect r = NSMakeRect(0.0, _yTop[i], w, _onH[i]);
    if (!NSIntersectsRect(r, dirty)) {
      continue;
    }
    NSImage* im = [self imageForTile:i];
    if (im != nil) {
      /* Draw the tile with an explicit local y-flip: the tile buffer is stored
       * top row first, but this NSView is flipped, so a plain draw would render
       * the page upside-down. Undo the view flip for the image only. */
      CGImageRef   cg  = [im CGImageForProposedRect:NULL context:nil hints:nil];
      CGContextRef ctx = [NSGraphicsContext currentContext].CGContext;
      if ((cg != NULL) && (ctx != NULL)) {
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, NSMinX(r), NSMinY(r) + NSHeight(r));
        CGContextScaleCTM(ctx, 1.0, -1.0);
        CGContextDrawImage(ctx, CGRectMake(0.0, 0.0, NSWidth(r), NSHeight(r)), cg);
        CGContextRestoreGState(ctx);
      }
    } else {
      /* Placeholder for an unrenderable page (e.g. a strip too tall for the
       * whole-image decoder -- re-download that title as JOF). */
      [[NSColor colorWithWhite:0.2 alpha:1.0] set];
      NSRectFill(r);
      [[NSColor colorWithWhite:0.4 alpha:1.0] set];
      NSFrameRect(r);
    }
  }
}

/** @brief Scroll the enclosing clip view by @p dy points (clamped to content). */
- (void)scrollByDy:(CGFloat)dy
{
  const NSRect  vis = self.visibleRect;
  CGFloat       y   = NSMinY(vis) + dy;
  const CGFloat max = _totalH - vis.size.height;
  if (y < 0.0) {
    y = 0.0;
  }
  if (y > max) {
    y = (max > 0.0) ? max : 0.0;
  }
  [self scrollPoint:NSMakePoint(0.0, y)];
}

- (void)keyDown:(NSEvent*)event
{
  const CGFloat page = self.visibleRect.size.height * 0.9;
  NSString*     s    = event.characters;
  const unichar u    = (s.length > 0U) ? [s characterAtIndex:0] : 0;
  if ((u == (unichar)NSPageDownFunctionKey) || (u == ' ') ||
      (u == (unichar)NSDownArrowFunctionKey)) {
    [self scrollByDy:page];
  } else if ((u == (unichar)NSPageUpFunctionKey) || (u == (unichar)NSUpArrowFunctionKey)) {
    [self scrollByDy:-page];
  } else if (u == (unichar)NSHomeFunctionKey) {
    [self scrollPoint:NSMakePoint(0.0, 0.0)];
  } else if (u == (unichar)NSEndFunctionKey) {
    [self scrollByDy:_totalH];
  } else {
    [super keyDown:event];
  }
}

/** @brief Clip-view resized: re-fit tiles to the new width. */
- (void)clipResized:(NSNotification*)note
{
  (void)note;
  const NSView* clip = self.superview;
  if (clip != nil) {
    [self layoutForWidth:clip.bounds.size.width];
  }
}
@end

struct ra8_viewer_view {
  NSWindow*           window;
  NSScrollView*       scroll;
  ReaderDocumentView* doc;
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

ra8_viewer_view_t* ra8_viewer_view_open(ra8_viewer_reader_t* reader, const char* title)
{
  if (reader == nullptr) {
    return nullptr;
  }
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    ra8_viewer_install_menu();

    /* A comfortable reading column, capped to the screen; user resizes freely. */
    const NSRect vis = [[NSScreen mainScreen] visibleFrame];
    CGFloat      w   = (CGFloat)k_viewer_dflt_w;
    CGFloat      h   = vis.size.height * 0.88;
    if (w > (vis.size.width * 0.9)) {
      w = vis.size.width * 0.9;
    }
    if (h > 1040.0) {
      h = 1040.0;
    }
    const NSRect     rect  = NSMakeRect(0.0, 0.0, w, h);
    const NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow*        win   = [[NSWindow alloc] initWithContentRect:rect
                                                         styleMask:style
                                                           backing:NSBackingStoreBuffered
                                                             defer:NO];
    if (win == nil) {
      return nullptr;
    }
    [win setReleasedWhenClosed:NO];
    [win setContentMinSize:NSMakeSize((CGFloat)k_viewer_min_win, (CGFloat)k_viewer_min_win)];
    if (title != nullptr) {
      [win setTitle:[NSString stringWithUTF8String:title]];
    }

    NSScrollView* scroll         = [[NSScrollView alloc] initWithFrame:rect];
    scroll.hasVerticalScroller   = YES;
    scroll.hasHorizontalScroller = NO;
    scroll.borderType            = NSNoBorder;
    scroll.drawsBackground       = YES;
    scroll.backgroundColor       = [NSColor colorWithWhite:0.11 alpha:1.0];
    scroll.autohidesScrollers    = YES;

    ReaderDocumentView* doc = [[ReaderDocumentView alloc] initWithReader:reader];
    scroll.documentView     = doc;
    [doc layoutForWidth:scroll.contentSize.width];

    /* Re-fit tiles to width whenever the clip view (window) resizes. */
    scroll.contentView.postsFrameChangedNotifications = YES;
    [[NSNotificationCenter defaultCenter] addObserver:doc
                                             selector:@selector(clipResized:)
                                                 name:NSViewFrameDidChangeNotification
                                               object:scroll.contentView];

    [win setContentView:scroll];
    [win center];
    [win makeKeyAndOrderFront:nil];
    [win makeFirstResponder:doc];
    [doc scrollPoint:NSMakePoint(0.0, 0.0)];
    [NSApp activateIgnoringOtherApps:YES];

    ra8_viewer_view_t* vv = (ra8_viewer_view_t*)calloc(1U, sizeof(*vv));
    if (vv == nullptr) {
      return nullptr;
    }
    vv->window = win;
    vv->scroll = scroll;
    vv->doc    = doc;
    return vv;
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

void ra8_viewer_view_close(ra8_viewer_view_t* view)
{
  if (view == nullptr) {
    return;
  }
  @autoreleasepool {
    if (view->doc != nil) {
      [[NSNotificationCenter defaultCenter] removeObserver:view->doc];
    }
    [view->window close];
    view->window = nil;
    view->scroll = nil;
    view->doc    = nil;
  }
  free(view);
}
