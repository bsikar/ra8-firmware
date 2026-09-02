/**
 * @file port/esp-hosted/inc/port_esp_hosted_host_config_features.h
 * @brief esp-hosted port header: optional host features, RPC bounds, counters.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Included from the bottom of ``port_esp_hosted_host_config.h``, which is
 * the file name the vendored core knows. It carries the switches for host
 * features that need code this port does not have, the two RPC concurrency
 * bounds, and the debug counters -- separated from the wiring facts because
 * the repository caps a source file at 1000 lines and because these answer a
 * different question: not "how is the link built" but "what does this image
 * choose not to do".
 *
 * @par Everything optional is off, and each one for a structural reason
 * A feature is off here because the code behind it is absent from this tree
 * or because the hardware it drives is absent from this board -- never
 * because it has not been got to. Power save wants an ESP-IDF sleep API and
 * a wake pin; network split wants a dual-stack lwIP with a port-range
 * splitter; the static-netif path wants ``esp_netif``; OpenThread wants an
 * RCP. None of those exist here, so switching any of them on would produce a
 * link error rather than a feature.
 *
 * @par Two flags that must stay UNDEFINED, not zero
 * The vendored core tests ``H_ESP_HOSTED_CLI_ENABLED`` and
 * ``H_PEER_DATA_TRANSFER`` with ``#ifdef``, not ``#if``. Defining either to
 * zero would switch it ON:
 *   - ``H_ESP_HOSTED_CLI_ENABLED`` would compile ``esp_hosted_cli.c``, which
 *     needs ``esp_console.h``, FreeRTOS task-trace APIs, ``lwip/sockets.h``
 *     and the ESP heap-tracing headers. None are in this tree.
 *   - ``H_PEER_DATA_TRANSFER`` would compile the custom-message handler
 *     table in ``rpc_evt.c`` and its RPC request and response halves; the
 *     application has no custom co-processor messages, so the table would be
 *     dead registration surface on a safety-critical image.
 * Neither is defined anywhere in this port. ::H_MAX_CUSTOM_MSG_HANDLERS is
 * still given a value so the bound is stated rather than discovered the day
 * someone enables the feature.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

/* ----------------------------------------------------------------------- */
/* Host power save */
/* ----------------------------------------------------------------------- */

/**
 * @def H_HOST_PS_ALLOWED
 * @brief Whether the host may sleep and be woken by the co-processor.
 * @details Zero. The vendored power-save driver drives an ESP-IDF sleep
 * entry point and a dedicated wake input; this board wires neither, and the
 * RA8's own low-power modes are managed by the firmware's LPM layer rather
 * than by a radio co-processor.
 * @note Read-only build configuration.
 * @warning With this at 1 and no wake pin, the host would sleep with nothing
 *          able to wake it.
 * @par Example:
 * @code
 * #if H_HOST_PS_ALLOWED
 * esp_hosted_power_save_init();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_PS_ALLOWED (0)

/**
 * @def H_HOST_WAKEUP_GPIO
 * @brief Pin index of the host wake input, or -1 when none is wired.
 * @details Minus one. The co-processor has no line back to the RA8 other
 * than HANDSHAKE and DATA_READY, and neither is routed to a low-power wake
 * source on this package.
 * @note Read-only build configuration.
 * @warning The vendored guards spell the "unwired" test as ``!= -1``, so any
 *          other sentinel here silently enables the power-save path.
 * @par Example:
 * @code
 * #if H_HOST_PS_ALLOWED && H_HOST_WAKEUP_GPIO != -1
 * configure_wakeup_interrupt();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_WAKEUP_GPIO (-1)

/**
 * @def H_HOST_WAKEUP_GPIO_PORT
 * @brief Opaque port handle of the host wake input.
 * @details Encodes ``k_ra8_pin_none`` through the same adapter the side-band
 * pins use, so an unwired signal has one representation across this port
 * rather than a special case per call site.
 * @note Read-only build configuration.
 * @warning Never dereferenced and never reached: ::H_HOST_PS_ALLOWED is 0,
 *          and the pin the driver actually tests is ::H_HOST_WAKEUP_GPIO.
 * @par Example:
 * @code
 * void *port = H_HOST_WAKEUP_GPIO_PORT;
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_WAKEUP_GPIO_PORT RA8_ESP_HOSTED_GPIO_PORT(k_ra8_pin_none)

/**
 * @def H_HOST_WAKEUP_GPIO_LEVEL
 * @brief Level the co-processor would drive to wake the host.
 * @details Zero. An active-low wake is the conventional choice because a
 * sleeping host can hold a pull-up and the co-processor only has to sink
 * current for the pulse.
 * @note Read-only build configuration.
 * @warning Unreachable while ::H_HOST_PS_ALLOWED is 0.
 * @par Example:
 * @code
 * if (current_level == H_HOST_WAKEUP_GPIO_LEVEL) { note_wakeup(); }
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_WAKEUP_GPIO_LEVEL (0)

/* ----------------------------------------------------------------------- */
/* Networking integration */
/* ----------------------------------------------------------------------- */

