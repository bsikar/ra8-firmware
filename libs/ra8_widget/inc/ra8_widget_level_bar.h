/**
 * @file ra8_widget_level_bar.h
 * @brief Signed 13-cell vertical level bar for equalizer bands.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include "ra8_widget.h"

/** Caller-owned vertical level bar; positive values fill above its centre. */
typedef struct ra8_widget_level_bar {
  const ra8_widget_paint_t* paint;
  uint32_t track;
  uint32_t fill;
  uint32_t center_mark;
  int8_t value;
  uint8_t reserved[3];
  ra8_ui_rect_t damage;
} ra8_widget_level_bar_t;

const ra8_widget_vtable_t* ra8_widget_level_bar_vtable(void);
ra8_err_t ra8_widget_level_bar_init(ra8_widget_t* w, ra8_widget_level_bar_t* bar);
#ifdef __cplusplus
}
#endif
