/**
 * @file ra_mipi_csi.h
 * @brief MIPI CSI-2 receiver HAL driver -- public API
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full HUM Ch 66 ("MIPI CSI Interface" p 3935-3972) HAL surface for
 * the RA8D2 MIPI CSI-2 receiver block. Every documented register
 * and every documented IRQ source is exposed.
 *
 * ## Functional groups
 *
 *  - **Lifecycle**          -- init / deinit / reset.
 *  - **Reception control**  -- start / stop receive (MCT3.RXEN).
 *  - **Data-type filter**   -- DTEL / DTEH (per-DT enable).
 *  - **ECC / scrambling**   -- MCT0.ECCV13, MCT0.LFSREN.
 *  - **Frame-error mode**   -- MCT0.ZLMD / EDMD / RVMD.
 *  - **EPD tuning**         -- EPCT (long/short spacers + Option-2).
 *  - **LRTE tuning**        -- EMCT.VLSIEN / EOTPEN.
 *  - **Module status**      -- MCG (RO IP info), MIST (IRQ summary).
 *  - **Receive status**     -- RXST / RXSC / RXIE.
 *  - **Per-data-lane IRQ**  -- DLST(N) / DLSC(N) / DLIE(N).
 *  - **Per-VC IRQ**         -- VCST(M) / VCSC(M) / VCIE(M).
 *  - **Power management**   -- PMST / PMSC / PMIE.
 *  - **Short-packet FIFO**  -- GSCT / GSST / GSSC / GSIE / GSHT / GSIU.
 *  - **ISR dispatch**       -- one dispatcher per HUM IRQ source
 *                              (RX, DL, VC, PM, GST).
 *  - **Power transition**   -- enter_stop / exit_stop (MSTP gate).
 *
 * The driver pairs with the parallel ``ra_mipi_phy`` driver (HUM
 * Ch 64) which configures the D-PHY itself. ``ra_mipi_phy`` is
 * owned by a different agent and is NOT invoked from here -- the
 * caller is expected to bring the PHY up before calling
 * ``ra_mipi_csi_init`` and tear it down after ``ra_mipi_csi_deinit``.
 *
 * @par State Machine
 * @startuml
 * [*] --> Gated : reset
 * Gated  --> Idle    : init()    [MSTP on, RXEN=0]
 * Idle   --> Active  : start_receive() [RXEN=1]
 * Active --> Idle    : stop_receive()  [RXEN=0, VSRST]
 * Idle   --> Gated   : deinit()
 * Idle   --> Stopped : enter_stop()
 * Stopped --> Idle   : exit_stop()
 * @enduml
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_mipi_csi_regs.h"
#include "ra_err.h"

/**
 * @enum ra_mipi_csi_lanes_t
 * @brief Number of D-PHY data lanes the receiver should bring up.
 *
 * @details
 * Maps directly onto MCT0.VDLN[3:0]. The HUM (Ch 66.3.2 p 3936)
 * permits values 0x1 (one lane) and 0x2 (two lanes) only; any other
 * value is "Setting prohibited".
 */
typedef enum : uint8_t {
  k_ra_mipi_csi_lanes_1 = 1U, /**< Single-lane D-PHY operation.  */
  k_ra_mipi_csi_lanes_2 = 2U, /**< Dual-lane D-PHY operation.    */
} ra_mipi_csi_lanes_t;

