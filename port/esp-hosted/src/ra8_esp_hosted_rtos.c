/**
 * @file port/esp-hosted/src/ra8_esp_hosted_rtos.c
 * @brief Thread, sleep, timer and clock vtable slots, plus the bind entry.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * See `port/esp-hosted/src/ra8_esp_hosted_rtos_internal.h` for the contracts.
 * Every kernel object the vendored core asks for is taken from a fixed table
 * in this translation unit -- there is no heap on this board -- and the
 * handles the core stores are the addresses of those table rows. The memory
 * and queue halves live in `ra8_esp_hosted_rtos_pool.c`, which also owns the
 * host-build ThreadX model's shared state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_esp_hosted_rtos_internal.h"
#include "ra8_log.h"

#ifdef RA8_SIMULATOR_MODE
#include "ra8_esp_hosted_tx_shim_sync_internal.h"
#else
#include "tx_api.h"
#endif

#include "port_esp_hosted_host_os.h"

/**
 * @var s_tag
 * @brief Log tag for the synchronisation half of the port.
 * @details Shared by every diagnostic this translation unit emits.
 * @note Static; do not access outside this TU.
 * @warning Changing it changes log-scraping expectations on the bench.
 * @since 0.1.0
 */
static const char* s_tag = "ESPH_RTOS";

/**
 * @enum ra8_esp_hosted_rtos_const_t
 * @brief Numeric constants of the object tables, sleeps and spin sizing.
 * @details Collected here so the timing assumptions the port makes are stated
 * once and are visible to anyone changing the kernel tick rate.
 * @invariant ::k_ra8_esp_hosted_ms_per_tick matches
 *            ``TX_TIMER_TICKS_PER_SECOND`` in ``port/threadx/inc/tx_user.h``.
 * @invariant ::k_ra8_esp_hosted_thread_stack_bytes is at least
 *            ``RPC_TASK_STACK_SIZE``, the deepest stack the core asks for.
 * @par Example:
 * @code
 * static_assert(k_ra8_esp_hosted_ms_per_tick == 1U, "1 kHz kernel tick");
 * @endcode
 * @see ra8_esp_hosted_rtos_ms_to_ticks
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_esp_hosted_ms_per_tick          = 1U,          /**< Milliseconds in one kernel tick. */
  k_ra8_esp_hosted_us_per_ms            = 1000U,       /**< Microseconds in one millisecond. */
  k_ra8_esp_hosted_ms_per_sec           = 1000U,       /**< Milliseconds in one second.      */
  k_ra8_esp_hosted_hz_per_mhz           = 1000000U,    /**< Hertz in one megahertz.          */
  k_ra8_esp_hosted_spin_cycles_per_iter = 4U,          /**< Core cycles per spin pass.       */
  k_ra8_esp_hosted_spin_iters_max       = 250000U,     /**< Spin bound (NASA Rule 2).        */
  k_ra8_esp_hosted_delay_iters_max      = 1000000U,    /**< Busy-delay iteration bound.      */
  k_ra8_esp_hosted_default_cpu_hz       = 1000000000U, /**< Fallback core rate, 1 GHz.       */
  k_ra8_esp_hosted_thread_stack_bytes   = 5120U,       /**< Bytes per thread stack.          */
  k_ra8_esp_hosted_name_max             = 16U,         /**< Object name buffer size.         */
} ra8_esp_hosted_rtos_const_t;

/**
 * @typedef ra8_esp_hosted_thread_entry_t
 * @brief Signature of the thread body the vendored core hands to the port.
 * @details Named rather than spelled inline at every use so the thread table
 * row and the ``_h_thread_create`` slot cannot drift apart, and so neither
 * declaration carries a nested function-pointer parameter that hides the
 * enclosing function's own name from a reader -- or from the MC/DC citation
 * checker, which anchors a decision to the function it sits in.
 * @param[in] arg The ``sr_arg`` the core supplied at creation.
 * @par Example:
 * @code
 * static void body(void const* arg) { (void)arg; }
 * ra8_esp_hosted_thread_entry_t entry = body;
 * @endcode
 * @see ra8_esp_hosted_thread_slot_t
 * @since 0.1.0
 */
typedef void (*ra8_esp_hosted_thread_entry_t)(void const* arg);