/**
 * @def H_HOST_USES_STATIC_NETIF
 * @brief Whether the transport attaches a statically created esp_netif.
 * @details Zero. That path calls ``esp_netif_attach_wifi_station()`` and
 * ``esp_netif_dhcpc_stop()``; this tree has no ``esp_netif``, it has NetX
 * Duo behind ``ra8_net_pal``, so the glue lives on the application side of
 * the transport rather than inside it.
 * @note Read-only build configuration.
 * @warning Setting this to 1 makes ``spi_drv.c`` and ``sdio_drv.c``
 *          reference four ESP-IDF network symbols that do not exist here.
 * @par Example:
 * @code
 * #if H_HOST_USES_STATIC_NETIF
 * ESP_ERROR_CHECK(esp_netif_attach_wifi_station(sta_netif));
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_USES_STATIC_NETIF (0)

/**
 * @def H_NETWORK_SPLIT_ENABLED
 * @brief Whether the TCP/IP stack is split between host and co-processor.
 * @details Zero. Network split runs part of the socket layer on the C6 and
 * partitions the local port space between the two ends; it needs a
 * co-processor image built for it and a host stack that honours the
 * partition. This image terminates every socket on the RA8.
 * @note Read-only build configuration.
 * @warning The four port-range pairs below are only consulted when this is
 *          1; they are stated anyway so the partition is documented.
 * @par Example:
 * @code
 * #if H_NETWORK_SPLIT_ENABLED
 * log_port_ranges();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_NETWORK_SPLIT_ENABLED (0)

/**
 * @def H_HOST_TCP_LOCAL_PORT_RANGE_START
 * @brief First ephemeral TCP port the host would own under network split.
 * @details 49152, the bottom of the IANA dynamic range, so a split
 * deployment never collides with a registered service port.
 * @note Read-only build configuration.
 * @warning Only read when ::H_NETWORK_SPLIT_ENABLED is 1.
 * @par Example:
 * @code
 * bind_local(H_HOST_TCP_LOCAL_PORT_RANGE_START);
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_TCP_LOCAL_PORT_RANGE_START (49152)

/**
 * @def H_HOST_TCP_LOCAL_PORT_RANGE_END
 * @brief Last ephemeral TCP port the host would own under network split.
 * @details 61439, leaving the top 4096 ports of the dynamic range to the
 * co-processor so the two halves cannot pick the same tuple.
 * @note Read-only build configuration.
 * @warning Must not overlap the co-processor's TCP range below.
 * @par Example:
 * @code
 * assert(port <= H_HOST_TCP_LOCAL_PORT_RANGE_END);
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_TCP_LOCAL_PORT_RANGE_END (61439)

/**
 * @def H_HOST_UDP_LOCAL_PORT_RANGE_START
 * @brief First ephemeral UDP port the host would own under network split.
 * @details 49152; the UDP partition mirrors the TCP one so one rule explains
 * both.
 * @note Read-only build configuration.
 * @warning Only read when ::H_NETWORK_SPLIT_ENABLED is 1.
 * @par Example:
 * @code
 * bind_local_udp(H_HOST_UDP_LOCAL_PORT_RANGE_START);
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_UDP_LOCAL_PORT_RANGE_START (49152)

/**
 * @def H_HOST_UDP_LOCAL_PORT_RANGE_END
 * @brief Last ephemeral UDP port the host would own under network split.
 * @details 61439; mirrors ::H_HOST_TCP_LOCAL_PORT_RANGE_END.
 * @note Read-only build configuration.
 * @warning Must not overlap the co-processor's UDP range below.
 * @par Example:
 * @code
 * assert(port <= H_HOST_UDP_LOCAL_PORT_RANGE_END);
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_UDP_LOCAL_PORT_RANGE_END (61439)

/**
 * @brief First TCP port the co-processor would own under network split.
 * @details 61440, one past the host's range, so the partition is a single
 * boundary rather than an interleaving.
 * @note Read-only build configuration.
 * @warning Only read when ::H_NETWORK_SPLIT_ENABLED is 1.
 * @since 0.1.0
 */
