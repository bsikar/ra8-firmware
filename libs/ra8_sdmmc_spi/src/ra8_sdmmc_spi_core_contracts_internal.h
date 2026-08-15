/**
 * @file ra8_sdmmc_spi_core_contracts_internal.h
 * @brief Contracts for file-local SD protocol helpers.
 * @details Declares only helpers defined by ra8_sdmmc_spi.c so their complete
 *          contracts stay authoritative without inflating the implementation.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifndef RA8_SDMMC_SPI_CORE_CONTRACTS_H
/** @brief Include guard for the SD protocol-helper contracts. */
#define RA8_SDMMC_SPI_CORE_CONTRACTS_H

#include "ra8_sdmmc_spi_internal.h"

/**
 * @brief Serialize one command and argument into an SD wire frame.
 * @details Writes the command byte, big-endian argument, and required CRC/end
 * bits.
 * @param[in] cmd Encoded SD command byte.
 * @param[in] arg Command argument in host order.
 * @param[out] out_frame Writable six-byte frame.
 * @pre @p out_frame addresses at least six writable bytes.
 * @pre @p cmd is a command supported by the driver.
 * @post @p out_frame contains a complete command frame.
 * @post No transport or driver state is changed.
 * @note CMD0 and CMD8 use their mandated fixed CRC values.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_build_frame(sd_cmd_t cmd, uint32_t arg, uint8_t* out_frame);

/**
 * @brief Poll and decode the one-byte R1 command response.
 * @details Clocks bounded idle bytes until the card clears the response high
 * bit.
 * @param[out] out_r1 Destination for the accepted R1 byte.
 * @return ra8_err_t Response polling result.
 * @retval k_ra8_ok A valid R1 byte was received.
 * @retval other Transport failure or bounded response timeout.
 * @pre Chip select is asserted for the active command.
 * @pre @p out_r1 is non-null and writable.
 * @post Success publishes exactly one R1 byte through @p out_r1.
 * @post Failure does not publish an unvalidated response.
 * @note Polling is bounded by the SD response limit.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_read_r1(uint8_t* out_r1);

/**
 * @brief Read the four-byte tail of an R3 or R7 response.
 * @details Uses the bulk transport and a bounded byte fallback when necessary.
 * @param[out] out_word Destination for the assembled big-endian response word.
 * @return ra8_err_t Tail transfer result.
 * @retval k_ra8_ok Four response bytes were assembled.
 * @retval other Propagated transport error.
 * @pre Chip select remains asserted after a successful R1 response.
 * @pre @p out_word is non-null and writable.
 * @post Success publishes the complete response word.
 * @post Failure leaves no partially accepted protocol result.
 * @note The fallback preserves transports that reject multi-byte transfers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_read_r3_or_r7_tail(uint32_t* out_word);

/**
 * @brief Decode a CSD register into 512-byte logical blocks.
 * @details Handles the SD CSD v1 and v2 capacity encodings with checked
 * arithmetic.
 * @param[in] csd Sixteen-byte CSD register image.
 * @return Decoded logical block count.
 * @retval 0 The CSD version or geometry is invalid.
 * @retval other Positive count of 512-byte blocks.
 * @pre @p csd addresses a complete CSD response.
 * @pre CSD bytes remain immutable during decoding.
 * @post The input register image is unchanged.
 * @post The result is zero or a representable block count.
 * @note This routine performs no transport I/O.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_csd_to_blocks(const uint8_t* csd);

/**
 * @brief Send CMD0 and require the idle-state response.
 * @details Brackets GO_IDLE_STATE with chip select and validates R1 exactly.
 * @return ra8_err_t CMD0 transaction result.
 * @retval k_ra8_ok The card entered SPI idle state.
 * @retval other Transport, timeout, or protocol error.
 * @pre The transport is bound and wake clocks were sent.
 * @pre Driver initialization is not yet published.
 * @post Chip select is released before return.
 * @post Success confirms the idle-state R1 value.
 * @note This is the first command of initialization.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_send_cmd0(void);

/**
 * @brief Probe SD v2 voltage support with CMD8.
 * @details Validates the R7 voltage/check pattern or classifies an illegal CMD8
 * as v1.
 * @param[out] out_is_v2 Receives true when the CMD8 echo is valid.
 * @return ra8_err_t CMD8 probe result.
 * @retval k_ra8_ok Card generation was classified.
 * @retval other Transport or malformed-response error.
 * @pre CMD0 completed and the card remains idle.
 * @pre @p out_is_v2 is non-null and writable.
 * @post Success publishes the card-generation classification.
 * @post Chip select is released before return.
 * @note Illegal-command R1 is the supported v1 classification path.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_send_cmd8(bool* out_is_v2);

/**
 * @brief Poll ACMD41 until the card leaves idle state.
 * @details Sends CMD55/ACMD41 pairs with the HCS argument selected by
 * generation.
 * @param[in] is_v2 True when CMD8 identified an SD v2 card.
 * @return ra8_err_t Readiness-poll result.
 * @retval k_ra8_ok The card reported ready.
 * @retval other Transport, protocol, or bounded-attempt timeout.
 * @pre CMD8 classification has completed.
 * @pre The transport remains bound at initialization speed.
 * @post Success confirms the card left idle state.
 * @post Every attempt releases chip select before the next.
 * @note The attempt count is compile-time bounded.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_acmd41_loop(bool is_v2);

/**
 * @brief Read OCR and classify high-capacity addressing.
 * @details Sends CMD58, validates R1, and inspects the CCS bit in the R3 tail.
 * @param[out] out_is_hc Receives the OCR high-capacity classification.
 * @return ra8_err_t OCR transaction result.
 * @retval k_ra8_ok OCR was read and classified.
 * @retval other Transport or protocol error.
 * @pre ACMD41 reported the card ready.
 * @pre @p out_is_hc is non-null and writable.
 * @post Success publishes the CCS classification.
 * @post Chip select is released before return.
 * @note SD v1 initialization skips this helper.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_read_ocr(bool* out_is_hc);

/**
 * @brief Set the fixed 512-byte block length for byte-addressed cards.
 * @details Sends CMD16 and requires a ready R1 response.
 * @return ra8_err_t Block-length command result.
 * @retval k_ra8_ok The card accepted 512-byte blocks.
 * @retval other Transport or protocol error.
 * @pre Card generation and capacity have been classified.
 * @pre The transport remains bound.
 * @post Chip select is released before return.
 * @post Success confirms the block length required by the public API.
 * @note SDHC cards do not require CMD16.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_set_block_len(void);

/**
 * @brief Map generation and CCS facts to the public card type.
 * @details Gives high-capacity classification precedence over SD version.
 * @param[in] is_v2 True for a valid CMD8 echo.
 * @param[in] is_hc True when OCR advertises block addressing.
 * @return Classified card type.
 * @retval k_ra8_sdmmc_spi_type_sdhc High-capacity card.
 * @retval other SD v1 or SD v2 standard-capacity type.
 * @pre Both facts were obtained from validated protocol responses.
 * @pre The two Boolean inputs are stable for the call.
 * @post No driver or transport state is changed.
 * @post A supported non-unknown card type is returned.
 * @note This routine performs no I/O.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_sdmmc_spi_card_type_t internal_classify_card(bool is_v2, bool is_hc);

/**
 * @brief Execute the wake, CMD0, CMD8, ACMD41, and OCR probe stages.
 * @details Recovers a stuck card once and publishes only validated
 * classification facts.
 * @param[out] out_is_v2 Receives the card-generation fact.
 * @param[out] out_is_hc Receives the high-capacity fact.
 * @return ra8_err_t Probe sequence result.
 * @retval k_ra8_ok Both classification facts are valid.
 * @retval other Transport, timeout, or protocol error.
 * @pre The transport is bound and initialization is unpublished.
 * @pre Both output pointers are non-null and writable.
 * @post Success publishes both classification facts.
 * @post Failure leaves initialization unpublished.
 * @note Recovery remains bounded and does not recurse.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_probe_card(bool* out_is_v2, bool* out_is_hc);

#endif
