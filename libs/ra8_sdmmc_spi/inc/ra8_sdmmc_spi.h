/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_sdmmc_spi.h
 * @brief SD card driver in SPI-mode (PMOD-attached cards)
 * @ingroup grp_storage
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * SPI-mode SD/SDHC/SDXC card driver. The card sits on an SPI bus
 * (RSPI / SPI_B or any compatible 8-bit full-duplex transport) and
 * answers the standard SD command set wrapped in the SPI-mode framing
 * defined in SD Specification Part 1 Physical Layer Simplified
 * Specification v9.10 section 7 ("SPI Mode").
 *
 * The driver is transport-agnostic. Callers wire it up by passing a
 * ::ra8_sdmmc_spi_transport_t descriptor that holds three function
 * pointers (clock-rate set, CS drive, full-duplex byte exchange).
 * The host-side unit tests inject a mock transport; the firmware
 * application binds the real ``ra8_spi`` HAL driver through a tiny
 * adapter that lives in the example app.
 *
 * Public API:
 *
 *   - ``ra8_sdmmc_spi_init(...)``        -- send CMD0/CMD8/ACMD41/CMD58
 *                                          probe, learn capacity, escalate
 *                                          the SPI clock to data speed.
 *   - ``ra8_sdmmc_spi_read_block(...)``  -- single-block read  (CMD17).
 *   - ``ra8_sdmmc_spi_read_blocks(...)`` -- multi-block read   (CMD18 + CMD12).
 *   - ``ra8_sdmmc_spi_write_block(...)`` -- single-block write (CMD24).
 *   - ``ra8_sdmmc_spi_write_blocks(...)``-- multi-block write  (CMD25).
 *   - ``ra8_sdmmc_spi_get_capacity(...)``-- expose the 512-byte block count.
 *   - ``ra8_sdmmc_spi_get_card_type(...)``-- SDSC / SDHC / SDXC.
 *   - ``ra8_sdmmc_spi_deinit(...)``      -- release the transport.
 *
 * The block I/O API takes a logical block address. Inside the driver
 * the address is converted to a *byte* address for SDSC cards (CMD17/24
 * argument is in bytes for v1.x cards) and left as a *block* address
 * for SDHC / SDXC cards (CMD17/24 argument is in blocks for v2.x cards
 * with the CCS bit set). Callers always speak in 512-byte blocks.
 *
 * The driver also exposes a small ::ra8_fs_backend_t adapter
 * (``ra8_sdmmc_spi_bind_fs_backend``) that plugs the SD card straight
 * into the ``ra8_fs`` FAT filesystem layer without any extra glue in
 * the application.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_port_constants.h"

/* =============================================================================
 * Compile-time limits and protocol constants
 * =============================================================================
 */

/**
 * @enum ra8_sdmmc_spi_limits_t
 * @brief Block-I/O size constants.
 */
typedef enum : uint16_t {
  k_ra8_sdmmc_spi_block_size       = 512U, /**< 512-byte block (SD spec PHY v9 section 7.2.2). */
  k_ra8_sdmmc_spi_cmd_frame_bytes  = 6U,   /**< 1 cmd-byte + 4 arg-bytes + 1 CRC-byte.         */
  k_ra8_sdmmc_spi_csd_response_len = 16U,  /**< CMD9 CSD register payload bytes.               */
} ra8_sdmmc_spi_limits_t;

/**
 * @enum ra8_sdmmc_spi_clock_t
 * @brief Canonical SPI clock rates used by ::ra8_sdmmc_spi_init.
 *
 * @details
 * The SD spec mandates the host opens the bus at 100-400 kHz for the
 * identification sequence (CMD0 ... ACMD41 ... CMD58) and only then
 * escalates to data-transfer speed (default-speed = 25 MHz, high-speed
 * = 50 MHz). We pick 25 MHz as the default-speed target -- it is the
 * universal floor the v1.x and v2.x card families all support.
 */
typedef enum : uint32_t {
  k_ra8_sdmmc_spi_clock_init_hz = 400000U,   /**< Mandatory init clock floor. */
  k_ra8_sdmmc_spi_clock_data_hz = 25000000U, /**< Default-speed data clock.   */
} ra8_sdmmc_spi_clock_t;

