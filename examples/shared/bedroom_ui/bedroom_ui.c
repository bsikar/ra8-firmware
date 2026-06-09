/**
 * @file bedroom_ui.c
 * @brief Shared GUIX 3-tab bedroom UI (host + EK-RA8D2 panel)
 *
 * @deprecated GUIX retirement -- tracked by issue #81 (replaced by the
 * ra_reflow chrome renderer, issue #80); scheduled for wholesale removal.
 *
 * @par Tag
 * [Ring 5 / APP] {World: NS}
 *
 * @details
 * A small smart-room control surface: a top tab bar with one tab per
 * bedroom, and, per room, a heading plus a 2x2 grid of stat cards
 * (climate / lighting / security). Tab switching rides GUIX's own
 * GX_EVENT_CLICKED events (caught by the background window's handler), so
 * the same code reacts to host mouse clicks and on-panel touch alike.
 *
 * The layout is RESOLUTION-ADAPTIVE. The widget tree is laid out from the
 * runtime canvas size (read from the created GX_DISPLAY), not hardcoded
 * pixels, so the same source fits the 1024x600 EK-RA8D2 panel, the macOS
 * preview, and any smaller display the simulator describes -- without
 * clipping off-screen. Every dimension is derived proportionally from the
 * width / height with sensible minimums; the named ratios + floors below
 * are reproduced at the 1024x600 design size (the result there is the same
 * as the original absolute-pixel layout), then scaled down for smaller
 * panels. Where a panel is too short to show every text row, the layout
 * sheds the subtitle, then the per-card sub-status line, so what remains
 * always fits its box (GUIX clips each widget to its bounds).
 *
 * Fonts: GUIX consumes fixed-size bitmap glyphs and the bundled system
 * fonts are all ~18 px tall, so text cannot be smoothly downscaled on a
 * small panel; instead the text boxes shrink with the layout and GUIX
 * clips each string to its box rather than letting it spill across the
 * screen. The win here is LAYOUT that fits at any size; font crispness is
 * bounded by the available bitmap assets.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "bedroom_ui.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gx_api.h"
#include "ra_err.h"
#include "ra_panel.h"

/**
 * @enum bedroom_structure_t
 * @brief Fixed structural counts (panel-size independent).
 *
 * @details These describe the shape of the UI -- how many tabs, cards, grid
 *          columns, and text rows per card -- and never change with the panel
 *          resolution. The pixel geometry is derived separately at runtime
 *          (see ::bedroom_geom_t).
 *
 * @note Widget-storage array sizes key off these, so they are compile-time.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ui_tabs        = 3U,   /**< Tab / screen count.                 */
  k_ui_cards       = 4U,   /**< Stat cards per screen (2x2).        */
  k_ui_cols        = 2U,   /**< Card grid columns.                  */
  k_ui_rows        = 2U,   /**< Card grid rows.                     */
  k_ui_card_rows   = 3U,   /**< Text rows per card (label/val/sub). */
  k_ui_tab_id_base = 100U, /**< First tab button widget id.         */
} bedroom_structure_t;

/**
 * @enum bedroom_design_t
 * @brief Proportional design ratios + minimum floors for the adaptive layout.
 *
 * @details The geometry is computed as @c dim * NUM / DEN so the numbers below
 *          are the design fractions of the 1024-wide x 600-tall reference
 *          panel; evaluating them at 1024x600 reproduces the original
 *          absolute-pixel layout (e.g. tab bar 600*56/600 = 56 px, outer pad
 *          600*32/600 = 32 px). The @c *_MIN floors keep small panels usable
 *          when a pure ratio would round to an unreadably tiny value. Held as
 *          a typed enum (no value macros) so the layout stays constant-driven.
 *
 * @note @c k_ui_ref_w / @c k_ui_ref_h are the ratio denominators, NOT the
 *       active panel size -- the active size is read at runtime.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ui_ref_w        = 1024U, /**< Width  ratio denominator (design). */
  k_ui_ref_h        = 600U,  /**< Height ratio denominator (design). */
  k_ui_font_h       = 18U,   /**< System-font line height (px).      */
  k_ui_tabbar_num   = 56U,   /**< Tab-bar height: H*56/600.          */
  k_ui_tabbar_min   = 30U,   /**< Tab-bar height floor (font + pad).  */
  k_ui_pad_num      = 32U,   /**< Outer inset: min(W,H)*32/600.       */
  k_ui_pad_min      = 10U,   /**< Outer inset floor.                  */
  k_ui_head_gap_num = 14U,   /**< Tab-bar -> heading gap: H*14/600.   */
  k_ui_head_gap_min = 6U,    /**< Heading gap floor.                  */
  k_ui_head_num     = 34U,   /**< Heading row height: H*34/600.       */
  k_ui_sub_gap_num  = 6U,    /**< Heading -> subtitle gap: H*6/600.   */
  k_ui_sub_gap_min  = 2U,    /**< Subtitle gap floor.                 */
  k_ui_sub_num      = 24U,   /**< Subtitle row height: H*24/600.      */
  k_ui_grid_gap_num = 14U,   /**< Subtitle -> grid gap: H*14/600.     */
  k_ui_grid_gap_min = 6U,    /**< Grid-top gap floor.                 */
  k_ui_bot_num      = 28U,   /**< Grid bottom margin: H*28/600.       */
  k_ui_bot_min      = 8U,    /**< Bottom margin floor.                */
  k_ui_accent_num   = 6U,    /**< Card accent-strip height: H*6/600.  */
  k_ui_accent_min   = 3U,    /**< Accent-strip floor.                 */
  k_ui_inset_num    = 22U,   /**< Card text inset: W*22/1024.         */
  k_ui_inset_min    = 8U,    /**< Card text inset floor.              */
  k_ui_lbl_num      = 22U,   /**< Card label y: row_h*22/200.         */
  k_ui_lbl_h_num    = 24U,   /**< Card label box height: row_h*24/200. */
  k_ui_val_num      = 66U,   /**< Card value y: row_h*66/200.         */
  k_ui_val_h_num    = 40U,   /**< Card value box height: row_h*40/200. */
  k_ui_subv_num     = 120U,  /**< Card sub   y: row_h*120/200.        */
  k_ui_subv_h_num   = 24U,   /**< Card sub box height: row_h*24/200.   */
  k_ui_row_den      = 200U,  /**< Per-card text-row ratio denominator. */
  k_ui_row_min_gap  = 2U,    /**< Min vertical gap between text rows.   */
} bedroom_design_t;

