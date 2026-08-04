/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/inc/idf_compat/esp_err.h
 * @brief ESP-IDF-compatible error type and error codes, supplied by this port.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vendored esp-hosted core includes ``esp_err.h`` by name from seven
 * translation units and names ``esp_err_t`` in roughly a hundred more through
 * ``transport_drv.h``. This project does not vendor ESP-IDF, so the name is
 * satisfied here by a first-party header carrying the same type and the same
 * numeric codes.
 *
 * @par Why the numbers are ESP-IDF's and not this project's
 * An esp-hosted error is not a purely local value. It travels: the RPC layer
 * copies a co-processor-side status straight into ``resp_event_status``, a
 * captured host log line sits next to a captured co-processor log line, and
 * anyone reading either is reading against ESP-IDF's published table. Renumbering
 * would make ``0x103`` mean one thing on the RA8 and another on the ESP32-C6 in
 * the same transaction. So the values below are ESP-IDF's exactly: ``ESP_OK``
 * is 0, ``ESP_FAIL`` is -1, and the general error space is the contiguous block
 * from ``0x101``. The whole block is defined rather than only the codes reached
 * today, because a partially-populated contiguous range is an invitation to
 * invent a wrong value for the next one needed.
 *
 * @par Why ``esp_err_t`` is `int` and not a typed enum
 * This is the one place in this port where the C23 typed-enum rule cannot be
 * satisfied, and the vendored source is what settles it. The core declares
 * functions with one spelling and defines them with the other: ``rpc_wrap.h``
 * declares ``esp_err_t rpc_init(void)`` while ``rpc_wrap.c`` defines
 * ``int rpc_init(void)``, and around forty ``rpc_*`` entry points do the same.
 * C requires the two to be the *same* type, not merely compatible in width, so
 * ``esp_err_t`` has to be ``int`` itself. An enumerated type -- even one with
 * ``int`` as its fixed underlying type -- is a distinct type, and every one of
 * those pairs would become a conflicting-types error. ESP-IDF spells it
 * ``typedef int esp_err_t`` for the same reason. The error *codes* are still a
 * C23 typed enum; only the alias is `int`.
 *
 * @par Two codes are macros, deliberately
 * ::ESP_ERR_ESP_NETIF_TX_FAILED and ::ESP_ERR_ESP_NETIF_NO_MEM are object-like
 * macros rather than enumerators, because ``transport_drv.c`` selects between
 * them six times with ``#if defined(ESP_ERR_ESP_NETIF_TX_FAILED)``. An
 * enumerator is invisible to ``defined()``, so making them enumerators would
 * silently take the fallback branch and every transmit failure on every
 * interface would be reported as an out-of-memory condition instead. They are
 * macros in ESP-IDF too, in ``esp_netif_types.h``, which is where the values
 * come from: base ``0x5000``, plus ``0x06`` for the no-memory code and ``0x0E``
 * for the transmit-failure code. The base itself is not defined here because
 * the vendored tree never names it.
 *
 * @par Symbols deliberately NOT defined here
 *   - ``ESP_ERROR_CHECK`` and ``ESP_ERROR_CHECK_WITHOUT_ABORT``. ESP-IDF puts
 *     them in this header; this port puts them in ``esp_log.h``, beside the
 *     writer that reports the failure, and every vendored user of them reaches
 *     that header. Restating either here would be a macro redefinition.
 *   - ``esp_err_to_name()``. The vendored tree calls it exactly once, in
 *     ``common/utils/esp_hosted_cli.c``, inside ``#ifdef H_ESP_HOSTED_CLI_ENABLED``.
 *     That symbol is only defined when ``CONFIG_ESP_HOSTED_COPROCESSOR`` **and**
 *     ``CONFIG_ESP_HOSTED_CLI_ENABLED`` are both set, and ``sdkconfig.h`` in this
 *     directory leaves both undefined because the CLI is a co-processor-side
 *     feature. The call site is therefore unreachable in this build, and
 *     declaring a function with no implementation behind it would advertise a
 *     capability that does not exist.
 *   - ``ESP_ERR_NO_BUFFS``. It appears once in ``transport_drv.c`` and only on a
 *     commented-out line; it is not part of ESP-IDF's core error space either.
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/* The type and enumerator spellings below are fixed by ESP-IDF: the vendored
   core declares functions with them, compares against them and forwards them
   to the co-processor, so they cannot take this project's k_ prefix or its
   lower_case typedef convention. clang-tidy's naming rule is suppressed across
   the block, following the ThreadX shim precedent in libs/ra8_wdt_supervisor
   and the sibling esp_log.h. */
/* NOLINTBEGIN(readability-identifier-naming) -- ESP-IDF-fixed spellings. */

/**
 * @typedef esp_err_t
 * @brief Result of an esp-hosted call: zero on success, non-zero otherwise.
 * @details Plain `int`, and it has to be: the vendored core declares around
 * forty ``rpc_*`` entry points as ``esp_err_t`` in ``rpc_wrap.h`` and defines
 * them as ``int`` in ``rpc_wrap.c``, which is only well-formed when the two
 * name the same type. Values are ::esp_err_code_t enumerators, the two
 * ``ESP_ERR_ESP_NETIF_*`` macros below, or a code minted by the vendored
 * ``esp_hosted_rpc.h`` from its own ``ESP_ERR_HOSTED_BASE``.
 * @note Not an enumerated type; a `switch` over it needs a `default`.
 * @see esp_err_code_t
 * @since 0.1.0
 */
typedef int esp_err_t;

