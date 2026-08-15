/**
 * @file port/esp-hosted/src/ra8_esp_hosted_gpio.c
 * @brief The eight side-band GPIO slots of the esp-hosted OS-abstraction vtable.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vendored SPI transport touches hardware pins only through these eight
 * function pointers. It hands each one an opaque ``(void* port, uint32_t
 * pin)`` pair, which this file decodes back into a packed ``ra8_port_pin_t``
 * exactly inversely to the encoding in ``port_esp_hosted_host_config.h`` --
 * including the deliberate detail that port 0 encodes as a **null pointer**,
 * so the port argument is range-checked after decoding and never
 * null-checked.
 *
 * Two of the slots need more than a HAL call. ``_h_config_gpio_as_interrupt``
 * has to cope with a package that routes only one of the link's side-band
 * nets to an ICU channel, and falls back on the software edge detector in
 * ``ra8_esp_hosted_gpio_edge.c`` for the rest. ``_h_hold_gpio`` wants
 * per-pin state retention across deep sleep, which this HAL does not yet
 * expose; it reports that honestly rather than pretending.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "esp_hosted_power_save.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_os.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_gpio_internal.h"
#include "ra8_esp_hosted_pins.h"
#include "ra8_icu_regs.h"
#include "ra8_log.h"
#include "ra8_pin_interface.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_reset.h"

/**
 * @var s_tag
 * @brief Log tag identifying lines emitted by the side-band GPIO slice.
 * @details Short enough to keep a log line readable next to the transport's
 * own ``spi`` tag, and distinct from every other tag in this port.
 * @note Read-only after load.
 * @warning Changing it breaks log filters that key on the string.
 * @since 0.1.0
 */
static const char* const s_tag = "eh_gpio";

/**
 * @enum ra8_esp_hosted_gpio_sentinel_t
 * @brief The "signal is not wired" pin value as it arrives at a slot.
 * @details The vendored vtable takes the pin as ``uint32_t`` while the
 * configuration macros produce ``-1`` for an unwired signal, so the sentinel
 * reaches a slot converted to all ones.
 * @invariant Numerically greater than ``k_ra8_pin_max``, so the range test
 *            would also reject it; the explicit test exists because "not
 *            wired" is a different fact from "out of range".
 * @par Example:
 * @code
 * if (gpio_num == (uint32_t)k_ra8_esp_hosted_gpio_pin_unwired) { reject(); }
 * @endcode
 * @see priv_ra8_esp_hosted_gpio_decode_pin
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_esp_hosted_gpio_pin_unwired = 0xFFFFFFFFU, /**< ``-1`` widened to 32 bits. */
} ra8_esp_hosted_gpio_sentinel_t;

/**
 * @enum ra8_esp_hosted_gpio_mode_t
 * @brief Pin directions ``_h_config_gpio`` accepts.
 * @details Mirrors the vendored ``H_GPIO_MODE_DEF_*`` spellings so the slot
 * can switch on a typed value; the static assertions below prove the two
 * encodings agree.
 * @invariant Exactly these two directions are accepted; anything else is
 *            rejected rather than defaulted.
 * @par Example:
 * @code
 * if (mode == (uint32_t)k_ra8_esp_hosted_gpio_mode_output) { drive(); }
 * @endcode
 * @see priv_ra8_esp_hosted_gpio_bind
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_esp_hosted_gpio_mode_input  = 0U, /**< Digital input.            */
  k_ra8_esp_hosted_gpio_mode_output = 1U, /**< Push-pull digital output. */
} ra8_esp_hosted_gpio_mode_t;

/**
 * @enum ra8_esp_hosted_gpio_pull_t
 * @brief Pull selectors ``_h_pull_gpio`` accepts.
 * @details Mirrors the vendored ``H_GPIO_PULL_*`` spellings. Only the pull-up
 * can be honoured on this part; the pull-down selector is recognised so it
 * can be refused explicitly.
 * @invariant The two values are distinct, so a refusal cannot be mistaken
 *            for a pull-up request.
 * @par Example:
 * @code
 * if (pull_value == (uint32_t)k_ra8_esp_hosted_gpio_pull_down) { refuse(); }
 * @endcode
 * @see priv_ra8_esp_hosted_gpio_bind
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_esp_hosted_gpio_pull_up   = 0U, /**< Internal pull-up.                */
  k_ra8_esp_hosted_gpio_pull_down = 1U, /**< Internal pull-down (no silicon). */
} ra8_esp_hosted_gpio_pull_t;

