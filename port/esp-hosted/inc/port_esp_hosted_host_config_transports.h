/**
 * @file port/esp-hosted/inc/port_esp_hosted_host_config_transports.h
 * @brief esp-hosted port header: constants for the transports not in use.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Included from the bottom of ``port_esp_hosted_host_config.h``, which is
 * the file name the vendored core knows. It exists because that header
 * would otherwise cross the repository's 1000-line maintainability cap, and
 * because these constants answer a different question from the ones there:
 * not "how is this board wired" but "what do the three transports this board
 * does not use need in order to compile".
 *
 * @par Why constants for unused transports exist at all
 * ``H_TRANSPORT_IN_USE`` selects full-duplex SPI, but the vendored
 * ``sdio_drv.c``, ``spi_hd_drv.c`` and ``uart_drv.c`` are still translation
 * units in the build, and ``transport_drv.c`` reads a handful of their
 * constants unconditionally when it decodes the co-processor's capability
 * word. Leaving a symbol undefined would make the preprocessor substitute
 * zero and silently pick a different branch, so each one is given a real,
 * defensible value instead.
 *
 * @par Where the values come from
 * The SDIO and half-duplex numbers describe what those transports would need
 * on an EK-RA8D2 if they were ever wired: the SDHI slot's 40 MHz ceiling,
 * the same DATA_READY net and polarity the full-duplex link already uses,
 * and the same queue depth. They are not aspirational defaults copied from
 * an Espressif development kit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

/* ----------------------------------------------------------------------- */
/* Half-duplex SPI */
/* ----------------------------------------------------------------------- */

