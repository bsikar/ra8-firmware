/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_fw_version/inc/c6_fwver.h
 * @brief Shared contract for the esp-hosted RPC round-trip application.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * ``c6_hosted_init`` proved one transaction reaches the co-processor and
 * comes back framed. This application proves the layer above it: a request
 * that goes up, is parsed by the co-processor, and is answered with a
 * populated response whose fields are then checked. The request chosen is
 * ``RPC_ID__Req_GetCoprocessorFwVersion``, because its answer is a fact this
 * side already knows independently -- the vendored host driver's own version
 * -- so the verdict can be a comparison rather than a shrug.
 *
 * One module per concern, all driven by ``main.c``:
 *
 *   - ``src/c6_fwver_console.c`` -- bounded console formatters, so the image
 *     links no newlib ``printf``.
 *   - ``src/c6_fwver_link.c``    -- the transaction pump: build a payload
 *     header around a payload, clock full-duplex transactions through the
 *     port's ``_h_do_bus_transfer``, validate each received frame and hand
 *     the good ones to a sink.
 *   - ``src/c6_fwver_priv.c``    -- the ESP_PRIV_IF control channel: the
 *     host-capabilities frame the reference host sends first, and the decode
 *     of the co-processor's boot INIT event.
 *   - ``src/c6_fwver_rpc.c``     -- the ESP_SERIAL_IF RPC channel: the
 *     protobuf request, its TLV envelope, the response decode and the
 *     verdict.
 *
 * Every protocol size and identifier is read from the vendored esp-hosted
 * headers rather than restated, and every pin identity is read from
 * ``port/esp-hosted/inc/ra8_esp_hosted_pins.h``. This file holds only what
 * an application genuinely owns: pacing, buffer bounds and formatter widths.
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
#include "ra8_err.h"

/**
 * @enum c6_fwver_cfg_t
 * @brief Link, thread and pacing parameters this application chooses.
 * @details Only values an application owns live here. NASA Power of 10 Rule 3
 * forbids allocation after initialisation and this image has no heap, so the
 * worker stack and the transaction-buffer alignment are sized here alongside
 * the link timing.
 * @invariant ::k_c6_fwver_sck_hz is the bit rate ``c6_hosted_init`` was proven
 *            at on 2026-07-28, so this application changes one variable at a
 *            time relative to that run.
 * @invariant ::k_c6_fwver_edge_poll_ms is non-zero; the port's software edge
 *            detector arms a ThreadX timer, which rejects a zero tick.
 * @par Example:
 * @code
 * cfg.sck_hz = (uint32_t)k_c6_fwver_sck_hz;
 * @endcode
 * @see ra8_esp_hosted_port_cfg_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_c6_fwver_uart_baud    = 115200U,  /**< Console rate, 8N1, over the J-Link OB VCOM. */
  k_c6_fwver_sck_hz       = 5000000U, /**< SPI bit rate; the rate the port ran at on
                                      silicon in the c6_hosted_init bring-up.          */
  k_c6_fwver_edge_poll_ms = 2U,       /**< Poll period for a side-band pin with no ICU
                                      channel.                                         */
  k_c6_fwver_boot_wait_ms = 200U,     /**< Settling delay before the first transaction. */
  k_c6_fwver_heartbeat_ms = 5000U,    /**< Heartbeat gap after the verdict, in
                                      milliseconds and therefore in ThreadX ticks.     */
  k_c6_fwver_dma_align    = 64U,      /**< Transaction-buffer alignment, in bytes. */
  k_c6_fwver_worker_stack = 8192U,    /**< Worker-thread stack, in bytes. The protobuf
                                      decoder recurses through nested messages, so this
                                      is double what c6_hosted_init needs.             */
  k_c6_fwver_worker_prio  = 8U,       /**< Worker priority and preemption threshold. */
  k_c6_fwver_rpc_uid      = 1U,       /**< UID stamped on the request; the response
                                      must carry it back unchanged.                    */
} c6_fwver_cfg_t;