/**
 * @typedef ra8_esp_hosted_timer_cb_t
 * @brief Signature of the expiry callback the vendored core hands to the port.
 * @details The mirror of ::ra8_esp_hosted_thread_entry_t for timers; it takes a
 * mutable ``void*`` because that is what the core's own timer contract states.
 * @param[in] arg The ``arg`` the core supplied at start.
 * @par Example:
 * @code
 * static void on_expiry(void* arg) { (void)arg; }
 * ra8_esp_hosted_timer_cb_t cb = on_expiry;
 * @endcode
 * @see ra8_esp_hosted_timer_slot_t
 * @since 0.1.0
 */
typedef void (*ra8_esp_hosted_timer_cb_t)(void* arg);

/**
 * @struct ra8_esp_hosted_thread_slot_t
 * @brief One row of the fixed thread table.
 * @details The ThreadX control block is first, so the slot address doubles as
 * the opaque thread handle. The entry point is kept beside it because the
 * core's signature takes ``void const*`` while ThreadX passes a ``ULONG``.
 * @invariant ``entry`` is non-null while the row is in use.
 * @invariant ``name`` is always NUL-terminated.
 * @par Example:
 * @code
 * thread_handle_t t = g_h.funcs->_h_thread_create("spi_trans", 12U, 4096U, fn, nullptr);
 * @endcode
 * @see ra8_esp_hosted_rtos_bind
 * @since 0.1.0
 */
typedef struct {
  TX_THREAD                     cb;     /**< ThreadX control block; must stay first. */
  ra8_esp_hosted_thread_entry_t entry;  /**< Core-supplied thread body.              */
  void*                         arg;    /**< Argument handed to the body.            */
  char name[k_ra8_esp_hosted_name_max]; /**< Bounded name copy.                      */
} ra8_esp_hosted_thread_slot_t;

/**
 * @struct ra8_esp_hosted_timer_slot_t
 * @brief One row of the fixed timer table.
 * @details Mirrors the thread slot: control block first, then the callback
 * the core supplied, which ThreadX cannot carry because its expiry function
 * takes a ``ULONG`` rather than a pointer.
 * @invariant ``cb_fn`` is non-null while the row is in use.
 * @invariant ``name`` is always NUL-terminated.
 * @par Example:
 * @code
 * void* h = g_h.funcs->_h_timer_start("hb", 1000, H_TIMER_TYPE_PERIODIC, cb, nullptr);
 * @endcode
 * @see ra8_esp_hosted_rtos_bind
 * @since 0.1.0
 */
typedef struct {
  TX_TIMER                  cb;         /**< ThreadX control block; must stay first. */
  ra8_esp_hosted_timer_cb_t cb_fn;      /**< Core-supplied expiry callback.          */
  void*                     arg;        /**< Argument handed to the callback.        */
  char name[k_ra8_esp_hosted_name_max]; /**< Bounded name copy.                      */
} ra8_esp_hosted_timer_slot_t;

/**
 * @struct ra8_esp_hosted_rtos_state_t
 * @brief Module state of the thread, timer and clock half -- entirely static.
 * @details Holds the thread and timer tables, their in-use flags, and the two
 * words that extend the 32-bit kernel tick to 64 bits. The mutex and
 * semaphore tables live in ``ra8_esp_hosted_rtos_sync.c``.
 * @invariant ``ready`` is true exactly while the substrate is initialised.
 * @invariant ``tick_epoch`` only ever increases.
 * @par Example:
 * @code
 * TEST_ASSERT_EQ(false, ra8_esp_hosted_rtos_is_ready());
 * @endcode
 * @see ra8_esp_hosted_rtos_init
 * @since 0.1.0
 */
typedef struct {
  ra8_esp_hosted_thread_slot_t threads[k_ra8_esp_hosted_max_threads];     /**< Threads.          */
  bool                         thread_used[k_ra8_esp_hosted_max_threads]; /**< Thread occupancy. */
  ra8_esp_hosted_timer_slot_t  timers[k_ra8_esp_hosted_max_timers];       /**< Timers.           */
  bool                         timer_used[k_ra8_esp_hosted_max_timers];   /**< Timer occupancy.  */
  uint32_t                     cpu_hz;     /**< Cached core rate used to size the spin loop. */
  uint32_t                     last_ticks; /**< Most recent 32-bit tick reading.             */
  uint64_t                     tick_epoch; /**< Accumulated rollovers, in ticks.             */
  bool                         ready;      /**< True while the substrate is initialised.     */
} ra8_esp_hosted_rtos_state_t;