/**
 * @def H_SPI_HD_HOST_INTERFACE
 * @brief Whether the host drives the half-duplex SPI interface.
 * @details Zero. ``transport_drv.c`` uses this to decide which half of the
 * co-processor's capability word to report, and reporting half-duplex
 * capabilities on a full-duplex link would be a lie in the boot log.
 * @note Read-only build configuration.
 * @warning Must agree with ``H_TRANSPORT_IN_USE``; the two are separate
 *          switches upstream and nothing cross-checks them.
 * @par Example:
 * @code
 * #if H_SPI_HD_HOST_INTERFACE
 * report_spi_hd_capabilities(cap);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_HOST_INTERFACE (0)

/**
 * @def H_SPI_HD_HOST_NUM_DATA_LINES
 * @brief Data lines the host would use in half-duplex mode.
 * @details Four. If half-duplex were ever wired on this board it would use
 * the quad-capable Pmod position, and the negotiation in
 * ``transport_drv.c`` falls back to two and then one line when the
 * co-processor cannot manage four.
 * @note Read-only build configuration.
 * @warning Only meaningful when ::H_SPI_HD_HOST_INTERFACE is 1.
 * @par Example:
 * @code
 * if (H_SPI_HD_HOST_NUM_DATA_LINES == 4) { negotiate_quad(); }
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_HOST_NUM_DATA_LINES (4)

/**
 * @def H_SPI_HD_CONFIG_1_DATA_LINE
 * @brief Selector passed to the port when one data line is negotiated.
 * @details One, so the selector value and the line count are the same
 * number and a mismatch is visible by inspection.
 * @note Read-only build configuration.
 * @warning Consumed by ``_h_spi_hd_set_data_lines``; changing it changes a
 *          vtable argument, not just a constant.
 * @par Example:
 * @code
 * g_h.funcs->_h_spi_hd_set_data_lines(H_SPI_HD_CONFIG_1_DATA_LINE);
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_CONFIG_1_DATA_LINE (1)

/**
 * @def H_SPI_HD_CONFIG_2_DATA_LINES
 * @brief Selector passed to the port when two data lines are negotiated.
 * @details Two; see ::H_SPI_HD_CONFIG_1_DATA_LINE for the numbering.
 * @note Read-only build configuration.
 * @warning Consumed by ``_h_spi_hd_set_data_lines``.
 * @par Example:
 * @code
 * g_h.funcs->_h_spi_hd_set_data_lines(H_SPI_HD_CONFIG_2_DATA_LINES);
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_CONFIG_2_DATA_LINES (2)

/**
 * @def H_SPI_HD_CONFIG_4_DATA_LINES
 * @brief Selector passed to the port when four data lines are negotiated.
 * @details Four; see ::H_SPI_HD_CONFIG_1_DATA_LINE for the numbering.
 * @note Read-only build configuration.
 * @warning Consumed by ``_h_spi_hd_set_data_lines``.
 * @par Example:
 * @code
 * g_h.funcs->_h_spi_hd_set_data_lines(H_SPI_HD_CONFIG_4_DATA_LINES);
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_CONFIG_4_DATA_LINES (4)

/**
 * @def H_SPI_HD_CLK_MHZ
 * @brief Half-duplex SPI clock, in megahertz.
 * @details Five, the same conservative first-light rate as the full-duplex
 * link: the RA8 side would use the same polled Simple-SPI backend, so the
 * software ceiling is identical.
 * @note Read-only build configuration.
 * @warning ``spi_hd_drv.c`` logs a hint below 40 MHz; that is the
 *          co-processor's limit, not this board's.
 * @par Example:
 * @code
 * spi_hd_config.clk_mhz = H_SPI_HD_CLK_MHZ;
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_CLK_MHZ (5)

/**
 * @def H_SPI_HD_CHECKSUM
 * @brief Whether half-duplex frames carry the esp-hosted checksum.
 * @details One. A half-duplex link has no return path to corroborate a
 * frame, so the 16-bit header checksum is the only integrity evidence the
 * receiver gets.
 * @note Read-only build configuration.
 * @warning Both ends must agree; the co-processor drops frames whose
 *          checksum field it was not expecting.
 * @par Example:
 * @code
 * #if H_SPI_HD_CHECKSUM
 * header->checksum = htole16(compute_checksum(buf, len));
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_CHECKSUM (1)

/**
 * @def H_SPI_HD_DATA_READY_ENABLED
 * @brief Whether half-duplex uses a DATA_READY line rather than polling.
 * @details One. The DATA_READY net is already wired and already has the
 * board's only Pmod1 ICU channel, so an interrupt is available and polling
 * would be a needless waste of the transaction thread.
 * @note Read-only build configuration.
 * @warning With this at 0 the driver falls back to
 *          ::H_SPI_HD_POLL_INTERVAL_MS, which must then be non-zero.
 * @par Example:
 * @code
 * #if H_SPI_HD_DATA_READY_ENABLED
 * attach_data_ready_interrupt();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_DATA_READY_ENABLED (1)

/**
 * @def H_SPI_HD_PORT_DATA_READY
 * @brief Opaque port handle of the half-duplex DATA_READY input.
 * @details The same net the full-duplex link uses, so the two transports
 * cannot disagree about which wire carries readiness.
 * @note Read-only build configuration.
 * @warning Only read when ::H_SPI_HD_DATA_READY_ENABLED is 1.
 * @par Example:
 * @code
 * g_h.funcs->_h_config_gpio_as_interrupt(H_SPI_HD_PORT_DATA_READY,
 *     H_SPI_HD_PIN_DATA_READY, H_SPI_HD_DR_INTR_EDGE, handler, NULL);
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_PORT_DATA_READY (H_GPIO_DATA_READY_Port)

/**
 * @def H_SPI_HD_PIN_DATA_READY
 * @brief Pin index of the half-duplex DATA_READY input.
 * @details The same net the full-duplex link uses; see
 * ::H_SPI_HD_PORT_DATA_READY.
 * @note Read-only build configuration.
 * @warning Carries the same -1 "not wired" convention as
 *          ::H_GPIO_DATA_READY_Pin.
 * @par Example:
 * @code
 * int pin = H_SPI_HD_PIN_DATA_READY;
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_PIN_DATA_READY (H_GPIO_DATA_READY_Pin)

/**
 * @def H_SPI_HD_DR_INTR_EDGE
 * @brief Interrupt sense for the half-duplex DATA_READY input.
 * @details Derived from ::H_DR_INTR_EDGE, so one polarity fact governs both
 * SPI transports and an inverted harness needs a single edit.
 * @note Read-only build configuration.
 * @warning Integer-encoded against ``ra8_icu_irqmd_t``; see ::H_DR_INTR_EDGE.
 * @par Example:
 * @code
 * uint32_t sense = H_SPI_HD_DR_INTR_EDGE;
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_DR_INTR_EDGE (H_DR_INTR_EDGE)

/**
 * @def H_SPI_HD_POLL_INTERVAL_MS
 * @brief Fallback poll period when half-duplex has no DATA_READY line.
 * @details Ten milliseconds. The driver refuses to start when this is zero
 * and DATA_READY is disabled, because that combination is a busy loop on the
 * transaction thread.
 * @note Read-only build configuration.
 * @warning Dead code while ::H_SPI_HD_DATA_READY_ENABLED is 1, but it must
 *          still be non-zero for the driver's own guard to pass.
 * @par Example:
 * @code
 * g_h.funcs->_h_msleep(H_SPI_HD_POLL_INTERVAL_MS);
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_POLL_INTERVAL_MS (10)

/**
 * @def H_SPI_HD_TX_QUEUE_SIZE
 * @brief Frames queued towards the co-processor in half-duplex mode.
 * @details ::H_TRANSPORT_QUEUE_SIZE, so every transport in this port sizes
 * its queues and its pool from one number.
 * @note Read-only build configuration.
 * @warning The driver multiplies this by the priority-queue count when it
 *          sizes its semaphore; raising it costs memory in two places.
 * @par Example:
 * @code
 * spi_hd_mempool_create(H_SPI_HD_TX_QUEUE_SIZE, H_SPI_HD_RX_QUEUE_SIZE);
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_TX_QUEUE_SIZE (H_TRANSPORT_QUEUE_SIZE)

/**
 * @def H_SPI_HD_RX_QUEUE_SIZE
 * @brief Frames queued from the co-processor in half-duplex mode.
 * @details ::H_TRANSPORT_QUEUE_SIZE; see ::H_SPI_HD_TX_QUEUE_SIZE.
 * @note Read-only build configuration.
 * @warning Also sizes the data-ready semaphore, so it bounds how many
 *          readiness edges can be outstanding.
 * @par Example:
 * @code
 * queue = g_h.funcs->_h_create_queue(H_SPI_HD_RX_QUEUE_SIZE, sizeof(h));
 * @endcode
 * @since 0.1.0
 */
