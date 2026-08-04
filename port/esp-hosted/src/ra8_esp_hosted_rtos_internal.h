/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/src/ra8_esp_hosted_rtos_internal.h
 * @brief Module-private contract of the esp-hosted RTOS abstraction slice.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vendored esp-hosted core reaches ThreadX only through the 72-entry
 * ``hosted_osi_funcs_t`` vtable. This header declares the port-private
 * symbols that build the RTOS part of that vtable and the fixed-storage
 * substrate underneath it: the byte pools, the object tables, and the three
 * pieces of arithmetic (millisecond-to-tick, queue word rounding,
 * microsecond spin sizing) that carry real decisions and therefore have to be
 * reachable from ``tests/`` on their own terms.
 *
 * Nothing here is public API. Production code outside
 * ``port/esp-hosted/src/`` must go through ``ra8_esp_hosted_port_init`` and
 * then through ``g_h.funcs``.
 *
 * @par Allocation policy
 * This board has no heap: ``_sbrk`` is a strong symbol that reports a fatal
 * error. Every object the vendored core asks the vtable to "allocate" is
 * carved from a fixed ``TX_BYTE_POOL`` over a static array, or taken from a
 * fixed object table with an in-use bitmap. Exhaustion returns a failure; it
 * never grows anything. That is what keeps the port inside NASA Power of 10
 * Rule 3.
 *
 * @par File split
 * The implementation is three translation units because one would be several
 * times the project's thousand-line file cap. The split follows the object
 * kinds, which is also how the fixed tables divide:
 *   - ``ra8_esp_hosted_rtos_pool.c`` -- the byte pools, the allocator and the
 *     queues, whose storage comes out of one of those pools;
 *   - ``ra8_esp_hosted_rtos_sync.c`` -- the mutex and semaphore tables;
 *   - ``ra8_esp_hosted_rtos.c`` -- threads, sleeps, timers, the extended
 *     clock, and the lifecycle that brings the other two up.
 * Each unit binds only its own vtable rows, so a row can never be assigned
 * from a unit that cannot see the function behind it.
 *
 * @par No mempool locks
 * ``H_USE_MEMPOOL`` is deliberately left undefined, so the four
 * ``_h_*_lock_mempool`` members are absent from ``hosted_osi_funcs_t`` and
 * nothing here binds them. ``port_esp_hosted_host_config.h`` records why: the
 * vendored header guards them with ``#ifdef``, not ``#if``, so defining the
 * macro to zero would switch them on and make the struct layout depend on
 * include order.
 *
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_hosted_os_abstraction.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/* ----------------------------------------------------------------------- */
/* Lifecycle */
/* ----------------------------------------------------------------------- */