/**
 * @enum c6_fwver_fmt_t
 * @brief Bounds for the console formatters in ``src/c6_fwver_console.c``.
 * @details The image links no newlib ``printf``, so the serialisers do their
 * own digit extraction; every loop they run is bounded by a value from this
 * enumeration, which is what satisfies NASA Power of 10 Rule 2.
 * @invariant ::k_c6_fwver_dec_digits holds the widest 32-bit decimal value.
 * @invariant ::k_c6_fwver_hex_digits holds the widest 32-bit hex value.
 * @par Example:
 * @code
 * c6_fwver_put_hex(csum, (uint8_t)k_c6_fwver_hex_word);
 * @endcode
 * @see c6_fwver_put_u32
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_c6_fwver_str_max    = 256U,  /**< Longest string the console helper emits.   */
  k_c6_fwver_dec_radix  = 10U,   /**< Decimal radix.                             */
  k_c6_fwver_dec_digits = 10U,   /**< Digits in the widest 32-bit decimal value. */
  k_c6_fwver_hex_digits = 8U,    /**< Digits in the widest 32-bit hex value.     */
  k_c6_fwver_hex_bits   = 4U,    /**< Bits per hexadecimal digit.                */
  k_c6_fwver_hex_mask   = 0x0FU, /**< Nibble mask.                               */
  k_c6_fwver_hex_alpha  = 10U,   /**< First nibble value spelled with a letter.  */
  k_c6_fwver_hex_byte   = 2U,    /**< Hex digits printed for a byte-wide field.  */
  k_c6_fwver_hex_word   = 4U,    /**< Hex digits printed for a 16-bit field.     */
  k_c6_fwver_text_max   = 32U,   /**< Longest co-processor-supplied string echoed
                                 to the console; anything longer is truncated
                                 rather than trusted.                            */
} c6_fwver_fmt_t;

/**
 * @enum c6_fwver_link_t
 * @brief Frame geometry and pump budgets the transaction layer works to.
 * @details The frame size is read from the vendored transport header rather
 * than restated. The budgets are this application's own: they exist so every
 * loop in the pump has a statically provable bound (NASA Power of 10 Rule 2)
 * and so a co-processor that never answers produces a verdict instead of a
 * hang.
 * @invariant ::k_c6_fwver_frame_bytes is exactly the byte count the
 *            co-processor clocks in one full-duplex transaction.
 * @invariant ::k_c6_fwver_max_transfers x ::k_c6_fwver_hs_wait_ms bounds the
 *            worst-case wall time of one pump, so the verdict always arrives.
 * @par Example:
 * @code
 * (void)c6_fwver_link_pump(ESP_SERIAL_IF, 0U, req, req_len,
 *                          (uint16_t)k_c6_fwver_max_transfers, sink, &stats);
 * @endcode
 * @see c6_fwver_link_pump
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_c6_fwver_frame_bytes = (uint16_t)MAX_TRANSPORT_BUFFER_SIZE,
  /**< Bytes clocked in one full-duplex transaction. */
  k_c6_fwver_max_transfers = 64U,
  /**< Transactions one pump may clock before giving up. */
  k_c6_fwver_hs_wait_ms = 200U,
  /**< Milliseconds to wait for HANDSHAKE before abandoning a transaction. */
  k_c6_fwver_hs_giveup = 3U,
  /**< Consecutive HANDSHAKE timeouts after which the pump stops. A
       co-processor that has not armed the line three times running is not
       there, and spending the rest of the transaction budget re-asking proves
       nothing while turning "absent" into a half-minute hang -- which is what
       a bench with the harness unplugged, and `ra8_emulator` with no C6
       modelled at all, both look like. */
  k_c6_fwver_hs_poll_ms = 1U,
  /**< Sampling period while waiting for HANDSHAKE. */
  k_c6_fwver_gap_ms = 2U,
  /**< Settling gap between consecutive transactions. */
  k_c6_fwver_tx_max = 512U,
  /**< Bound on any payload this application transmits: the packed RPC
       request and its TLV envelope are two orders of magnitude smaller,
       and the host-capabilities frame is fifteen bytes. */
} c6_fwver_link_t;

