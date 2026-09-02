/**
 * @file port/esp-hosted/inc/port_esp_hosted_host_os.h
 * @brief esp-hosted port header: RTOS handle types, return codes and budgets.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * One of the ten port headers the vendored esp-hosted core includes by
 * name (see ``docs/SOUP/esp-hosted-host.md``); twelve vendored translation
 * units reach it. Upstream ships an ESP-IDF / FreeRTOS version under
 * ``host/port/``, which this project excludes; this file is its
 * first-party ThreadX replacement and therefore keeps the upstream file
 * name and every upstream symbol spelling exactly.
 *
 * The core never names a ThreadX type. It moves opaque handles produced by
 * the OS-abstraction vtable (``hosted_osi_funcs_t``) around, so every
 * handle typedef here is ``void*`` and the concrete control block lives
 * behind it in ``port/esp-hosted/src/``. That is what makes the whole
 * hardware- and RTOS-facing surface of this SOUP first-party code.
 *
 * @par Timeout units
 * Every timeout the core passes is in **milliseconds**, never ticks. Two
 * values are special and both are spelled by the core, not invented here:
 *   - ``0`` means "do not block" -- ``spi_drv.c`` relies on this for its
 *     non-blocking queue drain and its try-take of the transmit-pending
 *     semaphore;
 *   - ::HOSTED_BLOCK_MAX means "block until satisfied".
 * ThreadX on this board runs a 1 kHz tick (``TX_TIMER_TICKS_PER_SECOND``
 * is 1000 in ``port/threadx/inc/tx_user.h``), so the millisecond-to-tick
 * conversion in the port is the identity.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* ESP-IDF hosts reach these through their own build; on this host the
   port supplies them, and this header is where the vendored core expects
   its environment to arrive from. ``esp_heap_caps.h`` carries the
   allocation capability flags the transport passes through,
   ``<inttypes.h>`` carries the PRIu32 family the serial driver formats
   with, ``<stdlib.h>`` declares the ``calloc`` that
   ``transport_util_calloc`` reaches for on the non-DMA arm, and
   ``<assert.h>`` carries the ``assert`` macro ``spi_drv.c`` guards every
   handle creation in ``bus_init_internal`` with. Without the latter two
   the vendored translation units compiled with C89 implicit declarations:
   ``calloc`` came back as ``int`` (``-Wbuiltin-declaration-mismatch``) and
   ``assert`` became a CALL to a function nothing defines, so the check was
   not a check. */
#include "esp_heap_caps.h"

/* The allocation macros below expand to calls through ``g_h``, so the
   vtable declaration has to come with them -- several vendored
   translation units include this header and nothing else that declares
   it. */
#include "esp_hosted_os_abstraction.h"
#include "ra8_esp_hosted_port.h"

/* ----------------------------------------------------------------------- */
/* Opaque RTOS handles */
/*                                                                          */
/* The vendored core stores these and hands them back to the vtable; it */
/* never dereferences one. Keeping them void* is what lets the concrete */
/* ThreadX control blocks stay entirely inside the port. */
/* ----------------------------------------------------------------------- */

/**
 * @typedef queue_handle_t
 * @brief Opaque handle to a port-owned message queue.
 * @details Produced by ``_h_create_queue`` and consumed by
 * ``_h_queue_item`` / ``_h_dequeue_item`` / ``_h_destroy_queue``. Backed by
 * a ``TX_QUEUE`` plus its storage inside the port.
 * @note Never dereferenced by the vendored core.
 * @since 0.1.0
 */
typedef void* queue_handle_t;

/**
 * @typedef semaphore_handle_t
 * @brief Opaque handle to a port-owned counting semaphore.
 * @details Produced by ``_h_create_semaphore``. The SPI driver keeps three
 * of these (transaction-ready, transmit-queue-pending, receive-queue-
 * pending) as file-scope globals.
 * @note Posted from interrupt context via ``_h_post_semaphore_from_isr``.
 * @since 0.1.0
 */
typedef void* semaphore_handle_t;