/**
 * @brief Bring the RTOS substrate up: byte pools and empty object tables.
 *
 * @details
 * Creates the two ``TX_BYTE_POOL`` instances -- the transport buffer pool
 * sized by ::k_ra8_esp_hosted_pool_bytes and the queue-storage pool sized by
 * ::k_ra8_esp_hosted_queue_pool_bytes -- over static arrays, clears every
 * object table and caches the CPU rate used to size the sub-millisecond spin
 * in ``_h_usleep``. This is the only allocation the RTOS slice ever performs
 * and it happens exactly here, during initialisation.
 *
 * A second call is an error, not a no-op: a silent success would hand the
 * caller pools whose contents the first caller still owns, and would hide a
 * double bring-up that is always a bug in the calling sequence.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The substrate is ready; the vtable may be bound.
 * @retval k_ra8_err_invalid_state The substrate was already initialised.
 * @retval k_ra8_err_rtos_error ThreadX refused to create a byte pool.
 *
 * @pre The ThreadX kernel is running, or this runs from
 *      ``tx_application_define``.
 * @pre No vendored esp-hosted entry point has been called yet.
 * @post On success the substrate reports ready and every object table is
 *       empty.
 * @post On failure no pool is left half-created and the substrate reports not
 *       ready.
 *
 * @note Not thread-safe; call once from a single-threaded bring-up path.
 * @warning The CPU-rate query is best-effort; a failure leaves the documented
 *          fallback rate in place and only affects ``_h_usleep`` accuracy.
 *
 * @par Example:
 * @code
 * if (ra8_esp_hosted_rtos_init() != k_ra8_ok) { report(); }
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_deinit
 * @see ra8_esp_hosted_rtos_bind
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 3: the only allocation is this init-time pool carve.
 * - Rule 5: two preconditions and two postconditions are checked.
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_esp_hosted_rtos_init(void);

/**
 * @brief Tear the RTOS substrate down, deleting every outstanding object.
 *
 * @details
 * Walks each object table and deletes whatever is still in use -- timers are
 * deactivated then deleted, threads terminated then deleted, queues, mutexes
 * and semaphores deleted -- then deletes both byte pools and clears the
 * module state. Objects the vendored core still holds handles to are deleted
 * regardless, so the core must be stopped first.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Everything was released.
 * @retval k_ra8_err_not_initialized The substrate was not up.
 * @retval k_ra8_err_rtos_error ThreadX refused to delete an object or a pool.
 *
 * @pre The vendored transport has been stopped.
 * @pre No interrupt handler is currently posting to a port semaphore.
 * @post Every object table is empty.
 * @post The substrate reports not ready and both pools are gone.
 *
 * @note Not thread-safe; call from the same context as the init.
 * @warning Tearing down while a thread is blocked on a port object leaves
 *          that thread waiting on a deleted control block.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_rtos_deinit();
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_init
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: every table walk is bounded by its compile-time table size.
 * - Rule 5: two preconditions and two postconditions are checked.
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_esp_hosted_rtos_deinit(void);

/**
 * @brief Report whether the RTOS substrate is currently initialised.
 *
 * @details
 * Reads the single module-state flag so callers can decide whether a teardown
 * is needed without provoking an error return, and so host tests can assert
 * the state machine without reaching into the module.
 *
 * @return Whether the substrate is up.
 * @retval true ::ra8_esp_hosted_rtos_init completed and no teardown has run.
 * @retval false The substrate has never been up, failed, or was torn down.
 *
 * @pre None; safe to call at any time, including before any init.
 * @pre The caller tolerates a value a concurrent teardown may stale.
 * @post No module state is modified.
 * @post The value reflects the flag at the moment of the read.
 *
 * @note Safe from interrupt context; a single aligned load.
 *
 * @par Example:
 * @code
 * if (ra8_esp_hosted_rtos_is_ready()) { (void)ra8_esp_hosted_rtos_deinit(); }
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_init
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] bool ra8_esp_hosted_rtos_is_ready(void);

/**
 * @brief Read live transport-pool occupancy.
 *
 * @details
 * Calls ``tx_byte_pool_info_get`` on the transport buffer pool and hands back
 * the two numbers a fragmentation problem actually shows up in: bytes still
 * available, and the fragment count. These are real ThreadX numbers, not a
 * port-side tally, which is the whole reason the port keeps a real byte pool
 * rather than a bump allocator. Backs ``ra8_esp_hosted_mem_dump``.
 *
 * @param[out] out_available Receives bytes still allocatable. May be null.
 * @param[out] out_fragments Receives the pool's fragment count. May be null.
 *
 *
 * @pre The substrate is initialised, or both outputs are set to zero.
 * @pre At least one output pointer is non-null for the call to be useful.
 * @post No pool state is modified.
 * @post Every non-null output has been written.
 *
 * @note Safe from any thread; ThreadX guards the pool internally.
 *
 * @par Example:
 * @code
 * uint32_t avail = 0U;
 * uint32_t frags = 0U;
 * ra8_esp_hosted_rtos_pool_stats(&avail, &frags);
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_init
 * @since 0.1.0
 */
RA8_PRIV void ra8_esp_hosted_rtos_pool_stats(uint32_t* out_available, uint32_t* out_fragments);