/**
 * @struct ra_mipi_csi_config_t
 * @brief Configuration descriptor for ``ra_mipi_csi_init``.
 *
 * @details
 * Fields cover everything programmable in MCT0 / MCT2 / EMCT / EPCT
 * + the receive data-type filter masks. Per-VC, per-DL, PM, and
 * short-packet IRQ enables can be set via ``ra_mipi_csi_init`` (the
 * matching arrays below) or applied later through the per-group
 * setters.  ``frrclk`` and ``frrskw`` are vclk/hsclk-derived rate-
 * adjust values described in HUM Ch 66.3.3 "MCT2 : Module Control
 * Register 2" p 3937; the caller is expected to compute them.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  /* MCT0 */
  ra_mipi_csi_lanes_t lanes;        /**< 1 or 2 lanes (MCT0.VDLN).        */
  bool                generic_rule; /**< MCT0.GRMD: must be 1 here.       */
  bool                eccv13;       /**< MCT0.ECCV13 -- ECC v1.3 mode.    */
  bool                lfsren;       /**< MCT0.LFSREN -- descrambling on.  */
  bool                zlmd;         /**< MCT0.ZLMD -- zero-length LP.     */
  bool                edmd;         /**< MCT0.EDMD -- ErrFrameData notify.*/
  bool                rvmd;         /**< MCT0.RVMD -- reserved-DT receive.*/

  /* MCT2 */
  uint16_t frrclk; /**< MCT2.FRRCLK[8:0] rate.                            */
  uint16_t frrskw; /**< MCT2.FRRSKW[8:0] skew.                            */

  /* EPCT (EPD option control). All zero = EPD off.                       */
  bool     epd_enable;       /**< EPCT.EPDEN -- enable EPD.                 */
  bool     epd_option_2;     /**< EPCT.EPDOP -- 1 = Option 2 (recommended). */
  uint16_t epd_long_spacer;  /**< EPCT.SLP[14:0].                         */
  uint16_t epd_short_spacer; /**< EPCT.SSP[14:0].                         */

  /* EMCT (EPD misc options).                                             */
  ra_mipi_csi_vlsien_t vlsien;      /**< EMCT.VLSIEN[5:4] LRTE spacer.   */
  bool                 eotp_enable; /**< EMCT.EOTPEN bit 6.              */

  /* DTEL / DTEH (data-type filter). 0 = accept reset defaults.           */
  uint32_t dt_low_mask;  /**< DTEL value to programme.                    */
  uint32_t dt_high_mask; /**< DTEH value to programme.                    */

  /* RXIE / DLIEx / VCIE[16] / PMIE / GSIE -- masks for IRQ enable.       */
  uint32_t rx_irq_mask;     /**< RXIE value.                            */
  uint32_t dl_irq_mask[2];  /**< DLIE0 / DLIE1 values.                  */
  uint32_t vc_irq_mask[16]; /**< VCIE0..VCIE15 values.                  */
  uint32_t pm_irq_mask;     /**< PMIE value.                            */
  uint32_t short_irq_mask;  /**< GSIE value.                            */

  /* GSCT (short-packet FIFO control).                                    */
  uint8_t short_threshold;    /**< GSCT.SHTH (threshold-1 in stages).   */
  bool    short_store_enable; /**< GSCT.GFIF -- 1 = store in FIFO.      */
} ra_mipi_csi_config_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_mipi_csi_event_fn_t
 * @brief MIPI CSI receive-status event callback.
 * @param[in] ctx       Caller context passed to ``attach_handler``.
 * @param[in] rxst_mask The RXST register value at dispatch time.
 */
typedef void (*ra_mipi_csi_event_fn_t)(void* ctx, uint32_t rxst_mask);

/**
 * @typedef ra_mipi_csi_dl_event_fn_t
 * @brief Data-lane IRQ callback.
 * @param[in] ctx     Caller context.
 * @param[in] lane    Data lane index (0..1) that fired.
 * @param[in] dlst    DLST(N) snapshot delivered to the callback.
 */
typedef void (*ra_mipi_csi_dl_event_fn_t)(void* ctx, uint8_t lane, uint32_t dlst);

/**
 * @typedef ra_mipi_csi_vc_event_fn_t
 * @brief Per-virtual-channel IRQ callback.
 * @param[in] ctx        Caller context.
 * @param[in] vc         Virtual channel index (0..15) or 0xFF if generic.
 * @param[in] vcst       VCST(M) snapshot for the channel; for generic
 *                       events (vc == 0xFF) only MLF + ECD bits are set.
 */
typedef void (*ra_mipi_csi_vc_event_fn_t)(void* ctx, uint8_t vc, uint32_t vcst);

/**
 * @typedef ra_mipi_csi_pm_event_fn_t
 * @brief Power-management IRQ callback.
 * @param[in] ctx      Caller context.
 * @param[in] pmst     PMST snapshot delivered to the callback.
 */
typedef void (*ra_mipi_csi_pm_event_fn_t)(void* ctx, uint32_t pmst);

/**
 * @typedef ra_mipi_csi_short_event_fn_t
 * @brief Generic-short-packet FIFO IRQ callback.
 * @param[in] ctx      Caller context.
 * @param[in] gsst     GSST snapshot delivered to the callback.
 */
typedef void (*ra_mipi_csi_short_event_fn_t)(void* ctx, uint32_t gsst);

/**
 * @struct ra_mipi_csi_short_packet_t
 * @brief One drained generic short-packet header.
 *
 * @details Decoded fields from a single GSHT read. Matches the
 *  format described in HUM Ch 66.3.28 p 3959.
 */
typedef struct {
  uint16_t payload;   /**< GSHT.SPDT[15:0]  -- 16-bit user data.        */
  uint8_t  data_type; /**< GSHT.DTYP[21:16] -- short-packet DT (0x08-0x0F). */
  uint8_t  vc;        /**< GSHT.SPVC[27:24] -- virtual channel.         */
  uint32_t raw;       /**< Unprocessed register snapshot.               */
} ra_mipi_csi_short_packet_t;

/**
 * @struct ra_mipi_csi_module_info_t
 * @brief Decoded MCG (Module Configuration) RO snapshot.
 */
