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
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
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
  k_viewer_prefetch = 2U,   /**< Tile prefetch radius. */
  k_viewer_keep     = 4U,   /**< Tile cache radius.    */
  k_viewer_min_win  = 240U, /**< Minimum window edge.  */
  k_viewer_dflt_w   = 760U, /**< Default column width. */
  /** @brief Workspace ABI guard. */
  k_viewer_view_layout_version = 0x56560001U,
} ra8_viewer_layout_t;

/**
 * @brief Build an ARGB CGImage from an RGB565 tile (caller releases it).
 * @details Expands backward in the same caller buffer, then asks CoreGraphics
 *          to copy the bytes into an explicitly platform-owned data object.
 * @param[in] rgb565 Row-major RGB565 pixels.
 * @param[in] scratch_bytes Writable extent beginning at @p rgb565.
 * @param[in] w      Tile width.
 * @param[in] h      Tile height.
 * @return A retained CGImageRef, or NULL on allocation failure.
 * @retval NULL Scratch was short or the platform object could not be created.
 * @pre @p rgb565 spans at least `w*h*4` writable bytes.
 * @pre @p w and @p h came from the open reader.
 * @post Success returns one caller-released platform image.
 * @post First-party code owns no allocation from this operation.
 * @note CoreFoundation/CoreGraphics object storage is explicit platform SOUP.
 * @since 0.1.0
 */
RA8_INTERNAL static CGImageRef
internal_cgimage_from_565(uint16_t* rgb565, size_t scratch_bytes, uint32_t w, uint32_t h)
{
  const size_t n = (size_t)w * (size_t)h;
  if (scratch_bytes < (n * sizeof(uint32_t))) {
    return NULL;
  }
  uint32_t* argb = (uint32_t*)rgb565;
  for (size_t remaining = n; remaining > 0U;) {
    --remaining;
    const size_t   i  = remaining;
    const uint16_t p  = rgb565[i];
    const uint32_t r5 = (uint32_t)((p >> k_viewer_r_shift) & k_viewer_r_mask);
    const uint32_t g6 = (uint32_t)((p >> k_viewer_g_shift) & k_viewer_g_mask);
    const uint32_t b5 = (uint32_t)(p & k_viewer_b_mask);
    const uint32_t r  = (r5 << 3) | (r5 >> 2);
    const uint32_t g  = (g6 << 2) | (g6 >> 4);
    const uint32_t b  = (b5 << 3) | (b5 >> 2);
    argb[i]           = (r << k_viewer_argb_r) | (g << k_viewer_argb_g) | b;
  }
  CFDataRef data =
    CFDataCreate(kCFAllocatorDefault, (const UInt8*)argb, (CFIndex)(n * sizeof(uint32_t)));
  CGDataProviderRef provider = (data != NULL) ? CGDataProviderCreateWithCFData(data) : NULL;
  CGColorSpaceRef   cs       = CGColorSpaceCreateDeviceRGB();
  CGImageRef        img = ((provider != NULL) && (cs != NULL))
                            ? CGImageCreate((size_t)w,
                                            (size_t)h,
                                            (size_t)k_viewer_bpp_bits,
                                            sizeof(uint32_t) * (size_t)k_viewer_bpp_bits,
                                            (size_t)w * sizeof(uint32_t),
                                            cs,
                                            kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                                            provider,
                                            NULL,
                                            false,
                                            kCGRenderingIntentDefault)
                            : NULL;
  if (cs != NULL) {
    CGColorSpaceRelease(cs);
  }
  if (provider != NULL) {
    CGDataProviderRelease(provider);
  }
  if (data != NULL) {
    CFRelease(data);
  }
  return img;
}

/**
 * @brief Flipped document view: one vertical strip of fit-to-width tiles.
 */