/**
 * @brief Populate the thread, sleep, timer and clock slots of the vtable.
 *
 * @details
 * Writes the ten rows this translation unit implements. It does **not** call
 * the sibling binders: each unit binds only the rows whose implementations it
 * can see, so a row can never be assigned from a unit that cannot name the
 * function behind it. ``ra8_esp_hosted_osi_bind_all`` calls all three.
 * Every slot outside this group -- memory, queue, mutex, semaphore, GPIO,
 * bus, logging, transport -- is left exactly as the caller had it.
 *
 * @param[out] out Vtable to populate. Must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The ten thread, sleep, timer and clock slots are set.
 * @retval k_ra8_err_null_ptr ``out`` was null.
 *
 * @pre ``out`` points at storage that outlives the vendored core.
 * @pre ::ra8_esp_hosted_rtos_init has run, or the slots will report failures.
 * @post Every slot in this group is non-null.
 * @post No slot outside this group is written.
 *
 * @note Not thread-safe; call once during bring-up.
 * @warning Binding without initialising first produces a vtable whose
 *          allocators fail every call rather than one that faults.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_rtos_bind(&g_hosted_osi_funcs);
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_bind_pool
 * @see ra8_esp_hosted_rtos_bind_sync
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_esp_hosted_rtos_bind(hosted_osi_funcs_t* out);

/**
 * @brief Populate the memory and queue slots of the vtable.
 *
 * @details
 * The binder of the pool translation unit, which owns the byte pools the
 * allocators and queue rings draw from. Called by
 * ``ra8_esp_hosted_osi_bind_all`` alongside the other two binders.
 *
 * @param[out] out Vtable to populate. Must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every memory and queue slot is populated.
 * @retval k_ra8_err_null_ptr ``out`` was null.
 *
 * @pre ``out`` points at storage that outlives the vendored core.
 * @pre ::ra8_esp_hosted_rtos_init has run, or the slots will report failures.
 * @post The eight memory slots and six queue slots are non-null.
 * @post No other slot is written.
 *
 * @note Not thread-safe; call once during bring-up.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_rtos_bind_pool(&funcs);
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_bind
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_esp_hosted_rtos_bind_pool(hosted_osi_funcs_t* out);

/**
 * @brief Populate the mutex and semaphore slots of the vtable.
 *
 * @details
 * The binder of the synchronisation translation unit, which owns the mutex
 * and semaphore tables. Called by ``ra8_esp_hosted_osi_bind_all`` alongside
 * the other two binders. There are no mempool-lock rows to fill; see the
 * file-level note on ``H_USE_MEMPOOL``.
 *
 * @param[out] out Vtable to populate. Must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every mutex and semaphore slot is populated.
 * @retval k_ra8_err_null_ptr ``out`` was null.
 *
 * @pre ``out`` points at storage that outlives the vendored core.
 * @pre ::ra8_esp_hosted_rtos_init has run, or the slots will report failures.
 * @post The four mutex slots and five semaphore slots are non-null.
 * @post No other slot is written.
 *
 * @note Not thread-safe; call once during bring-up.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_rtos_bind_sync(&funcs);
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_bind
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_esp_hosted_rtos_bind_sync(hosted_osi_funcs_t* out);

/**
 * @brief Clear the mutex and semaphore tables and mark them usable.
 *
 * @details
 * The synchronisation half of ::ra8_esp_hosted_rtos_init. Separated so the
 * unit that owns the tables also owns their lifecycle, and so a test can
 * bring the locks up without the allocator.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Both tables are empty and usable.
 * @retval k_ra8_err_invalid_state The tables were already initialised.
 *
 * @pre The ThreadX kernel is running.
 * @pre No handle from a previous incarnation of the tables is still held.
 * @post Every occupancy flag is clear.
 * @post A create call will now succeed.
 *
 * @note Not thread-safe; call once during bring-up.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_rtos_sync_init();
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_sync_deinit
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_esp_hosted_rtos_sync_init(void);

/**
 * @brief Delete every outstanding mutex and semaphore.
 *
 * @details
 * The synchronisation half of ::ra8_esp_hosted_rtos_deinit: walks both tables
 * and deletes whatever is still in use, then clears the state so a later
 * create fails cleanly rather than touching a dead control block.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every object was released.
 * @retval k_ra8_err_not_initialized The tables were not initialised.
 * @retval k_ra8_err_rtos_error ThreadX refused a delete.
 *
 * @pre No thread is blocked on a port mutex or semaphore.
 * @pre The vendored core has stopped using its handles.
 * @post Every occupancy flag is clear.
 * @post A later take through a stale handle reports ``RET_INVALID``.
 *
 * @note Not thread-safe; call from the same context as the init.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_rtos_sync_deinit();
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_sync_init
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_esp_hosted_rtos_sync_deinit(void);

/* ----------------------------------------------------------------------- */
/* Fixed-table helpers */
/* ----------------------------------------------------------------------- */

