/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/src/ra8_esp_hosted_spi.c
 * @brief The three transport slots of the esp-hosted OS-abstraction vtable.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * One esp-hosted transaction is one 1600-byte full-duplex SPI frame: the host
 * clocks a transmit buffer out while the co-processor clocks its own frame
 * back, and both directions complete or neither does. That is exactly the
 * shape of ``ra8_io_spi_bus_write_read``, so the transfer slot is a single
 * call to it wrapped in chip-select sequencing.
 *
 * @par Chip-select timing
 * The C6 image is built ``CONFIG_ESP_SPI_DEASSERT_HS_ON_CS``, so it drops
 * HANDSHAKE the moment the chip select falls: the falling edge is the event
 * that starts the transaction on the co-processor side, and the frame must
 * follow it. No artificial setup delay is inserted -- the pin write, the
 * bus-handle check and the SCI transmit-data-register write already separate
 * the two by tens of nanoseconds on a 1 GHz Cortex-M85, which is far more
 * than the peripheral needs. The hold is structural rather than timed:
 * ``ra8_io_spi_bus_write_read`` polls each frame to completion on the SCI
 * Simple-SPI backend, so the last clock edge has retired before the call
 * returns and therefore before the chip select rises.
 *
 * @since 0.1.0
 */

#include <stdint.h>

/*
 * ``port_esp_hosted_host_config.h`` is deliberately NOT named here even
 * though this file depends on it. It defines H_ESP_PAYLOAD_HEADER_OFFSET
 * under an #ifndef guard, while the vendored ``esp_hosted_header.h`` defines
 * the same macro unconditionally from a sizeof; whichever is reached first
 * wins and the other is skipped. ``transport_drv.h`` pulls them in that
 * order. Naming the config header here would sort it ahead of
 * ``transport_drv.h`` and turn the arrangement into a redefinition warning.
 */
#include "port_esp_hosted_host_os.h"
#include "port_esp_hosted_host_spi.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_pins.h"
#include "ra8_esp_hosted_spi_internal.h"
#include "ra8_gpio_constants.h"
#include "ra8_io_spi_bus.h"
#include "ra8_io_spi_bus_sci_spi.h"
#include "ra8_log.h"
#include "ra8_pin_interface.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci_spi.h"
#include "ra8_spi.h"
#include "transport_drv.h"

/**
 * @var s_tag
 * @brief Log tag identifying lines emitted by the SPI transport slice.
 * @details Distinct from the vendored driver's own ``spi`` tag so a reader
 * can tell a port-side refusal from a transport-side one.
 * @note Read-only after load.
 * @warning Changing it breaks log filters that key on the string.
 * @since 0.1.0
 */
static const char* const s_tag = "eh_spi";

/**
 * @enum ra8_esp_hosted_spi_frame_t
 * @brief The frame bound this port supplies, as a named constant.
 *
 * @details
 * ``MAX_TRANSPORT_BUFFER_SIZE`` is whichever value ``transport_drv.h`` picked
 * up from the port header its transport-selection chain chose. Binding it to
 * an enumerator here gives the assertion below a symbol to compare against
 * the constant the vendored SPI driver actually clocks, instead of comparing
 * one spelling of a macro with another spelling of the same macro.
 *
 * @invariant ::k_ra8_esp_hosted_spi_frame_bytes equals ``MAX_SPI_BUFFER_SIZE``
 *            -- asserted immediately below, because the two coming apart
 *            means this port and the driver would clock different frames.
 *
 * @par Example:
 * @code
 * static uint8_t frame[k_ra8_esp_hosted_spi_frame_bytes];
 * @endcode
 *
 * @see ra8_esp_hosted_spi_transfer
 * @since 0.1.0
 */
typedef enum : uint16_t {
  /** Bytes clocked in one full-duplex transaction, from the port header. */
  k_ra8_esp_hosted_spi_frame_bytes = (uint16_t)MAX_TRANSPORT_BUFFER_SIZE,
} ra8_esp_hosted_spi_frame_t;