/**
 * @struct bedroom_geom_t
 * @brief Concrete pixel geometry, computed once from the runtime canvas size.
 *
 * @details ::bedroom_compute_layout fills this from the active width / height;
 *          every widget rectangle is then placed from these fields instead of
 *          from compile-time constants, which is what makes the UI adapt to
 *          any panel. @c n_card_rows records how many of the three text rows
 *          fit a card at this size (3 normally, fewer on short panels), and
 *          @c show_subtitle whether the room subtitle band fits.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t w;             /**< Active canvas width  (px).            */
  uint16_t h;             /**< Active canvas height (px).            */
  uint16_t tabbar_h;      /**< Tab-bar height.                       */
  uint16_t pad;           /**< Outer content inset.                  */
  uint16_t gap;           /**< Gap between cards.                    */
  uint16_t head_y;        /**< Heading top edge.                     */
  uint16_t head_h;        /**< Heading row height.                   */
  uint16_t sub_y;         /**< Subtitle top edge.                    */
  uint16_t sub_h;         /**< Subtitle row height.                  */
  uint16_t grid_top;      /**< Card grid top edge.                   */
  uint16_t col_w;         /**< Card column width.                    */
  uint16_t row_h;         /**< Card row height.                      */
  uint16_t accent_h;      /**< Card top accent-strip height.         */
  uint16_t card_inset;    /**< Text inset inside a card.             */
  uint16_t lbl_dy;        /**< Card label y, below card top.         */
  uint16_t lbl_h;         /**< Card label box height.                */
  uint16_t val_dy;        /**< Card value y, below card top.         */
  uint16_t val_h;         /**< Card value box height.                */
  uint16_t subv_dy;       /**< Card sub   y, below card top.         */
  uint16_t subv_h;        /**< Card sub box height.                  */
  uint16_t n_card_rows;   /**< Text rows that fit a card (1..3).     */
  bool     show_subtitle; /**< Whether the room subtitle band fits.  */
} bedroom_geom_t;

/**
 * @enum bedroom_color_id_t
 * @brief Indices into our 32-entry GUIX colour table.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_col_bg          = 1U,  /**< Page backdrop (behind everything).  */
  k_col_tab_on      = 2U,  /**< Active tab fill.                    */
  k_col_tab_off     = 3U,  /**< Inactive tab fill.                  */
  k_col_tab_txt_on  = 4U,  /**< Active tab text.                    */
  k_col_tab_txt_off = 5U,  /**< Inactive tab text.                  */
  k_col_heading     = 6U,  /**< Room heading text.                  */
  k_col_subtitle    = 7U,  /**< Room subtitle text.                 */
  k_col_card        = 8U,  /**< Card fill.                          */
  k_col_label       = 9U,  /**< Card label / sub text.              */
  k_col_acc_temp    = 10U, /**< Temperature accent + value.         */
  k_col_acc_light   = 11U, /**< Lighting accent + value.            */
  k_col_acc_humid   = 12U, /**< Humidity accent + value.            */
  k_col_acc_sec     = 13U, /**< Security accent + value.            */
  k_col_acc_warn    = 14U, /**< Attention accent + value.           */
  k_col_screen0     = 15U, /**< Screen 0 backdrop.                  */
  k_col_screen1     = 16U, /**< Screen 1 backdrop.                  */
  k_col_screen2     = 17U, /**< Screen 2 backdrop.                  */
  k_col_table_len   = 32U, /**< Colour table length.                */
} bedroom_color_id_t;

/**
 * @enum bedroom_font_id_t
 * @brief Indices into our GUIX font table.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_font_none = 0U, /**< Slot 0 reserved (GUIX default).        */
  k_font_txt  = 1U, /**< 8bpp anti-aliased system font (all).   */
  k_font_len  = 2U, /**< Font table length.                     */
} bedroom_font_id_t;

/**
 * @enum bedroom_pack565_t
 * @brief Masks/shifts to pack 0xRRGGBB into a native 565 pixel.
 *
 * @details The 565rgb driver consumes colour-table entries directly as native
 *          pixels (GUIX Studio ships 16bpp tables pre-packed), so we pack our
 *          readable design colours the same way its native_color_get does.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_pack_mask_r  = 0x00F80000UL, /**< Top 5 red bits.   */
  k_pack_mask_g  = 0x0000FC00UL, /**< Top 6 green bits. */
  k_pack_mask_b  = 0x000000FFUL, /**< Top 5 blue bits.  */
  k_pack_shift_r = 8UL,          /**< Red   >> 8.       */
  k_pack_shift_g = 5UL,          /**< Green >> 5.       */
  k_pack_shift_b = 3UL,          /**< Blue  >> 3.       */
} bedroom_pack565_t;

/**
 * @struct bedroom_card_t
 * @brief One stat card's static content.
 *
 * @since 0.1.0
 */
typedef struct {
  const char* label;  /**< Small upper label (e.g. "TEMPERATURE"). */
  const char* value;  /**< Large value (e.g. "72 F").              */
  const char* sub;    /**< Small sub-status line.                  */
  uint8_t     accent; /**< Accent colour id (strip + value).       */
} bedroom_card_t;

