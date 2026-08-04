/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_hosted_init/src/c6_hosted_report.c
 * @brief Banner, resolved pin map and side-band reporting for the bring-up.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Everything the application states about the link before it clocks a
 * single bit: the port identity and SPI parameters, the pin map the port
 * resolved, the interrupt path each side-band net actually gets, and the
 * live level of both side-band lines read through the OS-abstraction
 * vtable. The event handler lives here too, since what it does is report.
 *
 * @par Where the facts come from
 * Nothing here is asserted from memory. The bus pins come from
 * ``ra8_esp_hosted_pin_t``; the side-band pair and the reset pin are
 * printed in the exact ``H_GPIO_*_Port`` / ``H_GPIO_*_Pin`` encoding the
 * vendored driver consumes; the interrupt routing comes from
 * ``ra8_esp_hosted_pin_irq_num``, a pure table lookup, so a pin with no ICU
 * channel is reported as taking the port's software edge detector rather
 * than being assumed to raise an edge.
 *
 * @par Reading through the vtable
 * ``c6_hosted_report_sideband`` deliberately calls ``_h_read_gpio`` rather
 * than the GPIO HAL: that is the seam the vendored SPI driver uses, so a
 * level printed here is the level the driver would act on.
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "c6_hosted.h"
#include "esp_hosted_os_abstraction.h"
#include "port_esp_hosted_host_config.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_esp_hosted_pins.h"
#include "ra8_spi.h"

/**
 * @var s_c6_hosted_events
 * @brief Number of events ::c6_hosted_on_event has been handed.
 * @details Surfaced through ::c6_hosted_event_count and printed on every
 * heartbeat, so a scope-less bench can still see whether the co-processor
 * ever announced itself.
 * @note Incremented on the posting thread; a single 32-bit store.
 * @warning Diagnostic only, not an atomic counter.
 * @since 0.1.0
 */
static volatile uint32_t s_c6_hosted_events;

/**
 * @brief Print the interrupt path the port takes for one side-band pin.
 * @param[in] label Signal name, printed verbatim.
 * @param[in] pin   Packed side-band pin from the port's pin table.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p pin is one of the link's side-band nets.
 * @post One line naming either the ICU channel or the software edge
 *       detector was emitted.
 * @post No application state is modified.
 * @note The lookup is pure, so this reports the routing the port really
 *       uses rather than an assumption about the package.
 * @since 0.1.0
 */
static void c6_hosted_print_route(const char* label, ra8_esp_hosted_pin_t pin)
{
  const uint8_t irq = ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)pin);
  c6_hosted_puts("c6_hosted_init: ");
  c6_hosted_print_pin(label, pin);
  if (irq == (uint8_t)k_ra8_esp_hosted_irq_none) {
    c6_hosted_puts(" irq=none path=software-edge-detector poll_ms=");
    c6_hosted_put_u32((uint32_t)k_c6_hosted_edge_poll_ms);
  } else {
    c6_hosted_puts(" irq=");
    c6_hosted_put_u32((uint32_t)irq);
    c6_hosted_puts(" path=icu-edge");
  }
  c6_hosted_puts("\r\n");
}

/**
 * @brief Sample one side-band input through the OS-abstraction vtable.
 * @param[in] label     Signal name, printed verbatim.
 * @param[in] gpio_port Opaque port handle from the matching macro pair.
 * @param[in] gpio_pin  Pin index from the same pair, negative if unwired.
 * @param[in] active    Level that means "asserted" for this signal.
 * @return Nothing.
 * @pre ``g_h.funcs`` is populated, so the port came up.
 * @pre @p gpio_port and @p gpio_pin are the two halves of one macro pair.
 * @post One line giving the level and whether it is asserted was emitted.
 * @post No application state is modified.
 * @note An unwired signal is said to be unwired rather than read, since a
 *       negative pin index would convert to a nonsense unsigned argument.
 * @since 0.1.0
 */
