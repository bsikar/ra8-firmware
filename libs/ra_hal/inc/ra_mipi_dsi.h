/**
 * @file ra_mipi_dsi.h
 * @brief MIPI DSI-2 host driver -- public API (full HUM Ch 65 surface)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Hand-written HAL wrapper around the RA8D2 MIPI DSI Host module
 * (HUM Ch 65, p 3839-3934). Pairs with the GLCDC (HUM Ch 63) on the
 * pixel-data side and the on-die D-PHY (HUM Ch 64) on the link side.
 *
 * ## Pairing with `ra_mipi_phy`
 *
 * The D-PHY analog timing block lives in HUM Ch 64 and is owned by
 * a separate `ra_mipi_phy` driver. This module **does not** touch the
 * PHY register window at `0x40346C00` -- the caller is expected to
 * bring the PHY up first (PLL locked, lanes out of LP-11) and then
 * call `ra_mipi_dsi_init()` to configure the link/protocol layer on
 * top of it. Tearing down goes the other way: stop the DSI link, then
 * power down the PHY.
 *
 * ## Surface (this revision)
 *
 *  - Lifecycle: `_init / _deinit / _enter_stop / _exit_stop`.
 *  - HS clock control: `_hs_clock_start / _hs_clock_stop`.
 *  - Software reset helper: `_soft_reset`.
 *  - Sequence channel commands: `_send_short_packet`,
 *    `_send_long_packet`, `_send_command` (full BTA + LP/HS + aux op).
 *  - Read transactions: `_read_packet` (assembles BTA-then-read on
 *    sequence channel 0).
 *  - Video mode: `_video_configure`, `_video_start`, `_video_stop`,
 *    `_video_status_get`.
 *  - ULPS: `_ulps_enter`, `_ulps_exit` -- per lane (clock / data).
 *  - Receive path: `_rx_result_get`, `_rx_payload_read`,
 *    `_ack_error_get`, `_ack_error_clear`.
 *  - Status / IRQ: every sub-status register has a `_get_*` and
 *    `_clear_*` helper, every IER has a `_irq_enable_*` helper.
 *  - Tearing-effect: `_te_event_pending`, `_te_event_clear`.
 *  - Per-IRQ-source dispatch: `_dispatch_seq0`, `_dispatch_seq1`,
 *    `_dispatch_video`, `_dispatch_receive`, `_dispatch_fatal`,
 *    `_dispatch_phy` plus the legacy fanout `_dispatch`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_mipi_dsi_regs.h"
#include "ra_err.h"

/**
 * @enum ra_mipi_dsi_lane_count_t
 * @brief Number of D-PHY data lanes the DSI link drives.
 *
 * @details
 * HUM Ch 65.1 "Overview" p 3839 confirms the host supports 1- and
 * 2-lane configurations only; the package brings out two physical
 * data lanes plus one clock lane.
 */
typedef enum : uint8_t {
  k_ra_mipi_dsi_lanes_1 = 1U, /**< 1 data lane + 1 clock lane.            */
  k_ra_mipi_dsi_lanes_2 = 2U, /**< 2 data lanes + 1 clock lane.           */
} ra_mipi_dsi_lane_count_t;

/**
 * @enum ra_mipi_dsi_clock_mode_t
 * @brief Continuous vs non-continuous HS clock (HSCLKSETR.HSCLMD).
 */
typedef enum : uint8_t {
  k_ra_mipi_dsi_clock_non_continuous = 0U, /**< HS clock idles in LP.     */
  k_ra_mipi_dsi_clock_continuous     = 1U, /**< HS clock runs forever.    */
} ra_mipi_dsi_clock_mode_t;

/**
 * @enum ra_mipi_dsi_vc_t
 * @brief DSI virtual channel identifier (DT[7:6] in the packet header).
 *
 * @details
 * Only VC0 is wired up by the EK-RA8D2 panel; the other three are
 * reserved for multi-display use cases. HUM Ch 65 lists VC0..VC3 in
 * the descriptor word A bit layout.
 */
typedef enum : uint8_t {
  k_ra_mipi_dsi_vc0 = 0U, /**< Virtual channel 0 (default panel).         */
  k_ra_mipi_dsi_vc1 = 1U, /**< Virtual channel 1.                         */
  k_ra_mipi_dsi_vc2 = 2U, /**< Virtual channel 2.                         */
  k_ra_mipi_dsi_vc3 = 3U, /**< Virtual channel 3.                         */
} ra_mipi_dsi_vc_t;

/**
 * @enum ra_mipi_dsi_dt_t
 * @brief Frequently used DSI Data Types (cmd id) for short and long packets.
 *
 * @details
 * Drawn from the MIPI DSI 1.1 specification table 7. Both short- and
 * long-packet entries are listed so the same enum can drive
 * `ra_mipi_dsi_send_short_packet`, `_send_long_packet`, and the
 * video-mode pixel-stream selector.
 */
typedef enum : uint8_t {
  /* Short packet writes */
  k_ra_mipi_dsi_dt_v_sync_start        = 0x01U, /**< Sync event: VSYNC start.    */
  k_ra_mipi_dsi_dt_v_sync_end          = 0x11U, /**< Sync event: VSYNC end.      */
  k_ra_mipi_dsi_dt_h_sync_start        = 0x21U, /**< Sync event: HSYNC start.    */
  k_ra_mipi_dsi_dt_h_sync_end          = 0x31U, /**< Sync event: HSYNC end.      */
  k_ra_mipi_dsi_dt_color_mode_off      = 0x02U, /**< CM Off command.             */
  k_ra_mipi_dsi_dt_color_mode_on       = 0x12U, /**< CM On command.              */
  k_ra_mipi_dsi_dt_shutdown_peripheral = 0x22U, /**< Shut down peripheral.       */
  k_ra_mipi_dsi_dt_turn_on_peripheral  = 0x32U, /**< Turn on peripheral.         */
  k_ra_mipi_dsi_dt_compression_mode    = 0x07U, /**< Compression mode command.   */
  k_ra_mipi_dsi_dt_end_of_transmission = 0x08U, /**< End of Transmission packet. */
  k_ra_mipi_dsi_dt_dcs_short_write_0   = 0x05U, /**< DCS short write, 0 args.    */
  k_ra_mipi_dsi_dt_dcs_short_write_1   = 0x15U, /**< DCS short write, 1 arg.     */
  k_ra_mipi_dsi_dt_gen_short_write_0   = 0x03U, /**< Generic short, 0 args.      */
  k_ra_mipi_dsi_dt_gen_short_write_1   = 0x13U, /**< Generic short, 1 arg.       */
  k_ra_mipi_dsi_dt_gen_short_write_2   = 0x23U, /**< Generic short, 2 args.      */
  k_ra_mipi_dsi_dt_gen_read_0          = 0x04U, /**< Generic read, 0 args.       */
  k_ra_mipi_dsi_dt_gen_read_1          = 0x14U, /**< Generic read, 1 arg.        */
  k_ra_mipi_dsi_dt_gen_read_2          = 0x24U, /**< Generic read, 2 args.       */
  k_ra_mipi_dsi_dt_dcs_read            = 0x06U, /**< DCS read, 0 args.           */
  k_ra_mipi_dsi_dt_execute_queue       = 0x16U, /**< Execute queue.              */
  k_ra_mipi_dsi_dt_set_max_return_pkt  = 0x37U, /**< Set max return packet size. */
  /* Long packet writes */
  k_ra_mipi_dsi_dt_null_packet     = 0x09U, /**< Null packet (long).         */
  k_ra_mipi_dsi_dt_blanking_packet = 0x19U, /**< Blanking packet (long).     */
  k_ra_mipi_dsi_dt_gen_long_write  = 0x29U, /**< Generic long write.         */
  k_ra_mipi_dsi_dt_dcs_long_write  = 0x39U, /**< DCS long write.             */
  /* Pixel stream long packets */
  k_ra_mipi_dsi_dt_pixel_rgb565       = 0x0EU, /**< 16-bit RGB packed pixel.    */
  k_ra_mipi_dsi_dt_pixel_rgb666       = 0x1EU, /**< 18-bit RGB packed pixel.    */
  k_ra_mipi_dsi_dt_pixel_rgb666_loose = 0x2EU, /**< 18-bit RGB loosely packed.  */
  k_ra_mipi_dsi_dt_pixel_rgb888       = 0x3EU, /**< 24-bit RGB packed pixel.    */
} ra_mipi_dsi_dt_t;