/* Module-static widget storage -- one UI per process (NASA P10 Rule 3). */
static GX_WINDOW_ROOT* s_root;
static GX_WINDOW       s_bg;
static GX_TEXT_BUTTON  s_tab[k_ui_tabs];
static GX_WINDOW       s_screen[k_ui_tabs];
static GX_PROMPT       s_heading[k_ui_tabs];
static GX_PROMPT       s_subtitle[k_ui_tabs];
static GX_WINDOW       s_card[k_ui_tabs][k_ui_cards];
static GX_WINDOW       s_accent[k_ui_tabs][k_ui_cards];
static GX_PROMPT       s_lbl[k_ui_tabs][k_ui_cards];
static GX_PROMPT       s_val[k_ui_tabs][k_ui_cards];
static GX_PROMPT       s_sub[k_ui_tabs][k_ui_cards];
static uint16_t        s_active;

/** @brief Pixel geometry for the active panel (filled by compute_layout). */
static bedroom_geom_t s_geom;

static GX_COLOR s_color_table[k_col_table_len];
static GX_FONT* s_font_table[k_font_len];

/** @brief Per-tab captions (also the screen headings). */
static const char* const s_names[k_ui_tabs] = {
  "Primary Bedroom",
  "Secondary Bedroom 1",
  "Secondary Bedroom 2",
};

/** @brief Per-screen subtitle. */
static const char* const s_subtitles[k_ui_tabs] = {
  "Climate  /  Lighting  /  Security",
  "Climate  /  Lighting  /  Security",
  "Climate  /  Lighting  /  Security",
};

/** @brief Per-screen backdrop colour ids. */
static const GX_RESOURCE_ID s_screen_bg[k_ui_tabs] = {
  (GX_RESOURCE_ID)k_col_screen0,
  (GX_RESOURCE_ID)k_col_screen1,
  (GX_RESOURCE_ID)k_col_screen2,
};

/** @brief Card content per room (2x2: temp, lights, humidity, security). */
static const bedroom_card_t s_cards[k_ui_tabs][k_ui_cards] = {
  {
    {"TEMPERATURE", "72 F", "Set to 70", k_col_acc_temp},
    {"LIGHTS", "On", "60% brightness", k_col_acc_light},
    {"HUMIDITY", "44%", "Comfortable", k_col_acc_humid},
    {"DOOR LOCK", "Secured", "Auto-lock on", k_col_acc_sec},
  },
  {
    {"TEMPERATURE", "70 F", "Set to 70", k_col_acc_temp},
    {"LIGHTS", "Off", "Tap to turn on", k_col_acc_light},
    {"HUMIDITY", "47%", "Comfortable", k_col_acc_humid},
    {"DOOR LOCK", "Secured", "Auto-lock on", k_col_acc_sec},
  },
  {
    {"TEMPERATURE", "73 F", "Set to 71", k_col_acc_temp},
    {"LIGHTS", "On", "20% brightness", k_col_acc_light},
    {"HUMIDITY", "41%", "A bit dry", k_col_acc_humid},
    {"WINDOW", "Open", "Since 3:40 PM", k_col_acc_warn},
  },
};

/**
 * @brief Wrap a C string as a GX_STRING.
 *
 * @param[in] s NUL-terminated string.
 * @return GX_STRING pointing at @p s with its byte length.
 */
static GX_STRING bedroom_str(const char* s)
{
  GX_STRING g;
  g.gx_string_ptr    = s;
  g.gx_string_length = (UINT)strlen(s);
  return g;
}

/**
 * @brief Pack a 0xRRGGBB design colour into a native 565 pixel.
 *
 * @param[in] rgb888 Colour as 0x00RRGGBB.
 * @return GX_COLOR Native 16-bit 565 value for the colour table.
 */
static GX_COLOR bedroom_pack565(GX_COLOR rgb888)
{
  return (GX_COLOR)(((rgb888 & (GX_COLOR)k_pack_mask_r) >> (GX_COLOR)k_pack_shift_r) |
                    ((rgb888 & (GX_COLOR)k_pack_mask_g) >> (GX_COLOR)k_pack_shift_g) |
                    ((rgb888 & (GX_COLOR)k_pack_mask_b) >> (GX_COLOR)k_pack_shift_b));
}

/**
 * @brief Larger of two unsigned values (a min-floor helper for the layout).
 *
 * @param[in] a First value.
 * @param[in] b Second value.
 *
 * @return uint16_t @p a if @p a >= @p b, else @p b.
 * @retval a When @p a is the larger (or equal).
 * @retval b When @p b is strictly larger.
 *
 * @note Pure, side-effect free; lets the proportional math clamp to a floor.
 * @since 0.1.0
 */
static uint16_t bedroom_u16_max(uint16_t a, uint16_t b)
{
  return (a >= b) ? a : b;
}

/**
 * @brief Clamp an unsigned value into the inclusive range [lo, hi].
 *
 * @param[in] v  Value to clamp.
 * @param[in] lo Lower bound.
 * @param[in] hi Upper bound.
 *
 * @return uint16_t @p v limited to [@p lo, @p hi]; @p lo if @p lo > @p hi.
 * @retval lo When @p v < @p lo, or the range is inverted.
 * @retval hi When @p v > @p hi.
 *
 * @note Inverted ranges (@p lo > @p hi) collapse to @p lo so a row that
 *       cannot fit is pinned to its floor; callers test fit separately.
 * @since 0.1.0
 */
static uint16_t bedroom_u16_clamp(uint16_t v, uint16_t lo, uint16_t hi)
{
  if (lo > hi) {
    return lo;
  }
  if (v < lo) {
    return lo;
  }
  return (v > hi) ? hi : v;
}

/**
 * @brief Scale a dimension by a design ratio, floored to a minimum.
 *
 * @param[in] dim Active dimension (width or height) in pixels.
 * @param[in] num Ratio numerator (design value at the reference size).
 * @param[in] den Ratio denominator (reference width or height).
 * @param[in] lo  Minimum result so small panels stay usable.
 *
 * @return uint16_t max(@p dim * @p num / @p den, @p lo).
 * @retval lo When the scaled value rounds below the floor.
 *
 * @note Integer math (truncating divide); evaluating at the reference size
 *       reproduces the original absolute-pixel design value.
 * @since 0.1.0
 */
