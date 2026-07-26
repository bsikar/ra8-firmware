/**
 * @file board_console.h
 * @brief Multi-channel console log store backing the board view's tabbed console.
 *
 * @details
 * The emulator surfaces firmware activity through many distinct endpoints: the
 * SCI_B serial console (@c [uart] SCIn:), the CoreSight ITM/SWO trace port
 * (@c [itm]), the SEGGER RTT up-buffer drain (@c [rtt]), and one lane per
 * modelled I/O protocol -- SPI, I2C, CAN, USB, SD, OSPI, I2S, ADC, DAC, GPIO,
 * DMA, and the virtual network peer. These are
 * genuinely different wires, so this module keeps one fixed-capacity scrollback
 * ring per channel plus an aggregate ALL ring that interleaves every channel in
 * arrival order. The board view renders one channel at a time behind a clickable
 * tab bar, exactly like a logic analyser splits its lanes.
 *
 * The store is the single source of truth for console text: the SCI model routes
 * each completed UART line here, main.c routes each completed ITM line here, and
 * the overlay reads back the active channel's lines to paint the console panel.
 * It owns no Unicorn engine and no AppKit dependency -- it is plain C over static
 * ring buffers (no malloc after init), so it builds and tests on any host and the
 * tabbed console stays deterministic / headlessly verifiable.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Console channels the board view exposes as tabs.
 *
 * @details ::k_board_console_ch_all is the interleaved aggregate of every other
 * channel in arrival order; the remaining values are the per-endpoint lanes.
 * Adding a lane is a one-line change here (insert before
 * ::k_board_console_ch_count) plus a matching ::board_console_name entry -- the
 * rings, the tab bar, and the hit-test all size themselves off
 * ::k_board_console_ch_count.
 *
 * @invariant ::k_board_console_ch_all is 0 so a plain loop from 1 walks the
 * source lanes only, leaving ALL to receive its copies via ::board_console_push.
 */
typedef enum : uint32_t {
  k_board_console_ch_all   = 0U,  /**< Aggregate of all lanes (arrival order).  */
  k_board_console_ch_uart  = 1U,  /**< SCI_B serial console ([uart] SCIn:).     */
  k_board_console_ch_itm   = 2U,  /**< CoreSight ITM/SWO trace ([itm]).         */
  k_board_console_ch_rtt   = 3U,  /**< SEGGER RTT up-buffer 0 drain ([rtt]).    */
  k_board_console_ch_spi   = 4U,  /**< SPI_B transfer summaries.                */
  k_board_console_ch_i2c   = 5U,  /**< I2C (IIC_B + RIIC) transfer summaries.   */
  k_board_console_ch_can   = 6U,  /**< CAN-FD frame summaries.                  */
  k_board_console_ch_usb   = 7U,  /**< USBFS transfer / enumeration summaries.  */
  k_board_console_ch_sd    = 8U,  /**< SD (SDHI + SD-over-SPI) block summaries. */
  k_board_console_ch_ospi  = 9U,  /**< Octal-SPI (xSPI) command summaries.      */
  k_board_console_ch_i2s   = 10U, /**< SSIE / I2S sample-block summaries.       */
  k_board_console_ch_adc   = 11U, /**< ADC_B conversion summaries.              */
  k_board_console_ch_dac   = 12U, /**< DAC_B output summaries.                  */
  k_board_console_ch_gpio  = 13U, /**< GPIO / PORT pin-level transitions.       */
  k_board_console_ch_dma   = 14U, /**< DMAC + DTC transfer-complete summaries.  */
  k_board_console_ch_net   = 15U, /**< Ethernet / IPv4 / TCP packet summaries.  */
  k_board_console_ch_count = 16U, /**< Channel count (ring + tab array size).   */
} board_console_ch_t;

/** @brief Fixed ring geometry (mirrors the overlay line cap + SCI ring depth). */
typedef enum : uint32_t {
  k_board_console_line_cap   = 128U, /**< Max chars stored per line (with NUL). */
  k_board_console_ring_depth = 64U,  /**< Recent lines retained per channel.    */
} board_console_dims_t;