typedef struct {
  uint8_t  version;     /**< MCG.VER[3:0]    -- IP-core version.          */
  uint8_t  lanes_max;   /**< MCG.SDLN[11:8]  -- max supported lanes.      */
  uint8_t  fifo_stages; /**< MCG.GSNM[23:16] -- short-packet FIFO depth.  */
  uint32_t raw;         /**< Unprocessed register snapshot.               */
} ra_mipi_csi_module_info_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the MIPI CSI receiver.
 *
 * @details
 * Sequence:
 *
 *  1. Validate ``cfg`` and the lane count.
 *  2. Ungate the peripheral via ``ra_mstp_enable(k_ra_mstp_mipi_csi)``.
 *  3. Clear MCT3.RXEN (no reception during configuration).
 *  4. Spin until RTST.VSRSTS reads 0 (no reset in progress).
 *  5. Programme MCT0 (lane count + GRMD + ECCV13 + LFSREN +
 *     ZLMD + EDMD + RVMD).
 *  6. Programme MCT2 (FRRCLK + FRRSKW).
 *  7. Programme EPCT (EPD enable, option, spacers).
 *  8. Programme EMCT (VLSIEN + EOTPEN).
 *  9. Programme DTEL / DTEH (data-type filter).
 * 10. Programme GSCT (short-packet threshold + store flag).
 * 11. Programme RXIE / DLIE0 / DLIE1 / VCIE0..15 / PMIE / GSIE.
 * 12. Leave RXEN clear -- caller arms via ``start_receive``.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Receiver configured, RXEN still 0.
 * @retval k_ra_err_null_ptr      ``cfg`` was nullptr.
 * @retval k_ra_err_invalid_arg   Lane count not in ``{1, 2}`` or VLSIEN
 *                                out of range.
 * @retval k_ra_err_hw_init_failed MSTP enable failed.
 * @retval k_ra_err_hw_timeout    VSRSTS did not clear within budget.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 *
 * @post On success, the peripheral is clocked and configured.
 * @post MCT3.RXEN is 0 (no incoming data accepted).
 *
 * @note Thread safety: not thread-safe.
 * @see ra_mipi_csi_start_receive
 * @see ra_mipi_csi_deinit
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_init(const ra_mipi_csi_config_t* cfg);

/**
 * @brief Tear down the MIPI CSI receiver.
 *
 * @details
 * Clears MCT3.RXEN, pulses VSRST to flush any in-flight state,
 * clears every IRQ-enable register, drops cached callbacks, and
 * gates the MSTP bit. Safe to call after a partial init.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Receiver gated cleanly.
 * @retval k_ra_err_invalid_state ra_mstp ref-count was already 0.
 *
 * @pre IRQs masked or single-threaded teardown context.
 * @pre Caller has stopped any upstream camera transmission.
 * @post MCT3.RXEN is 0.
 * @post MSTP bit for MIPI CSI is set (peripheral gated).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_deinit(void);

/**
 * @brief Issue a software reset (RTCT.VSRST) and wait for completion.
 *
 * @details
 * Pulses RTCT.VSRST then spins on RTST.VSRSTS until it clears. Used
 * after stop_receive to drain the video-pixel pipeline before the
 * next start.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok             Reset completed.
 * @retval k_ra_err_hw_timeout VSRSTS did not clear in time.
 *
 * @pre Peripheral is ungated.
 * @pre Caller is in single-threaded context.
 * @post RTST.VSRSTS reads 0.
 * @post Video-pixel pipeline is empty.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_reset(void);

/* =============================================================================
 * Reception control
 * =============================================================================
 */

/**
 * @brief Enable MIPI CSI reception (set MCT3.RXEN).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                MCT3.RXEN set.
 * @retval k_ra_err_invalid_state RXEN was already set.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Upstream MIPI PHY is up and clocking.
 * @post MCT3.RXEN reads 1.
 * @post Subsequent CSI-2 packets land in RXST / VCSTn / GSHT.
 *
 * @note Thread safety: not thread-safe.
 * @see ra_mipi_csi_stop_receive
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_start_receive(void);

/**
 * @brief Disable MIPI CSI reception (clear MCT3.RXEN, pulse VSRST).
 *
 * @details
 * The HUM (and FSP) require the application to wait for a full
 * frame after stopping the upstream camera before calling this --
 * we do not poll for that condition because the driver has no way
 * to know the frame interval. Caller is responsible.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Upstream camera has stopped sending data.
 * @post MCT3.RXEN reads 0.
 * @post RTCT.VSRST has been written 1 (reset pulse issued).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_stop_receive(void);

/* =============================================================================
 * Module-level status
 * =============================================================================
 */

/**
 * @brief Read the RXST receive-status register.
 *
 * @param[out] out_mask On success, the live RXST value (RACT,
 *                      RACTDET, FRMn[15:0]).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok           Status returned in ``*out_mask``.
 * @retval k_ra_err_null_ptr ``out_mask`` was nullptr.
 *
 * @pre ``out_mask`` is non-NULL.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @post ``*out_mask`` holds the RXST value.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_get_status(uint32_t* out_mask);

/**
 * @brief Clear the sticky receive-status bits selected by ``mask``.
 *
 * @details
 * Only RACTDET (bit 17) is currently writable in RXSC; other bits
 * in ``mask`` are silently dropped by hardware.
 *
 * @param[in] mask Bit mask to write to RXSC.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller knows which sticky bits are write-1-to-clear.
 * @post RXSC has been written.
 * @post Hardware will eventually deassert the targeted RXST bits.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_clear_status(uint32_t mask);

/**
 * @brief Programme RXIE (receive interrupt enable mask).
 * @param[in] mask  Bit mask to programme into RXIE.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller is in single-threaded init context.
 * @post RXIE = mask.
 * @post Subsequent matching events trigger the receive IRQ vector.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_set_rx_irq_enable(uint32_t mask);

/**
 * @brief Read MIST (Module Interrupt Status, RO summary).
 * @param[out] out_mask On success, the live MIST value.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok           Status returned.
 * @retval k_ra_err_null_ptr out_mask was nullptr.
 * @pre out_mask is non-NULL.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @post *out_mask holds MIST.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_get_module_irq_status(uint32_t* out_mask);

/**
 * @brief Read MCG (Module Configuration, RO IP info) and decode.
 *
 * @param[out] out On success, the decoded MCG snapshot.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok           Info returned in ``*out``.
 * @retval k_ra_err_null_ptr ``out`` was nullptr.
 *
 * @pre out is non-NULL.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @post *out holds the decoded MCG snapshot.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_get_module_info(ra_mipi_csi_module_info_t* out);

/* =============================================================================
 * Data-type filter
 * =============================================================================
 */