/**
 * @enum ra8_esp_hosted_gpio_read_t
 * @brief Values ``_h_read_gpio`` returns for a successful read.
 * @details The vendored driver compares the result against ``H_HS_VAL_*`` /
 * ``H_DR_VAL_*``, which are plain 0 and 1, so the raw logic level is
 * returned without inversion.
 * @invariant Both values are non-negative, and every failure code the slot
 *            can return is negative, so a failure never reads as a level.
 * @par Example:
 * @code
 * gpio_pin_state_t hs = g_h.funcs->_h_read_gpio(port, pin);
 * @endcode
 * @see priv_ra8_esp_hosted_gpio_bind
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_esp_hosted_gpio_read_low  = 0U, /**< Pin read low.  */
  k_ra8_esp_hosted_gpio_read_high = 1U, /**< Pin read high. */
} ra8_esp_hosted_gpio_read_t;

static_assert((uint32_t)k_ra8_esp_hosted_gpio_mode_input == (uint32_t)H_GPIO_MODE_DEF_INPUT,
              "input mode selector must match the vendored spelling");
static_assert((uint32_t)k_ra8_esp_hosted_gpio_mode_output == (uint32_t)H_GPIO_MODE_DEF_OUTPUT,
              "output mode selector must match the vendored spelling");
static_assert((uint32_t)k_ra8_esp_hosted_gpio_pull_up == (uint32_t)H_GPIO_PULL_UP,
              "pull-up selector must match the vendored spelling");
static_assert((uint32_t)k_ra8_esp_hosted_gpio_pull_down == (uint32_t)H_GPIO_PULL_DOWN,
              "pull-down selector must match the vendored spelling");
static_assert((uint8_t)H_HS_INTR_EDGE <= (uint8_t)k_ra8_icu_irqmd_low,
              "HANDSHAKE edge selector must be a legal ICU sense");
static_assert((uint8_t)H_DR_INTR_EDGE <= (uint8_t)k_ra8_icu_irqmd_low,
              "DATA_READY edge selector must be a legal ICU sense");

/**
 * @struct ra8_esp_hosted_gpio_irq_row
 * @brief One pin served by a real ICU external-interrupt channel.
 *
 * @details
 * The trampoline installed with the HAL receives this row as its context, so
 * it can reach the vendored callback without a search. The row also carries
 * the channel number, which teardown needs and the vendored driver never
 * learns.
 *
 * @invariant ``used`` is true exactly when ``handler`` is non-null.
 * @invariant ``irq_num`` is 0..15 while ``used`` is true.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_gpio_irq_row_t row = {};
 * @endcode
 *
 * @see priv_ra8_esp_hosted_gpio_bind
 * @since 0.1.0
 */
typedef struct ra8_esp_hosted_gpio_irq_row {
  ra8_port_pin_t pin;         /**< Packed pin the channel watches.        */
  void (*handler)(void* arg); /**< Vendored callback to invoke.           */
  void*   arg;                /**< Opaque argument for ``handler``.       */
  uint8_t irq_num;            /**< ICU external-interrupt channel, 0..15. */
  bool    used;               /**< True while the row is occupied.        */
} ra8_esp_hosted_gpio_irq_row_t;

/**
 * @var s_irq_rows
 * @brief Pins currently attached to an ICU external-interrupt channel.
 * @details Statically sized (NASA Power of 10 Rule 3): the link has at most
 * four side-band nets, so the table can never be outgrown by the vendored
 * driver, and an over-request is refused rather than allocated.
 * @note Written only by the interrupt-configuration and teardown slots.
 * @warning Read from interrupt context through the trampoline's row pointer;
 *          never compact or reorder the table while a channel is attached.
 * @since 0.1.0
 */
static ra8_esp_hosted_gpio_irq_row_t s_irq_rows[k_ra8_esp_hosted_gpio_row_max];

