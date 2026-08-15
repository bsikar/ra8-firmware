/**
 * @file ra8_sdmmc_spi_io_contracts_internal.h
 * @brief Contracts for file-local SD block-I/O helpers.
 * @details Declares only helpers defined by ra8_sdmmc_spi_io.c so contracts
 *          remain authoritative without expanding the implementation TU.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ra8_sdmmc_spi_internal.h"

/**
 * @brief Validate and stage the transport for initialization.
 * @details Rejects reinitialization, copies callbacks, and selects the init
 * clock.
 * @param[in] transport Complete caller-owned transport descriptor.
 * @return ra8_err_t Preparation result.
 * @retval k_ra8_ok Transport state and init clock are ready.
 * @retval other Invalid state or clock callback failure.
 * @pre @p transport passed the public callback validation.
 * @pre The caller owns the singleton initialization sequence.
 * @post Success binds the transport without publishing initialization.
 * @post Failure leaves initialized false.
 * @note The transport descriptor is copied, not retained by address.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_prepare_init(const ra8_sdmmc_spi_transport_t* transport);

/**
 * @brief Switch to data clock and publish initialized state.
 * @details Performs the final clock callback only after protocol setup
 * succeeds.
 * @return ra8_err_t Finalization result.
 * @retval k_ra8_ok Data clock is active and initialization is published.
 * @retval other Propagated clock callback failure.
 * @pre The full card initialization sequence succeeded.
 * @pre The bound set-clock callback remains valid.
 * @post Success sets initialized true.
 * @post Failure leaves initialized false.
 * @note Publication occurs after the fallible clock transition.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_finalize_init(void);

/**
 * @brief Convert an LBA to the card command argument.
 * @details Uses block addressing for SDHC and checked byte addressing
 * otherwise.
 * @param[in] lba Logical 512-byte block address.
 * @return Command argument for the active card type.
 * @retval other LBA or byte-addressed LBA value.
 * @pre Card type was classified during initialization.
 * @pre @p lba is within the published capacity.
 * @post No state or storage is modified.
 * @post The result follows the active card addressing mode.
 * @note Caller range checks prevent byte-address overflow.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_lba_to_arg(uint32_t lba);

/**
 * @brief Send one command and require a ready R1 response.
 * @details Brackets the command with chip select and rejects any nonzero R1.
 * @param[in] cmd Encoded command byte.
 * @param[in] arg Command argument.
 * @return ra8_err_t Command result.
 * @retval k_ra8_ok The card returned ready.
 * @retval other Transport or protocol error.
 * @pre The card is initialized and transport callbacks are valid.
 * @pre @p cmd is legal for the current operation.
 * @post Chip select is released before return.
 * @post Success confirms an all-clear R1 response.
 * @note Used by erase setup and execution commands.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cmd_require_ready(sd_cmd_t cmd, uint32_t arg);

/**
 * @brief Issue the SD erase-start, erase-end, and erase commands.
 * @details Converts the inclusive range endpoints through the active addressing
 * mode.
 * @param[in] lba First logical block to erase.
 * @param[in] count Number of consecutive blocks.
 * @return ra8_err_t Erase command result.
 * @retval k_ra8_ok The card accepted and completed the erase.
 * @retval other Command, transport, or busy-timeout error.
 * @pre @p count is positive and the range is capacity-checked.
 * @pre The driver is initialized.
 * @post Success completes the requested card erase range.
 * @post Failure is returned without publishing a false success.
 * @note Card-level erase granularity is implementation-defined.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_erase_range(uint32_t lba, uint32_t count);

/**
 * @brief Read one 512-byte payload from the active data stream.
 * @details Clocks exactly one public block into caller storage.
 * @param[out] buf Writable block-sized destination.
 * @return ra8_err_t Payload transfer result.
 * @retval k_ra8_ok Exactly one block was received.
 * @retval other Propagated transport failure.
 * @pre A valid data-start token was consumed.
 * @pre @p buf addresses at least one block.
 * @post Success initializes every destination byte.
 * @post No bytes beyond the destination block are written.
 * @note CRC bytes are handled separately.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_read_block_payload(uint8_t* buf);

/**
 * @brief Read and verify the CRC trailer for one block.
 * @details Compares the two wire CRC bytes against CRC16 of caller data.
 * @param[in] buf Complete block payload to authenticate.
 * @return ra8_err_t CRC validation result.
 * @retval k_ra8_ok Wire and computed CRC values match.
 * @retval other Transport failure or CRC mismatch.
 * @pre @p buf contains one complete block.
 * @pre The stream is positioned at the CRC trailer.
 * @post Input payload remains unchanged.
 * @post Success authenticates the just-read block.
 * @note CRC uses the SD SPI polynomial and seed.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_read_block_crc_check(const uint8_t* buf);

/**
 * @brief Execute one complete CMD17 read data phase.
 * @details Sends the addressed command, waits for a token, reads data, and
 * checks CRC.
 * @param[in] lba Capacity-checked logical block address.
 * @param[out] buf Writable block destination.
 * @return ra8_err_t Single-block read result.
 * @retval k_ra8_ok One authenticated block was read.
 * @retval other Command, token, transfer, or CRC error.
 * @pre The driver is initialized and @p lba is in range.
 * @pre @p buf addresses at least one block.
 * @post Chip select is released before return.
 * @post Success publishes a complete authenticated block.
 * @note The public wrapper owns null and range validation.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_read_data_phase(uint32_t lba, uint8_t* buf);

/**
 * @brief Stop a CMD18 multi-block read stream.
 * @details Sends CMD12, accepts its stuff byte, and waits for card readiness.
 * @return ra8_err_t Stop-sequence result.
 * @retval k_ra8_ok The stream stopped and the card became ready.
 * @retval other Transport, response, or busy-timeout error.
 * @pre A CMD18 stream is active with chip select asserted.
 * @pre The bound transport remains valid.
 * @post The card stop sequence was attempted exactly once.
 * @post Caller can release chip select after return.
 * @note Stop is attempted even after an earlier stream error.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_read_multi_stop(void);

/**
 * @brief Transmit one data block, CRC trailer, and start token.
 * @details Validates the card data-response token and waits out programming
 * busy.
 * @param[in] buf Complete 512-byte source block.
 * @param[in] start_token Single- or multi-block data-start token.
 * @return ra8_err_t Data-block write result.
 * @retval k_ra8_ok The card accepted and programmed the block.
 * @retval other Transport, response, or busy-timeout error.
 * @pre Chip select is asserted for an accepted write command.
 * @pre @p buf addresses one immutable source block.
 * @post Source bytes remain unchanged.
 * @post Success confirms the card left its programming-busy state.
 * @note CRC16 is generated from the source payload.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_write_data_block(const uint8_t* buf, uint8_t start_token);

/**
 * @brief Stream consecutive blocks for an accepted CMD25 operation.
 * @details Writes each source block, then sends the multi-block stop token.
 * @param[in] buf Contiguous immutable source blocks.
 * @param[in] count Number of blocks to transmit.
 * @return ra8_err_t Stream result.
 * @retval k_ra8_ok Every block and the stop token were accepted.
 * @retval other First transfer, response, or busy error.
 * @pre @p buf addresses @p count complete blocks.
 * @pre @p count is positive and capacity-checked.
 * @post Source storage remains unchanged.
 * @post The stop token is attempted before successful return.
 * @note The caller releases chip select around the stream.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_write_multi_stream(const uint8_t* buf, uint32_t count);

/**
 * @brief Adapt the filesystem read callback to SD multi-block reads.
 * @details Narrows checked 64-bit arguments and delegates to the public driver
 * API.
 * @param[in] ctx Unused backend context.
 * @param[in] lba First logical block.
 * @param[in] count Number of blocks.
 * @param[out] buf Writable destination blocks.
 * @return ra8_err_t Adapter result.
 * @retval k_ra8_ok Requested blocks were read.
 * @retval other Validation or driver error.
 * @pre @p buf is non-null for a nonzero request.
 * @pre Inputs fit the driver 32-bit geometry.
 * @post Success initializes all requested destination blocks.
 * @post @p ctx remains untouched.
 * @note The bound backend intentionally uses no context object.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_fs_read_block(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf);

/**
 * @brief Adapt the filesystem write callback to SD multi-block writes.
 * @details Narrows checked 64-bit arguments and delegates to the public driver
 * API.
 * @param[in] ctx Unused backend context.
 * @param[in] lba First logical block.
 * @param[in] count Number of blocks.
 * @param[in] buf Immutable source blocks.
 * @return ra8_err_t Adapter result.
 * @retval k_ra8_ok Requested blocks were written.
 * @retval other Validation or driver error.
 * @pre @p buf is non-null for a nonzero request.
 * @pre Inputs fit the driver 32-bit geometry.
 * @post Source blocks remain unchanged.
 * @post @p ctx remains untouched.
 * @note The bound backend intentionally uses no context object.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_fs_write_block(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf);

/**
 * @brief Adapt the filesystem erase callback to the SD erase API.
 * @details Validates and narrows the filesystem range before delegation.
 * @param[in] ctx Unused backend context.
 * @param[in] lba First logical block.
 * @param[in] count Number of blocks.
 * @return ra8_err_t Adapter result.
 * @retval k_ra8_ok Requested range was erased.
 * @retval other Validation or driver error.
 * @pre The range fits the driver 32-bit geometry.
 * @pre The driver is initialized.
 * @post @p ctx remains untouched.
 * @post Success confirms the erase operation completed.
 * @note Zero-length ranges are rejected by the public layer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_fs_erase_block(void* ctx, uint64_t lba, uint64_t count);

/**
 * @brief Publish SD geometry through the filesystem capacity callback.
 * @details Returns the initialized block count and fixed 512-byte block size.
 * @param[in] ctx Unused backend context.
 * @param[out] block_count Destination for logical block count.
 * @param[out] block_size Destination for bytes per block.
 * @return ra8_err_t Capacity query result.
 * @retval k_ra8_ok Both geometry values were published.
 * @retval other Null-pointer or invalid-state error.
 * @pre Both output pointers are non-null and writable.
 * @pre The driver is initialized with validated capacity.
 * @post Success publishes both geometry values.
 * @post @p ctx remains untouched.
 * @note The block size is always ::k_ra8_sdmmc_spi_block_size.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_fs_get_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size);
