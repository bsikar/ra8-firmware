/**
 * @file port/esp-hosted/src/ra8_esp_hosted_tx_shim_internal.h
 * @brief Recording, injectable ThreadX model for the esp-hosted RTOS port.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * On the cross-compiled target the port includes the real ``tx_api.h``. The
 * host unit-test build (``RA8_OFF_TARGET``) links no ThreadX at all, so
 * this header stands in for it. Unlike the always-succeeding stub used by
 * ``libs/ra8_wdt_supervisor``, this one is a **model**: it keeps real queue
 * rings, real semaphore counts and a real bump-allocating byte pool, and it
 * accepts an injected failure code per API family. That is what lets the
 * tests reach the port's error branches -- queue full, queue empty, semaphore
 * timeout, pool exhaustion, tick rollover -- and not merely its happy path.
 *
 * @par Why ULONG is 32-bit here
 * The real ThreadX Cortex-M85 port spells it ``unsigned long``, which is 32
 * bits on that ABI. Copying the spelling onto an LP64 host would make it 64
 * bits, and the whole reason ``_h_get_time_ms`` exists -- extending a 32-bit
 * tick counter across its rollover -- would become untestable because the
 * model's counter could never wrap. The model fixes the width, not the
 * spelling. It also keeps ``sizeof(ULONG)`` at 4 on both builds, so the
 * port's queue word-rounding arithmetic is identical host and target.
 *
 * @par Storage ownership
 * The model's state is one object shared by every translation unit that
 * includes this header. Exactly one TU defines
 * ``RA8_ESP_HOSTED_TX_SHIM_IMPL`` before the include and owns the definition;
 * the rest see an ``extern`` declaration. Every function is ``static inline``
 * and works on that single object, so the port sources and the tests observe
 * the same fake kernel.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef RA8_ESP_HOSTED_TX_SHIM_IMPL
/**
 * @def RA8_ESP_HOSTED_TX_SHIM_STORAGE
 * @brief Owning storage class: this translation unit defines the model state.
 * @details Expands to nothing, so the declaration it prefixes becomes the one
 * tentative definition of ::g_ra8_esp_hosted_tx_shim in the program.
 * @note Read-only build configuration.
 * @warning Defining ``RA8_ESP_HOSTED_TX_SHIM_IMPL`` in two translation units
 *          is a duplicate-symbol link error.
 * @par Example:
 * @code
 * #define RA8_ESP_HOSTED_TX_SHIM_IMPL
 * #include "ra8_esp_hosted_tx_shim_internal.h"
 * @endcode
 * @since 0.1.0
 */
#define RA8_ESP_HOSTED_TX_SHIM_STORAGE
#else
/**
 * @def RA8_ESP_HOSTED_TX_SHIM_STORAGE
 * @brief Borrowing storage class: the model state lives in another unit.
 * @details Expands to ``extern``, so this translation unit only declares
 * ::g_ra8_esp_hosted_tx_shim and links against the owning definition.
 * @note Read-only build configuration.
 * @warning Linking without any owning translation unit is an undefined-symbol
 *          error at link time, not a silent zero-initialised state.
 * @par Example:
 * @code
 * #include "ra8_esp_hosted_tx_shim_internal.h"
 * @endcode
 * @since 0.1.0
 */
#define RA8_ESP_HOSTED_TX_SHIM_STORAGE extern
#endif

/* The four ThreadX type aliases below intentionally violate the project's
 * lower_case typedef rule: they MUST mirror the vendor ``tx_api.h`` spelling,
 * because this header replaces that vendor header on the host build and the
 * port sources use the upper-case names the vendor API mandates. */

/** @typedef ULONG @brief ThreadX unsigned long, pinned to 32 bits. */
typedef uint32_t ULONG; /* NOLINT(readability-identifier-naming) -- ThreadX name. */
/** @typedef UINT @brief ThreadX status and count type. */
typedef unsigned int UINT; /* NOLINT(readability-identifier-naming) -- ThreadX name. */
/** @typedef CHAR @brief ThreadX character type used for object names. */
typedef char CHAR; /* NOLINT(readability-identifier-naming) -- ThreadX name. */
/** @typedef UCHAR @brief ThreadX unsigned character type for pool storage. */
typedef unsigned char UCHAR; /* NOLINT(readability-identifier-naming) -- ThreadX name. */