/**
 * @union c6_fwver_frame
 * @brief One esp-hosted transaction buffer, addressable either way.
 * @details The transport clocks a flat byte array; the protocol reads a
 * twelve-byte payload header off the front of it. Declaring both as members
 * of one union makes that overlay legal by construction -- union member access
 * is defined in C, whereas casting the byte array to a header pointer is not,
 * and clang-tidy's ``bugprone-casting-through-void`` says so.
 * @invariant ``bytes`` and ``header`` name the same storage, so a write
 *            through either is visible through the other.
 * @invariant The union is exactly ::k_c6_fwver_frame_bytes long, because the
 *            header member is smaller than the byte member.
 * @par Example:
 * @code
 * static c6_fwver_frame_t frame;
 * frame.header.len = 0U;
 * @endcode
 * @see c6_fwver_link_t
 * @since 0.1.0
 */
typedef union c6_fwver_frame {
  uint8_t bytes[k_c6_fwver_frame_bytes];
  /**< The transaction as the transport sees it: flat bytes. */
  struct esp_payload_header header;
  /**< The same storage as the protocol sees it: a payload header. */
} c6_fwver_frame_t;

/**
 * @struct c6_fwver_pump_stats
 * @brief What one call to ::c6_fwver_link_pump actually did on the wire.
 * @details Counted rather than inferred, because the difference between "the
 * co-processor sent nothing" and "the co-processor sent frames this host
 * rejected" is the difference between a bench fault and a firmware fault, and
 * a bare pass/fail cannot tell them apart.
 * @invariant ``frames + idle + bad_checksum + malformed`` never exceeds
 *            ``transfers``: every counted frame came from one transaction.
 * @invariant ``transfers`` never exceeds the ``max_transfers`` the caller
 *            passed.
 * @par Example:
 * @code
 * c6_fwver_pump_stats_t stats = {};
 * (void)c6_fwver_link_pump(..., &stats);
 * c6_fwver_put_u32(stats.transfers);
 * @endcode
 * @see c6_fwver_link_pump
 * @since 0.1.0
 */
typedef struct c6_fwver_pump_stats {
  uint16_t transfers;    /**< Full-duplex transactions clocked.                    */
  uint16_t frames;       /**< Well-formed payload frames handed to the sink.       */
  uint16_t idle;         /**< Filler frames the co-processor returned (len zero).  */
  uint16_t bad_checksum; /**< Frames whose recomputed checksum disagreed.          */
  uint16_t malformed;    /**< Frames whose offset or length could not be believed. */
  uint16_t hs_timeouts;  /**< Transactions abandoned waiting for HANDSHAKE.        */
  uint16_t ifnum_defect; /**< Bad-checksum frames explained by the co-processor's
                              ``if_num`` accounting; see the app README.           */
  bool     bus_error;    /**< A bus transfer did not return ``RET_OK``. */
  bool     sink_stopped; /**< The sink asked the pump to stop early.    */
} c6_fwver_pump_stats_t;

/**
 * @typedef c6_fwver_sink_t
 * @brief Receiver for one well-formed frame the co-processor sent.
 * @details A function pointer rather than a fixed call keeps the transaction
 * layer ignorant of the protocol layers above it (Dependency Inversion; NASA
 * Power of 10 Rule 9 permits function pointers for exactly this).
 * @param[in] if_type Interface type from the received payload header.
 * @param[in] if_num Interface number from the received payload header.
 * @param[in] payload Frame payload; never null, never zero length.
 * @param[in] len Payload length in bytes, at most ``MAX_PAYLOAD_SIZE``.
 * @return ``true`` to stop the pump, ``false`` to keep clocking.
 * @note Runs on the pumping thread and must not start a transaction of its
 *       own; the bus is in use for the duration of the call.
 * @since 0.1.0
 */
