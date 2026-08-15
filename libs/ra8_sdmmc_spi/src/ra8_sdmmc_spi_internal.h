/**
 * @file ra8_sdmmc_spi_internal.h
 * @brief Module-private cross-TU surface for the SPI-mode SD card driver.
 * @ingroup grp_storage
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Shared between the core/init translation unit (``ra8_sdmmc_spi.c``) and
 * the block-I/O translation unit (``ra8_sdmmc_spi_io.c``) of the
 * ``ra8_sdmmc_spi`` library. It carries the protocol enums both units use,
 * the single mutable driver-state definition, and the prototypes of the
 * low-level transport / framing helpers that the block-I/O unit calls.
 *
 * This header is private to the ``ra8_sdmmc_spi`` module; no other library
 * includes it. All enum values and helper contracts mirror SD
 * Specification Part 1 Physical Layer Simplified Specification v9.10
 * section 7 ("SPI Mode") -- the same references documented inline at each
 * symbol's original definition site.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_sdmmc_spi.h"

/* ---------------------------------------------------------------------------
 * Protocol constants shared between the core/init and block-I/O TUs
 * ---------------------------------------------------------------------------
 */

/**
 * @enum sd_cmd_t
 * @brief Wire-byte for every command used by this driver.
 *
 * @details
 * The wire byte is ``0x40 | cmd_index`` per SD spec PHY v9 section 7.3.1.1
 * "Command Format" (bit 7 = 0, bit 6 = 1, bits 5..0 = command index).
 */
typedef enum : uint8_t {
  k_sd_cmd_go_idle_state           = 0x40U,       /**< CMD0.   */
  k_sd_cmd_send_if_cond            = 0x40U | 8U,  /**< CMD8.   */
  k_sd_cmd_send_csd                = 0x40U | 9U,  /**< CMD9.   */
  k_sd_cmd_send_cid                = 0x40U | 10U, /**< CMD10.  */
  k_sd_cmd_stop_transmission       = 0x40U | 12U, /**< CMD12.  */
  k_sd_cmd_set_blocklen            = 0x40U | 16U, /**< CMD16.  */
  k_sd_cmd_read_single_block       = 0x40U | 17U, /**< CMD17.  */
  k_sd_cmd_read_multi_block        = 0x40U | 18U, /**< CMD18.  */
  k_sd_cmd_write_single_block      = 0x40U | 24U, /**< CMD24.  */
  k_sd_cmd_write_multi_block       = 0x40U | 25U, /**< CMD25.  */
  k_sd_cmd_erase_wr_blk_start      = 0x40U | 32U, /**< CMD32.  */
  k_sd_cmd_erase_wr_blk_end        = 0x40U | 33U, /**< CMD33.  */
  k_sd_cmd_erase                   = 0x40U | 38U, /**< CMD38.  */
  k_sd_cmd_app_cmd                 = 0x40U | 55U, /**< CMD55.  */
  k_sd_cmd_read_ocr                = 0x40U | 58U, /**< CMD58.  */
  k_sd_acmd_sd_send_op_cond        = 0x40U | 41U, /**< ACMD41. */
  k_sd_acmd_set_wr_blk_erase_count = 0x40U | 23U, /**< ACMD23. */
} sd_cmd_t;

/**
 * @enum sd_data_token_t
 * @brief Data tokens used for block I/O (SD spec PHY v9 section 7.3.3).
 */
typedef enum : uint8_t {
  k_sd_token_data_start_single = 0xFEU, /**< Single-block start. */
  k_sd_token_data_start_multi  = 0xFCU, /**< Multi-block start.  */
  k_sd_token_stop_multi        = 0xFDU, /**< Multi-block stop.   */
  k_sd_token_idle              = 0xFFU, /**< Idle bus.           */
} sd_data_token_t;

/**
 * @enum sd_data_response_t
 * @brief Data-response token bit layout (SD spec PHY v9 section 7.3.3.1).
 *
 * @details
 * Wire layout xxx0sss1 where sss = 010 "accepted", 101 "CRC error",
 * 110 "write error". Mask with ``k_sd_data_response_mask`` first.
 */
typedef enum : uint8_t {
  k_sd_data_response_mask      = 0x1FU, /**< SD data response mask.    */
  k_sd_data_response_accepted  = 0x05U, /**< 0b00101 -- data accepted. */
  k_sd_data_response_crc_err   = 0x0BU, /**< 0b01011 -- CRC error.     */
  k_sd_data_response_write_err = 0x0DU, /**< 0b01101 -- write error.   */
} sd_data_response_t;