/**
 * @enum esp_err_code_t
 * @brief ESP-IDF's general error codes, with ESP-IDF's numeric values.
 *
 * @details
 * ``ESP_OK`` and ``ESP_FAIL`` are the two coarse outcomes; everything from
 * ::ESP_ERR_NO_MEM upwards is the contiguous general-purpose block ESP-IDF
 * reserves at ``0x100``. Enumerators, not macros, so they obey this project's
 * constant rules -- and they remain integer constant expressions, which is what
 * lets ``esp_hosted_transport_config.h`` seed its own enumeration from
 * ``ESP_OK``, ``ESP_ERR_INVALID_ARG``, ``ESP_ERR_NOT_ALLOWED`` and
 * ``ESP_ERR_INVALID_STATE``.
 *
 * The underlying type is ``int32_t`` so an enumerator assigns into an
 * ::esp_err_t without conversion on the 32-bit target and on the 64-bit
 * unit-test host alike.
 *
 * @invariant ``ESP_OK`` is zero and is the only success value; every caller in
 *            the vendored core tests ``!= ESP_OK`` or plain truthiness.
 * @invariant ``ESP_FAIL`` is the only negative value here, so a caller may test
 *            ``< 0`` for "generic failure" and ``> 0`` for "specific reason".
 * @invariant The ``ESP_ERR_*`` values match ESP-IDF's published table exactly,
 *            so a code logged on the RA8 and a code logged on the co-processor
 *            mean the same thing.
 *
 * @par Example:
 * @code
 * esp_err_t rc = setup_transport(on_up);
 * if (rc != ESP_OK) {
 *   ESP_LOGE(TAG, "transport setup failed: %d", rc);
 * }
 * @endcode
 *
 * @see esp_err_t
 * @see ESP_ERR_ESP_NETIF_TX_FAILED
 * @since 0.1.0
 */
typedef enum : int32_t {
  /* Reached by the vendored core today. */
  ESP_OK   = 0,  /**< Success. The only non-error value.                    */
  ESP_FAIL = -1, /**< Generic failure with no more specific code available. */

  ESP_ERR_NO_MEM        = 0x101, /**< Allocation failed.                    */
  ESP_ERR_INVALID_ARG   = 0x102, /**< A parameter was out of range or null. */
  ESP_ERR_INVALID_STATE = 0x103, /**< Legal call, wrong moment to make it.  */
  ESP_ERR_INVALID_SIZE  = 0x104, /**< A length or buffer size was rejected. */
  ESP_ERR_NOT_FOUND     = 0x105, /**< The requested item does not exist.    */
  ESP_ERR_NOT_SUPPORTED = 0x106, /**< The co-processor image lacks this.    */
  ESP_ERR_TIMEOUT       = 0x107, /**< The operation did not finish in time. */

  /* Defined for completeness of ESP-IDF's contiguous block; see @details. */
  ESP_ERR_INVALID_RESPONSE = 0x108, /**< Reply was malformed or unexpected. */
  ESP_ERR_INVALID_CRC      = 0x109, /**< A checksum did not verify.         */
  ESP_ERR_INVALID_VERSION  = 0x10A, /**< Version field was not understood.  */
  ESP_ERR_INVALID_MAC      = 0x10B, /**< MAC address was malformed.         */
  ESP_ERR_NOT_FINISHED     = 0x10C, /**< Partial result; call again.        */
  ESP_ERR_NOT_ALLOWED      = 0x10D, /**< Refused by policy, not by state.   */
} esp_err_code_t;

/* NOLINTEND(readability-identifier-naming) */

/**
 * @def ESP_ERR_ESP_NETIF_NO_MEM
 * @brief Network-interface layer could not obtain a buffer.
 * @details ESP-IDF's ``ESP_ERR_ESP_NETIF_BASE + 0x06``, hence ``0x5006``. A
 * macro rather than an enumerator only because its partner
 * ::ESP_ERR_ESP_NETIF_TX_FAILED has to be one; keeping the pair in one form
 * stops a reader concluding the two are drawn from different spaces. It is the
 * fallback the vendored ``transport_drv.c`` returns when the transmit-failure
 * code is unavailable, which in this build it is not.
 * @note Read-only build configuration.
 * @warning Not part of the ``0x100`` general block -- do not compare it against
 *          ::esp_err_code_t enumerators expecting adjacency.
 * @par Example:
 * @code
 * return ESP_ERR_ESP_NETIF_NO_MEM;
 * @endcode
 * @since 0.1.0
 */
#define ESP_ERR_ESP_NETIF_NO_MEM (0x5006)

/**
 * @def ESP_ERR_ESP_NETIF_TX_FAILED
 * @brief A frame could not be handed to the transport for transmission.
 * @details ESP-IDF's ``ESP_ERR_ESP_NETIF_BASE + 0x0E``, hence ``0x500E``. This
 * is the code the vendored ``transport_drv.c`` returns from all three of its
 * transmit paths -- station, access point and serial -- when the link is down,
 * when Wi-Fi flow control is throttling, or when the transport mempool is
 * exhausted.
 * @note Read-only build configuration. It is a macro, not an enumerator,
 * because ``transport_drv.c`` guards those six returns with
 * ``#if defined(ESP_ERR_ESP_NETIF_TX_FAILED)``.
 * @warning Never convert this to an enumerator. ``defined()`` would then be
 *          false, the fallback branch would compile, and every transmit failure
 *          would be reported as ::ESP_ERR_ESP_NETIF_NO_MEM -- a wrong diagnosis
 *          that the build would not warn about.
 * @par Example:
 * @code
 * if (!is_transport_tx_ready()) {
 *   return ESP_ERR_ESP_NETIF_TX_FAILED;
 * }
 * @endcode
 * @since 0.1.0
 */
#define ESP_ERR_ESP_NETIF_TX_FAILED (0x500E)
