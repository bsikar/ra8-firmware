/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/inc/port_esp_hosted_host_config.h
 * @brief esp-hosted port header: transport selection, side-band pin map, link polarity.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The most widely reached of the ten port headers the vendored esp-hosted
 * core includes by name (see ``docs/SOUP/esp-hosted-host.md``): nineteen
 * vendored translation units name it, either directly or through
 * ``transport_drv.h``. Upstream generates its equivalent from Kconfig under
 * ``host/port/``, which this project excludes; this file is its first-party
 * replacement and therefore keeps the upstream file name and every upstream
 * macro spelling exactly.
 *
 * @par What lives here and what does not
 * This header answers "which transport, on which wires, with what polarity".
 * Three neighbours own the rest, and none of the four ever defines the same
 * symbol twice:
 *   - ``port_esp_hosted_host_config_transports.h`` -- the SDIO, half-duplex
 *     SPI and UART blocks. Those transports are not in use, but their
 *     drivers still compile, so their constants must exist.
 *   - ``port_esp_hosted_host_config_features.h`` -- host features this port
 *     does not implement, the RPC bounds and the debug counters.
 *   - ``port_esp_hosted_host_wifi_config.h`` -- Wi-Fi and supplicant feature
 *     availability, which tracks the co-processor image rather than the
 *     board.
 * Both sibling config headers are included from the bottom of this file, so
 * a translation unit that names only the upstream file gets the complete
 * set, and the ``#if`` derivations in them see the polarity flags defined
 * here.
 *
 * @par The GPIO encoding contract
 * The vendored core moves side-band pins around as an opaque ``void*`` port
 * plus an ``int`` pin (``gpio_pin_t`` in ``esp_hosted_transport_config.h``).
 * This port encodes an RA8 pin into that pair with
 * ::RA8_ESP_HOSTED_GPIO_PORT and ::RA8_ESP_HOSTED_GPIO_PIN, which split the
 * packed ``ra8_port_pin_t`` from ``ra8_esp_hosted_pins.h`` using
 * ``RA8_PIN_PORT()`` / ``RA8_PIN_PIN()``:
 *   - the ``..._Port`` value is the port index cast to ``void*``, so port 0
 *     encodes as a null pointer. That is legal here because the port never
 *     null-checks the port argument -- it range-checks the decoded index,
 *     and index 0 is a real RA8 port;
 *   - the ``..._Pin`` value is the pin index within that port, or ``-1``
 *     when the pin is ``k_ra8_pin_none``. Minus one is the vendored code's
 *     own "not wired" spelling: ``spi_drv.c`` tests ``!= -1`` before it
 *     configures or tears down a side-band interrupt.
 * Every pin fact stays in ``ra8_esp_hosted_pins.h``; nothing here restates a
 * port or pin number.
 *
 * @par Interrupt-sense values without the HAL
 * ::H_HS_INTR_EDGE and ::H_DR_INTR_EDGE are consumed by the port's
 * ``_h_config_gpio_as_interrupt`` implementation, which maps them onto
 * ``ra8_icu_irqmd_t``. They are written here as the plain integers that
 * enumeration uses (0 falling, 1 rising, 2 both, 3 low-level) rather than as
 * the enumerators, because including ``ra8_icu_regs.h`` would drag the HAL
 * register map into all nineteen vendored translation units.
 *
 * @par Symbols deliberately NOT defined here
 *   - ``H_ESP_HOSTED_CLI_ENABLED`` and ``H_PEER_DATA_TRANSFER`` are tested
 *     with ``#ifdef``, so defining either to zero would switch the feature
 *     ON. Both stay undefined; see the features header for the reasoning.
 *   - ``H_MIN``, ``H_FREE_PTR_WITH_FUNC``, ``H_BUFF_ZEROCOPY``,
 *     ``H_BUFF_NO_ZEROCOPY`` and ``H_DEFLT_FREE_FUNC`` are defined by the
 *     vendored ``transport_drv.h``; ``H_SET_BIT`` / ``H_GET_BIT`` by
 *     ``esp_hosted_bitmasks.h``; ``H_BIT0`` .. ``H_BIT2`` and the
 *     ``H_CP_GPIO_*`` family by ``esp_hosted_cp_gpio.h``; ``H_FLOW_CTRL_*``
 *     by ``esp_hosted_transport.h``. Restating any of them with different
 *     text is a macro redefinition, which is a warning, not an improvement.
 *   - The two per-direction SPI queue depths are likewise already defined by
 *     the vendored ``spi_drv.h`` (20 each). ::H_TRANSPORT_QUEUE_SIZE is kept
 *     equal to them so the transport mempool that ``transport_drv.c`` sizes
 *     from it matches the queues ``spi_drv.c`` actually creates.
 *   - ``H_GPIO_MODE_DEF_*``, ``H_GPIO_PULL_*``, ``H_TIMER_TYPE_*``,
 *     ``H_IRAM_ATTR`` and ``H_WEAK_REF`` belong to
 *     ``port_esp_hosted_host_os.h``.
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_esp_hosted_pins.h"

/* ----------------------------------------------------------------------- */
/* Transport identity */
/* ----------------------------------------------------------------------- */