/**
 * @brief Claim the first free row of an occupancy bitmap.
 *
 * @details
 * The one place a table row is taken, shared by every object kind so none of
 * them can grow its table by accident. Returning ``count`` rather than a
 * sentinel index keeps the caller's bound check and the failure check the
 * same comparison.
 *
 * @param[in,out] used Occupancy flags; must cover ``count`` entries.
 * @param[in] count Number of rows in the table.
 *
 * @return Index of the claimed row, or ``count`` when the table is full.
 * @retval count Every row was occupied, or ``used`` was null.
 * @retval 0..count-1 The claimed row, now marked in use.
 *
 * @pre ``used`` covers ``count`` entries.
 * @pre The caller releases the row on any later failure.
 * @post Exactly one flag changes from false to true on success.
 * @post No flag changes when the table is full.
 *
 * @note Not thread-safe against a concurrent claim; creates happen during
 *       single-threaded bring-up.
 * @warning A claimed row is in use even if the caller then fails to create
 *          the ThreadX object; release it explicitly on that path.
 *
 * @par MC/DC:
 * Two single-condition decisions, ``used == nullptr`` and ``!used[i]``; no
 * compound condition, so a null table, a table with a free row, and a full
 * table cover every outcome.
 *
 * @par Example:
 * @code
 * const uint32_t idx = ra8_esp_hosted_rtos_slot_take(s_used, k_max);
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_slot_index
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] uint32_t ra8_esp_hosted_rtos_slot_take(bool* used, uint32_t count);

/**
 * @brief Resolve an opaque handle to its row index in a fixed table.
 *
 * @details
 * Every handle the port hands the vendored core is the address of a table
 * row. Rather than trusting that address, this scans the table for pointer
 * identity and checks the occupancy flag, so a foreign pointer or a handle
 * retained past its destroy is rejected instead of followed. The tables hold
 * at most eight rows, so the scan is cheaper than any bookkeeping that would
 * replace it.
 *
 * @param[in] handle Opaque handle the core is holding.
 * @param[in] base Address of the table's first row.
 * @param[in] stride Bytes between consecutive rows.
 * @param[in] count Number of rows in the table.
 * @param[in] used Occupancy flags for the same table.
 *
 * @return Row index, or ``count`` when the handle is not live.
 * @retval count The handle is null, foreign, or names a freed row.
 * @retval 0..count-1 The live row index.
 *
 * @pre ``base`` and ``used`` describe the same table.
 * @pre ``stride`` is ``sizeof`` one row.
 * @post No state is modified.
 * @post A returned index always has its occupancy flag set.
 *
 * @note Thread-safe for reads; the tables are mutated only during bring-up
 *       and teardown.
 * @warning Passing a ``stride`` that is not the real row size makes every
 *          lookup miss rather than fault.
 *
 * @par MC/DC:
 * Decision ``(handle == nullptr) || (base == nullptr) || (used == nullptr)``
 * (3 conditions) plus ``(row == handle) && used[i]`` (2 conditions). Four
 * vectors cover the first -- all non-null, then each argument null in turn --
 * and three cover the second: a matching live row, a non-matching row, and a
 * matching row whose flag is clear.
 *
 * @par Example:
 * @code
 * const uint32_t idx = ra8_esp_hosted_rtos_slot_index(
 *   h, s_rows, sizeof(s_rows[0]), k_max, s_used);
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_slot_take
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] uint32_t ra8_esp_hosted_rtos_slot_index(const void* handle,
                                                               const void* base,
                                                               size_t      stride,
                                                               uint32_t    count,
                                                               const bool* used);

/**
 * @brief Create the two byte pools over their static backing arrays.
 *
 * @details
 * The pool half of ::ra8_esp_hosted_rtos_init. Separated so the memory
 * translation unit owns both the arrays and the control blocks, and so a test
 * can bring the allocator up without the thread and timer tables.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Both pools are ready.
 * @retval k_ra8_err_invalid_state The pools were already created.
 * @retval k_ra8_err_rtos_error ThreadX refused one of the pools.
 *
 * @pre The ThreadX kernel is running.
 * @pre No block from a previous incarnation of the pools is still held.
 * @post On success both pools report their full size available.
 * @post On failure neither pool is left half-created.
 *
 * @note Not thread-safe; call once during bring-up.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_rtos_pool_init();
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_pool_deinit
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_esp_hosted_rtos_pool_init(void);

/**
 * @brief Destroy every queue and both byte pools.
 *
 * @details
 * The pool half of ::ra8_esp_hosted_rtos_deinit: deletes any queue still in
 * use, releases its storage, then deletes both pools and clears the pool
 * state so a later allocation fails cleanly rather than touching a dead
 * control block.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Both pools and every queue were released.
 * @retval k_ra8_err_not_initialized The pools were not created.
 * @retval k_ra8_err_rtos_error ThreadX refused a delete.
 *
 * @pre No thread is blocked on a port queue.
 * @pre The vendored core has stopped allocating.
 * @post The queue table is empty.
 * @post Neither pool satisfies a later allocation.
 *
 * @note Not thread-safe; call from the same context as the init.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_rtos_pool_deinit();
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_pool_init
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_esp_hosted_rtos_pool_deinit(void);

/* ----------------------------------------------------------------------- */
/* Testable arithmetic */
/* ----------------------------------------------------------------------- */