/**
 * @enum ra8_sdmmc_spi_card_type_t
 * @brief Detected SD card capacity / addressing class.
 */
typedef enum : uint8_t {
  k_ra8_sdmmc_spi_type_unknown = 0U, /**< Not yet probed / probe failed.      */
  k_ra8_sdmmc_spi_type_sdv1    = 1U, /**< SD v1.x (byte-addressed).           */
  k_ra8_sdmmc_spi_type_sdv2    = 2U, /**< SD v2.x SC (byte-addressed).        */
  k_ra8_sdmmc_spi_type_sdhc    = 3U, /**< SDHC / SDXC v2.x (block-addressed). */
} ra8_sdmmc_spi_card_type_t;

/* =============================================================================
 * Transport descriptor (Dependency Inversion seam)
 * =============================================================================
 */

/**
 * @brief Set the SPI clock frequency.
 *
 * @param[in] ctx     Transport context cookie.
 * @param[in] hz      Target clock rate in Hz.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Clock applied.
 * @retval k_ra8_err_invalid_arg  ``hz`` outside the transport's range.
 *
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_sdmmc_spi_set_clock_fn_t)(void* ctx, uint32_t hz);

/**
 * @brief Drive the chip-select line of the SD card.
 *
 * @param[in] ctx       Transport context cookie.
 * @param[in] asserted  ``true`` to drive CS low (selected), ``false`` to
 *                      release CS high.
 *
 * @return ra8_err_t error code (k_ra8_ok on success).
 *
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_sdmmc_spi_cs_fn_t)(void* ctx, bool asserted);

/**
 * @brief Exchange ``len`` bytes full-duplex on the SPI bus.
 *
 * @param[in]  ctx Transport context cookie.
 * @param[in]  tx  TX buffer; if NULL the transport shall shift out 0xFF
 *                 (idle pattern -- SD spec PHY v9 section 7.2.4).
 * @param[out] rx  RX buffer; if NULL the transport shall discard.
 * @param[in]  len Number of bytes to exchange; must be > 0.
 *
 * @return ra8_err_t error code.
 *
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_sdmmc_spi_xfer_fn_t)(void*          ctx,
                                             const uint8_t* tx,
                                             uint8_t*       rx,
                                             uint32_t       len);

/**
 * @struct ra8_sdmmc_spi_transport_t
 * @brief Driver-to-bus binding (Dependency Inversion seam).
 *
 * @details
 * All three callbacks must be non-NULL. ``ctx`` is passed back into
 * every callback so the caller can carry its own state.
 *
 * @invariant Every function pointer is non-NULL for a usable transport.
 */
typedef struct {
  ra8_sdmmc_spi_set_clock_fn_t set_clock; /**< Set SPI clock frequency.               */
  ra8_sdmmc_spi_cs_fn_t        cs;        /**< Drive chip-select asserted / released. */
  ra8_sdmmc_spi_xfer_fn_t      xfer;      /**< Full-duplex byte exchange.             */
  void*                        ctx;       /**< Caller-owned context cookie.           */
} ra8_sdmmc_spi_transport_t;

/* =============================================================================
 * Convenience SCI Simple-SPI transport factory (EK-RA8D2 Pmod)
 * =============================================================================
 */

/**
 * @struct ra8_sdmmc_spi_sci_pins_t
 * @brief The four bus pins of an SCI Simple-SPI SD slot.
 *
 * @details
 * Names the SCK / CIPO / COPI / CS pins routed to one SCI channel running in
 * Simple-SPI mode. On the EK-RA8D2 these are the Pmod2 (J25) pins exposed as
 * ``k_ra8_board_pmod2_spi_sck`` and friends. The chip-select is driven as a
 * plain GPIO output (SD SPI-mode holds CS asserted across a whole command +
 * data trailer, which the SCI hardware chip-select controller cannot do).
 *
 * @invariant @ref sck, @ref cipo, @ref copi, @ref cs are four distinct pins.
 *
 * @see ra8_sdmmc_spi_transport_sci
 * @since 0.1.0
 */