/*
 * The core only ever compares these against each other, so the numbering is
 * free; it is dense and starts at zero so "none" is falsy.
 */

/**
 * @def H_TRANSPORT_NONE
 * @brief No transport has been configured yet.
 * @details Zero, and the initial value of the transport-configuration
 * union's discriminant. ``esp_hosted_transport_get_reset_config()`` treats
 * it as "fall back to the compile-time reset pin".
 * @note Read-only build configuration.
 * @warning Never assign this to ::H_TRANSPORT_IN_USE; the vendored default
 *          config function returns an error when no transport is selected.
 * @par Example:
 * @code
 * case H_TRANSPORT_NONE: use_compile_time_reset_pin(); break;
 * @endcode
 * @since 0.1.0
 */
#define H_TRANSPORT_NONE (0)

/**
 * @def H_TRANSPORT_SPI
 * @brief Transport id of the full-duplex SPI link.
 * @details One. This is the transport the EK-RA8D2 uses: four wires plus
 * HANDSHAKE and DATA_READY, matching the ESP32-C6 image built from
 * ``coprocessor/esp32c6/``.
 * @note Read-only build configuration.
 * @warning Selecting it makes ``transport_drv.h`` include
 *          ``port_esp_hosted_host_spi.h``, which is where the frame size
 *          lives.
 * @par Example:
 * @code
 * #if H_TRANSPORT_IN_USE == H_TRANSPORT_SPI
 * #include "port_esp_hosted_host_spi.h"
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_TRANSPORT_SPI (1)

/**
 * @def H_TRANSPORT_SDIO
 * @brief Transport id of the SDIO link.
 * @details Two. Not used on this board: the only SDHI socket carries the SD
 * card, and the C6 module is wired to a Pmod SPI position.
 * @note Read-only build configuration.
 * @warning ``sdio_drv.c`` still compiles; its constants live in
 *          ``port_esp_hosted_host_config_transports.h``.
 * @par Example:
 * @code
 * #if H_TRANSPORT_IN_USE == H_TRANSPORT_SDIO
 * sdio_bus_init();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_TRANSPORT_SDIO (2)

/**
 * @def H_TRANSPORT_SPI_HD
 * @brief Transport id of the half-duplex (1/2/4-line) SPI link.
 * @details Three. Not used: the C6 image is built with
 * ``CONFIG_ESP_SPI_HOST_INTERFACE``, the full-duplex mode.
 * @note Read-only build configuration.
 * @warning Half-duplex needs a DATA_READY line the full-duplex driver also
 *          claims; the two transports cannot coexist on one harness.
 * @par Example:
 * @code
 * #if H_TRANSPORT_IN_USE == H_TRANSPORT_SPI_HD
 * spi_hd_set_data_lines(H_SPI_HD_CONFIG_4_DATA_LINES);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_TRANSPORT_SPI_HD (3)

/**
 * @def H_TRANSPORT_UART
 * @brief Transport id of the UART link.
 * @details Four. Not used: a byte-framed UART cannot carry the 1600-byte
 * frames the rest of this port is sized for at any useful rate.
 * @note Read-only build configuration.
 * @warning ``uart_drv.c`` still compiles; its constants live in
 *          ``port_esp_hosted_host_config_transports.h``.
 * @par Example:
 * @code
 * #if H_TRANSPORT_IN_USE == H_TRANSPORT_UART
 * uart_open_link();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_TRANSPORT_UART (4)

/**
 * @def H_TRANSPORT_IN_USE
 * @brief The transport this image actually drives.
 * @details ::H_TRANSPORT_SPI. Both ends must agree: the co-processor image
 * sets ``CONFIG_ESP_SPI_HOST_INTERFACE=y`` and ``CONFIG_SPI_HSPI=y`` in
 * ``coprocessor/esp32c6/sdkconfig.defaults``.
 * @note Read-only build configuration.
 * @warning Changing this without rebuilding and reflashing the C6 leaves the
 *          host clocking a bus the co-processor is not listening on.
 * @par Example:
 * @code
 * #if H_TRANSPORT_IN_USE == H_TRANSPORT_SPI
 * transport = spi_get_handle();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_TRANSPORT_IN_USE (H_TRANSPORT_SPI)

/* ----------------------------------------------------------------------- */
/* Co-processor part number */
/* ----------------------------------------------------------------------- */

/*
 * Exactly one of these is 1; transport_drv.c turns it into the chip id it
 * expects to read back in the co-processor's boot event, and mismatches are
 * reported rather than ignored. The upstream macro names carry a legacy role
 * word this project does not use in its own prose.
 */

/**
 * @def H_SLAVE_TARGET_ESP32C6 LEGACY-OK: upstream esp-hosted macro name
 * @brief The co-processor on this board is an ESP32-C6.
 * @details One. ``coprocessor/esp32c6/pins.env`` pins the module, the
 * ESP-IDF version (v5.5.4) and the esp-hosted-mcu commit it is built from;
 * the part reports chip id 0x0D in its boot event.
 * @note Read-only build configuration.
 * @warning Exactly one target macro may be 1. The vendored selection is an
 *          ``#if`` / ``#elif`` chain, so a second 1 is silently ignored and
 *          the wrong chip id is expected.
 * @par Example:
 * @code
 * #if H_SLAVE_TARGET_ESP32C6 // LEGACY-OK: upstream esp-hosted macro name
 * exp_chip_id = ESP_PRIV_FIRMWARE_CHIP_ESP32C6;
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SLAVE_TARGET_ESP32C6 (1) /* LEGACY-OK: upstream esp-hosted macro name */

