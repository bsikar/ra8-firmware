/**
 * @file port/esp-hosted/inc/ra8_esp_hosted_port.h
 * @brief Public entry point of the RA8D2 + ThreadX port of esp-hosted.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vendored esp-hosted host driver reaches the hardware through exactly
 * two seams: the ten ``port_esp_hosted_host_*.h`` headers, and the 72-entry
 * ``hosted_osi_funcs_t`` vtable it finds behind the global ``g_h``. This
 * header is the first-party side of that arrangement -- it brings the port
 * up, and it declares the handful of port entry points the port headers
 * expand macros onto.
 *
 * @par Bring-up order
 * ::ra8_esp_hosted_port_init must run before any vendored esp-hosted call,
 * and after the clock, MSTP and ThreadX kernel are up. It:
 *   1. validates the configuration and the pin map;
 *   2. carves the transport byte pool out of a static array;
 *   3. routes the SCI Simple-SPI pins and opens the bus at the requested
 *      mode and bit rate;
 *   4. takes the chip select as a GPIO output, idle high;
 *   5. configures HANDSHAKE and DATA_READY as inputs, attaching each to
 *      either an ICU channel or the software edge detector according to
 *      ``ra8_esp_hosted_pin_irq_num``;
 *   6. publishes ``g_h`` so the vendored core can be started.
 *
 * @par Runtime status -- proven on silicon 2026-07-28
 * This port runs on the bench. ``ra8_esp_hosted_port_init`` returns
 * ``k_ra8_ok``, the pin map and interrupt routing resolve to the nets the
 * board layer names, and ``_h_do_bus_transfer`` clocks 1600-byte full-duplex
 * transactions at **5 MHz** -- five times the rate the original probe
 * qualified -- with zero bad checksums and zero handshake timeouts.
 * ``examples/ek_ra8d2/hw_validated/c6/c6_hosted_init`` established the
 * transaction, and ``c6_fw_version`` then completed a full esp-hosted RPC
 * round-trip through this vtable and checked the co-processor's answer.
 * ``just hil::c6`` re-runs all of it.
 *
 * The physical map came from ``c6_spi_probe``, which scope-qualified every J26
 * hole on 2026-07-27 at SPI mode 3 / 1 MHz; that probe drives the SCI directly
 * and reaches none of this code, which is why the port needed its own proof.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_esp_hosted_limits_t
 * @brief Fixed budgets the port allocates out of static storage.
 *
 * @details
 * NASA Power of 10 Rule 3 forbids dynamic allocation after
 * initialisation, and this board has no heap at all -- ``_sbrk`` is a
 * strong symbol that reports a fatal error. Every object the vendored core
 * asks the vtable to "allocate" therefore comes out of a fixed pool sized
 * by these bounds, and an over-request fails cleanly rather than growing
 * the address space.
 *
 * @invariant The queue, semaphore, mutex and thread counts each cover the
 *            worst case the SPI transport reaches: three priority queues
 *            in each direction, three semaphores, one bus mutex, one
 *            mempool lock and two transport threads, with headroom for the
 *            serial and Bluetooth channels layered above.
 * @invariant ::k_ra8_esp_hosted_pool_bytes is a multiple of
 *            ``HOSTED_MEM_ALIGNMENT_64``.
 *
 * @par Example:
 * @code
 * static_assert(k_ra8_esp_hosted_max_queues >= 6, "two directions x 3 priorities");
 * @endcode
 *
 * @see ra8_esp_hosted_port_init
 * @since 0.1.0
 */
typedef enum : uint32_t {
  /** Message queues the port can hand out before refusing. */
  k_ra8_esp_hosted_max_queues = 8U,
  /** Counting semaphores the port can hand out before refusing. */
  k_ra8_esp_hosted_max_semaphores = 8U,
  /** Mutexes the port can hand out, covering the bus lock and the
      mempool lock with headroom for the RPC layer. */
  k_ra8_esp_hosted_max_mutexes = 4U,
  /** Threads the port can start: the SPI transaction pump, the receive
      dispatcher, and headroom for the RPC worker. */
  k_ra8_esp_hosted_max_threads = 4U,
  /** Software timers the port can arm concurrently. */
  k_ra8_esp_hosted_max_timers = 4U,
  /** Bytes reserved for queue message storage, shared across all
      queues; one esp-hosted buffer handle is 28 bytes on this ABI and
      the transport wants twenty per queue. */
  k_ra8_esp_hosted_queue_pool_bytes = 8192U,
  /** Bytes reserved for the transport buffer pool: enough for the
      receive ring, a transmit frame, and the mempool bookkeeping, all
      at 1600 bytes per block. */
  k_ra8_esp_hosted_pool_bytes = 65536U,
} ra8_esp_hosted_limits_t;

