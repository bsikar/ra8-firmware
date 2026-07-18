/**
 * @file ra8_sdhi.h
 * @brief SD/MMC Host Interface (SDHI) driver scaffold
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * introduces a minimal SDHI driver scaffold covering the
 * lifecycle + status + IRQ + power-transition surface, the polled
 * command + block-transfer engine, and bus-width negotiation. The
 * default bus width is the conservative 1-bit mode; callers widen an
 * SD card to 4-bit only after it acknowledges ACMD6 (SET_BUS_WIDTH),
 * or an eMMC device to 8-bit after it acknowledges CMD6 (SWITCH).
 *
 * API surface:
 *
 * - ``ra8_sdhi_init(instance)`` -- MSTP enable + clear IRQ masks
 * - ``ra8_sdhi_deinit(instance)`` -- disable + MSTP release
 * - ``ra8_sdhi_get_status`` -- SD_INFO1/INFO2 mask
 * - ``ra8_sdhi_clear_status`` -- clear SD_INFO1/INFO2 bits
 * - ``ra8_sdhi_attach_handler`` -- install IRQ callback
 * - ``ra8_sdhi_enter_stop / exit_stop`` -- power transition
 * - ``ra8_sdhi_dispatch`` -- ISR entry point
 * - ``ra8_sdhi_set_bus_width`` -- program SD_OPTION.WIDTH / WIDTH8
 * - ``ra8_sdhi_set_bus_width_4bit`` -- ACMD6 negotiate 4-bit bus (SD)
 * - ``ra8_sdhi_set_bus_width_8bit`` -- CMD6 SWITCH negotiate 8-bit bus (eMMC)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @typedef ra8_sdhi_event_fn_t
 * @brief SDHI event callback.
 */
typedef void (*ra8_sdhi_event_fn_t)(void* ctx, uint8_t instance, uint32_t status_mask);

/**
 * @enum ra8_sdhi_bus_width_t
 * @brief SD data-bus width accepted by ::ra8_sdhi_set_bus_width.
 *
 * @details
 * The numeric values equal the physical lane count so callers read as
 * intent. They map onto the SD_OPTION.WIDTH / WIDTH8 bit pair via the
 * HUM Ch 47.2.16 truth table (1-bit: WIDTH=1,WIDTH8=0; 4-bit:
 * WIDTH=0,WIDTH8=0; 8-bit: WIDTH=0,WIDTH8=1).
 *
 * @invariant Only these three values are valid; any other is rejected
 *            with ``k_ra8_err_invalid_arg``.
 *
 * @see ra8_sdhi_set_bus_width()
 * @see ra8_sdhi_set_bus_width_4bit()
 */
typedef enum : uint8_t {
  k_ra8_sdhi_bus_width_unset = 0U, /**< Unset: leave the bus at its power-on width. */
  k_ra8_sdhi_bus_width_1bit  = 1U, /**< Single data lane (power-on safe default).   */
  k_ra8_sdhi_bus_width_4bit  = 4U, /**< Four data lanes (SD default-speed wide).    */
  k_ra8_sdhi_bus_width_8bit  = 8U, /**< Eight data lanes (eMMC only).               */
} ra8_sdhi_bus_width_t;