/**
 * @enum ra_mipi_dsi_lane_sel_t
 * @brief Lane selector for ULPS enter / exit transitions.
 *
 * @details
 * Bits can be OR'd: for example `lane = k_ra_mipi_dsi_lane_clock |
 * k_ra_mipi_dsi_lane_data` puts the entire link into ULPS.
 */
typedef enum : uint8_t {
  k_ra_mipi_dsi_lane_none  = 0U,      /**< No lanes selected.                */
  k_ra_mipi_dsi_lane_clock = 1U << 0, /**< Clock lane.                     */
  k_ra_mipi_dsi_lane_data  = 1U << 1, /**< All data lanes (1 or 2).        */
  k_ra_mipi_dsi_lane_all   = (uint8_t)((1U << 0) | (1U << 1)),
  /**< Composite mask: clock + all data lanes.                             */
} ra_mipi_dsi_lane_sel_t;

/**
 * @enum ra_mipi_dsi_event_t
 * @brief Event class delivered to the user callback.
 *
 * @details
 * Mirrors the FSP `mipi_dsi_event_t` so application code that already
 * targets FSP can be ported with a 1:1 enum substitution.
 */
typedef enum : uint8_t {
  k_ra_mipi_dsi_event_seq0    = 0U, /**< Sequence channel 0 (LP) finished. */
  k_ra_mipi_dsi_event_seq1    = 1U, /**< Sequence channel 1 (HS) finished. */
  k_ra_mipi_dsi_event_video   = 2U, /**< Video-mode status changed.        */
  k_ra_mipi_dsi_event_receive = 3U, /**< Packet / response received.       */
  k_ra_mipi_dsi_event_fatal   = 4U, /**< Fatal-error class IRQ.            */
  k_ra_mipi_dsi_event_phy     = 5U, /**< PPI (PHY) lane event.             */
} ra_mipi_dsi_event_t;