#define H_SPI_HD_RX_QUEUE_SIZE (H_TRANSPORT_QUEUE_SIZE)

/* ----------------------------------------------------------------------- */
/* SDIO */
/* ----------------------------------------------------------------------- */

/**
 * @def H_SDIO_ALWAYS_HOST_RX_MAX_TRANSPORT_SIZE
 * @brief Receive-mode id: always read a whole maximum-size transport buffer.
 * @details One. Trades bus time for never having to ask the co-processor how
 * many bytes are waiting.
 * @note Read-only build configuration.
 * @warning An id, not a switch. Compare it against ::H_SDIO_HOST_RX_MODE.
 * @par Example:
 * @code
 * #if H_SDIO_HOST_RX_MODE == H_SDIO_ALWAYS_HOST_RX_MAX_TRANSPORT_SIZE
 * read_fixed_size_block();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SDIO_ALWAYS_HOST_RX_MAX_TRANSPORT_SIZE (1)

/**
 * @def H_SDIO_HOST_STREAMING_MODE
 * @brief Receive-mode id: treat the SDIO link as a byte stream.
 * @details Two. Removes the per-packet framing at the cost of a
 * double-buffer and a reassembly state machine in the driver.
 * @note Read-only build configuration.
 * @warning An id, not a switch. Compare it against ::H_SDIO_HOST_RX_MODE.
 * @par Example:
 * @code
 * #if H_SDIO_HOST_RX_MODE == H_SDIO_HOST_STREAMING_MODE
 * stream_reassemble();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SDIO_HOST_STREAMING_MODE (2)

/**
 * @def H_SDIO_HOST_RX_MODE
 * @brief Which SDIO receive mode the host would use.
 * @details Zero, which matches neither named id and therefore selects the
 * driver's plain packet mode: read the length the co-processor advertises in
 * its register, then read exactly that many bytes. That is the mode with the
 * least bus waste and no reassembly state, and it is what an EK-RA8D2 SDHI
 * slot would want.
 * @note Read-only build configuration.
 * @warning Never set this to an id the driver does not know; the mode is
 *          chosen by ``!=`` comparisons, so an unknown value quietly selects
 *          packet mode.
 * @par Example:
 * @code
 * #if H_SDIO_HOST_RX_MODE != H_SDIO_HOST_STREAMING_MODE
 * packet_mode_rx();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SDIO_HOST_RX_MODE (0)

/**
 * @def H_SDIO_CHECKSUM
 * @brief Whether SDIO frames carry the esp-hosted checksum.
 * @details One, for the same reason as ::H_SPI_HD_CHECKSUM: the header
 * checksum is the receiver's only integrity evidence.
 * @note Read-only build configuration.
 * @warning Both ends must agree.
 * @par Example:
 * @code
 * #if H_SDIO_CHECKSUM
 * rx_checksum = le16toh(header->checksum);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SDIO_CHECKSUM (1)

/**
 * @def H_SDIO_CLOCK_FREQ_KHZ
 * @brief SDIO clock, in kilohertz.
 * @details Forty thousand. Forty megahertz is the ceiling the driver itself
 * names for a PCB-routed SDIO bus, and it is what the EK-RA8D2 SDHI slot is
 * qualified for with the SD card already fitted.
 * @note Read-only build configuration.
 * @warning The driver logs a hint below 40000; that hint is about signal
 *          integrity headroom, not a defect.
 * @par Example:
 * @code
 * sdio_config.clock_freq_khz = H_SDIO_CLOCK_FREQ_KHZ;
 * @endcode
 * @since 0.1.0
 */