/**
 * @var s_rtos
 * @brief Singleton state of the thread, timer and clock half.
 * @details Zero-initialised at link time; brought up by
 * ::ra8_esp_hosted_rtos_init.
 * @note Static; do not access outside this TU.
 * @warning Direct modification bypasses every ThreadX consistency check.
 * @since 0.1.0
 */
static ra8_esp_hosted_rtos_state_t s_rtos;

/**
 * @var s_thread_stacks
 * @brief Fixed stack storage, one region per thread table row.
 * @details Sized by ::k_ra8_esp_hosted_thread_stack_bytes, which covers the
 * deepest stack the vendored core asks for.
 * @note Static; owned exclusively by the thread table.
 * @warning A thread whose requested stack exceeds one region is refused, not
 *          silently given a smaller one.
 * @since 0.1.0
 */
static uint8_t s_thread_stacks[k_ra8_esp_hosted_max_threads][k_ra8_esp_hosted_thread_stack_bytes];

/* ----------------------------------------------------------------------- */
/* Table helpers */
/* ----------------------------------------------------------------------- */

uint32_t ra8_esp_hosted_rtos_slot_take(bool* used, uint32_t count)
{
  if (used == nullptr) {
    return count;
  }
  for (uint32_t i = 0U; i < count; ++i) {
    if (!used[i]) {
      used[i] = true;
      return i;
    }
  }
  return count;
}

uint32_t ra8_esp_hosted_rtos_slot_index(const void* handle,
                                        const void* base,
                                        size_t      stride,
                                        uint32_t    count,
                                        const bool* used)
{
  if ((handle == nullptr) || (base == nullptr) || (used == nullptr)) {
    return count;
  }
  for (uint32_t i = 0U; i < count; ++i) {
    const void* const row = (const void*)((const uint8_t*)base + ((size_t)i * stride));
    if ((row == handle) && used[i]) {
      return i;
    }
  }
  return count;
}

/**
 * @brief Copy a caller-supplied object name into a fixed buffer.
 * @details ThreadX keeps the name pointer rather than the characters, so the
 * port must own storage that outlives the caller's literal. Truncates rather
 * than refusing: a shortened diagnostic name is never worth failing a create.
 *
 * The copy is a bounded character loop that writes the terminator itself
 * rather than a ``strlen``-then-``memcpy`` pair. The name comes from the
 * vendored core, so it may be longer than ``cap`` or -- on a corrupted
 * caller -- unterminated; this form reads at most ``cap`` bytes of it either
 * way, and the terminator is written on every path instead of being inferred
 * from a preceding ``memset``.
 * @param[out] dst Destination buffer.
 * @param[in] cap Destination capacity in bytes, including the terminator.
 * @param[in] src Name to copy; a null pointer yields an empty name.
 * @pre ``dst`` covers ``cap`` bytes.
 * @pre ``cap`` is at least one.
 * @post ``dst`` is NUL-terminated.
 * @post At most ``cap - 1`` characters are copied.
 * @note Thread-safe; touches only caller storage.
 * @note The loop is bounded by ``cap - 1`` (NASA Power of 10 Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_copy_name(char* dst, size_t cap, const char* src)
{
  if ((dst == nullptr) || (cap == 0U)) {
    return;
  }
  (void)memset(dst, 0, cap);
  if (src == nullptr) {
    return;
  }
  const size_t limit = cap - 1U;
  size_t       n     = 0U;
  while ((n < limit) && (src[n] != '\0')) {
    dst[n] = src[n];
    n++;
  }
  dst[n] = '\0';
}

/**
 * @brief ThreadX entry shim that calls the core-supplied thread body.
 * @details ThreadX hands a ``ULONG`` to a thread; the vendored core wants a
 * ``void const*``. The row index is passed instead of a pointer so a stale
 * entry cannot dereference freed storage.
 * @param[in] index Thread table row, passed as the ThreadX entry input.
 * @pre ``index`` names a live thread row.
 * @pre The substrate is initialised.
 * @post The core-supplied body has been entered, or nothing happened.
 * @post No table state is modified.
 * @note Runs on the created thread; not called from anywhere else.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_thread_entry(ULONG index)
{
  const uint32_t idx = (uint32_t)index;
  if (idx >= (uint32_t)k_ra8_esp_hosted_max_threads) {
    return;
  }
  if (s_rtos.thread_used[idx] && (s_rtos.threads[idx].entry != nullptr)) {
    s_rtos.threads[idx].entry(s_rtos.threads[idx].arg);
  }
}

/**
 * @brief ThreadX expiry shim that calls the core-supplied timer callback.
 * @details Same indirection as the thread entry shim, and for the same
 * reason: ThreadX carries a ``ULONG``, the core wants a ``void*``.
 * @param[in] index Timer table row, passed as the ThreadX expiry input.
 * @pre ``index`` names a live timer row.
 * @pre The substrate is initialised.
 * @post The core-supplied callback has been entered, or nothing happened.
 * @post No table state is modified.
 * @note Runs in the ThreadX timer context; the callback must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_timer_expiry(ULONG index)
{
  const uint32_t idx = (uint32_t)index;
  if (idx >= (uint32_t)k_ra8_esp_hosted_max_timers) {
    return;
  }
  if (s_rtos.timer_used[idx] && (s_rtos.timers[idx].cb_fn != nullptr)) {
    s_rtos.timers[idx].cb_fn(s_rtos.timers[idx].arg);
  }
}

/**
 * @brief Burn a bounded number of core cycles standing in for a short delay.
 * @details The loop counter is volatile so the compiler cannot delete it, and
 * the iteration count comes from ::ra8_esp_hosted_rtos_us_spin_iters, which
 * clamps it -- that clamp is what makes the loop statically bounded.
 * @param[in] usec Microseconds to approximate.
 * @pre The cached core rate is set, or the call becomes a no-op.
 * @pre The caller accepts a factor-of-two accuracy bound.
 * @post At least the computed iteration count has executed.
 * @post No module state is modified.
 * @note Safe from any context; it neither blocks nor allocates.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_BOUNDED_LOOP(k_ra8_esp_hosted_spin_iters_max)
static void internal_spin_us(uint32_t usec)
{
  const uint32_t    iters = ra8_esp_hosted_rtos_us_spin_iters(s_rtos.cpu_hz, usec);
  volatile uint32_t sink  = 0U;
  /* The bound is written into the condition rather than left implicit in
     `iters`, so the loop's upper limit is provable from the loop itself --
     which is what NASA Power of 10 Rule 2 asks for and what
     RA8_BOUNDED_LOOP names. */
  for (uint32_t i = 0U; (i < iters) && (i < (uint32_t)k_ra8_esp_hosted_spin_iters_max); ++i) {
    sink = sink + 1U;
  }
}