/**
 * @def H_SLAVE_TARGET_ESP32 LEGACY-OK: upstream esp-hosted macro name
 * @brief The co-processor is not an original ESP32.
 * @details Zero. Also read by ``sdio_reg.h`` to pick a register layout,
 * which is another reason it must be honest rather than merely unused.
 * @note Read-only build configuration.
 * @warning Setting this alongside the C6 selector makes the host expect the
 *          wrong chip id and refuse the link.
 * @par Example:
 * @code
 * #if H_SLAVE_TARGET_ESP32 // LEGACY-OK: upstream esp-hosted macro name
 * exp_chip_id = ESP_PRIV_FIRMWARE_CHIP_ESP32;
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SLAVE_TARGET_ESP32 (0) /* LEGACY-OK: upstream esp-hosted macro name */

/**
 * @def H_SLAVE_TARGET_ESP32C2 LEGACY-OK: upstream esp-hosted macro name
 * @brief The co-processor is not an ESP32-C2.
 * @details Zero; no C2 module exists on this bench.
 * @note Read-only build configuration.
 * @warning Exactly one target selector may be 1; see the C6 one above.
 * @since 0.1.0
 */
#define H_SLAVE_TARGET_ESP32C2 (0) /* LEGACY-OK: upstream esp-hosted macro name */

/**
 * @def H_SLAVE_TARGET_ESP32C3 LEGACY-OK: upstream esp-hosted macro name
 * @brief The co-processor is not an ESP32-C3.
 * @details Zero; no C3 module exists on this bench.
 * @note Read-only build configuration.
 * @warning Exactly one target selector may be 1; see the C6 one above.
 * @since 0.1.0
 */
#define H_SLAVE_TARGET_ESP32C3 (0) /* LEGACY-OK: upstream esp-hosted macro name */

/**
 * @def H_SLAVE_TARGET_ESP32C5 LEGACY-OK: upstream esp-hosted macro name
 * @brief The co-processor is not an ESP32-C5.
 * @details Zero. Also consulted by ``sdio_reg.h``.
 * @note Read-only build configuration.
 * @warning Exactly one target selector may be 1; see the C6 one above.
 * @since 0.1.0
 */
#define H_SLAVE_TARGET_ESP32C5 (0) /* LEGACY-OK: upstream esp-hosted macro name */

/**
 * @def H_SLAVE_TARGET_ESP32C61 LEGACY-OK: upstream esp-hosted macro name
 * @brief The co-processor is not an ESP32-C61.
 * @details Zero. Also consulted by ``sdio_reg.h``.
 * @note Read-only build configuration.
 * @warning Exactly one target selector may be 1; see the C6 one above.
 * @since 0.1.0
 */
#define H_SLAVE_TARGET_ESP32C61 (0) /* LEGACY-OK: upstream esp-hosted macro name */

/**
 * @def H_SLAVE_TARGET_ESP32H2 LEGACY-OK: upstream esp-hosted macro name
 * @brief The co-processor is not an ESP32-H2.
 * @details Zero; the H2 has no Wi-Fi radio, which is the point of this link.
 * @note Read-only build configuration.
 * @warning Exactly one target selector may be 1; see the C6 one above.
 * @since 0.1.0
 */
#define H_SLAVE_TARGET_ESP32H2 (0) /* LEGACY-OK: upstream esp-hosted macro name */

/**
 * @def H_SLAVE_TARGET_ESP32H4 LEGACY-OK: upstream esp-hosted macro name
 * @brief The co-processor is not an ESP32-H4.
 * @details Zero; no H4 module exists on this bench.
 * @note Read-only build configuration.
 * @warning Exactly one target selector may be 1; see the C6 one above.
 * @since 0.1.0
 */
#define H_SLAVE_TARGET_ESP32H4 (0) /* LEGACY-OK: upstream esp-hosted macro name */

/**
 * @def H_SLAVE_TARGET_ESP32S2 LEGACY-OK: upstream esp-hosted macro name
 * @brief The co-processor is not an ESP32-S2.
 * @details Zero; no S2 module exists on this bench.
 * @note Read-only build configuration.
 * @warning Exactly one target selector may be 1; see the C6 one above.
 * @since 0.1.0
 */
#define H_SLAVE_TARGET_ESP32S2 (0) /* LEGACY-OK: upstream esp-hosted macro name */

/**
 * @def H_SLAVE_TARGET_ESP32S3 LEGACY-OK: upstream esp-hosted macro name
 * @brief The co-processor is not an ESP32-S3.
 * @details Zero; no S3 module exists on this bench.
 * @note Read-only build configuration.
 * @warning Exactly one target selector may be 1; see the C6 one above.
 * @since 0.1.0
 */
