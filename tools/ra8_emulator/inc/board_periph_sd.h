/**
 * @file board_periph_sd.h
 * @brief SD-card-over-SPI device model for ra8_emulator (attached to SPI_B).
 *
 * @details
 * Models a high-capacity (SDHC) SD card running in SPI mode, backed by a
 * host image file passed with @c --sd. The SPI_B block model
 * (@c board_periph_spi.c) routes each SPDR byte exchange into
 * @ref board_sd_exchange when a card is attached and the channel is NOT in
 * internal loopback, so the firmware's genuine @c ra8_sdmmc_spi command /
 * response / data-token path runs against a real FAT image.
 *
 * The protocol handled: CMD0 / CMD8 / CMD55+ACMD41 / CMD58 / CMD9 (CSD) /
 * CMD17 (single-block read), with R1/R3/R7 responses, the 0xFE data token,
 * and CRC16-CCITT data CRCs. Commands self-frame off the @c 01xxxxxx lead
 * bits, so no chip-select wiring is required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

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
 * @pre Called once during ra8_emulator start-up (single-threaded).
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
 * @brief Create a blank, FAT-formatted SD card in memory and attach it.
 *
 * @details Formats an empty FAT16 or FAT32 volume (complete BPB + FAT media /
 * EOC markers, valid to a host `fsck_msdos` and the firmware's ra8_fs) so a card
 * of an arbitrary size / format can be set up with no pre-built image.
 *
 * @param[in] total_sectors Card size in 512-byte sectors (>= 64).
 * @param[in] fat_bits      16 for FAT16, 32 for FAT32 (other values -> FAT16).
 * @param[in] label         Up to 11-char volume label (NULL -> blank).
 *
 * @return true on success (a blank card is attached and serving).
 * @retval false Size too small or allocation failed.
 * @pre Called once during ra8_emulator start-up (single-threaded).
 * @post On success @ref board_sd_attached returns true.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool board_sd_attach_blank(uint32_t total_sectors, uint8_t fat_bits, const char* label);

/**
 * @brief Write the current SD-card image (with any firmware writes) to a file.
 *
 * @param[in] path Output path for the raw card image.
 * @return true on success.
 * @retval false No card attached, or the file could not be written.
 * @pre A card is attached.
 * @post The host file holds the current card contents.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool board_sd_save(const char* path);

/**
 * @brief Read back the attached card's summary (for the status view / report).
 *
 * @param[out] attached Receives whether a card is attached (may be NULL).
 * @param[out] bytes    Receives the card size in bytes, 64-bit for >4 GB (NULL ok).
 * @param[out] fat_bits Receives 12/16/32 if formatted by --sd-new, else 0 (NULL ok).
 * @param[out] label    Receives a pointer to the NUL-terminated label (NULL ok).
 * @pre None.
 * @post No card state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void board_sd_info(bool* attached, uint64_t* bytes, uint8_t* fat_bits, const char** label);

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
 * @brief Copy one 512-byte block straight out of the backing image.
 *
 * @details
 * The byte-identical data CMD17 would stream back, served in a single call so
 * ra8_emulator's `--fast-sd` block-read hook can fill the firmware's sector buffer
 * without clocking 512 individual SPI byte-exchanges through MMIO. Uses the same
 * SDHC block-addressing (@p lba * 512) as the modelled CMD17 path, so the bytes
 * delivered are exactly those the full protocol would produce.
 *
 * @param[in]  lba Logical block address (SDHC block units).
 * @param[out] dst Destination for 512 bytes; must hold at least 512 bytes.
 *
 * @return true if a card is attached and @p lba is within the image.
 * @retval false No card attached, or @p lba is past the end of the image.
 * @pre @p dst is non-null and sized for >= 512 bytes.
 * @pre A card image is attached (@ref board_sd_attached is true).
 * @post On true, @p dst holds the block; the backing image is unmodified.
 * @post On false, @p dst is left unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool board_sd_read_block(uint32_t lba, uint8_t* dst);

/**
 * @brief Copy one 512-byte block straight into the backing image.
 *
 * @details
 * The write-side mirror of @ref board_sd_read_block: stores the byte-identical
 * data a CMD24 single-block write would land, served in a single call so the
 * native-SDHI block model can commit the firmware's sector buffer without
 * clocking 128 individual SD_BUF0 FIFO words. Uses the same SDHC block-addressing
 * (@p lba * 512) as the modelled CMD24 path, so the bytes stored are exactly
 * those the full protocol would write.
 *
 * @param[in] lba Logical block address (SDHC block units).
 * @param[in] src Source of 512 bytes; must hold at least 512 bytes.
 *
 * @return true if a card is attached and a full block fits at @p lba.
 * @retval false No card attached, or @p lba is past the end of the image.
 * @pre @p src is non-null and sized for >= 512 bytes.
 * @pre A card image is attached (@ref board_sd_attached is true).
 * @post On true, the block at @p lba holds @p src.
 * @post On false, the backing image is left unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool board_sd_write_block(uint32_t lba, const uint8_t* src);

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
