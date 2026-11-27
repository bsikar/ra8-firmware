/**
 * @file tests/wireless/src/nimble_npl_host.c
 * @brief Deterministic host-side NimBLE NPL backend for wireless integration tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nimble/nimble_npl.h"

typedef enum : uint32_t {
  k_nimble_host_sem_pump_limit = 64U,
  k_nimble_host_max_callouts   = 32U,
  k_nimble_host_sem_max        = 0xFFFFU,
} nimble_host_const_t;

static ble_npl_time_t            s_now_ticks    = 0U;
static uint32_t                  s_crit_nesting = 0U;
static nimble_npl_host_pump_fn_t s_pump_cb      = nullptr;
static void*                     s_pump_ctx     = nullptr;
static struct ble_npl_callout*   s_callouts[k_nimble_host_max_callouts];
static uint32_t                  s_callout_count = 0U;

void nimble_npl_host_set_pump(nimble_npl_host_pump_fn_t pump_cb, void* ctx)
{
  s_pump_cb  = pump_cb;
  s_pump_ctx = ctx;
}

void nimble_npl_host_reset(void)
{
  s_now_ticks     = 0U;
  s_crit_nesting  = 0U;
  s_pump_cb       = nullptr;
  s_pump_ctx      = nullptr;
  s_callout_count = 0U;
  (void)memset(s_callouts, 0, sizeof(s_callouts));
}

void nimble_npl_host_tick(uint32_t delta_ms)
{
  s_now_ticks += (ble_npl_time_t)delta_ms;
  for (uint32_t i = 0U; i < s_callout_count; i++) {
    struct ble_npl_callout* const co = s_callouts[i];
    if ((co == nullptr) || !co->active) {
      continue;
    }
    if ((ble_npl_stime_t)(s_now_ticks - co->ticks) >= 0) {
      co->active = false;
      if (co->evq != nullptr) {
        ble_npl_eventq_put(co->evq, &co->ev);
      } else if (co->ev.fn != nullptr) {
        co->ev.fn(&co->ev);
      }
    }
  }
}

bool ble_npl_os_started(void)
{
  return true;
}

void* ble_npl_get_current_task_id(void)
{
  static uint8_t s_main_task_token;
  return &s_main_task_token;
}

void ble_npl_eventq_init(struct ble_npl_eventq* evq)
{
  if (evq != nullptr) {
    (void)memset(evq, 0, sizeof(*evq));
  }
}

struct ble_npl_event* ble_npl_eventq_get(struct ble_npl_eventq* evq, ble_npl_time_t tmo)
{
  if (evq == nullptr) {
    return nullptr;
  }
  if ((evq->count == 0U) && (tmo != 0U) && (s_pump_cb != nullptr)) {
    s_pump_cb(s_pump_ctx);
  }
  if (evq->count == 0U) {
    return nullptr;
  }
  struct ble_npl_event* const ev = evq->ring[evq->head];
  evq->ring[evq->head]           = nullptr;
  evq->head                      = (uint16_t)((evq->head + 1U) % (uint16_t)k_nimble_host_evq_cap);
  evq->count--;
  if (ev != nullptr) {
    ev->queued = 0U;
  }
  return ev;
}

void ble_npl_eventq_put(struct ble_npl_eventq* evq, struct ble_npl_event* ev)
{
  if ((evq == nullptr) || (ev == nullptr) || (ev->queued != 0U)) {
    return;
  }
  if (evq->count >= (uint16_t)k_nimble_host_evq_cap) {
    return;
  }
  evq->ring[evq->tail] = ev;
  evq->tail            = (uint16_t)((evq->tail + 1U) % (uint16_t)k_nimble_host_evq_cap);
  evq->count++;
  ev->queued = 1U;
}

void ble_npl_eventq_remove(struct ble_npl_eventq* evq, struct ble_npl_event* ev)
{
  if ((evq == nullptr) || (ev == nullptr) || (ev->queued == 0U)) {
    return;
  }
  const uint16_t n = evq->count;
  for (uint16_t i = 0U; i < n; i++) {
    struct ble_npl_event* const cur = ble_npl_eventq_get(evq, 0U);
    if ((cur != nullptr) && (cur != ev)) {
      ble_npl_eventq_put(evq, cur);
    }
  }
  ev->queued = 0U;
}

void ble_npl_event_init(struct ble_npl_event* ev, ble_npl_event_fn* fn, void* arg)
{
  if (ev != nullptr) {
    (void)memset(ev, 0, sizeof(*ev));
    ev->fn  = fn;
    ev->arg = arg;
  }
}

bool ble_npl_event_is_queued(struct ble_npl_event* ev)
{
  return (ev != nullptr) && (ev->queued != 0U);
}

void* ble_npl_event_get_arg(struct ble_npl_event* ev)
{
  return (ev == nullptr) ? nullptr : ev->arg;
}

void ble_npl_event_set_arg(struct ble_npl_event* ev, void* arg)
{
  if (ev != nullptr) {
    ev->arg = arg;
  }
}

bool ble_npl_eventq_is_empty(struct ble_npl_eventq* evq)
{
  return (evq == nullptr) || (evq->count == 0U);
}

void ble_npl_event_run(struct ble_npl_event* ev)
{
  if ((ev != nullptr) && (ev->fn != nullptr)) {
    ev->fn(ev);
  }
}

ble_npl_error_t ble_npl_mutex_init(struct ble_npl_mutex* mu)
{
  if (mu == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  mu->depth = 0U;
  return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_mutex_pend(struct ble_npl_mutex* mu, ble_npl_time_t timeout)
{
  (void)timeout;
  if (mu == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  mu->depth++;
  return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_mutex_release(struct ble_npl_mutex* mu)
{
  if (mu == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  if (mu->depth > 0U) {
    mu->depth--;
  }
  return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_sem_init(struct ble_npl_sem* sem, uint16_t tokens)
{
  if (sem == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  sem->count = tokens;
  return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_sem_pend(struct ble_npl_sem* sem, ble_npl_time_t timeout)
{
  if (sem == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  if ((sem->count == 0U) && (timeout > 0U) && (s_pump_cb != nullptr)) {
    for (uint32_t i = 0U; (i < (uint32_t)k_nimble_host_sem_pump_limit) && (sem->count == 0U); i++) {
      s_pump_cb(s_pump_ctx);
    }
  }
  if (sem->count == 0U) {
    return BLE_NPL_TIMEOUT;
  }
  sem->count--;
  return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_sem_release(struct ble_npl_sem* sem)
{
  if (sem == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  if (sem->count < (uint16_t)k_nimble_host_sem_max) {
    sem->count++;
  }
  return BLE_NPL_OK;
}

uint16_t ble_npl_sem_get_count(struct ble_npl_sem* sem)
{
  return (sem == nullptr) ? 0U : sem->count;
}

void ble_npl_callout_init(struct ble_npl_callout* co,
                          struct ble_npl_eventq*  evq,
                          ble_npl_event_fn*       ev_cb,
                          void*                   ev_arg)
{
  if (co == nullptr) {
    return;
  }
  (void)memset(co, 0, sizeof(*co));
  co->evq    = evq;
  co->ev.fn  = ev_cb;
  co->ev.arg = ev_arg;
  bool seen  = false;
  for (uint32_t i = 0U; i < s_callout_count; i++) {
    if (s_callouts[i] == co) {
      seen = true;
      break;
    }
  }
  if (!seen && (s_callout_count < (uint32_t)k_nimble_host_max_callouts)) {
    s_callouts[s_callout_count] = co;
    s_callout_count++;
  }
}

ble_npl_error_t ble_npl_callout_reset(struct ble_npl_callout* co, ble_npl_time_t ticks)
{
  if (co == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  co->ticks  = s_now_ticks + ticks;
  co->active = true;
  return BLE_NPL_OK;
}

void ble_npl_callout_stop(struct ble_npl_callout* co)
{
  if (co != nullptr) {
    co->active = false;
  }
}

bool ble_npl_callout_is_active(struct ble_npl_callout* co)
{
  return (co != nullptr) && co->active;
}

ble_npl_time_t ble_npl_callout_get_ticks(struct ble_npl_callout* co)
{
  return (co == nullptr) ? 0U : co->ticks;
}

ble_npl_time_t ble_npl_callout_remaining_ticks(struct ble_npl_callout* co, ble_npl_time_t time)
{
  if (co == nullptr) {
    return 0U;
  }
  const ble_npl_stime_t delta = (ble_npl_stime_t)(co->ticks - time);
  return (delta > 0) ? (ble_npl_time_t)delta : 0U;
}

void ble_npl_callout_set_arg(struct ble_npl_callout* co, void* arg)
{
  if (co != nullptr) {
    co->ev.arg = arg;
  }
}

ble_npl_time_t ble_npl_time_get(void)
{
  return s_now_ticks;
}

ble_npl_error_t ble_npl_time_ms_to_ticks(uint32_t ms, ble_npl_time_t* out_ticks)
{
  if (out_ticks == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  *out_ticks = (ble_npl_time_t)ms;
  return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_time_ticks_to_ms(ble_npl_time_t ticks, uint32_t* out_ms)
{
  if (out_ms == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  *out_ms = (uint32_t)ticks;
  return BLE_NPL_OK;
}

ble_npl_time_t ble_npl_time_ms_to_ticks32(uint32_t ms)
{
  return (ble_npl_time_t)ms;
}

uint32_t ble_npl_time_ticks_to_ms32(ble_npl_time_t ticks)
{
  return (uint32_t)ticks;
}

void ble_npl_time_delay(ble_npl_time_t ticks)
{
  nimble_npl_host_tick((uint32_t)ticks);
}

uint32_t ble_npl_hw_enter_critical(void)
{
  const uint32_t prev = s_crit_nesting;
  s_crit_nesting++;
  return prev;
}

void ble_npl_hw_exit_critical(uint32_t ctx)
{
  s_crit_nesting = ctx;
}

bool ble_npl_hw_is_in_critical(void)
{
  return s_crit_nesting > 0U;
}