static_assert((uint8_t)k_ra8_esp_hosted_spi_mode == (uint8_t)k_ra8_spi_mode_3,
              "the co-processor image is built CONFIG_ESP_SPI_MODE=3");
/*
 * Not a tautology: the left operand is the bound reached through
 * ``MAX_TRANSPORT_BUFFER_SIZE``, which ``transport_drv.h`` resolves through
 * whichever port header its ``H_TRANSPORT_IN_USE`` chain selected, while the
 * right is the constant the vendored ``spi_drv.c`` clocks. Selecting a
 * different transport, or editing this port's SPI header to a hand-written
 * number, makes them disagree and stops the build here rather than on the
 * wire.
 */
static_assert(k_ra8_esp_hosted_spi_frame_bytes == MAX_SPI_BUFFER_SIZE,
              "the port's frame bound must be the one the vendored SPI driver clocks");

/**
 * @var s_bus
 * @brief Bus handle bound to the SCI Simple-SPI channel by the open.
 * @details Caller-allocated by contract of ``ra8_io_spi_bus_t``, so the
 * storage lives here for the lifetime of the image; ``iface`` is null until
 * an open binds a backend into it.
 * @note Written only by ::ra8_esp_hosted_spi_open and
 *       ::ra8_esp_hosted_spi_close.
 * @warning Also handed to the vendored driver as its opaque bus handle; do
 *          not move it.
 * @since 0.1.0
 */
static ra8_io_spi_bus_t s_bus;

/**
 * @var s_injected_bus
 * @brief Test-injected replacement for ::s_bus, or null in production.
 * @details Kept separate from ::s_bus so an injection survives an open and an
 * open does not clobber an injection.
 * @note Written only by ::ra8_esp_hosted_spi_set_bus.
 * @warning Pointing this at a short-lived object leaves the transfer slot
 *          clocking through a dangling vtable.
 * @since 0.1.0
 */
static const ra8_io_spi_bus_t* s_injected_bus;

/**
 * @var s_pin_if
 * @brief Pin driver the chip select is configured and driven through.
 * @details Null means "use the production instance", which keeps the seam
 * usable before any initialisation has run.
 * @note Written only by ::ra8_esp_hosted_spi_set_pin_interface.
 * @warning Swapping it mid-transfer would split one frame's chip-select
 *          edges across two drivers.
 * @since 0.1.0
 */
static const ra8_pin_interface_t* s_pin_if;

/**
 * @var s_channel
 * @brief SCI channel the open brought up, meaningful only while ::s_open.
 * @details Remembered so the close can disable exactly the channel that was
 * enabled, without the caller having to repeat it.
 * @note Written only by ::ra8_esp_hosted_spi_open.
 * @warning Reading it while ::s_open is false yields a stale channel index.
 * @since 0.1.0
 */
static uint8_t s_channel;

/**
 * @var s_open
 * @brief True between a successful open and its matching close.
 * @details The single piece of state the open/close pair maintains; the
 * transfer slot deliberately does not consult it, because an injected bus
 * makes a transfer meaningful with no channel open at all.
 * @note Written only by ::ra8_esp_hosted_spi_open and
 *       ::ra8_esp_hosted_spi_close.
 * @warning Never set it without the channel actually being enabled.
 * @since 0.1.0
 */
static bool s_open;

/*
 * The production pin driver is defined in libs/ra8_hal/src/gpio.c and, like
 * every other consumer of it in this tree, is reached by declaration rather
 * than through a header -- ra8_pin_interface.h describes the type, not the
 * instance.
 */
extern const ra8_pin_interface_t g_ra8_gpio_pin_interface;