static uint16_t bedroom_scale(uint16_t dim, uint16_t num, uint16_t den, uint16_t lo)
{
  const uint32_t v = ((uint32_t)dim * (uint32_t)num) / (uint32_t)den;
  return bedroom_u16_max((uint16_t)v, lo);
}

/**
 * @brief Compute the vertical bands + card grid metrics from the canvas size.
 *
 * @param[in,out] g Geometry block; @c w / @c h must be set on entry, the band
 *                  and grid fields are filled on return.
 *
 * @return void
 * @retval void Always; @p g is updated in place.
 *
 * @details Places the tab bar, heading, and (space permitting) subtitle from
 *          the top, then gives the remaining height to a 2-row card grid. If
 *          the subtitle would starve the grid below a two-text-row minimum the
 *          subtitle is dropped and the grid moves up under the heading.
 *
 * @pre @p g is non-NULL with @c w > 0 and @c h > 0.
 * @post All band fields and @c col_w / @c row_h / @c show_subtitle are set.
 * @note Floors keep every band visible on small panels.
 * @since 0.1.0
 */
static void bedroom_layout_bands(bedroom_geom_t* g)
{
  g->tabbar_h            = bedroom_scale(g->h, k_ui_tabbar_num, k_ui_ref_h, k_ui_tabbar_min);
  const uint16_t shorter = (g->w < g->h) ? g->w : g->h;
  g->pad                 = bedroom_scale(shorter, k_ui_pad_num, k_ui_ref_h, k_ui_pad_min);
  g->gap                 = bedroom_u16_max((uint16_t)((uint32_t)g->pad * 3U / 4U), 8U);

  const uint16_t head_gap = bedroom_scale(g->h, k_ui_head_gap_num, k_ui_ref_h, k_ui_head_gap_min);
  g->head_y               = (uint16_t)(g->tabbar_h + head_gap);
  g->head_h               = bedroom_scale(g->h, k_ui_head_num, k_ui_ref_h, k_ui_font_h);
  const uint16_t sub_gap  = bedroom_scale(g->h, k_ui_sub_gap_num, k_ui_ref_h, k_ui_sub_gap_min);
  g->sub_y                = (uint16_t)(g->head_y + g->head_h + sub_gap);
  g->sub_h                = bedroom_scale(g->h, k_ui_sub_num, k_ui_ref_h, k_ui_font_h);

  const uint16_t grid_gap = bedroom_scale(g->h, k_ui_grid_gap_num, k_ui_ref_h, k_ui_grid_gap_min);
  const uint16_t bot      = bedroom_scale(g->h, k_ui_bot_num, k_ui_ref_h, k_ui_bot_min);
  const uint16_t accent   = bedroom_scale(g->h, k_ui_accent_num, k_ui_ref_h, k_ui_accent_min);
  /* Subtitle is worth keeping only if each of the 2 card rows can still show
   * at least its label + value (two font lines) plus the accent strip. */
  const uint16_t min_card = (uint16_t)(accent + (2U * k_ui_font_h) + 14U);
  const uint16_t grid_sub = (uint16_t)(g->sub_y + g->sub_h + grid_gap);
  const int32_t  avail_s  = (int32_t)g->h - (int32_t)bot - (int32_t)grid_sub - (int32_t)g->gap;

  if (avail_s >= (int32_t)(k_ui_rows * min_card)) {
    g->show_subtitle = true;
    g->grid_top      = grid_sub;
  } else {
    g->show_subtitle = false;
    g->grid_top      = (uint16_t)(g->head_y + g->head_h + grid_gap);
  }
  const int32_t avail = (int32_t)g->h - (int32_t)bot - (int32_t)g->grid_top - (int32_t)g->gap;
  g->col_w            = (uint16_t)(((uint32_t)g->w - (2U * g->pad) - g->gap) / k_ui_cols);
  g->row_h            = (uint16_t)((avail > 0 ? (uint32_t)avail : 0U) / k_ui_rows);
}

/**
 * @brief Compute per-card accent, inset, and text-row offsets that fit a card.
 *
 * @param[in,out] g Geometry block; the band/grid fields must be set on entry,
 *                  the card-text fields are filled on return.
 *
 * @return void
 * @retval void Always; @p g is updated in place.
 *
 * @details Each row's top offset and box height come from the design fractions
 *          of the card row height, so at 1024x600 the boxes match the original
 *          (label/sub 24 px, value 40 px) and GUIX centres the same text at the
 *          same y. Box heights floor at one font line; a row whose centred text
 *          would fall past the card bottom is shed via @c n_card_rows (sub
 *          before value), so a short card shows label + value cleanly.
 *
 * @pre @p g has @c row_h set (from bedroom_layout_bands).
 * @post @c accent_h, @c card_inset, the three @c *_dy / @c *_h fields, and
 *       @c n_card_rows are set.
 * @note GUIX clips each text widget to its box, so any residual overflow is
 *       trimmed to the card, never drawn across the screen.
 * @since 0.1.0
 */
static void bedroom_layout_text_rows(bedroom_geom_t* g)
{
  g->accent_h          = bedroom_scale(g->h, k_ui_accent_num, k_ui_ref_h, k_ui_accent_min);
  g->card_inset        = bedroom_scale(g->w, k_ui_inset_num, k_ui_ref_w, k_ui_inset_min);
  const uint16_t rh    = g->row_h;
  const uint16_t floor = (uint16_t)(g->accent_h + 2U);

  g->lbl_h  = bedroom_scale(rh, k_ui_lbl_h_num, k_ui_row_den, k_ui_font_h);
  g->val_h  = bedroom_scale(rh, k_ui_val_h_num, k_ui_row_den, k_ui_font_h);
  g->subv_h = bedroom_scale(rh, k_ui_subv_h_num, k_ui_row_den, k_ui_font_h);

  g->lbl_dy  = bedroom_u16_max(bedroom_scale(rh, k_ui_lbl_num, k_ui_row_den, floor), floor);
  g->val_dy  = bedroom_u16_max(bedroom_scale(rh, k_ui_val_num, k_ui_row_den, 0U),
                               (uint16_t)(g->lbl_dy + g->lbl_h + k_ui_row_min_gap));
  g->subv_dy = bedroom_u16_max(bedroom_scale(rh, k_ui_subv_num, k_ui_row_den, 0U),
                               (uint16_t)(g->val_dy + g->val_h + k_ui_row_min_gap));

  /* Shed a row if its first text line would start past the card bottom. */
  uint16_t n_rows = k_ui_card_rows;
  if ((uint16_t)(g->subv_dy + k_ui_font_h) > rh) {
    n_rows = 2U;
  }
  if ((uint16_t)(g->val_dy + k_ui_font_h) > rh) {
    n_rows = 1U;
  }
  g->n_card_rows = n_rows;
}