@interface ReaderDocumentView : NSView {
  ra8_viewer_reader_t*                      _reader;       /**< Tile source.     */
  uint32_t                                  _count;        /**< Tile count.      */
  CGFloat*                                  _yTop;         /**< Tile top edges.  */
  CGFloat*                                  _onH;          /**< Display heights. */
  uint8_t*                                  _scratch;      /**< Pixel scratch.   */
  size_t                                    _scratchBytes; /**< Scratch extent.  */
  CGFloat                                   _layoutW;      /**< Layout width.    */
  CGFloat                                   _totalH;       /**< Document height. */
  NSMutableDictionary<NSNumber*, NSImage*>* _cache;        /**< Image cache.     */
  NSMutableIndexSet*                        _failed;       /**< Failed tiles.    */
}
@end

@implementation ReaderDocumentView

- (instancetype)initWithReader:(ra8_viewer_reader_t*)reader
                          yTop:(CGFloat*)yTop
                           onH:(CGFloat*)onH
                       scratch:(uint8_t*)scratch
                  scratchBytes:(size_t)scratchBytes
{
  self = [super initWithFrame:NSMakeRect(0, 0, (CGFloat)k_viewer_dflt_w, (CGFloat)k_viewer_dflt_w)];
  if (self == nil) {
    return nil;
  }
  _reader       = reader;
  _count        = ra8_viewer_tile_count(reader);
  _yTop         = yTop;
  _onH          = onH;
  _scratch      = scratch;
  _scratchBytes = scratchBytes;
  _cache        = [NSMutableDictionary dictionary];
  _failed       = [NSMutableIndexSet indexSet];
  return self;
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
    uint32_t nativeWidth  = 0U;
    uint32_t nativeHeight = 0U;
    (void)ra8_viewer_tile_size(_reader, i, &nativeWidth, &nativeHeight);
    /* A tile that never probed (0x0) still gets a slot so the reader can show a
     * placeholder in its place; give it a square-ish stand-in height. */
    const CGFloat tw = (nativeWidth > 0U) ? (CGFloat)nativeWidth : width;
    const CGFloat th = (nativeHeight > 0U) ? (CGFloat)nativeHeight : width;
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
  uint32_t                      w      = 0U;
  uint32_t                      h      = 0U;
  uint16_t*                     buf    = nullptr;
  ra8_viewer_workspace_report_t report = {};
  if ((ra8_viewer_render_tile565(_reader, i, _scratch, _scratchBytes, &w, &h, &buf, &report) !=
       k_ra8_ok) ||
      (buf == nullptr)) {
    [_failed addIndex:(NSUInteger)i];
    return nil;
  }
  CGImageRef cg = internal_cgimage_from_565(buf, _scratchBytes, w, h);
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
  NSWindow*           window; /**< Platform window.           */
  NSScrollView*       scroll; /**< Platform scroll container. */
  ReaderDocumentView* doc;    /**< Platform document view.    */
};

/** @brief Workspace partition cursor for the Cocoa first-party state. */
typedef struct {
  uint8_t* base;     /**< Backing base, or NULL while sizing.    */
  size_t   capacity; /**< Accessible extent.                     */
  size_t   used;     /**< First unused byte.                     */
  bool     valid;    /**< False after overflow/capacity failure. */
} viewer_view_layout_t;