/**
 * @struct ra8_esp_hosted_port_cfg
 * @brief Everything the port needs that is not fixed by the board.
 *
 * @details
 * The pin assignment is deliberately absent: it is a board fact and lives
 * once in ``ra8_esp_hosted_pins.h``. What remains is the clocking, which
 * an application knows and the port cannot discover, plus the poll period
 * for any side-band pin the package cannot route to the ICU.
 *
 * @invariant ``pclk_hz`` is the live PCLKA rate; a stale value silently
 *            mis-programs the SCI bit-rate divider.
 * @invariant ``sck_hz`` is non-zero and within what the divider can reach
 *            from ``pclk_hz``.
 * @invariant ``edge_poll_ms`` is non-zero, or the software edge detector
 *            would arm a zero-tick timer, which ThreadX rejects.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_port_cfg_t cfg = {};
 * (void)ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &cfg.pclk_hz);
 * cfg.sck_hz       = 5000000U;
 * cfg.edge_poll_ms = 2U;
 * (void)ra8_esp_hosted_port_init(&cfg);
 * @endcode
 *
 * @see ra8_esp_hosted_port_init
 * @since 0.1.0
 */
typedef struct ra8_esp_hosted_port_cfg {
  uint32_t pclk_hz;      /**< PCLKA rate in hertz; the SCI baud clock source. */
  uint32_t sck_hz;       /**< Requested SPI bit rate in hertz.                */
  uint16_t edge_poll_ms; /**< Sampling period for pins with no ICU channel.   */
  uint8_t  sci_channel;  /**< SCI channel carrying Simple-SPI; 0..9.          */
} ra8_esp_hosted_port_cfg_t;

/**
 * @typedef ra8_esp_hosted_event_cb_t
 * @brief Application handler for an event the vendored core posts.
 *
 * @details
 * ESP-IDF hosts deliver esp-hosted events through the IDF event loop.
 * There is no such loop here, and adding a thread and a queue to
 * re-create one would cost more than it buys: the port instead calls this
 * handler synchronously, on the thread that posted, so the application
 * sees exactly one delivery path.
 *
 * @param[in] ctx The opaque context registered alongside the handler.
 * @param[in] base Event namespace as a NUL-terminated string; never null.
 * @param[in] event_id Identifier within that namespace.
 * @param[in] data Payload, or null when ``data_len`` is zero. Owned by the
 *                 caller and invalid once the handler returns.
 * @param[in] data_len Payload length in bytes.
 *
 * @return Nothing.
 *
 * @note Runs on the posting thread -- often the SPI receive dispatcher --
 *       so it must not block and must not call back into the transport.
 * @since 0.1.0
 */
typedef void (*ra8_esp_hosted_event_cb_t)(void*       ctx,
                                          const char* base,
                                          int32_t     event_id,
                                          const void* data,
                                          size_t      data_len);