/**
 * @typedef mutex_handle_t
 * @brief Opaque handle to a port-owned mutex.
 * @details Produced by ``_h_create_mutex``; guards the SPI bus so a
 * direct power-save transaction cannot interleave with the transaction
 * task.
 * @note Priority inheritance is enabled by the port.
 * @since 0.1.0
 */
typedef void* mutex_handle_t;

/**
 * @typedef thread_handle_t
 * @brief Opaque handle to a port-owned thread.
 * @details Produced by ``_h_thread_create`` and passed back to
 * ``_h_thread_cancel``. The SPI transport creates two: the transaction
 * pump and the receive dispatcher.
 * @note Threads are drawn from a fixed pool; there is no heap.
 * @since 0.1.0
 */
typedef void* thread_handle_t;

/**
 * @typedef spinlock_handle_t
 * @brief Opaque handle to a port-owned mempool lock.
 * @details Produced by ``_h_create_lock_mempool``. Despite the upstream
 * name it is not a spin lock here: on a single-core RTOS the correct
 * primitive is a mutex, and that is what the port supplies.
 * @note Only used when ::H_USE_MEMPOOL is set.
 * @since 0.1.0
 */
typedef void* spinlock_handle_t;

/**
 * @typedef gpio_pin_state_t
 * @brief Logic level read back from, or driven onto, a side-band pin.
 * @details The core compares values of this type against
 * ::H_HS_VAL_ACTIVE and friends, so it must be an integer type wide enough
 * for a zero/one level. ``_h_read_gpio`` returns ``int``, which converts
 * to this without loss.
 * @note Zero is a low level, one is a high level.
 * @since 0.1.0
 */
typedef uint8_t gpio_pin_state_t;

/* ----------------------------------------------------------------------- */
/* Return codes */
/* ----------------------------------------------------------------------- */

/**
 * @def RET_OK
 * @brief Operation completed.
 * @details Zero, so ``if (!g_h.funcs->_h_get_semaphore(sem, 0))`` reads as
 * "the take succeeded" -- a spelling the vendored SPI driver depends on.
 * @note Read-only build configuration.
 * @warning Must stay zero; the core tests these codes for truthiness.
 * @par Example:
 * @code
 * if (rc == RET_OK) { proceed(); }
 * @endcode
 * @since 0.1.0
 */
#define RET_OK (0)

/**
 * @def RET_FAIL
 * @brief Operation failed for an unspecified reason.
 * @details Minus one, matching the vendored ``FAILURE`` spelling.
 * @note Read-only build configuration.
 * @warning Distinct from ::RET_FAIL_TIMEOUT so callers can retry sensibly.
 * @par Example:
 * @code
 * return RET_FAIL;
 * @endcode
 * @since 0.1.0
 */
#define RET_FAIL (-1)

/**
 * @def RET_INVALID
 * @brief A parameter was out of contract (null handle, bad size).
 * @details Reported for programming errors rather than runtime conditions.
 * @note Read-only build configuration.
 * @warning The core mostly treats any non-zero code alike; the distinction
 *          exists for the port's own logs and tests.
 * @par Example:
 * @code
 * if (queue_handle == nullptr) { return RET_INVALID; }
 * @endcode
 * @since 0.1.0
 */
#define RET_INVALID (-2)

/**
 * @def RET_FAIL_TIMEOUT
 * @brief The requested wait expired before the operation could complete.
 * @details Distinguishes "nothing arrived in time" from a hard failure so
 * the SPI transaction pump can keep polling rather than tear down.
 * @note Read-only build configuration.
 * @warning A zero-millisecond wait that finds nothing reports this too.
 * @par Example:
 * @code
 * if (rc == RET_FAIL_TIMEOUT) { continue; }
 * @endcode
 * @since 0.1.0
 */
#define RET_FAIL_TIMEOUT (-3)

/* ----------------------------------------------------------------------- */
/* Blocking policy */
/* ----------------------------------------------------------------------- */

/**
 * @def HOSTED_BLOCKING
 * @brief Marker meaning "this call may block".
 * @details Passed where the core wants a blocking flag rather than a
 * duration.
 * @note Read-only build configuration.
 * @warning Not a timeout; do not pass it where milliseconds are expected.
 * @par Example:
 * @code
 * serial_drv_open(SERIAL_IF_FILE, HOSTED_BLOCKING);
 * @endcode
 * @since 0.1.0
 */
