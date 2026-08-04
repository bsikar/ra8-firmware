/**
 * @file ra8_sdcard.h
 * @brief SD card driver layered on top of the RA8D2 SDHI peripheral
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * High-level SD/SDHC/SDXC card driver. Sits on top of the lower-level
 * `ra8_sdhi` register-driver primitive and implements the standard SD
 * Physical Layer initialization sequence:
 *
 *   1. CMD0  GO_IDLE_STATE          (reset card to idle)
 *   2. CMD8  SEND_IF_COND           (interface condition + V2 probe)
 *   3. ACMD41 SD_SEND_OP_COND       (operating-condition negotiation,
 *                                    HCS=1 -> SDHC/SDXC support)
 *   4. CMD2  ALL_SEND_CID           (capture CID)
 *   5. CMD3  SEND_RELATIVE_ADDR     (host obtains card RCA)
 *   6. CMD9  SEND_CSD               (capture CSD -> capacity, class)
 *   7. CMD7  SELECT_CARD            (place card in transfer state)
 *
 * After CMD7 the bus clock is bumped from the 400 kHz identification
 * speed to 25 MHz default-speed; SDR50 / DDR50 is left as a future
 * extension.
 *
 * Block I/O is delegated straight to ::ra8_sdhi_read_block /
 * ::ra8_sdhi_write_block, which already implement the polled
 * SD_BUF0 FIFO drain. ``ra8_sdcard`` only owns the protocol state
 * (capacity in 512-byte blocks, RCA, OCR card-type bits, init flag).
 *
 * Citations against the RA8D2 Hardware User's Manual reference Ch 47
 * "SD/MMC Host Interface (SDHI)" (HUM pages 3122-3179). SD command
 * indices and response formats are from the SD Physical Layer
 * Specification v6.00 (open-access) -- the manual cites that spec
 * directly in 47.1 "SDHI Overview".
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
#include "ra8_sdhi.h"

/**
 * @enum ra8_sdcard_card_type_t
 * @brief Detected SD card capacity / addressing class.
 */
typedef enum : uint8_t {
  k_ra8_sdcard_type_unknown = 0U, /**< Not yet probed or no card present.           */
  k_ra8_sdcard_type_sdsc    = 1U, /**< Standard-capacity, byte-addressed (<= 2 GB). */
  k_ra8_sdcard_type_sdhc    = 2U, /**< High-capacity, block-addressed (2-32 GB).    */
  k_ra8_sdcard_type_sdxc    = 3U, /**< Extended-capacity, block-addressed (>32 GB). */
} ra8_sdcard_card_type_t;

/**
 * @struct ra8_sdcard_cfg_t
 * @brief User-supplied configuration for ::ra8_sdcard_init.
 *
 * @details
 * The driver owns its own internal state instance -- one card per host
 * instance, which matches the RA8D2 SDHI silicon.
 *
 * ``bus_width`` selects the data-bus width the init sequence tries to
 * reach after the card is parked in TRAN state:
 *
 * - ``0`` (zero-initialized default) or ::k_ra8_sdhi_bus_width_1bit --
 *   stay 1-bit; no bus-width negotiation is attempted.
 * - ::k_ra8_sdhi_bus_width_4bit -- negotiate the SD 4-bit bus with
 *   CMD55 + ACMD6 (::ra8_sdhi_set_bus_width_4bit). Best-effort: a card
 *   that declines is left at 1-bit and init still succeeds.
 *
 * The 8-bit width is an eMMC-only capability and is NOT an SD-card
 * option, so it is not accepted here; drive an eMMC device's 8-bit
 * bus directly through ::ra8_sdhi_set_bus_width_8bit.
 *
 * @invariant Only 0 / 1-bit / 4-bit are honored; any other value is
 *            treated as "stay 1-bit".
 */
typedef struct {
  uint8_t              instance;  /**< SDHI instance index (0 or 1).              */
  ra8_sdhi_bus_width_t bus_width; /**< Target bus width (0/1-bit default, 4-bit). */
} ra8_sdcard_cfg_t;

