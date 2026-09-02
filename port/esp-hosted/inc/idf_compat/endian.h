/**
 * @file port/esp-hosted/inc/idf_compat/endian.h
 * @brief Little-endian wire conversions for the esp-hosted frame header.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Four vendored transport drivers -- ``spi_drv.c``, ``spi_hd_drv.c``,
 * ``uart_drv.c`` and ``sdio_drv.c`` -- include ``endian.h`` by name and use it
 * on every frame they send or receive. ESP-IDF gets the name from its newlib;
 * this project's bare-metal build does not have one to get it from, so it is
 * supplied here.
 *
 * @par What is being converted
 * The esp-hosted payload header (``common/esp_hosted_header.h``) carries three
 * 16-bit fields -- ``len``, ``offset`` and ``seq_num`` -- and the wire format
 * defines them little-endian. Every transmit path runs them through
 * ::htole16 and every receive path runs them through ::le16toh; ``stats.h``
 * does the same for its throughput counters. That is the entire live use: a
 * grep of the vendored tree finds no ``htobe*`` or ``be*toh`` at all, and the
 * only mentions of ``htole32`` / ``htole64`` are inside protobuf-c comments,
 * which describe what its own ``fixed32_pack`` does rather than calling
 * anything. The 32-bit pair is provided anyway, because a header called
 * ``endian.h`` that supplies half a family is a trap: the next driver to need
 * ``le32toh`` would find the include already present and the symbol missing.
 *
 * @par Why these are functions and not `#define x (x)`
 * On this target the conversions are the identity -- the Cortex-M85 runs
 * little-endian and the wire format is little-endian -- and the compiler emits
 * nothing for them. The tempting shortcut is a macro that expands to its
 * argument. The reason not to take it is that the shortcut is *silently* right:
 * it is equally the identity on a big-endian toolchain, where it would frame
 * every packet with its length and sequence bytes swapped and the link would
 * fail with no diagnostic pointing here. So each conversion is a real
 * ``static inline`` function, and the file opens with a ``static_assert`` on
 * ``__BYTE_ORDER__`` that fails the build the moment the byte order stops being
 * the one these bodies assume. A wrong build that does not compile costs an
 * afternoon; a wrong build that runs costs a bench session.
 *
 * The functions also restore what the macro form throws away: the argument is
 * evaluated exactly once, it is type-checked, and the return type is the width
 * the caller asked for rather than whatever the argument promoted to.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/*
 * The build must be little-endian for the bodies below to be correct. GCC and
 * clang both define __BYTE_ORDER__ and __ORDER_LITTLE_ENDIAN__; if a toolchain
 * ever arrives that does not, the first branch fails loudly rather than letting
 * the second one assume anything.
 */
#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__)
#error "endian.h: toolchain does not report __BYTE_ORDER__; cannot prove byte order"
#endif

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "esp-hosted framing assumes a little-endian host; see this file's @details");

/* The four function spellings below are fixed by the BSD / glibc <endian.h>
   contract that the vendored transport drivers are written against, so they
   cannot take this project's ra8_ prefix. clang-tidy's naming rule is
   suppressed across the block, following the ThreadX shim precedent in
   libs/ra8_wdt_supervisor and the sibling esp_log.h. */

/**
 * @brief Convert a 16-bit value from host order to little-endian wire order.
 *
 * @details
 * Used on the transmit path of all four vendored transport drivers, on the
 * ``len``, ``offset`` and ``seq_num`` fields of every esp-hosted payload header
 * before the frame is handed to the bus. The identity on this target, which is
 * asserted at the top of this file rather than assumed here.
 *
 * @param[in] host_value Value in host byte order. Any 16-bit value; no range
 *                       is excluded.
 *
 * @return The same value in little-endian byte order.
 * @retval host_value Always, on a little-endian host -- the only byte order
 *                    this file compiles for.
 *
 * @pre The build is little-endian, which the file-scope ``static_assert``
 *      establishes at compile time.
 * @pre @p host_value has already been narrowed to 16 bits by the caller; this
 *      function does not truncate a wider value silently.
 * @post The returned value has the same bit pattern as @p host_value.
 * @post @p host_value is unmodified; the conversion is pure.
 *
 * @note Thread-safe and interrupt-safe: no state, no side effects. The
 *       vendored SPI driver calls it from its transaction task.
 * @warning Do not use this on a value that is already in wire order. The
 *          conversion is the identity here, so a double conversion is
 *          undetectable on this target and only surfaces if the code is ever
 *          moved to a big-endian host.
 *
 * @par Example:
 * @code
 * header->len = htole16((uint16_t)payload_len);
 * @endcode
 *
 * @see le16toh
 * @since 0.1.0
 */
