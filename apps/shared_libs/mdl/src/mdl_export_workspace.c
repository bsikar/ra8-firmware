/**
 * @file mdl_export_workspace.c
 * @brief Manage the caller-owned media export bump workspace.
 *
 * @details Keeps arena initialization and aligned reservation independent of
 *          the format dispatcher so readers and validators can reuse the
 *          workspace contract without linking every exporter.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "mdl_export.h"

void mdl_export_workspace_init(mdl_export_workspace_t* ws, void* data, size_t cap)
{
  if (ws == nullptr) {
    return;
  }
  ws->data       = (uint8_t*)data;
  ws->cap        = (data == nullptr) ? 0U : cap;
  ws->used       = 0U;
  ws->high_water = 0U;
}

void* mdl_export_workspace_take(mdl_export_workspace_t* ws, size_t bytes, size_t alignment)
{
  if ((ws == nullptr) || (ws->data == nullptr) || (bytes == 0U) || (alignment == 0U) ||
      ((alignment & (alignment - 1U)) != 0U)) {
    return nullptr;
  }
  const size_t mask = alignment - 1U;
  uintptr_t    base = 0U;
  static_assert(sizeof(base) >= sizeof(ws->data), "uintptr_t must preserve object pointers");
  (void)memcpy((void*)&base, (const void*)&ws->data, sizeof(ws->data));
  if ((ws->used > (size_t)(UINTPTR_MAX - base)) ||
      ((base + (uintptr_t)ws->used) > (UINTPTR_MAX - (uintptr_t)mask))) {
    return nullptr;
  }
  const uintptr_t cursor  = base + (uintptr_t)ws->used;
  const uintptr_t aligned = (cursor + (uintptr_t)mask) & ~(uintptr_t)mask;
  const size_t    start   = (size_t)(aligned - base);
  if ((start > ws->cap) || (bytes > (ws->cap - start))) {
    return nullptr;
  }
  ws->used = start + bytes;
  if (ws->used > ws->high_water) {
    ws->high_water = ws->used;
  }
  return &ws->data[start];
}
