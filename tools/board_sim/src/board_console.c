/**
 * @file board_console.c
 * @brief Multi-channel console log store implementation (see board_console.h).
 *
 * @details
 * One fixed-capacity ring per channel plus an aggregate ALL ring. A source lane
 * (UART / ITM / ...) receives a line verbatim via ::board_console_push, which
 * also mirrors a "<NAME> <line>" prefixed copy into the ALL ring so the
 * aggregate view labels each line's origin. All storage is static (no malloc
 * after init); a back-index walk converts an age (0 = newest) to a ring slot the
 * same way the SCI scrollback ring did before this store subsumed it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "board_console.h"

#include <stdint.h>

/**
 * @brief One channel's scrollback ring.
 *
 * @details @c lines is a circular buffer of NUL-terminated rows; @c head is the
 * next write slot (one past the newest); @c count saturates at the ring depth
 * and @c total climbs without bound. A back-index @e b maps to ring slot
 * @c (head + depth - 1 - b) mod depth.
 *
 * @invariant @c count <= ::k_board_console_ring_depth at all times.
 * @invariant @c head < ::k_board_console_ring_depth at all times.
 */
typedef struct {
  char     lines[k_board_console_ring_depth][k_board_console_line_cap]; /**< Row storage. */
  uint32_t head;  /**< Next write slot (one past newest).   */
  uint32_t count; /**< Rows held (saturates at ring depth). */
  uint32_t total; /**< Rows ever pushed (monotonic).        */
} console_ring_t;

/** @brief One ring per channel, including the aggregate ALL ring at index 0. */
static console_ring_t s_rings[k_board_console_ch_count];

/** @brief Tab captions, indexed by ::board_console_ch_t (also the ALL prefix). */
static const char* const k_channel_names[k_board_console_ch_count] = {
  "ALL",  /**< k_board_console_ch_all  */
  "UART", /**< k_board_console_ch_uart */
  "ITM",  /**< k_board_console_ch_itm  */
  "RTT",  /**< k_board_console_ch_rtt  */
  "SPI",  /**< k_board_console_ch_spi  */
  "I2C",  /**< k_board_console_ch_i2c  */
  "CAN",  /**< k_board_console_ch_can  */
  "USB",  /**< k_board_console_ch_usb  */
  "SD",   /**< k_board_console_ch_sd   */
  "OSPI", /**< k_board_console_ch_ospi */
  "I2S",  /**< k_board_console_ch_i2s  */
  "ADC",  /**< k_board_console_ch_adc  */
  "DAC",  /**< k_board_console_ch_dac  */
  "GPIO", /**< k_board_console_ch_gpio */
  "DMA",  /**< k_board_console_ch_dma  */
  "NET",  /**< k_board_console_ch_net  */
};

const char* board_console_name(board_console_ch_t ch)
{
  if ((uint32_t)ch >= (uint32_t)k_board_console_ch_count) {
    return "?";
  }
  return k_channel_names[(uint32_t)ch];
}

/**
 * @brief Copy a bounded, NUL-terminated string into a ring slot.
 *
 * @details Writes at most ::k_board_console_line_cap - 1 chars from @p src into
 * @p dst then NUL-terminates, so an over-length line is truncated rather than
 * overrunning the slot. A NULL @p src yields an empty string.
 *
 * @param[out] dst Destination ring slot (::k_board_console_line_cap bytes).
 * @param[in]  src Source text (may be NULL).
 * @return Nothing.
 * @pre @p dst points at a ::k_board_console_line_cap-byte buffer.
 * @pre The copy is bounded by ::k_board_console_line_cap (no overrun possible).
 * @post @p dst is NUL-terminated within its capacity.
 * @post At most ::k_board_console_line_cap - 1 source chars are copied.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
static void slot_copy(char* dst, const char* src)
{
  if (dst == nullptr) {
    return;
  }
  uint32_t i = 0U;
  for (; i < (uint32_t)(k_board_console_line_cap - 1U); i++) {
    if (src == nullptr) {
      break;
    }
    if (src[i] == '\0') {
      break;
    }
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

/**
 * @brief Append one already-formed line to a single ring.
 *
 * @details Copies @p line into the head slot, advances @c head modulo the ring
 * depth, saturates @c count at the depth, and bumps the monotonic @c total.
 *
 * @param[in,out] ring Ring to push into.
 * @param[in]     line Line text to copy (bounded; may be NULL -> empty).
 * @return Nothing.
 * @pre @p ring is a valid ::console_ring_t with @c head < ring depth.
 * @pre @p ring has space for the copy (a ring is never full -- it overwrites).
 * @post @p ring gains one line; @c count <= ::k_board_console_ring_depth.
 * @post @c total increased by one.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
static void ring_push(console_ring_t* ring, const char* line)
{
  if (ring == nullptr) {
    return;
  }
  if (ring->head >= (uint32_t)k_board_console_ring_depth) {
    ring->head = 0U; /* defensive: keep the slot index in range */
  }
  slot_copy(ring->lines[ring->head], line);
  ring->head = (ring->head + 1U) % (uint32_t)k_board_console_ring_depth;
  if (ring->count < (uint32_t)k_board_console_ring_depth) {
    ring->count++;
  }
  ring->total++;
}

