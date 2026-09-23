// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

#include <stddef.h>
#include <stdint.h>

#include "ra8_log.h"
#include "ra8_ui.h"

size_t ra8_keyboard_test_log_calls;
const char* ra8_keyboard_test_last_message = "";
size_t ra8_keyboard_test_contains_calls;

void ra8_keyboard_test_reset(void) {
  ra8_keyboard_test_log_calls = 0U;
  ra8_keyboard_test_last_message = "";
  ra8_keyboard_test_contains_calls = 0U;
}

void ra8_log_emit_error(const char* tag, const char* message) {
  (void)tag;
  ++ra8_keyboard_test_log_calls;
  ra8_keyboard_test_last_message = message;
}

uint8_t ra8_ui_rect_contains(const ra8_ui_rect_t* r, int32_t px, int32_t py) {
  ++ra8_keyboard_test_contains_calls;
  if (r == NULL) {
    return 0U;
  }

  const int64_t left = r->x;
  const int64_t top = r->y;
  const int64_t right = left + r->w;
  const int64_t bottom = top + r->h;
  const int64_t point_x = px;
  const int64_t point_y = py;
  return (uint8_t)((point_x >= left) && (point_x < right) &&
                   (point_y >= top) && (point_y < bottom));
}
