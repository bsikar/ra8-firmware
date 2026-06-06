/* Generic single-threaded GUIX RTOS binding for the host (macOS) spike.
 *
 * GUIX, when built with GX_DISABLE_THREADX_BINDING, calls these gx_generic_*
 * functions instead of ThreadX. We run GUIX single-threaded and drive it from
 * the application loop (gx_system_canvas_refresh / _gx_system_event_dispatch),
 * so:
 *   - thread_start does NOT spawn anything (the app drives GUIX),
 *   - the event queue is a small static ring,
 *   - mutex calls are no-ops (single-threaded),
 *   - time comes from the monotonic clock.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <time.h>

#include "gx_api.h"
#include "gx_host_run.h"
#include "gx_system.h"

enum { K_HOST_EVENT_RING = 64 };

static GX_EVENT s_ring[K_HOST_EVENT_RING];
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
  /* Single-threaded host: do not run GUIX's event loop on a thread; the
   * application pumps GUIX directly. Just acknowledge. */
  (void)thread_entry;
  return GX_SUCCESS;
}

UINT gx_generic_event_post(GX_EVENT* event_ptr)
{
  if (event_ptr == GX_NULL) {
    return GX_FAILURE;
  }
  if (s_count >= K_HOST_EVENT_RING) {
    return GX_FAILURE;
  }
  s_ring[s_tail] = *event_ptr;
  s_tail         = (s_tail + 1) % K_HOST_EVENT_RING;
  s_count++;
  return GX_SUCCESS;
}

UINT gx_generic_event_fold(GX_EVENT* event_ptr)
{
  /* Spike: no coalescing -- just enqueue. */
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
  s_head     = (s_head + 1) % K_HOST_EVENT_RING;
  s_count--;
  return GX_SUCCESS;
}

VOID gx_generic_timer_start(VOID) {}

VOID gx_generic_timer_stop(VOID) {}

VOID gx_generic_system_mutex_lock(VOID) {}

VOID gx_generic_system_mutex_unlock(VOID) {}

ULONG gx_generic_system_time_get(VOID)
{
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  unsigned long ms = (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L);
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
   * up with gx_system_canvas_refresh() to repaint. */
  GX_EVENT ev;
  while (gx_generic_event_pop(&ev, GX_FALSE) == GX_SUCCESS) {
    (void)_gx_system_event_dispatch(&ev);
  }
}
