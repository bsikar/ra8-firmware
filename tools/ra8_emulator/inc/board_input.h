/**
 * @file board_input.h
 * @brief Host-side keystroke FIFO: window / CLI key source -> UART RX sink
 *
 * @details
 * A tiny plain-C ring buffer that decouples the keystroke SOURCE from the SINK
 * so the interactive keyboard input is deterministically testable. The source is
 * either the live @c --view window (its Cocoa keyDown handler pushes each typed
 * character) or the headless @c --keys flag (which pushes a scripted string); the
 * sink is the run loop, which drains the FIFO every chunk into the console UART
 * RX -- the same SCI channel @c --input targets. Keeping the FIFO here in plain C
 * (not inside the Objective-C view) lets @c --keys exercise the exact push ->
 * drain -> SCI path on any host with no window and no OS key events, so the
 * keyboard path is verifiable headlessly and in CI -- not only by typing into a
 * window. Single-threaded harness use; not thread-safe.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Push one keystroke byte into the input FIFO.
 *
 * @details Appends @p ch to the ring; on overflow the newest key is dropped (the
 * firmware draining the FIFO is the slow consumer). Called by the window keyDown
 * handler and by the @c --keys injector alike, so both reach the UART through one
 * path.
 *
 * @param[in] ch Keystroke byte to enqueue.
 * @return Nothing.
 * @note Not thread-safe; single-threaded harness use.
 * @since 0.1.0
 */
void board_input_push_key(char ch);

/**
 * @brief Pop the oldest buffered keystroke, if any.
 *
 * @param[out] out Receives one keystroke byte when true is returned.
 * @return true if a byte was dequeued (and consumed); false if empty / @p out
 *         is NULL.
 * @note Not thread-safe; single-threaded harness use.
 * @since 0.1.0
 */
bool board_input_pop_key(char* out);

#ifdef __cplusplus
}
#endif
