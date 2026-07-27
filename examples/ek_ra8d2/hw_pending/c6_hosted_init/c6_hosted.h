/**
 * @file examples/ek_ra8d2/hw_pending/c6_hosted_init/c6_hosted.h
 * @brief Shared contract for the esp-hosted port bring-up application.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The application is split into one module per concern, all driven by
 * ``main.c``:
 *
 *   - ``src/c6_hosted_console.c`` -- bounded console formatters and the two
 *     pin printers, so the image links no newlib ``printf``.
 *   - ``src/c6_hosted_report.c``  -- the banner, the resolved pin map with
 *     each side-band pin's interrupt path, the side-band sampler that reads
 *     through the OS-abstraction vtable, and the event handler.
 *   - ``src/c6_hosted_frame.c``   -- the transmitted idle frame, the single
 *     full-duplex transaction, and the decode and verdict of what came back.
 *
 * Every parameter an application owns is stated once here. Pin identity is
 * a board fact and lives in ``port/esp-hosted/inc/ra8_esp_hosted_pins.h``;
 * protocol sizes are read from the vendored esp-hosted headers, never
 * restated. The frame type below is the one place the two meet: it is
 * declared here because both the transaction buffers and the decode need it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_hosted_header.h"
#include "port_esp_hosted_host_spi.h"
#include "ra8_esp_hosted_pins.h"

/**
 * @enum c6_hosted_cfg_t
 * @brief Link and storage parameters this application chooses.
 * @details Only values an application owns live here. NASA Power of 10
 * Rule 3 forbids allocation after initialisation and this image has no
 * heap, so the transaction-buffer alignment and the worker stack are sized
 * here as well as the link timing.
 * @invariant ::k_c6_hosted_sck_hz stays below the 40 MHz the C6 full-duplex
 *            peripheral accepts and inside what the SCI Simple-SPI divider
 *            can reach from PCLKA.
 * @invariant ::k_c6_hosted_edge_poll_ms is non-zero; the port's software
 *            edge detector arms a ThreadX timer, which rejects a zero tick.
 * @par Example:
 * @code
 * cfg.sck_hz = (uint32_t)k_c6_hosted_sck_hz;
 * @endcode
 * @see ra8_esp_hosted_port_cfg_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_c6_hosted_uart_baud    = 115200U,  /**< Console rate, 8N1, over the J-Link OB VCOM. */
  k_c6_hosted_sck_hz       = 5000000U, /**< SPI bit rate: the 5 MHz upstream recommends for
                                       first light, an order above the probe's 1 MHz. */
  k_c6_hosted_edge_poll_ms = 2U,       /**< Poll period for a side-band pin with no ICU channel. */
  k_c6_hosted_heartbeat_ms = 2000U,    /**< Heartbeat gap; the ThreadX tick is 1 ms, so this
                                       is also the sleep expressed in ticks. */
  k_c6_hosted_boot_wait_ms = 200U,     /**< Settling delay before the first vtable read. */
  k_c6_hosted_dma_align    = 64U,      /**< Transaction-buffer alignment, in bytes.      */
  k_c6_hosted_worker_stack = 4096U,    /**< Worker-thread stack, in bytes.               */
  k_c6_hosted_worker_prio  = 8U,       /**< Worker priority and preemption threshold.    */
} c6_hosted_cfg_t;

/**
 * @enum c6_hosted_fmt_t
 * @brief Bounds for the console formatters in ``src/c6_hosted_console.c``.
 * @details The image links no newlib ``printf``, so the serialisers do their
 * own digit extraction; every loop they run is bounded by a value from this
 * enumeration, which is what satisfies NASA Power of 10 Rule 2.
 * @invariant ::k_c6_hosted_dec_digits holds the widest 32-bit decimal value.
 * @invariant ::k_c6_hosted_hex_digits holds the widest 32-bit hex value.
 * @par Example:
 * @code
 * c6_hosted_put_hex(csum, (uint8_t)k_c6_hosted_hex_csum);
 * @endcode
 * @see c6_hosted_put_u32
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_c6_hosted_str_max    = 256U,  /**< Longest string the console helper emits.   */
  k_c6_hosted_dec_radix  = 10U,   /**< Decimal radix.                             */
  k_c6_hosted_dec_digits = 10U,   /**< Digits in the widest 32-bit decimal value. */
  k_c6_hosted_hex_digits = 8U,    /**< Digits in the widest 32-bit hex value.     */
  k_c6_hosted_hex_bits   = 4U,    /**< Bits per hexadecimal digit.                */
  k_c6_hosted_hex_mask   = 0x0FU, /**< Nibble mask.                               */
  k_c6_hosted_hex_alpha  = 10U,   /**< First nibble value spelled with a letter.  */
  k_c6_hosted_hex_byte   = 2U,    /**< Hex digits printed for a byte-wide field.  */
  k_c6_hosted_hex_csum   = 4U,    /**< Hex digits printed for a 16-bit checksum.  */
} c6_hosted_fmt_t;