/**
 * @brief Compute the whole adaptive layout into ::s_geom for a canvas size.
 *
 * @param[in] w Active canvas width  (px).
 * @param[in] h Active canvas height (px).
 *
 * @return void
 * @retval void Always; the module-static ::s_geom is updated.
 *
 * @details Single entry point that derives every pixel rectangle the widget
 *          builders consume from the runtime panel size, so the same source
 *          fits any display. Delegates to bedroom_layout_bands (vertical bands
 *          + card grid) and bedroom_layout_text_rows (per-card text), keeping
 *          each step within the function-size budget.
 *
 * @pre @p w > 0 and @p h > 0 (a created canvas guarantees this).
 * @post ::s_geom fully describes the layout for (@p w, @p h).
 * @note Recomputing for a new size before rebuilding is safe; no widget state
 *       is touched here.
 * @since 0.1.0
 */
static void bedroom_compute_layout(uint16_t w, uint16_t h)
{
  s_geom.w = w;
  s_geom.h = h;
  bedroom_layout_bands(&s_geom);
  bedroom_layout_text_rows(&s_geom);
}

/**
 * @brief Install the colour + font resource tables on the display.
 *
 * @param[in] display GUIX display.
 * @return UINT GX_SUCCESS or the first failing GUIX status.
 */
static UINT bedroom_install_resources(GX_DISPLAY* display)
{
  for (uint16_t i = 0U; i < (uint16_t)k_col_table_len; i++) {
    s_color_table[i] = 0x00000000UL;
  }
  s_color_table[k_col_bg]          = 0x00121821UL;
  s_color_table[k_col_tab_on]      = 0x00FFFFFFUL;
  s_color_table[k_col_tab_off]     = 0x003A4250UL;
  s_color_table[k_col_tab_txt_on]  = 0x001B2433UL;
  s_color_table[k_col_tab_txt_off] = 0x00C3C9D4UL;
  s_color_table[k_col_heading]     = 0x001B2433UL;
  s_color_table[k_col_subtitle]    = 0x00707A88UL;
  s_color_table[k_col_card]        = 0x00FFFFFFUL;
  s_color_table[k_col_label]       = 0x008A93A1UL;
  s_color_table[k_col_acc_temp]    = 0x00E3733AUL;
  s_color_table[k_col_acc_light]   = 0x00D79A12UL;
  s_color_table[k_col_acc_humid]   = 0x003E84C4UL;
  s_color_table[k_col_acc_sec]     = 0x00379A63UL;
  s_color_table[k_col_acc_warn]    = 0x00D1503FUL;
  s_color_table[k_col_screen0]     = 0x00F2EEE6UL;
  s_color_table[k_col_screen1]     = 0x00EAEFF4UL;
  s_color_table[k_col_screen2]     = 0x00EAF2ECUL;

  /* The 565rgb driver uses table entries as native pixels; pack to 565. */
  for (uint16_t i = 0U; i < (uint16_t)k_col_table_len; i++) {
    s_color_table[i] = bedroom_pack565(s_color_table[i]);
  }

  UINT status = gx_display_color_table_set(display, s_color_table, (UINT)k_col_table_len);
  if (status != GX_SUCCESS) {
    return status;
  }

  /* NOLINTBEGIN(bugprone-reserved-identifier) -- vendor font symbol. */
  extern GX_CONST GX_FONT _gx_system_font_8bpp;
  /* NOLINTEND(bugprone-reserved-identifier) */
  s_font_table[k_font_none] = GX_NULL;
  s_font_table[k_font_txt]  = (GX_FONT*)&_gx_system_font_8bpp;
  return gx_display_font_table_set(display, s_font_table, (UINT)k_font_len);
}

/**
 * @brief Create a transparent text label inside a parent widget.
 *
 * @param[in] p      Prompt control block.
 * @param[in] parent Parent widget.
 * @param[in] r      Rectangle (absolute).
 * @param[in] text   Static text to show.
 * @param[in] font   Font resource id.
 * @param[in] color  Text colour id.
 * @return UINT GX_SUCCESS or the first failing GUIX status.
 */
static UINT bedroom_make_label(GX_PROMPT*     p,
                               GX_WIDGET*     parent,
                               GX_RECTANGLE   r,
                               const char*    text,
                               GX_RESOURCE_ID font,
                               GX_RESOURCE_ID color)
{
  UINT status = gx_prompt_create(p,
                                 "label",
                                 parent,
                                 0,
                                 GX_STYLE_TEXT_LEFT | GX_STYLE_ENABLED | GX_STYLE_TRANSPARENT,
                                 GX_ID_NONE,
                                 &r);
  if (status != GX_SUCCESS) {
    return status;
  }
  GX_STRING g = bedroom_str(text);
  status      = gx_prompt_text_set_ext(p, &g);
  if (status != GX_SUCCESS) {
    return status;
  }
  status = gx_prompt_font_set(p, font);
  if (status != GX_SUCCESS) {
    return status;
  }
  return gx_prompt_text_color_set(p, color, color, color);
}

