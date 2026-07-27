/**
 * @file port/esp-hosted/src/ra8_esp_hosted_spi_internal.h
 * @brief Module-private surface of the esp-hosted full-duplex SPI transport.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vendored transport reaches the wire through three slots of
 * ``hosted_osi_funcs_t`` -- ``_h_bus_init``, ``_h_bus_deinit`` and
 * ``_h_do_bus_transfer`` -- and nothing else. This header declares the
 * first-party side: the slot binder, the two port-facing entry points that
 * open and close the SCI Simple-SPI channel, and the two dependency-injection
 * seams that make the transfer path testable on a host with no silicon.
 *
 * @par The link this transport drives
 * One 1600-byte full-duplex frame per transaction, mode 3, MSB first, with
 * the chip select held low for the whole frame. Mode 3 is not a preference:
 * the co-processor image in ``coprocessor/esp32c6/`` is built
 * ``CONFIG_ESP_SPI_MODE=3``, and ::k_ra8_esp_hosted_spi_mode is asserted
 * against ``k_ra8_spi_mode_3`` so the two cannot drift apart silently.
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
#include "ra8_io_spi_bus.h"
#include "ra8_pin_interface.h"

/**
 * @enum ra8_esp_hosted_spi_limits_t
 * @brief Fixed facts about the wire this transport drives.
 *
 * @details
 * Both ends of the link agree these values out of band -- the co-processor
 * image is built with them -- so they are stated once here and asserted
 * against the HAL's own encodings in the implementation.
 *
 * @invariant ::k_ra8_esp_hosted_spi_mode equals ``k_ra8_spi_mode_3``.
 * @invariant ::k_ra8_esp_hosted_spi_channel_max bounds every channel index
 *            the SCI Simple-SPI driver accepts.
 *
 * @par Example:
 * @code
 * static_assert(k_ra8_esp_hosted_spi_mode == 3U, "C6 is built CONFIG_ESP_SPI_MODE=3");
 * @endcode
 *
 * @see ra8_esp_hosted_spi_open
 * @since 0.1.0
 */
typedef enum : uint8_t {
  /** Clock polarity/phase both high: the C6 image's ``CONFIG_ESP_SPI_MODE``. */
  k_ra8_esp_hosted_spi_mode = 3U,
  /** One past the highest SCI channel index the HAL accepts. */
  k_ra8_esp_hosted_spi_channel_max = 10U,
} ra8_esp_hosted_spi_limits_t;