/* ----------------------------------------------------------------------- */
/* Testable arithmetic */
/* ----------------------------------------------------------------------- */

uint32_t ra8_esp_hosted_rtos_ms_to_ticks(int timeout_ms)
{
  if (timeout_ms < 0) {
    return (uint32_t)TX_WAIT_FOREVER;
  }
  if (timeout_ms == 0) {
    return (uint32_t)TX_NO_WAIT;
  }
  return (uint32_t)timeout_ms / (uint32_t)k_ra8_esp_hosted_ms_per_tick;
}

uint32_t ra8_esp_hosted_rtos_us_spin_iters(uint32_t cpu_hz, uint32_t usec)
{
  if ((cpu_hz == 0U) || (usec == 0U)) {
    return 0U;
  }
  const uint32_t cycles_per_us = cpu_hz / (uint32_t)k_ra8_esp_hosted_hz_per_mhz;
  const uint32_t iters = (cycles_per_us * usec) / (uint32_t)k_ra8_esp_hosted_spin_cycles_per_iter;
  if (iters == 0U) {
    return 1U;
  }
  if (iters > (uint32_t)k_ra8_esp_hosted_spin_iters_max) {
    return (uint32_t)k_ra8_esp_hosted_spin_iters_max;
  }
  return iters;
}

/* ----------------------------------------------------------------------- */
/* Thread and sleep vtable slots */
/* ----------------------------------------------------------------------- */