/**
 * @brief Programme the receive data-type filter (DTEL + DTEH).
 *
 * @details
 * A bit set in ``low_mask`` enables the corresponding data type in
 * the 0x00-0x1F range; a bit set in ``high_mask`` enables one in
 * 0x20-0x3F. Bits documented as reserved (DTEL[3:0] read 1, [7:5]
 * read 0, [29:16] read 0; DTEH bits other than 4 and 10) are
 * preserved by the hardware.
 *
 * @param[in] low_mask  Mask written to DTEL.
 * @param[in] high_mask Mask written to DTEH.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller is in single-threaded context.
 * @post DTEL = low_mask, DTEH = high_mask.
 * @post Receiver accepts only the selected data types.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_set_data_type_filter(uint32_t low_mask, uint32_t high_mask);

/* =============================================================================
 * ECC / scrambling / frame-error knobs
 * =============================================================================
 */

/**
 * @brief Update MCT0 ECC bits (ECCV13, LFSREN) atomically.
 *
 * @param[in] eccv13 1 = ECC v1.3 (24-bit) mode, 0 = legacy ECC.
 * @param[in] lfsren 1 = enable LFSR descrambling.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Bits updated.
 * @retval k_ra_err_invalid_state RXEN was set (can only update while gated).
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Reception is stopped (MCT3.RXEN = 0).
 * @post MCT0.ECCV13 and MCT0.LFSREN reflect the requested state.
 * @post Other MCT0 bits are preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_set_ecc_mode(bool eccv13, bool lfsren);

/**
 * @brief Update MCT0 frame-error notification bits atomically.
 *
 * @param[in] zlmd 1 = treat zero-length LP as ErrFrameSync.
 * @param[in] edmd 1 = forward ErrFrameData errors to ISR.
 * @param[in] rvmd 1 = receive Reserved-DT packets normally.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Bits updated.
 * @retval k_ra_err_invalid_state RXEN was set.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Reception is stopped (MCT3.RXEN = 0).
 * @post MCT0.ZLMD / EDMD / RVMD reflect the requested state.
 * @post Other MCT0 bits are preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_set_frame_error_mode(bool zlmd, bool edmd, bool rvmd);

/* =============================================================================
 * EPD / LRTE tuning
 * =============================================================================
 */

/**
 * @brief Programme EPCT (long-packet spacer + Option-2 EPD).
 *
 * @param[in] enable      1 = EPCT.EPDEN -- enable EPD operation.
 * @param[in] option_2    1 = EPCT.EPDOP -- select Option-2 EPD.
 * @param[in] long_spacer EPCT.SLP[14:0] -- long-packet spacer count.
 * @param[in] short_spacer EPCT.SSP[14:0] -- short-packet spacer count.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Updated.
 * @retval k_ra_err_invalid_arg   spacer values exceed 15 bits.
 * @retval k_ra_err_invalid_state RXEN was set.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Reception is stopped (MCT3.RXEN = 0).
 * @post EPCT contains the requested fields.
 * @post Subsequent transmissions honour the new spacer values.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_mipi_csi_set_epd(bool enable, bool option_2, uint16_t long_spacer, uint16_t short_spacer);

/**
 * @brief Programme EMCT (LRTE / EOTP options).
 *
 * @param[in] vlsien Variable-length spacer mode (EMCT.VLSIEN[5:4]).
 * @param[in] eotp_enable 1 = EMCT.EOTPEN -- expect EoT packet.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Updated.
 * @retval k_ra_err_invalid_arg   vlsien out of enum range.
 * @retval k_ra_err_invalid_state RXEN was set.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Reception is stopped (MCT3.RXEN = 0).
 * @post EMCT.VLSIEN and EMCT.EOTPEN reflect the requested state.
 * @post Other EMCT bits are preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_set_lrte(ra_mipi_csi_vlsien_t vlsien, bool eotp_enable);

/* =============================================================================
 * Per-data-lane status / IRQ
 * =============================================================================
 */

