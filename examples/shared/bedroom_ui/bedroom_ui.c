/**
 * @file bedroom_ui.c
 * @brief Shared GUIX 3-tab bedroom UI (host + EK-RA8D2 panel)
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
 * GUIX widget coordinates are absolute (canvas-relative); every rectangle
 * below is placed in absolute pixels for the 1024x600 design size shared
 * by the macOS preview window and the EK-RA8D2 panel.
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
 * @enum bedroom_layout_t
 * @brief Layout geometry (absolute pixels, 1024x600 design size).
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ui_w           = k_panel_width_px,  /**< Active panel width.        */
  k_ui_h           = k_panel_height_px, /**< Active panel height.       */
  k_ui_tabbar_h    = 56U,               /**< Tab-bar height.                     */
  k_ui_tabs        = 3U,                /**< Tab / screen count.                 */
  k_ui_cards       = 4U,                /**< Stat cards per screen (2x2).        */
  k_ui_cols        = 2U,                /**< Card grid columns.                  */
  k_ui_card_rows   = 3U,                /**< Text rows per card (label/val/sub). */
  k_ui_pad         = 32U,               /**< Outer content inset.                */
  k_ui_gap         = 24U,               /**< Gap between cards.                  */
  k_ui_grid_top    = 148U,              /**< Card grid top edge.                 */
  k_ui_bot_margin  = 28U,               /**< Card grid bottom margin.            */
  k_ui_accent_h    = 6U,                /**< Card top accent-strip height.       */
  k_ui_card_inset  = 22U,               /**< Text inset inside a card.           */
  k_ui_head_y      = 70U,               /**< Heading top edge.                   */
  k_ui_head_h      = 34U,               /**< Heading row height.                 */
  k_ui_sub_y       = 110U,              /**< Subtitle top edge.                  */
  k_ui_sub_h       = 24U,               /**< Subtitle row height.                */
  k_ui_lbl_dy      = 22U,               /**< Card label  y, below card top.      */
  k_ui_lbl_h       = 24U,               /**< Card label  row height.             */
  k_ui_val_dy      = 66U,               /**< Card value  y, below card top.      */
  k_ui_val_h       = 40U,               /**< Card value  row height.             */
  k_ui_subv_dy     = 120U,              /**< Card sub    y, below card top.      */
  k_ui_subv_h      = 24U,               /**< Card sub    row height.             */
  k_ui_tab_id_base = 100U,              /**< First tab button widget id.         */
} bedroom_layout_t;

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
 * @return UINT GX_SUCCESS or the first failing GUIX status.
 */
