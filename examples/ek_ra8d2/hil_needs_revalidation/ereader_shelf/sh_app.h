/**
 * @file sh_app.h
 * @brief Shared contract for the ereader_shelf multi-screen e-reader.
 *
 * @details
 * The app is split into one module per screen plus a shared image blitter, all
 * driven by `main.c`'s boot + input loop. This header is the contract between
 * them: the screen enumeration, the layout/colour constants, the single global
 * ::sh_state_t, and every module entry point. State lives in one ::g_sh instance
 * (defined in `main.c`) rather than function-threaded context, mirroring the
 * single-screen-at-a-time nature of the UI.
 *
 * Screens:
 *   - ::k_sh_screen_shelf  -- grid of book cards, each with a cover thumbnail.
 *   - ::k_sh_screen_cover  -- one book's full cover + title/author + actions.
 *   - ::k_sh_screen_toc    -- scrollable chapter list; tap a chapter to open it.
 *   - ::k_sh_screen_reader -- full-book pagination across every chapter.
 *   - ::k_sh_screen_comic  -- full-page image reader for a CBZ / CBR comic.
 *
 *
 * [Ring 6 / App] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "library.h"
#include "ra8_book.h"
#include "ra8_book_paged.h"
#include "ra8_ui.h"
#include "ra8_vsource.h"
#include "sh_classify.h"

/**
 * @enum sh_const_t
 * @brief Panel geometry, layout, font, and reader sizing constants.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_sh_fb_w        = 1024U,       /**< Panel width in pixels.                 */
  k_sh_fb_h        = 600U,        /**< Panel height in pixels.                */
  k_sh_fb_align    = 64U,         /**< AXI framebuffer alignment.             */
  k_sh_glyph_w     = 8U,          /**< Bitmap font cell width.                */
  k_sh_glyph_h     = 16U,         /**< Bitmap font cell height.               */
  k_sh_line_h      = 22U,         /**< Reader text line height.               */
  k_sh_bar_h       = 56U,         /**< Header / title-bar height.             */
  k_sh_pad         = 24U,         /**< Outer margin / content inset.          */
  k_sh_gap         = 24U,         /**< Gap between shelf cards.               */
  k_sh_card_pad    = 14U,         /**< Inner card inset.                      */
  k_sh_thumb_w     = 130U,        /**< Shelf cover-thumbnail box width.       */
  k_sh_thumb_h     = 195U,        /**< Shelf cover-thumbnail box height.      */
  k_sh_grid_cols   = 4U,          /**< Shelf grid columns.                    */
  k_sh_toc_row_h   = 40U,         /**< TOC list row height.                   */
  k_sh_linebuf     = 160U,        /**< Per-line draw buffer bytes.            */
  k_sh_text_cap    = 48U * 1024U, /**< Per-chapter plain-text buffer bytes.   */
  k_sh_max_lines   = 6144U,       /**< Max wrapped reader lines per chapter.  */
  k_sh_max_books   = 12U,         /**< Shelf / thumbnail-cache capacity.      */
  k_sh_thumb_bytes = 150U * 225U, /**< gray8 bytes per cached thumbnail.      */
  k_sh_name_cap    = 128U,        /**< SD file-name buffer bytes (VFAT long). */
  k_sh_title_cap   = 72U,         /**< Entry title buffer bytes.              */
  k_sh_author_cap  = 56U,         /**< Entry author buffer bytes.             */
  k_sh_uart_chan   = 8U,          /**< SCI8 J-Link OB console channel.        */
  k_sh_uart_baud   = 115200U,     /**< Console baud.                          */
  k_sh_poll_pts    = 5U,          /**< Touch points polled per read.          */
  k_sh_poll_ms     = 30U,         /**< Input poll period in ms.               */
  k_sh_dec_base    = 10U,         /**< Decimal formatting base.               */
} sh_const_t;

/**
 * @enum sh_loupe_const_t
 * @brief Press-and-hold magnifier-loupe geometry + gesture threshold (#211).
 * @details The lens is a fixed square window centred on the panel. While a press
 *          is held over a full-resolution cover/page image, a
 *          `k_sh_loupe_w / k_sh_loupe_zoom` by `k_sh_loupe_h / k_sh_loupe_zoom`
 *          window of the source is sampled at `k_sh_loupe_zoom` magnification and
 *          drawn 1:1 (no scale-to-fit decimation) via ra8_gfx_blit_gray4_zoom().
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_sh_loupe_w       = 320U, /**< Lens window width in framebuffer pixels.      */
  k_sh_loupe_h       = 320U, /**< Lens window height in framebuffer pixels.     */
  k_sh_loupe_zoom    = 2U,   /**< Integer magnification of the sampled window.  */
  k_sh_loupe_border  = 2U,   /**< Lens chrome (frame) thickness in pixels.      */
  k_sh_loupe_hold_ms = 350U, /**< Press-hold duration that opens the lens (ms). */
} sh_loupe_const_t;