/**
 * @enum sd_protocol_const_t
 * @brief Magic argument values and retry budgets.
 */
typedef enum : uint32_t {
  /* CMD0 has a static CRC7-shifted-and-tagged byte of 0x95 (SD spec PHY
   * v9 section 7.3.1.1 example). Pre-computing avoids running CRC7 over
   * the all-zero argument every probe. */
  k_sd_crc7_cmd0_byte = 0x95U, /**< SD crc7 cmd0 byte. */
  /* CMD8 with argument 0x000001AA uses the documented "pattern 0xAA at
   * 2.7-3.6 V" check (SD spec PHY v9 section 7.3.2.6). The CRC7-shifted
   * byte for this exact frame is 0x87, also published in the spec. */
  k_sd_cmd8_arg_check_pattern = 0x000001AAUL, /**< SD cmd8 arg check pattern. */
  k_sd_crc7_cmd8_byte         = 0x87U,        /**< SD crc7 cmd8 byte.         */
  /* ACMD41 with HCS=1 for v2.x cards. */
  k_sd_acmd41_arg_hcs = 0x40000000UL, /**< SD acmd41 arg hcs. */
  /* OCR bit set when CCS = 1 (block-addressed SDHC/SDXC). */
  k_sd_ocr_ccs_bit  = 0x40000000UL, /**< SD ocr ccs bit.  */
  k_sd_ocr_busy_bit = 0x80000000UL, /**< SD ocr busy bit. */
  /* Retry budgets -- bounded loops, NASA P10 Rule 2. */
  k_sd_max_r1_wait_bytes    = 16U,      /**< R1 must appear within 8 bytes per spec; 2x slack.   */
  k_sd_max_data_token_polls = 50000U,   /**< ~500 ms at 100 us / poll.                           */
  k_sd_max_busy_poll_bytes  = 100000U,  /**< Worst-case write timeout.                           */
  k_sd_max_acmd41_attempts  = 1000U,    /**< 1 s at 1 ms / attempt.                              */
  k_sd_init_dummy_clocks    = 80U,      /**< 80 clocks = 10 bytes of 0xFF (>=74 required).       */
  k_sd_recover_flush_bytes  = 530U,     /**< >= 512 data + 2 CRC + token to flush a stuck write. */
  k_sd_recover_idle_bytes   = 64U,      /**< 512 CS-released clocks drain busy + reset framing.  */
  k_sd_max_recover_attempts = 4U,       /**< Re-flush + retry CMD0 before failing.               */
  k_sd_max_erase_poll_bytes = 5000000U, /**< Busy ceiling for a bulk CMD38 erase (seconds).      */
} sd_protocol_const_t;

/**
 * @enum sd_bit_shift_t
 * @brief Bit-shift constants reused across byte packing.
 */
typedef enum : uint8_t {
  k_sd_bit_byte       = 8U,  /**< SD bit byte.       */
  k_sd_bit_two_byte   = 16U, /**< SD bit two byte.   */
  k_sd_bit_three_byte = 24U, /**< SD bit three byte. */
} sd_bit_shift_t;

/**
 * @enum sd_byte_mask_t
 * @brief Generic byte-extraction masks used across argument packing.
 *
 * @details
 * ``k_sd_mask_byte`` isolates the low 8 bits when packing a 32-bit
 * command argument into the 4 big-endian frame bytes (SD spec PHY v9
 * section 7.3.1.1 "Command Format") or splitting a 16-bit CRC16 into
 * its two on-wire bytes. ``k_sd_mask_12bit`` is the CMD8 echo-check
 * mask (SD spec PHY v9 section 7.3.2.6: bits [11:0] of the R7 echo
 * carry the voltage-range code (bits 11:8) and the host check pattern
 * (bits 7:0)).
 */
typedef enum : uint32_t {
  k_sd_mask_byte  = 0xFFU,  /**< Low 8 bits of a wider value.                */
  k_sd_mask_12bit = 0xFFFU, /**< CMD8 R7 echo voltage-range + check pattern. */
} sd_byte_mask_t;

/* ---------------------------------------------------------------------------
 * Shared driver state
 * ---------------------------------------------------------------------------
 */

/**
 * @struct sd_state_t
 * @brief Cached protocol state held across the driver public API.
 *
 * @invariant ``initialized == true`` iff CMD0 ... CMD16 init succeeded.
 */
