/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/src/ra8_esp_hosted_gpio_edge.c
 * @brief Software edge detector for side-band pins the ICU cannot serve.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * On this package the ICU external-interrupt inputs are concentrated on port
 * 0, so of the Pmod1 side-band nets only one has a channel. The rest still
 * have to raise the vendored callback, and this file is how: one periodic
 * ThreadX timer samples every registered pin, compares each sample against
 * the level stored from the previous pass, and calls the pin's handler when
 * the configured edge appears.
 *
 * This is a real detector with a worse latency bound, not a placeholder. The
 * bound is exactly one sampling period -- the port's ``edge_poll_ms``, two
 * milliseconds by default -- plus the timer thread's own scheduling latency.
 * That is acceptable for this link because the co-processor holds DATA_READY
 * asserted until the host takes the frame and holds HANDSHAKE asserted until
 * the chip select falls, so neither line produces a pulse the sampler can
 * miss; it only produces a later response than an ICU channel would.
 *
 * @par Why this lives beside ``ra8_esp_hosted_gpio.c`` rather than inside it
 * The eight vtable slots plus this detector overrun the project's
 * thousand-line file cap, and the split falls on a real seam: the slots own
 * the vendored calling convention, this file owns a table and a timer.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_gpio_internal.h"
#include "ra8_icu_regs.h"
#include "ra8_log.h"
#include "ra8_pin_interface.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#ifdef RA8_OFF_TARGET
#include "ra8_esp_hosted_tx_shim_sync_internal.h"
#else
#include "tx_api.h"
#endif

/**
 * @var s_tag
 * @brief Log tag identifying lines emitted by the software edge detector.
 * @details Distinct from the GPIO slice's own tag so a reader can tell a
 * polled-path refusal from a slot-level one.
 * @note Read-only after load.
 * @warning Changing it breaks log filters that key on the string.
 * @since 0.1.0
 */
static const char* const s_tag = "eh_edge";

/**
 * @enum ra8_esp_hosted_gpio_edge_const_t
 * @brief Named constants the detector needs beyond the shared limits.
 *
 * @details
 * The two sampled levels are named so the table's stored level reads as a
 * level rather than as an anonymous zero or one, and the timer's name is
 * length-checked against nothing -- ThreadX only stores the pointer.
 *
 * @invariant ::k_ra8_esp_hosted_gpio_edge_low and
 *            ::k_ra8_esp_hosted_gpio_edge_high are the only levels the table
 *            ever stores.
 * @invariant ::k_ra8_esp_hosted_gpio_edge_one_row is the row-count delta of a
 *            single registration.
 *
 * @par Example:
 * @code
 * row->last_level = (uint8_t)k_ra8_esp_hosted_gpio_edge_low;
 * @endcode
 *
 * @see ra8_esp_hosted_gpio_edge_seen
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_esp_hosted_gpio_edge_low     = 0U, /**< Sample read low.            */
  k_ra8_esp_hosted_gpio_edge_high    = 1U, /**< Sample read high.           */
  k_ra8_esp_hosted_gpio_edge_one_row = 1U, /**< Rows one registration adds. */
} ra8_esp_hosted_gpio_edge_const_t;

/**
 * @struct ra8_esp_hosted_gpio_edge_row
 * @brief One pin under software edge detection.
 *
 * @details
 * The stored level is what makes the detector an *edge* detector rather than
 * a level poller: it is seeded at registration from a live read, so the first
 * sampling pass compares against reality and cannot invent an edge that never
 * happened.
 *
 * @invariant ``used`` is true exactly when ``handler`` is non-null.
 * @invariant ``sense`` is 0..3, using the ``ra8_icu_irqmd_t`` encoding.
 * @invariant ``last_level`` is 0 or 1.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_gpio_edge_row_t row = {};
 * @endcode
 *
 * @see ra8_esp_hosted_gpio_edge_register
 * @since 0.1.0
 */
typedef struct ra8_esp_hosted_gpio_edge_row {
  ra8_port_pin_t pin;         /**< Packed pin being sampled.           */
  void (*handler)(void* arg); /**< Vendored callback to invoke.        */
  void*   arg;                /**< Opaque argument for ``handler``.    */
  uint8_t sense;              /**< Edge selector, ``ra8_icu_irqmd_t``. */
  uint8_t last_level;         /**< Level seen by the previous sample.  */
  bool    used;               /**< True while the row is occupied.     */
} ra8_esp_hosted_gpio_edge_row_t;

