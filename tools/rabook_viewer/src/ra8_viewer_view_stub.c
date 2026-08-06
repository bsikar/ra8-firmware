/**
 * @file ra8_viewer_view_stub.c
 * @brief Headless stand-in for the Cocoa reader window on non-Apple hosts.
 *
 * @details
 * The viewer's live window (ra8_viewer_view.m) is Cocoa, so it builds only on
 * macOS. Off the APPLE path -- a Linux CI runner with no window server -- this
 * file is compiled in its place to satisfy the exact same C ABI declared in
 * ra8_viewer_view.h, with zero AppKit/QuartzCore references.
 *
 * ::ra8_viewer_view_open reports failure by returning `nullptr`, which is a
 * documented outcome of that contract ("NULL if a window could not be created,
 * e.g. a headless host with no display"), not an invented one. main.c already
 * has the matching branch: it prints the "no window (headless host?)" hint and
 * exits non-zero, so nothing silently pretends a window appeared. The render
 * paths that matter off a desktop -- `--headless --dump-ppm` and `--dump-tile`
 * -- never open a window and are therefore fully exercised on Linux.
 *
 * This is a platform alternative, not a stub in the sense the no-stubs policy
 * bans: the CMakeLists compiles exactly one of ra8_viewer_view.m (APPLE) or
 * this file (non-APPLE), so it is the ONLY definition of these symbols in its
 * build and never shadows a working implementation. tools/ra8_emulator's
 * board_view_stub.c stands in the same relation to board_view.m.
 *
 * @see ra8_viewer_view.h  The contract both backends implement.
 * @see ra8_viewer_view.m  The Cocoa backend this replaces off the APPLE path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 *

 */

#include <stdint.h>

#include "ra8_viewer_view.h"

/**
 * @brief Report that no desktop window is available on this build.
 *
 * @details Non-Apple builds carry no window server binding, so window creation
 *          cannot succeed and says so rather than handing back a handle that
 *          presents nothing.
 *
 * @param[in] reader Open reader whose tiles a window would render (unused).
 * @param[in] title  Window title (unused, headless).
 *
 * @return Always `nullptr` -- there is no window backend off the APPLE path.
 * @retval nullptr Invariably; callers take their documented headless branch.
 *
 * @pre The build is not targeting Apple (this file is compiled only there).
 * @pre Caller handles a `nullptr` return, as the header requires.
 * @post No window, thread, or display resource is created.
 * @post @p reader is untouched and still owned by the caller.
 *
 * @note Thread-safe: touches no state.
 *
 * @since 0.1.0
 */
ra8_viewer_view_t*
ra8_viewer_view_open(ra8_viewer_reader_t* reader, const char* title, ra8_arena_t* arena)
{
  (void)reader;
  (void)title;
  (void)arena;
  return nullptr;
}

/**
 * @brief Report the (never-opened) window as closed.
 *
 * @details ::ra8_viewer_view_open never returns a handle here, so any pump is
 *          by definition over a closed window; returning true stops a caller
 *          that loops until closure rather than spinning forever.
 *
 * @param[in] view View handle (always `nullptr` on this build, unused).
 *
 * @return Always `true` -- treated as closed.
 * @retval true Invariably; the caller should exit its event loop.
 *
 * @pre The build is not targeting Apple.
 * @pre @p view came from ::ra8_viewer_view_open (hence `nullptr`).
 * @post No events are drained; no state changes.
 * @post The caller's loop terminates on the first iteration.
 *
 * @note Thread-safe: touches no state.
 *
 * @since 0.1.0
 */
bool ra8_viewer_view_pump(ra8_viewer_view_t* view)
{
  (void)view;
  return true;
}

/**
 * @brief No-op close (no window was ever opened).
 *
 * @details Present so callers need no platform conditional around teardown.
 *
 * @param[in] view View handle (always `nullptr` on this build, unused).
 *
 * @pre The build is not targeting Apple.
 * @pre @p view came from ::ra8_viewer_view_open (hence `nullptr`).
 * @post No resource is released, because none was acquired.
 * @post Safe to call any number of times.
 *
 * @note Thread-safe: touches no state.
 *
 * @since 0.1.0
 */
void ra8_viewer_view_close(ra8_viewer_view_t* view)
{
  (void)view;
}