/**
 * @enum sh_color_t
 * @brief 0x00RRGGBB palette shared by every screen.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_sh_col_bg      = 0x201A14U, /**< App background (dark wood).     */
  k_sh_col_bar     = 0x3A2E22U, /**< Header / title-bar fill.        */
  k_sh_col_card    = 0xF4ECDFU, /**< Card / page fill (paper).       */
  k_sh_col_card_hi = 0xFFF7E0U, /**< Selected card fill.             */
  k_sh_col_ink     = 0x1A140EU, /**< Body ink.                       */
  k_sh_col_sub     = 0x6A5C4AU, /**< Secondary ink (author / hints). */
  k_sh_col_edge    = 0xB7A789U, /**< Card / image border.            */
  k_sh_col_sel     = 0xC8762AU, /**< Selection accent (amber).       */
  k_sh_col_barfg   = 0xF4ECDFU, /**< Text on the bar.                */
  k_sh_col_rowhi   = 0xE7DCC6U, /**< TOC selected-row fill.          */
} sh_color_t;

/**
 * @enum sh_screen_t
 * @brief The active screen in ::sh_state_t.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_sh_screen_shelf  = 0U, /**< Book grid with cover thumbnails.    */
  k_sh_screen_cover  = 1U, /**< One book's cover / title page.      */
  k_sh_screen_toc    = 2U, /**< Chapter list.                       */
  k_sh_screen_reader = 3U, /**< Reading a book.                     */
  k_sh_screen_comic  = 4U, /**< Full-page image reader (CBZ / CBR). */
} sh_screen_t;

/**
 * @struct sh_line_t
 * @brief One wrapped reader line: a byte slice of ::sh_state_t::text.
 * @since 0.1.0
 */
typedef struct {
  uint32_t off; /**< Start offset into the chapter text buffer. */
  uint16_t len; /**< Length in bytes.                           */
} sh_line_t;

/**
 * @struct sh_entry_t
 * @brief One shelf book, sourced from MRAM (baked) or the SD card, in either
 *        the `.rabook` or `.epub` format.
 * @details The reader/TOC/cover screens are backend-agnostic: sh_book.c opens
 *          the right parser by ::fmt and exposes a uniform chapter/cover API.
 * @since 0.1.0
 */
typedef struct {
  bool           from_sd;                 /**< true: read ::sd_name from SD; false: use ::blob. */
  sh_book_fmt_t  fmt;                     /**< Container format (rabook / epub).                */
  const uint8_t* blob;                    /**< Baked RBKC bytes (MRAM), or NULL when SD.        */
  uint32_t       blob_len;                /**< Baked length, or SD file size in bytes.          */
  const uint8_t* thumb;                   /**< Pre-baked gray8 cover thumbnail, or NULL.        */
  uint16_t       thumb_w;                 /**< Pre-baked thumbnail width.                       */
  uint16_t       thumb_h;                 /**< Pre-baked thumbnail height.                      */
  char           sd_name[k_sh_name_cap];  /**< SD file name, e.g. "Meditations.rabook".         */
  char           title[k_sh_title_cap];   /**< Display title.                                   */
  char           author[k_sh_author_cap]; /**< Display author.                                  */
} sh_entry_t;

/**
 * @struct sh_state_t
 * @brief Whole-app UI + reader state (single instance ::g_sh).
 * @details Holds the active screen, the shelf selection + per-book cover
 *          thumbnail cache, the currently open (demand-paged) book, and the
 *          reader's chapter/page pagination working set.
 * @invariant `screen` is one of ::sh_screen_t.
 * @invariant When a rabook is open, `book_src.vm != nullptr` (paged bind via
 *            sh_paged.c); when none is, `book_src` is all-zero.
 * @since 0.1.0
 */