/**
 * @enum c6_hosted_link_t
 * @brief Frame geometry both ends of the link must agree on.
 * @details Derived from the vendored transport header rather than restated,
 * so an upstream change to the transaction size moves this application with
 * it instead of leaving a stale literal behind.
 * @invariant ::k_c6_hosted_frame_bytes is exactly the byte count the
 *            co-processor clocks in one full-duplex transaction; a mismatch
 *            mis-decodes every frame.
 * @invariant ::k_c6_hosted_frame_bytes is at least
 *            ``sizeof(struct esp_payload_header)``, so a frame always has
 *            room for its own header.
 * @par Example:
 * @code
 * c6_hosted_put_u32((uint32_t)k_c6_hosted_frame_bytes);
 * @endcode
 * @see c6_hosted_frame_t
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_c6_hosted_frame_bytes = (uint16_t)MAX_TRANSPORT_BUFFER_SIZE,
  /**< Bytes clocked in one full-duplex transaction. */
} c6_hosted_link_t;

/**
 * @union c6_hosted_frame
 * @brief One esp-hosted transaction buffer, addressable either way.
 * @details The transport clocks a flat byte array; the protocol reads a
 * twelve-byte payload header off the front of it. Declaring both as members
 * of one union makes that overlay legal by construction -- union member
 * access is defined in C, whereas casting the byte array to a header pointer
 * is not, and clang-tidy's ``bugprone-casting-through-void`` says so.
 * @invariant ``bytes`` and ``header`` name the same storage, so a write
 *            through either is visible through the other.
 * @invariant The union is exactly ::k_c6_hosted_frame_bytes long, because
 *            the header member is smaller than the byte member.
 * @par Example:
 * @code
 * static c6_hosted_frame_t frame;
 * frame.header.len = 0U;
 * const uint16_t csum = compute_checksum(frame.bytes, 12U);
 * @endcode
 * @see c6_hosted_link_t
 * @since 0.1.0
 */
typedef union c6_hosted_frame {
  uint8_t bytes[k_c6_hosted_frame_bytes];
  /**< The transaction as the transport sees it: flat bytes. */
  struct esp_payload_header header;
  /**< The same storage as the protocol sees it: a payload header. */
} c6_hosted_frame_t;

/**
 * @brief Write a NUL-terminated string to the board console.
 * @param[in] text String to emit; null is ignored rather than dereferenced
 *                 and the length is capped at ::k_c6_hosted_str_max.
 * @return Nothing.
 * @pre ``ra8_board_uart_console_init`` has succeeded.
 * @pre @p text is NUL-terminated within ::k_c6_hosted_str_max bytes.
 * @post The bytes are queued on the console transmitter.
 * @post No application state is modified.
 * @note Not thread-safe; only pre-kernel bring-up and the single worker
 *       thread call it, and those never overlap.
 * @see c6_hosted_put_u32
 * @since 0.1.0
 */
void c6_hosted_puts(const char* text);

/**
 * @brief Emit an unsigned 32-bit value in decimal.
 * @param[in] value Value to print; the whole 32-bit range is representable.
 * @return Nothing.
 * @pre The console is up.
 * @pre The caller wants no padding; zero prints as a single digit.
 * @post Between one and ::k_c6_hosted_dec_digits characters were emitted.
 * @post No application state is modified.
 * @note Not thread-safe, for the same reason as ::c6_hosted_puts.
 * @see c6_hosted_put_i32
 * @since 0.1.0
 */
void c6_hosted_put_u32(uint32_t value);

/**
 * @brief Emit a signed 32-bit value in decimal.
 * @param[in] value Value to print, including ``INT32_MIN``.
 * @return Nothing.
 * @pre The console is up.
 * @pre The caller accepts a leading minus sign on negative values.
 * @post One optional sign plus the decimal magnitude were emitted.
 * @post No application state is modified.
 * @note The magnitude is formed in unsigned arithmetic, so ``INT32_MIN``
 *       does not overflow on negation.
 * @see c6_hosted_put_u32
 * @since 0.1.0
 */
void c6_hosted_put_i32(int32_t value);

/**
 * @brief Emit a value as a fixed-width lower-case hexadecimal field.
 * @param[in] value  Value to print.
 * @param[in] digits Field width, 1..::k_c6_hosted_hex_digits; an
 *                   out-of-range width prints nothing rather than
 *                   overrunning the output array.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p digits is within 1..::k_c6_hosted_hex_digits.
 * @post Exactly @p digits characters were emitted, or none on a bad width.
 * @post No application state is modified.
 * @note The loop is bounded by the range-checked @p digits (NASA Rule 2).
 * @see c6_hosted_put_u32
 * @since 0.1.0
 */
void c6_hosted_put_hex(uint32_t value, uint8_t digits);

/**
 * @brief Print an ``H_GPIO_*_Port`` / ``H_GPIO_*_Pin`` pair as one field.
 * @param[in] label     Field name, printed verbatim; null prints nothing.
 * @param[in] gpio_port Opaque port handle -- an index in pointer clothing,
 *                      never dereferenced.
 * @param[in] gpio_pin  Pin index, or negative when the signal is unwired.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p gpio_port and @p gpio_pin are the two halves of one macro pair.
 * @post One field was emitted, ``=unwired`` for a negative pin.
 * @post No application state is modified.
 * @note This is the encoding the vendored driver itself consumes, so
 *       printing it shows what the driver will act on.
 * @see c6_hosted_print_pin
 * @since 0.1.0
 */
