/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_wifi_link/inc/c6_wifi.h
 * @brief Shared contract for the `ra8_c6link` facade bring-up application.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * ``c6_fw_version`` proved one esp-hosted RPC round-trip by hand-building the
 * request inside the application. This application proves the same wire through
 * ``libs/ra8_c6link``, the facade every consumer is meant to use, and then goes
 * one step further: it brings the co-processor's Wi-Fi station up and reads back
 * its MAC address. That is the first thing #492's NetX Duo glue will do, and it
 * is the part of the control plane a host test cannot settle -- whether the
 * `Req_WifiInit` configuration this host transmits is one the co-processor's own
 * build accepts.
 *
 * Two modules, both driven by ``main.c``:
 *
 *   - ``src/c6_wifi_console.c`` -- bounded console formatters, so the image
 *     links no newlib ``printf``.
 *   - ``main.c``                -- bring-up, the four phases, and the verdict.
 *
 * Everything protocol-shaped lives in the facade. This file holds only what an
 * application genuinely owns: pacing, buffer bounds and formatter widths.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_c6link.h"
#include "ra8_err.h"

/**
 * @enum c6_wifi_cfg_t
 * @brief Link, thread and pacing parameters this application chooses.
 * @details Only values an application owns live here. NASA Power of 10 Rule 3
 * forbids allocation after initialisation and this image has no heap, so the
 * worker stack and the decode arena are sized here alongside the link timing.
 * @invariant ::k_c6_wifi_sck_hz is the bit rate ``c6_fw_version`` completed an
 *            RPC round-trip at on 2026-07-28, so this application changes one
 *            variable at a time relative to that run.
 * @invariant ::k_c6_wifi_arena_bytes is at least ::k_ra8_c6link_arena_min,
 *            which ::ra8_c6link_open enforces.
 * @par Example:
 * @code
 * cfg.sck_hz = (uint32_t)k_c6_wifi_sck_hz;
 * @endcode
 * @see ra8_esp_hosted_port_cfg_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_c6_wifi_uart_baud    = 115200U,  /**< Console rate, 8N1, over the J-Link OB VCOM. */
  k_c6_wifi_sck_hz       = 5000000U, /**< SPI bit rate; the rate the RPC round-trip
                                     ran at on silicon.                              */
  k_c6_wifi_edge_poll_ms = 2U,       /**< Poll period for a side-band pin with no ICU
                                     channel.                                        */
  k_c6_wifi_boot_wait_ms = 200U,     /**< Settling delay before the first transaction. */
  k_c6_wifi_heartbeat_ms = 5000U,    /**< Heartbeat gap after the verdict, in
                                     milliseconds and therefore in ThreadX ticks.    */
  k_c6_wifi_worker_stack = 8192U,    /**< Worker-thread stack, in bytes. The protobuf
                                     decoder recurses through nested messages.       */
  k_c6_wifi_worker_prio  = 8U,       /**< Worker priority and preemption threshold. */
  k_c6_wifi_arena_bytes  = 4096U,    /**< Decode arena handed to the facade. Double
                                     the enforced minimum, because an AP record
                                     carries nested country and HE sub-messages.     */
} c6_wifi_cfg_t;

/**
 * @enum c6_wifi_fmt_t
 * @brief Bounds for the console formatters in ``src/c6_wifi_console.c``.
 * @details The image links no newlib ``printf``, so the serialisers do their own
 * digit extraction; every loop they run is bounded by a value from this
 * enumeration, which is what satisfies NASA Power of 10 Rule 2.
 * @invariant ::k_c6_wifi_dec_digits holds the widest 32-bit decimal value.
 * @invariant ::k_c6_wifi_hex_digits holds the widest 32-bit hex value.
 * @par Example:
 * @code
 * c6_wifi_put_hex(chip_id, (uint8_t)k_c6_wifi_hex_byte);
 * @endcode
 * @see c6_wifi_put_u32
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_c6_wifi_str_max    = 256U,  /**< Longest string the console helper emits.   */
  k_c6_wifi_dec_radix  = 10U,   /**< Decimal radix.                             */
  k_c6_wifi_dec_digits = 10U,   /**< Digits in the widest 32-bit decimal value. */
  k_c6_wifi_hex_digits = 8U,    /**< Digits in the widest 32-bit hex value.     */
  k_c6_wifi_hex_bits   = 4U,    /**< Bits per hexadecimal digit.                */
  k_c6_wifi_hex_mask   = 0x0FU, /**< Nibble mask.                               */
  k_c6_wifi_hex_alpha  = 10U,   /**< First nibble value spelled with a letter.  */
  k_c6_wifi_hex_byte   = 2U,    /**< Hex digits printed for a byte-wide field.  */
  k_c6_wifi_text_max   = 32U,   /**< Longest co-processor-supplied string echoed
                                 to the console; anything longer is truncated
                                 rather than trusted.                            */
} c6_wifi_fmt_t;

