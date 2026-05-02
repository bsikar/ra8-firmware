/**
 * @file ra_sdhi.h
 * @brief SD/MMC Host Interface (SDHI) driver scaffold
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * introduces a minimal SDHI driver scaffold covering the
 * lifecycle + status + IRQ + power-transition surface. Block-level
 * SD card command engine, DMA transfers, and 4-bit / 8-bit wide-bus
 * switching land with the first consumer.
 *
 * API surface:
 *
 * - ``ra_sdhi_init(instance)`` -- MSTP enable + clear IRQ masks
 * - ``ra_sdhi_deinit(instance)`` -- disable + MSTP release
 * - ``ra_sdhi_get_status`` -- SD_INFO1/INFO2 mask
 * - ``ra_sdhi_clear_status`` -- clear SD_INFO1/INFO2 bits
 * - ``ra_sdhi_attach_handler`` -- install IRQ callback
 * - ``ra_sdhi_enter_stop / exit_stop`` -- power transition
 * - ``ra_sdhi_dispatch`` -- ISR entry point
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @typedef ra_sdhi_event_fn_t
 * @brief SDHI event callback.
 */
typedef void (*ra_sdhi_event_fn_t)(void* ctx, uint8_t instance, uint32_t status_mask);

/**
 * @brief Initialise an SDHI instance.
 * @param[in] instance SDHI instance (0 or 1).
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdhi_init(uint8_t instance);

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
[[nodiscard]] ra_err_t
ra_sdhi_send_command(uint8_t instance, uint32_t cmd, uint32_t arg, uint32_t* out_rsp);

/**
 * @brief Set the SD bus clock divider (SD_CLK_CTRL).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdhi_set_clock(uint8_t instance, uint32_t divider);

/**
 * @brief Tear down an SDHI instance.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdhi_deinit(uint8_t instance);

/**
 * @brief Read the SD_INFO1 status register.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdhi_get_status(uint8_t instance, uint32_t* out_mask);

/**
 * @brief Clear SD_INFO1 status bits via write-0-to-clear.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdhi_clear_status(uint8_t instance, uint32_t mask);

/**
 * @brief Attach an SDHI event callback (shared across instances).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdhi_attach_handler(ra_sdhi_event_fn_t fn, void* ctx);

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
 * @return None.
 * @retval None
 *
 * @pre Called from ISR context or a host-test driver.
 * @pre ``instance`` < 2.
 *
 * @post Stored callback (if any) has been invoked exactly once.
 * @post Status latch is left for the caller to clear via
 *       ``ra_sdhi_clear_status``.
 *
 * @note Not thread-safe; pair with NVIC masking.
 * @since 0.1.0
 */
void ra_sdhi_dispatch(uint8_t instance);

/**
 * @brief Put an SDHI instance into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdhi_enter_stop(uint8_t instance);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdhi_exit_stop(uint8_t instance);

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
 * @retval k_ra_ok Success.
 * @retval k_ra_err_null_ptr ``buf`` was NULL or ``instance`` invalid.
 * @retval k_ra_err_invalid_arg ``block_count`` was 0.
 * @retval k_ra_err_hw_timeout RSPEND or BRE poll exceeded the spin budget.
 *
 * @pre Card has been initialised through CMD0..ACMD41 + CMD2/3/7 by the consumer.
 * @pre ``ra_sdhi_init`` has been called for ``instance``.
 * @post On success ``buf[0..block_count*512]`` holds card data.
 * @post SD_INFO1.RSPEND and SD_INFO2 BRE bits are cleared.
 *
 * @note Blocking, polled implementation; not safe to call from an ISR.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_sdhi_read_block(uint8_t instance, uint32_t lba, uint8_t* buf, uint32_t block_count);

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
 * @retval k_ra_ok Success.
 * @retval k_ra_err_null_ptr ``buf`` was NULL or ``instance`` invalid.
 * @retval k_ra_err_invalid_arg ``block_count`` was 0.
 * @retval k_ra_err_hw_timeout RSPEND or BWE poll exceeded the spin budget.
 *
 * @pre Card has been initialised through CMD0..ACMD41 + CMD2/3/7 by the consumer.
 * @pre Card is not write-protected (caller responsibility).
 * @post On success the requested block range has been pushed into the SDHI FIFO.
 * @post For multi-block writes a CMD12 STOP_TRANSMISSION has been issued.
 *
 * @note Blocking, polled implementation; not safe to call from an ISR.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_sdhi_write_block(uint8_t instance, uint32_t lba, const uint8_t* buf, uint32_t block_count);

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
 * @retval k_ra_ok Success.
 * @retval k_ra_err_null_ptr ``instance`` invalid.
 *
 * @pre ``ra_sdhi_init`` has been called for ``instance``.
 * @post SD_DMAEN reflects ``enable``.
 *
 * @note The DMAC channel itself must be wired up by the caller via
 *       ``ra_dmac`` before any transfer is started.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sdhi_attach_dma(uint8_t instance, uint8_t enable);

#ifdef __cplusplus
}
#endif