typedef bool (*c6_fwver_sink_t)(uint8_t        if_type,
                                uint8_t        if_num,
                                const uint8_t* payload,
                                uint16_t       len);

/**
 * @brief Write a NUL-terminated string to the board console.
 * @param[in] text String to emit; null is ignored rather than dereferenced and
 *                 the length is capped at ::k_c6_fwver_str_max.
 * @return Nothing.
 * @pre ``ra8_board_uart_console_init`` has succeeded.
 * @pre @p text is NUL-terminated within ::k_c6_fwver_str_max bytes.
 * @post The bytes are queued on the console transmitter.
 * @post No application state is modified.
 * @note Not thread-safe; only pre-kernel bring-up and the single worker thread
 *       call it, and those never overlap.
 * @see c6_fwver_put_u32
 * @since 0.1.0
 */
void c6_fwver_puts(const char* text);

/**
 * @brief Emit an unsigned 32-bit value in decimal.
 * @param[in] value Value to print; the whole 32-bit range is representable.
 * @return Nothing.
 * @pre The console is up.
 * @pre The caller wants no padding; zero prints as a single digit.
 * @post Between one and ::k_c6_fwver_dec_digits characters were emitted.
 * @post No application state is modified.
 * @note Not thread-safe, for the same reason as ::c6_fwver_puts.
 * @see c6_fwver_put_i32
 * @since 0.1.0
 */
void c6_fwver_put_u32(uint32_t value);

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
 * @see c6_fwver_put_u32
 * @since 0.1.0
 */
void c6_fwver_put_i32(int32_t value);

/**
 * @brief Emit a value as a fixed-width lower-case hexadecimal field.
 * @param[in] value Value to print.
 * @param[in] digits Field width, 1..::k_c6_fwver_hex_digits; an out-of-range
 *                   width prints nothing rather than overrunning the output
 *                   array.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p digits is within 1..::k_c6_fwver_hex_digits.
 * @post Exactly @p digits characters were emitted, or none on a bad width.
 * @post No application state is modified.
 * @note The loop is bounded by the range-checked @p digits (NASA Rule 2).
 * @see c6_fwver_put_u32
 * @since 0.1.0
 */
void c6_fwver_put_hex(uint32_t value, uint8_t digits);

/**
 * @brief Emit a length-counted byte range as printable text.
 * @details The co-processor supplies the bytes, so nothing about them is
 * trusted: the range is truncated to ::k_c6_fwver_text_max and every byte
 * outside printable ASCII is replaced with a full stop. A protocol field is
 * evidence, and evidence that can reprogram a terminal is not evidence.
 * @param[in] text Bytes to emit; null prints nothing.
 * @param[in] len Number of bytes available at @p text.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p len bytes are readable at @p text.
 * @post At most ::k_c6_fwver_text_max characters were emitted.
 * @post No application state is modified.
 * @note The loop is bounded by ::k_c6_fwver_text_max (NASA Rule 2).
 * @see c6_fwver_puts
 * @since 0.1.0
 */
void c6_fwver_put_text(const uint8_t* text, size_t len);

/**
 * @brief Print the banner: identity, clocks and SPI parameters.
 * @param[in] cpuclk_hz Live CPUCLK0 rate in hertz.
 * @param[in] pclka_hz Live PCLKA rate in hertz, the SCI baud-clock source.
 * @return Nothing.
 * @pre The console is up.
 * @pre Both rates were read from the CGC rather than assumed.
 * @post Three banner lines were emitted.
 * @post No application state is modified.
 * @note The SPI mode printed is the co-processor's: the C6 image in
 *       ``coprocessor/esp32c6/`` is built with ``CONFIG_ESP_SPI_MODE=3`` and
 *       the port opens the bus to match.
 * @see c6_fwver_link_pump
 * @since 0.1.0
 */
void c6_fwver_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz);

