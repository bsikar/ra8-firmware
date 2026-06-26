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
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 6 / App] {World: NS}
 *
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "library.h"
#include "ra_book.h"
#include "ra_ui.h"

/**
 * @enum sh_const_t
 * @brief Panel geometry, layout, font, and reader sizing constants.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_sh_fb_w        = 1024U,       /**< Panel width in pixels.                */
  k_sh_fb_h        = 600U,        /**< Panel height in pixels.               */
  k_sh_fb_align    = 64U,         /**< AXI framebuffer alignment.            */
  k_sh_glyph_w     = 8U,          /**< Bitmap font cell width.               */
  k_sh_glyph_h     = 16U,         /**< Bitmap font cell height.              */
  k_sh_line_h      = 22U,         /**< Reader text line height.              */
  k_sh_bar_h       = 56U,         /**< Header / title-bar height.            */
  k_sh_pad         = 24U,         /**< Outer margin / content inset.         */
  k_sh_gap         = 24U,         /**< Gap between shelf cards.              */
  k_sh_card_pad    = 14U,         /**< Inner card inset.                     */
  k_sh_thumb_w     = 130U,        /**< Shelf cover-thumbnail box width.      */
  k_sh_thumb_h     = 195U,        /**< Shelf cover-thumbnail box height.     */
  k_sh_grid_cols   = 4U,          /**< Shelf grid columns.                   */
  k_sh_toc_row_h   = 40U,         /**< TOC list row height.                  */
  k_sh_linebuf     = 160U,        /**< Per-line draw buffer bytes.           */
  k_sh_text_cap    = 48U * 1024U, /**< Per-chapter plain-text buffer bytes.  */
  k_sh_max_lines   = 6144U,       /**< Max wrapped reader lines per chapter. */
  k_sh_max_books   = 12U,         /**< Shelf / thumbnail-cache capacity.     */
  k_sh_thumb_bytes = 150U * 225U, /**< gray8 bytes per cached thumbnail.     */
  k_sh_name_cap    = 16U,         /**< SD 8.3 file-name buffer bytes.        */
  k_sh_title_cap   = 72U,         /**< Entry title buffer bytes.             */
  k_sh_author_cap  = 56U,         /**< Entry author buffer bytes.            */
  k_sh_uart_chan   = 8U,          /**< SCI8 J-Link OB console channel.       */
  k_sh_uart_baud   = 115200U,     /**< Console baud.                         */
  k_sh_gt911_addr  = 0x5DU,       /**< GT911 touch 7-bit I2C address.        */
  k_sh_poll_pts    = 5U,          /**< Touch points polled per read.         */
  k_sh_poll_ms     = 30U,         /**< Input poll period in ms.              */
  k_sh_dec_base    = 10U,         /**< Decimal formatting base.              */
} sh_const_t;

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
  k_sh_screen_shelf  = 0U, /**< Book grid with cover thumbnails. */
  k_sh_screen_cover  = 1U, /**< One book's cover / title page.   */
  k_sh_screen_toc    = 2U, /**< Chapter list.                    */
  k_sh_screen_reader = 3U, /**< Reading a book.                  */
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
 * @enum sh_book_fmt_t
 * @brief Book container format behind a shelf entry / the open book.
 * @details Both formats render through the same screens via the sh_book.c
 *          backend: `.rabook` is the pre-parsed ra_book blob (fast, baked or on
 *          SD); `.epub` is parsed on-device by ra_epub (SD only).
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_sh_fmt_rabook = 0U, /**< ra_book RBKZ container (inflate + walk). */
  k_sh_fmt_epub   = 1U, /**< EPUB parsed on-device by ra_epub.        */
} sh_book_fmt_t;

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
  const uint8_t* blob;                    /**< Baked RBKZ bytes (MRAM), or NULL when SD.        */
  uint32_t       blob_len;                /**< Baked length, or SD file size in bytes.          */
  const uint8_t* thumb;                   /**< Pre-baked gray8 cover thumbnail, or NULL.        */
  uint16_t       thumb_w;                 /**< Pre-baked thumbnail width.                       */
  uint16_t       thumb_h;                 /**< Pre-baked thumbnail height.                      */
  char           sd_name[k_sh_name_cap];  /**< SD 8.3 file name (e.g. "BOOK01.RBK").            */
  char           title[k_sh_title_cap];   /**< Display title.                                   */
  char           author[k_sh_author_cap]; /**< Display author.                                  */
} sh_entry_t;

/**
 * @struct sh_state_t
 * @brief Whole-app UI + reader state (single instance ::g_sh).
 * @details Holds the active screen, the shelf selection + per-book cover
 *          thumbnail cache, the currently open inflated book, and the reader's
 *          chapter/page pagination working set.
 * @invariant `screen` is one of ::sh_screen_t.
 * @invariant `book_base != nullptr` whenever `screen != k_sh_screen_shelf`.
 * @since 0.1.0
 */