#define H_SDIO_CLOCK_FREQ_KHZ (40000)

/**
 * @def H_SDIO_TX_BLOCK_ONLY_XFER
 * @brief Whether SDIO transmits are padded up to whole blocks.
 * @details Zero. Padding every frame to a block boundary wastes bus time on
 * the short RPC frames that dominate this link, and the co-processor already
 * accepts byte-granular transfers.
 * @note Read-only build configuration.
 * @warning Turning this on also changes how much memory the receive path
 *          allocates; the two block-only switches are not independent of the
 *          buffer sizing.
 * @par Example:
 * @code
 * #if H_SDIO_TX_BLOCK_ONLY_XFER
 * len_to_send = round_up_to_block(len_to_send);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SDIO_TX_BLOCK_ONLY_XFER (0)

/**
 * @def H_SDIO_RX_BLOCK_ONLY_XFER
 * @brief Whether SDIO receives are padded up to whole blocks.
 * @details Zero, matching ::H_SDIO_TX_BLOCK_ONLY_XFER so both directions
 * agree about framing.
 * @note Read-only build configuration.
 * @warning With this at 1 every receive buffer must be rounded up before it
 *          is allocated, or the transfer overruns it.
 * @par Example:
 * @code
 * #if H_SDIO_RX_BLOCK_ONLY_XFER
 * len = round_up_to_block(len);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SDIO_RX_BLOCK_ONLY_XFER (0)

/**
 * @def H_HOST_SDIO_RESET_DELAY_MS
 * @brief Milliseconds the host waits after resetting the SDIO co-processor.
 * @details One hundred, comfortably past an ESP32 part's boot-ROM handoff,
 * so the first CMD0 is not answered by a half-initialised card interface.
 * @note Read-only build configuration.
 * @warning Too short a delay looks exactly like a dead co-processor: the
 *          host reports no response and gives up.
 * @par Example:
 * @code
 * g_h.funcs->_h_msleep(H_HOST_SDIO_RESET_DELAY_MS);
 * @endcode
 * @since 0.1.0
 */
#define H_HOST_SDIO_RESET_DELAY_MS (100)

/**
 * @brief Whether the host resets the co-processor only when it looks stuck.
 * @details Zero, so the host resets on every boot. Conditional reset needs a
 * reliable "is it already up" probe, and this harness has no reset wire at
 * all (::H_GPIO_PIN_RESET is -1), so the conditional path would be deciding
 * between two things it cannot do.
 * @note Read-only build configuration.
 * @warning Only reachable in the SDIO driver; the full-duplex SPI driver
 *          resets unconditionally when a reset pin exists.
 * @par Example:
 * @code
 * #if H_SLAVE_RESET_ONLY_IF_NECESSARY // LEGACY-OK: upstream macro name
 * reset_if_unresponsive();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SLAVE_RESET_ONLY_IF_NECESSARY (0) /* LEGACY-OK: upstream esp-hosted macro name */

