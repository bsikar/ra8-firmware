/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_input.c
 * @brief Host-side keystroke FIFO implementation (see board_input.h)
 *
 * @details
 * A fixed-size ring buffer. The window's keyDown handler and the @c --keys flag
 * both push through ::board_input_push_key; the run loop drains via
 * ::board_input_pop_key into the console UART RX. Single-threaded harness use.
 *
 *
 * @since 0.1.0
 */

#include "board_input.h"

#include <stdint.h>

/** @brief Keystroke ring geometry. */
typedef enum : uint32_t {
  k_key_ring_cap = 64U, /**< Ring capacity (one slot is the full/empty guard). */
} board_input_cfg_t;

static char     s_ring[k_key_ring_cap]; /**< Keystroke ring storage.    */
static uint32_t s_head;                 /**< Read cursor (oldest byte). */
static uint32_t s_tail;                 /**< Write cursor (next free).  */

void board_input_push_key(char ch)
{
  const uint32_t next = (s_tail + 1U) % (uint32_t)k_key_ring_cap;
  if (next != s_head) {
    s_ring[s_tail] = ch;
    s_tail         = next;
  }
}

bool board_input_pop_key(char* out)
{
  if ((out == nullptr) || (s_head == s_tail)) {
    return false;
  }
  *out   = s_ring[s_head];
  s_head = (s_head + 1U) % (uint32_t)k_key_ring_cap;
  return true;
}