#define HOSTED_BLOCKING (1)

/**
 * @def HOSTED_BLOCK_MAX
 * @brief Timeout value meaning "wait until satisfied".
 * @details All ones. The port maps exactly this value onto ThreadX's
 * ``TX_WAIT_FOREVER``; every other value is a millisecond count. The
 * vtable takes ``int`` timeouts, so this arrives as -1 after conversion --
 * the port therefore treats "all ones or any negative value" as forever,
 * which keeps a sign-extension bug from silently becoming a busy loop.
 * @note Read-only build configuration.
 * @warning A thread blocked on this can only be released by a post or by
 *          thread termination.
 * @par Example:
 * @code
 * g_h.funcs->_h_get_semaphore(sem, HOSTED_BLOCK_MAX);
 * @endcode
 * @since 0.1.0
 */
#define HOSTED_BLOCK_MAX (0xFFFFFFFF)

/**
 * @def SEC_TO_MILLISEC
 * @brief Convert whole seconds to milliseconds.
 * @details The core expresses a few waits in seconds and multiplies by
 * this to reach the millisecond-based vtable.
 * @param[in] x Seconds to convert.
 * @note Read-only build configuration.
 * @warning Argument is evaluated once but is not parenthesised by the
 *          caller; pass a simple expression.
 * @par Example:
 * @code
 * g_h.funcs->_h_msleep(SEC_TO_MILLISEC(2));
 * @endcode
 * @since 0.1.0
 */
#define SEC_TO_MILLISEC(x) ((x) * (1000))

/* ----------------------------------------------------------------------- */
/* Memory */
/* ----------------------------------------------------------------------- */

/**
 * @def HOSTED_MEM_ALIGNMENT_64
 * @brief Alignment, in bytes, demanded of transport buffers.
 * @details Sixty-four. Upstream chose it for ESP DMA; it is equally
 * correct here because the Cortex-M85 data cache line on this part is 64
 * bytes, so a buffer aligned to it can be cleaned and invalidated without
 * touching a neighbouring allocation.
 * @note Read-only build configuration.
 * @warning Lowering this would make cache maintenance around a DMA-driven
 *          SPI transfer corrupt adjacent data.
 * @par Example:
 * @code
 * void *p = g_h.funcs->_h_malloc_align(len, HOSTED_MEM_ALIGNMENT_64);
 * @endcode
 * @since 0.1.0
 */
#define HOSTED_MEM_ALIGNMENT_64 (64)

/**
 * @def HOSTED_UNLIKELY
 * @brief Branch hint: the controlling expression is expected to be false.
 * @details ESP-IDF supplies the vendor-facing ``unlikely`` spelling from
 * ``esp_compiler.h``. The ``esp_hosted_objs`` target maps that token to this
 * first-party macro without modifying the pinned vendor tree. A macro, not a
 * function: ``__builtin_expect`` has to see the expression at the call site
 * to steer code layout.
 * @param[in] x Expression to test; evaluated exactly once.
 * @note Read-only build configuration.
 * @warning A wrong hint costs a mispredict, never correctness.
 * @par Example:
 * @code
 * if (HOSTED_UNLIKELY(ret)) { HOSTED_FREE(copy_payload); }
 * @endcode
 * @since 0.1.0
 */
#define HOSTED_UNLIKELY(x) (__builtin_expect(!!(x), 0))

/**
 * @def HOSTED_LIKELY
 * @brief Branch hint: the controlling expression is expected to be true.
 * @details The companion of ::HOSTED_UNLIKELY. The vendored ``likely`` token
 * is mapped to this macro only while compiling ``esp_hosted_objs``.
 * @param[in] x Expression to test; evaluated exactly once.
 * @note Read-only build configuration.
 * @warning A wrong hint costs a mispredict, never correctness.
 * @par Example:
 * @code
 * if (HOSTED_LIKELY(len > 0)) { transmit(buf, len); }
 * @endcode
 * @since 0.1.0
 */