/**
 * @brief Register the handler the port calls when the core posts an event.
 *
 * @details
 * Registering a null handler removes the current one, after which posted
 * events are reported to the core as unconsumed rather than dropped
 * quietly -- the core can then decide whether the loss matters.
 *
 * @param[in] cb Handler to install, or null to remove the current one.
 * @param[in] ctx Opaque context handed back to the handler. Must be null
 *                when ``cb`` is null, since there would be nothing to hand
 *                it to.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The registration was stored.
 * @retval k_ra8_err_invalid_arg A context was supplied without a handler.
 *
 * @pre The caller owns ``ctx`` and keeps it alive past any deregistration.
 * @pre No event is being dispatched concurrently on another thread.
 * @post Subsequent posts reach ``cb``, or are reported unconsumed when it
 *       is null.
 * @post No other port state is modified.
 *
 * @note Not thread-safe against a concurrent post; register during
 *       bring-up, before the transport is started.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_port_set_event_cb(on_link_event, &app_state);
 * @endcode
 *
 * @see ra8_esp_hosted_event_cb_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_esp_hosted_port_set_event_cb(ra8_esp_hosted_event_cb_t cb, void* ctx);

/**
 * @brief Bring the esp-hosted port up and publish the OS-abstraction vtable.
 *
 * @details
 * Performs the six bring-up steps listed in the file-level documentation.
 * Every step is checked; the first failure unwinds the steps already taken
 * -- the SCI channel is closed, claimed pins are released and attached
 * interrupts are detached -- so a failed init leaves no pin stranded and a
 * retry after fixing the cause can succeed.
 *
 * The pool carve is the only allocation the port ever performs, and it
 * happens exactly here, during initialisation, which is what keeps the
 * whole port inside NASA Power of 10 Rule 3.
 *
 * @param[in] cfg Clocking and pacing configuration. Must be non-null;
 *                ``pclk_hz`` and ``sck_hz`` must be non-zero,
 *                ``edge_poll_ms`` must be non-zero, and ``sci_channel``
 *                must be below the SCI channel count.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Port is up; ``g_h`` may be used.
 * @retval k_ra8_err_null_ptr ``cfg`` was null.
 * @retval k_ra8_err_invalid_arg A configuration field was out of range.
 * @retval k_ra8_err_invalid_state The port was already initialised.
 * @retval k_ra8_err_gpio_conflict A link pin is owned by another module.
 * @retval k_ra8_err_rtos_error ThreadX refused a pool, mutex or timer.
 * @retval k_ra8_err_spi_error The SCI Simple-SPI channel would not open.
 *
 * @pre The CGC is configured and ``pclk_hz`` reflects the live PCLKA rate.
 * @pre The ThreadX kernel is running, or this is called from
 *      ``tx_application_define``.
 * @post On success the port reports ready and ``g_h.funcs`` is populated.
 * @post On failure no pin is left claimed and no interrupt is left
 *       attached by this call.
 *
 * @note Not thread-safe; call once from a single-threaded bring-up path.
 * @warning Calling any vendored esp-hosted entry point before this
 *          dereferences an unpopulated vtable.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_port_cfg_t cfg = { .pclk_hz = pclka, .sck_hz = 5000000U,
 *                                   .edge_poll_ms = 2U, .sci_channel = 2U };
 * if (ra8_esp_hosted_port_init(&cfg) != k_ra8_ok) { report(); }
 * @endcode
 *
 * @see ra8_esp_hosted_port_deinit
 * @see ra8_esp_hosted_port_is_ready
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 3: the only allocation is this init-time pool carve.
 * - Rule 5: four preconditions and two postconditions are checked.
 */
[[nodiscard]] ra8_err_t ra8_esp_hosted_port_init(const ra8_esp_hosted_port_cfg_t* cfg);

/**
 * @brief Tear the port down, releasing every pin, interrupt and pool.
 *
 * @details
 * Reverses ::ra8_esp_hosted_port_init in the opposite order: detaches the
 * side-band interrupts, stops the software edge detector, closes the SCI
 * channel, releases the chip select and the two side-band pins, deletes
 * the pools and marks the port not ready. Objects the vendored core still
 * holds handles to are deleted, so the core must be torn down first.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Everything was released.
 * @retval k_ra8_err_not_initialized The port was not up.
 * @retval k_ra8_err_rtos_error ThreadX refused to delete an object,
 *         typically because a thread is still blocked on it.
 *
 * @pre The vendored transport has been stopped.
 * @pre No interrupt handler is currently executing against this port.
 * @post Every pin the port claimed is released.
 * @post The port reports not ready and ``g_h.funcs`` must not be used.
 *
 * @note Not thread-safe; call from the same context as the init.
 * @warning Tearing down while the SPI transaction thread runs leaves it
 *          blocked on a deleted semaphore.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_port_deinit();
 * @endcode
 *
 * @see ra8_esp_hosted_port_init
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: one precondition and two postconditions are checked.
 */
[[nodiscard]] ra8_err_t ra8_esp_hosted_port_deinit(void);