void c6_hosted_print_gpio(const char* label, const void* gpio_port, int32_t gpio_pin);

/**
 * @brief Print one packed board pin as ``<label>=port<n>.pin<n>``.
 * @param[in] label Field name, printed verbatim; null prints nothing.
 * @param[in] pin   Packed pin from ``ra8_esp_hosted_pins.h``, possibly the
 *                  ``k_ra8_pin_none`` sentinel.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p pin came from the port's pin table, never from a literal.
 * @post One field was emitted, ``=unwired`` for the sentinel.
 * @post No application state is modified.
 * @note Port and pin indices are decoded at run time, so this application
 *       states no board pin of its own.
 * @see c6_hosted_print_gpio
 * @since 0.1.0
 */
void c6_hosted_print_pin(const char* label, ra8_esp_hosted_pin_t pin);

/**
 * @brief Print the port identity, the clocks and the SPI parameters.
 * @param[in] cpuclk_hz Live CPUCLK0 rate in hertz.
 * @param[in] pclka_hz  Live PCLKA rate in hertz, the SCI baud-clock source.
 * @return Nothing.
 * @pre The console is up.
 * @pre Both rates were read from the CGC rather than assumed.
 * @post Three banner lines were emitted.
 * @post No application state is modified.
 * @note The SPI mode printed is the co-processor's: the C6 image in
 *       ``coprocessor/esp32c6/`` is built with ``CONFIG_ESP_SPI_MODE=3``
 *       and the port opens the bus to match.
 * @see c6_hosted_print_pin_map
 * @since 0.1.0
 */
void c6_hosted_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz);

/**
 * @brief Print the resolved pin map, bus pins and side-band alike.
 * @return Nothing.
 * @pre The console is up.
 * @pre ``ra8_esp_hosted_pins.h`` describes the harness currently fitted.
 * @post Four lines were emitted: bus pins, the driver's view of the
 *       side-band pair and the reset pin, then one route line per
 *       side-band signal.
 * @post No application state is modified.
 * @note Every value is decoded from the port's own headers at run time, so
 *       a harness change shows up here with no edit to this application.
 * @see c6_hosted_print_banner
 * @since 0.1.0
 */
void c6_hosted_print_pin_map(void);

/**
 * @brief Sample both side-band lines through the vtable and report them.
 * @return Nothing.
 * @pre ``ra8_esp_hosted_port_init`` returned ``k_ra8_ok``, so ``g_h.funcs``
 *      is populated.
 * @pre The console is up.
 * @post One line per side-band signal was emitted.
 * @post No application state is modified.
 * @note Each level is compared against the active level the port config
 *       derives, so the line says "asserted" rather than leaving the reader
 *       to recall the polarity.
 * @see c6_hosted_print_pin_map
 * @since 0.1.0
 */
void c6_hosted_report_sideband(void);

/**
 * @brief Report an event the port delivered from the vendored core.
 * @param[in] ctx      Registered context; unused, the counter is file-scope.
 * @param[in] base     Event namespace; never null per the port contract.
 * @param[in] event_id Identifier within that namespace.
 * @param[in] data     Payload, or null; not decoded here.
 * @param[in] data_len Payload length in bytes.
 * @return Nothing.
 * @pre The console is up.
 * @pre The port has been told about this handler.
 * @post The event counter was incremented.
 * @post One line naming the base and the identifier was emitted.
 * @note Runs on the posting thread, so it only formats: it never blocks and
 *       never calls back into the transport.
 * @see c6_hosted_event_count
 * @since 0.1.0
 */
void c6_hosted_on_event(void*       ctx,
                        const char* base,
                        int32_t     event_id,
                        const void* data,
                        size_t      data_len);

/**
 * @brief Report how many events ::c6_hosted_on_event has seen.
 * @return The running count, which starts at zero and never resets.
 * @retval 0 The co-processor has never announced anything.
 * @pre None; safe to call before any event has arrived.
 * @pre The caller tolerates a value a concurrent post may stale.
 * @post No application state is modified.
 * @post The returned value reflects the counter at the moment of the read.
 * @note Diagnostic only; a single aligned load, not an atomic read.
 * @see c6_hosted_on_event
 * @since 0.1.0
 */
uint32_t c6_hosted_event_count(void);

/**
 * @brief Clock one full-duplex transaction and report the verdict.
 * @return Nothing.
 * @pre ``ra8_esp_hosted_port_init`` returned ``k_ra8_ok``.
 * @pre The console is up.
 * @post Exactly one transfer line, one header line and one verdict line
 *       were emitted.
 * @post The receive buffer holds whatever the co-processor drove.
 * @note Runs once, on the worker thread. The first completed transaction
 *       drains the co-processor's queued boot event for good, so a rerun
 *       needs the C6 reset.
 * @see c6_hosted_report_sideband
 * @since 0.1.0
 */
void c6_hosted_run_transaction(void);
