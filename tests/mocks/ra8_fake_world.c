/**
 * @file ra8_fake_world.c
 * @brief Host-side TrustZone partition mock
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_fake_world.h"

#include <stdint.h>

#include "ra8_err.h"

typedef struct {
  uintptr_t             start;  /**< Start.  */
  uintptr_t             end;    /**< End.    */
  ra8_fake_world_attr_t attr;   /**< Attr.   */
  bool                  in_use; /**< In use. */
} ra8_fake_world_region_t;

static ra8_fake_world_region_t s_regions[k_ra8_fake_world_max_regions];

void ra8_fake_world_reset(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_fake_world_max_regions; ++i) {
    s_regions[i].start  = 0U;
    s_regions[i].end    = 0U;
    s_regions[i].attr   = k_ra8_fake_world_attr_secure;
    s_regions[i].in_use = false;
  }
}

static ra8_err_t internal_mark(const void* ptr, uint32_t len, ra8_fake_world_attr_t attr)
{
  if (len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_fake_world_max_regions; ++i) {
    if (!s_regions[i].in_use) {
      s_regions[i].start  = (uintptr_t)ptr;
      s_regions[i].end    = (uintptr_t)ptr + (uintptr_t)len;
      s_regions[i].attr   = attr;
      s_regions[i].in_use = true;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_no_mem;
}

ra8_err_t ra8_fake_world_mark_ns(const void* ptr, uint32_t len)
{
  return internal_mark(ptr, len, k_ra8_fake_world_attr_ns);
}

ra8_err_t ra8_fake_world_mark_s(const void* ptr, uint32_t len)
{
  return internal_mark(ptr, len, k_ra8_fake_world_attr_secure);
}

bool ra8_fake_world_check_ns_range(const void* ptr, uint32_t len)
{
  if (len == 0U) {
    return false;
  }
  const uintptr_t want_start = (uintptr_t)ptr;
  const uintptr_t want_end   = want_start + (uintptr_t)len;

  /* The query is satisfied if at least one NS region fully
   * contains [want_start, want_end) AND no Secure region overlaps
   * any byte of the range. */
  bool ns_covered = false;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_fake_world_max_regions; ++i) {
    if (!s_regions[i].in_use) {
      continue;
    }
    const bool overlaps = ((s_regions[i].start < want_end) && (s_regions[i].end > want_start));
    if (overlaps && (s_regions[i].attr == k_ra8_fake_world_attr_secure)) {
      return false; /* Secure overlap kills the query. */
    }
    if ((s_regions[i].attr == k_ra8_fake_world_attr_ns) && (s_regions[i].start <= want_start) &&
        (s_regions[i].end >= want_end)) {
      ns_covered = true;
    }
  }
  return ns_covered;
}