static inline uint16_t htole16(uint16_t host_value)
{
  return host_value;
}

/**
 * @brief Convert a 16-bit value from little-endian wire order to host order.
 *
 * @details
 * The receive-path inverse of ::htole16, applied to the ``len``, ``offset`` and
 * ``seq_num`` fields of an incoming esp-hosted payload header before the
 * driver trusts them -- ``spi_drv.c`` uses the recovered length to bound the
 * copy out of the DMA buffer, so this runs before any length validation.
 *
 * @param[in] wire_value Value in little-endian byte order, as read from the
 *                       frame. Any 16-bit value; no range is excluded.
 *
 * @return The same value in host byte order.
 * @retval wire_value Always, on a little-endian host -- the only byte order
 *                    this file compiles for.
 *
 * @pre The build is little-endian, which the file-scope ``static_assert``
 *      establishes at compile time.
 * @pre @p wire_value was read from a fully received frame, not from a buffer
 *      still being filled by DMA.
 * @post The returned value has the same bit pattern as @p wire_value.
 * @post @p wire_value is unmodified; the conversion is pure.
 *
 * @note Thread-safe and interrupt-safe: no state, no side effects.
 * @warning Converting does not validate. A frame header may still carry a
 *          length longer than the buffer it arrived in; the caller must
 *          bounds-check the result.
 *
 * @par Example:
 * @code
 * uint16_t len = le16toh(header->len);
 * @endcode
 *
 * @see htole16
 * @since 0.1.0
 */
static inline uint16_t le16toh(uint16_t wire_value)
{
  return wire_value;
}

/**
 * @brief Convert a 32-bit value from host order to little-endian wire order.
 *
 * @details
 * The 32-bit member of the family. No vendored translation unit calls it
 * today; it exists so that a driver added later finds the whole ``<endian.h>``
 * contract behind an include it already has, rather than a half of it. Same
 * body, same guarantee, same compile-time byte-order proof as ::htole16.
 *
 * @param[in] host_value Value in host byte order. Any 32-bit value; no range
 *                       is excluded.
 *
 * @return The same value in little-endian byte order.
 * @retval host_value Always, on a little-endian host -- the only byte order
 *                    this file compiles for.
 *
 * @pre The build is little-endian, which the file-scope ``static_assert``
 *      establishes at compile time.
 * @pre @p host_value is the whole quantity being framed, not one half of a
 *      64-bit value being emitted piecewise.
 * @post The returned value has the same bit pattern as @p host_value.
 * @post @p host_value is unmodified; the conversion is pure.
 *
 * @note Thread-safe and interrupt-safe: no state, no side effects.
 * @warning protobuf-c does its own little-endian packing in ``fixed32_pack``
 *          and does not call this. Do not add a second conversion around a
 *          protobuf field.
 *
 * @par Example:
 * @code
 * hdr->timestamp_us = htole32(now_us);
 * @endcode
 *
 * @see le32toh
 * @since 0.1.0
 */
static inline uint32_t htole32(uint32_t host_value)
{
  return host_value;
}

/**
 * @brief Convert a 32-bit value from little-endian wire order to host order.
 *
 * @details
 * The receive-path inverse of ::htole32, and like it not reached by any
 * vendored translation unit today. Present for the same reason: an
 * ``<endian.h>`` that answers ``htole32`` but not ``le32toh`` would be a
 * half-kept promise.
 *
 * @param[in] wire_value Value in little-endian byte order, as read from the
 *                       frame. Any 32-bit value; no range is excluded.
 *
 * @return The same value in host byte order.
 * @retval wire_value Always, on a little-endian host -- the only byte order
 *                    this file compiles for.
 *
 * @pre The build is little-endian, which the file-scope ``static_assert``
 *      establishes at compile time.
 * @pre @p wire_value was read from a fully received frame, not from a buffer
 *      still being filled by DMA.
 * @post The returned value has the same bit pattern as @p wire_value.
 * @post @p wire_value is unmodified; the conversion is pure.
 *
 * @note Thread-safe and interrupt-safe: no state, no side effects.
 * @warning Converting does not validate; the caller must still range-check
 *          anything it uses as a length or an index.
 *
 * @par Example:
 * @code
 * uint32_t stamp = le32toh(hdr->timestamp_us);
 * @endcode
 *
 * @see htole32
 * @since 0.1.0
 */
static inline uint32_t le32toh(uint32_t wire_value)
{
  return wire_value;
}
