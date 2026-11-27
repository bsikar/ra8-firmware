/**
 * @file port/nimble/src/nimble_npl_threadx.c
 * @brief NimBLE Native Porting Layer mapping onto Eclipse ThreadX
 *
 * @par Tag
 * [Ring 4 / PORT] {World: S}
 *
 * @details
 * Implementation of the NimBLE NPL primitives on top of ThreadX.
 * Each ``ble_npl_*`` symbol delegates to the matching ``tx_*``
 * service while upstream ``nimble_port.c``, ``nimble/transport/``, and
 * ``nimble/host/`` provide the host runtime symbols.
 *
 * Tick rate: ``TX_TIMER_TICKS_PER_SECOND = 1000`` (see
 * ``port/threadx/inc/tx_user.h``), so 1 NPL tick == 1 ms.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "nimble_npl_threadx.h"

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "tx_api.h"

/** @brief Active ThreadX critical-section nesting counter. */
static volatile uint32_t s_critical_nesting = 0U;

/* =============================================================================
 * Generic helpers
 * =============================================================================
 */

/* Check whether ThreadX scheduler has started -- see nimble/nimble_npl.h. */
bool ble_npl_os_started(void)
{
  return tx_thread_identify() != nullptr;
}

/* Identify the active ThreadX thread -- see nimble/nimble_npl.h. */
void* ble_npl_get_current_task_id(void)
{
  return (void*)tx_thread_identify();
}

/**
 * @enum ble_npl_threadx_const_t
 * @brief Local sizing/timeout constants.
 */
typedef enum : uint32_t {
  k_ble_npl_tx_wait_forever = 0xFFFFFFFFU, /**< TX_WAIT_FOREVER literal. */
  k_ble_npl_tx_no_wait      = 0U,          /**< TX_NO_WAIT literal.      */
  k_ble_npl_ms_per_sec      = 1000U,       /**< Helper: ms / second.     */
} ble_npl_threadx_const_t;

/**
 * @brief Translate an NPL timeout into a ThreadX wait code.
 *
 * @details Preserves finite tick counts and maps the NPL forever sentinel to
 * ThreadX's exact wait-forever value.
 *
 * @param[in] tmo NPL timeout value to translate.
 * @return ThreadX wait-code representation of @p tmo.
 * @retval TX_WAIT_FOREVER The input requested an unbounded wait.
 * @pre @p tmo is a value accepted by the calling NPL primitive.
 * @pre The configured NPL and ThreadX tick domains use the documented rate.
 * @post Finite timeout values are returned without mutation.
 * @post The helper changes no NPL or ThreadX object state.
 * @note This conversion does not round or rescale finite tick values.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ULONG internal_tmo_to_tx(ble_npl_time_t tmo)
{
  if (tmo == (ble_npl_time_t)BLE_NPL_TIME_FOREVER) {
    return (ULONG)k_ble_npl_tx_wait_forever;
  }
  return (ULONG)tmo;
}

/* =============================================================================
 * Mutex
 * =============================================================================
 */