/**
 * @struct ra_mipi_dsi_timing_t
 * @brief LP / HS guard-band timing values.
 *
 * @details
 * One-to-one mapping with `mipi_dsi_timing_t` from the FSP -- carries
 * the values that ultimately land in CLSTPTSETR and LPTRNSTSETR.
 *
 * cppcheck cannot see the test TU so it flags every member as unused;
 * each value is consumed by `ra_mipi_dsi_init()` in
 * `libs/ra_hal/src/ra_mipi_dsi.c`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t clock_stop_time;       /**< CLSTPTSETR.CLKSTPT[11:2].          */
  uint8_t  clock_beforehand_time; /**< CLSTPTSETR.CLKBFHT[23:16].         */
  uint8_t  clock_keep_time;       /**< CLSTPTSETR.CLKKPT[31:24].          */
  uint16_t go_lp_and_back;        /**< LPTRNSTSETR.GOLPBKT[9:0].          */
} ra_mipi_dsi_timing_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_mipi_dsi_timeouts_t
 * @brief Bus timeout counters (HSTXTOSETR / LRXHTOSETR / TATOSETR /
 *        PRESPTO* registers).
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t hs_tx_timeout;      /**< HSTXTOSETR.                          */
  uint32_t lp_rx_host_timeout; /**< LRXHTOSETR.                          */
  uint32_t turnaround_timeout; /**< TATOSETR.                            */
  uint32_t bta_timeout;        /**< PRESPTOBTASETR.                      */
  uint32_t lp_rw_timeout;      /**< PRESPTOLPSETR (split LPRTO|LPWTO).   */
  uint32_t hs_rw_timeout;      /**< PRESPTOHSSETR (split HSRTO|HSWTO).   */
} ra_mipi_dsi_timeouts_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_mipi_dsi_video_cfg_t
 * @brief Video-mode timing + format + sync configuration.
 *
 * @details
 * Aggregates the inputs to VMSET0R / VMSET1R / VMPPSETR / VMVSSETR /
 * VMVPSETR / VMHSSETR / VMHPSETR. All counts are in panel pixels /
 * lines (the hardware multiplies by the per-lane byte clock as needed).
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_mipi_dsi_dt_t pixel_format;             /**< RGB565 / RGB666 / RGB888.   */
  ra_mipi_dsi_vc_t virtual_channel;          /**< VC for the pixel stream.    */
  bool             sync_pulse;               /**< Non-burst with sync pulse.  */
  bool             hsa_no_lp;                /**< Stay HS during HSA period.  */
  bool             hbp_no_lp;                /**< Stay HS during HBP period.  */
  bool             hfp_no_lp;                /**< Stay HS during HFP period.  */
  bool             vsync_active_high;        /**< VMVSSETR.VSPOL.             */
  bool             hsync_active_high;        /**< VMHSSETR.HSPOL.             */
  uint16_t         vertical_sync_lines;      /**< VMVSSETR.VSA[11:0].         */
  uint16_t         vertical_active_lines;    /**< VMVSSETR.VACT[30:16].       */
  uint16_t         vertical_back_porch;      /**< VMVPSETR.VBP[12:0].         */
  uint16_t         vertical_front_porch;     /**< VMVPSETR.VFP[28:16].        */
  uint16_t         horizontal_sync_lines;    /**< VMHSSETR.HSA[11:0].         */
  uint16_t         horizontal_active_pixels; /**< VMHSSETR.HACT[30:16].    */
  uint16_t         horizontal_back_porch;    /**< VMHPSETR.HBP[12:0].         */
  uint16_t         horizontal_front_porch;   /**< VMHPSETR.HFP[28:16].        */
  uint16_t         video_mode_delay;         /**< VMSET1R.DLY[13:2] x 4.      */
} ra_mipi_dsi_video_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_mipi_dsi_command_t
 * @brief Generic DSI packet descriptor for `ra_mipi_dsi_send_command`.
 *
 * @details
 * Encapsulates everything the hardware needs to assemble a sequence-
 * channel descriptor. `tx_len > 2` triggers long-packet mode; otherwise
 * the bytes are packed into the descriptor header.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_mipi_dsi_dt_t  cmd_id;          /**< Data Type / command opcode.   */
  ra_mipi_dsi_vc_t  virtual_channel; /**< VC[1:0] in the packet header. */
  ra_mipi_dsi_bta_t bta;             /**< BTA selector (none/after/...).*/
  bool              low_power;       /**< true = LP escape (ch 0).      */
  bool              ack_request;     /**< Request ack from peripheral.  */
  bool              aux_operation;   /**< AUXOP descriptor word C.      */
  uint8_t           action_code;     /**< ACTCODE (only when aux_op).   */
  uint16_t          tx_len;          /**< Bytes in p_tx_buffer.         */
  const uint8_t*    p_tx_buffer;     /**< Source bytes for long packet. */
  uint8_t*          p_rx_buffer;     /**< Sink bytes for read responses.*/
} ra_mipi_dsi_command_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_mipi_dsi_rx_result_t
 * @brief Decoded view of a single RXRSS slot register.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t          data[2];              /**< Header DATA0 / DATA1 bytes. */
  ra_mipi_dsi_dt_t cmd_id;               /**< DT[5:0] received.           */
  ra_mipi_dsi_vc_t virtual_channel;      /**< VC[1:0] received.           */
  bool             long_packet;          /**< FMT bit (1 = long packet).  */
  bool             rx_success;           /**< RXSUC flag.                 */
  bool             rx_fatal_error;       /**< RXFERR flag.                */
  bool             rx_fail;              /**< RXFAIL flag.                */
  bool             rx_packet_data_fail;  /**< RXPFAIL flag.               */
  bool             rx_correctable_error; /**< RXCERR flag.                */
  bool             rx_ack_and_error;     /**< RXAKE flag.                 */
  bool             info_overwritten;     /**< INFOOW flag.                */
} ra_mipi_dsi_rx_result_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_mipi_dsi_link_status_t
 * @brief Decoded view of LINKSR.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  bool sequence_ch0_running; /**< LINKSR.SQ0RUN.                          */
  bool sequence_ch1_running; /**< LINKSR.SQ1RUN.                          */
  bool video_running;        /**< LINKSR.VRUN.                            */
  bool hs_busy;              /**< LINKSR.HSBUSY.                          */
  bool lp_busy;              /**< LINKSR.LPBUSY.                          */
} ra_mipi_dsi_link_status_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_mipi_dsi_ack_error_t
 * @brief Decoded view of AKEPLATIR / AKEPACMSR.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t         error_report;    /**< 16-bit DSI ack/error bitmap.     */
  ra_mipi_dsi_vc_t virtual_channel; /**< VC tag of the report.            */
} ra_mipi_dsi_ack_error_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_mipi_dsi_config_t
 * @brief Static configuration passed to ::ra_mipi_dsi_init.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read in `ra_mipi_dsi_init()` in
 * `libs/ra_hal/src/ra_mipi_dsi.c`.
 *
 * @invariant `lane_count` is one of `k_ra_mipi_dsi_lanes_1` or
 *            `k_ra_mipi_dsi_lanes_2` -- any other value is rejected.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_mipi_dsi_lane_count_t lane_count;             /**< 1 or 2.        */
  ra_mipi_dsi_clock_mode_t clock_mode;             /**< HS clock mode. */
  uint16_t                 max_return_packet_size; /**< DSISETR.MRPSZ. */
  uint8_t                  ulps_wakeup_period;     /**< ULPSSETR.WKUP. */
  bool                     ecc_check_enable;       /**< DSISETR.ECCEN. */
  bool                     eotp_enable;            /**< DSISETR.EOTPEN.*/
  bool                     scramble_enable;        /**< DSISETR.SCREN. */
  bool                     tearing_detect_enable;  /**< DSISETR.EXTEMD.*/
  uint8_t                  crc_check_vc_mask;      /**< Bit i = VCi CRC.*/
  ra_mipi_dsi_timing_t     timing;                 /**< Guard timings. */
  ra_mipi_dsi_timeouts_t   timeouts;               /**< Bus timeouts.  */
} ra_mipi_dsi_config_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_mipi_dsi_event_fn_t
 * @brief MIPI DSI ISR-context callback signature.
 *
 * @details
 * `status_mask` carries the snapshot of the ISR register at the moment
 * the dispatch routine ran. The driver clears the underlying sub-status
 * registers (SQCH0SCR, SQCH1SCR, VMSCR, RXSCR, FERRSCR, PLSCR) before
 * invoking the user callback so the application sees a coherent view.
 */