static void
c6_hosted_read_sideband(const char* label, void* gpio_port, int32_t gpio_pin, int32_t active)
{
  c6_hosted_puts("c6_hosted_init: sideband ");
  c6_hosted_puts(label);
  if (gpio_pin < 0) {
    c6_hosted_puts(" unwired\r\n");
    return;
  }
  const int32_t level = (int32_t)g_h.funcs->_h_read_gpio(gpio_port, (uint32_t)gpio_pin);
  c6_hosted_puts(" level=");
  c6_hosted_put_i32(level);
  c6_hosted_puts(" active_level=");
  c6_hosted_put_i32(active);
  c6_hosted_puts((level == active) ? " state=asserted\r\n" : " state=deasserted\r\n");
}

void c6_hosted_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz)
{
  c6_hosted_puts("\r\nc6_hosted_init: EK-RA8D2 <-> ESP32-C6 esp-hosted port bring-up\r\n");
  c6_hosted_puts("c6_hosted_init: port=port/esp-hosted (RA8D2 + ThreadX) "
                 "driver=esp-hosted-mcu\r\n");
  c6_hosted_puts("c6_hosted_init: cpuclk0_hz=");
  c6_hosted_put_u32(cpuclk_hz);
  c6_hosted_puts(" pclka_hz=");
  c6_hosted_put_u32(pclka_hz);
  c6_hosted_puts("\r\nc6_hosted_init: spi sci=");
  c6_hosted_put_u32((uint32_t)k_ra8_board_pmod1_sci_channel);
  c6_hosted_puts(" mode=");
  c6_hosted_put_u32((uint32_t)k_ra8_spi_mode_3);
  c6_hosted_puts(" sck_hz=");
  c6_hosted_put_u32((uint32_t)k_c6_hosted_sck_hz);
  c6_hosted_puts("\r\n");
}

void c6_hosted_print_pin_map(void)
{
  c6_hosted_puts("c6_hosted_init: bus");
  c6_hosted_print_pin(" cs", k_ra8_esp_hosted_pin_chip_select);
  c6_hosted_print_pin(" copi", k_ra8_esp_hosted_pin_copi);
  c6_hosted_print_pin(" cipo", k_ra8_esp_hosted_pin_cipo);
  c6_hosted_print_pin(" sck", k_ra8_esp_hosted_pin_sck);
  c6_hosted_puts("\r\nc6_hosted_init: driver-view");
  c6_hosted_print_gpio(" handshake", H_GPIO_HANDSHAKE_Port, (int32_t)H_GPIO_HANDSHAKE_Pin);
  c6_hosted_print_gpio(" data_ready", H_GPIO_DATA_READY_Port, (int32_t)H_GPIO_DATA_READY_Pin);
  c6_hosted_print_gpio(" reset", H_GPIO_PORT_RESET, (int32_t)H_GPIO_PIN_RESET);
  c6_hosted_puts("\r\n");
  c6_hosted_print_route("handshake", k_ra8_esp_hosted_pin_handshake);
  c6_hosted_print_route("data_ready", k_ra8_esp_hosted_pin_data_ready);
}

void c6_hosted_report_sideband(void)
{
  c6_hosted_read_sideband("handshake",
                          H_GPIO_HANDSHAKE_Port,
                          (int32_t)H_GPIO_HANDSHAKE_Pin,
                          (int32_t)H_HS_VAL_ACTIVE);
  c6_hosted_read_sideband("data_ready",
                          H_GPIO_DATA_READY_Port,
                          (int32_t)H_GPIO_DATA_READY_Pin,
                          (int32_t)H_DR_VAL_ACTIVE);
}

void c6_hosted_on_event(void*       ctx,
                        const char* base,
                        int32_t     event_id,
                        const void* data,
                        size_t      data_len)
{
  (void)ctx;
  (void)data;
  s_c6_hosted_events++;
  c6_hosted_puts("c6_hosted_init: event base=");
  c6_hosted_puts((base != nullptr) ? base : "(null)");
  c6_hosted_puts(" id=");
  c6_hosted_put_i32(event_id);
  c6_hosted_puts(" len=");
  c6_hosted_put_u32((uint32_t)data_len);
  c6_hosted_puts("\r\n");
}

uint32_t c6_hosted_event_count(void)
{
  return s_c6_hosted_events;
}