/**
 * @brief Take one aligned view workspace slice.
 * @details Advances the cursor after checked alignment and extent arithmetic.
 * @param[in,out] layout Mutable view-layout cursor.
 * @param[in] bytes Requested slice extent.
 * @param[in] alignment Required power-of-two alignment.
 * @return Slice base in binding mode, otherwise NULL.
 * @retval non-NULL The requested bound slice is available.
 * @retval NULL Sizing mode is active or the request failed.
 * @pre @p layout is non-NULL.
 * @pre @p layout was initialized by the caller.
 * @post Failure marks the cursor invalid.
 * @post Success advances used to the end of the slice.
 * @note Sizing mode charges space without touching workspace bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static void*
internal_view_take(viewer_view_layout_t* layout, size_t bytes, size_t alignment)
{
  const size_t mask = alignment - 1U;
  if (!layout->valid || (alignment == 0U) || ((alignment & mask) != 0U) ||
      (layout->used > (SIZE_MAX - mask))) {
    layout->valid = false;
    return nullptr;
  }
  const size_t start = (layout->used + mask) & ~mask;
  if ((start > (SIZE_MAX - bytes)) ||
      ((layout->base != nullptr) && ((start + bytes) > layout->capacity))) {
    layout->valid = false;
    return nullptr;
  }
  void* result = (layout->base == nullptr) ? nullptr : &layout->base[start];
  layout->used = start + bytes;
  return result;
}

/**
 * @brief Charge the complete deterministic view layout.
 * @details Walks the C handle, two CGFloat layout arrays, and pixel scratch.
 * @param[in] need View requirements.
 * @param[in,out] base Workspace base, or NULL for sizing.
 * @param[in] capacity Accessible extent when binding.
 * @return Completed layout cursor.
 * @retval viewer_view_layout_t Cursor with valid false on failure.
 * @pre @p need is non-NULL.
 * @pre @p base is suitably aligned when non-NULL.
 * @post Sizing mode mutates no bytes.
 * @post Binding mode calculates addresses without platform acquisition.
 * @note Pure in sizing mode.
 * @since 0.1.0
 */
RA8_INTERNAL static viewer_view_layout_t
internal_view_layout(const ra8_viewer_view_requirements_t* need, uint8_t* base, size_t capacity)
{
  viewer_view_layout_t layout = {.base = base, .capacity = capacity, .used = 0U, .valid = true};
  (void)internal_view_take(&layout, sizeof(ra8_viewer_view_t), alignof(max_align_t));
  (void)internal_view_take(&layout, need->layout_bytes / 2U, alignof(CGFloat));
  (void)internal_view_take(&layout, need->layout_bytes / 2U, alignof(CGFloat));
  (void)internal_view_take(&layout, need->pixel_bytes, alignof(uint32_t));
  return layout;
}