typedef void (*ra_mipi_dsi_event_fn_t)(void* ctx, ra_mipi_dsi_event_t event, uint32_t status_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the MIPI DSI host link layer.
 *
 * @details
 * 1. Releases module-stop bit `MSTPCRC.MSTPC10` (HUM 11.2.8 p 446).
 * 2. Asserts then de-asserts `RSTCR.SWRST` to drop the block back to
 *    a known reset state.
 * 3. Programmes lane count + lane enables in `TXSETR`.
 * 4. Loads `DSISETR` from the config struct (ECC, CRC mask, scramble,
 *    EoTp, tearing detect, max-return-packet size).
 * 5. Loads guard-band timing into `CLSTPTSETR / LPTRNSTSETR`.
 * 6. Loads bus timeouts into `HSTXTOSETR / LRXHTOSETR / TATOSETR /
 *    PRESPTOBTASETR / PRESPTOLPSETR / PRESPTOHSSETR`.
 * 7. Loads ULPS wake-up period into `ULPSSETR`.
 * 8. Clears all latched status flags so the first IRQ reflects only
 *    post-init events.
 *
 * The HS clock is **not** started by this function; call
 * `ra_mipi_dsi_hs_clock_start()` separately so the application can
 * insert PHY-level steps in between.
 *
 * @param[in] cfg Static driver configuration. Must not be `nullptr`.
 *
 * @return `ra_err_t` outcome.
 * @retval k_ra_ok                Driver initialised, link up.
 * @retval k_ra_err_null_ptr      `cfg == nullptr`.
 * @retval k_ra_err_invalid_arg   Lane count outside [1,2].
 * @retval k_ra_err_hw_timeout    MSTP read-back loop timed out.
 *
 * @pre `cfg` is non-NULL.
 * @pre `cfg->lane_count` is one of the `k_ra_mipi_dsi_lanes_*` values.
 * @post On success, `LINKSR` reports HSBUSY/LPBUSY clear.
 * @post On success, every sub-status register reads 0.
 *
 * @note Not thread-safe. Call from single-threaded init context.
 * @warning The MIPI PHY (HUM Ch 64) must be brought up first.
 *
 * @see ra_mipi_dsi_deinit
 * @see ra_mipi_dsi_hs_clock_start
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_init(const ra_mipi_dsi_config_t* cfg);

/**
 * @brief Tear down the MIPI DSI host link layer.
 *
 * @details
 * Detaches any IRQ handler, asserts `RSTCR.SWRST`, then disables the
 * MSTP gate. Idempotent: calling twice in a row is harmless.
 *
 * @return `ra_err_t` outcome.
 * @retval k_ra_ok                Block stopped, MSTP gated.
 * @retval k_ra_err_hw_timeout    MSTP read-back loop timed out.
 *
 * @pre Driver was initialised with ::ra_mipi_dsi_init (ignored if not).
 * @pre No active sequence operation (`LINKSR.SQ0RUN == SQ1RUN == 0`).
 * @post IRQ callback pointer cleared.
 * @post `MSTPCRC.MSTPC10` set (block clock gated).
 *
 * @note Not thread-safe.
 *
 * @see ra_mipi_dsi_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_deinit(void);

/**
 * @brief Gate the MIPI DSI block at the MSTP register.
 *
 * @return `ra_err_t` outcome.
 * @retval k_ra_ok               Block gated.
 * @retval k_ra_err_hw_timeout   MSTP read-back loop timed out.
 *
 * @pre Sequence + video engines are idle (caller's responsibility).
 * @pre IRQs are masked.
 * @post `MSTPCRC.MSTPC10` is set.
 *
 * @see ra_mipi_dsi_exit_stop
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_enter_stop(void);

/**
 * @brief Re-enable the MIPI DSI block clock from MSTP.
 *
 * @return `ra_err_t` outcome.
 * @retval k_ra_ok               Block clocked again.
 * @retval k_ra_err_hw_timeout   MSTP read-back loop timed out.
 *
 * @pre Caller is in single-threaded context.
 * @pre PHY is up and out of LP-11 stop.
 * @post `MSTPCRC.MSTPC10` is clear, register I/O works again.
 *
 * @see ra_mipi_dsi_enter_stop
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_exit_stop(void);

/**
 * @brief Pulse `RSTCR.SWRST` to take the protocol-layer state back to
 *        a clean slate (registers retained).
 *
 * @return `k_ra_ok` always.
 *
 * @pre Caller has IRQs masked or is in IRQ context.
 * @pre Sequence channels are idle.
 * @post `RSTSR.RSTHS|RSTLP|RSTAPB|RSTAXI|RSTV` read 0 on success.
 * @post Block is ready for a fresh `ra_mipi_dsi_hs_clock_start()`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_soft_reset(void);

/* =============================================================================
 * HS clock control
 * =============================================================================
 */

/**
 * @brief Start the high-speed differential clock to the panel.
 *
 * @details
 * Sets `HSCLKSETR.HSCLST = 1` and, if the init config selected
 * continuous mode, also sets `HSCLMD = 1`. Polls `PLSR.CLLP2HS` until
 * the clock-lane LP-to-HS event fires (bounded by
 * `k_ra_mipi_dsi_busy_loop_max`).
 *
 * @return `k_ra_ok` on success, `k_ra_err_hw_timeout` on poll timeout.
 *
 * @pre Driver is initialised.
 * @pre PHY is up.
 * @post `LINKSR.HSBUSY` clear once HS clock is stable.
 * @post Continuous mode: clock keeps running until
 *       `ra_mipi_dsi_hs_clock_stop()`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_hs_clock_start(void);

/**
 * @brief Stop the high-speed differential clock.
 *
 * @details
 * Clears `HSCLKSETR.HSCLST` and waits for `PLSR.CLHS2LP`.
 *
 * @return `k_ra_ok` on success, `k_ra_err_hw_timeout` on poll timeout.
 *
 * @pre Sequence channels idle.
 * @pre Video mode stopped.
 * @post HS clock lane is back to LP-11.
 * @post Subsequent HS Tx will fail until the clock is restarted.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_hs_clock_stop(void);

/* =============================================================================
 * Sequence-channel commands
 * =============================================================================
 */

/**
 * @brief Send a DSI short packet on sequence channel 0 (LP escape).
 *
 * @details
 * Backwards-compat helper -- assembles a default LP descriptor with
 * SPD=1, BTA=none, no ack, no aux, then pulses START. Equivalent to
 * `ra_mipi_dsi_send_command()` with the matching `ra_mipi_dsi_command_t`.
 *
 * @param[in] cmd_id One of the `k_ra_mipi_dsi_dt_*` short-write constants.
 * @param[in] vc     Virtual channel (0..3).
 * @param[in] param0 First parameter byte.
 * @param[in] param1 Second parameter byte.
 *
 * @return `ra_err_t` outcome.
 * @retval k_ra_ok                Packet queued and START asserted.
 * @retval k_ra_err_invalid_arg   `vc > 3`.
 * @retval k_ra_err_busy          Sequence engine already running.
 *
 * @pre Driver was initialised via ::ra_mipi_dsi_init.
 * @pre `LINKSR.SQ0RUN` is 0.
 * @post Descriptor 0 of channel 0 carries the assembled header.
 * @post `SQCH0SET0R` was pulsed with START + CHSEL.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_send_short_packet(ra_mipi_dsi_dt_t cmd_id,
                                                     ra_mipi_dsi_vc_t vc,
                                                     uint8_t          param0,
                                                     uint8_t          param1);

/**
 * @brief Send a DSI long-packet write (Generic Long / DCS Long).
 *
 * @details
 * Stages the payload bytes into TXPPD0..3R (max 16 bytes) and points
 * the descriptor at the packet RAM. For payloads > 16 bytes the
 * descriptor word D carries an external buffer pointer.
 *
 * @param[in] cmd_id   `k_ra_mipi_dsi_dt_gen_long_write` or
 *                     `k_ra_mipi_dsi_dt_dcs_long_write`.
 * @param[in] vc       Virtual channel (0..3).
 * @param[in] data     Payload bytes (`tx_len` long).
 * @param[in] tx_len   Payload length in bytes (max 65535 per spec).
 * @param[in] low_power true to send via channel 0 (LP), false for
 *                      channel 1 (HS).
 *
 * @return `ra_err_t` outcome.
 * @retval k_ra_ok               Packet queued, START asserted.
 * @retval k_ra_err_null_ptr     `data == nullptr` and `tx_len > 0`.
 * @retval k_ra_err_invalid_arg  `vc > 3` or LP and `tx_len > 128`.
 * @retval k_ra_err_busy         Sequence engine already running.
 *
 * @pre Driver initialised.
 * @pre Either `tx_len == 0` or `data` valid.
 * @post Descriptor 0 of the selected channel carries the long-packet
 *       header (DATA0 = WC[7:0], DATA1 = WC[15:8]).
 * @post `SQCH*SET0R` pulsed with START + CHSEL for that channel.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_send_long_packet(ra_mipi_dsi_dt_t cmd_id,
                                                    ra_mipi_dsi_vc_t vc,
                                                    const uint8_t*   data,
                                                    uint16_t         tx_len,
                                                    bool             low_power);

/**
 * @brief Send any sequence-channel command (full-feature path).
 *
 * @details
 * Assembles a descriptor from `cmd` -- handles short / long / aux-op /
 * BTA / ack-request / LP vs HS / read-with-rx-buffer in one entry
 * point. This is the FSP-equivalent surface; the helpers above just
 * wrap it.
 *
 * @param[in] cmd Validated command descriptor.
 *
 * @return `ra_err_t` outcome.
 * @retval k_ra_ok                Packet queued.
 * @retval k_ra_err_null_ptr      `cmd == nullptr`.
 * @retval k_ra_err_invalid_arg   `cmd->virtual_channel > 3`, or LP and
 *                                `tx_len > 128`, or HS and `tx_len > 1024`,
 *                                or aux op while video running.
 * @retval k_ra_err_busy          Sequence engine running.
 * @retval k_ra_err_invalid_state LP requested while video mode running.
 *
 * @pre `cmd` non-NULL.
 * @pre Driver initialised.
 * @post Selected sequence channel descriptor 0 populated.
 * @post `SQCH*SET0R` pulsed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_send_command(const ra_mipi_dsi_command_t* cmd);

/**
 * @brief Issue a BTA-then-read on sequence channel 0 and return the
 *        captured response payload.
 *
 * @details
 * Builds a Generic / DCS read descriptor with `BTA = bta_read`, kicks
 * the sequence, then leaves response collection to the IRQ -- the
 * caller polls `ra_mipi_dsi_rx_result_get()` for the slot containing
 * the result.
 *
 * @param[in]  cmd_id `k_ra_mipi_dsi_dt_gen_read_*` or
 *                    `k_ra_mipi_dsi_dt_dcs_read`.
 * @param[in]  vc     Virtual channel.
 * @param[in]  param0 First parameter byte.
 * @param[in]  param1 Second parameter byte.
 * @param[out] p_rx_buffer Buffer the IRQ handler will copy RXPPD into.
 * @param[in]  rx_len Maximum bytes the buffer can hold.
 *
 * @return `ra_err_t` outcome.
 * @retval k_ra_ok               Read kicked off.
 * @retval k_ra_err_null_ptr     `p_rx_buffer == nullptr`.
 * @retval k_ra_err_invalid_arg  `vc > 3` or `rx_len == 0`.
 * @retval k_ra_err_busy         Sequence engine running.
 *
 * @pre Driver initialised; PHY in BTA-capable state.
 * @pre `p_rx_buffer` lives until the IRQ fires.
 * @post Descriptor 0 of channel 0 holds a read header.
 * @post `SQCH0SET0R` pulsed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_read_packet(ra_mipi_dsi_dt_t cmd_id,
                                               ra_mipi_dsi_vc_t vc,
                                               uint8_t          param0,
                                               uint8_t          param1,
                                               uint8_t*         p_rx_buffer,
                                               uint16_t         rx_len);

/* =============================================================================
 * ULPS
 * =============================================================================
 */

/**
 * @brief Drive the selected lanes into Ultra-Low-Power State.
 *
 * @param[in] lanes OR of `k_ra_mipi_dsi_lane_clock` /
 *                  `k_ra_mipi_dsi_lane_data`.
 *
 * @return `k_ra_ok` on success; `k_ra_err_invalid_arg` if `lanes == 0`
 *         or the clock lane is requested while continuous-clock mode
 *         is on (HW prohibits it).
 *
 * @pre Driver initialised and HS clock currently running.
 * @pre `lanes` is a non-empty subset of valid lane bits.
 * @post `ULPSCR.CLENT` and/or `DLENT` were pulsed for selected lanes.
 * @post Subsequent `ra_mipi_dsi_send_*` is rejected until `_ulps_exit`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_ulps_enter(uint8_t lanes);

/**
 * @brief Wake the selected lanes out of ULPS.
 *
 * @param[in] lanes OR of `k_ra_mipi_dsi_lane_*` bits.
 *
 * @return `k_ra_ok` on success; `k_ra_err_invalid_arg` if `lanes == 0`.
 *
 * @pre At least one of the requested lanes is currently in ULPS
 *      (otherwise the exit pulse is a no-op).
 * @pre Driver initialised.
 * @post `ULPSCR.CLEXIT` and/or `DLEXIT` pulsed.
 * @post After the wake-up sequence completes (driver-tracked) the link
 *       is ready for HS / LP traffic again.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_ulps_exit(uint8_t lanes);

/* =============================================================================
 * Video mode
 * =============================================================================
 */

/**
 * @brief Programme every video-mode timing register from a single config.
 *
 * @details
 * Touches `VMSET1R`, `VMPPSETR`, `VMVSSETR`, `VMVPSETR`, `VMHSSETR`,
 * `VMHPSETR`. Does **not** assert `VMSET0R.VSTART` -- call
 * `ra_mipi_dsi_video_start()` for that.
 *
 * @param[in] vcfg Pointer to video-mode timing struct.
 *
 * @return `k_ra_ok` on success; `k_ra_err_null_ptr` if `vcfg == nullptr`,
 *         `k_ra_err_invalid_arg` if a field is out of range.
 *
 * @pre `vcfg` is non-NULL.
 * @pre All sync / porch / active-line counts fit into their HUM-defined
 *      bit widths.
 * @post Video timing registers loaded.
 * @post Subsequent `_video_start` will use these values.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_video_configure(const ra_mipi_dsi_video_cfg_t* vcfg);

/**
 * @brief Start video-mode operation.
 *
 * @details
 * Writes `VMSET0R` with the HSA/HBP/HFP no-LP bits and the VSTART bit.
 * Polls `VMSR.VIRDY` until set (bounded).
 *
 * @return `k_ra_ok` on success, `k_ra_err_hw_timeout` on poll timeout.
 *
 * @pre `_video_configure` has run since the last reset.
 * @pre HS clock running.
 * @post `LINKSR.VRUN` set.
 * @post Video pixel stream is being transmitted.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_video_start(const ra_mipi_dsi_video_cfg_t* vcfg);

/**
 * @brief Request a video-mode stop and wait for it.
 *
 * @details
 * Sets `VMSET0R.VSTOP`, polls `VMSR.STOP` until 1, then scrubs the
 * VMSCR latches. Equivalent to FSP `dsi_stop`.
 *
 * @return `k_ra_ok` on success, `k_ra_err_hw_timeout` if VMSR.STOP
 *         never asserts within the bounded loop.
 *
 * @pre Video mode is currently running (otherwise call is a no-op).
 * @pre Driver initialised.
 * @post `LINKSR.VRUN` clear.
 * @post `VMSR` cleared of stale flags.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_video_stop(void);

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

/**
 * @brief Read the top-level Interrupt Status Register.
 *
 * @param[out] out_mask Receives the raw ISR value.
 *
 * @return `k_ra_ok` / `k_ra_err_null_ptr`.
 *
 * @pre `out_mask` non-NULL.
 * @pre Driver init has run.
 * @post `*out_mask` reflects the current ISR snapshot.
 * @post No registers mutated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_get_status(uint32_t* out_mask);

/**
 * @brief Decode `LINKSR` into a struct.
 *
 * @param[out] out_status Decoded status.
 * @return `k_ra_ok` / `k_ra_err_null_ptr`.
 *
 * @pre `out_status` non-NULL.
 * @pre Driver initialised.
 * @post `*out_status` populated with the current LINKSR snapshot.
 * @post No registers mutated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_link_status_get(ra_mipi_dsi_link_status_t* out_status);

/**
 * @brief Clear bits in any of the sub-status registers.
 *
 * @details
 * Fans `mask` out across `SQCH0SCR`, `SQCH1SCR`, `VMSCR`, `RXSCR`,
 * `FERRSCR`, `PLSCR` so callers do not need to track which bit lives
 * where.
 *
 * @param[in] mask OR of `ra_mipi_dsi_isr_mask_t` bits to clear.
 * @return `k_ra_ok` always.
 *
 * @pre Driver init has run (or this is the bring-up path).
 * @pre `mask` only contains bits defined in `ra_mipi_dsi_isr_mask_t`.
 * @post Cleared sub-status registers read 0 for the requested bits.
 * @post Other status bits are preserved.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_clear_status(uint32_t mask);

/**
 * @brief Read the latest accumulated ack/error report and clear it.
 *
 * @param[out] out_err Decoded report.
 * @return `k_ra_ok` / `k_ra_err_null_ptr`.
 *
 * @pre `out_err` non-NULL.
 * @pre Driver initialised.
 * @post `AKEPACMSR` written to `AKEPSCR` (cleared in HW).
 * @post `*out_err` reflects the snapshot before clearing.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_ack_error_get(ra_mipi_dsi_ack_error_t* out_err);

/**
 * @brief Decode and consume an RXRSS slot.
 *
 * @param[in]  slot      0..3 -- which RXRSS register.
 * @param[out] out_result Decoded packet header.
 * @return `k_ra_ok` if the slot was valid, `k_ra_err_no_data` if the
 *         slot's RXRSSR.SLT*VLD bit was clear.
 *
 * @pre `out_result` non-NULL.
 * @pre `slot` in 0..3.
 * @post On success, slot's RXRSSR + RXRINFOOWSR valid bits cleared.
 * @post `*out_result` carries the decoded header.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_rx_result_get(uint8_t slot, ra_mipi_dsi_rx_result_t* out_result);

/**
 * @brief Copy out the most-recent long-packet RX payload (RXPPD0..3R).
 *
 * @param[out] dest    Caller buffer.
 * @param[in]  max_len Bytes available in `dest` (capped at 16).
 * @param[out] out_len Bytes actually read.
 * @return `k_ra_ok` / `k_ra_err_null_ptr`.
 *
 * @pre `dest` and `out_len` non-NULL.
 * @pre `max_len <= k_ra_mipi_dsi_payload_max`.
 * @post `dest[0..*out_len)` populated.
 * @post `*out_len` <= 16.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_mipi_dsi_rx_payload_read(uint8_t* dest, uint16_t max_len, uint16_t* out_len);

/**
 * @brief Check if a tearing-effect (TE) trigger was received.
 *
 * @param[out] out_pending Receives true if `RXSR.RXTE` (rx TE trigger)
 *                         or `RXSR.EXTEDET` (external TE pin) is set.
 * @return `k_ra_ok` / `k_ra_err_null_ptr`.
 *
 * @pre `out_pending` non-NULL.
 * @pre Driver initialised.
 * @post `*out_pending` reflects current RXSR snapshot.
 * @post No registers mutated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_te_event_pending(bool* out_pending);

/**
 * @brief Clear the latched tearing-effect bits in RXSR.
 *
 * @return `k_ra_ok`.
 *
 * @pre Driver initialised.
 * @pre Caller is in IRQ context or has IRQs masked.
 * @post `RXSR.RXTE / EXTEDET` clear.
 * @post Other RXSR bits preserved.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_te_event_clear(void);

/**
 * @brief Toggle a sub-class IER bit.
 *
 * @param[in] event  Which class (`k_ra_mipi_dsi_event_*`).
 * @param[in] mask   Bits inside the class IER register to update.
 * @param[in] enable true -> set selected bits, false -> clear.
 *
 * @return `k_ra_ok` / `k_ra_err_invalid_arg` for unknown class.
 *
 * @pre Driver initialised.
 * @pre `event` is a defined enumerator.
 * @post Selected IER bits updated.
 * @post Other IER bits preserved.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_mipi_dsi_irq_enable(ra_mipi_dsi_event_t event, uint32_t mask, bool enable);

/**
 * @brief Attach a callback for MIPI DSI interrupt events.
 *
 * @param[in] fn  Function to call (or `nullptr` to detach).
 * @param[in] ctx Opaque context passed back to `fn`.
 *
 * @return `k_ra_ok`.
 *
 * @pre Caller's `fn` is ISR-safe.
 * @pre Caller owns `ctx` lifetime for as long as `fn` is attached.
 * @post Subsequent `ra_mipi_dsi_dispatch*` calls invoke `fn`.
 * @post Detached state silently swallowed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_attach_handler(ra_mipi_dsi_event_fn_t fn, void* ctx);

/**
 * @brief Snapshot ISR, clear sub-status, fan out per-class callbacks.
 *
 * @details
 * Convenience trampoline that calls every per-class
 * `ra_mipi_dsi_dispatch_*` whose ISR bit is set. The platform's NVIC
 * stub may also call individual `_dispatch_*` directly when wired up
 * per-IRQ.
 *
 * @return None.
 * @retval None
 *
 * @pre Called either in IRQ context or with IRQs masked.
 * @pre Driver init has run.
 * @post `ISR` reads 0 for the bits that were set when the call began.
 * @post Per-class dispatch trampolines have each fired at most once.
 *
 * @note Not thread-safe; pair with NVIC masking. See HUM Ch 65
 *       "MIPI DSI Host" pp 3839-3934.
 * @since 0.1.0
 */
void ra_mipi_dsi_dispatch(void);

/** @brief Sequence-channel-0 IRQ handler (LP path).
 *  @details Snapshots SQCH0SR, clears the latched bits and invokes the
 *           registered seq0 callback. Safe to call from a spurious IRQ.
 *  @return None. @retval None
 *  @pre Driver init done. @pre IRQ context.
 *  @post `SQCH0SR` cleared. @post Callback invoked with seq0 event.
 *  @note ISR-context only; not thread-safe.
 *  @since 0.1.0 */
void ra_mipi_dsi_dispatch_seq0(void);

/** @brief Sequence-channel-1 IRQ handler (HS path).
 *  @details Snapshots SQCH1SR, clears the latched bits and invokes the
 *           registered seq1 callback. Safe to call from a spurious IRQ.
 *  @return None. @retval None
 *  @pre Driver init done. @pre IRQ context.
 *  @post `SQCH1SR` cleared. @post Callback invoked with seq1 event.
 *  @note ISR-context only; not thread-safe.
 *  @since 0.1.0 */
void ra_mipi_dsi_dispatch_seq1(void);

/** @brief Video-mode IRQ handler.
 *  @details Snapshots VMSR, clears the latched bits and invokes the
 *           registered video callback. Safe to call from a spurious IRQ.
 *  @return None. @retval None
 *  @pre Driver init done. @pre IRQ context.
 *  @post `VMSR` cleared. @post Callback invoked with video event.
 *  @note ISR-context only; not thread-safe.
 *  @since 0.1.0 */
void ra_mipi_dsi_dispatch_video(void);

/** @brief Receive IRQ handler.
 *  @details Snapshots RXSR, clears the latched bits and invokes the
 *           registered receive callback. Safe to call from a spurious IRQ.
 *  @return None. @retval None
 *  @pre Driver init done. @pre IRQ context.
 *  @post `RXSR` cleared. @post Callback invoked with receive event.
 *  @note ISR-context only; not thread-safe.
 *  @since 0.1.0 */
void ra_mipi_dsi_dispatch_receive(void);

/** @brief Fatal-error IRQ handler.
 *  @details Snapshots FERRSR, clears the latched bits and invokes the
 *           registered fatal callback. Safe to call from a spurious IRQ.
 *  @return None. @retval None
 *  @pre Driver init done. @pre IRQ context.
 *  @post `FERRSR` cleared. @post Callback invoked with fatal event.
 *  @note ISR-context only; not thread-safe.
 *  @since 0.1.0 */
void ra_mipi_dsi_dispatch_fatal(void);

/** @brief PHY-protocol-interface IRQ handler.
 *  @details Snapshots PLSR, clears the latched bits and invokes the
 *           registered phy callback. Safe to call from a spurious IRQ.
 *  @return None. @retval None
 *  @pre Driver init done. @pre IRQ context.
 *  @post `PLSR` cleared. @post Callback invoked with phy event.
 *  @note ISR-context only; not thread-safe.
 *  @since 0.1.0 */
void ra_mipi_dsi_dispatch_phy(void);

/* =============================================================================
 * Sweep 6 convenience surfaces
 * =============================================================================
 */

/**
 * @struct ra_mipi_dsi_video_timing_t
 * @brief Compact video-mode timing block (HSA/HBP/HACT/HFP + VSA/VBP/VACT/VFP).
 *
 * @details
 * Lighter-weight cousin of ::ra_mipi_dsi_video_cfg_t for callers that just
 * need to load timing numbers and accept the driver defaults for sync
 * polarity, pixel format (RGB888) and HSA/HBP/HFP-no-LP behaviour.
 * Maps onto VMVSSETR / VMVPSETR / VMHSSETR / VMHPSETR per HUM Ch 65
 * "MIPI DSI Host" pp 3839-3934.
 *
 * @invariant All eight counts fit into the bit widths of their target
 *            registers (sync = 12 bits, porch = 13 bits, active = 15 bits).
 *
 * @code
 * const ra_mipi_dsi_video_timing_t t = {
 *   .horizontal_sync   = 12,  .horizontal_back_porch = 64,
 *   .horizontal_active = 1024, .horizontal_front_porch = 32,
 *   .vertical_sync     = 4,   .vertical_back_porch    = 8,
 *   .vertical_active   = 600, .vertical_front_porch   = 6,
 * };
 * (void)ra_mipi_dsi_set_video_timing(&t);
 * @endcode
 *
 * @see ra_mipi_dsi_video_cfg_t for the full (polarity-aware) struct.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t horizontal_sync;        /**< HSA pixel count -> VMHSSETR.HSA.  */
  uint16_t horizontal_back_porch;  /**< HBP pixel count -> VMHPSETR.HBP.  */
  uint16_t horizontal_active;      /**< HACT pixel count -> VMHSSETR.HACT.*/
  uint16_t horizontal_front_porch; /**< HFP pixel count -> VMHPSETR.HFP.  */
  uint16_t vertical_sync;          /**< VSA line count  -> VMVSSETR.VSA.  */
  uint16_t vertical_back_porch;    /**< VBP line count  -> VMVPSETR.VBP.  */
  uint16_t vertical_active;        /**< VACT line count -> VMVSSETR.VACT. */
  uint16_t vertical_front_porch;   /**< VFP line count  -> VMVPSETR.VFP.  */
} ra_mipi_dsi_video_timing_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Load only the HSA/HBP/HACT/HFP/VSA/VBP/VACT/VFP video-timing
 *        registers from a compact descriptor.
 *
 * @details
 * Wraps ::ra_mipi_dsi_video_configure with sensible defaults
 * (RGB888 pixel format on VC0, sync-pulse mode off, HSA/HBP/HFP-no-LP
 * all set so the link stays HS through blanking). The remaining
 * VMSET1R / VMPPSETR fields keep their power-on defaults.
 *
 * @param[in] timing Compact timing descriptor.
 *
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok               Timing registers loaded.
 * @retval k_ra_err_null_ptr     `timing == nullptr`.
 * @retval k_ra_err_invalid_arg  A field is wider than the target register.
 *
 * @pre Driver initialised.
 * @pre `timing` is non-NULL.
 * @post Video-mode timing registers reflect the new values.
 * @post Other VM* registers preserve their power-on defaults.
 *
 * @note Not thread-safe. Call from single-threaded init or with IRQs masked.
 * @see ra_mipi_dsi_video_configure -- full-feature surface.
 * @see ra_mipi_dsi_video_start
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_set_video_timing(const ra_mipi_dsi_video_timing_t* timing);

