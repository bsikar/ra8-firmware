/**
 * @file ra_secure.c
 * @brief Secure-comparison primitives implementation
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_secure.h"

#include <stddef.h>
#include <stdint.h>

bool ra_ct_equal(const void* a, const void* b, size_t len)
{
  if (a == nullptr) {
    return false;
  }
  if (b == nullptr) {
    return false;
  }
  const uint8_t* pa   = (const uint8_t*)a;
  const uint8_t* pb   = (const uint8_t*)b;
  uint8_t        diff = 0U;
  for (size_t i = 0U; i < len; ++i) {
    diff = (uint8_t)(diff | (uint8_t)(pa[i] ^ pb[i]));
  }
  return (diff == 0U);
}