typedef struct {
  ra8_port_pin_t sck;  /**< SPI clock pin (routed to SCKn).             */
  ra8_port_pin_t cipo; /**< Controller-In / Peripheral-Out pin (CIPOn). */
  ra8_port_pin_t copi; /**< Controller-Out / Peripheral-In pin (COPIn). */
  ra8_port_pin_t cs;   /**< Chip-select pin, driven as a GPIO output.   */
} ra8_sdmmc_spi_sci_pins_t;

/**
 * @brief Build the standard EK-RA8D2 SCI Simple-SPI transport for an SD card.
 *
 * @details
 * One-line bring-up that gives SD-over-SPI the same ergonomics the SDHI host
 * controller already has: instead of every app hand-writing the three
 * ``set_clock`` / ``cs`` / ``xfer`` callbacks plus the pin routing, this
 * factory does all of it and hands back a ready-to-use transport descriptor.
 *
 * Concretely it:
 *
 *   1. Routes @p pins->sck / cipo / copi to the SCI Simple-SPI function via
 *      ``ra8_pfs_route_peripheral`` (PSEL = SCI async) and claims @p pins->cs
 *      as a GPIO output held high (card deselected).
 *   2. Brings the SCI channel up in Simple-SPI controller mode at the SD
 *      power-on clock (``k_ra8_sdmmc_spi_clock_init_hz``) via ``ra8_sci_spi_init``.
 *   3. Caches the bus parameters into module-private storage and wires the
 *      three transport callbacks to the existing ``ra8_sci_spi`` / GPIO HAL.
 *
 * The returned transport is then passed straight to ::ra8_sdmmc_spi_init:
 *
 * @code
 * ra8_sdmmc_spi_transport_t   tr;
 * const ra8_sdmmc_spi_sci_pins_t pins = {
 *   .sck  = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck,
 *   .cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo,
 *   .copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi,
 *   .cs   = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs,
 * };
 * (void)ra8_sdmmc_spi_transport_sci(0U, pclka_hz, &pins, &tr);
 * (void)ra8_sdmmc_spi_init(&tr);
 * @endcode
 *
 * The factory is an opt-in convenience: ::ra8_sdmmc_spi_init still accepts any
 * caller-built ::ra8_sdmmc_spi_transport_t, so an app needing a custom bus
 * (different SCI channel mux, bit-banged transport, mock) skips this helper
 * and populates the descriptor itself.
 *
 * @param[in]  sci_channel SCI channel index (0..9) wired to Simple-SPI mode.
 * @param[in]  pclk_hz     PCLKA rate (Hz) feeding the SCI baud divider; non-zero.
 * @param[in]  pins        Non-NULL SCK / CIPO / COPI / CS pin descriptor.
 * @param[out] out         Non-NULL transport descriptor populated on success.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                 Pins routed, SCI up, @p out populated.
 * @retval k_ra8_err_null_ptr       @p pins or @p out is NULL.
 * @retval k_ra8_err_invalid_arg    @p pclk_hz is 0.
 * @retval other                   Propagated from ``ra8_pfs_route_peripheral`` /
 *                                 ``ra8_gpio_output_init`` / ``ra8_sci_spi_init``.
 *
 * @pre ``ra8_cgc_init`` has run and @p pclk_hz is the live PCLKA rate.
 * @pre @p pins references four pins free for the SCI Simple-SPI mux.
 * @post On success @p out carries non-NULL set_clock / cs / xfer pointers and a
 *       ctx cookie pointing at module-private bus state.
 * @post On success the SCI channel is configured at the SD init clock and CS is
 *       deasserted.
 *
 * @note Single-shot module state backs the transport ctx, so only one SD bus
 *       may be brought up through this factory at a time. Not thread-safe and
 *       not ISR-safe; call once from the single-threaded init path.
 *
 * @see ra8_sdmmc_spi_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdmmc_spi_transport_sci(uint8_t                         sci_channel,
                                                    uint32_t                        pclk_hz,
                                                    const ra8_sdmmc_spi_sci_pins_t* pins,
                                                    ra8_sdmmc_spi_transport_t*      out);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Run the SD SPI-mode identification sequence on a card.
 *
 * @details
 * Steps (SD spec PHY v9 section 7.2.1 "Mode Selection and Initialization"):
 *
 *   1. Set bus clock to 400 kHz, send >= 74 dummy clocks with CS high.
 *   2. Assert CS, send CMD0 (GO_IDLE_STATE), expect R1 = 0x01 (idle).
 *   3. Send CMD8 (SEND_IF_COND, arg = 0x000001AA). R7 echo of the low
 *      12 bits classifies the card as v2.x; "illegal command" (R1.bit2
 *      set) classifies it as v1.x.
 *   4. Loop ACMD41 (CMD55 + CMD41, HCS = 1 for v2.x cards) until R1
 *      reports "ready" (bit 0 of R1 == 0).
 *   5. For v2.x cards, send CMD58 (READ_OCR) and capture the CCS bit;
 *      CCS == 1 means SDHC/SDXC (block-addressed).
 *   6. Send CMD9 (SEND_CSD), parse the CSD-v1 or CSD-v2 fields to
 *      compute the capacity in 512-byte blocks.
 *   7. Send CMD16 (SET_BLOCKLEN, 512) to lock the block size.
 *   8. Bump the bus clock to ``k_ra8_sdmmc_spi_clock_data_hz`` (25 MHz).
 *
 * @param[in] transport Non-NULL transport descriptor with all three
 *                      function pointers populated. The struct is copied
 *                      so the caller may free its storage on return.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Card probed and ready.
 * @retval k_ra8_err_null_ptr          ``transport`` or any callback is NULL.
 * @retval k_ra8_err_invalid_state     Driver already initialized.
 * @retval k_ra8_err_hw_timeout        CMD0 / ACMD41 never produced a response.
 * @retval k_ra8_err_protocol_error    CMD8 echo mismatch, unexpected R1 bits.
 * @retval k_ra8_err_crc_mismatch      CMD response CRC7 mismatch.
 * @retval k_ra8_err_hw_init_failed    ACMD41 init loop exceeded retry budget.
 *
 * @pre ``transport`` is non-NULL.
 * @pre ``transport->set_clock / cs / xfer`` are all non-NULL.
 * @post On success ::ra8_sdmmc_spi_get_capacity returns a non-zero block count.
 * @post On success the SPI bus runs at ``k_ra8_sdmmc_spi_clock_data_hz``.
 *
 * @note Blocking, polled implementation; not safe to call from an ISR.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdmmc_spi_init(const ra8_sdmmc_spi_transport_t* transport);

/**
 * @brief Tear down the driver and release the transport binding.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                  Always (idempotent).
 *
 * @pre None.
 * @post Internal state is the same as before the first ::ra8_sdmmc_spi_init.
 * @post Subsequent ::ra8_sdmmc_spi_init may re-bind a new transport.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdmmc_spi_deinit(void);

/* =============================================================================
 * Block I/O
 * =============================================================================
 */