#define HOSTED_LIKELY(x) (__builtin_expect(!!(x), 1))

/**
 * @brief ESP-IDF capability-aware allocation, served from the port pool.
 *
 * @details
 * ``host/drivers/transport/transport_util.c`` calls this on the
 * capability-carrying arm of ``transport_util_calloc``. ESP-IDF implements
 * it in its heap component; this host has no such component, and until this
 * definition existed the call compiled under the C89 implicit-declaration
 * rule as ``int heap_caps_malloc()`` -- an ``int`` return assigned straight
 * into a ``void *`` -- and left an undefined ``heap_caps_malloc`` reference
 * in the object that only linker garbage collection kept out of the image.
 *
 * The RA8D2 has one transport pool and it is internal, byte-addressable
 * SRAM reachable by both the DMAC and the SCI, so every capability the
 * vendored tree asks for is satisfiable from it. The one bit that changes
 * the ANSWER rather than the question is ::MALLOC_CAP_DMA, which is routed
 * through the aligned allocator so the block starts on a
 * ::HOSTED_MEM_ALIGNMENT_64 boundary and cache maintenance around a
 * DMA-driven transfer cannot touch a neighbouring allocation. That is the
 * identical treatment ``transport_util_malloc`` gives ``HOSTED_MEM_CAP_DMA``
 * two functions above the call site.
 *
 * @param[in] size Payload bytes required; must be non-zero.
 * @param[in] caps Bitwise or of ::ra8_esp_hosted_malloc_cap_t values.
 *
 * @return Pointer to the payload, or null on failure.
 * @retval nullptr The vtable is not bound, ``size`` is zero, or the pool
 *         could not serve the request.
 * @retval non-null A block of at least ``size`` bytes, aligned to
 *         ::HOSTED_MEM_ALIGNMENT_64 when ::MALLOC_CAP_DMA was requested.
 *
 * @pre ``ra8_esp_hosted_port_init`` has bound the OS-abstraction vtable.
 * @pre ``size`` is non-zero.
 * @post On success the pool reports fewer bytes available.
 * @post On failure the pool is unchanged.
 *
 * @note Thread-safe; ThreadX serialises the underlying byte pool.
 * @warning Release the block with ``g_h.funcs->_h_free_align`` when
 *          ::MALLOC_CAP_DMA was requested and ``_h_free`` otherwise; the
 *          port uses one header for both, so either spelling is correct,
 *          but ``free()`` is not.
 *
 * @par Example:
 * @code
 * void *dma = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
 * @endcode
 *
 * @see ra8_esp_hosted_malloc_cap_t The capability bits.
 * @since 0.1.0
 */
static inline void* heap_caps_malloc(size_t size, uint32_t caps)
{
  void* result = nullptr;
  if (g_h.funcs != nullptr) {
    if (size != 0U) {
      if ((caps & (uint32_t)MALLOC_CAP_DMA) != 0U) {
        result = g_h.funcs->_h_malloc_align(size, (size_t)HOSTED_MEM_ALIGNMENT_64);
      } else {
        result = g_h.funcs->_h_malloc(size);
      }
    }
  }
  return result;
}

/**
 * @def HOSTED_FREE
 * @brief Release a pointer through the vtable and null the variable.
 * @details Nulling is part of the contract: the vendored code frees a
 * buffer on one path and tests the same variable on another.
 * @param[in,out] buff Pointer variable to release and clear.
 * @note Read-only build configuration.
 * @warning ``buff`` must be an lvalue; the macro assigns to it.
 * @par Example:
 * @code
 * HOSTED_FREE(copy_payload);
 * @endcode
 * @since 0.1.0
 */
#define HOSTED_FREE(buff)                                                                          \
  do {                                                                                             \
    if (buff) {                                                                                    \
      g_h.funcs->_h_free(buff);                                                                    \
      (buff) = nullptr;                                                                            \
    }                                                                                              \
  } while (0)