/**
 * @brief Build the "<NAME> <line>" form an ALL-ring entry stores.
 *
 * @details Prefixes @p line with the source channel's name and a space so the
 * aggregate view labels each line's origin, truncating to the slot capacity.
 *
 * @param[in]  ch   Source channel whose name prefixes the line.
 * @param[in]  line Source line text (may be NULL -> name only).
 * @param[out] out  Destination buffer (::k_board_console_line_cap bytes).
 * @return Nothing.
 * @pre @p out points at a ::k_board_console_line_cap-byte buffer.
 * @pre @p ch is a source lane with a valid ::board_console_name.
 * @post @p out is NUL-terminated within its capacity.
 * @post @p out begins with the channel name followed by a space.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
static void format_all_line(board_console_ch_t ch, const char* line, char* out)
{
  if (out == nullptr) {
    return;
  }
  const char* name = board_console_name(ch);
  uint32_t    n    = 0U;
  for (; n < (uint32_t)(k_board_console_line_cap - 1U); n++) {
    if (name[n] == '\0') {
      break;
    }
    out[n] = name[n];
  }
  if (n < (uint32_t)(k_board_console_line_cap - 1U)) {
    out[n] = ' ';
    n++;
  }
  uint32_t j = 0U;
  for (; n < (uint32_t)(k_board_console_line_cap - 1U); n++) {
    if (line == nullptr) {
      break;
    }
    if (line[j] == '\0') {
      break;
    }
    out[n] = line[j];
    j++;
  }
  out[n] = '\0';
}

void board_console_push(board_console_ch_t ch, const char* line)
{
  if ((uint32_t)ch == (uint32_t)k_board_console_ch_all) {
    return; /* ALL is a mirror only; never pushed to directly. */
  }
  if ((uint32_t)ch >= (uint32_t)k_board_console_ch_count) {
    return;
  }
  if (line == nullptr) {
    return;
  }
  ring_push(&s_rings[(uint32_t)ch], line);
  char all_line[k_board_console_line_cap];
  format_all_line(ch, line, all_line);
  ring_push(&s_rings[(uint32_t)k_board_console_ch_all], all_line);
}

uint32_t board_console_count(board_console_ch_t ch)
{
  if ((uint32_t)ch >= (uint32_t)k_board_console_ch_count) {
    return 0U;
  }
  return s_rings[(uint32_t)ch].count;
}

uint32_t board_console_total(board_console_ch_t ch)
{
  if ((uint32_t)ch >= (uint32_t)k_board_console_ch_count) {
    return 0U;
  }
  return s_rings[(uint32_t)ch].total;
}

const char* board_console_line(board_console_ch_t ch, uint32_t back)
{
  if ((uint32_t)ch >= (uint32_t)k_board_console_ch_count) {
    return nullptr;
  }
  const console_ring_t* ring = &s_rings[(uint32_t)ch];
  if (back >= ring->count) {
    return nullptr; /* older than the retained history */
  }
  /* back = 0 is the newest line; head points one past the newest. */
  const uint32_t idx = (ring->head + (uint32_t)k_board_console_ring_depth - 1U - back) %
                       (uint32_t)k_board_console_ring_depth;
  return ring->lines[idx];
}

void board_console_reset(void)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_board_console_ch_count; ch++) {
    s_rings[ch].head  = 0U;
    s_rings[ch].count = 0U;
    s_rings[ch].total = 0U;
  }
}