/**
 * @var s_rows
 * @brief Every pin currently under software edge detection.
 * @details Statically sized (NASA Power of 10 Rule 3); a registration that
 * does not fit is refused rather than allocated.
 * @note Written by the registration pair and by the sampling pass.
 * @warning Read from timer context; do not compact it while the timer runs.
 * @since 0.1.0
 */
static ra8_esp_hosted_gpio_edge_row_t s_rows[k_ra8_esp_hosted_gpio_row_max];

/**
 * @var s_timer
 * @brief The one periodic ThreadX timer shared by every polled row.
 * @details One timer for the whole table rather than one per pin: the
 * sampling pass is a handful of register reads, so a second timer would cost
 * more than it saves.
 * @note Created on the first registration, deleted with the last.
 * @warning Firing it re-enters ::ra8_esp_hosted_gpio_edge_poll_once; row
 *          handlers must therefore not block.
 * @since 0.1.0
 */
static TX_TIMER s_timer;

/**
 * @var s_timer_live
 * @brief True while ::s_timer has been created and not yet deleted.
 * @details ThreadX has no "is this control block live" query, so the fact is
 * tracked here; without it a second registration would try to create an
 * already-created timer.
 * @note Written only by the arm and disarm helpers.
 * @warning Must never disagree with the kernel's view of ::s_timer.
 * @since 0.1.0
 */
static bool s_timer_live;

/**
 * @var s_poll_ms
 * @brief Sampling period in milliseconds, shared by every polled row.
 * @details The ThreadX tick on this board is 1 kHz, so the millisecond-to-
 * tick conversion is the identity and this value is also the tick count.
 * @note Written only by ::ra8_esp_hosted_gpio_set_edge_poll_ms.
 * @warning Zero is rejected at the setter; ThreadX refuses a zero-tick timer.
 * @since 0.1.0
 */
static uint16_t s_poll_ms = (uint16_t)k_ra8_esp_hosted_gpio_poll_ms_default;

/**
 * @var s_tx_name_eh_edge
 * @brief Writable ThreadX object name for the ``eh_edge`` object.
 * @details ThreadX takes object names as ``CHAR*`` rather than
 * ``const CHAR*``, so a string literal would have to be cast and would drop a
 * qualifier the object really has. A writable array removes the cast instead
 * of hiding it.
 * @note Read by the create call only; ThreadX keeps the pointer.
 * @warning Must outlive the object it names; file-scope storage does.
 * @since 0.1.0
 */
static char s_tx_name_eh_edge[] = "eh_edge";

RA8_PRIV bool ra8_esp_hosted_gpio_edge_seen(uint8_t prev_level, uint8_t now_level, uint8_t sense)
{
  if (sense > (uint8_t)k_ra8_icu_irqmd_low) {
    return false;
  }
  if (sense == (uint8_t)k_ra8_icu_irqmd_low) {
    return now_level == (uint8_t)k_ra8_esp_hosted_gpio_edge_low;
  }
  if (prev_level == now_level) {
    return false;
  }
  if (sense == (uint8_t)k_ra8_icu_irqmd_both) {
    return true;
  }
  if (sense == (uint8_t)k_ra8_icu_irqmd_rising) {
    return now_level == (uint8_t)k_ra8_esp_hosted_gpio_edge_high;
  }
  return now_level == (uint8_t)k_ra8_esp_hosted_gpio_edge_low;
}

/**
 * @brief Find the row watching a pin.
 *
 * @details
 * Linear scan over a table of four; a scan cannot be indexed out of range and
 * beats any structure at this size.
 *
 * @param[in] pin Packed pin to look for.
 *
 * @return Row index, or ::k_ra8_esp_hosted_gpio_row_max when there is none.
 * @retval k_ra8_esp_hosted_gpio_row_max The pin is not being sampled.
 *
 * @pre The table is not being mutated concurrently.
 * @pre @p pin is a packed ``RA8_PIN`` value.
 * @post No table state is modified.
 * @post The result indexes ::s_rows or equals the table length.
 *
 * @note Safe from timer context; a pure scan.
 *
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: the loop is bounded by the compile-time table length.
 */