#define H_SLAVE_TARGET_ESP32S3 (0) /* LEGACY-OK: upstream esp-hosted macro name */

/* ----------------------------------------------------------------------- */
/* Packed-pin to (void* port, int pin) adapters */
/* ----------------------------------------------------------------------- */

/**
 * @def RA8_ESP_HOSTED_GPIO_PORT
 * @brief Extract the ``void*`` port half of a packed RA8 pin.
 * @details Decodes the high byte of a ``ra8_port_pin_t`` with
 * ``RA8_PIN_PORT()`` and carries it through ``uintptr_t`` into the opaque
 * pointer the vendored vtable takes. Port 0 therefore yields a null
 * pointer, which is a valid encoding here: the port decodes the integer back
 * and range-checks it, and never treats null as "absent".
 * @param[in] pin_code Packed ``RA8_PIN(port, pin)`` value.
 * @note Read-only build configuration; expands to a constant expression.
 * @warning Do not dereference the result. It is an index in pointer
 *          clothing, not an address.
 * @par Example:
 * @code
 * void *port = RA8_ESP_HOSTED_GPIO_PORT(k_ra8_esp_hosted_pin_handshake);
 * @endcode
 * @since 0.1.0
 */
#define RA8_ESP_HOSTED_GPIO_PORT(pin_code) ((void*)(uintptr_t)RA8_PIN_PORT(pin_code))

/**
 * @def RA8_ESP_HOSTED_GPIO_PIN
 * @brief Extract the ``int`` pin half of a packed RA8 pin, or -1 if unwired.
 * @details Decodes the low byte with ``RA8_PIN_PIN()``, except that the
 * ``k_ra8_pin_none`` sentinel maps to ``-1`` -- the value the vendored SPI
 * driver tests against before it configures or tears down a side-band
 * interrupt.
 *
 * Both sides of the sentinel test are narrowed to ``uint16_t`` first. The
 * argument and ``k_ra8_pin_none`` come from different enumerations, and
 * comparing two enumerated types directly is a ``-Wenum-compare`` error
 * under this project's warning settings.
 * @param[in] pin_code Packed ``RA8_PIN(port, pin)`` value, or
 *                     ``k_ra8_pin_none``.
 * @note Read-only build configuration; expands to a constant expression.
 * @warning The vtable takes ``uint32_t`` for the pin, so a ``-1`` that
 *          reaches a call converts to 0xFFFFFFFF. Guard the call, as
 *          ``spi_drv.c`` does.
 * @par Example:
 * @code
 * if (RA8_ESP_HOSTED_GPIO_PIN(k_ra8_esp_hosted_pin_reset) != -1) { hold(); }
 * @endcode
 * @since 0.1.0
 */
#define RA8_ESP_HOSTED_GPIO_PIN(pin_code)                                                          \
  (((uint16_t)(pin_code) == (uint16_t)k_ra8_pin_none) ? (-1) : (int)RA8_PIN_PIN(pin_code))

/* ----------------------------------------------------------------------- */
/* Side-band pin map */
/* ----------------------------------------------------------------------- */

/*
 * Note the upstream spellings: CamelCase _Port / _Pin for HANDSHAKE and
 * DATA_READY, but a _PORT_ / _PIN_ infix for RESET. Both are verified against
 * spi_drv.c and esp_hosted_transport_config.c.
 */

/**
 * @def H_GPIO_HANDSHAKE_Port
 * @brief Opaque port handle of the HANDSHAKE input.
 * @details Derived from ``k_ra8_esp_hosted_pin_handshake``; see the encoding
 * contract in this file's header block.
 * @note Read-only build configuration.
 * @warning Meaningless without the matching ::H_GPIO_HANDSHAKE_Pin; the two
 *          are always passed together.
 * @par Example:
 * @code
 * g_h.funcs->_h_read_gpio(H_GPIO_HANDSHAKE_Port, H_GPIO_HANDSHAKE_Pin);
 * @endcode
 * @since 0.1.0
 */
/* NOLINTNEXTLINE(readability-identifier-naming) -- upstream-fixed mixed-case spelling. */
#define H_GPIO_HANDSHAKE_Port RA8_ESP_HOSTED_GPIO_PORT(k_ra8_esp_hosted_pin_handshake)

/**
 * @def H_GPIO_HANDSHAKE_Pin
 * @brief Pin index of the HANDSHAKE input, or -1 when it is not wired.
 * @details Derived from ``k_ra8_esp_hosted_pin_handshake``. The C6 asserts
 * this line when it is ready for the next transaction and deasserts it on
 * the chip-select edge.
 * @note Read-only build configuration.
 * @warning ``spi_drv.c`` skips interrupt teardown when this is -1; a harness
 *          without a HANDSHAKE wire runs the link on DATA_READY alone.
 * @par Example:
 * @code
 * if (H_GPIO_HANDSHAKE_Pin != -1) { teardown_handshake_irq(); }
 * @endcode
 * @since 0.1.0
 */
/* NOLINTNEXTLINE(readability-identifier-naming) -- upstream-fixed mixed-case spelling. */
#define H_GPIO_HANDSHAKE_Pin RA8_ESP_HOSTED_GPIO_PIN(k_ra8_esp_hosted_pin_handshake)