/**
 * @brief Initialise an SDHI instance.
 * @param[in] instance SDHI instance (0 or 1).
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdhi_init(uint8_t instance);

/**
 * @brief Issue a single SD command and read the 4-word response.
 *
 * @details
 * polling primitive. Loads SD_ARG with ``arg``, writes
 * ``cmd`` to SD_CMD, polls SD_INFO1.RSPEND for completion, and
 * copies SD_RSP10/32/54/76 into ``out_rsp[0..3]``. The caller
 * encodes the SD command index + response type in ``cmd``.
 *
 * @param[in] instance SDHI instance.
 * @param[in] cmd Pre-encoded SD_CMD register value.
 * @param[in] arg 32-bit command argument (zero-extended).
 * @param[out] out_rsp 4-word response buffer; may be NULL if
 * the command type returns no response.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_sdhi_send_command(uint8_t instance, uint32_t cmd, uint32_t arg, uint32_t* out_rsp);

/**
 * @brief Set the SD bus clock divider (SD_CLK_CTRL).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdhi_set_clock(uint8_t instance, uint32_t divider);

/**
 * @brief Program the host-side SD data-bus width (SD_OPTION.WIDTH / WIDTH8).
 *
 * @details
 * Low-level, host-only setter: it flips the SD_OPTION.WIDTH (bit 15)
 * and WIDTH8 (bit 13) bits per the HUM Ch 47.2.16 truth table while
 * preserving the TOP / CTOP / TOUTMASK timeout fields with a
 * read-modify-write. It does NOT touch the card -- the card's own bus
 * width must already match (negotiate it with
 * ::ra8_sdhi_set_bus_width_4bit first), otherwise transfers corrupt.
 *
 * @param[in] instance SDHI instance (0 or 1).
 * @param[in] width    Desired width: ::k_ra8_sdhi_bus_width_1bit,
 *                     ::k_ra8_sdhi_bus_width_4bit, or
 *                     ::k_ra8_sdhi_bus_width_8bit.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               SD_OPTION updated to the requested width.
 * @retval k_ra8_err_null_ptr     ``instance`` out of range.
 * @retval k_ra8_err_invalid_arg  ``width`` not one of the three valid widths.
 *
 * @pre ::ra8_sdhi_init has been called for ``instance``.
 * @pre The card has been moved to ``width`` on its side (or is in 1-bit).
 * @post On success SD_OPTION.WIDTH / WIDTH8 reflect ``width``.
 * @post Timeout fields (TOP / CTOP / TOUTMASK) are unchanged.
 *
 * @note Not thread-safe; serialize with the rest of the SDHI command path.
 * @warning Widening the host before the card corrupts every transfer.
 *
 * @see ra8_sdhi_set_bus_width_4bit()  Negotiate the card side via ACMD6.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdhi_set_bus_width(uint8_t instance, ra8_sdhi_bus_width_t width);

/**
 * @brief Negotiate a 4-bit data bus with the card via CMD55 + ACMD6.
 *
 * @details
 * Runs the SD application-command handshake that switches an SD card
 * from its power-on 1-bit bus to 4-bit, then widens the host side to
 * match. The sequence is:
 *
 *   1. CMD55 APP_CMD (arg = ``rca`` << 16) -- prefix the next command
 *      as an application command. The R1 response must echo APP_CMD.
 *   2. ACMD6 SET_BUS_WIDTH (arg = 0b10) -- request 4-bit on the card.
 *      The R1 response must carry no error/violation bits.
 *   3. On a clean acknowledgement, call ::ra8_sdhi_set_bus_width with
 *      ::k_ra8_sdhi_bus_width_4bit so the host SD_OPTION follows.
 *
 * If the card declines (missing APP_CMD echo or any R1 error bit) the
 * host is left in 1-bit mode and ``k_ra8_err_not_supported`` is
 * returned -- the conservative default is preserved.
 *
 * @param[in] instance SDHI instance (0 or 1).
 * @param[in] rca      Card relative address published by CMD3.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Card and host both switched to 4-bit.
 * @retval k_ra8_err_null_ptr      ``instance`` out of range.
 * @retval k_ra8_err_hw_timeout    CMD55 or ACMD6 RSPEND never asserted.
 * @retval k_ra8_err_not_supported Card declined ACMD6; host stays 1-bit.
 *
 * @pre Card is in TRAN state (CMD7 selected) with a known ``rca``.
 * @pre ::ra8_sdhi_init has been called for ``instance``.
 * @post On success the card and host are both 4-bit.
 * @post On failure the host bus width is unchanged (1-bit).
 *
 * @note Blocking, polled; not safe to call from an ISR.
 * @see ra8_sdhi_set_bus_width()  Host-only width setter this drives.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdhi_set_bus_width_4bit(uint8_t instance, uint16_t rca);

/**
 * @brief Negotiate an 8-bit data bus with an eMMC device via CMD6 SWITCH.
 *
 * @details
 * The 8-bit data bus is an eMMC-only capability (SD cards top out at
 * the 4-bit ACMD6 path). Unlike ACMD6 this uses the native JEDEC CMD6
 * SWITCH command -- no CMD55 application prefix -- to write the
 * EXT_CSD BUS_WIDTH byte (index 183) with value 2 (8-bit SDR). The
 * sequence is:
 *
 *   1. CMD6 SWITCH (arg = ::k_ra8_sdhi_cmd6_arg_8bit) -- request the
 *      EXT_CSD BUS_WIDTH write. The R1b response must carry no error
 *      or status-violation bits (::k_ra8_sdhi_r1_error_mask).
 *   2. On a clean acknowledgement, call ::ra8_sdhi_set_bus_width with
 *      ::k_ra8_sdhi_bus_width_8bit so the host SD_OPTION follows.
 *
 * If the device declines (any R1 error bit) the host is left at its
 * current width and ``k_ra8_err_not_supported`` is returned -- an SD
 * card, which cannot do 8-bit, lands here and stays narrow.
 *
 * @param[in] instance SDHI instance (0 or 1).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Device and host both switched to 8-bit.
 * @retval k_ra8_err_null_ptr      ``instance`` out of range.
 * @retval k_ra8_err_hw_timeout    CMD6 RSPEND never asserted.
 * @retval k_ra8_err_not_supported Device declined CMD6; host width unchanged.
 *
 * @pre  The device is an eMMC in TRAN state (CMD7 selected).
 * @pre  ::ra8_sdhi_init has been called for ``instance``.
 * @post On success the device and host are both 8-bit.
 * @post On failure the host bus width is unchanged.
 *
 * @note Blocking, polled; not safe to call from an ISR.
 * @see ra8_sdhi_set_bus_width_4bit()  The SD ACMD6 4-bit counterpart.
 * @see ra8_sdhi_set_bus_width()       Host-only width setter this drives.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdhi_set_bus_width_8bit(uint8_t instance);

/**
 * @brief Tear down an SDHI instance.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdhi_deinit(uint8_t instance);

/**
 * @brief Read the SD_INFO1 status register.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdhi_get_status(uint8_t instance, uint32_t* out_mask);

/**
 * @brief Clear SD_INFO1 status bits via write-0-to-clear.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdhi_clear_status(uint8_t instance, uint32_t mask);

/**
 * @brief Attach an SDHI event callback (shared across instances).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdhi_attach_handler(ra8_sdhi_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an SDHI event -- snapshot status + fire callback.
 *
 * @details
 * Called from the SDHI access / DMA-end / SDIO ISR (HUM Ch 50
 * "SD/MMC Host Interface (SDHI)", p 2655) to snapshot SD_INFO1 /
 * SD_INFO2 and invoke the registered handler. Spurious IRQs and
 * out-of-range ``instance`` values are silently ignored.
 *
 * @param[in] instance SDHI instance index (0 or 1).
 *
 * @pre Called from ISR context or a host-test driver.
 * @pre ``instance`` < 2.
 *
 * @post Stored callback (if any) has been invoked exactly once.
 * @post Status latch is left for the caller to clear via
 *       ``ra8_sdhi_clear_status``.
 *
 * @note Not thread-safe; pair with NVIC masking.
 * @since 0.1.0
 */
