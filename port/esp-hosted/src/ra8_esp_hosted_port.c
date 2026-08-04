/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/src/ra8_esp_hosted_port.c
 * @brief Bring-up and teardown of the esp-hosted RA8D2 + ThreadX port.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Sequences the port's four slices and unwinds them in the opposite
 * order. The order is not arbitrary:
 *
 *   1. **Validate first, touch nothing.** The configuration and the
 *      compiled-in pin map are checked before any hardware is claimed, so
 *      a mistyped clock or a duplicated pin assignment fails with no pin
 *      stranded and no channel half-open.
 *   2. **RTOS pools.** Everything after this point may need to allocate,
 *      and on this board allocation means the port's fixed ThreadX byte
 *      pool -- there is no heap.
 *   3. **The vtable.** Bound before the pins, because the pins are
 *      configured *through* it: doing it that way exercises the same rows
 *      the vendored driver will use, so a binding mistake surfaces during
 *      bring-up rather than on the first frame.
 *   4. **The SPI channel**, then the two side-band inputs.
 *
 * Teardown walks back up, and every step runs even if an earlier one
 * reports a problem -- a teardown that stops at the first error leaves
 * exactly the stranded pin it was supposed to release.
 *
 * @since 0.1.0
 */

#include "ra8_esp_hosted_port.h"

#include <stdint.h>

#include "esp_hosted_os_abstraction.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_log.h"
#include "port_esp_hosted_host_os.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_esp_hosted_gpio_internal.h"
#include "ra8_esp_hosted_osi_internal.h"
#include "ra8_esp_hosted_pins.h"
#include "ra8_esp_hosted_port_internal.h"
#include "ra8_esp_hosted_rtos_internal.h"
#include "ra8_esp_hosted_spi_internal.h"
#include "ra8_port_constants.h"

DEFINE_LOG_TAG(port);

/**
 * @enum ra8_esp_hosted_port_limit_t
 * @brief Bounds the bring-up path validates against.
 *
 * @details
 * Named so each rejection reads as a contract violation rather than a
 * comparison against a bare number.
 *
 * @invariant ::k_ra8_esp_hosted_port_sci_channels matches the number of
 *            SCI channels the RA8D2 provides.
 *
 * @par Example:
 * @code
 * if (cfg->sci_channel >= (uint8_t)k_ra8_esp_hosted_port_sci_channels) { reject(); }
 * @endcode
 *
 * @see ra8_esp_hosted_port_cfg_check
 * @since 0.1.0
 */
typedef enum : uint8_t {
  /** SCI channels on this part; a channel index must be below this. */
  k_ra8_esp_hosted_port_sci_channels = 10U,
} ra8_esp_hosted_port_limit_t;

/**
 * @var s_ra8_esp_hosted_port_ready
 * @brief Whether the port has completed bring-up and not been torn down.
 * @details The single piece of module state. Set only after every slice
 * has reported success, so a partially-initialised port never reports
 * ready.
 * @note Read by ::ra8_esp_hosted_port_is_ready and by the vtable's init
 * hook.
 * @warning Do not set it directly; the bring-up path owns it.
 * @since 0.1.0
 */
static bool s_ra8_esp_hosted_port_ready;

/** @brief Implementation of `ra8_esp_hosted_port_cfg_check()` -- a pure
 *  predicate, so bring-up can reject a configuration before claiming
 *  anything. */
RA8_PRIV
ra8_err_t ra8_esp_hosted_port_cfg_check(const ra8_esp_hosted_port_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, TAG, "cfg");

  if (cfg->pclk_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->sck_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->edge_poll_ms == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->sci_channel >= (uint8_t)k_ra8_esp_hosted_port_sci_channels) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Whether a packed pin code decodes to a legal port and pin index.
 *
 * @details
 * ``k_ra8_pin_none`` decodes to port 255, pin 255, so the range test
 * rejects it without needing a separate comparison against the sentinel.
 *
 * @param[in] pin Packed ``RA8_PIN(port, pin)`` code.
 *
 * @return Whether both halves are within range.
 * @retval true The port is at most ``k_ra8_port_max`` and the pin at most
 *         ``k_ra8_pin_max``.
 * @retval false Either half is out of range, including the no-pin
 *         sentinel.
 *
 * @pre The value is a packed pin code, not a raw port or pin index.
 * @pre The caller treats the no-pin sentinel as a rejection.
 * @post No state is modified.
 * @post The answer depends only on ``pin``.
 *
 * @note Reentrant; a pure predicate.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_pin_in_range(uint16_t pin)
{
  return ((uint16_t)RA8_PIN_PORT(pin) <= (uint16_t)k_ra8_port_max) &&
         ((uint16_t)RA8_PIN_PIN(pin) <= (uint16_t)k_ra8_pin_max);
}

