/**
 * @file port/esp-hosted/src/ra8_esp_hosted_gpio_internal.h
 * @brief Module-private surface of the esp-hosted side-band GPIO slice.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vendored esp-hosted core reaches every side-band pin through eight
 * slots of ``hosted_osi_funcs_t``. ``ra8_esp_hosted_gpio.c`` fills those
 * slots; ``ra8_esp_hosted_gpio_edge.c`` owns the software edge detector the
 * slots fall back on. This header is the seam between those two translation
 * units and the tests, and nothing outside ``port/esp-hosted/`` may include
 * it.
 *
 * @par Why a software edge detector exists at all
 * On this package the ICU external-interrupt inputs are concentrated on port
 * 0, so of the Pmod1 side-band nets only one (P006 -> IRQ11) can raise a
 * hardware edge. ``ra8_esp_hosted_pin_irq_num`` reports that per pin, and
 * ``_h_config_gpio_as_interrupt`` picks the path accordingly: an ICU channel
 * when one exists, otherwise a row in the polled table below. Both paths
 * deliver the same callback with the same argument, so the vendored driver
 * cannot tell them apart -- only the latency differs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "esp_hosted_os_abstraction.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_pin_interface.h"
#include "ra8_port_constants.h"

/**
 * @enum ra8_esp_hosted_gpio_limits_t
 * @brief Fixed bounds of the side-band GPIO slice.
 *
 * @details
 * The link uses four side-band nets at most -- chip select, HANDSHAKE,
 * DATA_READY and the co-processor reset -- and only the ones configured as
 * interrupts take a table row, so four rows can never be exhausted by the
 * vendored driver. The bound is stated anyway because a registration that
 * cannot be recorded must fail loudly rather than be silently dropped.
 *
 * @invariant ::k_ra8_esp_hosted_gpio_row_max is the exact row count of the
 *            edge table; ``priv_ra8_esp_hosted_gpio_edge_count`` never exceeds it.
 * @invariant ::k_ra8_esp_hosted_gpio_poll_ms_default is non-zero, so the
 *            detector can always arm a legal ThreadX timer.
 *
 * @par Example:
 * @code
 * static_assert(k_ra8_esp_hosted_gpio_row_max >= 2U, "handshake + data ready");
 * @endcode
 *
 * @see priv_ra8_esp_hosted_gpio_edge_register
 * @since 0.1.0
 */
typedef enum : uint8_t {
  /** Rows in the polled edge table; also the registration ceiling. */
  k_ra8_esp_hosted_gpio_row_max = 4U,
  /** Sampling period used until ::priv_ra8_esp_hosted_gpio_set_edge_poll_ms runs. */
  k_ra8_esp_hosted_gpio_poll_ms_default = 2U,
  /** NVIC priority given to a hardware side-band edge. */
  k_ra8_esp_hosted_gpio_irq_priority = 6U,
} ra8_esp_hosted_gpio_limits_t;