/**
 * @brief Convert an esp-hosted millisecond timeout to a ThreadX wait option.
 *
 * @details
 * The vendored core expresses every timeout in milliseconds through an
 * ``int`` parameter, and ThreadX takes a tick count. Two values are special:
 * zero means "do not block" and ``HOSTED_BLOCK_MAX`` (all ones) means "block
 * until satisfied". ``HOSTED_BLOCK_MAX`` reaches the vtable as -1 after its
 * conversion to ``int``, so the rule this function implements is "any
 * negative value blocks forever" -- deliberately wider than "exactly -1", so
 * a sign-extension bug elsewhere can never turn an intended block into a
 * zero-tick busy loop. The kernel runs at 1 kHz
 * (``TX_TIMER_TICKS_PER_SECOND`` is 1000), so the positive case is the
 * identity; it is still spelled out here rather than inlined at 30 call sites
 * so that changing the tick rate is a one-line change and so the mapping is
 * directly testable.
 *
 * @param[in] timeout_ms Timeout in milliseconds: 0 = do not block, negative =
 *                       block forever, positive = that many milliseconds.
 *
 * @return ThreadX wait option in ticks.
 * @retval 0 ``timeout_ms`` was zero: ``TX_NO_WAIT``.
 * @retval 0xFFFFFFFF ``timeout_ms`` was negative: ``TX_WAIT_FOREVER``.
 * @retval 1..0x7FFFFFFF The millisecond count, unchanged.
 *
 * @pre The kernel tick is 1 kHz.
 * @pre The caller passes the value the vendored core supplied, unmodified.
 * @post No state is modified.
 * @post The result is never ``TX_WAIT_FOREVER`` for a non-negative input.
 *
 * @note Pure function; safe from any context including interrupts.
 * @warning Do not pre-clamp the argument at the call site; the negative case
 *          is load-bearing.
 *
 * @par MC/DC:
 * Three-way decision with no compound condition: ``timeout_ms < 0`` is tested
 * first, then ``timeout_ms == 0``. Independence is trivial because each
 * decision has a single condition; the vectors that cover every outcome are
 * -1 (and ``(int)HOSTED_BLOCK_MAX``), 0, and any positive value.
 *
 * @par Example:
 * @code
 * const ULONG wait = (ULONG)ra8_esp_hosted_rtos_ms_to_ticks(timeout_ms);
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_queue_words
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] uint32_t ra8_esp_hosted_rtos_ms_to_ticks(int timeout_ms);

/**
 * @brief Round an esp-hosted queue element size up to whole ThreadX words.
 *
 * @details
 * ``tx_queue_create`` takes its message size in 32-bit words, capped at
 * ``TX_16_ULONG``. The core asks for byte sizes that are not necessarily a
 * whole number of words -- ``sizeof(interface_buffer_handle_t)`` is 28 bytes
 * on this ABI, which is seven words exactly, but nothing in the vtable
 * contract guarantees that. The rounding is upward, so an element always
 * fits; the cost is at most three unused bytes per message, which is why
 * rounding is preferable to rejecting an odd size.
 *
 * A request that would need more than sixteen words is rejected rather than
 * truncated: ThreadX would refuse the create anyway, and a truncating port
 * would silently corrupt every message.
 *
 * @param[in] item_bytes Element size in bytes as the core supplied it.
 *
 * @return Message size in 32-bit words, or zero when unusable.
 * @retval 0 ``item_bytes`` was zero, or exceeded sixteen words.
 * @retval 1..16 The rounded-up word count.
 *
 * @pre ``item_bytes`` is the size the core will actually copy.
 * @pre The caller treats zero as "refuse to create the queue".
 * @post No state is modified.
 * @post The result multiplied by four is at least ``item_bytes``.
 *
 * @note Pure function; safe from any context.
 * @warning A zero return must not be passed to ``tx_queue_create``.
 *
 * @par MC/DC:
 * Decision ``(item_bytes == 0U) || (words > k_ra8_esp_hosted_queue_words_max)``
 * (2 conditions). Vector 1: 28 bytes -> false/false. Vector 2: 0 bytes ->
 * true/(short-circuit). Vector 3: 68 bytes -> false/true. Vectors 1+2 prove
 * the emptiness condition's independent influence; 1+3 prove the same for the
 * word cap. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * @par Example:
 * @code
 * const uint32_t words = ra8_esp_hosted_rtos_queue_words((uint32_t)qitem_size);
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_ms_to_ticks
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] uint32_t ra8_esp_hosted_rtos_queue_words(uint32_t item_bytes);

/**
 * @brief Size the busy-wait loop that stands in for a microsecond delay.
 *
 * @details
 * There is no microsecond timer on this target: ``ra8_time.h`` offers a
 * millisecond tick and a millisecond busy delay, and nothing finer. Rather
 * than fake a microsecond sleep or round every sub-millisecond request to
 * zero, ``_h_usleep`` spins, and this function decides for how long. The
 * estimate is ``cpu_hz / 1e6`` cycles per microsecond divided by
 * ::k_ra8_esp_hosted_spin_cycles_per_iter, the measured cost of one iteration
 * of the volatile-counter loop on the Cortex-M85.
 *
 * The result is clamped to ::k_ra8_esp_hosted_spin_iters_max so the loop is
 * statically bounded (NASA Power of 10 Rule 2) whatever the caller asks for.
 * Accuracy is roughly a factor of two: cache state, the branch predictor and
 * any interrupt taken mid-spin all move the real duration, and none of them
 * is modelled. That is stated rather than hidden -- a caller needing better
 * than that needs a hardware timer, not a better guess.
 *
 * @param[in] cpu_hz Core clock in hertz; zero means "rate unknown".
 * @param[in] usec Microseconds to spin for.
 *
 * @return Loop iterations to execute.
 * @retval 0 ``cpu_hz`` or ``usec`` was zero: do not spin at all.
 * @retval 1..k_ra8_esp_hosted_spin_iters_max The clamped iteration count.
 *
 * @pre ``cpu_hz`` is the live core rate, or zero.
 * @pre The caller runs the loop with a volatile counter so it is not deleted.
 * @post No state is modified.
 * @post The result never exceeds ::k_ra8_esp_hosted_spin_iters_max.
 *
 * @note Pure function; safe from any context.
 * @warning The duration is an estimate, not a guarantee; do not use it for a
 *          protocol timing requirement.
 *
 * @par MC/DC:
 * Decision ``(cpu_hz == 0U) || (usec == 0U)`` (2 conditions). Vector 1:
 * cpu_hz=1e9, usec=10 -> false/false. Vector 2: cpu_hz=0, usec=10 ->
 * true/(short-circuit). Vector 3: cpu_hz=1e9, usec=0 -> false/true. Vectors
 * 1+2 prove the rate condition's independent influence; 1+3 prove the same
 * for the duration. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * @par Example:
 * @code
 * const uint32_t iters = ra8_esp_hosted_rtos_us_spin_iters(cpu_hz, 250U);
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_ms_to_ticks
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] uint32_t ra8_esp_hosted_rtos_us_spin_iters(uint32_t cpu_hz, uint32_t usec);

/* ----------------------------------------------------------------------- */
/* Fixed-storage allocator */
/* ----------------------------------------------------------------------- */