/*
 * HOSTED_CALLOC is DELIBERATELY NOT DEFINED. Read this before adding it.
 *
 * Upstream's version is an allocate-or-bail macro whose failure arm is a
 * ``goto`` to a caller-supplied label. Defining it here would put a ``goto``
 * in first-party code, which NASA Power of 10 Rule 1 forbids and
 * ``scripts/checks/check_no_goto_setjmp.py`` rejects outright -- that gate has
 * no allowlist, by design.
 *
 * Three ways out were considered and two were rejected:
 *   - Keeping the ``goto`` and waiving the gate. That is the allowlist this
 *     project deliberately does not have.
 *   - Substituting a plain ``return`` for the jump. This one looks tempting
 *     and is simply wrong: of the two call sites at the pinned commit, the one
 *     in ``host/drivers/serial/serial_drv.c`` jumps to a ``free_bufs`` label
 *     that releases two buffers before returning. A bare ``return`` there
 *     leaks both. The two sites also return different types, so no single
 *     substitution could serve both.
 *   - Leaving the macro out, which is what this port does.
 *
 * The consequence is bounded and stated in ``cmake/esp_hosted.cmake``:
 * ``serial_drv.c`` and ``serial_if.c`` are the only vendored translation units
 * that expand this macro, and neither is compiled. Nothing else needs it --
 * their consumers are in the RPC layer, which does not compile in this build
 * for an unrelated reason (it wants ESP-IDF's Wi-Fi API). When the RPC layer
 * is brought in, those two files need a jump-free allocate-or-bail path, and
 * that is a change to make there rather than a rule to bend here.
 */

/**
 * @def MEM_DUMP
 * @brief Report heap occupancy at a named point.
 * @details Routed to the port's allocator statistics, which are real
 * numbers here: the transport pool is a fixed ThreadX byte pool, so
 * "largest free block" and "bytes available" are both meaningful and are
 * exactly what a fragmentation problem shows up in.
 * @param[in] s Short label naming the call site.
 * @note Read-only build configuration.
 * @warning Emits at info level; noisy if placed on a per-packet path.
 * @par Example:
 * @code
 * MEM_DUMP("spi_mempool_create");
 * @endcode
 * @since 0.1.0
 */
#define MEM_DUMP(s) ra8_esp_hosted_mem_dump(s)

/* ----------------------------------------------------------------------- */
/* Thread budgets */
/*                                                                          */
/* ThreadX priorities run 0 (highest) to TX_MAX_PRIORITIES-1 (lowest), the */
/* opposite sense to FreeRTOS. The values below are already in ThreadX */
/* order, so the port passes them through unchanged. */
/* ----------------------------------------------------------------------- */

/**
 * @def DFLT_TASK_PRIO
 * @brief ThreadX priority for the SPI transaction and receive threads.
 * @details Twelve, comfortably above application work and below the
 * kernel timer thread, so a co-processor edge is serviced promptly without
 * starving anything that owns the display or storage.
 * @note Read-only build configuration.
 * @warning Zero is the *highest* ThreadX priority; do not copy a FreeRTOS
 *          value here.
 * @par Example:
 * @code
 * g_h.funcs->_h_thread_create("spi_trans", DFLT_TASK_PRIO, ...);
 * @endcode
 * @since 0.1.0
 */
#define DFLT_TASK_PRIO (12)

/**
 * @def DFLT_TASK_STACK_SIZE
 * @brief Stack, in bytes, for each esp-hosted transport thread.
 * @details Four kibibytes. The deepest path is the receive dispatcher
 * running the payload-header decode and the checksum sweep over a
 * 1600-byte frame, all of which works out of the caller's buffers rather
 * than the stack.
 * @note Read-only build configuration.
 * @warning Must be at least ``TX_MINIMUM_STACK`` (512) or thread creation
 *          fails.
 * @par Example:
 * @code
 * g_h.funcs->_h_thread_create("spi_rx", DFLT_TASK_PRIO, DFLT_TASK_STACK_SIZE, fn, NULL);
 * @endcode
 * @since 0.1.0
 */
#define DFLT_TASK_STACK_SIZE (4096)