/**
 * @brief Read one 512-byte block at logical block address ``lba``.
 *
 * @details
 * Issues CMD17 (READ_SINGLE_BLOCK) with the appropriate byte/block
 * address conversion for the detected card type. Waits for the data
 * token (0xFE), drains 512 payload bytes, then drains the trailing
 * 2-byte CRC16 (SD spec PHY v9 section 7.3.3.2 "Start Block Token").
 * The CRC16 is checked.
 *
 * @param[in]  lba LBA in 512-byte units. Must satisfy ``lba < capacity``.
 * @param[out] buf Destination buffer, exactly 512 bytes.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Block read and CRC verified.
 * @retval k_ra8_err_null_ptr          ``buf`` is NULL.
 * @retval k_ra8_err_invalid_state     Driver not initialized.
 * @retval k_ra8_err_out_of_range      ``lba >= capacity``.
 * @retval k_ra8_err_hw_timeout        Data token never arrived.
 * @retval k_ra8_err_crc_mismatch      CRC16 mismatch on the payload.
 * @retval k_ra8_err_protocol_error    Card returned non-zero R1.
 *
 * @pre  ::ra8_sdmmc_spi_init has returned k_ra8_ok.
 * @pre  ``buf`` is non-NULL.
 * @post On success, ``buf[0..511]`` holds the requested block.
 * @post Card remains in ``tran`` state.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdmmc_spi_read_block(uint32_t lba, uint8_t* buf);

/**
 * @brief Read @p count contiguous 512-byte blocks in one CMD18 transaction.
 *
 * @details The fast bulk-read path and the mirror of
 * ::ra8_sdmmc_spi_write_blocks: a single READ_MULTIPLE_BLOCK (CMD18) command
 * streams every block -- data-start token (0xFE), 512 payload bytes, and a
 * verified CRC16 trailer per block -- and is terminated by CMD12
 * (STOP_TRANSMISSION). One command for the whole run instead of @p count
 * CMD17 single-block reads. Falls back to ::ra8_sdmmc_spi_read_block when
 * @p count == 1. On a mid-stream error (missing token, CRC mismatch, bus
 * fault) CMD12 is still issued so the card leaves the data state before CS
 * is released; the stream error takes precedence over any stop error.
 *
 * @param[in]  lba   First LBA in 512-byte units. ``lba + count <= capacity``.
 * @param[out] buf   Destination buffer of exactly ``count * 512`` bytes.
 * @param[in]  count Number of contiguous blocks to read (0 is a no-op).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                 All blocks read and every CRC16 verified
 *                                 (also returned for the ``count == 0`` no-op).
 * @retval k_ra8_err_null_ptr       ``buf`` is NULL.
 * @retval k_ra8_err_invalid_state  Driver not initialized.
 * @retval k_ra8_err_out_of_range   ``lba + count`` exceeds capacity.
 * @retval k_ra8_err_hw_timeout     A data token never arrived.
 * @retval k_ra8_err_crc_mismatch   CRC16 mismatch on a block payload.
 * @retval k_ra8_err_protocol_error CMD18 or CMD12 returned non-zero R1.
 *
 * @pre  ::ra8_sdmmc_spi_init has returned k_ra8_ok.
 * @pre  ``buf`` is writable for at least ``count * 512`` bytes.
 * @post On success ``buf[0 .. (count * 512) - 1]`` holds the requested blocks.
 * @post CMD12 has been sent (count > 1) and the card is back in ``tran``
 *       state; CS is released on every return path.
 *
 * @note Not thread-safe; serialise card access. Blocking, polled
 *       implementation -- not safe to call from an ISR.
 *
 * @see ra8_sdmmc_spi_read_block()   Single-block CMD17 read.
 * @see ra8_sdmmc_spi_write_blocks() The CMD25 bulk-write mirror.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdmmc_spi_read_blocks(uint32_t lba, uint8_t* buf, uint32_t count);

/**
 * @brief Write one 512-byte block at logical block address ``lba``.
 *
 * @details
 * Issues CMD24 (WRITE_BLOCK), sends the data-block token (0xFE), the
 * 512 payload bytes, the 2-byte CRC16, then polls until the card
 * de-asserts the busy-token (CIPO == 0xFF). The data-response token
 * is checked for "data accepted" (xxx00101).
 *
 * @param[in] lba LBA in 512-byte units. Must satisfy ``lba < capacity``.
 * @param[in] buf Source buffer, exactly 512 bytes.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Block written and card became idle.
 * @retval k_ra8_err_null_ptr          ``buf`` is NULL.
 * @retval k_ra8_err_invalid_state     Driver not initialized.
 * @retval k_ra8_err_out_of_range      ``lba >= capacity``.
 * @retval k_ra8_err_hw_timeout        Card never released busy.
 * @retval k_ra8_err_protocol_error    Data-response token != "accepted".
 *
 * @pre  ::ra8_sdmmc_spi_init has returned k_ra8_ok.
 * @pre  ``buf`` is non-NULL.
 * @post On success, the card's storage at LBA ``lba`` matches ``buf``.
 * @post Card remains in ``tran`` state.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdmmc_spi_write_block(uint32_t lba, const uint8_t* buf);

/**
 * @brief Write @p count contiguous 512-byte blocks in one CMD25 transaction.
 *
 * @details The fast bulk-write path: ACMD23 pre-erase hint (best-effort) plus a
 * single WRITE_MULTIPLE_BLOCK streaming every block, terminated by the stop
 * token. One command for the whole run instead of @p count CMD24 single-block
 * writes -- far faster for clearing a large region such as a multi-MB FAT during
 * format. Falls back to ::ra8_sdmmc_spi_write_block when @p count == 1.
 *
 * @param[in] lba   First LBA in 512-byte units. ``lba + count <= capacity``.
 * @param[in] buf   Source buffer of exactly ``count * 512`` bytes.
 * @param[in] count Number of contiguous blocks to write (>= 1).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                 All blocks written and the card became idle.
 * @retval k_ra8_err_null_ptr       ``buf`` is NULL.
 * @retval k_ra8_err_invalid_state  Driver not initialized.
 * @retval k_ra8_err_out_of_range   ``lba + count`` exceeds capacity.
 * @retval k_ra8_err_protocol_error A command R1 or data-response token rejected.
 *
 * @pre  ::ra8_sdmmc_spi_init has returned k_ra8_ok.
 * @pre  ``buf`` holds at least ``count * 512`` bytes.
 * @post On success, the card's storage at ``lba .. lba+count-1`` matches ``buf``.
 * @post Card remains in ``tran`` state.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_sdmmc_spi_write_blocks(uint32_t lba, const uint8_t* buf, uint32_t count);

/**
 * @brief Bulk-erase @p count blocks at @p lba, guaranteeing a zero read-back.
 *
 * @details The CMD32/CMD33/CMD38 erase sequence. The card erases the range
 * internally in one operation -- vastly faster than streaming zeros for a large
 * region (e.g. the ~30 MB FAT of a 128 GB FAT32 card). The post-erase value is
 * card-dependent (``0x00`` on some cards, ``0xFF`` on others), and the SCR
 * ``DATA_STAT_AFTER_ERASE`` bit's polarity is unreliable in practice, so this
 * call **probes**: it erases only the first block, reads it back, and proceeds
 * to erase the rest of the range only when that block is actually all-zero. A
 * non-zero probe yields ::k_ra8_err_not_supported (the card erases to ones),
 * letting the caller fall back to writing zeros without having wasted a
 * full-region erase. A ::k_ra8_ok return is therefore a verified "this region
 * now reads as all-zero bytes".
 *
 * @param[in] lba   First LBA in 512-byte units. ``lba + count <= capacity``.
 * @param[in] count Number of contiguous blocks to erase (>= 1).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                 Range erased AND verified to read back as zero.
 * @retval k_ra8_err_invalid_state  Driver not initialized.
 * @retval k_ra8_err_invalid_arg    ``count`` is 0.
 * @retval k_ra8_err_out_of_range   ``lba + count`` exceeds capacity.
 * @retval k_ra8_err_not_supported  Card erases to a non-zero value (read-back != 0).
 * @retval k_ra8_err_protocol_error A command R1 was rejected.
 * @retval k_ra8_err_hw_timeout     The post-erase busy wait timed out.
 *
 * @pre  ::ra8_sdmmc_spi_init has returned k_ra8_ok.
 * @pre  ``lba + count <= capacity`` (in 512-byte blocks).
 * @post On ::k_ra8_ok, ``lba .. lba+count-1`` read back as ``0x00``.
 * @post Card remains in ``tran`` state.
 *
 * @note Erase granularity is the card's internal AU; the spec guarantees the
 *       addressed range is fully erased without disturbing blocks outside it.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdmmc_spi_erase_blocks(uint32_t lba, uint32_t count);

/* =============================================================================
 * Status queries
 * =============================================================================
 */

