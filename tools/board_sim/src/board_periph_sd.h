/**
 * @file board_periph_sd.h
 * @brief SD-card-over-SPI device model for board_sim (attached to SPI_B).
 *
 * @details
 * Models a high-capacity (SDHC) SD card running in SPI mode, backed by a
 * host image file passed with @c --sd. The SPI_B block model
 * (@c board_periph_spi.c) routes each SPDR byte exchange into
 * @ref board_sd_exchange when a card is attached and the channel is NOT in
 * internal loopback, so the firmware's genuine @c ra_sdmmc_spi command /
 * response / data-token path runs against a real FAT image.
 *
 * The protocol handled: CMD0 / CMD8 / CMD55+ACMD41 / CMD58 / CMD9 (CSD) /
 * CMD17 (single-block read), with R1/R3/R7 responses, the 0xFE data token,
 * and CRC16-CCITT data CRCs. Commands self-frame off the @c 01xxxxxx lead
 * bits, so no chip-select wiring is required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */
#ifndef BOARD_PERIPH_SD_H
#define BOARD_PERIPH_SD_H

#include <stdint.h>

/**
 * @brief Attach an SD-card image from a host file.
 *
 * @details Reads the whole file into memory as the card's block store and
 * arms the model. Idempotent-ish: a second call replaces the image.
 *
 * @param[in] path Host path to a raw FAT image (512-byte sectors).
 * @return true if the image loaded and the card is armed; false on I/O error.
 * @retval false The file could not be opened or read.
 * @pre `path` is non-null.
 * @pre Called once during board_sim start-up (single-threaded).
 * @post On success @ref board_sd_attached returns true.
 * @post On failure no card is attached.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool board_sd_attach(const char* path);

/**
 * @brief Report whether an SD-card image is currently attached.
 *
 * @return true if a card image is loaded and serving.
 * @retval false No `--sd` image was attached.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @post No state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool board_sd_attached(void);

/**
 * @brief Exchange one full-duplex SPI byte with the modelled card.
 *
 * @details Drives the SD SPI-mode state machine: command bytes are framed
 * and answered with the matching R1/R3/R7 + data-token + block stream.
 *
 * @param[in] tx Byte clocked out by the host (host-out).
 * @return The byte the card drives back (card-out); 0xFF when idle.
 * @retval 255 Bus idle / no response pending.
 * @pre A card is attached (@ref board_sd_attached is true).
 * @pre None.
 * @post The model's command / response state may advance.
 * @post The backing image is never modified (read path only).
 * @note Not thread-safe.
 * @since 0.1.0
 */
uint8_t board_sd_exchange(uint8_t tx);

/**
 * @brief Reset the card's command / response framing to power-on.
 *
 * @details Clears the in-flight command collector and pending response;
 * the attached image and learned ready state are preserved.
 *
 * @return None.
 * @pre None.
 * @pre None.
 * @post Framing state is cleared; any attached image stays attached.
 * @post No host I/O is performed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void board_sd_reset(void);

#endif /* BOARD_PERIPH_SD_H */