/**
 * @brief Print what one pump did, as one console line.
 * @param[in] label Field name printed before the counters; null prints
 *                  nothing at all rather than being dereferenced.
 * @param[in] stats Counters filled in by ::c6_fwver_link_pump; null prints
 *                  nothing.
 * @return Nothing.
 * @pre The console is up.
 * @pre @p stats came from a completed pump.
 * @post Exactly one line was emitted, or none on a null argument.
 * @post No application state is modified.
 * @note Every counter is printed, including the zero ones: a zero
 *       ``bad_checksum`` beside a non-zero ``transfers`` is itself the
 *       evidence that the link is clean.
 * @see c6_fwver_pump_stats
 * @since 0.1.0
 */
void c6_fwver_print_pump(const char* label, const c6_fwver_pump_stats_t* stats);

/**
 * @brief Clock full-duplex transactions until the sink is satisfied.
 *
 * @details
 * The pump is this application's stand-in for the vendored
 * ``spi_transaction_task``, reduced to what a bring-up needs: it transmits at
 * most one payload, then keeps clocking filler frames so the co-processor has
 * transactions to answer in. Each iteration waits for HANDSHAKE, builds either
 * the pending payload frame or an ``ESP_MAX_IF`` filler, clocks
 * ``_h_do_bus_transfer``, and classifies what came back. Frames that are
 * well-formed and carry a payload go to @p sink; filler and malformed frames
 * are counted and dropped.
 *
 * @param[in] if_type Interface type for the transmitted payload, from
 *                    ``esp_hosted_if_type_t``. Ignored when @p payload_len is
 *                    zero.
 * @param[in] if_num Interface number for the transmitted payload, 0..15.
 * @param[in] payload Payload to transmit once, or null to pump filler only.
 * @param[in] payload_len Length of @p payload in bytes; must be zero when
 *                        @p payload is null and at most ::k_c6_fwver_tx_max
 *                        otherwise.
 * @param[in] max_transfers Transactions this call may clock; must be non-zero.
 * @param[in] sink Receiver for well-formed frames, or null to count only.
 * @param[out] stats Counters describing the run; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The pump ran; @p stats says what happened.
 * @retval k_ra8_err_null_ptr @p stats was null, or @p payload_len was non-zero
 *         with a null @p payload.
 * @retval k_ra8_err_invalid_arg @p payload_len exceeds ::k_c6_fwver_tx_max, or
 *         @p max_transfers was zero.
 * @retval k_ra8_err_not_initialized The esp-hosted port is not up, so the
 *         vtable behind ``g_h`` cannot be called.
 * @retval k_ra8_err_hw_timeout HANDSHAKE never went active, so not one
 *         transaction was clocked.
 * @retval k_ra8_err_spi_error A bus transfer did not return ``RET_OK``.
 *
 * @pre ``ra8_esp_hosted_port_init`` returned ``k_ra8_ok``.
 * @pre No other context is driving the SPI bus.
 * @post At most @p max_transfers transactions were clocked.
 * @post @p stats holds the counts for exactly this call.
 *
 * @note Not thread-safe; one pump at a time owns the bus.
 * @warning The sink runs inside the pump, so it must not start a transaction.
 *
 * @par Example:
 * @code
 * c6_fwver_pump_stats_t stats = {};
 * (void)c6_fwver_link_pump((uint8_t)ESP_SERIAL_IF, 0U, req, req_len,
 *                          (uint16_t)k_c6_fwver_max_transfers,
 *                          c6_fwver_dispatch, &stats);
 * @endcode
 *
 * @see c6_fwver_pump_stats
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: the transaction loop is bounded by @p max_transfers and the
 *   HANDSHAKE wait by ::k_c6_fwver_hs_wait_ms.
 * - Rule 5: four preconditions and two postconditions are checked.
 */
[[nodiscard]] ra8_err_t c6_fwver_link_pump(uint8_t                if_type,
                                           uint8_t                if_num,
                                           const uint8_t*         payload,
                                           uint16_t               payload_len,
                                           uint16_t               max_transfers,
                                           c6_fwver_sink_t        sink,
                                           c6_fwver_pump_stats_t* stats);