/**
 * @brief Report the pin driver the chip select is driven through.
 *
 * @details
 * Folds the "nothing injected" case onto the production instance so every
 * caller can use the result without a null test.
 *
 * @return The installed pin interface.
 * @retval non-null Always.
 *
 * @pre The slice is linked against ``libs/ra8_hal``.
 * @pre The caller does not retain the pointer across a seam swap.
 * @post No module state is modified.
 * @post The result has non-null ``output_init`` and ``write`` rows.
 *
 * @note Safe from interrupt context; a single aligned load.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static const ra8_pin_interface_t* internal_pin_if(void)
{
  return (s_pin_if != nullptr) ? s_pin_if : &g_ra8_gpio_pin_interface;
}

/**
 * @brief Report the bus the transfer slot should clock through.
 *
 * @details
 * An injected bus wins over the internally bound one, so a test can drive the
 * transfer path with no channel open. When neither is available the result is
 * null and the caller reports failure rather than clocking a dead channel.
 *
 * @return The bus to use, or null when none is usable.
 * @retval non-null A backend vtable is bound into the returned handle.
 * @retval nullptr Nothing is injected and no open has bound a backend.
 *
 * @pre An injected handle, when present, out-lives the call.
 * @pre The caller treats null as "cannot transfer".
 * @post No module state is modified.
 * @post A non-null result always has a bound ``iface``.
 *
 * @note Safe from interrupt context; two aligned loads.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static const ra8_io_spi_bus_t* internal_bus(void)
{
  if (s_injected_bus != nullptr) {
    return s_injected_bus;
  }
  return (s_bus.iface != nullptr) ? &s_bus : nullptr;
}

/**
 * @brief Route the three Pmod1 SPI data pins to the SCI peripheral.
 *
 * @details
 * The chip select is deliberately not routed here: esp-hosted needs it driven
 * by software for the whole frame, so it stays a GPIO output. The three data
 * pins take the SCI peripheral function, which the Simple-SPI mode of the SCI
 * shares with its asynchronous mode.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok All three pins are routed.
 * @retval k_ra8_err_gpio_conflict A pin is owned by another module.
 * @retval k_ra8_err_hw_unmapped The PFS window for a pin is not mapped.
 *
 * @pre The Pmod1 mode mux is in its SPI position (board switch SW4-3 on).
 * @pre The IOPORT module is powered.
 * @post On success the three data pins carry the SCI function.
 * @post On failure the caller releases whatever was routed.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_route_data_pins(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_pmod1_spi_sck,
                                           k_ra8_psel_sci_async,
                                           "esp_hosted.sck");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_pmod1_spi_copi,
                                 k_ra8_psel_sci_async,
                                 "esp_hosted.copi");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_pmod1_spi_cipo,
                                  k_ra8_psel_sci_async,
                                  "esp_hosted.cipo");
}

/**
 * @brief Hand every pin the transport claimed back to the pin validator.
 *
 * @details
 * Used both by the failure unwind inside the open and by the close, so the
 * two cannot disagree about which pins the transport owns. Release failures
 * are ignored on purpose: a pin that was never claimed is exactly the case
 * the unwind has to tolerate.
 *
 *
 * @pre No transfer is in flight.
 * @pre The SCI channel has already been disabled, or was never enabled.
 * @post None of the four link pins is claimed by this module.
 * @post The call is idempotent; a second one changes nothing.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_release_pins(void)
{
  (void)ra8_gpio_release((ra8_port_pin_t)k_ra8_board_pmod1_spi_sck);
  (void)ra8_gpio_release((ra8_port_pin_t)k_ra8_board_pmod1_spi_copi);
  (void)ra8_gpio_release((ra8_port_pin_t)k_ra8_board_pmod1_spi_cipo);
  (void)ra8_gpio_release((ra8_port_pin_t)k_ra8_esp_hosted_pin_chip_select);
}

RA8_PRIV ra8_err_t ra8_esp_hosted_spi_open(uint8_t sci_channel, uint32_t pclk_hz, uint32_t sck_hz)
{
  if (s_open) {
    return k_ra8_err_invalid_state;
  }
  if ((pclk_hz == 0U) || (sck_hz == 0U) ||
      (sci_channel != (uint8_t)k_ra8_board_pmod1_sci_channel)) {
    return k_ra8_err_invalid_arg;
  }

  ra8_err_t err = internal_route_data_pins();
  if (err == k_ra8_ok) {
    const ra8_pin_interface_t* pin_if = internal_pin_if();
    err = pin_if->output_init(pin_if->ctx,
                              (ra8_port_pin_t)k_ra8_esp_hosted_pin_chip_select,
                              k_ra8_level_high);
  }
  if (err != k_ra8_ok) {
    internal_release_pins();
    ra8_log_error_val(s_tag, "pin setup failed", (uint32_t)err);
    return err;
  }

  const ra8_sci_spi_cfg_t cfg = {
    .baud_hz   = sck_hz,
    .pclk_hz   = pclk_hz,
    .mode      = (ra8_spi_mode_t)k_ra8_esp_hosted_spi_mode,
    .lsb_first = false,
  };
  err = ra8_sci_spi_init(sci_channel, &cfg);
  if (err == k_ra8_ok) {
    err = ra8_io_spi_bus_bind_sci_spi(&s_bus, sci_channel);
  }
  if (err != k_ra8_ok) {
    (void)ra8_sci_spi_deinit(sci_channel);
    internal_release_pins();
    s_bus = (ra8_io_spi_bus_t){};
    ra8_log_error_val(s_tag, "sci simple-spi open failed", (uint32_t)err);
    return k_ra8_err_spi_error;
  }

  s_channel = sci_channel;
  s_open    = true;
  ra8_log_info_val(s_tag, "bus open on sci channel", (uint32_t)sci_channel);
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t ra8_esp_hosted_spi_close(void)
{
  if (!s_open) {
    return k_ra8_err_not_initialized;
  }
  const ra8_err_t err = ra8_sci_spi_deinit(s_channel);
  internal_release_pins();
  s_bus  = (ra8_io_spi_bus_t){};
  s_open = false;
  return (err == k_ra8_ok) ? k_ra8_ok : k_ra8_err_spi_error;
}

RA8_PRIV bool ra8_esp_hosted_spi_is_open(void)
{
  return s_open;
}

RA8_PRIV void ra8_esp_hosted_spi_set_bus(const ra8_io_spi_bus_t* bus)
{
  s_injected_bus = bus;
}

RA8_PRIV void ra8_esp_hosted_spi_set_pin_interface(const ra8_pin_interface_t* iface)
{
  s_pin_if = iface;
}

/**
 * @brief ``_h_bus_init``: hand the vendored transport its opaque bus handle.
 *
 * @details
 * The channel itself is opened by the port's bring-up, not here: the vendored
 * driver has no clocking configuration to give and simply asserts that the
 * handle is non-null. Reporting null when no bus is usable is therefore the
 * honest answer -- it makes the transport refuse to start rather than clock a
 * channel that was never enabled.
 *
 * @return void* The opaque bus handle, or null.
 * @retval non-null A bus is bound and transfers can proceed.
 * @retval nullptr No open has run and nothing was injected.
 *
 * @pre ::ra8_esp_hosted_spi_open has run, or a bus was injected.
 * @pre The returned handle is only ever passed back to ``_h_bus_deinit``.
 * @post No module state is modified.
 * @post The result is null exactly when a transfer would fail.
 *
 * @note Not thread-safe with respect to the open and close pair.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void* internal_bus_init(void)
{
  if (internal_bus() == nullptr) {
    ra8_log_warn(s_tag, "bus init before the port opened the channel");
    return nullptr;
  }
  return &s_bus;
}

/**
 * @brief ``_h_bus_deinit``: close the channel behind an opaque bus handle.
 *
 * @details
 * Accepts only the handle ``_h_bus_init`` produced, so a stray pointer cannot
 * tear the transport down. A close on an already-closed bus reports failure
 * rather than success, which is what lets the port's own teardown tell "I
 * closed it" from "it was already gone".
 *
 * @param[in] bus_handle The handle ``_h_bus_init`` returned.
 *
 * @return int Vendored return code.
 * @retval RET_OK The channel was closed and its pins released.
 * @retval RET_INVALID @p bus_handle was null or not ours.
 * @retval RET_FAIL The bus was not open, or the HAL refused.
 *
 * @pre The vendored transport has stopped clocking frames.
 * @pre No transfer is in flight.
 * @post On RET_OK no link pin is claimed by this module.
 * @post An unrecognised handle changes nothing.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_bus_deinit(void* bus_handle)
{
  if (bus_handle == nullptr) {
    return RET_INVALID;
  }
  if (bus_handle != (void*)&s_bus) {
    return RET_INVALID;
  }
  return (ra8_esp_hosted_spi_close() == k_ra8_ok) ? RET_OK : RET_FAIL;
}

/**
 * @brief ``_h_do_bus_transfer``: clock one full-duplex esp-hosted frame.
 *
 * @details
 * Validates the context and both buffers, asserts the chip select, exchanges
 * the caller's frame in a single full-duplex transfer, deasserts the chip
 * select and reports what the bus did. The transmit size is the caller's --
 * the vendored driver sets it to ``MAX_SPI_BUFFER_SIZE`` -- but it is bounded
 * against ``MAX_TRANSPORT_BUFFER_SIZE`` here as well, because the receive
 * buffer is only ever that large and a longer transfer would overrun it.
 *
 * The chip select is deasserted on every path that asserted it, including the
 * failure path, so a bus error cannot leave the co-processor selected.
 *
 * @param[in,out] transfer_context A ``struct hosted_transport_context_t``
 *                with both buffers set and ``tx_buf_size`` in bytes.
 *
 * @return int Vendored return code.
 * @retval RET_OK The frame was exchanged; ``rx_buf`` holds the reply.
 * @retval RET_INVALID Null context, a null buffer, or a size of zero or more
 *         than one frame.
 * @retval RET_FAIL No bus is bound, the chip select would not move, or the
 *         bus reported an error.
 *
 * @pre Both buffers cover ``tx_buf_size`` bytes.
 * @pre The chip select was configured as an output by the open.
 * @post The chip select is high on every return path.
 * @post On RET_OK exactly ``tx_buf_size`` bytes were clocked each way.
 *
 * @note Not thread-safe; the vendored driver serialises callers on its own
 *       bus mutex.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_do_bus_transfer(void* transfer_context)
{
  struct hosted_transport_context_t* ctx = (struct hosted_transport_context_t*)transfer_context;
  if (ctx == nullptr) {
    return RET_INVALID;
  }
  if ((ctx->tx_buf == nullptr) || (ctx->rx_buf == nullptr)) {
    return RET_INVALID;
  }
  if ((ctx->tx_buf_size == 0U) || (ctx->tx_buf_size > (uint32_t)MAX_TRANSPORT_BUFFER_SIZE)) {
    return RET_INVALID;
  }
  const ra8_io_spi_bus_t* bus = internal_bus();
  if (bus == nullptr) {
    return RET_FAIL;
  }

  const ra8_pin_interface_t* pin_if = internal_pin_if();
  const ra8_port_pin_t       cs     = (ra8_port_pin_t)k_ra8_esp_hosted_pin_chip_select;
  if (pin_if->write(pin_if->ctx, cs, k_ra8_level_low) != k_ra8_ok) {
    return RET_FAIL;
  }
  const ra8_err_t err =
    ra8_io_spi_bus_write_read(bus, ctx->tx_buf, ctx->rx_buf, ctx->tx_buf_size, k_ra8_spi_width_8);
  const ra8_err_t cs_err = pin_if->write(pin_if->ctx, cs, k_ra8_level_high);
  if (err != k_ra8_ok) {
    ra8_log_error_val(s_tag, "frame transfer failed", (uint32_t)err);
    return RET_FAIL;
  }
  return (cs_err == k_ra8_ok) ? RET_OK : RET_FAIL;
}

RA8_PRIV ra8_err_t ra8_esp_hosted_spi_bind(hosted_osi_funcs_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "vtable must not be nullptr");
  out->_h_bus_init        = internal_bus_init;
  out->_h_bus_deinit      = internal_bus_deinit;
  out->_h_do_bus_transfer = internal_do_bus_transfer;
  return (out->_h_do_bus_transfer != nullptr) ? k_ra8_ok : k_ra8_err_invalid_state;
}