/**
 * @brief Create a solid-fill child window (card body or accent strip).
 *
 * @param[in] w      Window control block.
 * @param[in] parent Parent widget.
 * @param[in] r      Rectangle (absolute).
 * @param[in] fill   Fill colour id.
 * @return UINT GX_SUCCESS or the first failing GUIX status.
 */
static UINT bedroom_make_panel(GX_WINDOW* w, GX_WIDGET* parent, GX_RECTANGLE r, GX_RESOURCE_ID fill)
{
  UINT status = gx_window_create(w, "panel", parent, GX_STYLE_ENABLED, GX_ID_NONE, &r);
  if (status != GX_SUCCESS) {
    return status;
  }
  return gx_widget_fill_color_set(w, fill, fill, fill);
}

/**
 * @brief Create the three tab buttons across the top bar.
 *
 * @details Splits the active canvas width into ::k_ui_tabs equal buttons across
 *          the adaptive tab-bar height (::s_geom), the last one stretched to the
 *          right edge so rounding never leaves a sliver column.
 *
 * @return UINT GX_SUCCESS or the first failing GUIX status.
 * @retval GX_SUCCESS All tab buttons created and styled.
 *
 * @pre ::s_geom has been filled by bedroom_compute_layout and ::s_bg exists.
 * @post ::s_tab[0..k_ui_tabs) are created as children of ::s_bg.
 * @note Tab fill/text colours are applied later by bedroom_ui_set_active_tab.
 * @since 0.1.0
 */
static UINT bedroom_create_tabs(void)
{
  const UINT tabw = (UINT)s_geom.w / (UINT)k_ui_tabs;
  for (uint16_t i = 0U; i < (uint16_t)k_ui_tabs; i++) {
    const GX_VALUE x0 = (GX_VALUE)((UINT)i * tabw);
    const GX_VALUE x1 =
      (GX_VALUE)((i == (uint16_t)(k_ui_tabs - 1U)) ? ((UINT)s_geom.w - 1U)
                                                   : (((UINT)i + 1U) * tabw - 1U));
    GX_RECTANGLE r;
    gx_utility_rectangle_define(&r, x0, 0, x1, (GX_VALUE)(s_geom.tabbar_h - 1U));

    UINT status = gx_text_button_create(&s_tab[i],
                                        "tab",
                                        &s_bg,
                                        0,
                                        GX_STYLE_ENABLED | GX_STYLE_TEXT_CENTER,
                                        (USHORT)((UINT)k_ui_tab_id_base + (UINT)i),
                                        &r);
    if (status != GX_SUCCESS) {
      return status;
    }
    GX_STRING str = bedroom_str(s_names[i]);
    status        = gx_text_button_text_set_ext(&s_tab[i], &str);
    if (status != GX_SUCCESS) {
      return status;
    }
    status = gx_text_button_font_set(&s_tab[i], (GX_RESOURCE_ID)k_font_txt);
    if (status != GX_SUCCESS) {
      return status;
    }
  }
  return GX_SUCCESS;
}

/**
 * @brief Compute one card's rectangle in the adaptive 2-column grid.
 *
 * @param[in]  i   Card index in [0, k_ui_cards).
 * @param[out] out Card rectangle (canvas-relative pixels).
 *
 * @return void
 * @retval void Always; @p out receives the rectangle.
 *
 * @details Reads the precomputed column width, row height, outer pad, gap, and
 *          grid top from ::s_geom, so the grid scales with the panel and never
 *          extends past the canvas bounds.
 *
 * @pre ::s_geom has been filled by bedroom_compute_layout; @p out is non-NULL.
 * @post @p out holds card @p i 's rectangle within [0,w) x [0,h).
 * @note Column-major-by-row index: i = row * k_ui_cols + col.
 * @since 0.1.0
 */
static void bedroom_card_bounds(uint16_t i, GX_RECTANGLE* out)
{
  const UINT col = (UINT)i % (UINT)k_ui_cols;
  const UINT row = (UINT)i / (UINT)k_ui_cols;
  const UINT x0  = (UINT)s_geom.pad + (col * ((UINT)s_geom.col_w + (UINT)s_geom.gap));
  const UINT y0  = (UINT)s_geom.grid_top + (row * ((UINT)s_geom.row_h + (UINT)s_geom.gap));
  gx_utility_rectangle_define(out,
                              (GX_VALUE)x0,
                              (GX_VALUE)y0,
                              (GX_VALUE)(x0 + (UINT)s_geom.col_w - 1U),
                              (GX_VALUE)(y0 + (UINT)s_geom.row_h - 1U));
}

/**
 * @brief Create a card's body panel and its top accent strip.
 *
 * @param[in] room Room index.
 * @param[in] i    Card index.
 * @param[in] card Card rectangle from bedroom_card_bounds.
 * @return UINT GX_SUCCESS or the first failing GUIX status.
 */
static UINT bedroom_create_card_frame(uint16_t room, uint16_t i, const GX_RECTANGLE* card)
{
  UINT status = bedroom_make_panel(&s_card[room][i],
                                   (GX_WIDGET*)&s_screen[room],
                                   *card,
                                   (GX_RESOURCE_ID)k_col_card);
  if (status != GX_SUCCESS) {
    return status;
  }
  GX_RECTANGLE strip;
  gx_utility_rectangle_define(&strip,
                              card->gx_rectangle_left,
                              card->gx_rectangle_top,
                              card->gx_rectangle_right,
                              (GX_VALUE)(card->gx_rectangle_top + (GX_VALUE)s_geom.accent_h - 1));
  return bedroom_make_panel(&s_accent[room][i],
                            (GX_WIDGET*)&s_card[room][i],
                            strip,
                            (GX_RESOURCE_ID)s_cards[room][i].accent);
}