/**
 * @brief Build the host-capabilities frame the reference host sends first.
 * @details Byte-for-byte the TLV set the vendored ``transport_drv.c`` composes
 * in ``send_slave_config()``  LEGACY-OK: upstream esp-hosted function name
 * -- host capabilities, the firmware chip id the host expects, the
 * raw-throughput direction, and the two flow-control thresholds, behind an
 * ``ESP_PRIV_EVENT_INIT`` event header.
 * @param[out] out Buffer to fill; must be non-null.
 * @param[in] cap Bytes available at @p out.
 * @param[out] out_len Bytes written; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The frame was built and @p out_len is its length.
 * @retval k_ra8_err_null_ptr @p out or @p out_len was null.
 * @retval k_ra8_err_invalid_size @p cap cannot hold the frame.
 * @pre @p cap bytes are writable at @p out.
 * @pre The caller transmits the result on ``ESP_PRIV_IF``, interface 0.
 * @post On success @p out_len is non-zero and at most @p cap.
 * @post On failure @p out is not modified.
 * @note Pure formatting; touches no hardware and is safe from any thread.
 * @see c6_fwver_priv_consume
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t c6_fwver_priv_host_caps(uint8_t* out, uint16_t cap, uint16_t* out_len);

/**
 * @brief Decode an ``ESP_PRIV_IF`` frame and report what it announced.
 * @details Walks the TLV list of an ``ESP_PRIV_EVENT_INIT`` event, printing
 * the capability word, the firmware chip id and -- the interesting one -- the
 * ``ESP_PRIV_FIRMWARE_VERSION`` tag, which carries the co-processor's own
 * version as a little-endian 32-bit word. That is an independent second
 * reading of the number the RPC round-trip asks for.
 * @param[in] if_type Interface type from the received header.
 * @param[in] if_num Interface number from the received header.
 * @param[in] payload Frame payload; null is ignored.
 * @param[in] len Payload length in bytes.
 * @return ``false`` always: an announcement is never a reason to stop pumping.
 * @retval false The frame was decoded, ignored, or was not a priv frame.
 * @pre The console is up.
 * @pre @p len bytes are readable at @p payload.
 * @post At most one line was emitted per recognised TLV.
 * @post The recorded init-event version is updated when the tag is present.
 * @note Matches ::c6_fwver_sink_t so the pump can call it directly.
 * @see c6_fwver_priv_init_version
 * @since 0.1.0
 */
bool c6_fwver_priv_consume(uint8_t if_type, uint8_t if_num, const uint8_t* payload, uint16_t len);

/**
 * @brief Report the version the boot INIT event announced, if one arrived.
 * @return The packed ``(major << 16) | (minor << 8) | patch`` word, or zero
 *         when no INIT event carrying the tag has been decoded.
 * @retval 0 No ``ESP_PRIV_FIRMWARE_VERSION`` tag has been seen.
 * @pre None; safe to call before any frame has arrived.
 * @pre The caller treats zero as "not observed", not as version 0.0.0.
 * @post No application state is modified.
 * @post The returned value reflects the last INIT event decoded.
 * @note The co-processor queues this event once per boot, so a run that
 *       follows an earlier run without power-cycling the C6 will not see it.
 * @see c6_fwver_priv_consume
 * @since 0.1.0
 */
[[nodiscard]] uint32_t c6_fwver_priv_init_version(void);