static UINT bedroom_create_tabs(void)
{
  const UINT tabw = (UINT)k_ui_w / (UINT)k_ui_tabs;
  for (uint16_t i = 0U; i < (uint16_t)k_ui_tabs; i++) {
    const GX_VALUE x0 = (GX_VALUE)((UINT)i * tabw);
    const GX_VALUE x1 =
      (GX_VALUE)((i == (uint16_t)(k_ui_tabs - 1U)) ? ((UINT)k_ui_w - 1U)
                                                   : (((UINT)i + 1U) * tabw - 1U));
    GX_RECTANGLE r;
    gx_utility_rectangle_define(&r, x0, 0, x1, (GX_VALUE)(k_ui_tabbar_h - 1U));

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
 * @brief Compute one card's absolute rectangle in the 2-column grid.
 *
 * @param[in]  i   Card index in [0, k_ui_cards).
 * @param[out] out Card rectangle (absolute pixels).
 */
static void bedroom_card_bounds(uint16_t i, GX_RECTANGLE* out)
{
  const UINT grid_rows = (UINT)k_ui_cards / (UINT)k_ui_cols;
  const UINT col_w     = ((UINT)k_ui_w - (2U * (UINT)k_ui_pad) - (UINT)k_ui_gap) / (UINT)k_ui_cols;
  const UINT row_h =
    (((UINT)k_ui_h - (UINT)k_ui_bot_margin) - (UINT)k_ui_grid_top - (UINT)k_ui_gap) / grid_rows;
  const UINT col = (UINT)i % (UINT)k_ui_cols;
  const UINT row = (UINT)i / (UINT)k_ui_cols;
  const UINT x0  = (UINT)k_ui_pad + (col * (col_w + (UINT)k_ui_gap));
  const UINT y0  = (UINT)k_ui_grid_top + (row * (row_h + (UINT)k_ui_gap));
  gx_utility_rectangle_define(out,
                              (GX_VALUE)x0,
                              (GX_VALUE)y0,
                              (GX_VALUE)(x0 + col_w - 1U),
                              (GX_VALUE)(y0 + row_h - 1U));
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
                              (GX_VALUE)(card->gx_rectangle_top + (GX_VALUE)k_ui_accent_h - 1));
  return bedroom_make_panel(&s_accent[room][i],
                            (GX_WIDGET*)&s_card[room][i],
                            strip,
                            (GX_RESOURCE_ID)s_cards[room][i].accent);
}

/**
 * @brief Create a card's three text rows (label, value, sub-status).
 *
 * @param[in] room Room index.
 * @param[in] i    Card index.
 * @param[in] card Card rectangle from bedroom_card_bounds.
 * @return UINT GX_SUCCESS or the first failing GUIX status.
 */
static UINT bedroom_create_card_text(uint16_t room, uint16_t i, const GX_RECTANGLE* card)
{
  const bedroom_card_t* c  = &s_cards[room][i];
  const GX_VALUE        y0 = card->gx_rectangle_top;
  const GX_VALUE        lx = (GX_VALUE)(card->gx_rectangle_left + (GX_VALUE)k_ui_card_inset);
  const GX_VALUE        rx = (GX_VALUE)(card->gx_rectangle_right - (GX_VALUE)k_ui_card_inset);
  const struct {
    GX_PROMPT*     widget;
    GX_VALUE       dy;
    GX_VALUE       h;
    const char*    text;
    GX_RESOURCE_ID color;
  } rows[k_ui_card_rows] = {
    {&s_lbl[room][i],
     (GX_VALUE)k_ui_lbl_dy,
     (GX_VALUE)k_ui_lbl_h,
     c->label,
     (GX_RESOURCE_ID)k_col_label},
    {&s_val[room][i],
     (GX_VALUE)k_ui_val_dy,
     (GX_VALUE)k_ui_val_h,
     c->value,
     (GX_RESOURCE_ID)c->accent},
    {&s_sub[room][i],
     (GX_VALUE)k_ui_subv_dy,
     (GX_VALUE)k_ui_subv_h,
     c->sub,
     (GX_RESOURCE_ID)k_col_label},
  };
  for (uint16_t k = 0U; k < (uint16_t)k_ui_card_rows; k++) {
    GX_RECTANGLE r;
    gx_utility_rectangle_define(&r,
                                lx,
                                (GX_VALUE)(y0 + rows[k].dy),
                                rx,
                                (GX_VALUE)(y0 + rows[k].dy + rows[k].h));
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
 * @brief Create one room screen: backdrop, heading, subtitle, card grid.
 *
 * @param[in] room Room index.
 * @return UINT GX_SUCCESS or the first failing GUIX status.
 */
static UINT bedroom_create_screen(uint16_t room)
{
  GX_RECTANGLE r;
  gx_utility_rectangle_define(&r,
                              0,
                              (GX_VALUE)k_ui_tabbar_h,
                              (GX_VALUE)(k_ui_w - 1U),
                              (GX_VALUE)(k_ui_h - 1U));
  UINT status = bedroom_make_panel(&s_screen[room], (GX_WIDGET*)&s_bg, r, s_screen_bg[room]);
  if (status != GX_SUCCESS) {
    return status;
  }

  const UINT lx = (UINT)k_ui_pad;
  const UINT rx = (UINT)k_ui_w - 1U - (UINT)k_ui_pad;

  gx_utility_rectangle_define(&r,
                              (GX_VALUE)lx,
                              (GX_VALUE)k_ui_head_y,
                              (GX_VALUE)rx,
                              (GX_VALUE)((UINT)k_ui_head_y + (UINT)k_ui_head_h));
  status = bedroom_make_label(&s_heading[room],
                              (GX_WIDGET*)&s_screen[room],
                              r,
                              s_names[room],
                              (GX_RESOURCE_ID)k_font_txt,
                              (GX_RESOURCE_ID)k_col_heading);
  if (status != GX_SUCCESS) {
    return status;
  }

  gx_utility_rectangle_define(&r,
                              (GX_VALUE)lx,
                              (GX_VALUE)k_ui_sub_y,
                              (GX_VALUE)rx,
                              (GX_VALUE)((UINT)k_ui_sub_y + (UINT)k_ui_sub_h));
  status = bedroom_make_label(&s_subtitle[room],
                              (GX_WIDGET*)&s_screen[room],
                              r,
                              s_subtitles[room],
                              (GX_RESOURCE_ID)k_font_txt,
                              (GX_RESOURCE_ID)k_col_subtitle);
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

  GX_RECTANGLE full;
  gx_utility_rectangle_define(&full, 0, 0, (GX_VALUE)(k_ui_w - 1U), (GX_VALUE)(k_ui_h - 1U));
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