typedef struct {
  sh_screen_t screen;                /**< Active screen.                     */
  uint16_t    selected;              /**< Highlighted shelf book index.      */
  uint16_t    book_count;            /**< Shelf entries in use (baked + SD). */
  bool        sd_ready;              /**< true once an SD volume mounted.    */
  sh_entry_t  entry[k_sh_max_books]; /**< Unified baked+SD book table.       */

  sh_book_fmt_t  open_fmt;      /**< Format of the currently open book.       */
  ra8_book_src_t book_src;      /**< Paged source over the open rabook.       */
  uint32_t       chapter;       /**< Reader: current chapter index.           */
  uint32_t       chapter_count; /**< Open book chapter count.                 */
  uint32_t       page;          /**< Reader: current page within the chapter. */
  uint32_t       chap_pages;    /**< Reader: pages in the current chapter.    */
  int32_t        toc_scroll;    /**< TOC: first visible row index.            */

  uint32_t comic_page;  /**< Comic reader: current page index (0-based).        */
  uint32_t comic_count; /**< Comic reader: open comic page count (0 = none).    */
  bool     comic_rtl;   /**< Comic reader: right-to-left (manga) reading order. */

  bool     loupe_active;  /**< Press-hold magnifier loupe currently open (#211).   */
  int32_t  loupe_cx;      /**< Loupe pan centre X in full-res source pixels.       */
  int32_t  loupe_cy;      /**< Loupe pan centre Y in full-res source pixels.       */
  uint32_t touch_down_ms; /**< ra8_time_ms() timestamp at the current press start. */
  int32_t  touch_down_x;  /**< Press-start X in framebuffer pixels.                */
  int32_t  touch_down_y;  /**< Press-start Y in framebuffer pixels.                */

  size_t    text_len;              /**< Bytes of valid chapter text.  */
  uint32_t  line_count;            /**< Wrapped lines in the chapter. */
  char      text[k_sh_text_cap];   /**< Current chapter plain text.   */
  sh_line_t lines[k_sh_max_lines]; /**< Wrapped line slices.          */

  ra8_ui_rect_t card_rect[k_sh_max_books];               /**< Cached shelf card rects. */
  uint8_t       thumb[k_sh_max_books][k_sh_thumb_bytes]; /**< gray8 covers.            */
  uint16_t      thumb_w[k_sh_max_books];                 /**< Drawn thumbnail width.   */
  uint16_t      thumb_h[k_sh_max_books];                 /**< Drawn thumbnail height.  */
} sh_state_t;

/** @brief The single whole-app state instance (defined in `main.c`). */
extern sh_state_t g_sh;

/* ----- sh_image.c : 4bpp grayscale cover / image rendering ----------------- */

/**
 * @brief Decode a 4bpp grayscale image, aspect-fit-scale it into @p box, and
 *        write the result into a gray8 buffer (host-friendly: no framebuffer).
 *
 * @param[in]  src      Bound book source (image bytes read on demand).
 * @param[in]  img_idx  Image-table index (`< header image_count`).
 * @param[out] out      Destination gray8 buffer (>= box_w*box_h bytes).
 * @param[in]  box_w    Bounding-box width in pixels (> 0).
 * @param[in]  box_h    Bounding-box height in pixels (> 0).
 * @param[out] out_w    Receives the actual scaled width.
 * @param[out] out_h    Receives the actual scaled height.
 * @return k_ra8_ok on success; k_ra8_err_invalid_arg on bad index/format.
 * @pre @p src was bound by ra8_book_src_paged() / ra8_book_src_resident().
 * @post On success @p out holds out_w*out_h gray bytes (row-major).
 * @since 0.1.0
 */
ra8_err_t sh_image_decode_gray8(const ra8_book_src_t* src,
                                uint32_t              img_idx,
                                uint8_t*              out,
                                int32_t               box_w,
                                int32_t               box_h,
                                int32_t*              out_w,
                                int32_t*              out_h);

/**
 * @brief Blit a gray8 bitmap to the bound framebuffer at @p dst_x, @p dst_y.
 * @param[in] src   gray8 pixels (row-major, w*h bytes), non-NULL.
 * @param[in] w     Width in pixels (> 0).
 * @param[in] h     Height in pixels (> 0).
 * @param[in] dst_x Left edge in framebuffer pixels.
 * @param[in] dst_y Top edge in framebuffer pixels.
 * @pre ra8_gfx is bound to the framebuffer.
 * @post Pixels in range are written; out-of-range pixels are clipped.
 * @since 0.1.0
 */
void sh_image_blit_gray8(const uint8_t* src, int32_t w, int32_t h, int32_t dst_x, int32_t dst_y);

/**
 * @brief Decode + aspect-fit a 4bpp image straight into the framebuffer box.
 * @param[in]  src     Bound book source (image bytes read on demand).
 * @param[in]  img_idx Image-table index.
 * @param[in]  dst_x   Box left edge.
 * @param[in]  dst_y   Box top edge.
 * @param[in]  box_w   Box width (> 0).
 * @param[in]  box_h   Box height (> 0).
 * @param[out] out_w   Receives drawn width (may be NULL).
 * @param[out] out_h   Receives drawn height (may be NULL).
 * @return k_ra8_ok on success.
 * @pre @p src bound; ra8_gfx bound.
 * @post The scaled image is centred in the box.
 * @since 0.1.0
 */
ra8_err_t sh_image_blit_cover(const ra8_book_src_t* src,
                              uint32_t              img_idx,
                              int32_t               dst_x,
                              int32_t               dst_y,
                              int32_t               box_w,
                              int32_t               box_h,
                              int32_t*              out_w,
                              int32_t*              out_h);