/**
 * @brief Populate the eight GPIO slots of the OS-abstraction vtable.
 *
 * @details
 * Writes ``_h_config_gpio``, ``_h_config_gpio_as_interrupt``,
 * ``_h_teardown_gpio_interrupt``, ``_h_read_gpio``, ``_h_write_gpio``,
 * ``_h_pull_gpio``, ``_h_hold_gpio`` and
 * ``_h_get_host_wakeup_or_reboot_reason`` into @p out. No other slot is
 * touched, so the RTOS and transport slices may fill theirs before or after
 * this call in any order.
 *
 * @param[out] out Vtable to populate; must be non-null. Only the eight GPIO
 *                 rows are written.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The eight rows are populated.
 * @retval k_ra8_err_null_ptr @p out was null.
 *
 * @pre @p out points at storage that out-lives the vendored core.
 * @pre The pin map in ``ra8_esp_hosted_pins.h`` describes the live harness.
 * @post The eight GPIO rows of @p out are non-null.
 * @post No non-GPIO row of @p out is modified.
 *
 * @note Not thread-safe; call once from the port's bring-up path.
 * @warning Binding does not claim a pin. Pins are claimed lazily by the
 *          slots, which is what lets a failed bring-up leave none stranded.
 *
 * @par Example:
 * @code
 * (void)priv_ra8_esp_hosted_gpio_bind(&g_hosted_osi_funcs);
 * @endcode
 *
 * @see priv_ra8_esp_hosted_spi_bind
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t priv_ra8_esp_hosted_gpio_bind(hosted_osi_funcs_t* out);

/**
 * @brief Decode the vendored ``(void* port, uint32_t pin)`` pair into a pin.
 *
 * @details
 * Exact inverse of ``RA8_ESP_HOSTED_GPIO_PORT`` / ``RA8_ESP_HOSTED_GPIO_PIN``
 * in ``port_esp_hosted_host_config.h``: the port half is an index carried
 * inside a pointer, so port 0 arrives as a null pointer and must NOT be
 * null-checked -- the decoded index is range-checked instead. The pin half
 * arrives as ``uint32_t``, so the vendored ``-1`` "not wired" spelling
 * arrives as ``0xFFFFFFFF``; that value is rejected explicitly rather than
 * only by the range test, because it is a distinct fact about the harness.
 *
 * @param[in] gpio_port Port index in pointer clothing. Never dereferenced.
 * @param[in] gpio_num Pin index within the port, or ``0xFFFFFFFF`` for a
 *                     signal this harness does not wire.
 * @param[out] out_pin Receives the packed ``RA8_PIN(port, pin)`` value.
 *                     Untouched on any rejection.
 *
 * @return Whether the pair named a legal RA8 pin.
 * @retval true ``*out_pin`` holds the packed pin.
 * @retval false @p out_pin was null, the signal is unwired, or an index was
 *         out of range.
 *
 * @pre @p out_pin is writable when non-null.
 * @pre @p gpio_port was produced by ``RA8_ESP_HOSTED_GPIO_PORT``.
 * @post On false no output is written.
 * @post On true the decoded port is <= ``k_ra8_port_max`` and the decoded
 *       pin is <= ``k_ra8_pin_max``.
 *
 * @note Pure function; safe from interrupt context.
 *
 * @par MC/DC:
 * Decision: `(gpio_num == unwired) || (port_idx > port_max) || (gpio_num >
 * pin_max)` (3 conditions). Vectors: (F,F,F) accepts; (T,F,F), (F,T,F) and
 * (F,F,T) each reject. Pairing the accepting vector with each rejecting one
 * proves that condition's independent influence: N+1 = 4 vectors.
 *
 * @par Example:
 * @code
 * ra8_port_pin_t pin = k_ra8_pin_none;
 * if (priv_ra8_esp_hosted_gpio_decode_pin(H_GPIO_DATA_READY_Port,
 *                                    (uint32_t)H_GPIO_DATA_READY_Pin, &pin)) {
 *   (void)ra8_gpio_read(pin, &level);
 * }
 * @endcode
 *
 * @see priv_ra8_esp_hosted_gpio_bind
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] bool priv_ra8_esp_hosted_gpio_decode_pin(const void*     gpio_port,
                                                                uint32_t        gpio_num,
                                                                ra8_port_pin_t* out_pin);

/**
 * @brief Replace the pin driver the slice reads and writes levels through.
 *
 * @details
 * Dependency-injection seam. Production leaves it at
 * ``g_ra8_gpio_pin_interface``; host tests point it at a recorder so pin
 * levels can be driven without hardware. The interface covers
 * ``output_init``, ``write``, ``read`` and ``toggle`` only, so input
 * configuration, pin release and interrupt attachment still call the HAL
 * directly -- those have no row in ``ra8_pin_interface_t``.
 *
 * @param[in] iface Replacement interface, or null to restore the production
 *                  instance. Must out-live every later slot call.
 *
 *
 * @pre @p iface, when non-null, has non-null ``write`` and ``read`` rows.
 * @pre No slot call is in flight on another thread.
 * @post Later reads and writes go through @p iface.
 * @post Passing null restores the production pin driver.
 *
 * @note Not thread-safe; intended for bring-up and for tests.
 * @warning Swapping the interface does not re-configure any pin; the rows
 *          the previous interface configured stay as they were.
 *
 * @par Example:
 * @code
 * priv_ra8_esp_hosted_gpio_set_pin_interface(&mock_pin_iface);
 * @endcode
 *
 * @see priv_ra8_esp_hosted_gpio_pin_interface
 * @since 0.1.0
 */
RA8_PRIV void priv_ra8_esp_hosted_gpio_set_pin_interface(const ra8_pin_interface_t* iface);