/**
 * @def H_GPIO_DATA_READY_Port
 * @brief Opaque port handle of the DATA_READY input.
 * @details Derived from ``k_ra8_esp_hosted_pin_data_ready``; see the encoding
 * contract in this file's header block.
 * @note Read-only build configuration.
 * @warning Meaningless without the matching ::H_GPIO_DATA_READY_Pin.
 * @par Example:
 * @code
 * g_h.funcs->_h_read_gpio(H_GPIO_DATA_READY_Port, H_GPIO_DATA_READY_Pin);
 * @endcode
 * @since 0.1.0
 */
/* NOLINTNEXTLINE(readability-identifier-naming) -- upstream-fixed mixed-case spelling. */
#define H_GPIO_DATA_READY_Port RA8_ESP_HOSTED_GPIO_PORT(k_ra8_esp_hosted_pin_data_ready)

/**
 * @def H_GPIO_DATA_READY_Pin
 * @brief Pin index of the DATA_READY input, or -1 when it is not wired.
 * @details Derived from ``k_ra8_esp_hosted_pin_data_ready``. The C6 asserts
 * it while its transmit queue holds a frame for the host, and keeps it
 * asserted until the host drains that frame.
 * @note Read-only build configuration.
 * @warning As wired this net (J26-8) has no ICU channel, so the port serves
 *          it with the software edge detector. That costs nothing only
 *          because the level is held: the one Pmod1 side-band net with a
 *          channel is HANDSHAKE's, which needs it, since HANDSHAKE merely
 *          pulses.
 * @par Example:
 * @code
 * if (H_GPIO_DATA_READY_Pin != -1) { attach_data_ready_irq(); }
 * @endcode
 * @since 0.1.0
 */
/* NOLINTNEXTLINE(readability-identifier-naming) -- upstream-fixed mixed-case spelling. */
#define H_GPIO_DATA_READY_Pin RA8_ESP_HOSTED_GPIO_PIN(k_ra8_esp_hosted_pin_data_ready)

/**
 * @def H_GPIO_PORT_RESET
 * @brief Opaque port handle of the co-processor reset output.
 * @details Derived from ``k_ra8_esp_hosted_pin_reset``. Used as the
 * fall-back reset pin before any transport configuration has been set.
 * @note Read-only build configuration.
 * @warning Upstream spells this one ``_PORT_`` in the middle, unlike the
 *          CamelCase side-band pair above; both spellings are mandatory.
 * @par Example:
 * @code
 * gpio_pin_t reset = { .port = H_GPIO_PORT_RESET, .pin = H_GPIO_PIN_RESET };
 * @endcode
 * @since 0.1.0
 */
#define H_GPIO_PORT_RESET RA8_ESP_HOSTED_GPIO_PORT(k_ra8_esp_hosted_pin_reset)

/**
 * @def H_GPIO_PIN_RESET
 * @brief Pin index of the co-processor reset output, or -1 when unwired.
 * @details Minus one on this harness: ``coprocessor/esp32c6/pins.env``
 * records the C6 reset input as disconnected, so the pin table sets
 * ``k_ra8_esp_hosted_pin_reset`` to ``k_ra8_pin_none``.
 * @note Read-only build configuration.
 * @warning With no reset wire the host cannot recover a wedged co-processor;
 *          it has to be power-cycled. Wiring one is a pin-table edit only.
 * @par Example:
 * @code
 * if (H_GPIO_PIN_RESET != -1) { pulse_coprocessor_reset(); }
 * @endcode
 * @since 0.1.0
 */
#define H_GPIO_PIN_RESET RA8_ESP_HOSTED_GPIO_PIN(k_ra8_esp_hosted_pin_reset)

/* ----------------------------------------------------------------------- */
/* Side-band polarity and interrupt sense */
/* ----------------------------------------------------------------------- */

/*
 * The C6 image drives both HANDSHAKE and DATA_READY push-pull and asserts
 * them HIGH, so both flags are 1 and both edges are rising. The derived
 * values below follow from the flags, never the other way round.
 */

/**
 * @def H_HANDSHAKE_ACTIVE_HIGH
 * @brief Whether the co-processor asserts HANDSHAKE by driving it high.
 * @details One. The stock esp-hosted C6 image drives HANDSHAKE high to say
 * "ready for the next transaction"; the bench probe saw the same.
 * @note Read-only build configuration.
 * @warning ``spi_drv.c`` inverts its own bookkeeping when this is 0; do not
 *          also invert the derived values by hand.
 * @par Example:
 * @code
 * #if !H_HANDSHAKE_ACTIVE_HIGH
 * invert_handshake_tracking();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_HANDSHAKE_ACTIVE_HIGH (1)

/**
 * @def H_DATAREADY_ACTIVE_HIGH
 * @brief Whether the co-processor asserts DATA_READY by driving it high.
 * @details One, for the same image and the same bench observation as
 * ::H_HANDSHAKE_ACTIVE_HIGH.
 * @note Read-only build configuration.
 * @warning An inverted harness must change this flag, not the derived
 *          level and edge macros.
 * @par Example:
 * @code
 * #if !H_DATAREADY_ACTIVE_HIGH
 * invert_data_ready_tracking();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_DATAREADY_ACTIVE_HIGH (1)

#if H_HANDSHAKE_ACTIVE_HIGH

/**
 * @def H_HS_VAL_ACTIVE
 * @brief Level read back on HANDSHAKE when it is asserted.
 * @details One, because ::H_HANDSHAKE_ACTIVE_HIGH is 1.
 * @note Read-only build configuration.
 * @warning Must stay consistent with ::H_HS_INTR_EDGE or the driver waits
 *          for an edge that never arrives on the level it then tests.
 * @par Example:
 * @code
 * if (gpio_handshake == H_HS_VAL_ACTIVE) { start_transaction(); }
 * @endcode
 * @since 0.1.0
 */