/**
 * @brief Allocate an aligned block from the transport byte pool.
 *
 * @details
 * The single allocation primitive underneath ``_h_malloc``, ``_h_calloc``,
 * ``_h_realloc`` and ``_h_malloc_align``. ThreadX byte pools neither record a
 * block's size nor honour an alignment request, and ``tx_byte_release``
 * demands the exact pointer ``tx_byte_allocate`` returned -- so the port
 * over-allocates and lays each block out as:
 *
 * @verbatim
 * base ---> [ padding 0..align-1 ][ 16-byte header ][ payload (size bytes) ]
 *                                 ^                 ^
 *                                 |                 +-- returned to caller
 *                                 +-- header sits immediately below it
 * @endverbatim
 *
 * The header holds the base pointer (so the release can hand ThreadX exactly
 * what it gave out), the payload size (so ``_h_realloc`` can copy the right
 * number of bytes) and a sentinel (so a foreign pointer is rejected rather
 * than followed). It is written and read with ``memcpy`` because its address
 * inherits only the requested alignment, which may be weaker than the
 * pointer's own.
 *
 * Worst-case overhead is ``16 + align - 1`` bytes: 16 bytes for a plain
 * ``_h_malloc`` (which asks for no extra alignment) and 79 bytes for a
 * 64-byte-aligned transport buffer.
 *
 * @param[in] size Payload bytes required. Must be non-zero.
 * @param[in] align Required payload alignment in bytes; must be a power of
 *                  two no greater than ::k_ra8_esp_hosted_align_max. Pass 1
 *                  for "no particular alignment".
 *
 * @return Pointer to the aligned payload, or null on failure.
 * @retval nullptr The substrate is down, an argument was out of contract, or
 *         the pool could not satisfy the request.
 * @retval non-null A block of at least ``size`` bytes aligned to ``align``.
 *
 * @pre The substrate is initialised.
 * @pre ``align`` is a power of two.
 * @post On success the returned address is a multiple of ``align``.
 * @post On failure the pool is left exactly as it was.
 *
 * @note Thread-safe; ThreadX serialises the pool internally.
 * @warning The block must be released with ::ra8_esp_hosted_rtos_release, not
 *          with ``tx_byte_release``, or the padding leaks.
 *
 * @par Example:
 * @code
 * void* p = ra8_esp_hosted_rtos_alloc(1600U, 64U);
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_release
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 3: draws from a fixed init-time pool; never grows.
 * - Rule 5: three preconditions and two postconditions are checked.
 */