/**
 * @brief Read DLST(N) (Data Lane N Status).
 * @param[in]  lane     Data lane index (0..1).
 * @param[out] out_mask Live DLST(N) value.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok            Status returned.
 * @retval k_ra_err_invalid_arg lane > 1.
 * @retval k_ra_err_null_ptr  out_mask was nullptr.
 * @pre lane <= 1.
 * @pre out_mask is non-NULL.
 * @post *out_mask holds the DLST(N) value.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_dl_get_status(uint8_t lane, uint32_t* out_mask);

/**
 * @brief W1C-clear bits in DLST(N) via DLSC(N).
 * @param[in] lane Data lane index (0..1).
 * @param[in] mask Bits to clear.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok            Cleared.
 * @retval k_ra_err_invalid_arg lane > 1.
 * @pre lane <= 1.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @post DLSC(N) has been written.
 * @post Targeted DLST(N) bits will eventually deassert.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_dl_clear_status(uint8_t lane, uint32_t mask);

/**
 * @brief Programme DLIE(N) (Data Lane N IRQ enable).
 * @param[in] lane Data lane index (0..1).
 * @param[in] mask Bits to set in DLIE(N).
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok            Updated.
 * @retval k_ra_err_invalid_arg lane > 1.
 * @pre lane <= 1.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @post DLIE(N) = mask.
 * @post Subsequent matching DL events trigger the DL IRQ vector.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_dl_set_irq_enable(uint8_t lane, uint32_t mask);

/* =============================================================================
 * Per-virtual-channel status / IRQ
 * =============================================================================
 */

/**
 * @brief Read VCST(M) (Virtual Channel M Status).
 * @param[in]  vc       Virtual channel index (0..15).
 * @param[out] out_mask Live VCST(M) value.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok             Status returned.
 * @retval k_ra_err_invalid_arg vc > 15.
 * @retval k_ra_err_null_ptr   out_mask was nullptr.
 * @pre vc <= 15.
 * @pre out_mask is non-NULL.
 * @post *out_mask holds the VCST(M) value.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_vc_get_status(uint8_t vc, uint32_t* out_mask);

/**
 * @brief W1C-clear bits in VCST(M) via VCSC(M).
 * @param[in] vc   Virtual channel index (0..15).
 * @param[in] mask Bits to clear.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok             Cleared.
 * @retval k_ra_err_invalid_arg vc > 15.
 * @pre vc <= 15.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @post VCSC(M) has been written.
 * @post Targeted VCST(M) bits will eventually deassert.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_vc_clear_status(uint8_t vc, uint32_t mask);

/**
 * @brief Programme VCIE(M) (Virtual Channel M IRQ enable).
 * @param[in] vc   Virtual channel index (0..15).
 * @param[in] mask Bits to set in VCIE(M).
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok             Updated.
 * @retval k_ra_err_invalid_arg vc > 15.
 * @pre vc <= 15.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @post VCIE(M) = mask.
 * @post Subsequent matching VC events trigger the VC IRQ vector.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_vc_set_irq_enable(uint8_t vc, uint32_t mask);

/* =============================================================================
 * Power-management status / IRQ
 * =============================================================================
 */

/**
 * @brief Read PMST (Power Management Status) live value.
 * @param[out] out_mask Live PMST value.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok            Status returned.
 * @retval k_ra_err_null_ptr  out_mask was nullptr.
 * @pre out_mask is non-NULL.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @post *out_mask holds the PMST value.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_pm_get_status(uint32_t* out_mask);

/**
 * @brief W1C-clear bits in PMST via PMSC.
 * @param[in] mask Bits to clear (only [7:0] are clearable).
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller knows which PM bits are W1C.
 * @post PMSC has been written.
 * @post Targeted PMST bits will eventually deassert.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_pm_clear_status(uint32_t mask);

/**
 * @brief Programme PMIE (Power Management IRQ enable).
 * @param[in] mask Bits to set in PMIE.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller is in single-threaded init context.
 * @post PMIE = mask.
 * @post Subsequent matching PM events trigger the PM IRQ vector.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_pm_set_irq_enable(uint32_t mask);

/* =============================================================================
 * Generic short-packet FIFO
 * =============================================================================
 */