/**
 * @brief Create a card's text rows (label, value, and sub-status if it fits).
 *
 * @param[in] room Room index.
 * @param[in] i    Card index.
 * @param[in] card Card rectangle from bedroom_card_bounds.
 *
 * @return UINT GX_SUCCESS or the first failing GUIX status.
 * @retval GX_SUCCESS The visible text rows were created.
 *
 * @details Places each row at its adaptive offset from ::s_geom and creates
 *          only the rows that fit this card height (::bedroom_geom_t::n_card_rows
 *          is 3 on roomy panels, fewer on short ones). Each text box bottom is
 *          clamped to the card so GUIX clips an overlong string to its box
 *          instead of drawing it across the screen.
 *
 * @pre ::s_geom is filled and the card frame (::s_card[room][i]) exists.
 * @post Up to three prompts are created as children of the card window.
 * @note Hidden rows leave their GX_PROMPT control blocks uninitialised; they
 *       are never attached, so GUIX never visits them.
 * @since 0.1.0
 */
static UINT bedroom_create_card_text(uint16_t room, uint16_t i, const GX_RECTANGLE* card)
{
  const bedroom_card_t* c   = &s_cards[room][i];
  const GX_VALUE        y0  = card->gx_rectangle_top;
  const GX_VALUE        bot = card->gx_rectangle_bottom;
  const GX_VALUE        lx  = (GX_VALUE)(card->gx_rectangle_left + (GX_VALUE)s_geom.card_inset);
  const GX_VALUE        rx  = (GX_VALUE)(card->gx_rectangle_right - (GX_VALUE)s_geom.card_inset);
  const struct {
    GX_PROMPT*     widget;
    GX_VALUE       dy;
    GX_VALUE       h;
    const char*    text;
    GX_RESOURCE_ID color;
  } rows[k_ui_card_rows] = {
    {&s_lbl[room][i],
     (GX_VALUE)s_geom.lbl_dy,
     (GX_VALUE)s_geom.lbl_h,
     c->label,
     (GX_RESOURCE_ID)k_col_label},
    {&s_val[room][i],
     (GX_VALUE)s_geom.val_dy,
     (GX_VALUE)s_geom.val_h,
     c->value,
     (GX_RESOURCE_ID)c->accent},
    {&s_sub[room][i],
     (GX_VALUE)s_geom.subv_dy,
     (GX_VALUE)s_geom.subv_h,
     c->sub,
     (GX_RESOURCE_ID)k_col_label},
  };
  for (uint16_t k = 0U; k < s_geom.n_card_rows; k++) {
    GX_VALUE     top = (GX_VALUE)(y0 + rows[k].dy);
    GX_VALUE     end = (GX_VALUE)(top + rows[k].h);
    GX_RECTANGLE r;
    gx_utility_rectangle_define(&r, lx, top, rx, (end > bot) ? bot : end);
    const UINT status = bedroom_make_label(rows[k].widget,
                                           (GX_WIDGET*)&s_card[room][i],
                                           r,
                                           rows[k].text,
                                           (GX_RESOURCE_ID)k_font_txt,
                                           rows[k].color);
    if (status != GX_SUCCESS) {
      return status;
    }
  }
  return GX_SUCCESS;
}

/**
 * @brief Create one stat card (body, accent strip, label, value, sub).
 *
 * @param[in] room Room index.
 * @param[in] i    Card index in [0, k_ui_cards).
 * @return UINT GX_SUCCESS or the first failing GUIX status.
 */
static UINT bedroom_create_card(uint16_t room, uint16_t i)
{
  GX_RECTANGLE card;
  bedroom_card_bounds(i, &card);
  const UINT status = bedroom_create_card_frame(room, i, &card);
  if (status != GX_SUCCESS) {
    return status;
  }
  return bedroom_create_card_text(room, i, &card);
}

/**
 * @brief Create a screen's heading and (if it fits) its subtitle band.
 *
 * @param[in] room Room index.
 *
 * @return UINT GX_SUCCESS or the first failing GUIX status.
 * @retval GX_SUCCESS The heading (and subtitle when shown) were created.
 *
 * @details Both rows use the adaptive offsets in ::s_geom. The subtitle is
 *          created only when ::bedroom_geom_t::show_subtitle is set; on a panel
 *          too short for it the band is skipped and the card grid takes the
 *          reclaimed height (decided in bedroom_layout_bands).
 *
 * @pre ::s_geom is filled and ::s_screen[room] exists.
 * @post ::s_heading[room] (and ::s_subtitle[room] when shown) are created.
 * @note A hidden subtitle's GX_PROMPT is left unused, never attached.
 * @since 0.1.0
 */
static UINT bedroom_create_screen_header(uint16_t room)
{
  const UINT   lx = (UINT)s_geom.pad;
  const UINT   rx = (UINT)s_geom.w - 1U - (UINT)s_geom.pad;
  GX_RECTANGLE r;
  gx_utility_rectangle_define(&r,
                              (GX_VALUE)lx,
                              (GX_VALUE)s_geom.head_y,
                              (GX_VALUE)rx,
                              (GX_VALUE)((UINT)s_geom.head_y + (UINT)s_geom.head_h));
  UINT status = bedroom_make_label(&s_heading[room],
                                   (GX_WIDGET*)&s_screen[room],
                                   r,
                                   s_names[room],
                                   (GX_RESOURCE_ID)k_font_txt,
                                   (GX_RESOURCE_ID)k_col_heading);
  if ((status != GX_SUCCESS) || !s_geom.show_subtitle) {
    return status;
  }
  gx_utility_rectangle_define(&r,
                              (GX_VALUE)lx,
                              (GX_VALUE)s_geom.sub_y,
                              (GX_VALUE)rx,
                              (GX_VALUE)((UINT)s_geom.sub_y + (UINT)s_geom.sub_h));
  return bedroom_make_label(&s_subtitle[room],
                            (GX_WIDGET*)&s_screen[room],
                            r,
                            s_subtitles[room],
                            (GX_RESOURCE_ID)k_font_txt,
                            (GX_RESOURCE_ID)k_col_subtitle);
}