/**
 * @brief Report whether the port is currently initialised.
 *
 * @details
 * Reads the single module-state flag. Exists so an application can decide
 * whether a teardown is needed without provoking an error return, and so
 * host tests can assert the state machine without reaching into the
 * module.
 *
 * @return true when ::ra8_esp_hosted_port_init has completed successfully
 *         and no teardown has run since.
 * @retval true The port is up.
 * @retval false The port has never been up, failed to come up, or has been
 *         torn down.
 *
 * @pre None; safe to call at any time, including before any init.
 * @pre The caller tolerates a value that a concurrent teardown may stale.
 * @post No module state is modified.
 * @post The returned value reflects the flag at the moment of the read.
 *
 * @note Safe from interrupt context; a single aligned load.
 *
 * @par Example:
 * @code
 * if (ra8_esp_hosted_port_is_ready()) { (void)ra8_esp_hosted_port_deinit(); }
 * @endcode
 *
 * @see ra8_esp_hosted_port_init
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_esp_hosted_port_is_ready(void);

/**
 * @brief Report whether the co-processor has queued receive data.
 * @details Samples the held DATA_READY side-band signal through the bound
 *          esp-hosted GPIO seam. This is a non-blocking level check for poll
 *          schedulers; it does not clock SPI or consume the pending frame.
 *
 * @return Whether one or more receive frames are pending.
 * @retval true DATA_READY is asserted on an initialized port.
 * @retval false The port is not initialized or DATA_READY is inactive.
 *
 * @pre The caller does not concurrently tear the port down.
 * @pre DATA_READY retains the board-qualified active polarity.
 * @post No GPIO, SPI, or queue state is modified.
 * @post A true result remains advisory until the caller acquires its wire lock.
 *
 * @note Reentrant while the port remains initialized.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_esp_hosted_port_rx_pending(void);

/**
 * @brief Report transport pool occupancy at a named point.
 *
 * @details
 * Backs the ``MEM_DUMP`` macro the vendored core sprinkles through its
 * allocation paths. Reads the live ThreadX byte-pool statistics -- bytes
 * available and fragment count -- and emits them at info level under the
 * port's log tag. Those are real numbers on this port because the pool is
 * a fixed carve, so a shrinking largest-fragment is exactly how a
 * fragmentation problem announces itself.
 *
 * @param[in] label Short call-site name, used verbatim in the log line.
 *                  A null pointer is replaced with a fixed placeholder
 *                  rather than dereferenced.
 *
 * @return Nothing.
 *
 * @pre The port is initialised, or the call reports the pool as absent.
 * @pre Logging has been initialised, or the line is dropped.
 * @post No pool state is modified.
 * @post Exactly one log line is emitted per call.
 *
 * @note Not for per-packet paths; it logs unconditionally at info level.
 *
 * @par Example:
 * @code
 * MEM_DUMP("spi_mempool_create");
 * @endcode
 *
 * @see ra8_esp_hosted_port_init
 * @since 0.1.0
 */
void ra8_esp_hosted_mem_dump(const char* label);

/**
 * @brief Record that an allocation through the vtable could not be served.
 *
 * @details
 * Backs the failure arm of the ``HOSTED_CALLOC`` macro. On a fixed-pool
 * port an allocation failure is a budgeting fact rather than a transient
 * one, so the report names the requesting function and the size it wanted
 * -- which together are enough to resize
 * ::k_ra8_esp_hosted_pool_bytes without guesswork.
 *
 * @param[in] func Name of the requesting function, normally ``__func__``.
 *                 A null pointer is replaced with a fixed placeholder.
 * @param[in] bytes Size that could not be served, in bytes.
 *
 * @return Nothing.
 *
 * @pre Logging has been initialised, or the line is dropped.
 * @pre The caller has already decided to abandon the operation.
 * @post No pool state is modified.
 * @post Exactly one log line is emitted per call.
 *
 * @note Safe from any thread; it only formats and logs.
 *
 * @par Example:
 * @code
 * HOSTED_CALLOC(uint8_t, write_buf, buf_len, free_bufs2);
 * @endcode
 *
 * @see ra8_esp_hosted_mem_dump
 * @since 0.1.0
 */
void ra8_esp_hosted_alloc_failed(const char* func, size_t bytes);