/** @brief Implementation of `ra8_esp_hosted_port_pins_check_values()` --
 *  distinctness first, then range, so a duplicated assignment is reported
 *  as such rather than as an out-of-range pin. */
RA8_PRIV
ra8_err_t
ra8_esp_hosted_port_pins_check_values(uint16_t chip_select, uint16_t handshake, uint16_t data_ready)
{
  if ((chip_select == handshake) || (chip_select == data_ready) || (handshake == data_ready)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_pin_in_range(chip_select)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_pin_in_range(handshake)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_pin_in_range(data_ready)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_esp_hosted_port_pins_check()` -- applies
 *  the value check to the compiled-in map. */
RA8_PRIV
ra8_err_t ra8_esp_hosted_port_pins_check(void)
{
  return ra8_esp_hosted_port_pins_check_values((uint16_t)k_ra8_esp_hosted_pin_chip_select,
                                               (uint16_t)k_ra8_esp_hosted_pin_handshake,
                                               (uint16_t)k_ra8_esp_hosted_pin_data_ready);
}

/**
 * @brief Configure the two side-band nets as digital inputs.
 *
 * @details
 * Goes through the freshly-bound vtable rather than calling the GPIO HAL
 * directly, so bring-up exercises the same rows the vendored driver will
 * use. A row bound to the wrong function therefore fails here, where the
 * error message names the port, instead of on the first frame.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Both pins are inputs.
 * @retval k_ra8_err_gpio_conflict A pin is owned by another module, or a
 *         vtable row rejected the request.
 *
 * @pre The vtable has been bound.
 * @pre Neither pin is currently claimed by another module.
 * @post On success both pins are configured as inputs.
 * @post On failure neither pin is left half-configured by this call.
 *
 * @note Not thread-safe; part of the single-threaded bring-up path.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_configure_sideband(void)
{
  if (g_h.funcs->_h_config_gpio(H_GPIO_HANDSHAKE_Port,
                                (uint32_t)H_GPIO_HANDSHAKE_Pin,
                                H_GPIO_MODE_DEF_INPUT) != RET_OK) {
    ESP_LOGE(TAG, "handshake pin would not configure as an input");
    return k_ra8_err_gpio_conflict;
  }
  if (g_h.funcs->_h_config_gpio(H_GPIO_DATA_READY_Port,
                                (uint32_t)H_GPIO_DATA_READY_Pin,
                                H_GPIO_MODE_DEF_INPUT) != RET_OK) {
    ESP_LOGE(TAG, "data-ready pin would not configure as an input");
    return k_ra8_err_gpio_conflict;
  }
  return k_ra8_ok;
}

/**
 * @brief Release everything the bring-up path may have claimed.
 *
 * @details
 * Runs every step regardless of what the previous one reported, because a
 * teardown that stops at the first error strands exactly the resource it
 * was meant to release. The first error seen is what it returns.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every step released cleanly.
 * @retval k_ra8_err_rtos_error ThreadX refused to release an object.
 * @retval k_ra8_err_spi_error The SCI channel would not close.
 *
 * @pre No interrupt handler for this port is currently executing.
 * @pre The vendored transport has been stopped.
 * @post Every step has been attempted exactly once.
 * @post The module reports not ready.
 *
 * @note Not thread-safe; part of the single-threaded teardown path.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_unwind(void)
{
  /* Both release steps run before either result is folded, so neither can be
     skipped by an earlier failure. The fold itself lives in
     `ra8_esp_hosted_port_first_error()` because it is the only decision here
     and reaching this function needs a brought-up port. */
  const ra8_err_t spi_err  = ra8_esp_hosted_spi_close();
  ra8_err_t       first    = ra8_esp_hosted_port_first_error(k_ra8_ok, spi_err);
  const ra8_err_t rtos_err = ra8_esp_hosted_rtos_deinit();
  first                    = ra8_esp_hosted_port_first_error(first, rtos_err);

  g_hosted_osi_funcs          = (hosted_osi_funcs_t){};
  s_ra8_esp_hosted_port_ready = false;
  return first;
}

/** @brief Implementation of `ra8_esp_hosted_port_first_error()` -- a pure
 *  fold, so the unwind path carries no decision of its own. */
RA8_PRIV
ra8_err_t ra8_esp_hosted_port_first_error(ra8_err_t first, ra8_err_t next)
{
  if ((first == k_ra8_ok) && (next != k_ra8_ok)) {
    return next;
  }
  return first;
}

/**
 * @brief Bring the port's slices up in dependency order.
 *
 * @details
 * The second half of ::ra8_esp_hosted_port_init, split out so validation and
 * bring-up are separately readable and each stays inside the project's
 * function-length limit. Every step depends on the one before it: the RTOS
 * substrate must exist before the vtable can be bound, the vtable must be
 * bound before the edge-poll period or the side-band pins can be reached
 * through it, and the bus must be open before the co-processor is spoken to.
 *
 * The RTOS init is separate from the chain below it because it is the only
 * step whose failure leaves nothing to release: the ``err == k_ra8_ok``
 * ladder starts once there is state a caller would have to unwind.
 *
 * The readiness of the port is not re-tested here; ::ra8_esp_hosted_port_init
 * establishes it before calling, and a second test would be a branch no
 * caller can reach.
 *
 * @param[in] cfg Validated configuration; must be non-null and already
 *                accepted by ``ra8_esp_hosted_port_cfg_check``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every slice came up.
 * @retval k_ra8_err_null_ptr @p cfg was null.
 * @retval k_ra8_err_rtos_error The ThreadX substrate would not initialise.
 * @retval k_ra8_err_spi_error The SCI channel would not open.
 * @retval k_ra8_err_gpio_conflict A side-band pin would not configure.
 *
 * @pre @p cfg has already passed ``ra8_esp_hosted_port_cfg_check``.
 * @pre The port is not already up, which the caller has established.
 * @post On success every slice is up and the vtable is fully bound.
 * @post On failure nothing this call claimed is left claimed; the caller
 *       unwinds and the module stays not-ready.
 *
 * @note Not thread-safe; part of the single-threaded bring-up path.
 * @see ra8_esp_hosted_port_init
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_bring_up(const ra8_esp_hosted_port_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, TAG, "cfg");

  ra8_err_t err = ra8_esp_hosted_rtos_init();
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_esp_hosted_osi_bind_all(&g_hosted_osi_funcs);
  if (err == k_ra8_ok) {
    err = ra8_esp_hosted_gpio_set_edge_poll_ms(cfg->edge_poll_ms);
  }
  if (err == k_ra8_ok) {
    err = ra8_esp_hosted_spi_open(cfg->sci_channel, cfg->pclk_hz, cfg->sck_hz);
  }
  if (err == k_ra8_ok) {
    err = internal_configure_sideband();
  }
  return err;
}

/** @brief Implementation of `ra8_esp_hosted_port_init()` -- validate,
 *  then bring the slices up in dependency order, unwinding on the first
 *  failure. */
ra8_err_t ra8_esp_hosted_port_init(const ra8_esp_hosted_port_cfg_t* cfg)
{
  const ra8_err_t cfg_err = ra8_esp_hosted_port_cfg_check(cfg);
  if (cfg_err != k_ra8_ok) {
    return cfg_err;
  }
  if (s_ra8_esp_hosted_port_ready) {
    return k_ra8_err_invalid_state;
  }

  const ra8_err_t pin_err = ra8_esp_hosted_port_pins_check();
  if (pin_err != k_ra8_ok) {
    ESP_LOGE(TAG, "pin map is not self-consistent");
    return pin_err;
  }

  const ra8_err_t err = internal_bring_up(cfg);
  if (err != k_ra8_ok) {
    (void)internal_unwind();
    return err;
  }

  s_ra8_esp_hosted_port_ready = true;
  ESP_LOGI(TAG,
           "esp-hosted port up on SCI%u at %u Hz",
           (unsigned int)cfg->sci_channel,
           (unsigned int)cfg->sck_hz);
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_esp_hosted_port_deinit()` -- refuses when
 *  the port was never up, so a double teardown is visible rather than
 *  silently accepted. */
ra8_err_t ra8_esp_hosted_port_deinit(void)
{
  if (!s_ra8_esp_hosted_port_ready) {
    return k_ra8_err_not_initialized;
  }
  return internal_unwind();
}

/** @brief Implementation of `ra8_esp_hosted_port_is_ready()` -- one
 *  aligned load, so it is safe from any context. */
bool ra8_esp_hosted_port_is_ready(void)
{
  return s_ra8_esp_hosted_port_ready;
}
