/**
 * @file port/esp-hosted/inc/idf_compat/esp_idf_version.h
 * @brief The ESP-IDF API level the co-processor image was built against.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vendored esp-hosted core compiles itself differently depending on the
 * ESP-IDF it is built against, and it asks with ``#if ESP_IDF_VERSION <
 * ESP_IDF_VERSION_VAL(5, 5, 0)`` in ``common/rpc/esp_hosted_bitmasks.h``. On
 * ESP-IDF that question is answered by the framework's own header; here it is
 * answered by this file.
 *
 * @par This states a fact about the co-processor, not about the host
 * The RA8D2 does not run ESP-IDF. It runs this project's bare-metal firmware
 * with ThreadX, and nothing on the Cortex-M85 side has an ESP-IDF version at
 * all. What has one is the **ESP32-C6 image on the other end of the SPI link**,
 * which ``coprocessor/esp32c6/`` builds from esp-hosted-mcu against a pinned
 * toolchain -- ``ESP_IDF_VERSION=v5.5.4`` in ``coprocessor/esp32c6/pins.env``,
 * the single source of truth that the build and flash scripts also read. The
 * numbers below are that pin, restated in the form the vendored core's
 * preprocessor understands.
 *
 * @par Why that makes the wire format line up
 * The two ends of the link exchange protobuf-encoded RPC structures. Several of
 * those structures are bitfields whose meaning changed between ESP-IDF
 * releases: the ``WIFI_STA_CONFIG_2`` word gained three VHT beamforming bits in
 * 5.5, moving its reserved mask from ``0xFFFFFC00`` to ``0xFFFFE000`` and its
 * highest used bit from 10 to 13. The host must pack what the co-processor
 * expects to unpack. Setting this to the co-processor's API level is what makes
 * the host compile the same field layout the co-processor image was compiled
 * with; setting it to anything else would produce a build that links, runs, and
 * quietly corrupts the reserved half of a station-configuration word.
 *
 * @par Keeping it honest
 * If ``coprocessor/esp32c6/pins.env`` is re-pinned to another ESP-IDF, change
 * ::ESP_IDF_VERSION_MAJOR / ::ESP_IDF_VERSION_MINOR / ::ESP_IDF_VERSION_PATCH
 * here in the same change. They are the host-side half of one decision, not two
 * independent settings.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

/**
 * @def ESP_IDF_VERSION_VAL
 * @brief Pack a major.minor.patch triple into one comparable integer.
 * @details Byte-packed exactly as ESP-IDF packs it, so a comparison written
 * against upstream's macro keeps upstream's meaning. Every operand is
 * parenthesised and the whole expression is parenthesised, which is what lets
 * it be used inside an ``#if`` and be compared against ::ESP_IDF_VERSION with
 * no surprises from operator precedence.
 * @param[in] major Major version number, 0..255.
 * @param[in] minor Minor version number, 0..255.
 * @param[in] patch Patch version number, 0..255.
 * @note Read-only build configuration. Preprocessor-arithmetic-safe: it expands
 * to integer literals and arithmetic only, with no identifiers, casts or
 * ``sizeof``, so it is legal in an ``#if``.
 * @warning A component above 255 silently overflows into the next field. That
 *          is upstream's behaviour too; no ESP-IDF release has come near it.
 * @par Example:
 * @code
 * #if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
 * #define WIFI_STA_CONFIG_2_RESERVED_BITMASK 0xFFFFFC00
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define ESP_IDF_VERSION_VAL(major, minor, patch) (((major) << 16) + ((minor) << 8) + (patch))

/**
 * @def ESP_IDF_VERSION_MAJOR
 * @brief Major component of the co-processor image's ESP-IDF version.
 * @details Five, from ``ESP_IDF_VERSION=v5.5.4`` in
 * ``coprocessor/esp32c6/pins.env``.
 * @note Read-only build configuration; states the co-processor's API level, not
 * the RA8 host's, which has none.
 * @warning Changing this without re-pinning and rebuilding the ESP32-C6 image
 *          desynchronises the RPC field layouts across the link.
 * @par Example:
 * @code
 * ESP_LOGI(TAG, "cp built against IDF %d.x", ESP_IDF_VERSION_MAJOR);
 * @endcode
 * @since 0.1.0
 */
#define ESP_IDF_VERSION_MAJOR (5)

/**
 * @def ESP_IDF_VERSION_MINOR
 * @brief Minor component of the co-processor image's ESP-IDF version.
 * @details Five. This is the component that matters most to the vendored core:
 * the only two version tests in the tree both ask whether the build is older
 * than 5.5, and at 5.5 both take the newer branch.
 * @note Read-only build configuration.
 * @warning Dropping below 5 here silently reverts the ``WIFI_STA_CONFIG_2``
 *          bitfield layout to its pre-5.5 form while the co-processor keeps
 *          using the 5.5 one.
 * @par Example:
 * @code
 * ESP_LOGI(TAG, "cp IDF minor %d", ESP_IDF_VERSION_MINOR);
 * @endcode
 * @since 0.1.0
 */
#define ESP_IDF_VERSION_MINOR (5)

/**
 * @def ESP_IDF_VERSION_PATCH
 * @brief Patch component of the co-processor image's ESP-IDF version.
 * @details Four. No vendored test inspects the patch level; it is recorded so
 * this header states the whole pin rather than the part currently consulted.
 * @note Read-only build configuration.
 * @warning Do not treat a patch bump as cosmetic without re-reading the
 *          upstream release notes for RPC-visible changes.
 * @par Example:
 * @code
 * ESP_LOGI(TAG, "cp IDF patch %d", ESP_IDF_VERSION_PATCH);
 * @endcode
 * @since 0.1.0
 */
#define ESP_IDF_VERSION_PATCH (4)

/**
 * @def ESP_IDF_VERSION
 * @brief The co-processor image's ESP-IDF version as one comparable integer.
 * @details ``ESP_IDF_VERSION_VAL(5, 5, 4)``. This is the symbol the vendored
 * ``esp_hosted_bitmasks.h`` compares, and it is built from the three components
 * above rather than restating them, so the four cannot drift apart.
 * @note Read-only build configuration. Preprocessor-arithmetic-safe.
 * @warning Compare it only against ::ESP_IDF_VERSION_VAL results. Comparing it
 *          against a bare number assumes the packing, which is upstream's to
 *          change.
 * @par Example:
 * @code
 * #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
 * // 5.5-and-later RPC field layout
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define ESP_IDF_VERSION                                                                            \
  ESP_IDF_VERSION_VAL(ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH)
