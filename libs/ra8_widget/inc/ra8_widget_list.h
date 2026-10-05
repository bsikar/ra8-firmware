/**
 * @file ra8_widget_list.h
 * @brief Two-line settings and navigation rows for the ra8_widget tree.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include <stdint.h>
#include "ra8_widget.h"

typedef enum : uint8_t {
  k_ra8_widget_list_trailing_none = 0,
  k_ra8_widget_list_trailing_value = 1,
  k_ra8_widget_list_trailing_chevron = 2,
} ra8_widget_list_trailing_t;

typedef struct {
  const char* title;
  const char* subtitle;
  const char* trailing_text;
  uint16_t action_id;
  ra8_widget_list_trailing_t trailing;
} ra8_widget_list_row_t;

/**
 * Caller-owned row list. After a consumed tap, damage contains the row rect
 * to repaint; selected/has_selection expose the selection state.
 */
typedef struct {
  const ra8_widget_paint_t* paint;
  const ra8_widget_list_row_t* rows;
  uint16_t count;
  void (*on_select)(struct ra8_widget* w, uint16_t action_id);
  uint32_t bg, title_fg, subtitle_fg, trailing_fg, divider;
  int32_t row_height;
  int16_t pad;
  uint16_t selected;
  bool has_selection;
  ra8_ui_rect_t damage;
} ra8_widget_list_t;

const ra8_widget_vtable_t* ra8_widget_list_vtable(void);
ra8_err_t ra8_widget_list_init(ra8_widget_t* w, ra8_widget_list_t* list);
#ifdef __cplusplus
}
#endif