/**
 * @brief Start a thread from the fixed thread table.
 * @details Copies the name into port-owned storage, claims a table row, and
 * starts the thread on its pre-reserved stack. A stack request larger than
 * one reserved region is refused rather than silently shrunk.
 * @param[in] tname Diagnostic name; may be null.
 * @param[in] tprio ThreadX priority (0 is highest).
 * @param[in] tstack_size Requested stack size in bytes.
 * @param[in] start_routine Thread body.
 * @param[in] sr_arg Argument handed to the body.
 * @return Opaque thread handle, or null on failure.
 * @retval nullptr The substrate is down, an argument is out of contract, or
 *         the thread table is full.
 * @retval non-null A handle usable with ``_h_thread_cancel``.
 * @pre The substrate is initialised.
 * @pre ``tstack_size`` is between ``TX_MINIMUM_STACK`` and the reserved size.
 * @post On success one table row is occupied and the thread is running.
 * @post On failure no row stays claimed.
 * @note Not thread-safe against a concurrent create.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_h_thread_create(const char*                   tname,
                                                   uint32_t                      tprio,
                                                   uint32_t                      tstack_size,
                                                   ra8_esp_hosted_thread_entry_t start_routine,
                                                   void*                         sr_arg)
{
  if (!s_rtos.ready || (start_routine == nullptr)) {
    return nullptr;
  }
  if ((tstack_size < (uint32_t)TX_MINIMUM_STACK) ||
      (tstack_size > (uint32_t)k_ra8_esp_hosted_thread_stack_bytes)) {
    return nullptr;
  }
  const uint32_t idx =
    ra8_esp_hosted_rtos_slot_take(s_rtos.thread_used, k_ra8_esp_hosted_max_threads);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_threads) {
    ra8_log_error(s_tag, "thread table exhausted");
    return nullptr;
  }
  ra8_esp_hosted_thread_slot_t* const slot = &s_rtos.threads[idx];
  slot->entry                              = start_routine;
  slot->arg                                = sr_arg;
  internal_copy_name(slot->name, sizeof(slot->name), tname);
  if (tx_thread_create(&slot->cb,
                       slot->name,
                       internal_thread_entry,
                       (ULONG)idx,
                       s_thread_stacks[idx],
                       (ULONG)tstack_size,
                       (UINT)tprio,
                       (UINT)tprio,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    s_rtos.thread_used[idx] = false;
    return nullptr;
  }
  return slot;
}

/**
 * @brief Terminate and delete a thread, freeing its table row.
 * @details Terminates before deleting, which is what ThreadX requires of a
 * running thread, and frees the row last so a concurrent lookup never sees a
 * half-deleted object.
 * @param[in] thread_handle Handle from ``_h_thread_create``.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK The thread is gone and its row is free.
 * @retval RET_INVALID The handle was not usable.
 * @retval RET_FAIL ThreadX refused the terminate or the delete.
 * @pre The substrate is initialised.
 * @pre The thread is not the caller itself.
 * @post The table row is free for reuse.
 * @post The stack region is available to a later create.
 * @note Not thread-safe against a concurrent create. Validation is by return
 *       code because the vtable signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_thread_cancel(void* thread_handle)
{
  const uint32_t idx = ra8_esp_hosted_rtos_slot_index(thread_handle,
                                                      s_rtos.threads,
                                                      sizeof(s_rtos.threads[0]),
                                                      k_ra8_esp_hosted_max_threads,
                                                      s_rtos.thread_used);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_threads) {
    return RET_INVALID;
  }
  int rc = RET_OK;
  if (tx_thread_terminate(&s_rtos.threads[idx].cb) != TX_SUCCESS) {
    rc = RET_FAIL;
  }
  if (tx_thread_delete(&s_rtos.threads[idx].cb) != TX_SUCCESS) {
    rc = RET_FAIL;
  }
  s_rtos.threads[idx].entry = nullptr;
  s_rtos.thread_used[idx]   = false;
  return rc;
}

/**
 * @brief Give up the remainder of the current time slice.
 * @details Maps straight onto ``tx_thread_relinquish``; the vendored SPI
 * half-duplex driver uses it to let a peer thread make progress.
 * @pre The caller is a thread, not an interrupt handler.
 * @pre The substrate is initialised, or the call is a no-op.
 * @post Control may have passed to another ready thread.
 * @post No module state is modified.
 * @note Thread context only; ThreadX rejects a relinquish from an ISR.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_h_thread_yield(void)
{
  if (s_rtos.ready) {
    tx_thread_relinquish();
  }
}

/**
 * @brief Sleep the calling thread for a whole number of milliseconds.
 * @details One kernel tick is one millisecond, so the conversion is the
 * identity; a zero request still yields, which is what the core expects of a
 * zero-length sleep in a polling loop.
 * @param[in] mseconds Milliseconds to sleep.
 * @return The milliseconds requested.
 * @retval mseconds Always; ThreadX does not report a short sleep.
 * @pre The caller is a thread, not an interrupt handler.
 * @pre The substrate is initialised, or the call returns immediately.
 * @post At least ``mseconds`` milliseconds of kernel time have passed.
 * @post No module state is modified.
 * @note Thread context only.
 * @since 0.1.0
 */