/**
 * @brief Verify that a view requirements object still matches the reader.
 * @details Recomputes every public field so a shortened layout cannot alias
 *          arrays or pixel scratch even when its self-reported total matches.
 * @param[in] reader Open reader used for the original query.
 * @param[in] candidate Requirements object supplied to view open.
 * @return Whether every requirements field is exact.
 * @retval true @p candidate matches a fresh query.
 * @retval false The query failed or any field differs.
 * @pre @p reader is non-NULL.
 * @pre @p candidate is non-NULL.
 * @post No reader or workspace state is mutated.
 * @post The result covers totals, components, alignment, count, and ABI.
 * @note Performs bounded read-only tile requirement queries.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_view_requirements_match(const ra8_viewer_reader_t*            reader,
                                 const ra8_viewer_view_requirements_t* candidate)
{
  ra8_viewer_view_requirements_t actual = {};
  if (ra8_viewer_view_requirements(reader, &actual) != k_ra8_ok) {
    return false;
  }
  return (candidate->required_bytes == actual.required_bytes) &&
         (candidate->required_alignment == actual.required_alignment) &&
         (candidate->pixel_bytes == actual.pixel_bytes) &&
         (candidate->layout_bytes == actual.layout_bytes) &&
         (candidate->tile_count == actual.tile_count) &&
         (candidate->layout_version == actual.layout_version);
}

/**
 * @brief Install a minimal main menu so Cmd+Q works under the manual pump.
 * @details Creates platform-owned menu objects only when no main menu exists.
 * @pre NSApplication has been created.
 * @pre The caller executes on the main thread.
 * @post A pre-existing menu remains unchanged.
 * @post Otherwise a Quit item is installed.
 * @note AppKit object ownership is explicit platform SOUP.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_install_menu(void)
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

/** @copydoc ra8_viewer_view_requirements */
ra8_err_t ra8_viewer_view_requirements(const ra8_viewer_reader_t*      reader,
                                       ra8_viewer_view_requirements_t* out)
{
  if ((reader == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out                 = (ra8_viewer_view_requirements_t){};
  const uint32_t count = ra8_viewer_tile_count(reader);
  if (count == 0U) {
    return k_ra8_err_invalid_state;
  }
  size_t max_rgb565 = 0U;
  for (uint32_t index = 0U; index < count; ++index) {
    size_t          bytes     = 0U;
    size_t          alignment = 0U;
    const ra8_err_t error     = ra8_viewer_tile_requirements(reader, index, &bytes, &alignment);
    if (error != k_ra8_ok) {
      return error;
    }
    if (bytes > max_rgb565) {
      max_rgb565 = bytes;
    }
  }
  if (max_rgb565 > (SIZE_MAX / 2U)) {
    return k_ra8_err_invalid_size;
  }
  out->required_alignment           = alignof(max_align_t);
  out->pixel_bytes                  = max_rgb565 * 2U;
  out->layout_bytes                 = (size_t)count * 2U * sizeof(CGFloat);
  out->tile_count                   = count;
  out->layout_version               = (uint32_t)k_viewer_view_layout_version;
  const viewer_view_layout_t layout = internal_view_layout(out, nullptr, 0U);
  if (!layout.valid) {
    *out = (ra8_viewer_view_requirements_t){};
    return k_ra8_err_invalid_size;
  }
  out->required_bytes = layout.used;
  return k_ra8_ok;
}

/** @copydoc ra8_viewer_view_open */
ra8_err_t ra8_viewer_view_open(ra8_viewer_view_t**                   out,
                               ra8_viewer_reader_t*                  reader,
                               const char*                           title,
                               void*                                 workspace,
                               size_t                                workspace_bytes,
                               const ra8_viewer_view_requirements_t* requirements,
                               ra8_viewer_workspace_report_t*        report)
{
  if ((out == nullptr) || (reader == nullptr) || (requirements == nullptr) || (report == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out    = nullptr;
  *report = (ra8_viewer_workspace_report_t){.required_bytes = requirements->required_bytes,
                                            .supplied_bytes = workspace_bytes};
  const viewer_view_layout_t expected = internal_view_layout(requirements, nullptr, 0U);
  if ((workspace == nullptr) || !internal_view_requirements_match(reader, requirements) ||
      !expected.valid || (expected.used != requirements->required_bytes) ||
      (((uintptr_t)workspace % requirements->required_alignment) != 0U) ||
      (workspace_bytes < requirements->required_bytes)) {
    return k_ra8_err_invalid_size;
  }
  viewer_view_layout_t layout = {.base     = (uint8_t*)workspace,
                                 .capacity = workspace_bytes,
                                 .used     = 0U,
                                 .valid    = true};
  ra8_viewer_view_t*   view =
    (ra8_viewer_view_t*)internal_view_take(&layout, sizeof(*view), alignof(max_align_t));
  CGFloat* yTop =
    (CGFloat*)internal_view_take(&layout, requirements->layout_bytes / 2U, alignof(CGFloat));
  CGFloat* onH =
    (CGFloat*)internal_view_take(&layout, requirements->layout_bytes / 2U, alignof(CGFloat));
  uint8_t* scratch =
    (uint8_t*)internal_view_take(&layout, requirements->pixel_bytes, alignof(uint32_t));
  if (!layout.valid || (layout.used != requirements->required_bytes)) {
    return k_ra8_err_invalid_size;
  }
  *view = (ra8_viewer_view_t){};
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    internal_install_menu();

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
      return k_ra8_fail;
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

    ReaderDocumentView* doc = [[ReaderDocumentView alloc] initWithReader:reader
                                                                    yTop:yTop
                                                                     onH:onH
                                                                 scratch:scratch
                                                            scratchBytes:requirements->pixel_bytes];
    if (doc == nil) {
      [win close];
      return k_ra8_fail;
    }
    scroll.documentView = doc;
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

    view->window = win;
    view->scroll = scroll;
    view->doc    = doc;
    *out         = view;
    return k_ra8_ok;
  }
}

/** @copydoc ra8_viewer_view_pump */
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

/** @copydoc ra8_viewer_view_close */
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
}