/**
 * @brief Draw the magnifier loupe: a `k_sh_loupe_zoom`-magnified 1:1 window of a
 *        full-resolution 4bpp source image, centred on a source point (#211).
 * @details Samples a `k_sh_loupe_w / k_sh_loupe_zoom` by
 *          `k_sh_loupe_h / k_sh_loupe_zoom` window of the FULL-RESOLUTION source
 *          (read through the demand-paged book, one bounded window per call --
 *          never scale-to-fit decimated), integer-zooms it into the lens box, and
 *          frames it. The clip is confined to the lens window so nothing spills.
 * @param[in] src     Bound book source (image bytes read on demand).
 * @param[in] img_idx Image-table index of the source to magnify.
 * @param[in] src_cx  Pan-centre X in full-resolution source pixels.
 * @param[in] src_cy  Pan-centre Y in full-resolution source pixels.
 * @param[in] dst_x   Lens-window left edge in framebuffer pixels.
 * @param[in] dst_y   Lens-window top edge in framebuffer pixels.
 * @return k_ra8_ok on success; k_ra8_err_invalid_arg on a bad index/format/read.
 * @pre @p src was bound by ra8_book_src_paged() / ra8_book_src_resident().
 * @pre ra8_gfx is bound to the framebuffer.
 * @post On success the lens window holds the magnified source window + a frame.
 * @since 0.1.0
 */
ra8_err_t sh_image_loupe(const ra8_book_src_t* src,
                         uint32_t              img_idx,
                         int32_t               src_cx,
                         int32_t               src_cy,
                         int32_t               dst_x,
                         int32_t               dst_y);

/**
 * @brief Map a framebuffer touch point over an aspect-fit image box to
 *        full-resolution source coordinates (loupe pan target).
 * @param[in]  src     Bound book source.
 * @param[in]  img_idx Image-table index (the displayed image).
 * @param[in]  box_x   Display box left in framebuffer pixels.
 * @param[in]  box_y   Display box top in framebuffer pixels.
 * @param[in]  box_w   Display box width (> 0).
 * @param[in]  box_h   Display box height (> 0).
 * @param[in]  px      Touch X in framebuffer pixels.
 * @param[in]  py      Touch Y in framebuffer pixels.
 * @param[out] out_cx  Receives the source X the point maps to.
 * @param[out] out_cy  Receives the source Y the point maps to.
 * @return true if the point is over the displayed (aspect-fit) image; false
 *         otherwise (bad index/format, or point outside the fitted image).
 * @pre @p src, @p out_cx and @p out_cy are non-NULL.
 * @post On true, (@p out_cx, @p out_cy) is inside the source image bounds.
 * @since 0.1.0
 */
bool sh_image_loupe_map(const ra8_book_src_t* src,
                        uint32_t              img_idx,
                        int32_t               box_x,
                        int32_t               box_y,
                        int32_t               box_w,
                        int32_t               box_h,
                        int32_t               px,
                        int32_t               py,
                        int32_t*              out_cx,
                        int32_t*              out_cy);

/* ----- shared small helpers (sh_util.c) ------------------------------------ */

/** @brief Cells of the 8px bitmap font that fit in @p pixels. */
int32_t sh_cells(int32_t pixels);

/** @brief Copy @p src into @p dst (cap bytes), truncating past @p max_chars with "..". */
void sh_fit(char* dst, size_t cap, const char* src, int32_t max_chars);

/** @brief Append unsigned @p v as decimal into @p dst at @p pos; returns new pos. */
size_t sh_fmt_uint(char* dst, size_t pos, uint32_t v);

/** @brief Draw a @p width-thick rectangle outline in @p colour. */
void sh_border(ra8_ui_rect_t r, uint32_t colour, int32_t width);

/** @brief Fill the header bar and draw @p title left + optional @p right text. */
void sh_titlebar(const char* title, const char* right);

/* ----- per-screen render + input (one module each) ------------------------- */

/* ----- sh_sd.c : optional SD-card book source ------------------------------ */

/**
 * @brief Bring up the Pmod2 microSD over ra8_sdmmc_spi and mount its FAT volume.
 * @return true if a card mounted; false (the common no-card case) leaves the
 *         shelf baked-only.
 * @pre Clocks (ra8_cgc) are initialised.
 * @post On true, ::sh_sd_read / ::sh_sd_scan are usable.
 * @since 0.1.0
 */
bool sh_sd_mount(void);

/** @brief Append every root *.RBK file on the card as an SD shelf entry. */
void sh_sd_scan(void);