RA8_INTERNAL static unsigned int internal_h_msleep(unsigned int mseconds)
{
  if (s_rtos.ready) {
    (void)tx_thread_sleep((ULONG)mseconds * (ULONG)k_ra8_esp_hosted_ms_per_tick);
  }
  return mseconds;
}

/**
 * @brief Sleep the calling thread for a whole number of seconds.
 * @details Converts to milliseconds and reuses the millisecond sleep, so
 * there is one place the tick rate is applied.
 * @param[in] seconds Seconds to sleep.
 * @return The seconds requested.
 * @retval seconds Always; ThreadX does not report a short sleep.
 * @pre The caller is a thread, not an interrupt handler.
 * @pre The substrate is initialised, or the call returns immediately.
 * @post At least ``seconds`` seconds of kernel time have passed.
 * @post No module state is modified.
 * @note Thread context only.
 * @since 0.1.0
 */
RA8_INTERNAL static unsigned int internal_h_sleep(unsigned int seconds)
{
  (void)internal_h_msleep(seconds * (unsigned int)k_ra8_esp_hosted_ms_per_sec);
  return seconds;
}

/**
 * @brief Delay for a number of microseconds, honestly.
 * @details There is no microsecond timer on this part -- ``ra8_time.h`` has a
 * millisecond tick and nothing finer -- so this splits the request: the whole
 * milliseconds are slept on the RTOS, and only the sub-millisecond remainder
 * is spun out on a bounded cycle-count loop sized from the cached core rate.
 * Nothing is rounded to zero and nothing is faked. Accuracy on the spun part
 * is roughly a factor of two, because cache state, branch prediction and any
 * interrupt taken mid-spin all move it; the slept part is accurate to the
 * kernel tick, so plus or minus one millisecond.
 * @param[in] useconds Microseconds to delay.
 * @return The microseconds this call believes it delayed for.
 * @retval 0 ``useconds`` was zero; nothing was done.
 * @retval useconds The split delay was executed.
 * @pre The caller is a thread when ``useconds`` reaches a millisecond.
 * @pre The cached core rate is set, or the spun part is skipped.
 * @post At least the whole-millisecond part has elapsed on the kernel clock.
 * @post No module state is modified.
 * @note Thread context only once the request reaches one millisecond.
 * @since 0.1.0
 */
RA8_INTERNAL static unsigned int internal_h_usleep(unsigned int useconds)
{
  if (useconds == 0U) {
    return 0U;
  }
  const uint32_t whole_ms = (uint32_t)useconds / (uint32_t)k_ra8_esp_hosted_us_per_ms;
  const uint32_t rem_us   = (uint32_t)useconds % (uint32_t)k_ra8_esp_hosted_us_per_ms;
  if (whole_ms > 0U) {
    (void)internal_h_msleep(whole_ms);
  }
  internal_spin_us(rem_us);
  return useconds;
}

/**
 * @brief Busy-delay for a bounded number of loop iterations.
 * @details Deliberately not a time unit: upstream's contract for this slot is
 * a raw spin count for the few places the scheduler must not run. The count
 * is clamped so the loop stays statically bounded.
 * @param[in] number Iterations requested.
 * @return The iteration count requested, clamped or not.
 * @retval number Always; the return echoes the request, as upstream's does.
 * @pre The caller genuinely cannot yield here.
 * @pre The caller accepts that the duration depends on the core rate.
 * @post At most ::k_ra8_esp_hosted_delay_iters_max iterations executed.
 * @post No module state is modified and the scheduler never ran.
 * @note Safe from any context, including interrupt handlers.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_BOUNDED_LOOP(k_ra8_esp_hosted_delay_iters_max)
static unsigned int internal_h_blocking_delay(unsigned int number)
{
  const uint32_t    iters = (number > (unsigned int)k_ra8_esp_hosted_delay_iters_max)
                              ? (uint32_t)k_ra8_esp_hosted_delay_iters_max
                              : (uint32_t)number;
  volatile uint32_t sink  = 0U;
  /* `iters` is already clamped above; restating the bound in the condition
     makes the limit provable from the loop itself, per NASA Power of 10
     Rule 2 and the RA8_BOUNDED_LOOP annotation. */
  for (uint32_t i = 0U; (i < iters) && (i < (uint32_t)k_ra8_esp_hosted_delay_iters_max); ++i) {
    sink = sink + 1U;
  }
  return number;
}

/* ----------------------------------------------------------------------- */
/* Timer and clock vtable slots */
/* ----------------------------------------------------------------------- */