typedef struct {
  sh_screen_t screen;                /**< Active screen.                     */
  uint16_t    selected;              /**< Highlighted shelf book index.      */
  uint16_t    book_count;            /**< Shelf entries in use (baked + SD). */
  bool        sd_ready;              /**< true once an SD volume mounted.    */
  sh_entry_t  entry[k_sh_max_books]; /**< Unified baked+SD book table.       */

  sh_book_fmt_t open_fmt;      /**< Format of the currently open book.         */
  const void*   book_base;     /**< Open rabook's inflated base (rabook only). */
  uint32_t      chapter;       /**< Reader: current chapter index.             */
  uint32_t      chapter_count; /**< Open book chapter count.                   */
  uint32_t      page;          /**< Reader: current page within the chapter.   */
  uint32_t      chap_pages;    /**< Reader: pages in the current chapter.      */
  int32_t       toc_scroll;    /**< TOC: first visible row index.              */

  size_t    text_len;              /**< Bytes of valid chapter text.  */
  uint32_t  line_count;            /**< Wrapped lines in the chapter. */
  char      text[k_sh_text_cap];   /**< Current chapter plain text.   */
  sh_line_t lines[k_sh_max_lines]; /**< Wrapped line slices.          */

  ra_ui_rect_t card_rect[k_sh_max_books];               /**< Cached shelf card rects. */
  uint8_t      thumb[k_sh_max_books][k_sh_thumb_bytes]; /**< gray8 covers.            */
  uint16_t     thumb_w[k_sh_max_books];                 /**< Drawn thumbnail width.   */
  uint16_t     thumb_h[k_sh_max_books];                 /**< Drawn thumbnail height.  */
} sh_state_t;

/** @brief The single whole-app state instance (defined in `main.c`). */
extern sh_state_t g_sh;

/* ----- sh_image.c : 4bpp grayscale cover / image rendering ----------------- */

/**
 * @brief Decode a 4bpp grayscale image, aspect-fit-scale it into @p box, and
 *        write the result into a gray8 buffer (host-friendly: no framebuffer).
 *
 * @param[in]  base     Inflated book base (non-NULL).
 * @param[in]  img_idx  Image-table index (`< header image_count`).
 * @param[out] out      Destination gray8 buffer (>= box_w*box_h bytes).
 * @param[in]  box_w    Bounding-box width in pixels (> 0).
 * @param[in]  box_h    Bounding-box height in pixels (> 0).
 * @param[out] out_w    Receives the actual scaled width.
 * @param[out] out_h    Receives the actual scaled height.
 * @return k_ra_ok on success; k_ra_err_invalid_arg on bad index/format.
 * @pre @p base was accepted by ra_book_open().
 * @post On success @p out holds out_w*out_h gray bytes (row-major).
 * @since 0.1.0
 */
ra_err_t sh_image_decode_gray8(const void* base,
                               uint32_t    img_idx,
                               uint8_t*    out,
                               int32_t     box_w,
                               int32_t     box_h,
                               int32_t*    out_w,
                               int32_t*    out_h);

/**
 * @brief Blit a gray8 bitmap to the bound framebuffer at @p dst_x, @p dst_y.
 * @param[in] src   gray8 pixels (row-major, w*h bytes), non-NULL.
 * @param[in] w     Width in pixels (> 0).
 * @param[in] h     Height in pixels (> 0).
 * @param[in] dst_x Left edge in framebuffer pixels.
 * @param[in] dst_y Top edge in framebuffer pixels.
 * @pre ra_gfx is bound to the framebuffer.
 * @post Pixels in range are written; out-of-range pixels are clipped.
 * @since 0.1.0
 */
void sh_image_blit_gray8(const uint8_t* src, int32_t w, int32_t h, int32_t dst_x, int32_t dst_y);

/**
 * @brief Decode + aspect-fit a 4bpp image straight into the framebuffer box.
 * @param[in]  base    Inflated book base (non-NULL).
 * @param[in]  img_idx Image-table index.
 * @param[in]  dst_x   Box left edge.
 * @param[in]  dst_y   Box top edge.
 * @param[in]  box_w   Box width (> 0).
 * @param[in]  box_h   Box height (> 0).
 * @param[out] out_w   Receives drawn width (may be NULL).
 * @param[out] out_h   Receives drawn height (may be NULL).
 * @return k_ra_ok on success.
 * @pre @p base inflated; ra_gfx bound.
 * @post The scaled image is centred in the box.
 * @since 0.1.0
 */
ra_err_t sh_image_blit_cover(const void* base,
                             uint32_t    img_idx,
                             int32_t     dst_x,
                             int32_t     dst_y,
                             int32_t     box_w,
                             int32_t     box_h,
                             int32_t*    out_w,
                             int32_t*    out_h);

/* ----- shared small helpers (sh_util.c) ------------------------------------ */

/** @brief Cells of the 8px bitmap font that fit in @p pixels. */
int32_t sh_cells(int32_t pixels);

/** @brief Copy @p src into @p dst (cap bytes), truncating past @p max_chars with "..". */
void sh_fit(char* dst, size_t cap, const char* src, int32_t max_chars);