void ra8_sdhi_dispatch(uint8_t instance);

/**
 * @brief Put an SDHI instance into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdhi_enter_stop(uint8_t instance);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdhi_exit_stop(uint8_t instance);

/**
 * @brief Read one or more 512-byte SD blocks via the SD_BUF0 FIFO.
 *
 * @details
 * Polled (PIO-style) block-read primitive. Mirrors FSP
 * ``R_SDHI_Read`` -> ``r_sdhi_read_write_common`` (file
 * ``r_sdhi.c``, lines 450..490 and 1383..1405) without the DMA/DTC
 * machinery: the driver loads SD_SECCNT / SD_SIZE, kicks the
 * READ_SINGLE_BLOCK (CMD17) or READ_MULTIPLE_BLOCK (CMD18) command,
 * waits for SD_INFO1.RSPEND, then drains 512 bytes per block from
 * SD_BUF0 in 4-byte words while polling SD_INFO2.BRE.
 *
 * Algorithm:
 *  1. Validate ``buf`` non-NULL and ``block_count > 0``
 *  2. Write SD_STOP = SECCNT_ENABLE (multi-block only)
 *  3. Write SD_SECCNT = ``block_count``
 *  4. Write SD_SIZE = 512 (block size)
 *  5. Write SD_ARG = ``lba`` (sector address)
 *  6. Write SD_CMD = CMD17 (single) or CMD18 (multi)
 *  7. Poll SD_INFO1.RSPEND for command-response complete
 *  8. For each of ``block_count * 128`` words: poll SD_INFO2.BRE,
 *     copy SD_BUF0 -> ``buf``
 *  9. For multi-block: issue CMD12 STOP_TRANSMISSION
 *  10. Clear SD_INFO1 / SD_INFO2 flags
 *
 * @param[in] instance SDHI instance (0 or 1).
 * @param[in] lba Logical block address (sector number).
 * @param[out] buf Destination buffer; must hold at least
 *                 ``block_count * 512`` bytes.
 * @param[in] block_count Number of 512-byte blocks to read; must be > 0.
 *
 * @retval k_ra8_ok Success.
 * @retval k_ra8_err_null_ptr ``buf`` was NULL or ``instance`` invalid.
 * @retval k_ra8_err_invalid_arg ``block_count`` was 0.
 * @retval k_ra8_err_hw_timeout RSPEND or BRE poll exceeded the spin budget.
 *
 * @pre Card has been initialized through CMD0..ACMD41 + CMD2/3/7 by the consumer.
 * @pre ``ra8_sdhi_init`` has been called for ``instance``.
 * @post On success ``buf[0..block_count*512]`` holds card data.
 * @post SD_INFO1.RSPEND and SD_INFO2 BRE bits are cleared.
 *
 * @note Blocking, polled implementation; not safe to call from an ISR.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_sdhi_read_block(uint8_t instance, uint32_t lba, uint8_t* buf, uint32_t block_count);

/**
 * @brief Write one or more 512-byte SD blocks via the SD_BUF0 FIFO.
 *
 * @details
 * Polled (PIO-style) block-write primitive. Mirrors FSP
 * ``R_SDHI_Write`` -> ``r_sdhi_read_write_common`` (file
 * ``r_sdhi.c``, lines 509..554 and 1383..1405) without the DMA/DTC
 * machinery: the driver loads SD_SECCNT / SD_SIZE, kicks the
 * WRITE_SINGLE_BLOCK (CMD24) or WRITE_MULTIPLE_BLOCK (CMD25)
 * command, waits for SD_INFO1.RSPEND, then pushes 512 bytes per
 * block into SD_BUF0 in 4-byte words while polling SD_INFO2.BWE.
 *
 * @param[in] instance SDHI instance (0 or 1).
 * @param[in] lba Logical block address (sector number).
 * @param[in] buf Source buffer; must hold at least
 *                ``block_count * 512`` bytes.
 * @param[in] block_count Number of 512-byte blocks to write; must be > 0.
 *
 * @retval k_ra8_ok Success.
 * @retval k_ra8_err_null_ptr ``buf`` was NULL or ``instance`` invalid.
 * @retval k_ra8_err_invalid_arg ``block_count`` was 0.
 * @retval k_ra8_err_hw_timeout RSPEND or BWE poll exceeded the spin budget.
 *
 * @pre Card has been initialized through CMD0..ACMD41 + CMD2/3/7 by the consumer.
 * @pre Card is not write-protected (caller responsibility).
 * @post On success the requested block range has been pushed into the SDHI FIFO.
 * @post For multi-block writes a CMD12 STOP_TRANSMISSION has been issued.
 *
 * @note Blocking, polled implementation; not safe to call from an ISR.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_sdhi_write_block(uint8_t instance, uint32_t lba, const uint8_t* buf, uint32_t block_count);

/**
 * @brief Enable or disable DMAC-driven SDHI transfers.
 *
 * @details
 * Toggles SD_DMAEN.DMAEN and the SD_INFO2_MASK BREM/BWEM bits in
 * lock-step the way FSP r_sdhi_transfer_read / r_sdhi_transfer_write
 * do. With DMA enabled the polled BRE / BWE wait in the read/write
 * helpers above must be replaced by an external transfer primitive
 * that targets SD_BUF0; this function is the toggle point.
 *
 * @param[in] instance SDHI instance.
 * @param[in] enable Non-zero to enable DMA, 0 to fall back to PIO.
 *
 * @retval k_ra8_ok Success.
 * @retval k_ra8_err_null_ptr ``instance`` invalid.
 *
 * @pre ``ra8_sdhi_init`` has been called for ``instance``.
 * @post SD_DMAEN reflects ``enable``.
 *
 * @note The DMAC channel itself must be wired up by the caller via
 *       ``ra8_dmac`` before any transfer is started.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdhi_attach_dma(uint8_t instance, uint8_t enable);

#ifdef __cplusplus
}
#endif
