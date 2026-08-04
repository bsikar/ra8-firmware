/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_periph_eink.h
 * @brief IT8951 e-paper SPI-device model for ra8_emulator (attached to SPI_B).
 *
 * @details
 * Models a Waveshare / IT8951 e-paper timing controller sitting on the SPI_B
 * bus, so the ``epaper_refresh`` example drives the genuine firmware path --
 * ``ra8_epaper`` (Ring 3 HAL) through the ``ra8_display_pal_eink`` backend
 * (Ring 4 PAL) over ``ra8_io_spi_bus`` -- against a responding controller with
 * no physical panel.
 *
 * The controller is attached with ``--eink``. Two seams tie it into the rest of
 * the emulator, mirroring the way ``board_periph_sd.c`` attaches an SD card:
 *
 *  - The SPI_B block model (``board_periph_spi.c``) routes each SPDR byte
 *    exchange into ::board_eink_exchange when a controller is attached and the
 *    channel is NOT in internal loopback, so the firmware's real preamble /
 *    command / register / image-load / display path runs byte for byte.
 *  - The GPIO/PORT block model (``board_periph_gpio.c``) calls
 *    ::board_eink_apply_gpio_defaults from its power-on reset so the panel's
 *    HRDY "ready" line reads high -- the firmware polls HRDY before every
 *    preamble, so without it ``ra8_epaper_init`` would time out.
 *
 * The IT8951 SPI protocol modelled here follows the IT8951 datasheet rev 0.2
 * chapter 3.4 "SPI Interface" + chapter 4 "Application Note": every transaction
 * opens with a 16-bit preamble (``0x6000`` command / ``0x0000`` data write /
 * ``0x1000`` data read), then the host clocks 16-bit words MSB-first. The model
 * assembles those words from the byte stream and answers reads (GET_DEV_INFO
 * device block, register reads such as LUTAFSR "LUT idle") so the driver
 * completes exactly as it would on silicon.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Attach (arm) the modelled IT8951 e-paper controller.
 *
 * @details
 * Sets the module's armed flag so ::board_eink_attached returns true and the
 * SPI_B block routes byte exchanges into ::board_eink_exchange. Idempotent: a
 * second call is a no-op. Panel geometry is the fixed demo size the model
 * reports through GET_DEV_INFO.
 *
 * @return true once the controller is armed and serving.
 * @retval true Controller armed (always, on this in-memory model).
 *
 * @pre Called once during ra8_emulator start-up (single-threaded arg parse).
 * @pre No physical hardware is required.
 * @post ::board_eink_attached returns true.
 * @post The command / read framing is reset to power-on.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
bool board_eink_attach(void);

/**
 * @brief Report whether the IT8951 controller is currently attached.
 *
 * @return true if ``--eink`` armed the controller.
 * @retval false No ``--eink`` was passed.
 * @retval true  ``--eink`` armed the controller.
 *
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @post No state is modified.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
bool board_eink_attached(void);

/**
 * @brief Exchange one full-duplex SPI byte with the modelled controller.
 *
 * @details
 * Advances the IT8951 byte state machine: assembles 16-bit words from the host
 * TX stream, latches preambles / commands / register + image data, and returns
 * the controller's response byte during a read burst (0xFF idle otherwise).
 *
 * @param[in] tx Byte clocked out by the host (host-out).
 *
 * @return The byte the controller drives back (controller-out).
 * @retval 0 Bus idle / no read byte pending.
 *
 * @pre A controller is attached (::board_eink_attached is true).
 * @pre None.
 * @post The model's command / read framing may advance by one byte.
 * @post The captured pixel / refresh counters may advance.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
uint8_t board_eink_exchange(uint8_t tx);

/**
 * @brief Reset the controller's command / read framing to power-on.
 *
 * @details Clears the in-flight word assembly, current command, register-read
 * target and read cursor; the armed / attached flag is preserved. Called from
 * the SPI_B block's power-on reset (``board_periph_spi.c``).
 *
 * @return None.
 * @pre None.
 * @pre None.
 * @post The byte state machine is back at "expect preamble".
 * @post The attached flag is unchanged.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
void board_eink_reset(void);

/**
 * @brief Re-arm the panel HRDY "ready" GPIO input high (if attached).
 *
 * @details
 * Drives the modelled HRDY pin high through ``board_periph_gpio_set_input`` so
 * the firmware's ``ra8_gpio_read`` of the busy line returns "ready". Called from
 * the GPIO/PORT block's power-on reset AFTER it clears every port, so the level
 * survives the reset that would otherwise clear it. A no-op when no controller
 * is attached.
 *
 * @return None.
 * @pre The GPIO/PORT block model is registered.
 * @pre Called after the GPIO block cleared its ports.
 * @post When attached, the HRDY pin reads high.
 * @post When not attached, no GPIO state is modified.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
void board_eink_apply_gpio_defaults(void);

/**
 * @brief Print the controller's end-of-run summary line (if attached).
 *
 * @details One stderr line reporting the pixels loaded and refreshes issued,
 * so a run visibly exercised the image-load + display path. A no-op when no
 * controller is attached.
 *
 * @return None.
 * @pre None.
 * @pre None.
 * @post No model state is modified.
 * @post At most one line is written to stderr.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
void board_eink_report(void);

#ifdef __cplusplus
}
#endif