/**
 * @brief Create and start a software timer.
 * @details ``H_TIMER_TYPE_ONESHOT`` becomes a ThreadX timer with a zero
 * reschedule count, so the kernel stops it after one expiry;
 * ``H_TIMER_TYPE_PERIODIC`` reschedules at the same interval, so the port
 * never has to re-arm it by hand from the expiry context.
 * @param[in] name Diagnostic name; may be null.
 * @param[in] duration_ms Interval in milliseconds; must be positive.
 * @param[in] type ``H_TIMER_TYPE_ONESHOT`` or ``H_TIMER_TYPE_PERIODIC``.
 * @param[in] timeout_handler Callback run on each expiry.
 * @param[in] arg Argument handed to the callback.
 * @return Opaque timer handle, or null on failure.
 * @retval nullptr An argument was out of contract or the timer table is full.
 * @retval non-null A handle usable with ``_h_timer_stop``.
 * @pre The substrate is initialised.
 * @pre ``timeout_handler`` does not block.
 * @post On success one table row is occupied and the timer is running.
 * @post On failure no row stays claimed.
 * @note Not thread-safe against a concurrent create.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_h_timer_start(const char*               name,
                                                 int                       duration_ms,
                                                 int                       type,
                                                 ra8_esp_hosted_timer_cb_t timeout_handler,
                                                 void*                     arg)
{
  if (!s_rtos.ready || (timeout_handler == nullptr) || (duration_ms <= 0)) {
    return nullptr;
  }
  if ((type != H_TIMER_TYPE_ONESHOT) && (type != H_TIMER_TYPE_PERIODIC)) {
    return nullptr;
  }
  const uint32_t idx =
    ra8_esp_hosted_rtos_slot_take(s_rtos.timer_used, k_ra8_esp_hosted_max_timers);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_timers) {
    ra8_log_error(s_tag, "timer table exhausted");
    return nullptr;
  }
  ra8_esp_hosted_timer_slot_t* const slot = &s_rtos.timers[idx];
  slot->cb_fn                             = timeout_handler;
  slot->arg                               = arg;
  internal_copy_name(slot->name, sizeof(slot->name), name);
  const ULONG ticks  = (ULONG)ra8_esp_hosted_rtos_ms_to_ticks(duration_ms);
  const ULONG repeat = (type == H_TIMER_TYPE_PERIODIC) ? ticks : 0U;
  if (tx_timer_create(&slot->cb,
                      slot->name,
                      internal_timer_expiry,
                      (ULONG)idx,
                      ticks,
                      repeat,
                      TX_AUTO_ACTIVATE) != TX_SUCCESS) {
    s_rtos.timer_used[idx] = false;
    return nullptr;
  }
  return slot;
}

/**
 * @brief Stop a timer and free its table row.
 * @details Deactivates before deleting, which is what ThreadX wants of a
 * running timer, and frees the row last.
 * @param[in] timer_handle Handle from ``_h_timer_start``.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK The timer is stopped, deleted and its row is free.
 * @retval RET_INVALID The handle was not usable.
 * @retval RET_FAIL ThreadX refused the deactivate or the delete.
 * @pre The substrate is initialised.
 * @pre The expiry callback is not currently running.
 * @post The table row is free for reuse.
 * @post The callback will not run again.
 * @note Not thread-safe against a concurrent create. Validation is by return
 *       code because the vtable signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_timer_stop(void* timer_handle)
{
  const uint32_t idx = ra8_esp_hosted_rtos_slot_index(timer_handle,
                                                      s_rtos.timers,
                                                      sizeof(s_rtos.timers[0]),
                                                      k_ra8_esp_hosted_max_timers,
                                                      s_rtos.timer_used);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_timers) {
    return RET_INVALID;
  }
  int rc = RET_OK;
  if (tx_timer_deactivate(&s_rtos.timers[idx].cb) != TX_SUCCESS) {
    rc = RET_FAIL;
  }
  if (tx_timer_delete(&s_rtos.timers[idx].cb) != TX_SUCCESS) {
    rc = RET_FAIL;
  }
  s_rtos.timers[idx].cb_fn = nullptr;
  s_rtos.timer_used[idx]   = false;
  return rc;
}

/**
 * @brief Read milliseconds since kernel start, without a 32-bit wrap.
 * @details ``tx_time_get`` returns a 32-bit tick count that wraps after about
 * 49.7 days at a 1 kHz tick, and the vendored core uses this value for
 * elapsed-time arithmetic that a wrap would silently corrupt. Each call
 * compares the new reading against the previous one and, when it has gone
 * backwards, adds one full 2^32-tick epoch to a 64-bit accumulator. The
 * detection holds as long as this is called at least once per epoch, which
 * the transport's per-transaction use guarantees by a very wide margin.
 * @return Milliseconds since kernel start.
 * @retval 0 The kernel has just started and the substrate is down.
 * @retval 1..UINT64_MAX The extended millisecond count.
 * @pre The substrate is initialised, or the call reports zero.
 * @pre The caller polls at least once per 2^32 ticks.
 * @post The recorded previous reading equals the value just read.
 * @post The accumulator has grown by one epoch if and only if a wrap was
 *       observed.
 * @note Not reentrant: two threads racing here can both observe the same
 *       wrap. The transport calls it from one thread.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_h_get_time_ms(void)
{
  if (!s_rtos.ready) {
    return 0U;
  }
  const uint32_t now = (uint32_t)tx_time_get();
  if (now < s_rtos.last_ticks) {
    s_rtos.tick_epoch += ((uint64_t)UINT32_MAX + 1U);
  }
  s_rtos.last_ticks = now;
  return (s_rtos.tick_epoch + (uint64_t)now) * (uint64_t)k_ra8_esp_hosted_ms_per_tick;
}

/* ----------------------------------------------------------------------- */
/* Lifecycle and binding */
/* ----------------------------------------------------------------------- */