#define H_SLAVE_TCP_REMOTE_PORT_RANGE_START (61440) /* LEGACY-OK: upstream macro name */

/**
 * @brief Last TCP port the co-processor would own under network split.
 * @details 65535, the top of the port space.
 * @note Read-only build configuration.
 * @warning Only read when ::H_NETWORK_SPLIT_ENABLED is 1.
 * @since 0.1.0
 */
#define H_SLAVE_TCP_REMOTE_PORT_RANGE_END (65535) /* LEGACY-OK: upstream macro name */

/**
 * @brief First UDP port the co-processor would own under network split.
 * @details 61440; mirrors the TCP partition.
 * @note Read-only build configuration.
 * @warning Only read when ::H_NETWORK_SPLIT_ENABLED is 1.
 * @since 0.1.0
 */
#define H_SLAVE_UDP_REMOTE_PORT_RANGE_START (61440) /* LEGACY-OK: upstream macro name */

/**
 * @brief Last UDP port the co-processor would own under network split.
 * @details 65535, the top of the port space.
 * @note Read-only build configuration.
 * @warning Only read when ::H_NETWORK_SPLIT_ENABLED is 1.
 * @since 0.1.0
 */
#define H_SLAVE_UDP_REMOTE_PORT_RANGE_END (65535) /* LEGACY-OK: upstream macro name */

/**
 * @def H_WIFI_TX_DATA_THROTTLE_HIGH_THRESHOLD
 * @brief Queue occupancy, in percent, at which the co-processor throttles.
 * @details Eighty. Sent to the co-processor in the host's configuration
 * message: once four fifths of the host's receive capacity is committed, the
 * co-processor stops handing Wi-Fi frames to the transport rather than
 * letting them pile up and be dropped at the far end of the link.
 * @note Read-only build configuration.
 * @warning Must be strictly above ::H_WIFI_TX_DATA_THROTTLE_LOW_THRESHOLD or
 *          the co-processor oscillates between throttled and unthrottled on
 *          every frame.
 * @par Example:
 * @code
 * send_slave_config(0, chip, raw_tp, // LEGACY-OK: upstream function name
 *     H_WIFI_TX_DATA_THROTTLE_LOW_THRESHOLD,
 *     H_WIFI_TX_DATA_THROTTLE_HIGH_THRESHOLD);
 * @endcode
 * @since 0.1.0
 */
#define H_WIFI_TX_DATA_THROTTLE_HIGH_THRESHOLD (80)

/**
 * @def H_WIFI_TX_DATA_THROTTLE_LOW_THRESHOLD
 * @brief Queue occupancy, in percent, at which throttling is released.
 * @details Sixty. The twenty-point gap is the hysteresis band; with the
 * twenty-frame queues this port uses that is four frames of slack, enough
 * that one burst cannot cross both thresholds.
 * @note Read-only build configuration.
 * @warning Setting this equal to the high threshold removes the hysteresis
 *          entirely.
 * @par Example:
 * @code
 * uint8_t low = H_WIFI_TX_DATA_THROTTLE_LOW_THRESHOLD;
 * @endcode
 * @since 0.1.0
 */
#define H_WIFI_TX_DATA_THROTTLE_LOW_THRESHOLD (60)

/* ----------------------------------------------------------------------- */
/* Unresponsive-co-processor policy */
/* ----------------------------------------------------------------------- */

/**
 * @brief Whether a silent co-processor restarts the host.
 * @details Zero, for the same reason as ``H_TRANSPORT_RESTART_ON_FAILURE``:
 * the RA8 owns the display, the storage and the reader state, and a radio
 * that never answers must not cost the user their place in a book. The
 * transport reports the failure; the application decides.
 * @note Read-only build configuration.
 * @warning With this at 1 the host arms a one-shot timer at the start of
 *          every connection attempt and resets itself when it expires.
 * @par Example:
 * @code
 * #if H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE // LEGACY-OK: upstream name
 * arm_init_timeout_timer();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE (0) /* LEGACY-OK: upstream macro name */

/**
 * @brief Milliseconds of silence before that restart would fire.
 * @details Minus one, the vendored "disabled" spelling. The guard is
 * ``#if <flag> && <timeout> != -1``, so both halves have to agree that the
 * policy is off; leaving the timeout at a real number while the flag is 0
 * would be a trap for whoever flips the flag next.
 * @note Read-only build configuration.
 * @warning Any non-negative value here arms the policy the moment the
 *          companion flag becomes 1.
 * @par Example:
 * @code
 * timer = timer_start("unresponsive",
 *     H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT_MS, // LEGACY-OK: name
 *     H_TIMER_TYPE_ONESHOT, cb, NULL);
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT_MS                                      \
  (-1) /* LEGACY-OK: upstream esp-hosted macro name */