/* Initialise an NPL mutex -- see implementation for details. */
ble_npl_error_t ble_npl_mutex_init(struct ble_npl_mutex* mu)
{
  if (mu == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  /** @brief Mutable mutex name retained by the ThreadX control block. */
  static CHAR s_name[] = {'n', 'p', 'l', '_', 'm', 'u', '\0'};
  const UINT  st       = tx_mutex_create(&mu->handle, s_name, TX_INHERIT);
  return (st == TX_SUCCESS) ? BLE_NPL_OK : BLE_NPL_ERROR;
}

/* Pend on an NPL mutex with timeout -- see implementation for details. */
ble_npl_error_t ble_npl_mutex_pend(struct ble_npl_mutex* mu, ble_npl_time_t timeout)
{
  if (mu == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  const UINT st = tx_mutex_get(&mu->handle, internal_tmo_to_tx(timeout));
  if (st == TX_SUCCESS) {
    return BLE_NPL_OK;
  }
  return (st == TX_NOT_AVAILABLE) ? BLE_NPL_TIMEOUT : BLE_NPL_ERROR;
}

/* Release an NPL mutex -- see implementation for details. */
ble_npl_error_t ble_npl_mutex_release(struct ble_npl_mutex* mu)
{
  if (mu == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  const UINT st = tx_mutex_put(&mu->handle);
  return (st == TX_SUCCESS) ? BLE_NPL_OK : BLE_NPL_ERROR;
}

/* =============================================================================
 * Semaphore
 * =============================================================================
 */

/* Initialise an NPL counting semaphore -- see implementation for details. */
ble_npl_error_t ble_npl_sem_init(struct ble_npl_sem* sem, uint16_t tokens)
{
  if (sem == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  /** @brief Mutable semaphore name retained by the ThreadX control block. */
  static CHAR s_name[] = {'n', 'p', 'l', '_', 's', 'e', 'm', '\0'};
  const UINT  st       = tx_semaphore_create(&sem->handle, s_name, (ULONG)tokens);
  return (st == TX_SUCCESS) ? BLE_NPL_OK : BLE_NPL_ERROR;
}

/* Pend on an NPL semaphore -- see implementation for details. */
ble_npl_error_t ble_npl_sem_pend(struct ble_npl_sem* sem, ble_npl_time_t timeout)
{
  if (sem == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  const UINT st = tx_semaphore_get(&sem->handle, internal_tmo_to_tx(timeout));
  if (st == TX_SUCCESS) {
    return BLE_NPL_OK;
  }
  return (st == TX_NO_INSTANCE) ? BLE_NPL_TIMEOUT : BLE_NPL_ERROR;
}

/* Release a token back into an NPL semaphore -- see implementation for details. */
ble_npl_error_t ble_npl_sem_release(struct ble_npl_sem* sem)
{
  if (sem == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  const UINT st = tx_semaphore_put(&sem->handle);
  return (st == TX_SUCCESS) ? BLE_NPL_OK : BLE_NPL_ERROR;
}

/* Read the current token count of an NPL semaphore -- see implementation for details. */
uint16_t ble_npl_sem_get_count(struct ble_npl_sem* sem)
{
  if (sem == nullptr) {
    return 0U;
  }
  ULONG current = 0U;
  (void)tx_semaphore_info_get(&sem->handle, nullptr, &current, nullptr, nullptr, nullptr);
  return (uint16_t)current;
}

/* =============================================================================
 * Event + event queue
 * =============================================================================
 */

/* Initialise an NPL event -- see implementation for details. */
void ble_npl_event_init(struct ble_npl_event* ev, ble_npl_event_fn* fn, void* arg)
{
  if (ev == nullptr) {
    return;
  }
  (void)memset(ev, 0, sizeof(*ev));
  ev->fn  = fn;
  ev->arg = arg;
}

/* Initialise an NPL event queue -- see implementation for details. */
void ble_npl_eventq_init(struct ble_npl_eventq* evq)
{
  if (evq == nullptr) {
    return;
  }
  (void)memset(evq, 0, sizeof(*evq));
  /** @brief Mutable queue name retained by the ThreadX control block. */
  static CHAR s_name[] = {'n', 'p', 'l', '_', 'e', 'v', 'q', '\0'};
  (void)tx_queue_create(&evq->q, s_name, TX_1_ULONG, evq->storage, (ULONG)sizeof(evq->storage));
}

/**
 * @brief Wait for an event from an NPL event queue.
 *
 * @param[in,out] evq NPL event queue pointer.
 * @param[in]     tmo NPL ticks to wait; ``0xFFFFU`` is forever.
 *
 * @return Pointer to the popped event or NULL on timeout.
 *
 * @pre evq != NULL.
 * @post On non-NULL return the event has been removed from the queue.
 *
 * @since 0.1.0
 */
struct ble_npl_event* ble_npl_eventq_get(struct ble_npl_eventq* evq, ble_npl_time_t tmo)
{
  if (evq == nullptr) {
    return nullptr;
  }
  ULONG slot = 0U; /**< U. */
  if (tx_queue_receive(&evq->q, &slot, internal_tmo_to_tx(tmo)) != TX_SUCCESS) {
    return nullptr;
  }
  struct ble_npl_event* ev = (struct ble_npl_event*)(uintptr_t)slot; /**< Slot. */
  if (ev != nullptr) {
    const uint32_t sr = ble_npl_hw_enter_critical();
    ev->queued        = 0U;
    ble_npl_hw_exit_critical(sr);
  }
  return ev; /**< Ev. */
}

/* Post an event into an NPL event queue (idempotent) -- see implementation for details. */
void ble_npl_eventq_put(struct ble_npl_eventq* evq, struct ble_npl_event* ev)
{
  if (evq == nullptr || ev == nullptr) {
    return;
  }
  const uint32_t sr = ble_npl_hw_enter_critical();
  if (ev->queued == 0U) {
    ULONG slot = (ULONG)(uintptr_t)ev;
    if (tx_queue_send(&evq->q, &slot, TX_NO_WAIT) == TX_SUCCESS) {
      ev->queued = 1U;
    }
  }
  ble_npl_hw_exit_critical(sr);
}

/* Bounded queue rotation removing ``ev`` from ``evq`` -- see implementation for details. */
void ble_npl_eventq_remove(struct ble_npl_eventq* evq, struct ble_npl_event* ev)
{
  if (evq == nullptr || ev == nullptr) {
    return;
  }
  const uint32_t sr = ble_npl_hw_enter_critical();
  if (ev->queued != 0U) {
    ULONG enqueued = 0U;
    if (tx_queue_info_get(&evq->q, nullptr, &enqueued, nullptr, nullptr, nullptr, nullptr) ==
        TX_SUCCESS) {
      if (enqueued > (ULONG)k_ble_npl_threadx_eventq_depth) {
        enqueued = (ULONG)k_ble_npl_threadx_eventq_depth;
      }
      const ULONG target = (ULONG)(uintptr_t)ev;
      for (ULONG i = 0U; i < enqueued; i++) {
        ULONG slot = 0U;
        if (tx_queue_receive(&evq->q, &slot, TX_NO_WAIT) != TX_SUCCESS) {
          break;
        }
        if (slot != target) {
          (void)tx_queue_send(&evq->q, &slot, TX_NO_WAIT);
        }
      }
    }
    ev->queued = 0U;
  }
  ble_npl_hw_exit_critical(sr);
}

/* Run an NPL event by invoking its callback -- see implementation for details. */
void ble_npl_event_run(struct ble_npl_event* ev)
{
  if (ev == nullptr || ev->fn == nullptr) {
    return;
  }
  ev->fn(ev);
}

/* Test whether an event is currently queued -- see implementation for details. */
bool ble_npl_event_is_queued(struct ble_npl_event* ev)
{
  return (ev != nullptr) && (ev->queued != 0U);
}

/**
 * @brief Read the user-arg pointer attached to an event.
 *
 * @param[in] ev NPL event pointer.
 *
 * @return The user argument or NULL when ev is NULL.
 *
 * @pre ev may be NULL.
 *
 * @since 0.1.0
 */
void* ble_npl_event_get_arg(struct ble_npl_event* ev)
{
  return (ev == nullptr) ? nullptr : ev->arg;
}

/* Replace the user-arg pointer attached to an event -- see implementation for details. */
void ble_npl_event_set_arg(struct ble_npl_event* ev, void* arg)
{
  if (ev != nullptr) {
    ev->arg = arg;
  }
}

/* True when the event queue is empty -- see implementation for details. */
bool ble_npl_eventq_is_empty(struct ble_npl_eventq* evq)
{
  if (evq == nullptr) {
    return true;
  }
  ULONG enqueued = 0U;
  if (tx_queue_info_get(&evq->q, nullptr, &enqueued, nullptr, nullptr, nullptr, nullptr) !=
      TX_SUCCESS) {
    return true;
  }
  return enqueued == 0U;
}

/* =============================================================================
 * Callout (one-shot timer that posts an event)
 * =============================================================================
 */

/**
 * @brief Post a callout event from the ThreadX timer trampoline.
 *
 * @details Recovers the caller-owned NPL callout from the timer argument and
 * queues its embedded event when the recovered pointer is valid.
 *
 * @param[in] arg Integer representation of the callout pointer supplied at
 * timer creation.
 * @pre A nonzero @p arg identifies a live caller-owned callout object.
 * @pre The callout's event queue remains bound for the timer lifetime.
 * @post A valid callout has its embedded event offered to its event queue.
 * @post A null argument returns without mutating queue state.
 * @note The queue operation retains the NPL event object's normal ownership.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_callout_trampoline(ULONG arg)
{
  struct ble_npl_callout* co = (struct ble_npl_callout*)(uintptr_t)arg;
  if (co == nullptr) {
    return;
  }
  if (co->evq != nullptr) {
    ble_npl_eventq_put(co->evq, &co->ev);
  } else if (co->ev.fn != nullptr) {
    co->ev.fn(&co->ev);
  }
}

/* Initialise an NPL callout (does not start it) -- see implementation for details. */
void ble_npl_callout_init(struct ble_npl_callout* co,
                          struct ble_npl_eventq*  evq,
                          ble_npl_event_fn*       ev_cb,
                          void*                   ev_arg)
{
  if (co == nullptr) {
    return;
  }
  if (co->initialized != 0U) {
    (void)tx_timer_deactivate(&co->handle);
    (void)tx_timer_delete(&co->handle);
  }
  (void)memset(co, 0, sizeof(*co));
  co->evq    = evq;
  co->ev.fn  = ev_cb;
  co->ev.arg = ev_arg;
  /** @brief Mutable timer name retained by the ThreadX control block. */
  static CHAR s_name[] = {'n', 'p', 'l', '_', 'c', 'o', '\0'};
  if (tx_timer_create(&co->handle,
                      s_name,
                      internal_callout_trampoline,
                      (ULONG)(uintptr_t)co,
                      1U, /* initial-ticks (placeholder, reset on arm) */
                      0U, /* reschedule-ticks (one-shot)               */
                      TX_NO_ACTIVATE) == TX_SUCCESS) {
    co->initialized = 1U;
  }
}

/* Arm the callout to fire ``ticks`` from now -- see implementation for details. */
ble_npl_error_t ble_npl_callout_reset(struct ble_npl_callout* co, ble_npl_time_t ticks)
{
  if (co == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  co->ticks = ble_npl_time_get() + ticks;
  if (ticks == 0U) {
    ticks = 1U;
  }
  (void)tx_timer_deactivate(&co->handle);
  (void)tx_timer_change(&co->handle, (ULONG)ticks, 0U);
  const UINT st = tx_timer_activate(&co->handle);
  return (st == TX_SUCCESS) ? BLE_NPL_OK : BLE_NPL_ERROR;
}

/* Stop the callout if it is armed -- see implementation for details. */
void ble_npl_callout_stop(struct ble_npl_callout* co)
{
  if (co == nullptr) {
    return;
  }
  (void)tx_timer_deactivate(&co->handle);
}

/* Test whether the callout is armed -- see implementation for details. */
bool ble_npl_callout_is_active(struct ble_npl_callout* co)
{
  if (co == nullptr) {
    return false;
  }
  UINT active = 0U;
  if (tx_timer_info_get(&co->handle, nullptr, &active, nullptr, nullptr, nullptr) != TX_SUCCESS) {
    return false;
  }
  return active != 0U;
}

/* Return the tick deadline recorded when the callout was last reset. */
ble_npl_time_t ble_npl_callout_get_ticks(struct ble_npl_callout* co)
{
  return (co == nullptr) ? 0U : co->ticks;
}

/* Return remaining ticks until the armed callout deadline (0 if expired). */
ble_npl_time_t ble_npl_callout_remaining_ticks(struct ble_npl_callout* co, ble_npl_time_t time)
{
  if (co == nullptr) {
    return 0U;
  }
  const ble_npl_stime_t delta = (ble_npl_stime_t)(co->ticks - time);
  return (delta > 0) ? (ble_npl_time_t)delta : 0U;
}

/* Replace the callout's event arg pointer -- see implementation for details. */
void ble_npl_callout_set_arg(struct ble_npl_callout* co, void* arg)
{
  if (co != nullptr) {
    co->ev.arg = arg;
  }
}

/* =============================================================================
 * Time
 * =============================================================================
 */

/* Read the kernel tick counter -- see implementation for details. */
ble_npl_time_t ble_npl_time_get(void)
{
  return (ble_npl_time_t)tx_time_get();
}

/* Convert ms to NPL ticks (lossless when tick == 1 ms) -- see implementation for details. */
ble_npl_error_t ble_npl_time_ms_to_ticks(uint32_t ms, ble_npl_time_t* out_ticks)
{
  if (out_ticks == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  /* Project tick rate is 1000 Hz -> 1 tick == 1 ms. */
  *out_ticks = (ble_npl_time_t)ms;
  return BLE_NPL_OK;
}

/* Convert NPL ticks to ms -- see implementation for details. */
ble_npl_error_t ble_npl_time_ticks_to_ms(ble_npl_time_t ticks, uint32_t* out_ms)
{
  if (out_ms == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  *out_ms = (uint32_t)ticks;
  return BLE_NPL_OK;
}

/* Convert ms directly to 32-bit NPL ticks (1000 Hz tick base). */
ble_npl_time_t ble_npl_time_ms_to_ticks32(uint32_t ms)
{
  return (ble_npl_time_t)ms;
}

/* Convert NPL ticks directly to 32-bit milliseconds (1000 Hz tick base). */
uint32_t ble_npl_time_ticks_to_ms32(ble_npl_time_t ticks)
{
  return (uint32_t)ticks;
}

/* Sleep for the given number of NPL ticks -- see implementation for details. */
void ble_npl_time_delay(ble_npl_time_t ticks)
{
  (void)tx_thread_sleep((ULONG)ticks);
}

/* =============================================================================
 * Critical section
 * =============================================================================
 */

/* Enter a critical section by disabling interrupts -- see implementation for details. */
uint32_t ble_npl_hw_enter_critical(void)
{
  const UINT prev = tx_interrupt_control(TX_INT_DISABLE);
  s_critical_nesting += 1U;
  return (uint32_t)prev;
}

/* Exit a critical section, restoring the previous int mask -- see implementation for details. */
void ble_npl_hw_exit_critical(uint32_t ctx)
{
  if (s_critical_nesting > 0U) {
    s_critical_nesting -= 1U;
  }
  (void)tx_interrupt_control((UINT)ctx);
}

/* Report whether execution is currently inside an NPL critical section. */
bool ble_npl_hw_is_in_critical(void)
{
  return s_critical_nesting > 0U;
}