bool ra8_esp_hosted_rtos_is_ready(void)
{
  return s_rtos.ready;
}

ra8_err_t ra8_esp_hosted_rtos_init(void)
{
  if (s_rtos.ready) {
    ra8_log_error(s_tag, "RTOS substrate already initialised");
    return k_ra8_err_invalid_state;
  }
  (void)memset(&s_rtos, 0, sizeof(s_rtos));
  RA8_RETURN_ON_ERROR(ra8_esp_hosted_rtos_pool_init(), s_tag, "byte pools refused");
  RA8_RETURN_ON_ERROR(ra8_esp_hosted_rtos_sync_init(), s_tag, "sync tables refused");
  uint32_t cpu_hz = 0U;
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpu_hz) != k_ra8_ok) {
    cpu_hz = (uint32_t)k_ra8_esp_hosted_default_cpu_hz;
    ra8_log_warn(s_tag, "core rate query failed; usleep uses the fallback rate");
  }
  s_rtos.cpu_hz     = cpu_hz;
  s_rtos.last_ticks = (uint32_t)tx_time_get();
  s_rtos.ready      = true;
  return k_ra8_ok;
}

ra8_err_t ra8_esp_hosted_rtos_deinit(void)
{
  if (!s_rtos.ready) {
    ra8_log_error(s_tag, "RTOS substrate not initialised");
    return k_ra8_err_not_initialized;
  }
  ra8_err_t worst = k_ra8_ok;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_timers; ++i) {
    if (s_rtos.timer_used[i] && (internal_h_timer_stop(&s_rtos.timers[i]) != RET_OK)) {
      worst = k_ra8_err_rtos_error;
    }
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_threads; ++i) {
    if (s_rtos.thread_used[i] && (internal_h_thread_cancel(&s_rtos.threads[i]) != RET_OK)) {
      worst = k_ra8_err_rtos_error;
    }
  }
  if (ra8_esp_hosted_rtos_sync_deinit() != k_ra8_ok) {
    worst = k_ra8_err_rtos_error;
  }
  if (ra8_esp_hosted_rtos_pool_deinit() != k_ra8_ok) {
    worst = k_ra8_err_rtos_error;
  }
  (void)memset(&s_rtos, 0, sizeof(s_rtos));
  return worst;
}

ra8_err_t ra8_esp_hosted_rtos_bind(hosted_osi_funcs_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "vtable is NULL");
  out->_h_thread_create  = internal_h_thread_create;
  out->_h_thread_cancel  = internal_h_thread_cancel;
  out->_h_thread_yield   = internal_h_thread_yield;
  out->_h_msleep         = internal_h_msleep;
  out->_h_usleep         = internal_h_usleep;
  out->_h_sleep          = internal_h_sleep;
  out->_h_blocking_delay = internal_h_blocking_delay;
  out->_h_timer_start    = internal_h_timer_start;
  out->_h_timer_stop     = internal_h_timer_stop;
  out->_h_get_time_ms    = internal_h_get_time_ms;
  return k_ra8_ok;
}
