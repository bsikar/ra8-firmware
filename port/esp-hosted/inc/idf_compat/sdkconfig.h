/**
 * @file port/esp-hosted/inc/idf_compat/sdkconfig.h
 * @brief The ``CONFIG_*`` answers the vendored esp-hosted core asks for.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * On an ESP-IDF host this file does not exist in source control at all:
 * ``menuconfig`` writes it into the build directory from the Kconfig tree, and
 * every ``CONFIG_*`` symbol in it is a rendering of a choice made in that menu.
 * There is no Kconfig tree here and no menuconfig to run, so the file is
 * **authored**, and the authority for every value below is this project's own
 * build -- the transport that is actually wired, the RTOS that is actually
 * linked, the allocator policy this firmware actually obeys. Nothing in this
 * file may be justified by "that is what menuconfig defaults to"; it must be
 * justified by what the RA8D2 build does.
 *
 * Five vendored translation units include ``sdkconfig.h`` by name
 * (``esp_hosted_cli.{c,h}``, ``mempool.h``, ``mempool.c``), and a further
 * eighteen reach ``CONFIG_*`` symbols through other headers.
 *
 * @par The two test forms are not interchangeable
 * This is the trap the whole file is arranged around. A symbol tested with
 * ``#ifdef`` is ON whenever it exists, **whatever its value** -- so defining one
 * to ``0`` to mean "off" switches the feature on. A symbol tested with a bare
 * ``#if`` must exist, or ``-Wundef`` (which this project builds with, see
 * ``cmake/ra8_warnings.cmake``) reports it. So:
 *   - ``#ifdef``-tested symbols that should be off are **left undefined**;
 *   - ``#if``-tested symbols are **always defined**, to ``(0)`` or ``(1)``;
 *   - symbols guarded by ``#if defined(X) && X`` are safe either way, and are
 *     left undefined so the file lists only decisions that matter.
 * Every entry below says which form applies to it and where.
 *
 * @par Symbols the vendored tree names but never tests
 * ``CONFIG_IDF_FIRMWARE_CHIP_ID`` and ``CONFIG_WIFI_BSS_MAX_IDLE_SUPPORT``
 * appear only inside comments and inside the generated protobuf schema, where
 * they document what a co-processor-side field was derived from. Neither is
 * ever reached by the preprocessor on this side of the link, so neither is
 * defined here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

/* ----------------------------------------------------------------------- */
/* Defined: role and feature switches this build turns ON */
/* ----------------------------------------------------------------------- */

/**
 * @def CONFIG_ESP_HOSTED_ENABLED
 * @brief This firmware is an esp-hosted host.
 * @details Tested with ``#ifdef`` in ``common/utils/esp_hosted_cli.{c,h}``,
 * where it gates the includes of ``port_esp_hosted_host_config.h`` and the
 * power-save driver -- the host-side headers, as opposed to the co-processor
 * ones. Defined because the statement is simply true: the RA8D2 drives an
 * ESP32-C6 over SPI and this port supplies the host half of that link. The
 * value is ``1`` for readability only; the ``#ifdef`` never inspects it.
 * @note Read-only build configuration.
 * @warning Never defined at the same time as ``CONFIG_ESP_HOSTED_COPROCESSOR``.
 *          The two select opposite ends of the same link and the vendored CLI
 *          header includes a different, non-existent set of headers for each.
 * @par Example:
 * @code
 * #ifdef CONFIG_ESP_HOSTED_ENABLED
 * #include "port_esp_hosted_host_config.h"
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define CONFIG_ESP_HOSTED_ENABLED (1)

/**
 * @def CONFIG_ESP_HOSTED_USE_MEMPOOL
 * @brief Transport buffers come from esp-hosted's own block pool.
 * @details Zero. Its companion ``H_USE_MEMPOOL`` in
 * ``port_esp_hosted_host_config.h`` is left **undefined** rather than defined to
 * zero, because the vendored vtable header guards four of its rows with
 * ``#ifdef`` -- defining that symbol at all, even to zero, would grow
 * ``hosted_osi_funcs_t``. This one is tested with a bare ``#if``, so here zero
 * is the spelling that means off. The two say the same thing in the two
 * different dialects their consumers use. The reason for off is structural. ``common/mempool/mempool_ll.h`` includes
 * ``freertos/FreeRTOS.h``, ``portmacro.h``, ``task.h`` and ``semphr.h``
 * unconditionally -- upstream's pool is a FreeRTOS data structure, and this
 * image runs ThreadX.
 *
 * Switching it off costs nothing this project wanted. With it off every
 * transport buffer goes through the port's ``_h_malloc_align``, which serves
 * from a fixed ThreadX byte pool carved once during initialisation, so the
 * allocation profile stays bounded and NASA Power of 10 Rule 3 clean -- which
 * is the property the pool existed to provide.
 * @note Read-only build configuration. Tested with a bare ``#if``, so it must
 * be defined rather than left absent.
 * @warning Changing this without changing ``H_USE_MEMPOOL`` in the port config
 *          header splits one decision across two files that then disagree --
 *          and note that turning that one on means DEFINING it, not setting it
 *          to one.
 * @par Example:
 * @code
 * #if CONFIG_ESP_HOSTED_USE_MEMPOOL
 * struct os_mempool pool;
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define CONFIG_ESP_HOSTED_USE_MEMPOOL (0)

/**
 * @def CONFIG_ESP_HOSTED_DFLT_TASK_STACK
 * @brief Default stack, in bytes, for a transport worker thread.
 * @details Not a flag: ``sdio/sdio_drv.c`` expands it directly as the value of
 * its four ``*_TASK_STACK_SIZE`` macros, so an undefined symbol here is a syntax
 * error rather than a silent zero. ``4096`` matches ``DFLT_TASK_STACK_SIZE`` in
 * ``port_esp_hosted_host_os.h``, which is what the SPI driver -- the transport
 * this board actually uses -- passes to ``_h_thread_create``. Keeping the two
 * equal means the SDIO driver, if it is ever built, gets the stack this port
 * has already sized for a transport worker rather than a second, unrelated
 * number.
 * @note Read-only build configuration. Bytes, not words: the port's
 * ``_h_thread_create`` passes the value to ThreadX as a byte count.
 * @warning Must stay at or above ThreadX's ``TX_MINIMUM_STACK`` (512) or thread
 *          creation fails at run time, not at build time.
 * @par Example:
 * @code
 * #define RX_TASK_STACK_SIZE CONFIG_ESP_HOSTED_DFLT_TASK_STACK
 * @endcode
 * @since 0.1.0
 */