/**
 * @brief Open SD file @p name and hold it as the paged book backing.
 * @details Closes any previously held book file first. The handle stays open
 *          until ::sh_sd_book_close (or the next open), so ::sh_sd_book_read
 *          can serve chunk reads on demand -- the file is never slurped whole.
 * @param[in]  name    Root file name (e.g. "Meditations.rabook").
 * @param[out] out_len Receives the file size in bytes.
 * @return true if the file is open and non-empty.
 * @since 0.1.0
 */
bool sh_sd_book_open(const char* name, uint32_t* out_len);

/**
 * @brief ra8_vsource_read_fn over the held-open SD book file (seek + read).
 * @param[in]  ctx    Unused (the held file handle is module state).
 * @param[in]  offset Absolute byte offset within the file.
 * @param[out] buf    Destination for exactly @p len bytes.
 * @param[in]  len    Bytes to read; a read past EOF is rejected.
 * @return k_ra8_ok, or the ra8_fs error / k_ra8_err_out_of_range on a short read.
 * @pre A book file is held open via ::sh_sd_book_open.
 * @post On k_ra8_ok exactly @p len bytes were copied to @p buf.
 * @since 0.1.0
 */
ra8_err_t sh_sd_book_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len);

/** @brief Close the held SD book file (idempotent). */
void sh_sd_book_close(void);

/**
 * @brief ::ra8_comic_read_fn over the held-open SD book file (seek + clamped read).
 * @details Shares the held file handle with ::sh_sd_book_read but matches the
 *          comic reader's seam shape (returns the byte count, not an error) and
 *          serves a short read at EOF instead of rejecting it -- ra8_comic's
 *          miniz/RAR backends probe the archive tail and treat a short read as
 *          end-of-file.
 * @param[in]  ctx    Unused (the held file handle is module state).
 * @param[in]  offset Absolute byte offset within the file.
 * @param[out] buf    Destination buffer (@p len writable bytes).
 * @param[in]  len    Bytes requested.
 * @return Bytes actually copied (0 at/after EOF or on any error).
 * @pre A comic file is held open via ::sh_sd_book_open.
 * @pre @p buf holds @p len writable bytes.
 * @post At most @p len bytes were copied to @p buf; the return is that count.
 * @since 0.1.0
 */
size_t sh_sd_comic_read(void* ctx, uint64_t offset, void* buf, size_t len);

/**
 * @brief Stream-open SD file @p name as an EPUB via ra8_epub_open_streamed_fs.
 * @details No whole-file residency (#230): the source file stays held open by
 *          this module and every ZIP entry (chapter, cover, resource) is
 *          seek+read on demand, so there is no book-size ceiling below the
 *          `ra8_fs` 4 GiB offset limit. Release with ::sh_sd_close_epub.
 * @param[in]  name     Root file name (e.g. "Pride and Prejudice.epub").
 * @param[out] out_book ra8_epub_book_t* (void* to keep ra8_epub out of this header).
 * @return true if the EPUB opened (the source file is now held open).
 * @since 0.1.0
 */
bool sh_sd_open_epub(const char* name, void* out_book);

/**
 * @brief Close a streamed EPUB opened by ::sh_sd_open_epub and its source file.
 * @param[in,out] book The ra8_epub_book_t* passed to ::sh_sd_open_epub.
 * @since 0.1.0
 */
void sh_sd_close_epub(void* book);

/** @brief The live RGB565 framebuffer ra8_gfx scans (defined in main.c). */
uint16_t* sh_fb_pixels(void);

/** @brief Decode the open rabook source's 4bpp cover into thumbnail slot @p idx. */
void sh_decode_cover(uint16_t idx, const ra8_book_src_t* src);

/** @brief Copy the baked books' pre-decoded covers into ::sh_state_t::thumb at boot. */
void sh_shelf_build_thumbs(void);

/** @brief Render the shelf grid (cover cards) into the framebuffer. */
void sh_shelf_render(void);

/** @brief Hit-test a shelf tap; returns the book index or -1. */
int32_t sh_shelf_hit(int32_t x, int32_t y);

/**
 * @enum sh_cover_act_t
 * @brief Result of a tap on the cover screen.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_sh_cover_none = 0U, /**< Tap hit nothing actionable.   */
  k_sh_cover_read = 1U, /**< Open the reader at chapter 0. */
  k_sh_cover_toc  = 2U, /**< Open the table of contents.   */
} sh_cover_act_t;

/** @brief Render the selected book's cover / title page. */
void sh_cover_render(void);

/** @brief Classify a cover-screen tap into a ::sh_cover_act_t. */
sh_cover_act_t sh_cover_action(int32_t x, int32_t y);