#define H_HS_VAL_ACTIVE (1)

/**
 * @def H_HS_VAL_INACTIVE
 * @brief Level read back on HANDSHAKE when it is deasserted.
 * @details Zero, the complement of ::H_HS_VAL_ACTIVE.
 * @note Read-only build configuration.
 * @warning The SPI driver seeds its cached level with this before the first
 *          read, so an inconsistent pair reports a spurious first edge.
 * @par Example:
 * @code
 * gpio_pin_state_t handshake = H_HS_VAL_INACTIVE;
 * @endcode
 * @since 0.1.0
 */
#define H_HS_VAL_INACTIVE (0)

/**
 * @def H_HS_INTR_EDGE
 * @brief Interrupt sense the port programs for the HANDSHAKE input.
 * @details One, which is ``k_ra8_icu_irqmd_rising`` -- the asserting edge of
 * an active-high line. Written as an integer so the HAL register header does
 * not have to be visible to the vendored core.
 * @note Read-only build configuration.
 * @warning If the ICU sense encoding ever changes, this integer and
 *          ``ra8_icu_irqmd_t`` drift apart silently; the port's own
 *          conversion is where that is asserted.
 * @par Example:
 * @code
 * g_h.funcs->_h_config_gpio_as_interrupt(H_GPIO_HANDSHAKE_Port,
 *     H_GPIO_HANDSHAKE_Pin, H_HS_INTR_EDGE, gpio_hs_isr_handler, NULL);
 * @endcode
 * @since 0.1.0
 */
#define H_HS_INTR_EDGE (1)

#else /* active-low HANDSHAKE harness */

/**
 * @def H_HS_VAL_ACTIVE
 * @brief Level read back on HANDSHAKE when it is asserted (inverted harness).
 * @details Zero -- the active-low half of ::H_HANDSHAKE_ACTIVE_HIGH.
 * @note Read-only build configuration.
 * @warning Unreachable while ::H_HANDSHAKE_ACTIVE_HIGH is 1.
 * @since 0.1.0
 */
#define H_HS_VAL_ACTIVE   (0)

/**
 * @def H_HS_VAL_INACTIVE
 * @brief Level read back on HANDSHAKE when deasserted (inverted harness).
 * @details One -- the active-low half of ::H_HANDSHAKE_ACTIVE_HIGH.
 * @note Read-only build configuration.
 * @warning Unreachable while ::H_HANDSHAKE_ACTIVE_HIGH is 1.
 * @since 0.1.0
 */
#define H_HS_VAL_INACTIVE (1)

/**
 * @def H_HS_INTR_EDGE
 * @brief Interrupt sense for HANDSHAKE on an inverted harness.
 * @details Zero, which is ``k_ra8_icu_irqmd_falling``.
 * @note Read-only build configuration.
 * @warning Unreachable while ::H_HANDSHAKE_ACTIVE_HIGH is 1.
 * @since 0.1.0
 */
#define H_HS_INTR_EDGE    (0)

#endif /* H_HANDSHAKE_ACTIVE_HIGH */

#if H_DATAREADY_ACTIVE_HIGH

/**
 * @def H_DR_VAL_ACTIVE
 * @brief Level read back on DATA_READY when a frame is waiting.
 * @details One, because ::H_DATAREADY_ACTIVE_HIGH is 1.
 * @note Read-only build configuration.
 * @warning Must stay consistent with ::H_DR_INTR_EDGE.
 * @par Example:
 * @code
 * uint8_t pending = (gpio_rx_data_ready == H_DR_VAL_ACTIVE);
 * @endcode
 * @since 0.1.0
 */
#define H_DR_VAL_ACTIVE (1)

/**
 * @def H_DR_VAL_INACTIVE
 * @brief Level read back on DATA_READY when nothing is waiting.
 * @details Zero, the complement of ::H_DR_VAL_ACTIVE.
 * @note Read-only build configuration.
 * @warning Seeds the driver's cached level; an inconsistent pair reports a
 *          spurious first edge.
 * @par Example:
 * @code
 * gpio_pin_state_t data_ready = H_DR_VAL_INACTIVE;
 * @endcode
 * @since 0.1.0
 */
#define H_DR_VAL_INACTIVE (0)