/** @def TX_SUCCESS Service completed (host model). */
#define TX_SUCCESS (0U)
/** @def TX_POOL_ERROR Invalid byte-pool control block (host model). */
#define TX_POOL_ERROR (0x02U)
/** @def TX_PTR_ERROR Invalid pointer argument (host model). */
#define TX_PTR_ERROR (0x03U)
/** @def TX_SIZE_ERROR Invalid size argument (host model). */
#define TX_SIZE_ERROR (0x05U)
/** @def TX_QUEUE_ERROR Invalid queue control block (host model). */
#define TX_QUEUE_ERROR (0x09U)
/** @def TX_QUEUE_EMPTY No message available within the wait (host model). */
#define TX_QUEUE_EMPTY (0x0AU)
/** @def TX_QUEUE_FULL No room for the message within the wait (host model). */
#define TX_QUEUE_FULL (0x0BU)
/** @def TX_SEMAPHORE_ERROR Invalid semaphore control block (host model). */
#define TX_SEMAPHORE_ERROR (0x0CU)
/** @def TX_NO_INSTANCE No semaphore instance within the wait (host model). */
#define TX_NO_INSTANCE (0x0DU)
/** @def TX_THREAD_ERROR Invalid thread control block (host model). */
#define TX_THREAD_ERROR (0x0EU)
/** @def TX_NO_MEMORY Byte pool could not satisfy the request (host model). */
#define TX_NO_MEMORY (0x10U)
/** @def TX_TIMER_ERROR Invalid timer control block (host model). */
#define TX_TIMER_ERROR (0x15U)
/** @def TX_TICK_ERROR Invalid tick count for a timer (host model). */
#define TX_TICK_ERROR (0x16U)
/** @def TX_MUTEX_ERROR Invalid mutex control block (host model). */
#define TX_MUTEX_ERROR (0x1CU)
/** @def TX_NOT_OWNED Mutex released by a non-owner (host model). */
#define TX_NOT_OWNED (0x1EU)
/** @def TX_NO_WAIT Return immediately rather than suspend (host model). */
#define TX_NO_WAIT ((ULONG)0U)
/** @def TX_WAIT_FOREVER Suspend until satisfied (host model). */
#define TX_WAIT_FOREVER ((ULONG)0xFFFFFFFFU)
/** @def TX_16_ULONG Largest queue message size ThreadX accepts (host model). */
#define TX_16_ULONG (16U)
/** @def TX_AUTO_START Start the thread as soon as it is created (host model). */
#define TX_AUTO_START (1U)
/** @def TX_NO_TIME_SLICE Disable round-robin time slicing (host model). */
#define TX_NO_TIME_SLICE ((ULONG)0U)
/** @def TX_INHERIT Enable mutex priority inheritance (host model). */
#define TX_INHERIT (1U)
/** @def TX_AUTO_ACTIVATE Activate the timer when it is created (host model). */
#define TX_AUTO_ACTIVATE (1U)
/** @def TX_MINIMUM_STACK Smallest stack a thread may be given (host model). */
#define TX_MINIMUM_STACK (512U)

/**
 * @enum ra8_esp_hosted_tx_shim_limits_t
 * @brief Fixed bounds of the fake kernel.
 * @details The model exists to drive the port's decision branches, not to be
 * a second ThreadX, so every bound below is the worst case the port reaches.
 * @invariant ::k_ra8_esp_hosted_tx_shim_queue_cap_max bounds every ring the
 *            model accepts, so a port-side drain loop always terminates.
 * @invariant ::k_ra8_esp_hosted_tx_shim_pool_align divides every pointer the
 *            modelled byte pool returns.
 * @par Example:
 * @code
 * static_assert(k_ra8_esp_hosted_tx_shim_family_count > 0U, "one family min");
 * @endcode
 * @see ra8_esp_hosted_tx_shim_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_esp_hosted_tx_shim_pool_align    = 8U,          /**< Alignment of every pool block. */
  k_ra8_esp_hosted_tx_shim_pool_hdr      = 8U,          /**< Per-block bookkeeping bytes.   */
  k_ra8_esp_hosted_tx_shim_queue_cap_max = 64U,         /**< Deepest modelled ring.         */
  k_ra8_esp_hosted_tx_shim_family_count  = 7U,          /**< Injectable families.           */
  k_ra8_esp_hosted_tx_shim_magic         = 0x54585348U, /**< Stamped when created.          */
} ra8_esp_hosted_tx_shim_limits_t;