/**
 * @brief Map a cover-screen touch to full-res source coordinates for the loupe.
 * @details Only rabook covers with a real 4bpp cover image are magnifiable; EPUB
 *          covers (decoded JPEG/PNG) and cover-less books return false.
 * @param[in]  px     Touch X in framebuffer pixels.
 * @param[in]  py     Touch Y in framebuffer pixels.
 * @param[out] out_cx Receives the source-image X the point maps to.
 * @param[out] out_cy Receives the source-image Y the point maps to.
 * @return true if the point is over a magnifiable cover image.
 * @since 0.1.0
 */
bool sh_cover_loupe_map(int32_t px, int32_t py, int32_t* out_cx, int32_t* out_cy);

/** @brief Draw the magnifier loupe over the cover at the current pan centre. */
void sh_cover_loupe_render(void);

/** @brief Render the open book's chapter list (TOC). */
void sh_toc_render(void);

/** @brief Hit-test a TOC tap; returns the chapter index or -1. */
int32_t sh_toc_hit(int32_t x, int32_t y);

/** @brief Scroll the TOC list by @p dir pages (-1 up / +1 down), clamped. */
void sh_toc_scroll(int32_t dir);

/**
 * @brief Heap-free zlib inflater matching ra8_book_inflate_fn (miniz; main.c).
 * @details The chunked reader hands one chunk's zlib stream plus a destination
 *          sized to the recorded inflated length, so a single non-wrapping
 *          tinfl_decompress() call suffices -- no dictionary window, no
 *          allocation. tinfl also verifies the stream's Adler-32, which is the
 *          per-chunk integrity check the paged open path relies on.
 * @param[in]  src     Start of the zlib stream.
 * @param[in]  src_len Stream length in bytes.
 * @param[out] dst     Destination buffer to inflate into.
 * @param[in]  dst_cap Capacity of @p dst in bytes.
 * @param[out] out_len Receives the number of bytes written to @p dst.
 * @return k_ra8_ok, or k_ra8_err_invalid_size on a malformed/mismatched stream.
 * @pre @p src holds one complete zlib (RFC 1950) stream.
 * @post On k_ra8_ok the stream's Adler-32 checksum was verified.
 * @since 0.1.0
 */
ra8_err_t sh_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len);

/* ----- sh_paged.c : demand-paged .rabook backing (#204/#205) ---------------- */

/**
 * @brief Open a chunked `.rabook` container as the demand-paged book source.
 * @details Binds the RBKC chunk reader over @p file_read, registers it as a
 *          paged ::ra8_vsource object, (re)initialises the ra8_vmem frame cache,
 *          and binds `g_sh.book_src` in paged mode. Always paged -- there is
 *          deliberately no resident/paged size threshold (#205).
 * @param[in] file_read Byte reader over the container file bytes.
 * @param[in] file_ctx  Context for @p file_read.
 * @param[in] file_len  Container file length in bytes.
 * @return true on success; false leaves no book bound (and the SD backing,
 *         if any, closed).
 * @pre Any previously open book was torn down via ::sh_paged_close (the
 *      backing @p file_read reads from must be live when this is called).
 * @since 0.1.0
 */
bool sh_paged_open(ra8_vsource_read_fn file_read, void* file_ctx, uint64_t file_len);

/**
 * @brief Open a memory-mapped (baked MRAM) RBKC container as the paged source.
 * @details The file-read callback is a bounds-checked memcpy from @p blob; the
 *          container bytes are already addressable, but chunks still inflate
 *          on demand into cache frames -- same path as SD books.
 * @param[in] blob     First container byte (MRAM).
 * @param[in] blob_len Container length in bytes.
 * @return true on success; false leaves no book bound.
 * @since 0.1.0
 */
bool sh_paged_open_mram(const uint8_t* blob, uint32_t blob_len);

/** @brief Unbind the paged book source and release its SD backing (idempotent). */
void sh_paged_close(void);

/* ----- sh_book.c : format-agnostic book backend (rabook + epub) ------------ */

/**
 * @brief Open shelf entry @p idx by its format; sets g_sh.open_fmt + chapter
 *        count, and (for SD books on first open) the entry title/author/cover.
 * @details `.rabook` entries (baked or SD) bind the demand-paged source via
 *          sh_paged.c; `.epub` entries open through ra8_epub.
 * @param[in] idx Shelf entry index (`< g_sh.book_count`).
 * @return true on success; false leaves no book open.
 * @since 0.1.0
 */
bool sh_book_open(uint16_t idx);

/**
 * @brief Extract one chapter's plain text from the open book (either backend).
 * @param[in]  chapter Chapter index (`< g_sh.chapter_count`).
 * @param[out] out     Plain-text destination.
 * @param[in]  cap     Capacity of @p out.
 * @param[out] out_len Receives the text length.
 * @return k_ra8_ok on success.
 * @since 0.1.0
 */
