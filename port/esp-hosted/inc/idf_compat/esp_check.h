/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/inc/idf_compat/esp_check.h
 * @brief ESP-IDF-compatible check surface: the include, and nothing it does not need.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vendored esp-hosted core includes ``esp_check.h`` by name from exactly one
 * translation unit, ``host/api/src/esp_hosted_api.c``. This project does not
 * vendor ESP-IDF, so the name has to resolve to something -- and this file is
 * that something.
 *
 * @par This header deliberately defines no macros
 * ESP-IDF's version is a family of early-return helpers:
 * ``ESP_RETURN_ON_ERROR``, ``ESP_RETURN_ON_FALSE``, ``ESP_GOTO_ON_ERROR``,
 * ``ESP_GOTO_ON_FALSE`` and ``ESP_RETURN_VOID_ON_ERROR``. A grep of the entire
 * vendored tree finds **no use of any of them** -- not in the one file that
 * includes this header, and not anywhere else. Writing them anyway would add
 * five macros that nothing exercises, and an untested convenience macro is a
 * promise no one has checked. Two of them would additionally be dead on
 * arrival: the ``ESP_GOTO_*`` pair expands to a ``goto``, which NASA Power of 10
 * Rule 1 forbids in this tree, so a vendored file that started using one would
 * have to be dealt with rather than quietly accommodated.
 *
 * What the include *does* need to supply is the two headers ESP-IDF's version
 * pulls in and its users lean on transitively: ``esp_err.h`` for ``esp_err_t``
 * and the ``ESP_OK`` / ``ESP_ERR_*`` codes, and ``esp_log.h`` for the ``ESP_LOGx``
 * spellings and for ``ESP_ERROR_CHECK``, which this port keeps beside its
 * logger. Both are included below, so the one vendored consumer sees the same
 * set of names it would see on ESP-IDF.
 *
 * @par If a future vendored update starts using one
 * Add that macro here, with the same Doxygen block every other macro in this
 * port carries, and mirror ESP-IDF's semantics rather than inventing new ones.
 * For an ``ESP_GOTO_*``, restructure the call site instead: Rule 1 is not
 * negotiable for a convenience macro.
 *
 * @since 0.1.0
 */

#pragma once

#include "esp_err.h"
#include "esp_log.h"