/**
 * @brief Create one room screen: backdrop, heading, subtitle, card grid.
 *
 * @param[in] room Room index.
 *
 * @return UINT GX_SUCCESS or the first failing GUIX status.
 * @retval GX_SUCCESS The screen and all its children were created.
 *
 * @details The backdrop fills the canvas below the adaptive tab bar; the header
 *          and the 2x2 card grid are then placed from ::s_geom, so the whole
 *          screen scales to the panel without clipping.
 *
 * @pre ::s_geom is filled and ::s_bg exists.
 * @post ::s_screen[room] and its heading / subtitle / cards are created.
 * @note Screens are created hidden; bedroom_ui_set_active_tab shows one.
 * @since 0.1.0
 */
static UINT bedroom_create_screen(uint16_t room)
{
  GX_RECTANGLE r;
  gx_utility_rectangle_define(&r,
                              0,
                              (GX_VALUE)s_geom.tabbar_h,
                              (GX_VALUE)(s_geom.w - 1U),
                              (GX_VALUE)(s_geom.h - 1U));
  UINT status = bedroom_make_panel(&s_screen[room], (GX_WIDGET*)&s_bg, r, s_screen_bg[room]);
  if (status != GX_SUCCESS) {
    return status;
  }
  status = bedroom_create_screen_header(room);
  if (status != GX_SUCCESS) {
    return status;
  }
  for (uint16_t i = 0U; i < (uint16_t)k_ui_cards; i++) {
    status = bedroom_create_card(room, i);
    if (status != GX_SUCCESS) {
      return status;
    }
  }
  return GX_SUCCESS;
}

/**
 * @brief Background-window event handler: catch tab clicks, switch screens.
 *
 * @param[in] widget The background window.
 * @param[in] ev     Incoming GUIX event.
 * @return UINT GUIX status from the default window processing.
 */
static UINT bedroom_bg_event(GX_WIDGET* widget, GX_EVENT* ev)
{
  /* Button clicks arrive as a packed GX_SIGNAL(widget_id, GX_EVENT_CLICKED);
   * mask off the id to read the event code, and take the id from the sender. */
  if ((ev->gx_event_type & GX_SIGNAL_EVENT_MASK) == GX_EVENT_CLICKED) {
    const USHORT id = ev->gx_event_sender;
    if ((id >= (USHORT)k_ui_tab_id_base) &&
        (id < (USHORT)((UINT)k_ui_tab_id_base + (UINT)k_ui_tabs))) {
      bedroom_ui_set_active_tab((uint16_t)((UINT)id - (UINT)k_ui_tab_id_base));
    }
  }
  return gx_window_event_process((GX_WINDOW*)widget, ev);
}

uint16_t bedroom_ui_tab_count(void)
{
  return (uint16_t)k_ui_tabs;
}

void bedroom_ui_set_active_tab(uint16_t tab)
{
  if (tab >= (uint16_t)k_ui_tabs) {
    return;
  }
  s_active = tab;
  for (uint16_t i = 0U; i < (uint16_t)k_ui_tabs; i++) {
    const GX_RESOURCE_ID fill =
      (i == tab) ? (GX_RESOURCE_ID)k_col_tab_on : (GX_RESOURCE_ID)k_col_tab_off;
    const GX_RESOURCE_ID text =
      (i == tab) ? (GX_RESOURCE_ID)k_col_tab_txt_on : (GX_RESOURCE_ID)k_col_tab_txt_off;
    (void)gx_widget_fill_color_set(&s_tab[i], fill, fill, fill);
    (void)gx_text_button_text_color_set(&s_tab[i], text, text, text);
    (void)gx_system_dirty_mark(&s_tab[i]);
    if (i == tab) {
      (void)gx_widget_show(&s_screen[i]);
    } else {
      (void)gx_widget_hide(&s_screen[i]);
    }
  }
}

ra_err_t bedroom_ui_create(GX_DISPLAY* display, GX_WINDOW_ROOT* root)
{
  if ((display == GX_NULL) || (root == GX_NULL)) {
    return k_ra_err_null_ptr;
  }
  s_root = root;

  if (bedroom_install_resources(display) != GX_SUCCESS) {
    return k_ra_fail;
  }

  /* Derive the whole layout from the runtime canvas size (set by
   * gx_display_create) so the same source fits any panel -- the firmware's
   * BSP-sized 1024x600 framebuffer and any smaller display alike. */
  const GX_VALUE dw = (display->gx_display_width > 0) ? display->gx_display_width : 1;
  const GX_VALUE dh = (display->gx_display_height > 0) ? display->gx_display_height : 1;
  bedroom_compute_layout((uint16_t)dw, (uint16_t)dh);

  GX_RECTANGLE full;
  gx_utility_rectangle_define(&full, 0, 0, (GX_VALUE)(s_geom.w - 1U), (GX_VALUE)(s_geom.h - 1U));
  if (gx_window_create(&s_bg, "bg", GX_NULL, GX_STYLE_ENABLED, GX_ID_NONE, &full) != GX_SUCCESS) {
    return k_ra_fail;
  }
  if (gx_widget_fill_color_set(&s_bg,
                               (GX_RESOURCE_ID)k_col_bg,
                               (GX_RESOURCE_ID)k_col_bg,
                               (GX_RESOURCE_ID)k_col_bg) != GX_SUCCESS) {
    return k_ra_fail;
  }
  if (gx_widget_event_process_set(&s_bg, bedroom_bg_event) != GX_SUCCESS) {
    return k_ra_fail;
  }
  if (gx_widget_attach((GX_WIDGET*)root, (GX_WIDGET*)&s_bg) != GX_SUCCESS) {
    return k_ra_fail;
  }

  if (bedroom_create_tabs() != GX_SUCCESS) {
    return k_ra_fail;
  }
  for (uint16_t i = 0U; i < (uint16_t)k_ui_tabs; i++) {
    if (bedroom_create_screen(i) != GX_SUCCESS) {
      return k_ra_fail;
    }
  }

  bedroom_ui_set_active_tab(0U);
  return k_ra_ok;
}