/**
 * @enum ra8_esp_hosted_tx_shim_family_t
 * @brief API family selector for injected failures and call counting.
 * @details A test arms one family at a time so a single service call can be
 * made to fail without disturbing the rest of the model.
 * @invariant Every enumerator indexes the per-family arrays of
 *            ::ra8_esp_hosted_tx_shim_t.
 * @invariant The enumerators are contiguous from zero.
 * @par Example:
 * @code
 * ra8_esp_hosted_tx_shim_arm(k_ra8_esp_hosted_tx_shim_family_pool, TX_NO_MEMORY);
 * @endcode
 * @see ra8_esp_hosted_tx_shim_arm
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_esp_hosted_tx_shim_family_pool      = 0U, /**< Byte-pool services.  */
  k_ra8_esp_hosted_tx_shim_family_queue     = 1U, /**< Queue services.      */
  k_ra8_esp_hosted_tx_shim_family_semaphore = 2U, /**< Semaphore services.  */
  k_ra8_esp_hosted_tx_shim_family_mutex     = 3U, /**< Mutex services.      */
  k_ra8_esp_hosted_tx_shim_family_thread    = 4U, /**< Thread services.     */
  k_ra8_esp_hosted_tx_shim_family_timer     = 5U, /**< Timer services.      */
  k_ra8_esp_hosted_tx_shim_family_time      = 6U, /**< Tick-clock services. */
} ra8_esp_hosted_tx_shim_family_t;

/**
 * @struct TX_BYTE_POOL
 * @brief Modelled byte pool: a bump allocator over caller storage.
 * @details Real ThreadX keeps a free list; the model keeps a cursor, which is
 * enough to distinguish "allocated" from "pool exhausted" -- the only two
 * outcomes the port acts on.
 * @invariant ``cursor <= size`` at all times.
 * @invariant ``live`` counts blocks handed out and not yet released.
 * @par Example:
 * @code
 * TX_BYTE_POOL pool = {};
 * (void)tx_byte_pool_create(&pool, "p", backing, sizeof(backing));
 * @endcode
 * @see tx_byte_allocate
 * @since 0.1.0
 */
typedef struct {
  uint32_t magic;  /**< ::k_ra8_esp_hosted_tx_shim_magic once created.       */
  UCHAR*   start;  /**< Backing storage supplied at create time.             */
  uint32_t size;   /**< Backing storage length in bytes.                     */
  uint32_t cursor; /**< Bytes consumed from the start of the backing.        */
  uint32_t live;   /**< Blocks handed out and not yet released.              */
} TX_BYTE_POOL;    /* NOLINT(readability-identifier-naming) -- ThreadX name. */

/**
 * @struct TX_QUEUE
 * @brief Modelled message queue: a real fixed-capacity ring.
 * @details Messages are copied in and out exactly as ThreadX copies them, so
 * the port's word-rounding is exercised against a real capacity.
 * @invariant ``count <= capacity``.
 * @invariant ``head < capacity`` whenever ``capacity`` is non-zero.
 * @par Example:
 * @code
 * TX_QUEUE q = {};
 * (void)tx_queue_create(&q, "q", 2U, storage, sizeof(storage));
 * @endcode
 * @see tx_queue_send
 * @since 0.1.0
 */
typedef struct {
  uint32_t magic;     /**< ::k_ra8_esp_hosted_tx_shim_magic once created.       */
  ULONG*   storage;   /**< Caller-supplied message ring.                        */
  uint32_t words;     /**< Message size in 32-bit words.                        */
  uint32_t capacity;  /**< Ring depth in messages.                              */
  uint32_t count;     /**< Messages currently enqueued.                         */
  uint32_t head;      /**< Index of the oldest enqueued message.                */
  ULONG    last_wait; /**< Wait option of the most recent send or receive.      */
} TX_QUEUE;           /* NOLINT(readability-identifier-naming) -- ThreadX name. */

/**
 * @struct TX_SEMAPHORE
 * @brief Modelled counting semaphore.
 * @details The count is real, so a try-take of an empty semaphore fails and a
 * take of a posted one succeeds; the model never suspends, because the host
 * build has no scheduler to suspend to.
 * @invariant ``count`` changes by at most one per service call.
 * @invariant ``last_wait`` records what the port asked for even when the
 *            model could not honour it.
 * @par Example:
 * @code
 * TX_SEMAPHORE s = {};
 * (void)tx_semaphore_create(&s, "s", 0U);
 * @endcode
 * @see tx_semaphore_get
 * @since 0.1.0
 */
typedef struct {
  uint32_t magic;     /**< ::k_ra8_esp_hosted_tx_shim_magic once created.       */
  ULONG    count;     /**< Current instance count.                              */
  ULONG    last_wait; /**< Wait option of the most recent take.                 */
} TX_SEMAPHORE;       /* NOLINT(readability-identifier-naming) -- ThreadX name. */

/**
 * @struct TX_MUTEX
 * @brief Modelled recursive mutex.
 * @details Single-threaded host code cannot contend, so the model counts
 * ownership depth and reports ``TX_NOT_OWNED`` for an unbalanced release --
 * the one mutex failure the port can actually provoke.
 * @invariant ``owned`` is zero exactly when the mutex is free.
 * @invariant ``inherit`` records the create-time inheritance selector.
 * @par Example:
 * @code
 * TX_MUTEX m = {};
 * (void)tx_mutex_create(&m, "m", TX_INHERIT);
 * @endcode
 * @see tx_mutex_get
 * @since 0.1.0
 */