/**
 * @brief Report the pin driver currently installed in the slice.
 *
 * @details
 * Exists so the edge detector, which lives in its own translation unit, can
 * sample levels through exactly the interface the slots write through. It
 * never returns null: an unset seam reads back as the production instance.
 *
 * @return The installed pin interface.
 * @retval non-null Always; the production instance when nothing was injected.
 *
 * @pre The slice has been linked against ``libs/ra8_hal``.
 * @pre The caller does not retain the pointer across a seam swap.
 * @post No module state is modified.
 * @post The returned interface has non-null ``read`` and ``write`` rows.
 *
 * @note Safe from interrupt context; a single aligned load.
 *
 * @par Example:
 * @code
 * const ra8_pin_interface_t* pin_if = priv_ra8_esp_hosted_gpio_pin_interface();
 * @endcode
 *
 * @see priv_ra8_esp_hosted_gpio_set_pin_interface
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] const ra8_pin_interface_t* priv_ra8_esp_hosted_gpio_pin_interface(void);

/**
 * @brief Set the sampling period the software edge detector runs at.
 *
 * @details
 * Applies to the single periodic ThreadX timer shared by every polled row.
 * Changing it while rows are registered re-arms the timer, so the new period
 * takes effect from the next expiry. The port passes
 * ``ra8_esp_hosted_port_cfg_t::edge_poll_ms`` here during bring-up.
 *
 * @param[in] period_ms Sampling period in milliseconds; must be non-zero,
 *                      because ThreadX rejects a zero-tick timer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The period was accepted.
 * @retval k_ra8_err_invalid_arg @p period_ms was zero.
 * @retval k_ra8_err_rtos_error The timer could not be re-armed.
 *
 * @pre The ThreadX kernel is running when rows are already registered.
 * @pre @p period_ms is small enough that an edge cannot be missed; the C6
 *      holds DATA_READY asserted until the frame is taken, so any period
 *      shorter than a transaction is safe.
 * @post The detector's period reads back as @p period_ms.
 * @post No registered row is lost by the change.
 *
 * @note Not thread-safe; call from the port's bring-up path.
 *
 * @par Example:
 * @code
 * (void)priv_ra8_esp_hosted_gpio_set_edge_poll_ms(cfg->edge_poll_ms);
 * @endcode
 *
 * @see priv_ra8_esp_hosted_gpio_edge_poll_once
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t priv_ra8_esp_hosted_gpio_set_edge_poll_ms(uint16_t period_ms);

/**
 * @brief Take a pin under software edge detection.
 *
 * @details
 * Configures the pin as an input, records the level it starts at so the
 * first sample cannot report a phantom edge, and arms the shared periodic
 * timer if this is the first row. The row stores the vendored callback and
 * its argument verbatim, so a polled pin and an ICU-served pin deliver
 * identical calls.
 *
 * @param[in] pin Packed pin to watch; must be a legal RA8 pin.
 * @param[in] sense Edge selector using the ``ra8_icu_irqmd_t`` encoding:
 *                  0 falling, 1 rising, 2 both, 3 low level.
 * @param[in] handler Callback invoked on a detected edge; must be non-null
 *                    and must not block.
 * @param[in] arg Opaque argument handed back to @p handler.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The pin is being sampled.
 * @retval k_ra8_err_null_ptr @p handler was null.
 * @retval k_ra8_err_invalid_arg @p sense was outside 0..3.
 * @retval k_ra8_err_exists The pin already has a row.
 * @retval k_ra8_err_no_mem The table is full.
 * @retval k_ra8_err_rtos_error The periodic timer would not arm.
 * @retval k_ra8_err_gpio_conflict The pin is owned by another module.
 *
 * @pre The ThreadX kernel is running.
 * @pre @p handler tolerates being called from timer context.
 * @post On success ``priv_ra8_esp_hosted_gpio_edge_count`` has grown by one.
 * @post On any failure no row is added and no pin is left claimed by this
 *       call.
 *
 * @note Not thread-safe with respect to the timer callback.
 * @warning Detection latency is bounded by the sampling period, not by the
 *          pin: an edge shorter than one period can be missed entirely.
 *
 * @par Example:
 * @code
 * (void)priv_ra8_esp_hosted_gpio_edge_register(pin, 1U, gpio_hs_isr_handler, nullptr);
 * @endcode
 *
 * @see priv_ra8_esp_hosted_gpio_edge_unregister
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t priv_ra8_esp_hosted_gpio_edge_register(ra8_port_pin_t pin,
                                                                        uint8_t        sense,
                                                                        void (*handler)(void*),
                                                                        void* arg);

/**
 * @brief Drop a pin from software edge detection.
 *
 * @details
 * Frees the row, releases the pin claim and, when the last row goes, deletes
 * the shared periodic timer so a torn-down port leaves no kernel object
 * running.
 *
 * @param[in] pin Packed pin previously passed to
 *                ::priv_ra8_esp_hosted_gpio_edge_register.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The row was freed.
 * @retval k_ra8_err_not_found The pin had no row.
 * @retval k_ra8_err_rtos_error The timer would not stop.
 *
 * @pre The timer callback is not executing.
 * @pre The pin was registered by this module.
 * @post ``priv_ra8_esp_hosted_gpio_edge_count`` has fallen by one.
 * @post The pin is no longer claimed by this module.
 *
 * @note Not thread-safe with respect to the timer callback.
 *
 * @par Example:
 * @code
 * (void)priv_ra8_esp_hosted_gpio_edge_unregister(pin);
 * @endcode
 *
 * @see priv_ra8_esp_hosted_gpio_edge_register
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t priv_ra8_esp_hosted_gpio_edge_unregister(ra8_port_pin_t pin);

/**
 * @brief Report how many pins are under software edge detection.
 *
 * @details
 * Reads the table occupancy. Exists so the port and the tests can assert the
 * detector's state -- in particular that teardown emptied it -- without
 * reaching into the module's storage.
 *
 * @return Occupied rows, 0 .. ::k_ra8_esp_hosted_gpio_row_max.
 * @retval 0 No pin is polled; the periodic timer is not running.
 *
 * @pre None; safe to call before any registration.
 * @pre The caller tolerates a value a concurrent registration may stale.
 * @post No module state is modified.
 * @post The result never exceeds ::k_ra8_esp_hosted_gpio_row_max.
 *
 * @note Safe from interrupt context.
 *
 * @par Example:
 * @code
 * TEST_ASSERT_EQ(0U, priv_ra8_esp_hosted_gpio_edge_count());
 * @endcode
 *
 * @see priv_ra8_esp_hosted_gpio_edge_register
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] uint8_t priv_ra8_esp_hosted_gpio_edge_count(void);

/**
 * @brief Sample every polled row once and dispatch the edges seen.
 *
 * @details
 * The body of the periodic timer, exposed so tests drive it directly rather
 * than waiting on a kernel tick. For each occupied row it reads the pin
 * through the injected pin interface, asks ::priv_ra8_esp_hosted_gpio_edge_seen
 * whether the configured edge occurred, stores the new level and, when it
 * did, calls the row's handler. A read that fails leaves the stored level
 * untouched so a transient failure cannot manufacture an edge on the next
 * pass.
 *
 *
 * @pre A pin interface is installed (one always is).
 * @pre Row handlers do not block and do not re-enter this function.
 * @post Every occupied row's stored level matches its last good read.
 * @post One handler call was made per row that showed its configured edge.
 *
 * @note Runs from timer context in production; must not block.
 *
 * @par MC/DC:
 * Decision: `if (row->used && read_ok)` (2 conditions). Vectors: used=T
 * read_ok=T dispatches; used=F read_ok=T skips; used=T read_ok=F skips.
 * Vectors 1+2 prove ``used`` influences the outcome independently, 1+3 do
 * the same for ``read_ok``: N+1 = 3 vectors.
 *
 * @par Example:
 * @code
 * priv_ra8_esp_hosted_gpio_edge_poll_once();
 * @endcode
 *
 * @see priv_ra8_esp_hosted_gpio_edge_seen
 * @since 0.1.0
 */
