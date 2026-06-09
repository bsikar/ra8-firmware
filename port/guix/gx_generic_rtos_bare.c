/* Generic single-threaded GUIX RTOS binding for the bare-metal RA8D2 target.
 *
 * DEPRECATED: GUIX retirement -- tracked by issue #81 (replaced by the
 * ra_reflow chrome renderer, issue #80); scheduled for wholesale removal.
 *
 * GUIX, when built with GX_DISABLE_THREADX_BINDING, calls these gx_generic_*
 * functions instead of ThreadX. The firmware runs GUIX single-threaded and
 * drives it from the application loop (gx_host_pump / gx_system_canvas_refresh),
 * so:
 *   - thread_start does NOT spawn anything (main() pumps GUIX),
 *   - the event queue is a small static ring,
 *   - mutex calls are no-ops (single-threaded),
 *   - time comes from the firmware's SysTick-backed millisecond counter.
 *
 * Time comes from gx_generic_system_time_get(), backed by ra_time_ms()
 * (libs/ra_core/ra_time.c) -- the 1 kHz SysTick tick the board_sim advances
 * cooperatively between emulation chunks. A monotonically increasing
 * millisecond value is all GUIX's timer logic needs. The rest of the surface
 * (event ring + gx_host_pump) lets the panel app drive GUIX from main().
 *
 * Built only on the cross compile (the host unit-test harness does not link
 * the GUIX vendor tree); guarded with RA_SIMULATOR_MODE like the rest of the
 * GUIX-aware sources.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#ifndef RA_SIMULATOR_MODE

#include "gx_api.h"
#include "gx_host_run.h"
#include "gx_system.h"
#include "ra_time.h"

enum { K_BARE_EVENT_RING = 64 };

static GX_EVENT s_ring[K_BARE_EVENT_RING];
static int      s_head;
static int      s_tail;
static int      s_count;
static int      s_thread_token; /* address used as a non-NULL "current thread" id */

VOID gx_generic_rtos_initialize(VOID)
{
  s_head  = 0;
  s_tail  = 0;
  s_count = 0;
}

UINT gx_generic_thread_start(VOID (*thread_entry)(ULONG))
{
  /* Single-threaded target: do not run GUIX's event loop on a thread; main()
   * pumps GUIX directly. Just acknowledge. */
  (void)thread_entry;
  return GX_SUCCESS;
}

UINT gx_generic_event_post(GX_EVENT* event_ptr)
{
  if (event_ptr == GX_NULL) {
    return GX_FAILURE;
  }
  if (s_count >= K_BARE_EVENT_RING) {
    return GX_FAILURE;
  }
  s_ring[s_tail] = *event_ptr;
  s_tail         = (s_tail + 1) % K_BARE_EVENT_RING;
  s_count++;
  return GX_SUCCESS;
}

UINT gx_generic_event_fold(GX_EVENT* event_ptr)
{
  /* No coalescing -- just enqueue (matches the host bind). */
  return gx_generic_event_post(event_ptr);
}

VOID gx_generic_event_purge(GX_WIDGET* widget)
{
  (void)widget;
}

UINT gx_generic_event_pop(GX_EVENT* put_event, GX_BOOL wait)
{
  (void)wait; /* single-threaded: never block */
  if (s_count == 0) {
    return GX_FAILURE;
  }
  *put_event = s_ring[s_head];
  s_head     = (s_head + 1) % K_BARE_EVENT_RING;
  s_count--;
  return GX_SUCCESS;
}

VOID gx_generic_timer_start(VOID) {}

VOID gx_generic_timer_stop(VOID) {}

VOID gx_generic_system_mutex_lock(VOID) {}

VOID gx_generic_system_mutex_unlock(VOID) {}

ULONG gx_generic_system_time_get(VOID)
{
  /* Target time source: the SysTick-backed 1 kHz millisecond counter
   * (ra_time_ms), converted to GUIX timer ticks. On board_sim the SysTick
   * handler is fired cooperatively between emulation chunks, so this value
   * advances even in the headless render run. Wrapping is fine: GUIX compares
   * timer deltas, not absolutes. */
  const unsigned long ms = (unsigned long)ra_time_ms();
  return (ULONG)(ms / (unsigned long)GX_SYSTEM_TIMER_MS);
}

VOID* gx_generic_thread_identify(VOID)
{
  return &s_thread_token;
}

VOID gx_generic_time_delay(INT ticks)
{
  (void)ticks;
}

void gx_host_pump(void)
{
  /* Single-threaded drive: GUIX's own thread loop never runs, so drain the
   * generic event ring and dispatch each event here. Events generated while
   * dispatching (e.g. a button's GX_EVENT_CLICKED) are posted back into the
   * ring and picked up by the same loop before it drains. The caller follows
   * up with gx_system_canvas_refresh() to repaint. Identical body to the host
   * bind so the panel app and the host preview share the exact drive model. */
  GX_EVENT ev;
  while (gx_generic_event_pop(&ev, GX_FALSE) == GX_SUCCESS) {
    (void)_gx_system_event_dispatch(&ev);
  }
}

#endif /* !RA_SIMULATOR_MODE */