/**
 * @def RPC_TASK_PRIO
 * @brief ThreadX priority for the RPC request/response thread.
 * @details Fourteen -- one notch below the transport threads, because RPC
 * work is only meaningful once the transport has already delivered a
 * frame.
 * @note Read-only build configuration.
 * @warning Raising it above ::DFLT_TASK_PRIO can starve the transport.
 * @par Example:
 * @code
 * g_h.funcs->_h_thread_create("rpc", RPC_TASK_PRIO, RPC_TASK_STACK_SIZE, fn, NULL);
 * @endcode
 * @since 0.1.0
 */
#define RPC_TASK_PRIO (14)

/**
 * @def RPC_TASK_STACK_SIZE
 * @brief Stack, in bytes, for the RPC thread.
 * @details Five kibibytes: the protobuf-c decoder recurses through nested
 * messages, so this thread is deeper than the transport ones.
 * @note Read-only build configuration.
 * @warning Must be at least ``TX_MINIMUM_STACK`` (512).
 * @par Example:
 * @code
 * g_h.funcs->_h_thread_create("rpc", RPC_TASK_PRIO, RPC_TASK_STACK_SIZE, fn, NULL);
 * @endcode
 * @since 0.1.0
 */
#define RPC_TASK_STACK_SIZE (5120)

/* ----------------------------------------------------------------------- */
/* Timers */
/* ----------------------------------------------------------------------- */

/**
 * @def H_TIMER_TYPE_ONESHOT
 * @brief Timer that fires once and then stops.
 * @details Selector passed to ``_h_timer_start``.
 * @note Read-only build configuration.
 * @warning The handle stays valid after expiry; stop it to release it.
 * @par Example:
 * @code
 * g_h.funcs->_h_timer_start("rpc_tmo", 5000, H_TIMER_TYPE_ONESHOT, cb, arg);
 * @endcode
 * @since 0.1.0
 */
#define H_TIMER_TYPE_ONESHOT (1)

/**
 * @def H_TIMER_TYPE_PERIODIC
 * @brief Timer that reschedules itself after every expiry.
 * @details Selector passed to ``_h_timer_start``.
 * @note Read-only build configuration.
 * @warning Runs until explicitly stopped with ``_h_timer_stop``.
 * @par Example:
 * @code
 * g_h.funcs->_h_timer_start("hb", 1000, H_TIMER_TYPE_PERIODIC, cb, arg);
 * @endcode
 * @since 0.1.0
 */
#define H_TIMER_TYPE_PERIODIC (2)

/* ----------------------------------------------------------------------- */
/* GPIO modes and pulls */
/* ----------------------------------------------------------------------- */

/**
 * @def H_GPIO_MODE_DEF_INPUT
 * @brief Configure a side-band pin as a digital input.
 * @details Passed as the ``mode`` argument of ``_h_config_gpio``.
 * @note Read-only build configuration.
 * @warning The RA8D2 pin must not already be claimed by a peripheral.
 * @par Example:
 * @code
 * g_h.funcs->_h_config_gpio(port, pin, H_GPIO_MODE_DEF_INPUT);
 * @endcode
 * @since 0.1.0
 */
#define H_GPIO_MODE_DEF_INPUT (0)

/**
 * @def H_GPIO_MODE_DEF_OUTPUT
 * @brief Configure a side-band pin as a push-pull digital output.
 * @details Passed as the ``mode`` argument of ``_h_config_gpio``; the
 * co-processor reset line is the only pin the host drives this way.
 * @note Read-only build configuration.
 * @warning Never apply this to HANDSHAKE or DATA_READY -- the C6 drives
 *          both push-pull and the pins would contend.
 * @par Example:
 * @code
 * g_h.funcs->_h_config_gpio(port, pin, H_GPIO_MODE_DEF_OUTPUT);
 * @endcode
 * @since 0.1.0
 */
#define H_GPIO_MODE_DEF_OUTPUT (1)

/**
 * @def H_GPIO_PULL_UP
 * @brief Select the internal pull-up when configuring a pin.
 * @details Passed as ``pull_value`` to ``_h_pull_gpio``.
 * @note Read-only build configuration.
 * @warning Only meaningful on an input.
 * @par Example:
 * @code
 * g_h.funcs->_h_pull_gpio(port, pin, H_GPIO_PULL_UP, H_ENABLE);
 * @endcode
 * @since 0.1.0
 */
