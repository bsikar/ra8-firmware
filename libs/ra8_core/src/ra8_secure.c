/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_secure.c
 * @brief Secure-comparison primitives implementation
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 */

#include "ra8_secure.h"

#include <stddef.h>
#include <stdint.h>

bool ra8_ct_equal(const void* a, const void* b, size_t len)
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

void ra8_secure_memzero(void* ptr, size_t len)
{
  if (ptr == nullptr) {
    return;
  }
  if (len == 0U) {
    return;
  }
  /* Write through a volatile pointer so the store cannot be dead-store
   * eliminated: unlike ``memset``, a volatile access is an observable side
   * effect the optimiser must preserve even when ``ptr`` is about to leave
   * scope. */
  volatile uint8_t* dst = (volatile uint8_t*)ptr;
  for (size_t i = 0U; i < len; ++i) {
    dst[i] = 0U;
  }
}