/**
 * @brief Return the card capacity in 512-byte blocks.
 *
 * @param[out] out_blocks Receives the block count; non-NULL.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                  Success.
 * @retval k_ra8_err_null_ptr        ``out_blocks`` is NULL.
 * @retval k_ra8_err_invalid_state   Driver not initialized.
 *
 * @pre  ``out_blocks`` is non-NULL.
 * @pre  ::ra8_sdmmc_spi_init has returned k_ra8_ok.
 * @post On success ``*out_blocks > 0``.
 * @post Driver state unchanged.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdmmc_spi_get_capacity(uint32_t* out_blocks);

/**
 * @brief Return the detected card type.
 *
 * @param[out] out_type Receives the card type; non-NULL.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                  Success.
 * @retval k_ra8_err_null_ptr        ``out_type`` is NULL.
 * @retval k_ra8_err_invalid_state   Driver not initialized.
 *
 * @pre  ``out_type`` is non-NULL.
 * @pre  ::ra8_sdmmc_spi_init has returned k_ra8_ok.
 * @post Driver state unchanged.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdmmc_spi_get_card_type(ra8_sdmmc_spi_card_type_t* out_type);

/* =============================================================================
 * ra8_fs backend adapter
 * =============================================================================
 */

/**
 * @brief Populate an ::ra8_fs_backend_t that mounts onto this SD driver.
 *
 * @details
 * Once ::ra8_sdmmc_spi_init has returned ``k_ra8_ok``, the caller can
 * invoke this helper to bind the SD card into the ``ra8_fs`` FAT layer:
 *
 * @code
 * ra8_fs_backend_t backend;
 * (void)ra8_sdmmc_spi_bind_fs_backend(&backend);
 * ra8_fs_mount_t* mount = nullptr;
 * (void)ra8_fs_mount(&backend, &mount);
 * @endcode
 *
 * The backend forwards ``read_block`` / ``write_block`` to the SD
 * driver one block at a time and reports the capacity in 512-byte
 * blocks. The ``ctx`` field is left NULL because the SD driver has a
 * single global instance.
 *
 * @param[out] out_backend Populated backend descriptor; non-NULL.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                  Backend populated.
 * @retval k_ra8_err_null_ptr        ``out_backend`` is NULL.
 * @retval k_ra8_err_invalid_state   Driver not initialized.
 *
 * @pre  ``out_backend`` is non-NULL.
 * @pre  ::ra8_sdmmc_spi_init has returned k_ra8_ok.
 * @post ``*out_backend`` carries non-NULL ``read_block`` /
 *       ``write_block`` / ``get_capacity`` pointers.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sdmmc_spi_bind_fs_backend(ra8_fs_backend_t* out_backend);

/* =============================================================================
 * Test-only helpers (CRC primitives)
 * =============================================================================
 */