/**
 * @brief Send a DSI command-mode short packet on VC0 (LP escape).
 *
 * @details
 * Convenience surface around ::ra_mipi_dsi_send_short_packet that
 * matches the FSP "command short" signature: a Data Type plus a
 * 2-byte parameter array. Always sent on virtual channel 0 in low
 * power escape mode. HUM Ch 65 "Command-mode short packet" p 3839+.
 *
 * @param[in] dt     One of the `k_ra_mipi_dsi_dt_*_short_*` constants.
 * @param[in] params 2-element array carrying parameter 0 / 1.
 *
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok                Packet queued.
 * @retval k_ra_err_null_ptr      `params == nullptr`.
 * @retval k_ra_err_busy          Sequence engine still running.
 * @retval k_ra_err_invalid_state Video mode running (LP rejected).
 *
 * @pre Driver initialised.
 * @pre `params` is non-NULL.
 * @post Sequence channel 0 descriptor 0 carries the assembled header.
 * @post `SQCH0SET0R` pulsed with START.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_send_command_short(ra_mipi_dsi_dt_t dt, const uint8_t params[2]);

/**
 * @brief Send a DSI command-mode long packet on VC0 (HS path).
 *
 * @details
 * Convenience surface around ::ra_mipi_dsi_send_long_packet -- always
 * uses VC0 and the high-speed sequence channel 1, which is what every
 * command-mode panel programming sequence wants. Use the lower-level
 * helper if you need LP routing or a non-zero VC. HUM Ch 65 "Long
 * packet write" p 3839+.
 *
 * @param[in] dt      `k_ra_mipi_dsi_dt_gen_long_write` or
 *                    `k_ra_mipi_dsi_dt_dcs_long_write`.
 * @param[in] payload Payload bytes (`len` bytes long).
 * @param[in] len     Payload length in bytes (max 1024 in HS mode).
 *
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok               Packet queued, START asserted.
 * @retval k_ra_err_null_ptr     `payload == nullptr` and `len > 0`.
 * @retval k_ra_err_invalid_arg  `len > 1024`.
 * @retval k_ra_err_busy         Sequence engine running.
 *
 * @pre Driver initialised.
 * @pre Either `len == 0` or `payload` valid.
 * @post Descriptor 0 of channel 1 carries the long-packet header.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_mipi_dsi_send_command_long(ra_mipi_dsi_dt_t dt, const uint8_t* payload, uint16_t len);

/**
 * @brief Send a DSI command-mode packet (short or long) on VC0 in LP escape.
 *
 * @details
 * Routes a command-mode payload through the LP-escape sequence
 * channel.  Short writes (``len <= 2``) use the short-packet path;
 * longer writes go through the long-packet path with the payload
 * staged into TXPPD0..3R.  Mirrors HUM Ch 65 "Command-mode packet
 * TX" pp 3839-3934.
 *
 * @param[in] packet_type DSI Data Type (see ``k_ra_mipi_dsi_dt_*``).
 * @param[in] payload     Payload bytes (``len`` bytes).
 * @param[in] len         Payload length in bytes (LP cap = 128).
 *
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok               Packet queued.
 * @retval k_ra_err_null_ptr     ``payload == nullptr`` and ``len > 0``.
 * @retval k_ra_err_invalid_arg  ``len > 128`` (LP cap).
 * @retval k_ra_err_busy         Sequence engine still running.
 *
 * @pre Driver initialised via ::ra_mipi_dsi_init.
 * @pre Either ``len == 0`` or ``payload`` is non-NULL.
 * @post Sequence channel 0 descriptor 0 carries the assembled header.
 * @post ``SQCH0SET0R`` pulsed with START.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_send_command_payload(ra_mipi_dsi_dt_t packet_type,
                                                        const uint8_t*   payload,
                                                        uint16_t         len);

/**
 * @brief Drive the entire link (clock + data lanes) into ULPS.
 *
 * @details
 * Convenience equivalent to ``ra_mipi_dsi_ulps_enter(k_ra_mipi_dsi_lane_all)``.
 * Honours the same continuous-clock-mode guard as the underlying call.
 *
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok              Both lanes pulsed into ULPS.
 * @retval k_ra_err_invalid_arg Continuous-clock mode active and clock
 *                              lane requested -- HW prohibits it.
 *
 * @pre Driver initialised, HS clock currently running.
 * @pre Continuous-clock mode is OFF.
 * @post ULPSCR.CLENT and DLENT have been pulsed.
 * @post Subsequent `ra_mipi_dsi_send_*` is rejected until ::ra_mipi_dsi_exit_ulps.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_enter_ulps(void);

/**
 * @brief Wake the entire link out of ULPS.
 *
 * @details
 * Convenience equivalent to ``ra_mipi_dsi_ulps_exit(k_ra_mipi_dsi_lane_all)``.
 *
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok Both lanes woken.
 *
 * @pre At least one of the lanes was previously placed in ULPS.
 * @pre Driver initialised.
 * @post ULPSCR.CLEXIT and DLEXIT pulsed.
 * @post Link is ready for HS / LP traffic again once the wake-up
 *       sequence completes.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_exit_ulps(void);

/**
 * @brief Snapshot the link state (running flags + busy flags + accumulated
 *        ack/error report) into a single struct.
 *
 * @details
 * Convenience around ::ra_mipi_dsi_link_status_get -- some callers want
 * the more direct "get_link_status" name from the FSP surface.
 *
 * @param[out] out Decoded link status.
 *
 * @return ::ra_err_t outcome.
 * @retval k_ra_ok           Status returned.
 * @retval k_ra_err_null_ptr `out == nullptr`.
 *
 * @pre `out` non-NULL.
 * @pre Driver initialised.
 * @post `*out` reflects the current LINKSR snapshot.
 * @post No registers mutated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_dsi_get_link_status(ra_mipi_dsi_link_status_t* out);

#ifdef __cplusplus
}
#endif