/**
 * @def H_DR_INTR_EDGE
 * @brief Interrupt sense the port programs for the DATA_READY input.
 * @details One, which is ``k_ra8_icu_irqmd_rising``. This is the edge that
 * actually wakes the transaction pump, so it is the latency-critical one.
 * @note Read-only build configuration.
 * @warning Integer-encoded to keep the HAL out of the vendored core; see
 *          ::H_HS_INTR_EDGE.
 * @par Example:
 * @code
 * g_h.funcs->_h_config_gpio_as_interrupt(H_GPIO_DATA_READY_Port,
 *     H_GPIO_DATA_READY_Pin, H_DR_INTR_EDGE, gpio_dr_isr_handler, NULL);
 * @endcode
 * @since 0.1.0
 */
#define H_DR_INTR_EDGE (1)

#else /* active-low DATA_READY harness */

/**
 * @def H_DR_VAL_ACTIVE
 * @brief Level on DATA_READY when a frame is waiting (inverted harness).
 * @details Zero -- the active-low half of ::H_DATAREADY_ACTIVE_HIGH.
 * @note Read-only build configuration.
 * @warning Unreachable while ::H_DATAREADY_ACTIVE_HIGH is 1.
 * @since 0.1.0
 */
#define H_DR_VAL_ACTIVE   (0)

/**
 * @def H_DR_VAL_INACTIVE
 * @brief Level on DATA_READY when idle (inverted harness).
 * @details One -- the active-low half of ::H_DATAREADY_ACTIVE_HIGH.
 * @note Read-only build configuration.
 * @warning Unreachable while ::H_DATAREADY_ACTIVE_HIGH is 1.
 * @since 0.1.0
 */
#define H_DR_VAL_INACTIVE (1)

/**
 * @def H_DR_INTR_EDGE
 * @brief Interrupt sense for DATA_READY on an inverted harness.
 * @details Zero, which is ``k_ra8_icu_irqmd_falling``.
 * @note Read-only build configuration.
 * @warning Unreachable while ::H_DATAREADY_ACTIVE_HIGH is 1.
 * @since 0.1.0
 */
#define H_DR_INTR_EDGE    (0)

#endif /* H_DATAREADY_ACTIVE_HIGH */

/* ----------------------------------------------------------------------- */
/* Co-processor reset polarity */
/* ----------------------------------------------------------------------- */

/**
 * @def H_RESET_VAL_ACTIVE
 * @brief Level that holds the co-processor in reset.
 * @details Zero: the ESP32 reset input (EN) is active-low, so asserting
 * reset means driving the line low.
 * @note Read-only build configuration.
 * @warning Only reached when a reset pin is wired; see ::H_GPIO_PIN_RESET.
 * @par Example:
 * @code
 * g_h.funcs->_h_write_gpio(reset.port, reset.pin, H_RESET_VAL_ACTIVE);
 * @endcode
 * @since 0.1.0
 */
#define H_RESET_VAL_ACTIVE (0)

/**
 * @def H_RESET_VAL_INACTIVE
 * @brief Level that releases the co-processor from reset.
 * @details One, the complement of ::H_RESET_VAL_ACTIVE.
 * @note Read-only build configuration.
 * @warning The line must be released before the boot event can arrive; the
 *          transport waits for that event and gives up without it.
 * @par Example:
 * @code
 * g_h.funcs->_h_write_gpio(reset.port, reset.pin, H_RESET_VAL_INACTIVE);
 * @endcode
 * @since 0.1.0
 */
#define H_RESET_VAL_INACTIVE (1)

/* ----------------------------------------------------------------------- */
/* Queue depth, pooling and host identity */
/* ----------------------------------------------------------------------- */

/**
 * @def H_TRANSPORT_QUEUE_SIZE
 * @brief Frames a single transport direction can hold.
 * @details Twenty, matching the per-direction depths the vendored
 * ``spi_drv.h`` already hard-codes. ``transport_drv.c`` sizes the shared
 * mempool from this value, so keeping the two equal is what stops the pool
 * running dry while the queues still have room.
 * @note Read-only build configuration.
 * @warning Twenty 1600-byte frames per direction is roughly 64 KiB of pool;
 *          raising it costs SRAM the display and storage layers also want.
 * @par Example:
 * @code
 * config.num_blocks = H_TRANSPORT_QUEUE_SIZE + MEMPOOL_PADDING;
 * @endcode
 * @since 0.1.0
 */
#define H_TRANSPORT_QUEUE_SIZE (20)