/**
 * @brief Programme GSCT (short-packet FIFO threshold + store flag).
 *
 * @param[in] threshold     FIFO depth-1 reported in GSST.PNUM that
 *                          asserts the GTH IRQ. Range 0..15.
 * @param[in] store_enable  1 = store incoming short packets in FIFO.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Updated.
 * @retval k_ra_err_invalid_arg threshold > 15.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre threshold <= 15.
 * @post GSCT.SHTH = threshold.
 * @post GSCT.GFIF = store_enable.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_short_packet_configure(uint8_t threshold, bool store_enable);

/**
 * @brief Programme GSIE (short-packet IRQ enable).
 * @param[in] mask Bits to set in GSIE.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller is in single-threaded init context.
 * @post GSIE = mask.
 * @post Subsequent matching GST events trigger the GST IRQ vector.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_short_packet_set_irq_enable(uint32_t mask);

/**
 * @brief Read GSST (short-packet status).
 * @param[out] out_mask Live GSST value.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok           Status returned.
 * @retval k_ra_err_null_ptr out_mask was nullptr.
 * @pre out_mask is non-NULL.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @post *out_mask holds the GSST value.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_short_packet_get_status(uint32_t* out_mask);

/**
 * @brief W1C-clear GSST.GOV (FIFO overflow) via GSSC.
 * @param[in] mask Bits to clear (only GOV bit is W1C).
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller knows which GSST bits are W1C.
 * @post GSSC has been written.
 * @post GSST.GOV will eventually deassert.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_short_packet_clear_status(uint32_t mask);

/**
 * @brief Drain one stored short-packet header from the FIFO.
 *
 * @details
 * Reads GSST.PNUM first; if non-zero, sets GSIU.FINC (advance read
 * pointer) then reads GSHT to retrieve the next header. The
 * returned ``raw`` field is the unprocessed GSHT snapshot and the
 * other fields are decoded for caller convenience.
 *
 * @param[out] out On success, the decoded short-packet header.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok           Header drained into ``*out``.
 * @retval k_ra_err_null_ptr out was nullptr.
 * @retval k_ra_err_empty    FIFO had no stored packets.
 *
 * @pre out is non-NULL.
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @post On success, GSIU.FINC was pulsed and *out holds a valid header.
 * @post GSST.PNUM is decremented by 1 on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_read_short_packet(ra_mipi_csi_short_packet_t* out);

/**
 * @brief Request a clear of the entire short-packet FIFO (GSIU.GFCLR).
 *
 * @details
 * Writes 1 to GSIU.GFCLR, then waits for GSST.GCD to read 1 (clear
 * complete) and writes 0 to GSIU.GFCLR to release the request line.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok             Cleared.
 * @retval k_ra_err_hw_timeout GCD did not assert within budget.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller is in single-threaded context.
 * @post FIFO is empty (GSST.PNUM = 0).
 * @post GSIU.GFCLR is back to 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_short_packet_clear_fifo(void);

/**
 * @brief Re-enable storing into the FIFO after a GOV overflow (GSIU.GFEN).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller has cleared the GOV flag via clear_status.
 * @post GSIU.GFEN was pulsed (W1).
 * @post Hardware will resume queuing packets.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_short_packet_re_enable_store(void);

/* =============================================================================
 * Interrupt path (handlers + dispatchers)
 * =============================================================================
 */

/**
 * @brief Attach the receive-status callback (MIPI CSI RX vector).
 *
 * @param[in] fn  Callback fired by ``ra_mipi_csi_dispatch``.
 * @param[in] ctx Caller context forwarded to ``fn``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always (passing ``fn = nullptr`` is the way to detach).
 *
 * @pre Caller is in single-threaded init context.
 * @pre ``fn`` is either nullptr (detach) or a valid function pointer.
 * @post The dispatcher will invoke ``fn(ctx, rxst)`` until detached.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_attach_handler(ra_mipi_csi_event_fn_t fn, void* ctx);

/**
 * @brief Attach the per-data-lane callback (MIPI CSI DL vector).
 * @param[in] fn  Callback fired by ``ra_mipi_csi_dispatch_dl``.
 * @param[in] ctx Caller context forwarded to ``fn``.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 * @pre Caller is in single-threaded init context.
 * @pre fn is either nullptr or a valid function pointer.
 * @post ``ra_mipi_csi_dispatch_dl`` will invoke ``fn`` for matching lanes.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_attach_dl_handler(ra_mipi_csi_dl_event_fn_t fn, void* ctx);

/**
 * @brief Attach the per-VC callback (MIPI CSI VC vector).
 * @param[in] fn  Callback fired by ``ra_mipi_csi_dispatch_vc``.
 * @param[in] ctx Caller context forwarded to ``fn``.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 * @pre Caller is in single-threaded init context.
 * @pre fn is either nullptr or a valid function pointer.
 * @post ``ra_mipi_csi_dispatch_vc`` will invoke ``fn`` per VC + once for generic.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_attach_vc_handler(ra_mipi_csi_vc_event_fn_t fn, void* ctx);

/**
 * @brief Attach the power-management callback (MIPI CSI PM vector).
 * @param[in] fn  Callback fired by ``ra_mipi_csi_dispatch_pm``.
 * @param[in] ctx Caller context forwarded to ``fn``.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 * @pre Caller is in single-threaded init context.
 * @pre fn is either nullptr or a valid function pointer.
 * @post ``ra_mipi_csi_dispatch_pm`` will invoke ``fn`` until detached.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_attach_pm_handler(ra_mipi_csi_pm_event_fn_t fn, void* ctx);

/**
 * @brief Attach the short-packet FIFO callback (MIPI CSI GST vector).
 * @param[in] fn  Callback fired by ``ra_mipi_csi_dispatch_short_packet``.
 * @param[in] ctx Caller context forwarded to ``fn``.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always.
 * @pre Caller is in single-threaded init context.
 * @pre fn is either nullptr or a valid function pointer.
 * @post ``ra_mipi_csi_dispatch_short_packet`` will invoke ``fn`` until detached.
 * @post No hardware state is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_attach_short_packet_handler(ra_mipi_csi_short_event_fn_t fn,
                                                               void*                        ctx);

/**
 * @brief ISR-side dispatcher for the receive vector.
 *
 * @details
 * Designed to be called from the wired-up MIPI CSI receive ISR
 * (``mipi_csi_rx_isr`` in the FSP layout). Reads RXST once, writes
 * RACTDETC to RXSC to clear the sticky detect bit, then forwards the
 * RXST snapshot to the registered callback.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller is the IRQ context owned by the MIPI CSI RX vector.
 * @post RXSC.RACTDETC has been written 1.
 * @post If a handler is attached, it has run synchronously.
 *
 * @note Thread safety: ISR-only; do not call from threads.
 * @since 0.1.0
 */