RA8_PRIV [[nodiscard]] void* ra8_esp_hosted_rtos_alloc(size_t size, size_t align);

/**
 * @brief Release a block obtained from ::ra8_esp_hosted_rtos_alloc.
 *
 * @details
 * Reads the header immediately below ``ptr``, checks its sentinel, and hands
 * ThreadX the original base pointer. Because every allocation carries the
 * same header, an aligned block and a plain one are released identically --
 * which is why ``_h_free`` and ``_h_free_align`` can be, and are, the same
 * operation.
 *
 * @param[in] ptr Payload pointer previously returned by the allocator. A null
 *                pointer is accepted and reported as invalid rather than
 *                dereferenced.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The block was returned to the pool.
 * @retval k_ra8_err_null_ptr ``ptr`` was null.
 * @retval k_ra8_err_invalid_arg The header sentinel did not match.
 * @retval k_ra8_err_rtos_error ThreadX refused the release.
 *
 * @pre ``ptr`` came from ::ra8_esp_hosted_rtos_alloc.
 * @pre The block has not already been released.
 * @post On success the pool reports the block's bytes as available again.
 * @post On failure no pool state changes.
 *
 * @note Thread-safe; ThreadX serialises the pool internally.
 * @warning Releasing a foreign pointer is detected by the sentinel, but a
 *          double release of a reallocated address is not.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_rtos_release(p);
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_alloc
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_esp_hosted_rtos_release(void* ptr);

/**
 * @brief Read back the payload size recorded for an allocated block.
 *
 * @details
 * Exists because ``_h_realloc`` has to copy ``min(old, new)`` bytes and
 * ThreadX byte pools do not record a block's size. The value comes from the
 * header ::ra8_esp_hosted_rtos_alloc wrote, so it is the size the caller
 * asked for, not the rounded pool footprint.
 *
 * @param[in] ptr Payload pointer previously returned by the allocator.
 * @param[out] out_size Receives the recorded payload size in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok ``out_size`` holds the recorded size.
 * @retval k_ra8_err_null_ptr ``ptr`` or ``out_size`` was null.
 * @retval k_ra8_err_invalid_arg The header sentinel did not match.
 *
 * @pre ``ptr`` came from ::ra8_esp_hosted_rtos_alloc.
 * @pre ``out_size`` is writable.
 * @post On success ``*out_size`` is the size originally requested.
 * @post No pool state is modified.
 *
 * @note Thread-safe; reads immutable header bytes.
 *
 * @par Example:
 * @code
 * size_t n = 0U;
 * (void)ra8_esp_hosted_rtos_block_size(p, &n);
 * @endcode
 *
 * @see ra8_esp_hosted_rtos_alloc
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_esp_hosted_rtos_block_size(const void* ptr, size_t* out_size);