/** @brief Append unsigned @p v as decimal into @p dst at @p pos; returns new pos. */
size_t sh_fmt_uint(char* dst, size_t pos, uint32_t v);

/** @brief Draw a @p width-thick rectangle outline in @p colour. */
void sh_border(ra_ui_rect_t r, uint32_t colour, int32_t width);

/** @brief Fill the header bar and draw @p title left + optional @p right text. */
void sh_titlebar(const char* title, const char* right);

/* ----- per-screen render + input (one module each) ------------------------- */

/* ----- sh_sd.c : optional SD-card book source ------------------------------ */

/**
 * @brief Bring up the Pmod2 microSD over ra_sdmmc_spi and mount its FAT volume.
 * @return true if a card mounted; false (the common no-card case) leaves the
 *         shelf baked-only.
 * @pre Clocks (ra_cgc) are initialised.
 * @post On true, ::sh_sd_read / ::sh_sd_scan are usable.
 * @since 0.1.0
 */
bool sh_sd_mount(void);

/** @brief Append every root *.RBK file on the card as an SD shelf entry. */
void sh_sd_scan(void);

/**
 * @brief Read SD file @p name into the shared file buffer; NULL on failure.
 * @param[in]  name    8.3 root file name (e.g. "BOOK01.RBK").
 * @param[out] out_len Receives the byte length read.
 * @return Pointer to the file bytes (valid until the next call), or NULL.
 * @since 0.1.0
 */
const uint8_t* sh_sd_read(const char* name, uint32_t* out_len);

/**
 * @brief Parse SD file @p name as an EPUB into @p out_book via ra_epub_open_fs.
 * @param[in]  name     8.3 root file name.
 * @param[out] out_book ra_epub_book_t* (void* to keep ra_epub out of this header).
 * @param[in]  filebuf  Caller buffer the whole .epub is read into (must outlive the book).
 * @param[in]  cap      Capacity of @p filebuf.
 * @return true if the EPUB opened.
 * @since 0.1.0
 */
bool sh_sd_open_epub(const char* name, void* out_book, uint8_t* filebuf, size_t cap);

/** @brief The live RGB565 framebuffer ra_gfx scans (defined in main.c). */
uint16_t* sh_fb_pixels(void);

/** @brief Decode the open rabook @p base's 4bpp cover into thumbnail slot @p idx. */
void sh_decode_cover(uint16_t idx, const void* base);

/** @brief Decode the baked books' covers into ::sh_state_t::thumb at boot. */
void sh_shelf_build_thumbs(void* scratch, size_t scratch_len);

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

/** @brief Render the open book's chapter list (TOC). */
void sh_toc_render(void);

/** @brief Hit-test a TOC tap; returns the chapter index or -1. */
int32_t sh_toc_hit(int32_t x, int32_t y);

/** @brief Scroll the TOC list by @p dir pages (-1 up / +1 down), clamped. */
void sh_toc_scroll(int32_t dir);

/**
 * @brief Inflate a compressed RBKZ blob into @p scratch (miniz; defined in main.c).
 * @param[in]  src         Compressed container bytes.
 * @param[in]  len         Container length.
 * @param[in]  scratch     SDRAM work buffer.
 * @param[in]  scratch_len Capacity of @p scratch.
 * @param[out] out_base    Receives the validated inflated base.
 * @return true on success.
 * @since 0.1.0
 */
bool sh_open_compressed(const uint8_t* src,
                        uint32_t       len,
                        void*          scratch,
                        size_t         scratch_len,
                        const void**   out_base);

/**
 * @brief Resolve entry @p idx to its compressed RBKZ bytes (defined in main.c).
 * @details Baked entries return the MRAM pointer directly; SD entries are read
 *          from the card into the shared file buffer first.
 * @param[in]  idx     Shelf entry index (`< g_sh.book_count`).
 * @param[out] out_len Receives the byte length.
 * @return Pointer to the compressed bytes, or NULL on failure.
 * @since 0.1.0
 */
const uint8_t* sh_get_compressed(uint16_t idx, uint32_t* out_len);

/* ----- sh_book.c : format-agnostic book backend (rabook + epub) ------------ */

/**
 * @brief Open shelf entry @p idx by its format; sets g_sh.open_fmt + chapter
 *        count, and (for SD books on first open) the entry title/author/cover.
 * @param[in] idx         Shelf entry index.
 * @param[in] scratch     SDRAM work buffer (rabook inflate target).
 * @param[in] scratch_len Capacity of @p scratch.
 * @return true on success; false leaves no book open.
 * @since 0.1.0
 */
bool sh_book_open(uint16_t idx, void* scratch, size_t scratch_len);

/**
 * @brief Extract one chapter's plain text from the open book (either backend).
 * @param[in]  chapter Chapter index (`< g_sh.chapter_count`).
 * @param[out] out     Plain-text destination.
 * @param[in]  cap     Capacity of @p out.
 * @param[out] out_len Receives the text length.
 * @return k_ra_ok on success.
 * @since 0.1.0
 */
ra_err_t sh_book_chapter_text(uint32_t chapter, char* out, size_t cap, size_t* out_len);

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