/* ----------------------------------------------------------------------- */
/* RPC concurrency bounds */
/* ----------------------------------------------------------------------- */

/**
 * @def H_MAX_SYNC_RPC_REQUESTS
 * @brief Synchronous RPC transactions that may be outstanding at once.
 * @details One. The RPC layer is driven from a single thread in this port
 * and every synchronous call blocks it until the response arrives, so a
 * second slot could never be occupied. Keeping it at one also keeps the
 * transaction table a single entry, which is what makes the RPC path
 * allocation-free after init.
 * @note Read-only build configuration.
 * @warning ``serial_drv.c`` sizes its read semaphore from the sum of this
 *          and ::H_MAX_ASYNC_RPC_REQUESTS; both must be at least one.
 * @par Example:
 * @code
 * sem = g_h.funcs->_h_create_semaphore(H_MAX_SYNC_RPC_REQUESTS +
 *                                      H_MAX_ASYNC_RPC_REQUESTS);
 * @endcode
 * @since 0.1.0
 */
#define H_MAX_SYNC_RPC_REQUESTS (1)

/**
 * @def H_MAX_ASYNC_RPC_REQUESTS
 * @brief Asynchronous RPC transactions that may be outstanding at once.
 * @details One. The application issues asynchronous requests one at a time
 * -- a scan, then a connect, then a status read -- and a deeper table would
 * be reserved memory that no call path can reach.
 * @note Read-only build configuration.
 * @warning Raising it enlarges a statically allocated table; it does not
 *          make the RPC layer concurrent.
 * @par Example:
 * @code
 * #define MAX_ASYNC_RPC_TRANSACTIONS H_MAX_ASYNC_RPC_REQUESTS
 * @endcode
 * @since 0.1.0
 */
#define H_MAX_ASYNC_RPC_REQUESTS (1)

/**
 * @def H_MAX_CUSTOM_MSG_HANDLERS
 * @brief Callback slots for co-processor custom messages, if ever enabled.
 * @details Four. Unreachable while ``H_PEER_DATA_TRANSFER`` stays undefined
 * (see this file's header block), but stated so the table bound is a
 * decision rather than something discovered later.
 * @note Read-only build configuration.
 * @warning The handler table is a designated-initialiser array sized by this
 *          value; it must be a positive integer literal.
 * @par Example:
 * @code
 * #define MAX_CUSTOM_CALLBACKS H_MAX_CUSTOM_MSG_HANDLERS
 * @endcode
 * @since 0.1.0
 */
#define H_MAX_CUSTOM_MSG_HANDLERS (4)

/* ----------------------------------------------------------------------- */
/* Debug counters and the raw-throughput harness */
/* ----------------------------------------------------------------------- */

/*
 * All off. Each one either costs a periodic reporting thread, a per-frame
 * counter update on the hot path, or -- for the raw-throughput harness --
 * deliberately floods the link with dummy frames, which is the opposite of
 * what a shipped image should do.
 */

/**
 * @def H_TEST_RAW_TP
 * @brief Whether the raw-throughput test harness is compiled in.
 * @details Zero. The harness spawns a thread that pushes dummy frames as
 * fast as the transport accepts them; it measures the wire, and it starves
 * everything else while it does so.
 * @note Read-only build configuration.
 * @warning ``stats.h`` re-exports this as ``TEST_RAW_TP``; both spellings
 *          refer to this one switch.
 * @par Example:
 * @code
 * #if TEST_RAW_TP
 * start_test_raw_tp();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_TEST_RAW_TP (0)

/**
 * @def H_TEST_RAW_TP_DIR
 * @brief Direction bitmap advertised for the raw-throughput test.
 * @details Zero, which is ``ESP_TEST_RAW_TP_NONE`` in the vendored
 * ``esp_hosted_transport_init.h``. It is read unconditionally -- the host
 * puts it in the configuration message it sends after the boot event -- so
 * it must be defined even with ::H_TEST_RAW_TP off, and zero is what tells
 * the co-processor not to start a test.
 * @note Read-only build configuration; written as a literal so this header
 * does not have to reach the vendored enumeration.
 * @warning A non-zero value here starts a throughput test on the
 *          co-processor even though the host has no harness to match it.
 * @par Example:
 * @code
 * uint8_t raw_tp_config = H_TEST_RAW_TP_DIR;
 * @endcode
 * @since 0.1.0
 */
