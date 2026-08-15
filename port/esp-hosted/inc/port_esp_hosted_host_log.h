/**
 * @file port/esp-hosted/inc/port_esp_hosted_host_log.h
 * @brief esp-hosted port header: the per-file log tag the core expects.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * One of the ten port headers the vendored esp-hosted core includes by name
 * (see ``docs/SOUP/esp-hosted-host.md``); thirteen vendored translation
 * units reach it. Upstream ships its equivalent under ``host/port/``, which
 * this project excludes; this file is its first-party replacement and keeps
 * the upstream file name and macro spelling exactly.
 *
 * It does two things and nothing else: it pulls in the ESP-IDF-compatible
 * logging surface (``port/esp-hosted/inc/idf_compat/esp_log.h``, also
 * first-party), and it supplies ::DEFINE_LOG_TAG, the one-liner every
 * vendored source uses to declare the file-scope ``s_esp_hosted_tag`` symbol that the
 * ``ESP_LOGx`` macros then take as their first argument.
 *
 * @par Why s_esp_hosted_tag has to be that exact name
 * The vendored ``common/log/esp_hosted_log.h`` builds its hex-dump helpers
 * on a bare ``s_esp_hosted_tag`` -- ``ESP_HEXLOGI(...)`` expands to a call that names
 * ``s_esp_hosted_tag`` without ever receiving it as an argument. So the symbol is part of
 * the contract, not an implementation detail, and it must be a file-scope
 * ``const char*`` in every translation unit that logs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "esp_log.h"

/**
 * @def DEFINE_LOG_TAG
 * @brief Declare the file-scope ``s_esp_hosted_tag`` string the ``ESP_LOGx`` macros use.
 * @details Expands to a ``static const char*`` initialised to the stringised
 * argument with the upstream ``H_`` prefix, so ``DEFINE_LOG_TAG(spi)``
 * yields ``"H_spi"``. That prefix is what distinguishes an esp-hosted line
 * from the rest of the firmware's log in a mixed console capture, and it
 * matches the hand-written tags the vendored drivers that do not use this
 * macro already carry (``"H_SDIO_DRV"``, ``"H_API"``).
 *
 * The declaration carries ``[[maybe_unused]]`` because several vendored
 * translation units define the tag near the top of the file and then reach a
 * build configuration in which every one of their log calls compiles out;
 * without the attribute that is an unused-variable diagnostic, and the tree
 * builds with ``-Werror``.
 * @param[in] tag Bare identifier naming the module; stringised, so it is not
 *                evaluated and does not have to exist as a symbol.
 * @note Read-only build configuration. Expand it exactly once per
 * translation unit, at file scope, followed by a semicolon -- that is how
 * every vendored source writes it.
 * @warning ``esp_hosted_api.c`` quietens three modules by calling
 *          ``esp_log_level_set()`` with the unprefixed names
 *          (``"rpc_core"``, ``"rpc_rsp"``, ``"rpc_evt"``), which do not
 *          match the ``H_``-prefixed tags this macro produces. That is an
 *          upstream inconsistency; do not "fix" it by dropping the prefix,
 *          which would break every other tag.
 * @par Example:
 * @code
 * DEFINE_LOG_TAG(rpc_evt);
 * ESP_LOGI(s_esp_hosted_tag, "event %u", evt_id);
 * @endcode
 * @since 0.1.0
 */
#define DEFINE_LOG_TAG(tag) [[maybe_unused]] static const char* s_esp_hosted_tag = "H_" #tag

/**
 * @def TAG
 * @brief Preserve the tag identifier required by vendored esp-hosted macros.
 * @details Expands only to the first-party ::s_esp_hosted_tag storage name;
 * no second object or external symbol is introduced. Vendored hex-dump and
 * error-check macros name ``TAG`` implicitly, so this compatibility spelling
 * is part of the port boundary while the owned storage still follows RA8
 * file-scope naming.
 * @note Macro alias only; read-only after ::DEFINE_LOG_TAG initializes storage.
 * @since 0.1.0
 */
#define TAG s_esp_hosted_tag