typedef struct {
  ra8_sdmmc_spi_transport_t transport;       /**< Transport callbacks.  */
  ra8_sdmmc_spi_card_type_t card_type;       /**< Card class.           */
  uint32_t                  capacity_blocks; /**< Block count.          */
  bool                      initialized;     /**< Initialization state. */
} sd_state_t;

/**
 * @var g_sdmmc_spi_state
 * @brief Single driver instance, defined in ``ra8_sdmmc_spi.c``.
 * @details Mutable protocol state shared between the core/init TU and the
 *          block-I/O TU. Renamed with the module infix so it is link-unique.
 * @note Internal mutable state; access from a single thread only.
 * @warning Do not duplicate; exactly one definition exists in
 *          ``ra8_sdmmc_spi.c``.
 * @since 0.1.0
 */
RA8_PRIV extern sd_state_t g_sdmmc_spi_state;

/* ---------------------------------------------------------------------------
 * Low-level helpers shared with the block-I/O TU
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Shift out one byte and capture the response over the transport.
 * @details Wraps a single-byte full-duplex transfer through the bound
 *          ``g_sdmmc_spi_state.transport`` so callers need not stage 1-byte
 *          buffers themselves.
 * @param[in]  tx Byte to clock out.
 * @param[out] rx Received byte, or NULL to discard.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Byte exchanged.
 * @retval other   Propagated from the transport ``xfer`` callback.
 * @pre The transport is bound (driver init has begun).
 * @pre The transport ``xfer`` callback is non-NULL.
 * @post On success @p rx holds the received byte when non-NULL.
 * @post No driver state beyond @p rx is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_sdmmc_spi_xfer_one(uint8_t tx, uint8_t* rx);

/**
 * @brief Shift @p n idle bytes (0xFF) and discard the response.
 * @details Bounded by @p n; clocks the bus idle to advance card timers.
 * @param[in] n Number of idle bytes to clock.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok All @p n bytes clocked.
 * @retval other   Propagated from the transport ``xfer`` callback.
 * @pre The transport is bound.
 * @pre The transport ``xfer`` callback is non-NULL.
 * @post Exactly @p n idle bytes were clocked on success.
 * @post No driver state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_sdmmc_spi_send_idle(uint32_t n);

/**
 * @brief Drive CS low and clock a single idle byte so CIPO is sampled.
 * @details Selects the card and emits one idle byte to align framing.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok CS asserted and one idle byte clocked.
 * @retval other   Propagated from the transport ``cs`` / ``xfer`` callbacks.
 * @pre The transport is bound.
 * @pre The transport ``cs`` callback is non-NULL.
 * @post On success CS is asserted (low).
 * @post One idle byte has been clocked on success.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_sdmmc_spi_cs_assert(void);

/**
 * @brief Drive CS high after clocking one idle byte (SD spec section 7.2.4).
 * @details Releases the card and emits trailing idle clocks so the card can
 *          finish any pending internal state transition.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok CS released and one idle byte clocked.
 * @retval other   Propagated from the transport ``cs`` / ``xfer`` callbacks.
 * @pre The transport is bound.
 * @pre The transport ``cs`` callback is non-NULL.
 * @post On success CS is released (high).
 * @post One idle byte has been clocked on success.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_sdmmc_spi_cs_release(void);

/**
 * @brief Send a command frame and capture the R1 response byte.
 * @details Builds the 6-byte frame (CRC7 included), clocks it out, then
 *          polls for the R1 token.
 * @param[in]  cmd    Command wire byte.
 * @param[in]  arg    32-bit command argument.
 * @param[out] out_r1 Captured R1 response byte.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok          Command sent and R1 captured.
 * @retval k_ra8_err_hw_timeout R1 did not appear within budget.
 * @retval other            Propagated from the transport callbacks.
 * @pre The transport is bound.
 * @pre @p out_r1 is non-NULL.
 * @post On success @p out_r1 holds the R1 byte.
 * @post No driver state beyond @p out_r1 is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_sdmmc_spi_send_command(sd_cmd_t cmd, uint32_t arg, uint8_t* out_r1);

/**
 * @brief Send an ACMD by prefixing it with CMD55 (APP_CMD).
 * @details Issues CMD55 then the application command, returning the
 *          application command's R1.
 * @param[in]  acmd   Application command wire byte.
 * @param[in]  arg    32-bit command argument.
 * @param[out] out_r1 Captured R1 response byte of the application command.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok  Both commands sent and R1 captured.
 * @retval other    Propagated from the transport callbacks.
 * @pre The transport is bound.
 * @pre @p out_r1 is non-NULL.
 * @post On success @p out_r1 holds the application command R1.
 * @post No driver state beyond @p out_r1 is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_sdmmc_spi_send_acmd(sd_cmd_t acmd, uint32_t arg, uint8_t* out_r1);

/**
 * @brief Send CMD12 (STOP_TRANSMISSION) and capture its R1 response byte.
 * @details Builds and clocks the 6-byte CMD12 frame, discards the one stuff
 *          byte that follows it (the tail of the interrupted read stream --
 *          its value is undefined, and a bit7-clear garbage byte would be
 *          misread as R1), then polls for the R1 token. Terminates a CMD18
 *          READ_MULTIPLE_BLOCK stream; the plain ::priv_sdmmc_spi_send_command
 *          cannot be used because it polls for R1 straight after the frame.
 * @param[out] out_r1 Captured R1 response byte.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             CMD12 sent and R1 captured.
 * @retval k_ra8_err_hw_timeout R1 did not appear within budget.
 * @retval other               Propagated from the transport callbacks.
 * @pre The transport is bound.
 * @pre @p out_r1 is non-NULL.
 * @post On success @p out_r1 holds the CMD12 R1 byte.
 * @post No driver state beyond @p out_r1 is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_sdmmc_spi_send_stop_transmission(uint8_t* out_r1);

/**
 * @brief Poll for the data-start token (0xFE), bounded by a retry budget.
 * @details Clocks idle bytes until the single-block data-start token
 *          appears or the poll budget is exhausted.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            Data-start token observed.
 * @retval k_ra8_err_hw_timeout Token did not appear within budget.
 * @retval other              Propagated from the transport callbacks.
 * @pre The transport is bound.
 * @pre CS is asserted by the caller.
 * @post On success the next byte clocked is the first payload byte.
 * @post No driver state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_sdmmc_spi_wait_data_token(void);

/**
 * @brief Wait for the card to release busy (CIPO -> 0xFF), bounded by @p
 * max_polls.
 * @details Clocks idle bytes until the card stops holding CIPO low or the
 *          poll budget is exhausted.
 * @param[in] max_polls Maximum idle bytes to clock before timing out.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            Card released busy.
 * @retval k_ra8_err_hw_timeout Card stayed busy beyond @p max_polls.
 * @retval other              Propagated from the transport callbacks.
 * @pre The transport is bound.
 * @pre CS is asserted by the caller.
 * @post On success the card is no longer busy.
 * @post No driver state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_sdmmc_spi_wait_not_busy_bounded(uint32_t max_polls);

/**
 * @brief Wait for the card to release the busy token (CIPO returns to 0xFF).
 * @details Convenience wrapper over ::priv_sdmmc_spi_wait_not_busy_bounded with
 *          the default worst-case write timeout.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            Card released busy.
 * @retval k_ra8_err_hw_timeout Card stayed busy beyond the default budget.
 * @retval other              Propagated from the transport callbacks.
 * @pre The transport is bound.
 * @pre CS is asserted by the caller.
 * @post On success the card is no longer busy.
 * @post No driver state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_sdmmc_spi_wait_not_busy(void);

/**
 * @brief Validate the transport descriptor: every callback non-NULL.
 * @details Rejects a NULL descriptor or any NULL callback before bind.
 * @param[in] transport Transport descriptor to check.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             Descriptor and all callbacks are non-NULL.
 * @retval k_ra8_err_null_ptr   @p transport is NULL.
 * @retval k_ra8_err_invalid_arg A callback is NULL.
 * @pre None.
 * @pre None.
 * @post No driver state is modified.
 * @post Return reflects only the descriptor contents.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_sdmmc_spi_validate_transport(const ra8_sdmmc_spi_transport_t* transport);

/**
 * @brief Walk the full SD identification sequence and learn capacity / type.
 * @details Runs the CMD0/CMD8/ACMD41/CMD58/CMD9/CMD16 probe and records the
 *          card type and capacity into ::g_sdmmc_spi_state.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Card identified; type and capacity cached.
 * @retval k_ra8_err_protocol_error A response was malformed.
 * @retval other                 Propagated from the probe steps.
 * @pre The transport is bound and the init clock is set.
 * @pre CS is released on entry.
 * @post On success ::g_sdmmc_spi_state.card_type and .capacity_blocks are set.
 * @post CS is released.
 * @note Not thread-safe; single-threaded init.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_sdmmc_spi_run_init_sequence(void);

#ifdef __cplusplus
}
#endif