void ra_mipi_csi_dispatch(void);

/**
 * @brief ISR-side dispatcher for the data-lane vector.
 *
 * @details
 * Reads MIST + DLST0 + DLST1, W1C-clears DLSC0/DLSC1 (preserving the
 * RO ULP bit), and invokes the data-lane callback once per lane that
 * showed up in MIST.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller is the IRQ context owned by the MIPI CSI DL vector.
 * @post DLSC0 / DLSC1 have been W1C-cleared.
 * @post Handler ran once per lane that flagged in MIST.
 *
 * @note Thread safety: ISR-only.
 * @since 0.1.0
 */
void ra_mipi_csi_dispatch_dl(void);

/**
 * @brief ISR-side dispatcher for the per-VC vector.
 *
 * @details
 * Reads MIST, then for every VC bit that is set walks the
 * per-channel registers, clears each VCSC(M), and invokes the
 * per-VC callback. Generic errors (MLF + ECD) are accumulated and
 * delivered as a final callback with vc = 0xFF.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller is the IRQ context owned by the MIPI CSI VC vector.
 * @post Every flagged VCSC(M) has been written.
 * @post Handler ran once per VC plus once for the generic-error summary.
 *
 * @note Thread safety: ISR-only.
 * @since 0.1.0
 */
void ra_mipi_csi_dispatch_vc(void);

/**
 * @brief ISR-side dispatcher for the power-management vector.
 *
 * @details
 * Reads PMST, W1C-clears the lower 8 bits via PMSC, then invokes
 * the PM callback.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller is the IRQ context owned by the MIPI CSI PM vector.
 * @post PMSC has been written with the W1C-clearable bits.
 * @post Handler ran once with the PMST snapshot.
 *
 * @note Thread safety: ISR-only.
 * @since 0.1.0
 */
void ra_mipi_csi_dispatch_pm(void);

/**
 * @brief ISR-side dispatcher for the generic-short-packet vector.
 *
 * @details
 * Reads GSST, W1C-clears GOV via GSSC, then invokes the GST
 * callback. The handler is expected to drain the FIFO via
 * ``ra_mipi_csi_read_short_packet``.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller is the IRQ context owned by the MIPI CSI GST vector.
 * @post GSSC has been written with the W1C-clearable bits.
 * @post Handler ran once with the GSST snapshot.
 *
 * @note Thread safety: ISR-only.
 * @since 0.1.0
 */
void ra_mipi_csi_dispatch_short_packet(void);

/* =============================================================================
 * Sweep 6 -- per-VC framing helpers + error reporting
 * =============================================================================
 */

/**
 * @enum ra_mipi_csi_data_format_t
 * @brief Per-virtual-channel payload format used by ::ra_mipi_csi_set_data_format.
 *
 * @details
 * Maps onto the CSI-2 data-type byte (HUM Ch 66.3.10 "DTEL" p 3943
 * and 66.3.11 "DTEH" p 3944). Selecting a format on a VC sets the
 * matching bit in DTEL/DTEH so the receiver accepts that format
 * within the CSI-2 packet header.  Selecting ``k_ra_mipi_csi_format_off``
 * removes the contribution that VC made, allowing graceful narrowing.
 */
typedef enum : uint8_t {
  k_ra_mipi_csi_format_off       = 0U,    /**< Disable -- accept nothing for VC.   */
  k_ra_mipi_csi_format_yuv422_8  = 0x1EU, /**< CSI-2 DT 0x1E -- YUV 4:2:2 8-bit.   */
  k_ra_mipi_csi_format_yuv422_10 = 0x1FU, /**< CSI-2 DT 0x1F -- YUV 4:2:2 10-bit.  */
  k_ra_mipi_csi_format_rgb888    = 0x24U, /**< CSI-2 DT 0x24 -- RGB888.            */
  k_ra_mipi_csi_format_raw8      = 0x2AU, /**< CSI-2 DT 0x2A -- RAW8.              */
  k_ra_mipi_csi_format_raw10     = 0x2BU, /**< CSI-2 DT 0x2B -- RAW10 (FSP-only).  */
  k_ra_mipi_csi_format_yuv420    = 0x18U, /**< CSI-2 DT 0x18 -- YUV 4:2:0 8-bit.   */
} ra_mipi_csi_data_format_t;

