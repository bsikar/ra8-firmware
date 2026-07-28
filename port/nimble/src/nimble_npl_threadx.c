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
 * service. This file also carries small stubs of the upstream
 * ``ble_transport_alloc_*`` / ``os_mbuf_*`` symbols so the per-app
 * build can link our adapter even before the curated upstream
 * NimBLE host TUs are wired into the build (the heavyweight host
 * stack is opt-in via ``RA8_USE_NIMBLE_HOST`` -- without it we still
 * provide a link-only scaffold so the demo links).
 *
 * Tick rate: ``TX_TIMER_TICKS_PER_SECOND = 1000`` (see
 * ``port/threadx/inc/tx_user.h``), so 1 NPL tick == 1 ms.
 *
 * @warning UNVALIDATED SCAFFOLD (issue #286): this NimBLE port and its
 * ThreadX Native Porting Layer link and pass the static gates, but have
 * NEVER been hardware-validated and are NOT emulator-gated -- ra8_emulator models
 * no RA8D2 BLE controller / HCI mailbox, and the underlying ra8_ble
 * transport is itself unproven on this board (see #86, #91). Treat every
 * symbol here as a link-only stub, not a working BLE stack. Consumers stay
 * under ``examples/_unsupported/`` until a NimBLE app is driven to real
 * hardware validation and promoted out of that tier.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "nimble_npl_threadx.h"

#include <stdint.h>
#include <string.h>

#include "nimble_transport_stubs.h"
#include "tx_api.h"

/* =============================================================================
 * NPL public API forward decls (matches upstream nimble/nimble_npl.h)
 * =============================================================================
 */

/* =============================================================================
 * Generic helpers
 * =============================================================================
 */

/**
 * @enum ble_npl_threadx_const_t
 * @brief Local sizing/timeout constants.
 */
typedef enum : uint32_t {
  k_ble_npl_tx_wait_forever = 0xFFFFFFFFU, /**< TX_WAIT_FOREVER literal. */
  k_ble_npl_tx_no_wait      = 0U,          /**< TX_NO_WAIT literal.      */
  k_ble_npl_ms_per_sec      = 1000U,       /**< Helper: ms / second.     */
} ble_npl_threadx_const_t;

/* Translate an NPL timeout into a ThreadX wait code -- see implementation for details. */
static ULONG priv_tmo_to_tx(ble_npl_time_t tmo)
{
  if (tmo == (ble_npl_time_t)k_ble_npl_threadx_wait_forever) {
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
  const UINT st = tx_mutex_create(&mu->handle, "npl_mu", TX_NO_INHERIT);
  return (st == TX_SUCCESS) ? BLE_NPL_OK : BLE_NPL_ERROR;
}

/* Pend on an NPL mutex with timeout -- see implementation for details. */
ble_npl_error_t ble_npl_mutex_pend(struct ble_npl_mutex* mu, ble_npl_time_t timeout)
{
  if (mu == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  const UINT st = tx_mutex_get(&mu->handle, priv_tmo_to_tx(timeout));
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
  const UINT st = tx_semaphore_create(&sem->handle, "npl_sem", (ULONG)tokens);
  return (st == TX_SUCCESS) ? BLE_NPL_OK : BLE_NPL_ERROR;
}

/* Pend on an NPL semaphore -- see implementation for details. */
ble_npl_error_t ble_npl_sem_pend(struct ble_npl_sem* sem, ble_npl_time_t timeout)
{
  if (sem == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
  const UINT st = tx_semaphore_get(&sem->handle, priv_tmo_to_tx(timeout));
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
  (void)tx_queue_create(&evq->q, "npl_evq", TX_1_ULONG, evq->storage, (ULONG)sizeof(evq->storage));
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
  if (tx_queue_receive(&evq->q, &slot, priv_tmo_to_tx(tmo)) != TX_SUCCESS) {
    return nullptr;
  }
  struct ble_npl_event* ev = (struct ble_npl_event*)(uintptr_t)slot; /**< Slot. */
  if (ev != nullptr) {
    ev->queued = 0U;
  }
  return ev; /**< Ev. */
}

/* Post an event into an NPL event queue (idempotent) -- see implementation for details. */
void ble_npl_eventq_put(struct ble_npl_eventq* evq, struct ble_npl_event* ev)
{
  if (evq == nullptr || ev == nullptr) {
    return;
  }
  if (ev->queued != 0U) {
    return;
  }
  ev->queued = 1U;
  ULONG slot = (ULONG)(uintptr_t)ev;
  (void)tx_queue_send(&evq->q, &slot, TX_NO_WAIT);
}

/* Best-effort removal of an event from a queue -- see implementation for details. */
void ble_npl_eventq_remove(struct ble_npl_eventq* evq, struct ble_npl_event* ev)
{
  (void)evq;
  if (ev == nullptr) {
    return;
  }
  ev->queued = 0U;
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

/* ThreadX timer trampoline: posts our event into its eventq -- see implementation for details. */
static void priv_callout_trampoline(ULONG arg)
{
  struct ble_npl_callout* co = (struct ble_npl_callout*)(uintptr_t)arg;
  if (co == nullptr) {
    return;
  }
  ble_npl_eventq_put(co->evq, &co->ev);
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
  (void)memset(co, 0, sizeof(*co));
  co->evq    = evq;
  co->ev.fn  = ev_cb;
  co->ev.arg = ev_arg;
  (void)tx_timer_create(&co->handle,
                        "npl_co",
                        priv_callout_trampoline,
                        (ULONG)(uintptr_t)co,
                        1U, /* initial-ticks (placeholder, reset on arm) */
                        0U, /* reschedule-ticks (one-shot)               */
                        TX_NO_ACTIVATE);
}

/* Arm the callout to fire ``ticks`` from now -- see implementation for details. */
ble_npl_error_t ble_npl_callout_reset(struct ble_npl_callout* co, ble_npl_time_t ticks)
{
  if (co == nullptr) {
    return BLE_NPL_INVALID_PARAM;
  }
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
  return (uint32_t)tx_interrupt_control(TX_INT_DISABLE);
}

/* Exit a critical section, restoring the previous int mask -- see implementation for details. */
void ble_npl_hw_exit_critical(uint32_t ctx)
{
  (void)tx_interrupt_control((UINT)ctx);
}

/**
 * @brief Test whether the calling context is inside a critical section.
 *
 * @return 1 when inside, 0 otherwise.
 *
 * @details Best-effort: ThreadX does not expose a "primask read"
 * helper, so we always report 0 (NimBLE uses this for assertions
 * only).
 *
 * @since 0.1.0
 *
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 */
bool ble_npl_hw_is_in_critical(void)
{
  return false;
}

/* =============================================================================
 * NimBLE port + transport stubs
 * =============================================================================
 *
 * These provide minimal definitions so the example app and the
 * companion `ble_hci_ra8_ble.c` link without pulling the heavyweight
 * upstream NimBLE host TUs into this curated build. When the host
 * stack is later wired in (Phase 1.4 / 1.5), these stubs are
 * superseded by the upstream symbols.
 */

/**
 * @brief Default eventq used by the host stack.
 *
 * @details Lives in BSS; ``ble_npl_eventq_init`` is called once at
 * boot. Surfaced via ``nimble_port_get_dflt_eventq``.
 */
static struct ble_npl_eventq s_dflt_eventq;

/** @brief Initialized flag for the default eventq. */
static uint8_t s_dflt_eventq_ready = 0U;

/* Bring the default eventq up -- see implementation for details. */
void nimble_port_init(void)
{
  if (s_dflt_eventq_ready == 0U) {
    ble_npl_eventq_init(&s_dflt_eventq);
    s_dflt_eventq_ready = 1U;
  }
}

/**
 * @brief Return the address of the default eventq.
 *
 * @return Pointer to the default ``ble_npl_eventq`` (never NULL).
 *
 * @pre ``nimble_port_init`` has been called.
 * @post Caller can pass the returned pointer to ``ble_npl_eventq_put``.
 *
 * @since 0.1.0
 */
struct ble_npl_eventq* nimble_port_get_dflt_eventq(void)
{
  return &s_dflt_eventq; /**< S dflt eventq. */
}

/**
 * @brief Run the NimBLE host event loop forever.
 *
 * @details Pumps events out of the default eventq; never returns
 * under normal operation.
 *
 * @pre ``nimble_port_init`` has been called.
 * @post Calling thread runs the host loop until shutdown.
 *
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void nimble_port_run(void)
{
  while (1) {
    struct ble_npl_event* ev =
      ble_npl_eventq_get(&s_dflt_eventq, (ble_npl_time_t)k_ble_npl_threadx_wait_forever);
    if (ev != nullptr) {
      ble_npl_event_run(ev);
    }
  }
}

/* =============================================================================
 * NimBLE transport allocator stubs
 * =============================================================================
 *
 * These are deliberately minimal: the curated build does not pull in
 * the upstream nimble/transport object library yet, but
 * `ble_hci_ra8_ble.c` references ``ble_transport_alloc_evt`` /
 * ``ble_transport_alloc_acl_from_ll`` / ``ble_transport_to_hs_evt``
 * / ``ble_transport_to_hs_acl`` / ``ble_transport_free`` /
 * ``os_mbuf_*``. We provide weak no-op definitions so the demo app
 * links; once the host stack lands these are replaced by the real
 * symbols at link time (the linker prefers strong over weak).
 */

[[gnu::weak]] void* ble_transport_alloc_evt(int discardable)
{
  (void)discardable;
  return nullptr;
}

[[gnu::weak]] struct os_mbuf* ble_transport_alloc_acl_from_ll(void)
{
  return nullptr;
}

[[gnu::weak]] void ble_transport_free(void* buf)
{
  (void)buf;
}

[[gnu::weak]] int ble_transport_to_hs_evt(void* buf)
{
  (void)buf;
  return 0;
}

[[gnu::weak]] int ble_transport_to_hs_acl(struct os_mbuf* om)
{
  (void)om;
  return 0;
}

[[gnu::weak]] int os_mbuf_append(struct os_mbuf* om, const void* data, uint16_t len)
{
  (void)om;
  (void)data;
  (void)len;
  return 0;
}

[[gnu::weak]] int os_mbuf_free_chain(struct os_mbuf* om)
{
  (void)om;
  return 0;
}

[[gnu::weak]] uint16_t os_mbuf_len(const struct os_mbuf* om)
{
  (void)om;
  return 0U;
}

[[gnu::weak]] int os_mbuf_copydata(const struct os_mbuf* om, int off, int len, void* dst)
{
  (void)om;
  (void)off;
  (void)len;
  (void)dst;
  return 0;
}

[[gnu::weak]] int ble_transport_to_ll_cmd(void* buf)
{
  return ble_transport_to_ll_cmd_impl(buf);
}

[[gnu::weak]] int ble_transport_to_ll_acl(struct os_mbuf* om)
{
  return ble_transport_to_ll_acl_impl(om);
}