/**
 * @brief Short display name for a channel (the tab caption).
 *
 * @details Returns a borrowed static string ("ALL" / "UART" / "ITM" / ...). The
 * same names prefix each line in the ALL ring so the aggregate view stays
 * legible, and the board view draws them as the tab captions.
 *
 * @param[in] ch Channel to name (::k_board_console_ch_all .. count-1).
 * @return Borrowed NUL-terminated name, or "?" if @p ch is out of range.
 * @retval "?" @p ch is >= ::k_board_console_ch_count.
 * @pre @p ch is a ::board_console_ch_t value.
 * @pre The returned pointer is read-only and outlives the call (static storage).
 * @post The returned pointer is never NULL.
 * @post The store is left unchanged (pure query).
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
const char* board_console_name(board_console_ch_t ch);

/**
 * @brief Append one completed line to a channel ring and the ALL ring.
 *
 * @details Copies @p line (bounded to ::k_board_console_line_cap, NUL-terminated)
 * into the channel @p ch ring, then copies a "<NAME> <line>" prefixed form into
 * the ALL ring so the aggregate view shows which endpoint each line came from.
 * Calling with ::k_board_console_ch_all is rejected (ALL is populated only as the
 * mirror of the source lanes, never directly).
 *
 * @param[in] ch   Source lane (must not be ::k_board_console_ch_all).
 * @param[in] line Completed line text (no trailing newline); copied, not retained.
 * @return Nothing.
 * @pre @p ch is a source lane in (::k_board_console_ch_all, ::k_board_console_ch_count).
 * @pre @p line is a valid NUL-terminated string.
 * @post On a valid call the channel ring and the ALL ring each gain one line.
 * @post Invalid @p ch / NULL @p line leave every ring unchanged.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
void board_console_push(board_console_ch_t ch, const char* line);

/**
 * @brief Count of lines currently retained in a channel ring.
 *
 * @details Saturates at ::k_board_console_ring_depth once the ring wraps. This is
 * the number of valid back-indices for ::board_console_line on @p ch.
 *
 * @param[in] ch Channel to query.
 * @return Lines held (0 .. ::k_board_console_ring_depth).
 * @retval 0 @p ch is out of range or no line has been pushed yet.
 * @pre @p ch is a ::board_console_ch_t value.
 * @pre The store has been zero-initialised (static lifetime guarantees it).
 * @post The store is left unchanged (pure query).
 * @post The result never exceeds ::k_board_console_ring_depth.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
uint32_t board_console_count(board_console_ch_t ch);

/**
 * @brief Monotonic count of lines ever pushed to a channel.
 *
 * @details Unlike ::board_console_count (which caps at the ring depth) this keeps
 * climbing, so the view can detect new arrivals while paused and hold its
 * absolute scroll position.
 *
 * @param[in] ch Channel to query.
 * @return Total lines ever pushed to @p ch since the last reset.
 * @retval 0 @p ch is out of range or nothing has been pushed.
 * @pre @p ch is a ::board_console_ch_t value.
 * @pre The store has been zero-initialised (static lifetime guarantees it).
 * @post The store is left unchanged (pure query).
 * @post The result is monotonic non-decreasing between resets.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
uint32_t board_console_total(board_console_ch_t ch);

/**
 * @brief Fetch a retained line by age (0 = newest) from a channel.
 *
 * @details @p back counts backwards from the newest retained line: 0 is the most
 * recent, 1 the one before it, up to ::board_console_count - 1. Older lines have
 * aged out of the ring.
 *
 * @param[in] ch   Channel to read.
 * @param[in] back Age index (0 = newest).
 * @return Borrowed NUL-terminated line text, or NULL if @p back is too old.
 * @retval NULL @p ch out of range, or @p back >= ::board_console_count.
 * @pre @p ch is a ::board_console_ch_t value.
 * @pre The returned pointer is used before the next ::board_console_push.
 * @post The store is left unchanged (pure query).
 * @post A non-NULL result points into the channel's static ring storage.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
const char* board_console_line(board_console_ch_t ch, uint32_t back);

/**
 * @brief Clear every channel ring (called on a warm reboot).
 *
 * @details Zeroes the head / count / total bookkeeping for all channels so a
 * rebooted firmware starts with an empty console, matching the peripheral models'
 * reset hooks. The line storage itself is left as-is (count gates all reads).
 *
 * @return Nothing.
 * @pre Called from the single-threaded reset path (no concurrent push).
 * @pre The store has been zero-initialised at least once (static lifetime).
 * @post Every channel reports ::board_console_count and ::board_console_total 0.
 * @post Subsequent ::board_console_line calls return NULL until a new push.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
void board_console_reset(void);

#ifdef __cplusplus
}
#endif