/**
 * @brief Bring up the SDHI block and perform full SD card initialization.
 *
 * @details
 * Runs the standard SD Physical Layer init sequence:
 *
 *   1. ::ra8_sdhi_init  -- MSTP gate + soft-reset + 400 kHz clock
 *   2. CMD0   GO_IDLE_STATE
 *   3. CMD8   SEND_IF_COND   (pattern 0x1AA, 2.7-3.6 V)
 *   4. ACMD41 with HCS=1 looped until OCR busy bit clears
 *   5. CMD2   ALL_SEND_CID
 *   6. CMD3   SEND_RELATIVE_ADDR -- captures the card-assigned RCA
 *   7. CMD9   SEND_CSD       -- decodes capacity + speed class
 *   8. CMD7   SELECT_CARD    -- transitions card to TRAN state
 *   9. optional ACMD6 4-bit bus-width negotiation when
 *      ``cfg->bus_width`` requests it (best-effort; see ::ra8_sdcard_cfg_t)
 *  10. ::ra8_sdhi_set_clock to 25 MHz default-speed
 *
 * On success the internal state holds card type, RCA, and capacity in
 * 512-byte blocks, and the SDHI block is ready for ``read_blocks`` /
 * ``write_blocks`` traffic.
 *
 * @param[in] cfg Non-NULL configuration with a valid SDHI instance.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Card initialized, ready for I/O.
 * @retval k_ra8_err_null_ptr        ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg     ``cfg->instance`` out of range.
 * @retval k_ra8_err_invalid_state   Already initialized (call ::ra8_sdcard_deinit first).
 * @retval k_ra8_err_hw_timeout      A command never produced RSPEND.
 * @retval k_ra8_err_hw_init_failed  CMD8 echo / ACMD41 pattern mismatch.
 *
 * @pre  ``cfg`` is non-NULL and points to a fully populated config struct.
 * @pre  No other consumer is currently using the chosen SDHI instance.
 * @post On success ::ra8_sdcard_get_capacity returns a non-zero block count.
 * @post On success the SDHI bus clock is at default speed (25 MHz).
 *
 * @note Blocking, polled implementation; not safe to call from an ISR.
 *
 * @par Example:
 * @code
 * ra8_sdcard_cfg_t cfg = { .instance = 0U, .bus_width = k_ra8_sdhi_bus_width_4bit };
 * ra8_err_t        err = ra8_sdcard_init(&cfg);
 * @endcode
 *
 * @see ra8_sdcard_read_blocks
 * @see ra8_sdcard_get_capacity
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdcard_init(const ra8_sdcard_cfg_t* cfg);

/**
 * @brief Read ``count`` 512-byte blocks starting at ``lba``.
 *
 * @details
 * Thin pass-through to ::ra8_sdhi_read_block once the card has been
 * initialized. The ``lba`` argument is the logical block address as
 * understood by the card -- which means *block* address for SDHC/SDXC
 * and *byte* address for SDSC. ``ra8_sdcard`` translates the SDSC case
 * internally (lba * 512) so callers always pass block indices.
 *
 * @param[in]  lba   Logical block address (sector number).
 * @param[out] buf   Destination buffer; must hold ``count * 512`` bytes.
 * @param[in]  count Number of 512-byte blocks; must be > 0.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Success.
 * @retval k_ra8_err_null_ptr      ``buf`` was NULL.
 * @retval k_ra8_err_invalid_arg   ``count`` was 0.
 * @retval k_ra8_err_invalid_state Card never initialized.
 * @retval k_ra8_err_out_of_range  ``lba + count`` exceeds card capacity.
 *
 * @pre  ::ra8_sdcard_init has returned k_ra8_ok.
 * @pre  ``buf`` is non-NULL.
 * @post Buffer holds card data on success.
 * @post Card remains in TRAN state.
 *
 * @note Blocking, polled implementation; not safe to call from an ISR.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdcard_read_blocks(uint32_t lba, uint8_t* buf, uint32_t count);

/**
 * @brief Write ``count`` 512-byte blocks starting at ``lba``.
 *
 * @details
 * Thin pass-through to ::ra8_sdhi_write_block. Same SDSC byte-address
 * translation as ::ra8_sdcard_read_blocks. The driver does not perform
 * pre-erase (ACMD23) or busy-poll the card after writes; the caller
 * is expected to layer those on top if required.
 *
 * @param[in] lba   Logical block address.
 * @param[in] buf   Source buffer; must hold ``count * 512`` bytes.
 * @param[in] count Number of 512-byte blocks; must be > 0.
 *
 * @return ra8_err_t Error code (same as ::ra8_sdcard_read_blocks).
 *
 * @pre  ::ra8_sdcard_init has returned k_ra8_ok.
 * @pre  ``buf`` is non-NULL.
 * @post On success the requested block range was pushed to the card.
 * @post Card remains in TRAN state.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdcard_write_blocks(uint32_t lba, const uint8_t* buf, uint32_t count);

/**
 * @brief Return card capacity in 512-byte blocks.
 *
 * @param[out] out_blocks Receives the block count; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Success.
 * @retval k_ra8_err_null_ptr      ``out_blocks`` was NULL.
 * @retval k_ra8_err_invalid_state Card never initialized.
 *
 * @pre  ``out_blocks`` is non-NULL.
 * @pre  ::ra8_sdcard_init has returned k_ra8_ok.
 * @post On success ``*out_blocks > 0``.
 * @post Driver state unchanged.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdcard_get_capacity(uint32_t* out_blocks);

/**
 * @brief Return the detected card type (SDSC / SDHC / SDXC).
 *
 * @param[out] out_type Receives the card type; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Success.
 * @retval k_ra8_err_null_ptr      ``out_type`` was NULL.
 * @retval k_ra8_err_invalid_state Card never initialized.
 *
 * @pre  ``out_type`` is non-NULL.
 * @pre  ::ra8_sdcard_init has returned k_ra8_ok.
 * @post Driver state unchanged.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdcard_get_type(ra8_sdcard_card_type_t* out_type);

/**
 * @brief Tear down the card driver and release the SDHI block.
 *
 * @details
 * Resets the internal protocol state (capacity, RCA, type) and calls
 * ::ra8_sdhi_deinit on the underlying instance. After this call
 * ::ra8_sdcard_init may be invoked again.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Success (or already-deinit no-op).
 * @retval k_ra8_err_invalid_state SDHI deinit failed.
 *
 * @pre  None.
 * @post Internal state is the same as before the first ::ra8_sdcard_init.
 * @post SDHI module is gated off (MSTP).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdcard_deinit(void);

#ifdef __cplusplus
}
#endif