RA8_INTERNAL
static uint8_t internal_find(ra8_port_pin_t pin)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_esp_hosted_gpio_row_max; i++) {
    if (s_rows[i].used && (s_rows[i].pin == pin)) {
      return i;
    }
  }
  return (uint8_t)k_ra8_esp_hosted_gpio_row_max;
}

/**
 * @brief Read one pin through the slice's injected pin interface.
 *
 * @details
 * Folds the HAL's ``ra8_level_t`` onto the 0/1 the table stores, and reports
 * read failure separately so the caller can leave the stored level alone
 * rather than treat a failed read as a level change.
 *
 * @param[in] pin Packed pin to sample.
 * @param[out] out_level Receives 0 or 1; untouched when the read fails.
 *
 * @return Whether the read succeeded.
 * @retval true ``*out_level`` holds the sampled level.
 * @retval false The pin driver refused; ``*out_level`` is unchanged.
 *
 * @pre @p out_level is non-null.
 * @pre A pin interface is installed (one always is).
 * @post No pin state is modified.
 * @post A failed read writes nothing.
 *
 * @note Safe from timer context.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_sample(ra8_port_pin_t pin, uint8_t* out_level)
{
  const ra8_pin_interface_t* pin_if = ra8_esp_hosted_gpio_pin_interface();
  ra8_level_t                level  = k_ra8_level_low;
  if (pin_if->read(pin_if->ctx, pin, &level) != k_ra8_ok) {
    return false;
  }
  *out_level = (level == k_ra8_level_high) ? (uint8_t)k_ra8_esp_hosted_gpio_edge_high
                                           : (uint8_t)k_ra8_esp_hosted_gpio_edge_low;
  return true;
}

/**
 * @brief ThreadX expiry function: run one sampling pass.
 *
 * @details
 * A one-line shim so the pass itself stays callable from a test without a
 * kernel. It takes ThreadX's ``ULONG`` argument and discards it: the pass
 * walks the whole table, so there is nothing per-timer to carry.
 *
 * @param[in] arg ThreadX expiry argument; unused.
 *
 *
 * @pre The table is initialised (statically, so always).
 * @pre Row handlers do not block.
 * @post One sampling pass has completed.
 * @post The timer remains armed; it is periodic.
 *
 * @note Timer-thread context: no blocking, no allocation.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_timer_expiry(ULONG arg)
{
  (void)arg;
  ra8_esp_hosted_gpio_edge_poll_once();
}

/**
 * @brief Create the shared periodic timer if it is not already running.
 *
 * @details
 * ThreadX takes the first expiry and the reschedule interval separately; both
 * are the sampling period here, so the very first pass happens one period
 * after the first registration rather than immediately. That is deliberate:
 * the registration has just seeded every row's level from a live read, so a
 * pass at tick zero could only report "no change".
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The timer is running.
 * @retval k_ra8_err_rtos_error ThreadX refused to create it.
 *
 * @pre The ThreadX kernel is running.
 * @pre ::s_poll_ms is non-zero.
 * @post On success ::s_timer_live is true.
 * @post A second call while the timer runs changes nothing.
 *
 * @note Not thread-safe with respect to the disarm helper.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_timer_arm(void)
{
  if (s_timer_live) {
    return k_ra8_ok;
  }
  const ULONG ticks = (ULONG)s_poll_ms;
  const UINT  st    = tx_timer_create(&s_timer,
                                      s_tx_name_eh_edge,
                                      internal_timer_expiry,
                                      (ULONG)0,
                                      ticks,
                                      ticks,
                                      TX_AUTO_ACTIVATE);
  if (st != TX_SUCCESS) {
    ra8_log_error_val(s_tag, "edge timer create failed", (uint32_t)st);
    return k_ra8_err_rtos_error;
  }
  s_timer_live = true;
  return k_ra8_ok;
}

/**
 * @brief Delete the shared periodic timer if it is running.
 *
 * @details
 * Called when the last row goes, so a torn-down port leaves no kernel object
 * behind, and by the period setter so a new period can be re-armed.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok No timer is running.
 * @retval k_ra8_err_rtos_error ThreadX refused to delete it.
 *
 * @pre The expiry function is not currently executing.
 * @pre The ThreadX kernel is running when the timer is live.
 * @post On success ::s_timer_live is false.
 * @post A second call changes nothing.
 *
 * @note Not thread-safe with respect to the arm helper.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_timer_disarm(void)
{
  if (!s_timer_live) {
    return k_ra8_ok;
  }
  const UINT st = tx_timer_delete(&s_timer);
  if (st != TX_SUCCESS) {
    ra8_log_error_val(s_tag, "edge timer delete failed", (uint32_t)st);
    return k_ra8_err_rtos_error;
  }
  s_timer_live = false;
  return k_ra8_ok;
}

RA8_PRIV uint8_t ra8_esp_hosted_gpio_edge_count(void)
{
  uint8_t count = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_esp_hosted_gpio_row_max; i++) {
    if (s_rows[i].used) {
      count = (uint8_t)(count + (uint8_t)k_ra8_esp_hosted_gpio_edge_one_row);
    }
  }
  return count;
}

RA8_PRIV ra8_err_t ra8_esp_hosted_gpio_set_edge_poll_ms(uint16_t period_ms)
{
  if (period_ms == 0U) {
    return k_ra8_err_invalid_arg;
  }
  s_poll_ms = period_ms;
  if (!s_timer_live) {
    return k_ra8_ok;
  }
  const ra8_err_t err = internal_timer_disarm();
  return (err == k_ra8_ok) ? internal_timer_arm() : err;
}

RA8_PRIV ra8_err_t ra8_esp_hosted_gpio_edge_register(ra8_port_pin_t pin,
                                                     uint8_t        sense,
                                                     void (*handler)(void*),
                                                     void* arg)
{
  if (handler == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (sense > (uint8_t)k_ra8_icu_irqmd_low) {
    return k_ra8_err_invalid_arg;
  }
  if (internal_find(pin) < (uint8_t)k_ra8_esp_hosted_gpio_row_max) {
    return k_ra8_err_exists;
  }
  uint8_t slot = (uint8_t)k_ra8_esp_hosted_gpio_row_max;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_esp_hosted_gpio_row_max; i++) {
    if (!s_rows[i].used) {
      slot = i;
      break;
    }
  }
  if (slot >= (uint8_t)k_ra8_esp_hosted_gpio_row_max) {
    return k_ra8_err_no_mem;
  }

  const ra8_err_t err = ra8_gpio_input_init(pin, k_ra8_pull_none);
  if (err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "polled pin input init failed", (uint32_t)err);
    return err;
  }
  uint8_t level = (uint8_t)k_ra8_esp_hosted_gpio_edge_low;
  (void)internal_sample(pin, &level);

  s_rows[slot].pin        = pin;
  s_rows[slot].handler    = handler;
  s_rows[slot].arg        = arg;
  s_rows[slot].sense      = sense;
  s_rows[slot].last_level = level;
  s_rows[slot].used       = true;

  const ra8_err_t timer_err = internal_timer_arm();
  if (timer_err != k_ra8_ok) {
    s_rows[slot] = (ra8_esp_hosted_gpio_edge_row_t){};
    (void)ra8_gpio_release(pin);
  }
  return timer_err;
}

RA8_PRIV ra8_err_t ra8_esp_hosted_gpio_edge_unregister(ra8_port_pin_t pin)
{
  const uint8_t slot = internal_find(pin);
  if (slot >= (uint8_t)k_ra8_esp_hosted_gpio_row_max) {
    return k_ra8_err_not_found;
  }
  s_rows[slot] = (ra8_esp_hosted_gpio_edge_row_t){};
  (void)ra8_gpio_release(pin);
  if (ra8_esp_hosted_gpio_edge_count() != 0U) {
    return k_ra8_ok;
  }
  return internal_timer_disarm();
}

RA8_PRIV void ra8_esp_hosted_gpio_edge_poll_once(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_esp_hosted_gpio_row_max; i++) {
    ra8_esp_hosted_gpio_edge_row_t* row = &s_rows[i];
    uint8_t                         now = (uint8_t)k_ra8_esp_hosted_gpio_edge_low;
    if (row->used && internal_sample(row->pin, &now)) {
      const uint8_t prev = row->last_level;
      row->last_level    = now;
      if (ra8_esp_hosted_gpio_edge_seen(prev, now, row->sense)) {
        row->handler(row->arg);
      }
    }
  }
}