/**
 * @def H_RESET_ON_EVERY_BOOTUP
 * @brief The complement of the conditional-reset switch above.
 * @details One. The vendored SDIO driver names this only in the comment on
 * the ``#else`` half of its reset selector, so nothing reads it; it is
 * defined here so the pair is stated explicitly rather than left implied by
 * a comment.
 * @note Read-only build configuration.
 * @warning Setting both this and the conditional-reset switch to 1
 *          would be self-contradictory, and nothing checks.
 * @par Example:
 * @code
 * static_assert(H_RESET_ON_EVERY_BOOTUP == 1, "reset policy");
 * @endcode
 * @since 0.1.0
 */
#define H_RESET_ON_EVERY_BOOTUP (1)

/**
 * @def H_TRANSPORT_RESTART_ON_FAILURE
 * @brief Whether a transport failure restarts the whole host.
 * @details Zero. On this board the RA8 owns the display, the storage and the
 * reader state; rebooting it because a radio co-processor stopped answering
 * would turn a recoverable link fault into a lost reading position. The
 * transport-failure event is raised either way, and the application decides.
 * @note Read-only build configuration.
 * @warning With this at 1 the SDIO driver calls ``_h_restart_host()`` from
 *          the receive path, which on this port is a system reset.
 * @par Example:
 * @code
 * #if H_TRANSPORT_RESTART_ON_FAILURE
 * g_h.funcs->_h_restart_host();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_TRANSPORT_RESTART_ON_FAILURE (0)

/* ----------------------------------------------------------------------- */
/* UART */
/* ----------------------------------------------------------------------- */

/**
 * @def H_UART_HOST_TRANSPORT
 * @brief Whether the host drives the UART transport.
 * @details Zero, for the same reason as ::H_SPI_HD_HOST_INTERFACE: it feeds
 * the capability report in ``transport_drv.c``, which must describe the link
 * this image actually opens.
 * @note Read-only build configuration.
 * @warning Must agree with ``H_TRANSPORT_IN_USE``.
 * @par Example:
 * @code
 * #elif H_UART_HOST_TRANSPORT
 * report_uart_capabilities(cap);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_UART_HOST_TRANSPORT (0)

/**
 * @def H_UART_CHECKSUM
 * @brief Whether UART frames carry the esp-hosted checksum.
 * @details One. A UART has no framing beyond the start bit, so the header
 * checksum is what tells a resynchronising receiver that it has found a real
 * frame boundary rather than payload that happens to look like one.
 * @note Read-only build configuration.
 * @warning The vendored driver also has one ``#if`` on a misspelt
 *          ``HOSTED_UART_CHECKSUM``, which is undefined upstream and stays
 *          undefined here; do not define it to paper over that.
 * @par Example:
 * @code
 * #if H_UART_CHECKSUM
 * header->checksum = htole16(compute_checksum(buf, len));
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_UART_CHECKSUM (1)

/**
 * @def H_UART_TX_QUEUE_SIZE
 * @brief Frames queued towards the co-processor over UART.
 * @details ::H_TRANSPORT_QUEUE_SIZE, keeping every transport's queue depth
 * derived from one number.
 * @note Read-only build configuration.
 * @warning Also multiplies into the transmit semaphore count.
 * @par Example:
 * @code
 * h_uart_mempool_create(H_UART_TX_QUEUE_SIZE, H_UART_RX_QUEUE_SIZE);
 * @endcode
 * @since 0.1.0
 */
#define H_UART_TX_QUEUE_SIZE (H_TRANSPORT_QUEUE_SIZE)

/**
 * @def H_UART_RX_QUEUE_SIZE
 * @brief Frames queued from the co-processor over UART.
 * @details ::H_TRANSPORT_QUEUE_SIZE; see ::H_UART_TX_QUEUE_SIZE.
 * @note Read-only build configuration.
 * @warning Also multiplies into the receive semaphore count.
 * @par Example:
 * @code
 * queue = g_h.funcs->_h_create_queue(H_UART_RX_QUEUE_SIZE, sizeof(h));
 * @endcode
 * @since 0.1.0
 */
#define H_UART_RX_QUEUE_SIZE (H_TRANSPORT_QUEUE_SIZE)