ra8_err_t sh_book_chapter_text(uint32_t chapter, char* out, size_t cap, size_t* out_len);

/** @brief Resolve a chapter's TOC label (or "Chapter N") into @p out. */
void sh_book_chapter_label(uint32_t chapter, char* out, size_t cap);

/** @brief Decode the open book's cover into thumbnail slot @p idx; returns ok. */
bool sh_book_decode_thumb(uint16_t idx);

/** @brief Draw the open book's cover full-size, aspect-fit into the box. */
void sh_book_cover_fullscreen(int32_t x, int32_t y, int32_t w, int32_t h);

/** @brief Load + wrap a chapter into the reader working set. */
void sh_reader_load_chapter(uint32_t chapter);

/** @brief First chapter with substantial prose (skips title/imprint front matter). */
uint32_t sh_reader_first_content(void);

/** @brief Render the current reader page. */
void sh_reader_render(void);

/** @brief Turn the reader page by @p dir (-1/+1) across chapters; returns changed. */
bool sh_reader_turn(int32_t dir);

/**
 * @brief Read-ahead warm the adjacent chapters into the page cache (#207).
 * @details Called in the reader's flush-idle window (after the panel flush, before
 *          the next input poll): a within-chapter page turn reads no new cache
 *          bytes (the chapter text is pre-extracted into ::sh_state_t::text), so the
 *          only demand faults happen when a turn crosses a chapter boundary. This
 *          warms chapter N+1 (forward reading) and N-1 (a back-flip) via
 *          ra8_book_src_prefetch_chapter() so that crossing finds them resident.
 *          Best-effort and transparent (warming never changes rendered output);
 *          a no-op unless a demand-paged `.rabook` is open.
 * @pre The reader screen is active (`g_sh.screen == k_sh_screen_reader`).
 * @pre A book is open (or the call is a no-op).
 * @post No cache frame is left pinned by this call.
 * @post Rendered output and ::g_sh reader state are unchanged.
 * @since 0.1.0
 */
void sh_reader_prefetch_adjacent(void);

/* ----- sh_comic.c : full-page image reader for CBZ / CBR comics (#236) ------ */

/**
 * @brief Open shelf entry @p idx as a comic archive (CBZ / CBR) for page reading.
 * @details Binds ::ra8_comic over the entry's backing -- a baked MRAM blob, or
 *          the held-open SD file (::sh_sd_book_open + ::sh_sd_comic_read) -- and
 *          builds the sorted page index. On success `g_sh.comic_count` is the
 *          page count and `g_sh.comic_page` is reset to 0. The reading direction
 *          (`g_sh.comic_rtl`) is preserved across opens (an app-level toggle,
 *          since raw CBZ/CBR carry no direction metadata).
 * @param[in] idx Shelf entry index (`< g_sh.book_count`) whose fmt is a comic.
 * @return true if the comic opened with >= 1 page; false leaves none open.
 * @retval true  Comic bound; `g_sh.comic_count >= 1`, `g_sh.comic_page == 0`.
 * @retval false Bad index/format, backing open failed, or no decodable pages.
 * @pre The entry at @p idx has ::sh_fmt_is_comic() true.
 * @pre Any previously open book/comic was torn down by the caller (sh_book_open).
 * @post On true a comic is bound and page 0 is the current page.
 * @post On false no comic is bound (`g_sh.comic_count == 0`).
 * @note Not thread-safe; single-threaded UI loop only.
 * @since 0.1.0
 */
bool sh_comic_open(uint16_t idx);

/** @brief Close any open comic and release its backing (idempotent). */
void sh_comic_close(void);

/** @brief Render the current comic page full-screen (title bar + fitted image). */
void sh_comic_render(void);

/**
 * @brief Map a comic-screen edge tap to a reading-order page delta (RTL-aware).
 * @details The left third yields one edge and the right third the other; the
 *          centre third is neutral. In left-to-right order the right edge is
 *          "next" (+1); in right-to-left (manga) order the mapping is mirrored,
 *          so the LEFT edge advances. The centre band returns 0.
 * @param[in] x Touch X in framebuffer pixels (0 .. ::k_sh_fb_w - 1).
 * @return +1 to advance a page, -1 to go back, 0 for a neutral centre tap.
 * @retval +1 Tap maps to the next page in reading order.
 * @retval -1 Tap maps to the previous page in reading order.
 * @retval  0 Tap fell in the neutral centre band.
 * @pre `g_sh.comic_rtl` reflects the active reading direction.
 * @post No state is modified (pure mapping).
 * @note Thread-safe: pure function of @p x and the RTL flag.
 * @since 0.1.0
 */
int32_t sh_comic_edge_dir(int32_t x);