/*
 * H_USE_MEMPOOL is DELIBERATELY LEFT UNDEFINED. Read this before adding it.
 *
 * The vendored ``host/esp_hosted_os_abstraction.h`` guards four rows of the
 * OS-abstraction vtable with ``#ifdef H_USE_MEMPOOL`` -- not ``#if``. So
 * ``#define H_USE_MEMPOOL (0)`` does NOT turn the feature off: it turns it ON
 * and grows ``hosted_osi_funcs_t`` by four function pointers. Worse, that
 * header does not include this one, so the struct would gain or lose those
 * four rows depending on whether a given translation unit happened to reach
 * this header first -- two different layouts for one type, in one image, with
 * no diagnostic from any compiler. Leaving the symbol undefined is what makes
 * the layout the same everywhere.
 *
 * Off is also the right answer on its own merits. Upstream's pool lives in
 * ``common/mempool/mempool_ll.{c,h}``, whose header includes
 * ``freertos/FreeRTOS.h``, ``portmacro.h``, ``task.h`` and ``semphr.h``
 * unconditionally -- it is a FreeRTOS data structure, and this image runs
 * ThreadX. With the symbol absent, ``transport_util.h`` routes every transport
 * buffer through ``_h_malloc_align`` instead, which this port serves from a
 * fixed ThreadX byte pool carved once during initialisation. That is the
 * bounded, allocate-once, NASA Power of 10 Rule 3 behaviour the upstream pool
 * existed to provide, one layer down.
 *
 * ``CONFIG_ESP_HOSTED_USE_MEMPOOL`` in ``idf_compat/sdkconfig.h`` is tested
 * with a bare ``#if`` and so is defined to 0 there; the two say the same
 * thing in the two different dialects their consumers use.
 */

/**
 * @def H_ESP_HOSTED_HOST
 * @brief Marks this build as the esp-hosted host, not the co-processor.
 * @details One. The vendored sources are shared between both ends; this is
 * the side that drives the bus and runs the RPC client.
 * @note Read-only build configuration; tested with ``#ifdef``, so its value
 * matters less than its presence.
 * @warning Never define this in a co-processor build; the two roles compile
 *          mutually exclusive halves of the same files.
 * @par Example:
 * @code
 * #ifdef H_ESP_HOSTED_HOST
 * host_side_only_command();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_ESP_HOSTED_HOST (1)

#ifndef H_ESP_PAYLOAD_HEADER_OFFSET
/**
 * @def H_ESP_PAYLOAD_HEADER_OFFSET
 * @brief Bytes of esp-hosted payload header in front of every frame.
 * @details Twelve, which is ``sizeof(struct esp_payload_header)`` for the
 * layout in the vendored ``common/esp_hosted_header.h``: a packed
 * interface/number byte, flags, length, offset, checksum, sequence number,
 * a throttle-and-reserved byte and the trailing one-byte union. That header
 * defines the same macro from the ``sizeof``; the guard means whichever
 * header a translation unit reaches first wins, and both give 12.
 * @note Read-only build configuration.
 * @warning The two definitions only agree while ``ESP_PKT_NUM_DEBUG`` is
 *          off. Enabling it grows the struct to 14 and this literal becomes
 *          a lie -- delete the literal rather than bump it.
 * @par Example:
 * @code
 * memcpy(frame + H_ESP_PAYLOAD_HEADER_OFFSET, payload, len);
 * @endcode
 * @since 0.1.0
 */
#define H_ESP_PAYLOAD_HEADER_OFFSET (12)
#endif /* H_ESP_PAYLOAD_HEADER_OFFSET */

/**
 * @def H_SPI_FD_CLK_MHZ
 * @brief Full-duplex SPI clock, in megahertz.
 * @details Five: a deliberately conservative first-light rate. The landed
 * link probe clocked at 1 MHz, the C6 full-duplex peripheral tolerates up to
 * 40, and the RA8 side runs on the SCI Simple-SPI backend, which is a polled
 * byte engine -- the ceiling here is host software, not the wire. Five is
 * fast enough that a 1600-byte frame is not the bottleneck and slow enough
 * that a marginal Pmod harness still latches.
 * @note Read-only build configuration.
 * @warning ``spi_drv.c`` logs a hint whenever this is under 40; that hint is
 *          about the co-processor's limit, not this board's.
 * @par Example:
 * @code
 * spi_config.clk_mhz = H_SPI_FD_CLK_MHZ;
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_FD_CLK_MHZ (5)

/**
 * @def H_ENABLE
 * @brief Generic "switch this on" argument for the GPIO vtable.
 * @details One. Passed as the ``enable`` argument of ``_h_pull_gpio`` and as
 * the ``hold_value`` of ``_h_hold_gpio``.
 * @note Read-only build configuration.
 * @warning Not a feature flag; it is a call argument.
 * @par Example:
 * @code
 * g_h.funcs->_h_pull_gpio(port, pin, H_GPIO_PULL_UP, H_ENABLE);
 * @endcode
 * @since 0.1.0
 */
#define H_ENABLE (1)

/**
 * @def H_DISABLE
 * @brief Generic "switch this off" argument for the GPIO vtable.
 * @details Zero, the complement of ::H_ENABLE.
 * @note Read-only build configuration.
 * @warning Not a feature flag; it is a call argument.
 * @par Example:
 * @code
 * g_h.funcs->_h_hold_gpio(reset.port, reset.pin, H_DISABLE);
 * @endcode
 * @since 0.1.0
 */
#define H_DISABLE (0)

/* ----------------------------------------------------------------------- */
/* Remaining configuration, split by theme */
/* ----------------------------------------------------------------------- */

/*
 * Included last so the derivations in these files see the transport and
 * polarity selections above. A vendored translation unit that names only
 * this header still gets the complete symbol set.
 */

#include "port_esp_hosted_host_config_features.h"
#include "port_esp_hosted_host_config_transports.h"
