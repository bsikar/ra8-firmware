/**
 * @file ra8_fake_world.c
 * @brief Host-side TrustZone partition mock
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details Models a bounded set of TrustZone address regions so host tests can
 * classify secure and non-secure accesses without target attribution hardware.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_fake_world.h"

#include <stdint.h>

#include "ra8_attributes.h"
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

/**
 * @brief Append one attributed address range to the fake world table.
 * @details Validates the span and bounded table capacity before recording the
 * exact base, length, and Secure or Non-Secure classification.
 * @param[in] ptr First byte of the host range.
 * @param[in] len Nonzero range length in bytes.
 * @param[in] attr World classification to record.
 * @return Append status.
 * @retval k_ra8_ok The range was recorded.
 * @retval k_ra8_err_invalid_arg @p len is zero.
 * @retval k_ra8_err_no_mem The bounded range table is full.
 * @pre @p attr is a valid ::ra8_fake_world_attr_t value.
 * @pre The caller keeps the described address range meaningful while queried.
 * @post Success increases the recorded range count by one.
 * @post Failure leaves the table unchanged.
 * @note A null @p ptr is accepted as an address value when @p len is nonzero.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mark(const void* ptr, uint32_t len, ra8_fake_world_attr_t attr)
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