/**
 * @brief Turn the comic page by @p dir in reading order, clamped to the ends.
 * @param[in] dir +1 for the next page, -1 for the previous; 0 is a no-op.
 * @return true if the page index changed (a repaint is needed).
 * @retval true  `g_sh.comic_page` advanced/retreated by one.
 * @retval false At an end, no comic open, or @p dir == 0.
 * @pre A comic is open (or the call is a no-op).
 * @post `g_sh.comic_page` stays within `[0, g_sh.comic_count)`.
 * @post On false no state changed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool sh_comic_turn(int32_t dir);

/**
 * @brief Handle a comic-screen tap: header returns to the shelf, else page-turn.
 * @param[in] x      Touch X in framebuffer pixels.
 * @param[in] header true if the tap landed in the header bar (`y < k_sh_bar_h`).
 * @return true if the screen must be repainted.
 * @retval true  Navigated to the shelf, or a page turned.
 * @retval false Neutral tap with no state change.
 * @pre The comic screen is active.
 * @post On a header tap `g_sh.screen == k_sh_screen_shelf`.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool sh_comic_tap(int32_t x, bool header);

/** @brief Flip the comic reading direction (LTR <-> RTL); page index unchanged. */
void sh_comic_toggle_rtl(void);

/**
 * @struct sh_comic_probe_t
 * @brief One container's headless self-check result (page 0 decode digest).
 * @details Filled by ::sh_comic_selfcheck for the baked CBZ and CBR fixtures so
 *          the boot banner can pin the integrated comic decode + render path
 *          with a deterministic, toolchain-independent digest (integer decode).
 * @invariant `ok` is true only when a page decoded and `crc` is meaningful.
 * @since 0.1.0
 */
typedef struct {
  bool     ok;    /**< true if page 0 opened + decoded successfully.   */
  uint32_t pages; /**< Page count parsed from the archive.             */
  int32_t  w;     /**< Blitted (fitted) page width in pixels.          */
  int32_t  h;     /**< Blitted (fitted) page height in pixels.         */
  uint32_t crc;   /**< FNV-1a-32 over the decoded scratch framebuffer. */
} sh_comic_probe_t;

/**
 * @brief Headless boot self-check: decode the baked CBZ + CBR page 0, verify RTL.
 * @details Opens each baked fixture through the SAME ::ra8_comic path the live
 *          screen uses, decodes page 0 into an off-screen RGB565 scratch via the
 *          integer ::ra8_img_decode_blit pipeline, and FNV-hashes the scratch --
 *          a deterministic digest identical on host, ra8_emulator, and silicon. It
 *          also exercises ::sh_comic_edge_dir in both reading directions. The
 *          live display framebuffer binding is restored on return, so the shelf
 *          render (and its pinned `fb=` hash) is untouched.
 * @param[out] cbz     Receives the CBZ page-0 decode result (non-NULL).
 * @param[out] cbr     Receives the CBR page-0 decode result (non-NULL).
 * @param[out] rtl_ok  Receives true if the RTL edge mapping mirrors LTR (non-NULL).
 * @pre ra8_gfx is bound to the live framebuffer on entry.
 * @pre `cbz`, `cbr`, and `rtl_ok` are non-NULL.
 * @post ra8_gfx is rebound to the live framebuffer (::sh_fb_pixels) on return.
 * @post No comic is left open.
 * @note Not thread-safe; single-threaded boot only.
 * @since 0.1.0
 */
void sh_comic_selfcheck(sh_comic_probe_t* cbz, sh_comic_probe_t* cbr, bool* rtl_ok);

/**
 * @brief Append " cbz=P:WxH:CRC cbr=P:WxH:CRC rtl=OK" to the boot banner buffer.
 * @details Formats the two ::sh_comic_selfcheck digests + the RTL verdict onto
 *          @p b at @p *pos, advancing @p *pos past what it wrote. The comic
 *          golden the shelf `hil.conf` pins lives entirely in these fields, so
 *          the shelf's own `fb=` hash stays byte-identical (this appends after).
 * @param[in,out] b      Banner buffer with room for the appended fields.
 * @param[in,out] pos    Current write offset into @p b; advanced on return.
 * @param[in]     cbz    CBZ page-0 self-check result (non-NULL).
 * @param[in]     cbr    CBR page-0 self-check result (non-NULL).
 * @param[in]     rtl_ok RTL edge-mapping verdict from ::sh_comic_selfcheck.
 * @pre @p b holds at least `*pos + 64` writable bytes.
 * @pre @p pos, @p cbz, @p cbr are non-NULL.
 * @post `*pos` advanced past the appended text; @p b is not NUL-terminated here.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void sh_comic_append_banner(char*                   b,
                            size_t*                 pos,
                            const sh_comic_probe_t* cbz,
                            const sh_comic_probe_t* cbr,
                            bool                    rtl_ok);