/**
 * @struct ra_mipi_csi_error_report_t
 * @brief One ECC / CRC error notification from the receiver.
 *
 * @details
 * Decoded from VCST(M) (HUM Ch 66.3.18 p 3949) -- the same bits the
 * VC dispatcher already surfaces, but reshaped so callers do not have
 * to re-decode them. ``vc == 0xFF`` is used for the catch-all "MLF/ECD"
 * flags which apply to every channel.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t  vc;                /**< Virtual channel that reported, or 0xFF.   */
  bool     ecc_corrected;     /**< VCST.ECC -- 1-bit ECC corrected.          */
  bool     ecc_two_bit_error; /**< VCST.ECD -- 2-bit ECC error.              */
  bool     crc_error;         /**< VCST.CRC -- payload CRC mismatch.         */
  uint32_t raw_vcst;          /**< Original VCST snapshot (bits as decoded). */
} ra_mipi_csi_error_report_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_mipi_csi_error_fn_t
 * @brief Callback invoked when the receiver reports an ECC or CRC error.
 * @param[in] ctx    Caller context registered with ::ra_mipi_csi_attach_error_handler.
 * @param[in] report Decoded error descriptor (only valid for the call).
 */
typedef void (*ra_mipi_csi_error_fn_t)(void* ctx, const ra_mipi_csi_error_report_t* report);

/**
 * @brief Filter which CSI-2 virtual channels the receiver will surface.
 *
 * @details
 * Each bit i of ``vc_mask`` enables VCi (i = 0..15). The driver
 * disables VCIE on every channel whose bit is clear (so spurious
 * traffic does not raise IRQs) and restores the previous IRQ mask on
 * channels that are re-enabled. HUM Ch 66.3.20 "VCIE(M)" p 3952.
 *
 * @param[in] vc_mask Bitmap of accepted virtual channels (bit 0 = VC0).
 *
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok               VCIE updated for every channel.
 * @retval k_ra_err_invalid_arg  ``vc_mask == 0`` (would silence the receiver).
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre Caller is in single-threaded init / reconfigure context.
 * @post VCIE for every selected VC is non-zero (default mask restored).
 * @post VCIE for every unselected VC is zero.
 *
 * @note Not thread-safe.
 * @see ra_mipi_csi_set_data_format
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_set_virtual_channels(uint16_t vc_mask);

/**
 * @brief Select the CSI-2 payload format the receiver should accept on a
 *        specific virtual channel.
 *
 * @details
 * The hardware filter (DTEL/DTEH) is global across virtual channels --
 * selecting a format on one VC therefore enables the matching DT bit
 * for the entire receiver. The driver records the per-VC selection in
 * a software shadow so a subsequent call with
 * ``k_ra_mipi_csi_format_off`` can re-compute the union and clear the
 * DT bit if no other VC still wants it. HUM Ch 66.3.10/66.3.11
 * pp 3943-3944.
 *
 * @param[in] vc     Virtual channel index (0..15).
 * @param[in] format Payload format to accept (or ``_off`` to disable).
 *
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok                Filter updated.
 * @retval k_ra_err_invalid_arg   ``vc > 15`` or unknown format value.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded.
 * @pre ``vc`` is in 0..15.
 * @post DTEL/DTEH reflect the union of all per-VC selections.
 * @post Future calls to ::ra_mipi_csi_set_data_type_filter override
 *       these contributions with explicit masks.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_set_data_format(uint8_t vc, ra_mipi_csi_data_format_t format);

/**
 * @brief Register a callback invoked on ECC / CRC error reports.
 *
 * @details
 * The CSI VC dispatcher already surfaces VCST contents. This helper
 * filters those events down to the ECC + CRC error subset and
 * decodes them into a ::ra_mipi_csi_error_report_t for the caller.
 * Pass ``fn = nullptr`` to detach.
 *
 * @param[in] fn  Callback to invoke (or NULL to detach).
 * @param[in] ctx Caller-owned context forwarded to ``fn``.
 *
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok Always.
 *
 * @pre Caller's ``fn`` is ISR-safe.
 * @pre Caller owns the ``ctx`` lifetime for as long as ``fn`` is attached.
 * @post Subsequent ``ra_mipi_csi_dispatch_vc`` calls forward ECC/CRC
 *       errors to ``fn`` in addition to the VC callback.
 * @post Detaching with NULL silently swallows further errors.
 *
 * @note ISR-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_attach_error_handler(ra_mipi_csi_error_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Gate the peripheral via MSTP (low-power entry helper).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Peripheral gated.
 * @retval k_ra_err_invalid_state ra_mstp ref-count was already 0.
 *
 * @pre ``ra_mipi_csi_init`` has succeeded at least once.
 * @pre Reception has been stopped via ``ra_mipi_csi_stop_receive``.
 * @post MSTP bit for MIPI CSI is set.
 * @post Register access is undefined until ``exit_stop``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_enter_stop(void);

/**
 * @brief Ungate the peripheral via MSTP (low-power exit helper).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Peripheral clocked.
 * @retval k_ra_err_hw_init_failed MSTP enable failed.
 *
 * @pre ``ra_mipi_csi_enter_stop`` was called previously.
 * @pre Caller is in single-threaded resume context.
 * @post MSTP bit for MIPI CSI is cleared.
 * @post Register access is valid again.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_csi_exit_stop(void);

#ifdef __cplusplus
}
#endif
