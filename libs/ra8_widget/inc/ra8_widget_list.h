/**
 * @file ra8_widget_list.h
 * @brief Settings and navigation rows for the ra8_widget tree.
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
  k_ra8_widget_list_trailing_value_chevron = 3,
} ra8_widget_list_trailing_t;

typedef enum : uint8_t {
  k_ra8_widget_list_standard = 0,
  k_ra8_widget_list_two_buttons = 1,
  k_ra8_widget_list_toggle_help = 2,
} ra8_widget_list_variant_t;

typedef enum : uint8_t {
  k_ra8_widget_list_element_row = 0,
  k_ra8_widget_list_element_button_1 = 1,
  k_ra8_widget_list_element_button_2 = 2,
  k_ra8_widget_list_element_toggle = 3,
} ra8_widget_list_element_t;

typedef struct {
  const char* title;
  const char* subtitle;
  const char* trailing_text;
  uint16_t action_id;
  ra8_widget_list_trailing_t trailing;
  ra8_widget_list_variant_t variant;
  const char* button_1_text;
  uint16_t button_1_action_id;
  const char* button_2_text;
  uint16_t button_2_action_id;
  const char* help_text;
  bool* toggle_value;
} ra8_widget_list_row_t;

/** Called for every consumed tap; element identifies the subcontrol. */
typedef void (*ra8_widget_list_on_select_element_t)(
    struct ra8_widget* w, uint16_t row, uint8_t element, uint16_t action_id);

/**
 * Caller-owned row list. After a consumed tap, damage contains only the
 * changed row element rect. The legacy on_select callback is retained.
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
  ra8_widget_text_face_t text_face;    /**< Text family; zero keeps sans. */
  ra8_widget_text_weight_t text_weight; /**< Text weight; zero keeps regular. */
  ra8_widget_text_size_t text_size;    /**< Text size; zero keeps size three. */
  ra8_widget_list_on_select_element_t on_select_element;
  uint8_t selected_element;
} ra8_widget_list_t;

const ra8_widget_vtable_t* ra8_widget_list_vtable(void);
ra8_err_t ra8_widget_list_init(ra8_widget_t* w, ra8_widget_list_t* list);
#ifdef __cplusplus
}
#endif