RA8_PRIV void priv_ra8_esp_hosted_gpio_edge_poll_once(void);

/**
 * @brief Decide whether two consecutive samples show the configured edge.
 *
 * @details
 * The whole decision logic of the software edge detector, kept pure so it can
 * be driven exhaustively without hardware. Edge senses compare the two
 * samples; the low-level sense is not an edge at all and reports on the
 * current sample alone, which matches what the ICU does with
 * ``k_ra8_icu_irqmd_low``.
 *
 * @param[in] prev_level Level recorded by the previous sample: 0 or 1.
 * @param[in] now_level Level just read: 0 or 1.
 * @param[in] sense Selector using the ``ra8_icu_irqmd_t`` encoding: 0
 *                  falling, 1 rising, 2 both, 3 low level.
 *
 * @return Whether the configured event is present.
 * @retval true The handler should run.
 * @retval false No event, or @p sense was outside 0..3.
 *
 * @pre @p prev_level and @p now_level are 0 or 1.
 * @pre @p sense uses the ICU encoding, not the vendored polarity flags.
 * @post No state is modified.
 * @post An out-of-range sense reports no event rather than guessing one.
 *
 * @note Pure function; safe from interrupt context.
 *
 * @par MC/DC:
 * Every decision here is single-condition, so each needs two vectors: the
 * range guard (sense=3 in range / sense=4 out of range), the level-sense
 * test (sense=3 vs sense<3), the change test (prev==now / prev!=now), the
 * both-edge test (sense=2 / sense!=2) and the rising test (sense=1 /
 * sense=0). Sweeping all four senses across all four (prev, now) pairs
 * covers every one of them.
 *
 * @par Example:
 * @code
 * if (priv_ra8_esp_hosted_gpio_edge_seen(0U, 1U, 1U)) { handler(arg); }
 * @endcode
 *
 * @see priv_ra8_esp_hosted_gpio_edge_poll_once
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] bool
priv_ra8_esp_hosted_gpio_edge_seen(uint8_t prev_level, uint8_t now_level, uint8_t sense);