/**
 * @brief Write a NUL-terminated string to the board console.
 * @param[in] text String to emit; null is ignored rather than dereferenced and
 *                 the length is capped at ::k_c6_wifi_str_max.
 * @return Nothing.
 * @pre ``ra8_board_uart_console_init`` has succeeded.
 * @pre @p text is NUL-terminated within ::k_c6_wifi_str_max bytes.
 * @post The bytes are queued on the console transmitter.
 * @post No application state is modified.
 * @note Not thread-safe; only pre-kernel bring-up and the single worker thread
 *       call it, and those never overlap.
 * @see c6_wifi_put_u32
 * @since 0.1.0
 */
void c6_wifi_puts(const char* text);

/**
 * @brief Emit an unsigned 32-bit value in decimal.
 * @param[in] value Value to print; the whole 32-bit range is representable.
 * @return Nothing.
 * @pre The console is up.
 * @pre The caller wants no padding; zero prints as a single digit.
 * @post Between one and ::k_c6_wifi_dec_digits characters were emitted.
 * @post No application state is modified.
 * @note Not thread-safe, for the same reason as ::c6_wifi_puts.
 * @see c6_wifi_put_i32
 * @since 0.1.0
 */
void c6_wifi_put_u32(uint32_t value);

/**
 * @brief Emit a signed 32-bit value in decimal.
 * @param[in] value Value to print, including ``INT32_MIN``.
 * @return Nothing.
 * @pre The console is up.
 * @pre The caller accepts a leading minus sign on negative values.
 * @post One optional sign plus the decimal magnitude were emitted.
 * @post No application state is modified.
 * @note The magnitude is formed in unsigned arithmetic, so ``INT32_MIN`` does
 *       not overflow on negation.
 * @see c6_wifi_put_u32
 * @since 0.1.0
 */
void c6_wifi_put_i32(int32_t value);

/**
 * @brief Emit a value as a fixed-width lower-case hexadecimal field.
 * @param[in] value Value to print.
 * @param[in] digits Field width, 1..::k_c6_wifi_hex_digits; an out-of-range
 *                   width prints nothing rather than overrunning the output
 *                   array.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p digits is within 1..::k_c6_wifi_hex_digits.
 * @post Exactly @p digits characters were emitted, or none on a bad width.
 * @post No application state is modified.
 * @note The loop is bounded by the range-checked @p digits (NASA Rule 2).
 * @see c6_wifi_put_u32
 * @since 0.1.0
 */
void c6_wifi_put_hex(uint32_t value, uint8_t digits);

/**
 * @brief Emit a NUL-terminated co-processor string as printable text.
 * @details The co-processor supplies the bytes, so nothing about them is
 * trusted: the string is truncated to ::k_c6_wifi_text_max and every byte
 * outside printable ASCII is replaced with a full stop. A protocol field is
 * evidence, and evidence that can reprogram a terminal is not evidence.
 * @param[in] text Bytes to emit; null prints nothing.
 * @param[in] len Number of bytes available at @p text.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p len bytes are readable at @p text.
 * @post At most ::k_c6_wifi_text_max characters were emitted.
 * @post No application state is modified.
 * @note The loop is bounded by ::k_c6_wifi_text_max (NASA Rule 2).
 * @see c6_wifi_puts
 * @since 0.1.0
 */
void c6_wifi_put_text(const char* text, size_t len);

/**
 * @brief Emit an IEEE 802 address as six colon-separated hexadecimal octets.
 * @param[in] mac Address to print; null prints nothing.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p mac was filled by ::ra8_c6link_wifi_mac or is the zero address.
 * @post Seventeen characters were emitted, or none on a null argument.
 * @post No application state is modified.
 * @note The loop is bounded by ::k_ra8_c6link_mac_bytes (NASA Rule 2).
 * @see ra8_c6link_wifi_mac
 * @since 0.1.0
 */
void c6_wifi_put_mac(const ra8_c6link_mac_t* mac);

/**
 * @brief Print the banner: identity, clocks and link parameters.
 * @param[in] cpuclk_hz Live CPUCLK0 rate in hertz.
 * @param[in] pclka_hz Live PCLKA rate in hertz, the SCI baud-clock source.
 * @return Nothing.
 * @pre The console is up.
 * @pre Both rates were read from the CGC rather than assumed.
 * @post Three banner lines were emitted.
 * @post No application state is modified.
 * @note The SPI mode is the co-processor's: the C6 image in
 *       ``coprocessor/esp32c6/`` is built with ``CONFIG_ESP_SPI_MODE=3`` and
 *       the port opens the bus to match.
 * @see c6_wifi_print_stats
 * @since 0.1.0
 */
void c6_wifi_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz);

/**
 * @brief Print what one facade poll did, as one console line.
 * @param[in] label Field name printed before the counters; null prints nothing
 *                  at all rather than being dereferenced.
 * @param[in] stats Counters filled in by ::ra8_c6link_poll; null prints nothing.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p stats came from a completed poll.
 * @post Exactly one line was emitted, or none on a null argument.
 * @post No application state is modified.
 * @note Every counter is printed, including the zero ones: a zero
 *       ``bad_checksum`` beside a non-zero ``transfers`` is itself the evidence
 *       that the link is clean.
 * @see ra8_c6link_stats
 * @since 0.1.0
 */
void c6_wifi_print_stats(const char* label, const ra8_c6link_stats_t* stats);