/**
 * @brief Build the TLV-wrapped RPC firmware-version request.
 * @details Packs an ``Rpc`` protobuf message carrying
 * ``RPC_ID__Req_GetCoprocessorFwVersion`` with the vendored generated codec,
 * then wraps it in the two-tag TLV envelope the co-processor's serial endpoint
 * expects (endpoint name, then data), exactly as ``compose_tlv()`` does in the
 * vendored ``serial_if.c``.
 * @param[out] out Buffer to fill; must be non-null.
 * @param[in] cap Bytes available at @p out.
 * @param[out] out_len Bytes written; must be non-null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The request was built and @p out_len is its length.
 * @retval k_ra8_err_null_ptr @p out or @p out_len was null.
 * @retval k_ra8_err_invalid_size @p cap cannot hold the envelope.
 * @retval k_ra8_err_validation_failed The generated codec packed a different number of
 *         bytes than it predicted.
 * @pre @p cap bytes are writable at @p out.
 * @pre The caller transmits the result on ``ESP_SERIAL_IF``, interface 0.
 * @post On success @p out_len is non-zero and at most @p cap.
 * @post On failure @p out_len is zero.
 * @note Pure formatting; touches no hardware and is safe from any thread.
 * @see c6_fwver_rpc_consume
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t c6_fwver_rpc_request(uint8_t* out, uint16_t cap, uint16_t* out_len);

/**
 * @brief Decode an ``ESP_SERIAL_IF`` frame as the awaited RPC response.
 * @details Unwraps the TLV envelope, unpacks the ``Rpc`` message with the
 * vendored codec, and -- only when it is the response to this application's
 * request -- records every field for the verdict.
 * @param[in] if_type Interface type from the received header.
 * @param[in] if_num Interface number from the received header.
 * @param[in] payload Frame payload; null is ignored.
 * @param[in] len Payload length in bytes.
 * @return ``true`` when the awaited response has been recorded and the pump
 *         should stop.
 * @retval true The response to this request arrived and was decoded.
 * @retval false The frame was something else, or could not be decoded.
 * @pre The console is up.
 * @pre @p len bytes are readable at @p payload.
 * @post On ``true`` the recorded response is complete and ::c6_fwver_rpc_report
 *       can judge it.
 * @post No frame is decoded twice: the first accepted response wins.
 * @note Matches ::c6_fwver_sink_t so the pump can call it directly.
 * @see c6_fwver_rpc_report
 * @since 0.1.0
 */
bool c6_fwver_rpc_consume(uint8_t if_type, uint8_t if_num, const uint8_t* payload, uint16_t len);

/**
 * @brief Print the recorded response and decide the run's verdict.
 * @details Prints every decoded field, then the version this host expects, and
 * then exactly one ``PASS``/``FAIL`` line. The expectation is the vendored
 * host driver's own version from ``esp_hosted_host_fw_ver.h``, so the check is
 * the host/co-processor version lock rather than a literal written twice.
 * @return ``true`` when the response arrived and every checked field matched.
 * @retval true The co-processor answered with the expected version.
 * @retval false No response arrived, or a field did not match.
 * @pre The console is up.
 * @pre A pump has run, so a response either arrived or provably did not.
 * @post Exactly one verdict line was emitted.
 * @post No application state is modified.
 * @note The verdict is printed here rather than returned only, so the console
 *       capture that CI greps and the value the code sees cannot disagree.
 * @see c6_fwver_rpc_consume
 * @since 0.1.0
 */
bool c6_fwver_rpc_report(void);

/**
 * @brief Route a received frame to the module that understands it.
 * @param[in] if_type Interface type from the received header.
 * @param[in] if_num Interface number from the received header.
 * @param[in] payload Frame payload; null is ignored.
 * @param[in] len Payload length in bytes.
 * @return ``true`` when the pump should stop.
 * @retval true The RPC layer accepted the awaited response.
 * @retval false Everything else, including announcements.
 * @pre The console is up.
 * @pre @p len bytes are readable at @p payload.
 * @post Exactly one module was offered the frame.
 * @post No application state is modified beyond that module's own.
 * @note Matches ::c6_fwver_sink_t; it is the sink every pump is given.
 * @see c6_fwver_link_pump
 * @since 0.1.0
 */
bool c6_fwver_dispatch(uint8_t if_type, uint8_t if_num, const uint8_t* payload, uint16_t len);