typedef struct {
  uint32_t magic;   /**< ::k_ra8_esp_hosted_tx_shim_magic once created.       */
  ULONG    owned;   /**< Ownership nesting depth.                             */
  UINT     inherit; /**< Priority-inheritance flag passed at create.          */
} TX_MUTEX;         /* NOLINT(readability-identifier-naming) -- ThreadX name. */

/**
 * @struct TX_THREAD
 * @brief Modelled thread control block: recorded, never scheduled.
 * @details Creation records the entry point and its argument; a test invokes
 * them directly when it wants the body to run. Termination and deletion are
 * tracked so the port's teardown can be asserted.
 * @invariant ``entry`` is non-null once ``magic`` is stamped.
 * @invariant ``terminated`` never returns to false once set.
 * @par Example:
 * @code
 * TX_THREAD t = {};
 * TEST_ASSERT_EQ(false, t.terminated);
 * @endcode
 * @see tx_thread_terminate
 * @since 0.1.0
 */
typedef struct {
  uint32_t magic;       /**< ::k_ra8_esp_hosted_tx_shim_magic once created.       */
  void (*entry)(ULONG); /**< Recorded entry point.                                */
  ULONG input;          /**< Recorded entry argument.                             */
  UINT  priority;       /**< Recorded ThreadX priority.                           */
  ULONG stack_size;     /**< Recorded stack size in bytes.                        */
  bool  terminated;     /**< Set by ``tx_thread_terminate``.                      */
} TX_THREAD;            /* NOLINT(readability-identifier-naming) -- ThreadX name. */

/**
 * @struct TX_TIMER
 * @brief Modelled application timer: recorded, fired on demand.
 * @details Nothing advances time on the host, so the model stores the expiry
 * function and both tick counts and lets a test fire it explicitly.
 * @invariant ``active`` reflects the last create/deactivate call.
 * @invariant ``reschedule`` is zero exactly for a one-shot timer.
 * @par Example:
 * @code
 * TX_TIMER t = {};
 * TEST_ASSERT_EQ(false, t.active);
 * @endcode
 * @see ra8_esp_hosted_tx_shim_fire_timer
 * @since 0.1.0
 */
typedef struct {
  uint32_t magic;        /**< ::k_ra8_esp_hosted_tx_shim_magic once created.       */
  void (*expiry)(ULONG); /**< Recorded expiry function.                            */
  ULONG input;           /**< Recorded expiry argument.                            */
  ULONG initial;         /**< Ticks before the first expiry.                       */
  ULONG reschedule;      /**< Ticks between expiries; 0 = one-shot.                */
  bool  active;          /**< True while the timer would be running.               */
} TX_TIMER;              /* NOLINT(readability-identifier-naming) -- ThreadX name. */

/**
 * @struct ra8_esp_hosted_tx_shim_t
 * @brief The whole fake kernel state, shared by every including TU.
 * @details Tests read these fields directly rather than through accessors: an
 * accessor per counter would be more code than the model itself.
 * @invariant ``armed[f]`` true means the next call in family ``f`` returns
 *            ``status[f]`` and then disarms.
 * @invariant ``calls[f]`` counts every entry into family ``f``, including the
 *            calls that returned an injected failure.
 * @par Example:
 * @code
 * ra8_esp_hosted_tx_shim_reset();
 * TEST_ASSERT_EQ(0U, g_ra8_esp_hosted_tx_shim.sleeps);
 * @endcode
 * @see ra8_esp_hosted_tx_shim_reset
 * @since 0.1.0
 */
typedef struct {
  ULONG    ticks;                                         /**< Fake 32-bit kernel tick counter. */
  UINT     status[k_ra8_esp_hosted_tx_shim_family_count]; /**< Injected codes.                  */
  bool     armed[k_ra8_esp_hosted_tx_shim_family_count];  /**< Injection armed.                 */
  uint32_t calls[k_ra8_esp_hosted_tx_shim_family_count];  /**< Per-family calls.                */
  uint32_t sleeps;       /**< Number of ``tx_thread_sleep`` calls.      */
  ULONG    last_sleep;   /**< Ticks requested by the most recent sleep. */
  uint32_t relinquishes; /**< Number of ``tx_thread_relinquish`` calls. */
} ra8_esp_hosted_tx_shim_t;