/**
 * @var s_pin_if
 * @brief The pin driver every level read and write goes through.
 * @details Null means "use the production instance", which keeps the seam
 * usable before any initialisation has run.
 * @note Swapped only by ::priv_ra8_esp_hosted_gpio_set_pin_interface.
 * @warning Pointing this at a short-lived object leaves the slice reading a
 *          dangling vtable.
 * @since 0.1.0
 */
static const ra8_pin_interface_t* s_pin_if;

/*
 * The production pin driver is defined in libs/ra8_hal/src/gpio.c and, like
 * every other consumer of it in this tree, is reached by declaration rather
 * than through a header -- ra8_pin_interface.h describes the type, not the
 * instance.
 */
extern const ra8_pin_interface_t g_ra8_gpio_pin_interface;

RA8_PRIV bool priv_ra8_esp_hosted_gpio_decode_pin(const void*     gpio_port,
                                                  uint32_t        gpio_num,
                                                  ra8_port_pin_t* out_pin)
{
  if (out_pin == nullptr) {
    return false;
  }
  const uintptr_t port_idx = (uintptr_t)gpio_port;
  if ((gpio_num == (uint32_t)k_ra8_esp_hosted_gpio_pin_unwired) ||
      (port_idx > (uintptr_t)k_ra8_port_max) || (gpio_num > (uint32_t)k_ra8_pin_max)) {
    return false;
  }
  *out_pin = RA8_PIN((uint16_t)port_idx, (uint16_t)gpio_num);
  return true;
}

RA8_PRIV void priv_ra8_esp_hosted_gpio_set_pin_interface(const ra8_pin_interface_t* iface)
{
  s_pin_if = iface;
}

RA8_PRIV const ra8_pin_interface_t* priv_ra8_esp_hosted_gpio_pin_interface(void)
{
  return (s_pin_if != nullptr) ? s_pin_if : &g_ra8_gpio_pin_interface;
}

/**
 * @brief Find the interrupt row watching a pin.
 *
 * @details
 * Linear scan over a table of four; a scan cannot be indexed out of range and
 * is faster than any structure at this size.
 *
 * @param[in] pin Packed pin to look for.
 *
 * @return Row index, or ::k_ra8_esp_hosted_gpio_row_max when the pin has no
 *         hardware row.
 * @retval k_ra8_esp_hosted_gpio_row_max The pin is not attached to a channel.
 *
 * @pre The table is only mutated with interrupts for these pins disabled.
 * @pre @p pin is a packed ``RA8_PIN`` value.
 * @post No table state is modified.
 * @post The result indexes ::s_irq_rows or equals the table length.
 *
 * @note Safe from interrupt context; a pure scan.
 *
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: the loop is bounded by the compile-time table length.
 */
RA8_INTERNAL
static uint8_t internal_irq_find(ra8_port_pin_t pin)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_esp_hosted_gpio_row_max; i++) {
    if (s_irq_rows[i].used && (s_irq_rows[i].pin == pin)) {
      return i;
    }
  }
  return (uint8_t)k_ra8_esp_hosted_gpio_row_max;
}

/**
 * @brief Take the first free interrupt row.
 *
 * @details
 * Rows are never compacted, because a live trampoline holds a pointer to the
 * row it was installed with; allocation therefore only looks for a hole.
 *
 * @return Row index, or ::k_ra8_esp_hosted_gpio_row_max when the table is
 *         full.
 * @retval k_ra8_esp_hosted_gpio_row_max Every row is occupied.
 *
 * @pre No trampoline is executing against a row being reused.
 * @pre The caller has already rejected a duplicate registration.
 * @post No table state is modified by the search itself.
 * @post The result indexes ::s_irq_rows or equals the table length.
 *
 * @note Not thread-safe with respect to the teardown slot.
 *
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: the loop is bounded by the compile-time table length.
 */
RA8_INTERNAL
static uint8_t internal_irq_alloc(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_esp_hosted_gpio_row_max; i++) {
    if (!s_irq_rows[i].used) {
      return i;
    }
  }
  return (uint8_t)k_ra8_esp_hosted_gpio_row_max;
}

