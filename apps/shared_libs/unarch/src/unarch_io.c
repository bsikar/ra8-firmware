/**
 * @file unarch_io.c
 * @brief Flat-memory backing for the shared unarchiver read seam.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The one trivial ::unarch_read_fn implementation everybody shares: a
 * bounds-clamped copy out of a ::unarch_mem_t view. Reads never touch a
 * byte outside `[base, base + len)` regardless of the (untrusted) offset and
 * length arguments, so the wrapped-open paths can point it at a decompressed
 * arena without further guarding.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include "unarch_io.h"

#include <string.h>

size_t unarch_mem_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  const unarch_mem_t* m = (const unarch_mem_t*)ctx;
  if (m == nullptr) {
    return 0U;
  }
  if (m->base == nullptr) {
    return 0U;
  }
  if (buf == nullptr) {
    return 0U;
  }
  if (offset >= (uint64_t)m->len) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)m->len - offset;
  const size_t   n     = ((uint64_t)len > avail) ? (size_t)avail : len;
  if (n == 0U) {
    return 0U;
  }
  (void)memcpy(buf, &m->base[offset], n);
  return n;
}