/**
 * @var g_ra8_esp_hosted_tx_shim
 * @brief The one fake-kernel state object.
 * @details Defined by the TU that sets ``RA8_ESP_HOSTED_TX_SHIM_IMPL``,
 * declared ``extern`` everywhere else.
 * @note Host build only; nothing in the target build references it.
 * @warning Reset it between tests or counters leak across cases.
 * @since 0.1.0
 */
RA8_ESP_HOSTED_TX_SHIM_STORAGE ra8_esp_hosted_tx_shim_t g_ra8_esp_hosted_tx_shim;

/**
 * @brief Clear the whole fake kernel back to power-on state.
 * @details Zeroes tick count, call counters and every armed injection. It
 * does not touch caller-owned control blocks: those are the port's statics.
 * @pre The host build is active.
 * @pre No modelled service is mid-call.
 * @post Every counter reads zero.
 * @post No family has an armed injection.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline void ra8_esp_hosted_tx_shim_reset(void)
{
  (void)memset(&g_ra8_esp_hosted_tx_shim, 0, sizeof(g_ra8_esp_hosted_tx_shim));
}

/**
 * @brief Arm the next call in one API family to return a chosen status.
 * @details One-shot: the first call in that family consumes the injection and
 * every later call behaves normally again.
 * @param[in] family Family to arm.
 * @param[in] status ThreadX status code the next call returns.
 * @pre ``family`` is a valid ::ra8_esp_hosted_tx_shim_family_t.
 * @pre The shim has been reset at least once.
 * @post The named family is armed.
 * @post No other family is affected.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline void ra8_esp_hosted_tx_shim_arm(ra8_esp_hosted_tx_shim_family_t family, UINT status)
{
  if ((uint32_t)family < (uint32_t)k_ra8_esp_hosted_tx_shim_family_count) {
    g_ra8_esp_hosted_tx_shim.status[(uint32_t)family] = status;
    g_ra8_esp_hosted_tx_shim.armed[(uint32_t)family]  = true;
  }
}

/**
 * @brief Set the fake kernel tick counter to an exact value.
 * @details Used to place the counter just below its 32-bit rollover so the
 * port's 64-bit time extension can be driven across the wrap.
 * @param[in] ticks New tick value.
 * @pre The shim has been reset at least once.
 * @pre The caller accepts that time may appear to move backwards.
 * @post ``tx_time_get`` returns ``ticks``.
 * @post No other model state changes.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline void ra8_esp_hosted_tx_shim_set_ticks(ULONG ticks)
{
  g_ra8_esp_hosted_tx_shim.ticks = ticks;
}

/**
 * @brief Count one entry into an API family and consume any injection.
 * @details The single place the model decides whether a service may run, so a
 * newly modelled service cannot forget to be countable or injectable.
 * @param[in] family Family being entered.
 * @param[out] status Receives the injected code when one was armed.
 * @return Whether an injected failure was consumed.
 * @retval true ``status`` holds the code the caller must return.
 * @retval false The caller should proceed normally.
 * @pre ``status`` is non-null.
 * @pre ``family`` is a valid ::ra8_esp_hosted_tx_shim_family_t.
 * @post The family's call counter has advanced by one.
 * @post Any injection for that family is disarmed.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline bool ra8_esp_hosted_tx_shim_enter(ra8_esp_hosted_tx_shim_family_t family,
                                                UINT*                           status)
{
  const uint32_t idx = (uint32_t)family;
  if ((status == nullptr) || (idx >= (uint32_t)k_ra8_esp_hosted_tx_shim_family_count)) {
    return false;
  }
  g_ra8_esp_hosted_tx_shim.calls[idx]++;
  if (!g_ra8_esp_hosted_tx_shim.armed[idx]) {
    return false;
  }
  g_ra8_esp_hosted_tx_shim.armed[idx] = false;
  *status                             = g_ra8_esp_hosted_tx_shim.status[idx];
  return true;
}

/**
 * @brief Run a modelled timer's expiry function once.
 * @details Expiry is explicit because the host has no tick source. A one-shot
 * timer is marked inactive first, matching ThreadX ceasing to reschedule it.
 * @param[in,out] timer_ptr Timer to fire.
 * @return Whether the expiry function ran.
 * @retval true The timer was active and its function was called.
 * @retval false The timer was null, uncreated or inactive.
 * @pre ``timer_ptr`` was created by ``tx_timer_create``.
 * @pre The expiry function tolerates being called from test context.
 * @post A one-shot timer is left inactive.
 * @post A periodic timer is left active.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline bool ra8_esp_hosted_tx_shim_fire_timer(TX_TIMER* timer_ptr)
{
  if ((timer_ptr == nullptr) || (timer_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return false;
  }
  if (!timer_ptr->active || (timer_ptr->expiry == nullptr)) {
    return false;
  }
  if (timer_ptr->reschedule == 0U) {
    timer_ptr->active = false;
  }
  timer_ptr->expiry(timer_ptr->input);
  return true;
}

/**
 * @brief Host model of ``tx_byte_pool_create``.
 * @details Records the backing store and resets the bump cursor.
 * @param[out] pool_ptr Pool control block to initialise.
 * @param[in] name_ptr Diagnostic name; accepted for signature shape.
 * @param[in] pool_start Backing storage.
 * @param[in] pool_size Backing storage length in bytes.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The pool is ready for allocation.
 * @retval TX_POOL_ERROR ``pool_ptr`` or ``pool_start`` was null.
 * @pre ``pool_start`` covers ``pool_size`` bytes.
 * @pre The pool is not already in use.
 * @post The pool reports its whole size as available.
 * @post The pool family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT
/* NOLINTNEXTLINE(readability-non-const-parameter) -- ThreadX API pins it non-const. */
tx_byte_pool_create(TX_BYTE_POOL* pool_ptr, CHAR* name_ptr, void* pool_start, ULONG pool_size)
{
  UINT injected = TX_SUCCESS;
  (void)name_ptr;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_pool, &injected)) {
    return injected;
  }
  if ((pool_ptr == nullptr) || (pool_start == nullptr)) {
    return TX_POOL_ERROR;
  }
  pool_ptr->magic  = (uint32_t)k_ra8_esp_hosted_tx_shim_magic;
  pool_ptr->start  = (UCHAR*)pool_start;
  pool_ptr->size   = (uint32_t)pool_size;
  pool_ptr->cursor = 0U;
  pool_ptr->live   = 0U;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_byte_pool_delete``.
 * @details Clears the sentinel so a later allocation is rejected, which is how
 * the port's use-after-teardown path is reached.
 * @param[in,out] pool_ptr Pool to delete.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The pool was deleted.
 * @retval TX_POOL_ERROR ``pool_ptr`` was null or never created.
 * @pre ``pool_ptr`` came from ``tx_byte_pool_create``.
 * @pre No block from the pool is still in use.
 * @post The pool no longer satisfies allocations.
 * @post The pool family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_byte_pool_delete(TX_BYTE_POOL* pool_ptr)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_pool, &injected)) {
    return injected;
  }
  if ((pool_ptr == nullptr) || (pool_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_POOL_ERROR;
  }
  (void)memset(pool_ptr, 0, sizeof(*pool_ptr));
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_byte_allocate``.
 * @details Bumps a cursor and returns ``TX_NO_MEMORY`` once the request no
 * longer fits -- exactly the pool-exhaustion branch the port must handle.
 * @param[in,out] pool_ptr Pool to allocate from.
 * @param[out] memory_ptr Receives the block address.
 * @param[in] memory_size Requested block length in bytes.
 * @param[in] wait_option Ignored; the model never suspends.
 * @return ThreadX status code.
 * @retval TX_SUCCESS ``memory_ptr`` holds a usable block.
 * @retval TX_POOL_ERROR ``pool_ptr`` was null or never created.
 * @retval TX_PTR_ERROR ``memory_ptr`` was null.
 * @retval TX_NO_MEMORY The remaining space could not satisfy the request.
 * @pre The pool was created.
 * @pre ``memory_size`` is non-zero.
 * @post On success the live-block count has risen by one.
 * @post On failure ``*memory_ptr`` is left null.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT
tx_byte_allocate(TX_BYTE_POOL* pool_ptr, void** memory_ptr, ULONG memory_size, ULONG wait_option)
{
  UINT injected = TX_SUCCESS;
  (void)wait_option;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_pool, &injected)) {
    return injected;
  }
  if ((pool_ptr == nullptr) || (pool_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_POOL_ERROR;
  }
  if (memory_ptr == nullptr) {
    return TX_PTR_ERROR;
  }
  *memory_ptr           = nullptr;
  const uint32_t algn   = (uint32_t)k_ra8_esp_hosted_tx_shim_pool_align;
  const uint32_t need   = (((uint32_t)memory_size + (algn - 1U)) & ~(algn - 1U)) +
                          (uint32_t)k_ra8_esp_hosted_tx_shim_pool_hdr;
  const uint32_t cursor = (pool_ptr->cursor + (algn - 1U)) & ~(algn - 1U);
  if ((need > pool_ptr->size) || (cursor > (pool_ptr->size - need))) {
    return TX_NO_MEMORY;
  }
  pool_ptr->cursor = cursor + need;
  pool_ptr->live++;
  *memory_ptr = &pool_ptr->start[cursor + (uint32_t)k_ra8_esp_hosted_tx_shim_pool_hdr];
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_byte_release``.
 * @details The model does not merge blocks back, so release only validates
 * the pointer and lets the injection seam report a failing release.
 * @param[in] memory_ptr Block previously returned by ``tx_byte_allocate``.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The block was released.
 * @retval TX_PTR_ERROR ``memory_ptr`` was null.
 * @pre ``memory_ptr`` came from this model's pool.
 * @pre The block is not released twice.
 * @post The pool family call counter has advanced.
 * @post No pool cursor moves; the model does not reclaim.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_byte_release(void* memory_ptr)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_pool, &injected)) {
    return injected;
  }
  if (memory_ptr == nullptr) {
    return TX_PTR_ERROR;
  }
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_byte_pool_info_get``.
 * @details Reports bytes still reachable from the cursor, and the live block
 * count plus one as the fragment count.
 * @param[in] pool_ptr Pool to inspect.
 * @param[out] name Unused; accepted for signature shape.
 * @param[out] available_bytes Receives the bytes still allocatable.
 * @param[out] fragments Receives the modelled fragment count.
 * @param[out] first_suspended Unused; accepted for signature shape.
 * @param[out] suspended_count Unused; accepted for signature shape.
 * @param[out] next_pool Unused; accepted for signature shape.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The outputs were written.
 * @retval TX_POOL_ERROR ``pool_ptr`` was null or never created.
 * @pre ``pool_ptr`` was created.
 * @pre The outputs the caller cares about are non-null.
 * @post Only the non-null outputs are written.
 * @post No pool state changes.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_byte_pool_info_get(
  TX_BYTE_POOL* pool_ptr,
  CHAR**        name,
  ULONG*        available_bytes,
  ULONG*        fragments,
  void**        first_suspended,
  /* NOLINTNEXTLINE(readability-non-const-parameter) -- ThreadX API pins it non-const. */
  ULONG*         suspended_count,
  TX_BYTE_POOL** next_pool)
{
  UINT injected = TX_SUCCESS;
  (void)name;
  (void)first_suspended;
  (void)suspended_count;
  (void)next_pool;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_pool, &injected)) {
    return injected;
  }
  if ((pool_ptr == nullptr) || (pool_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_POOL_ERROR;
  }
  if (available_bytes != nullptr) {
    *available_bytes = pool_ptr->size - pool_ptr->cursor;
  }
  if (fragments != nullptr) {
    *fragments = pool_ptr->live + 1U;
  }
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_queue_create``.
 * @details Derives the ring depth from the storage length and message size,
 * as ThreadX does, so the port's word rounding meets a real capacity.
 * @param[out] queue_ptr Queue control block to initialise.
 * @param[in] name_ptr Diagnostic name; accepted for signature shape.
 * @param[in] message_size Message size in 32-bit words.
 * @param[in] queue_start Message ring storage.
 * @param[in] queue_size Ring storage length in bytes.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The queue is ready.
 * @retval TX_QUEUE_ERROR ``queue_ptr`` or ``queue_start`` was null.
 * @retval TX_SIZE_ERROR The sizes yield no whole message, or too many.
 * @pre ``message_size`` is between one and ::TX_16_ULONG words.
 * @pre ``queue_start`` covers ``queue_size`` bytes.
 * @post The queue reports zero enqueued messages.
 * @post The queue family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_queue_create(
  TX_QUEUE* queue_ptr,
  /* NOLINTNEXTLINE(readability-non-const-parameter) -- ThreadX API pins it non-const. */
  CHAR* name_ptr,
  UINT  message_size,
  void* queue_start,
  ULONG queue_size)
{
  UINT injected = TX_SUCCESS;
  (void)name_ptr;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_queue, &injected)) {
    return injected;
  }
  if ((queue_ptr == nullptr) || (queue_start == nullptr)) {
    return TX_QUEUE_ERROR;
  }
  if ((message_size == 0U) || (message_size > TX_16_ULONG)) {
    return TX_SIZE_ERROR;
  }
  const uint32_t capacity = (uint32_t)queue_size / (message_size * (uint32_t)sizeof(ULONG));
  if ((capacity == 0U) || (capacity > (uint32_t)k_ra8_esp_hosted_tx_shim_queue_cap_max)) {
    return TX_SIZE_ERROR;
  }
  (void)memset(queue_ptr, 0, sizeof(*queue_ptr));
  queue_ptr->magic    = (uint32_t)k_ra8_esp_hosted_tx_shim_magic;
  queue_ptr->storage  = (ULONG*)queue_start;
  queue_ptr->words    = message_size;
  queue_ptr->capacity = capacity;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_queue_delete``.
 * @details Clears the sentinel so later sends and receives are rejected.
 * @param[in,out] queue_ptr Queue to delete.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The queue was deleted.
 * @retval TX_QUEUE_ERROR ``queue_ptr`` was null or never created.
 * @pre ``queue_ptr`` came from ``tx_queue_create``.
 * @pre Nothing is waiting on the queue.
 * @post The queue no longer accepts messages.
 * @post The queue family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_queue_delete(TX_QUEUE* queue_ptr)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_queue, &injected)) {
    return injected;
  }
  if ((queue_ptr == nullptr) || (queue_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_QUEUE_ERROR;
  }
  (void)memset(queue_ptr, 0, sizeof(*queue_ptr));
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_queue_flush``.
 * @details Discards every enqueued message without touching capacity.
 * @param[in,out] queue_ptr Queue to flush.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The queue is empty.
 * @retval TX_QUEUE_ERROR ``queue_ptr`` was null or never created.
 * @pre ``queue_ptr`` came from ``tx_queue_create``.
 * @pre The caller accepts that queued messages are lost.
 * @post The enqueued count is zero.
 * @post The queue family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_queue_flush(TX_QUEUE* queue_ptr)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_queue, &injected)) {
    return injected;
  }
  if ((queue_ptr == nullptr) || (queue_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_QUEUE_ERROR;
  }
  queue_ptr->count = 0U;
  queue_ptr->head  = 0U;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_queue_send``.
 * @details Copies one message into the ring. The model cannot suspend, so a
 * full queue reports ``TX_QUEUE_FULL`` whatever the wait option; the wait the
 * port asked for is recorded so a test can still prove it requested a block.
 * @param[in,out] queue_ptr Queue to send to.
 * @param[in] source_ptr Message to copy in.
 * @param[in] wait_option Recorded wait option.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The message was stored.
 * @retval TX_QUEUE_ERROR ``queue_ptr`` was null or never created.
 * @retval TX_PTR_ERROR ``source_ptr`` was null.
 * @retval TX_QUEUE_FULL The ring was already at capacity.
 * @pre ``source_ptr`` covers the queue's message size.
 * @pre The queue was created.
 * @post On success the enqueued count has risen by one.
 * @post ``last_wait`` records this call's wait option either way.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_queue_send(TX_QUEUE* queue_ptr, void* source_ptr, ULONG wait_option)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_queue, &injected)) {
    return injected;
  }
  if ((queue_ptr == nullptr) || (queue_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_QUEUE_ERROR;
  }
  queue_ptr->last_wait = wait_option;
  if (source_ptr == nullptr) {
    return TX_PTR_ERROR;
  }
  if (queue_ptr->count >= queue_ptr->capacity) {
    return TX_QUEUE_FULL;
  }
  const uint32_t slot = (queue_ptr->head + queue_ptr->count) % queue_ptr->capacity;
  (void)memcpy(&queue_ptr->storage[slot * queue_ptr->words],
               source_ptr,
               (size_t)queue_ptr->words * sizeof(ULONG));
  queue_ptr->count++;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_queue_receive``.
 * @details Copies the oldest message out. An empty queue reports
 * ``TX_QUEUE_EMPTY`` whatever the wait option, since there is no scheduler to
 * suspend on; the requested wait is recorded regardless.
 * @param[in,out] queue_ptr Queue to receive from.
 * @param[out] destination_ptr Receives the message.
 * @param[in] wait_option Recorded wait option.
 * @return ThreadX status code.
 * @retval TX_SUCCESS A message was copied out.
 * @retval TX_QUEUE_ERROR ``queue_ptr`` was null or never created.
 * @retval TX_PTR_ERROR ``destination_ptr`` was null.
 * @retval TX_QUEUE_EMPTY The ring held no message.
 * @pre ``destination_ptr`` covers the queue's message size.
 * @pre The queue was created.
 * @post On success the enqueued count has fallen by one.
 * @post ``last_wait`` records this call's wait option either way.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_queue_receive(TX_QUEUE* queue_ptr, void* destination_ptr, ULONG wait_option)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_queue, &injected)) {
    return injected;
  }
  if ((queue_ptr == nullptr) || (queue_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_QUEUE_ERROR;
  }
  queue_ptr->last_wait = wait_option;
  if (destination_ptr == nullptr) {
    return TX_PTR_ERROR;
  }
  if (queue_ptr->count == 0U) {
    return TX_QUEUE_EMPTY;
  }
  (void)memcpy(destination_ptr,
               &queue_ptr->storage[queue_ptr->head * queue_ptr->words],
               (size_t)queue_ptr->words * sizeof(ULONG));
  queue_ptr->head = (queue_ptr->head + 1U) % queue_ptr->capacity;
  queue_ptr->count--;
  return TX_SUCCESS;
}