#define H_TEST_RAW_TP_DIR (0)

/**
 * @def H_RAW_TP_PKT_LEN
 * @brief Payload bytes per raw-throughput test frame.
 * @details 1448 -- a 1460-byte TCP maximum segment less the twelve-byte
 * esp-hosted payload header, which is the comparison the vendored header
 * spells out. It makes the reported number directly comparable with a
 * network throughput measurement instead of a transport-only best case.
 * @note Read-only build configuration.
 * @warning Must not exceed ``MAX_PAYLOAD_SIZE`` (1588 for this transport) or
 *          the harness builds frames the receiver rejects.
 * @par Example:
 * @code
 * #define TEST_RAW_TP__BUF_SIZE H_RAW_TP_PKT_LEN
 * @endcode
 * @since 0.1.0
 */
#define H_RAW_TP_PKT_LEN (1448)

/**
 * @def H_RAW_TP_REPORT_INTERVAL
 * @brief Seconds between raw-throughput reports.
 * @details Ten: long enough to average out a scheduling hiccup, short enough
 * that a bench operator does not think the test has hung.
 * @note Read-only build configuration.
 * @warning The timer is periodic; the interval is also the resolution of the
 *          number it prints.
 * @par Example:
 * @code
 * #define TEST_RAW_TP__TIMEOUT H_RAW_TP_REPORT_INTERVAL
 * @endcode
 * @since 0.1.0
 */
#define H_RAW_TP_REPORT_INTERVAL (10)

/**
 * @def H_MEM_STATS
 * @brief Whether the transport keeps allocation counters.
 * @details Zero. The counters live on the per-frame allocate and free paths,
 * and the port already reports pool occupancy through ``MEM_DUMP`` when it
 * is asked, which answers the same question without a hot-path cost.
 * @note Read-only build configuration.
 * @warning Turning this on also defines the ``h_stats_g`` object the SPI
 *          driver then updates; it is not a log-only switch.
 * @par Example:
 * @code
 * #if H_MEM_STATS
 * h_stats_g.spi_mem_stats.tx_alloc++;
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_MEM_STATS (0)

/**
 * @def H_MEM_MONITOR
 * @brief Whether the RPC layer logs free-heap movement around each call.
 * @details Zero. It is a heap-leak hunt for a build with a general heap;
 * this port allocates from a fixed pool that cannot grow, so a leak shows up
 * as pool exhaustion, which ``MEM_DUMP`` reports directly.
 * @note Read-only build configuration.
 * @warning Emits two log lines per RPC transaction when enabled, which is
 *          enough to change the timing it is measuring.
 * @par Example:
 * @code
 * #if H_MEM_MONITOR
 * log_free_heap_delta(__func__);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_MEM_MONITOR (0)

/**
 * @def ESP_PKT_STATS
 * @brief Whether per-interface packet counters are compiled in.
 * @details Zero. The counters are incremented on every transmit and receive
 * and reported from a periodic thread; neither is free, and the link's
 * health is already visible through the transport event callbacks.
 * @note Read-only build configuration; upstream spells this one without the
 * ``H_`` prefix, and that spelling is what the vendored ``#if`` tests.
 * @warning Enabling it also requires ::ESP_PKT_STATS_REPORT_INTERVAL, which
 *          is only referenced from inside the guarded block.
 * @par Example:
 * @code
 * #if ESP_PKT_STATS
 * pkt_stats.sta_rx_in_pass++;
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define ESP_PKT_STATS (0)

/**
 * @def ESP_PKT_STATS_REPORT_INTERVAL
 * @brief Seconds between packet-counter reports.
 * @details Ten, matching ::H_RAW_TP_REPORT_INTERVAL so the two debug
 * harnesses print on the same cadence and their logs interleave predictably.
 * @note Read-only build configuration.
 * @warning Only reached when ::ESP_PKT_STATS is 1; it is defined
 *          unconditionally so flipping that switch cannot fail to build.
 * @par Example:
 * @code
 * timer_start("pkt_stats", SEC_TO_MILLISEC(ESP_PKT_STATS_REPORT_INTERVAL),
 *             H_TIMER_TYPE_PERIODIC, stats_timer_func, NULL);
 * @endcode
 * @since 0.1.0
 */
#define ESP_PKT_STATS_REPORT_INTERVAL (10)