#define H_GPIO_PULL_UP (0)

/**
 * @def H_GPIO_PULL_DOWN
 * @brief Select the internal pull-down when configuring a pin.
 * @details Passed as ``pull_value`` to ``_h_pull_gpio``. The RA8D2 PFS
 * carries a pull-up bit only, so the port accepts this selector, reports
 * it honestly through its return code and does not pretend a pull-down was
 * installed.
 * @note Read-only build configuration.
 * @warning The RA8D2 has no internal pull-down; fit an external resistor.
 * @par Example:
 * @code
 * g_h.funcs->_h_pull_gpio(port, pin, H_GPIO_PULL_DOWN, H_ENABLE);
 * @endcode
 * @since 0.1.0
 */
#define H_GPIO_PULL_DOWN (1)

/* ----------------------------------------------------------------------- */
/* Payload bound */
/* ----------------------------------------------------------------------- */

/**
 * @def MAX_PAYLOAD_SIZE
 * @brief Largest payload one transport frame can carry, in bytes.
 * @details The frame size less the twelve-byte esp-hosted payload header,
 * so 1588 for the SPI transport. The receive path rejects any header
 * claiming more than this, which is the first bound applied to
 * co-processor-supplied data.
 * @note Read-only build configuration.
 * @warning Derived from ::MAX_TRANSPORT_BUFFER_SIZE; the transport-
 *          specific port header must be included first.
 * @par Example:
 * @code
 * if (len > MAX_PAYLOAD_SIZE) { drop_frame(); }
 * @endcode
 * @since 0.1.0
 */
#define MAX_PAYLOAD_SIZE (MAX_TRANSPORT_BUFFER_SIZE - H_ESP_PAYLOAD_HEADER_OFFSET)

/* ----------------------------------------------------------------------- */
/* Placement and linkage attributes */
/* ----------------------------------------------------------------------- */

/**
 * @def FAST_RAM_ATTR
 * @brief Place a function in tightly-coupled RAM for interrupt latency.
 * @details Empty on this target. The RA8D2 executes from 1 MB of MRAM
 * behind the Cortex-M85 caches; there is no separate low-latency
 * instruction RAM to relocate into, so an attribute here would name a
 * section the linker script does not define. The two functions upstream
 * marks with it are the side-band edge handlers, which the port already
 * services through the ICU with a dedicated NVIC priority.
 * @note Read-only build configuration.
 * @warning Do not point this at a section that no linker script places.
 * @par Example:
 * @code
 * static void FAST_RAM_ATTR gpio_dr_isr_handler(void *arg) { ... }
 * @endcode
 * @since 0.1.0
 */
#define FAST_RAM_ATTR

/**
 * @def H_IRAM_ATTR
 * @brief Upstream alias of ::FAST_RAM_ATTR.
 * @details Empty for the same reason: the RA8D2 has no instruction RAM
 * distinct from the MRAM the image already executes from.
 * @note Read-only build configuration.
 * @warning See ::FAST_RAM_ATTR.
 * @par Example:
 * @code
 * H_IRAM_ATTR void handler(void) { ... }
 * @endcode
 * @since 0.1.0
 */
#define H_IRAM_ATTR

/**
 * @def H_WEAK_REF
 * @brief Mark a definition as overridable at link time.
 * @details The vendored Bluetooth drivers define ``hci_rx_handler`` weakly
 * so an application can replace it. Spelled in the C23 attribute form the
 * project mandates, which GCC accepts on both the ARM cross build and the
 * host test build.
 * @note Read-only build configuration.
 * @warning A weak symbol that nothing overrides still links; check that
 *          the intended strong definition is actually in the image.
 * @par Example:
 * @code
 * H_WEAK_REF int hci_rx_handler(uint8_t *buf, size_t len) { return 0; }
 * @endcode
 * @since 0.1.0
 */
#define H_WEAK_REF [[gnu::weak]]