/**
 * @brief Compute the SD-spec CRC7 over an arbitrary byte run.
 *
 * @details
 * Polynomial 0x09 (x^7 + x^3 + 1), left-shifted, as defined in SD spec
 * PHY v9 section 4.5 "CRC7". Used internally to seed every CMD frame's
 * trailing CRC byte (the bottom bit of the CRC byte is the "end bit",
 * always 1, so the wire value is ``(crc7 << 1) | 1U``).
 *
 * @param[in] data Buffer to hash; non-NULL when ``len > 0``.
 * @param[in] len  Byte count; may be zero.
 *
 * @return uint8_t CRC7 in the low 7 bits.
 * @retval 0..0x7F Computed CRC7. Returns 0 on NULL input.
 *
 * @pre  ``data`` is non-NULL or ``len == 0``.
 * @pre  Caller treats the return value as a 7-bit CRC, not 8.
 * @post No side effects.
 * @post Driver state is unchanged.
 *
 * @note Thread-safe; pure function with no global state.
 * @since 0.1.0
 */
uint8_t ra8_sdmmc_spi_crc7(const uint8_t* data, uint32_t len);

/**
 * @brief Compute the SD-spec CRC16-CCITT over an arbitrary byte run.
 *
 * @details
 * Polynomial 0x1021 (x^16 + x^12 + x^5 + 1), seed 0x0000, as defined
 * in SD spec PHY v9 section 4.5 "CRC16". The wire format is big-endian.
 *
 * @param[in] data Buffer to hash; non-NULL when ``len > 0``.
 * @param[in] len  Byte count; may be zero.
 *
 * @return uint16_t CRC16-CCITT over ``data[0..len-1]``.
 * @retval 0..0xFFFF Computed CRC16. Returns 0 on NULL input.
 *
 * @pre  ``data`` is non-NULL or ``len == 0``.
 * @pre  Caller serializes the result big-endian on the wire.
 * @post No side effects.
 * @post Driver state is unchanged.
 *
 * @note Thread-safe; pure function with no global state.
 * @since 0.1.0
 */
uint16_t ra8_sdmmc_spi_crc16(const uint8_t* data, uint32_t len);

#ifdef __cplusplus
}
#endif