#define CONFIG_ESP_HOSTED_DFLT_TASK_STACK (4096)

/* ----------------------------------------------------------------------- */
/* Defined to zero: bare-#if symbols this build turns OFF */
/*                                                                          */
/* These must exist even when off, because the vendored code tests them with */
/* a bare #if and this project builds with -Wundef. */
/* ----------------------------------------------------------------------- */

/**
 * @def CONFIG_H_LOWER_MEMCOPY
 * @brief Do not trade a pooled buffer for a per-packet heap allocation.
 * @details Tested with a bare ``#if`` in ``host/utils/stats.c`` and in
 * ``spi/spi_drv.c`` -- in the SPI driver inside an outer ``#if 0``, so only the
 * statistics path is live. Upstream's "lower memcopy" variant saves one copy by
 * calling ``_h_calloc`` for every raw-throughput packet and freeing it again
 * afterwards. Zero, because that is a per-packet heap allocation on a hot path:
 * NASA Power of 10 Rule 3 rules it out, and it would also contradict
 * ::CONFIG_ESP_HOSTED_USE_MEMPOOL, which says transport buffers come from the
 * pool.
 * @note Read-only build configuration.
 * @warning Do not set this to ``1`` to chase throughput without first removing
 *          the Rule 3 obligation; the win is one ``memcpy`` per frame and the
 *          cost is unbounded allocation during steady-state operation.
 * @par Example:
 * @code
 * #if CONFIG_H_LOWER_MEMCOPY
 * buf = g_h.funcs->_h_calloc(1, MAX_TRANSPORT_BUFFER_SIZE);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define CONFIG_H_LOWER_MEMCOPY (0)

/**
 * @def CONFIG_ESP_HOSTED_WIFI_AUTO_CONNECT_ON_STA_START
 * @brief The application decides when to associate, not the event handler.
 * @details Tested with a bare ``#if`` in three places in
 * ``rpc/wrap/rpc_wrap.c``: it guards a forward declaration, the definition of
 * ``rpc_wifi_connect_async()``, and the call to it from inside the
 * ``WIFI_EVENT_STA_START`` case. Zero, so the station-started event is reported
 * and nothing else happens. Two reasons. It matches ESP-IDF's own contract, in
 * which an application calls ``esp_wifi_connect()`` itself once it has
 * credentials -- code written against ESP-IDF and moved onto this port behaves
 * the same. And it keeps association policy in the application rather than
 * firing an asynchronous RPC from inside the Wi-Fi event dispatch, where a
 * failure has no caller to return to.
 * @note Read-only build configuration.
 * @warning Setting this to ``1`` makes the host issue a connect RPC before the
 *          application has necessarily supplied an SSID, and the result is
 *          delivered to a callback rather than to the caller.
 * @par Example:
 * @code
 * #if CONFIG_ESP_HOSTED_WIFI_AUTO_CONNECT_ON_STA_START
 * rpc_wifi_connect_async();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define CONFIG_ESP_HOSTED_WIFI_AUTO_CONNECT_ON_STA_START (0)

/**
 * @def CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS
 * @brief No transmit-side Wi-Fi driver statistics are collected.
 * @details Tested with a bare ``#if`` in ``common/utils/esp_hosted_cli.c``,
 * which gates CLI commands that dump the co-processor's Wi-Fi driver counters.
 * Zero: the counters live in the Wi-Fi driver on the ESP32-C6, and the only
 * thing that reads them is the CLI, which this build does not have (see
 * ``CONFIG_ESP_HOSTED_CLI_ENABLED`` in the undefined section below).
 * @note Read-only build configuration.
 * @warning Turning this on without also enabling the CLI defines a symbol that
 *          nothing consults; the counters are surfaced only through CLI
 *          commands.
 * @par Example:
 * @code
 * #if CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS
 * register_tx_stats_command();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS (0)

/**
 * @def CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
 * @brief No receive-side Wi-Fi driver statistics are collected.
 * @details The receive-side twin of ::CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS,
 * tested with a bare ``#if`` in the same three CLI command tables and zero for
 * the same reason.
 * @note Read-only build configuration.
 * @warning See ::CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS; the two are enabled and
 *          disabled together in practice.
 * @par Example:
 * @code
 * #if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
 * register_rx_stats_command();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS (0)

/*
 * -------------------------------------------------------------------------
 * Left UNDEFINED, on purpose. Each of these is tested with #ifdef, #ifndef or
 * `#if defined(X) && X`, so defining it -- even to zero -- would switch the
 * feature ON or change which branch compiles. They are listed here rather than
 * omitted so that "why is this not in sdkconfig.h" has an answer in the file
 * itself. Doxygen is not used on this block: a documented @def for a symbol
 * that does not exist would put a name in the generated docs that no
 * translation unit can see.
 *
 *   CONFIG_ESP_HOSTED_COPROCESSOR
 *       #ifdef, in esp_hosted_cli.{c,h}. This build is the host, not the
 *       ESP32-C6. Defining it would select the co-processor include set
 *       (iperf.h, wifi_cmd.h, host_power_save.h), none of which exists here.
 *
 *   CONFIG_ESP_HOSTED_CLI_ENABLED
 *       #ifdef, in esp_hosted_cli.h, and only consulted inside the
 *       CONFIG_ESP_HOSTED_COPROCESSOR branch: the CLI is a co-processor-side
 *       console. Leaving it undefined is what leaves H_ESP_HOSTED_CLI_ENABLED
 *       undefined, which in turn is what makes the whole body of
 *       esp_hosted_cli.c -- with its esp_console.h, freertos/, esp_wifi.h and
 *       lwip/sockets.h includes -- compile away to nothing. This is also why
 *       esp_err_to_name() is not declared in esp_err.h.
 *
 *   CONFIG_ESP_HOSTED_CLI_NEW_INSTANCE
 *       #ifdef, in esp_hosted_cli.c. Chooses between two ESP-IDF console
 *       initialisation styles inside code that is already switched off.
 *
 *   CONFIG_ESP_HOSTED_FW_VERSION_MISMATCH_WARNING_SUPPRESS
 *       #ifndef, twice in transport_drv.c. Undefined, so the warning is KEPT:
 *       a co-processor image whose esp-hosted version does not match this
 *       host's is exactly the fault this port most wants announced, and
 *       coprocessor/esp32c6/pins.env pins both ends precisely so the two agree.
 *
 *   CONFIG_ESP_WIFI_NVS_ENABLED
 *       #ifdef, in rpc_wrap.c, where it sets nvs_enable in the wifi_init RPC.
 *       Undefined: NVS is ESP-IDF's key-value store in the ESP32's own flash.
 *       This host has no NVS, and asking the co-processor to persist Wi-Fi
 *       credentials on the host's behalf is a decision the application layer
 *       should make explicitly if it ever wants it.
 *
 *   CONFIG_IDF_TARGET
 *       #ifdef, in sdio_drv.c, where its presence means "the host is itself an
 *       Espressif part" and lowers the maximum-SDIO-clock advisory from
 *       50 MHz to 40 MHz. The RA8D2 is not an Espressif part, so the symbol
 *       stays undefined and the 50 MHz branch compiles -- which is correct
 *       regardless, since SDIO is not the transport in use.
 *
 *   CONFIG_ESP_HOSTED_CP_EXT_COEX
 *   CONFIG_ESP_HOSTED_CP_EXT_COEX_ADVANCE
 *       `#if defined(X) && X`, in api/include/esp_hosted_cp_ext_coex.h. External
 *       radio coexistence needs a second radio sharing the antenna and a wired
 *       arbitration signal to it; this board has neither.
 *
 *   CONFIG_HEAP_TRACING
 *   CONFIG_FREERTOS_USE_TRACE_FACILITY
 *   CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
 *       #ifdef / #ifndef, all in esp_hosted_cli.c. Each names an ESP-IDF or
 *       FreeRTOS instrumentation facility that does not exist in this build.
 *
 *   CONFIG_ESP_CONSOLE_UART_DEFAULT
 *   CONFIG_ESP_CONSOLE_UART_CUSTOM
 *   CONFIG_ESP_CONSOLE_USB_CDC
 *   CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
 *       #if defined / #elif defined, in esp_hosted_cli.c. They pick which
 *       ESP32 peripheral the CLI console binds to. This port's console is an
 *       RA8 SCI UART reached through esp_log.h's writer, and the CLI is off.
 * -------------------------------------------------------------------------
 */