/**
 * @brief Populate the three transport slots of the OS-abstraction vtable.
 *
 * @details
 * Writes ``_h_bus_init``, ``_h_bus_deinit`` and ``_h_do_bus_transfer`` into
 * @p out and touches nothing else, so the GPIO and RTOS slices may fill
 * theirs in any order relative to this call.
 *
 * @param[out] out Vtable to populate; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The three rows are populated.
 * @retval k_ra8_err_null_ptr @p out was null.
 *
 * @pre @p out points at storage that out-lives the vendored core.
 * @pre ::ra8_esp_hosted_spi_open has run, or will run before the vendored
 *      transport is started.
 * @post The three transport rows of @p out are non-null.
 * @post No non-transport row of @p out is modified.
 *
 * @note Not thread-safe; call once from the port's bring-up path.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_spi_bind(&g_hosted_osi_funcs);
 * @endcode
 *
 * @see ra8_esp_hosted_gpio_bind
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_esp_hosted_spi_bind(hosted_osi_funcs_t* out);

/**
 * @brief Route the Pmod1 SPI pins and open the SCI Simple-SPI channel.
 *
 * @details
 * Performs, in order: route SCK, COPI and CIPO to the SCI peripheral through
 * the PFS; take the chip select as a GPIO output idling high (deasserted);
 * bring the SCI channel up at mode 3, MSB first, at the requested bit rate;
 * and bind the channel into the ``ra8_io`` SPI-bus facade so the transfer
 * slot can drive it. Any failure unwinds the steps already taken, so a failed
 * open leaves no pin claimed.
 *
 * The channel argument must name the Pmod1 Simple-SPI channel. The port
 * routes the Pmod1 pin trio and nothing else, so accepting a different
 * channel would clock a peripheral whose pins were never routed.
 *
 * @param[in] sci_channel SCI channel carrying Simple-SPI; must equal the
 *                        board's Pmod1 channel.
 * @param[in] pclk_hz Live PCLKA rate in hertz; non-zero.
 * @param[in] sck_hz Requested bit rate in hertz; non-zero.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The bus is open and bound.
 * @retval k_ra8_err_invalid_arg A clock was zero, or the channel is not the
 *         Pmod1 one.
 * @retval k_ra8_err_invalid_state The bus is already open.
 * @retval k_ra8_err_gpio_conflict A link pin is owned by another module.
 * @retval k_ra8_err_spi_error The SCI channel would not come up.
 *
 * @pre The CGC is configured and @p pclk_hz reflects the live PCLKA rate.
 * @pre The Pmod1 mode mux is in its SPI position (board switch SW4-3 on).
 * @post On success the chip select idles high and the channel is enabled.
 * @post On failure no pin is left claimed and the channel is left disabled.
 *
 * @note Not thread-safe; call once from the port's bring-up path.
 * @warning Opening the bus does not talk to the co-processor; the first
 *          traffic is whatever the vendored transport clocks out.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_spi_open(cfg->sci_channel, cfg->pclk_hz, cfg->sck_hz);
 * @endcode
 *
 * @see ra8_esp_hosted_spi_close
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t
ra8_esp_hosted_spi_open(uint8_t sci_channel, uint32_t pclk_hz, uint32_t sck_hz);

/**
 * @brief Close the SCI Simple-SPI channel and release every pin it took.
 *
 * @details
 * Exact reverse of ::ra8_esp_hosted_spi_open: the channel is disabled and its
 * module-stop gate released, then the chip select and the three routed data
 * pins are handed back to the pin validator. The bound bus handle is cleared
 * so a later transfer attempt fails rather than driving a dead channel.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Everything was released.
 * @retval k_ra8_err_not_initialized The bus was not open.
 * @retval k_ra8_err_spi_error The HAL refused to disable the channel.
 *
 * @pre The vendored transport has stopped clocking frames.
 * @pre No transfer is in flight.
 * @post Every pin ::ra8_esp_hosted_spi_open claimed is released.
 * @post A later ``_h_do_bus_transfer`` reports failure rather than clocking.
 *
 * @note Not thread-safe; call from the same context as the open.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_spi_close();
 * @endcode
 *
 * @see ra8_esp_hosted_spi_open
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_esp_hosted_spi_close(void);

/**
 * @brief Report whether the SCI Simple-SPI channel is currently open.
 *
 * @details
 * Reads the single module-state flag, so the port can decide whether a close
 * is needed and tests can assert the open/close state machine without
 * reaching into the module.
 *
 * @return Whether ::ra8_esp_hosted_spi_open has completed with no close
 *         since.
 * @retval true The channel is open and bound.
 * @retval false The channel was never opened, failed to open, or was closed.
 *
 * @pre None; safe to call at any time.
 * @pre The caller tolerates a value a concurrent close may stale.
 * @post No module state is modified.
 * @post The result reflects the flag at the moment of the read.
 *
 * @note Safe from interrupt context; a single aligned load.
 *
 * @par Example:
 * @code
 * if (ra8_esp_hosted_spi_is_open()) { (void)ra8_esp_hosted_spi_close(); }
 * @endcode
 *
 * @see ra8_esp_hosted_spi_open
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] bool ra8_esp_hosted_spi_is_open(void);

/**
 * @brief Replace the SPI bus the transfer slot clocks frames through.
 *
 * @details
 * Dependency-injection seam. Production leaves it unset, in which case the
 * slot uses the handle ::ra8_esp_hosted_spi_open bound to the SCI channel.
 * Host tests point it at a recording ``ra8_io_spi_bus_iface`` so the whole
 * transfer path -- argument validation, chip-select sequencing, error
 * propagation -- runs with no silicon and no open channel.
 *
 * @param[in] bus Replacement bus handle, or null to fall back on the handle
 *                the open bound. Must out-live every later transfer.
 *
 *
 * @pre @p bus, when non-null, has a bound backend vtable.
 * @pre No transfer is in flight.
 * @post Later transfers clock through @p bus.
 * @post Passing null restores the internally bound handle.
 *
 * @note Not thread-safe; intended for bring-up and for tests.
 * @warning Injecting a bus does not open a channel; the two are independent.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_spi_set_bus(&mock_bus);
 * @endcode
 *
 * @see ra8_esp_hosted_spi_open
 * @since 0.1.0
 */
RA8_PRIV RA8_DI_SLOT("spi_bus") void ra8_esp_hosted_spi_set_bus(const ra8_io_spi_bus_t* bus);

/**
 * @brief Replace the pin driver the transfer slot drives chip select with.
 *
 * @details
 * Dependency-injection seam, separate from the GPIO slice's own seam because
 * the two translation units are independent. Production leaves it at
 * ``g_ra8_gpio_pin_interface``; tests point it at a recorder so the order of
 * "chip select low, frame, chip select high" can be asserted rather than
 * assumed.
 *
 * @param[in] iface Replacement interface, or null to restore the production
 *                  instance. Must out-live every later transfer.
 *
 *
 * @pre @p iface, when non-null, has non-null ``output_init`` and ``write``
 *      rows.
 * @pre No transfer is in flight.
 * @post Later chip-select transitions go through @p iface.
 * @post Passing null restores the production pin driver.
 *
 * @note Not thread-safe; intended for bring-up and for tests.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_spi_set_pin_interface(&mock_pin_iface);
 * @endcode
 *
 * @see ra8_esp_hosted_spi_set_bus
 * @since 0.1.0
 */
RA8_PRIV RA8_DI_SLOT("chip_select") void ra8_esp_hosted_spi_set_pin_interface(
  const ra8_pin_interface_t* iface);