/**
 * @brief ICU handler shim that calls the vendored side-band callback.
 *
 * @details
 * Installed against the ICU channel with the owning row as its context, so
 * the hot path is one load and one indirect call. It deliberately does not
 * log and does not block: the callback it invokes posts a semaphore from
 * interrupt context, which is the whole latency budget of the DATA_READY
 * line.
 *
 * @param[in] ctx The ::ra8_esp_hosted_gpio_irq_row that installed it.
 *
 *
 * @pre @p ctx points at a row that is still occupied.
 * @pre The vendored callback is safe to run from interrupt context.
 * @post Exactly one callback invocation per accepted edge.
 * @post No module state is modified.
 *
 * @note Interrupt context: no logging, no blocking, no allocation.
 *
 * @since 0.1.0
 */
RA8_INTERNAL RA8_ISR_SAFE static void internal_isr_trampoline(void* ctx)
{
  const ra8_esp_hosted_gpio_irq_row_t* row = (const ra8_esp_hosted_gpio_irq_row_t*)ctx;
  if ((row == nullptr) || (row->handler == nullptr)) {
    return;
  }
  row->handler(row->arg);
}

/**
 * @brief Attach a pin to a real ICU external-interrupt channel.
 *
 * @details
 * Records the row first so the trampoline has a valid context the instant the
 * NVIC line is enabled, then programmes the pin, the IRQCR sense and the
 * handler in one HAL call. The digital filter stays off: the callback only
 * posts a semaphore, so a doubled edge is harmless, while filtering would add
 * latency to the one line that is latency-critical.
 *
 * @param[in] pin Packed pin to attach.
 * @param[in] irq_num ICU channel serving @p pin, 0..15.
 * @param[in] sense Edge selector in the ``ra8_icu_irqmd_t`` encoding.
 * @param[in] handler Vendored callback; must be non-null.
 * @param[in] arg Opaque argument for @p handler.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The channel is live.
 * @retval k_ra8_err_no_mem The interrupt table is full.
 * @retval k_ra8_err_exists The channel was already registered.
 * @retval k_ra8_err_gpio_conflict The pin is owned by another module.
 *
 * @pre ``ra8_icu_init`` and ``ra8_isr_init`` have run.
 * @pre @p pin has no row yet.
 * @post On success the row is occupied and the NVIC line is enabled.
 * @post On failure the row is released, so no stale context survives.
 *
 * @note Not thread-safe; call from the transport's start-up path.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_attach_hardware(ra8_port_pin_t pin,
                                          uint8_t        irq_num,
                                          uint8_t        sense,
                                          void (*handler)(void*),
                                          void* arg)
{
  const uint8_t slot = internal_irq_alloc();
  if (slot >= (uint8_t)k_ra8_esp_hosted_gpio_row_max) {
    return k_ra8_err_no_mem;
  }
  s_irq_rows[slot].pin     = pin;
  s_irq_rows[slot].handler = handler;
  s_irq_rows[slot].arg     = arg;
  s_irq_rows[slot].irq_num = irq_num;
  s_irq_rows[slot].used    = true;

  const ra8_gpio_irq_cfg_t cfg = {
    .pull       = k_ra8_pull_none,
    .sense      = (ra8_icu_irqmd_t)sense,
    .filter_div = k_ra8_icu_fclksel_pclkb,
    .filter_en  = false,
    .priority   = (uint8_t)k_ra8_esp_hosted_gpio_irq_priority,
  };
  const ra8_err_t err =
    ra8_gpio_attach_irq(pin, irq_num, &cfg, internal_isr_trampoline, &s_irq_rows[slot]);
  if (err != k_ra8_ok) {
    s_irq_rows[slot] = (ra8_esp_hosted_gpio_irq_row_t){};
    ra8_log_error_val(s_tag, "attach_irq failed", (uint32_t)err);
  }
  return err;
}

/**
 * @brief ``_h_config_gpio``: put a side-band pin in a plain digital mode.
 *
 * @details
 * Output configuration goes through the injected pin interface, which is the
 * only direction that has a row in ``ra8_pin_interface_t``; input
 * configuration calls the HAL directly. An output comes up at the level that
 * leaves the co-processor OUT of reset, so configuring the pin can never
 * itself assert reset.
 *
 * @param[in] gpio_port Encoded port index.
 * @param[in] gpio_num Encoded pin index, or the unwired sentinel.
 * @param[in] mode ``H_GPIO_MODE_DEF_INPUT`` or ``H_GPIO_MODE_DEF_OUTPUT``.
 *
 * @return int Vendored return code.
 * @retval RET_OK The pin is configured.
 * @retval RET_INVALID The pair did not name a pin, or @p mode is unknown.
 * @retval RET_FAIL The HAL refused the configuration.
 *
 * @pre The IOPORT module is powered.
 * @pre The pin is not already owned by a peripheral.
 * @post On RET_OK the pin is in the requested direction.
 * @post On any other code no pin state changed.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_config_gpio(void* gpio_port, uint32_t gpio_num, uint32_t mode)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  if (!priv_ra8_esp_hosted_gpio_decode_pin(gpio_port, gpio_num, &pin)) {
    return RET_INVALID;
  }
  ra8_err_t err = k_ra8_err_invalid_arg;
  if (mode == (uint32_t)k_ra8_esp_hosted_gpio_mode_output) {
    const ra8_pin_interface_t* pin_if = priv_ra8_esp_hosted_gpio_pin_interface();
    err = pin_if->output_init(pin_if->ctx, pin, (ra8_level_t)H_RESET_VAL_INACTIVE);
  } else if (mode == (uint32_t)k_ra8_esp_hosted_gpio_mode_input) {
    err = ra8_gpio_input_init(pin, k_ra8_pull_none);
  } else {
    return RET_INVALID;
  }
  return (err == k_ra8_ok) ? RET_OK : RET_FAIL;
}

/**
 * @brief ``_h_config_gpio_as_interrupt``: watch a side-band pin for an edge.
 *
 * @details
 * Asks ``ra8_esp_hosted_pin_irq_num`` whether the package routes this pin to
 * the ICU. When it does, a real hardware edge is programmed. When it does
 * not, the pin joins the software edge detector, which delivers the identical
 * callback at a bounded sampling latency instead of a hardware one. The
 * vendored driver is unaware of which path it got, which is what lets the
 * harness move without touching this code.
 *
 * @param[in] gpio_port Encoded port index.
 * @param[in] gpio_num Encoded pin index, or the unwired sentinel.
 * @param[in] intr_type Edge selector in the ``ra8_icu_irqmd_t`` encoding.
 * @param[in] gpio_isr_handler Callback for the edge; must be non-null.
 * @param[in] arg Opaque argument handed back to the callback.
 *
 * @return int Vendored return code.
 * @retval RET_OK The pin is watched by one of the two paths.
 * @retval RET_INVALID Bad pin pair, unknown sense, or a null handler.
 * @retval RET_FAIL The pin already had a row, or the HAL refused.
 *
 * @pre The ICU and ISR tables are initialised.
 * @pre The ThreadX kernel is running when the polled path is taken.
 * @post On RET_OK exactly one of the two paths owns the pin.
 * @post On any other code no pin is claimed by this call.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_config_gpio_as_interrupt(void*    gpio_port,
                                             uint32_t gpio_num,
                                             uint32_t intr_type,
                                             void (*gpio_isr_handler)(void* arg),
                                             void* arg)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  if (!priv_ra8_esp_hosted_gpio_decode_pin(gpio_port, gpio_num, &pin)) {
    return RET_INVALID;
  }
  if (gpio_isr_handler == nullptr) {
    return RET_INVALID;
  }
  if (intr_type > (uint32_t)k_ra8_icu_irqmd_low) {
    return RET_INVALID;
  }
  if (internal_irq_find(pin) < (uint8_t)k_ra8_esp_hosted_gpio_row_max) {
    return RET_FAIL;
  }

  const uint8_t   irq_num = ra8_esp_hosted_pin_irq_num(pin);
  const uint8_t   sense   = (uint8_t)intr_type;
  const ra8_err_t err =
    (irq_num == (uint8_t)k_ra8_esp_hosted_irq_none)
      ? priv_ra8_esp_hosted_gpio_edge_register(pin, sense, gpio_isr_handler, arg)
      : internal_attach_hardware(pin, irq_num, sense, gpio_isr_handler, arg);
  return (err == k_ra8_ok) ? RET_OK : RET_FAIL;
}

/**
 * @brief ``_h_teardown_gpio_interrupt``: stop watching a side-band pin.
 *
 * @details
 * Undoes whichever of the two paths took the pin: a hardware row is detached
 * from the ICU and freed, anything else is offered to the software edge
 * detector. A pin that neither path holds is reported as a failure rather
 * than silently accepted, because the vendored driver only calls this for
 * pins it believes it registered.
 *
 * @param[in] gpio_port Encoded port index.
 * @param[in] gpio_num Encoded pin index, or the unwired sentinel.
 *
 * @return int Vendored return code.
 * @retval RET_OK The pin is no longer watched.
 * @retval RET_INVALID The pair did not name a pin.
 * @retval RET_FAIL The pin was not registered, or the HAL refused.
 *
 * @pre No handler for this pin is currently executing.
 * @pre The pin was registered by ``_h_config_gpio_as_interrupt``.
 * @post The pin raises no further callbacks.
 * @post The row the pin occupied is free for reuse.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_teardown_gpio_interrupt(void* gpio_port, uint32_t gpio_num)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  if (!priv_ra8_esp_hosted_gpio_decode_pin(gpio_port, gpio_num, &pin)) {
    return RET_INVALID;
  }
  const uint8_t slot = internal_irq_find(pin);
  if (slot >= (uint8_t)k_ra8_esp_hosted_gpio_row_max) {
    return (priv_ra8_esp_hosted_gpio_edge_unregister(pin) == k_ra8_ok) ? RET_OK : RET_FAIL;
  }
  const ra8_err_t err = ra8_gpio_detach_irq(pin, s_irq_rows[slot].irq_num);
  s_irq_rows[slot]    = (ra8_esp_hosted_gpio_irq_row_t){};
  return (err == k_ra8_ok) ? RET_OK : RET_FAIL;
}

/**
 * @brief ``_h_read_gpio``: sample the raw logic level of a side-band pin.
 *
 * @details
 * Returns the level uninverted. The vendored driver compares the result
 * against ``H_HS_VAL_ACTIVE`` and friends, which already encode the harness
 * polarity, so inverting here would apply that polarity twice. Both failure
 * codes are negative and therefore cannot be mistaken for either level.
 *
 * @param[in] gpio_port Encoded port index.
 * @param[in] gpio_num Encoded pin index, or the unwired sentinel.
 *
 * @return int The level, or a vendored failure code.
 * @retval 0 The pin read low.
 * @retval 1 The pin read high.
 * @retval RET_INVALID The pair did not name a pin.
 * @retval RET_FAIL The pin driver could not read the pin.
 *
 * @pre The pin was configured as an input, or is an output being read back.
 * @pre A pin interface is installed (one always is).
 * @post No pin state is modified.
 * @post The result is 0, 1, or a negative failure code.
 *
 * @note Safe from interrupt context when the installed interface is.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_read_gpio(void* gpio_port, uint32_t gpio_num)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  if (!priv_ra8_esp_hosted_gpio_decode_pin(gpio_port, gpio_num, &pin)) {
    return RET_INVALID;
  }
  const ra8_pin_interface_t* pin_if = priv_ra8_esp_hosted_gpio_pin_interface();
  ra8_level_t                level  = k_ra8_level_low;
  if (pin_if->read(pin_if->ctx, pin, &level) != k_ra8_ok) {
    return RET_FAIL;
  }
  return (level == k_ra8_level_high) ? (int)k_ra8_esp_hosted_gpio_read_high
                                     : (int)k_ra8_esp_hosted_gpio_read_low;
}

/**
 * @brief ``_h_write_gpio``: drive a side-band output.
 *
 * @details
 * Any non-zero value drives the pin high, matching how the vendored driver
 * passes ``H_RESET_VAL_ACTIVE`` / ``H_RESET_VAL_INACTIVE`` straight through.
 *
 * @param[in] gpio_port Encoded port index.
 * @param[in] gpio_num Encoded pin index, or the unwired sentinel.
 * @param[in] value Zero drives low; anything else drives high.
 *
 * @return int Vendored return code.
 * @retval RET_OK The pin holds the requested level.
 * @retval RET_INVALID The pair did not name a pin.
 * @retval RET_FAIL The pin driver refused the write.
 *
 * @pre The pin was configured as an output.
 * @pre A pin interface is installed (one always is).
 * @post On RET_OK the pin holds @p value.
 * @post On any other code the pin is unchanged.
 *
 * @note Not thread-safe with respect to the same pin.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_write_gpio(void* gpio_port, uint32_t gpio_num, uint32_t value)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  if (!priv_ra8_esp_hosted_gpio_decode_pin(gpio_port, gpio_num, &pin)) {
    return RET_INVALID;
  }
  const ra8_pin_interface_t* pin_if = priv_ra8_esp_hosted_gpio_pin_interface();
  const ra8_level_t          level  = (value != 0U) ? k_ra8_level_high : k_ra8_level_low;
  return (pin_if->write(pin_if->ctx, pin, level) == k_ra8_ok) ? RET_OK : RET_FAIL;
}

/**
 * @brief ``_h_pull_gpio``: enable or disable an internal pull on a pin.
 *
 * @details
 * The RA8D2 PFS carries a pull-**up** bit and nothing else, so a pull-down
 * request is refused outright: reporting success for it would leave the
 * caller believing a bias exists that no silicon provides. A pull-up is real,
 * and is applied by re-taking the pin as an input with the pull selected --
 * the pin claim is released first because the HAL's input configuration is
 * also its claim, and the pin is already ours by this point.
 *
 * @param[in] gpio_port Encoded port index.
 * @param[in] gpio_num Encoded pin index, or the unwired sentinel.
 * @param[in] pull_value ``H_GPIO_PULL_UP`` or ``H_GPIO_PULL_DOWN``.
 * @param[in] enable Non-zero installs the pull; zero removes it.
 *
 * @return int Vendored return code.
 * @retval RET_OK The requested pull-up state is in force.
 * @retval RET_INVALID The pair did not name a pin, or the selector is
 *         unknown.
 * @retval RET_FAIL A pull-down was requested, or the HAL refused.
 *
 * @pre The pin is an input, or may be turned into one.
 * @pre The IOPORT module is powered.
 * @post On RET_OK the pin is an input with the requested pull.
 * @post A refused pull-down leaves the pin exactly as it was.
 *
 * @note Not thread-safe.
 * @warning No internal pull-down exists on this part; fit an external
 *          resistor if the harness needs one.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int
internal_pull_gpio(void* gpio_port, uint32_t gpio_num, uint32_t pull_value, uint32_t enable)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  if (!priv_ra8_esp_hosted_gpio_decode_pin(gpio_port, gpio_num, &pin)) {
    return RET_INVALID;
  }
  if (pull_value == (uint32_t)k_ra8_esp_hosted_gpio_pull_down) {
    ra8_log_warn(s_tag, "pull-down requested; RA8D2 PFS has no pull-down bit");
    return RET_FAIL;
  }
  if (pull_value != (uint32_t)k_ra8_esp_hosted_gpio_pull_up) {
    return RET_INVALID;
  }
  const ra8_pin_pull_t pull = (enable != 0U) ? k_ra8_pull_up : k_ra8_pull_none;
  (void)ra8_gpio_release(pin);
  return (ra8_gpio_input_init(pin, pull) == k_ra8_ok) ? RET_OK : RET_FAIL;
}

/**
 * @brief ``_h_hold_gpio``: freeze a pin's state across deep sleep.
 *
 * @details
 * The vendored power-save driver calls this on the co-processor reset line so
 * the line keeps its level while the host sleeps. On the RA8D2 that retention
 * is DPSBYCR.IOKEEP, which is a **whole-chip** control applied to Deep
 * Software Standby, and ``libs/ra8_hal`` exposes it only as one field of the
 * configuration ``ra8_lpm_init`` consumes -- there is no per-pin hold and no
 * standalone IOKEEP setter. Driving the whole LPM block from a per-pin call
 * would reconfigure far more than the caller asked for, so the request is
 * refused and the caller keeps the pin awake instead of believing in a hold
 * that never happened. Host power save is disabled in this port
 * (``H_HOST_PS_ALLOWED`` is 0), so nothing currently reaches this slot.
 *
 * TODO(ra8_hal exposes no standalone DPSBYCR.IOKEEP control: io_port_keep is
 * settable only through ra8_lpm_init's whole-block configuration, and there
 * is no per-pin retention API at all)
 *
 * @param[in] gpio_port Encoded port index.
 * @param[in] gpio_num Encoded pin index, or the unwired sentinel.
 * @param[in] hold_value ``H_ENABLE`` to freeze, ``H_DISABLE`` to release.
 *
 * @return int Vendored return code.
 * @retval RET_INVALID The pair did not name a pin.
 * @retval RET_FAIL No per-pin retention control is available.
 *
 * @pre The pin was configured by ``_h_config_gpio``.
 * @pre The caller treats a non-zero result as "the pin is not frozen".
 * @post No pin or LPM state is modified.
 * @post The caller is never told a hold succeeded.
 *
 * @note Not thread-safe.
 * @warning Never make this return RET_OK without a real retention control
 *          behind it; the caller would sleep with the reset line floating.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_hold_gpio(void* gpio_port, uint32_t gpio_num, uint32_t hold_value)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  if (!priv_ra8_esp_hosted_gpio_decode_pin(gpio_port, gpio_num, &pin)) {
    return RET_INVALID;
  }
  ra8_log_warn(s_tag, "pin hold unavailable: no per-pin retention control in ra8_hal");
  ra8_log_info_val(s_tag, "hold refused for pin", (uint32_t)pin);
  ra8_log_info_val(s_tag, "hold value requested", hold_value);
  return RET_FAIL;
}

/**
 * @brief ``_h_get_host_wakeup_or_reboot_reason``: why the host last started.
 *
 * @details
 * Reads the real latched reset cause through ``ra8_reset_get_cause``, which
 * decodes RSTSR0/1/2/3, and folds it onto the three answers the vendored
 * power-save driver understands. Only a Deep Software Standby exit counts as
 * "woke from power save"; every other latched cause is an ordinary reboot,
 * and a failed read is reported as undefined rather than guessed.
 *
 * @return int One of the vendored ``HOSTED_WAKEUP_*`` values.
 * @retval HOSTED_WAKEUP_DEEP_SLEEP The part exited Deep Software Standby.
 * @retval HOSTED_WAKEUP_NORMAL_REBOOT Any other latched reset cause.
 * @retval HOSTED_WAKEUP_UNDEFINED The cause could not be read.
 *
 * @pre The SYSC block is mapped (always true on target and under the host
 *      fake mapping).
 * @pre The reset flags have not been cleared since boot by another module.
 * @post No register is modified; the flags stay latched for other readers.
 * @post Exactly one of the three vendored values is returned.
 *
 * @note Safe from any context; a read-only register decode.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_wakeup_reason(void)
{
  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  if (ra8_reset_get_cause(&cause) != k_ra8_ok) {
    return (int)HOSTED_WAKEUP_UNDEFINED;
  }
  if (cause == k_ra8_reset_cause_deep_sw_standby) {
    return (int)HOSTED_WAKEUP_DEEP_SLEEP;
  }
  return (int)HOSTED_WAKEUP_NORMAL_REBOOT;
}

RA8_PRIV ra8_err_t priv_ra8_esp_hosted_gpio_bind(hosted_osi_funcs_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "vtable must not be nullptr");
  out->_h_config_gpio                      = internal_config_gpio;
  out->_h_config_gpio_as_interrupt         = internal_config_gpio_as_interrupt;
  out->_h_teardown_gpio_interrupt          = internal_teardown_gpio_interrupt;
  out->_h_read_gpio                        = internal_read_gpio;
  out->_h_write_gpio                       = internal_write_gpio;
  out->_h_pull_gpio                        = internal_pull_gpio;
  out->_h_hold_gpio                        = internal_hold_gpio;
  out->_h_get_host_wakeup_or_reboot_reason = internal_wakeup_reason;
  return (out->_h_read_gpio != nullptr) ? k_ra8_ok : k_ra8_err_invalid_state;
}
