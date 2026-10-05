/**
 * @file ra8_widget_text_field.h
 * @brief Caller-buffer-backed single-line text entry.
 * SPDX-License-Identifier: MIT
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include "ra8_widget.h"

typedef struct ra8_widget_text_field {
  const ra8_widget_paint_t* paint;
  char* buffer;
  uint16_t capacity; /**< Includes room for the trailing NUL. */
  uint16_t len;
  const char* placeholder;
  uint32_t fg;
  uint32_t bg;
  uint32_t caret;
  int16_t pad;
  ra8_widget_text_face_t face;
  ra8_widget_text_weight_t weight;
  ra8_widget_text_size_t size;
  bool focused;
  bool submitted;
  ra8_ui_rect_t damage;
  void (*on_submit)(ra8_widget_t* w);
} ra8_widget_text_field_t;

const ra8_widget_vtable_t* ra8_widget_text_field_vtable(void);
[[nodiscard]] ra8_err_t ra8_widget_text_field_init(ra8_widget_t* w, ra8_widget_text_field_t* field);
#ifdef __cplusplus
}
#endif
