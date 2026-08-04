/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_mipi_dsi_types.h
 * @brief MIPI DSI-2 host driver -- public types (enums / structs / typedefs)
 * @ingroup grp_hal_display
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Type-definition split of the MIPI DSI host public API (HUM Ch 65,
 * p 3839-3934). Carries the lane-count / clock-mode / virtual-channel /
 * data-type enumerations, the configuration + status + command + receive
 * structs, and the ISR-callback function-pointer typedef. Pulled out of
 * `ra8_mipi_dsi.h` so the umbrella stays a thin include shim; consumers
 * keep including `ra8_mipi_dsi.h` unchanged.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mipi_dsi_regs.h"

/**
 * @enum ra8_mipi_dsi_lane_count_t
 * @brief Number of D-PHY data lanes the DSI link drives.
 *
 * @details
 * HUM Ch 65.1 "Overview" p 3839 confirms the host supports 1- and
 * 2-lane configurations only; the package brings out two physical
 * data lanes plus one clock lane.
 */
typedef enum : uint8_t {
  k_ra8_mipi_dsi_lanes_1 = 1U, /**< 1 data lane + 1 clock lane.  */
  k_ra8_mipi_dsi_lanes_2 = 2U, /**< 2 data lanes + 1 clock lane. */
} ra8_mipi_dsi_lane_count_t;

/**
 * @enum ra8_mipi_dsi_clock_mode_t
 * @brief Continuous vs non-continuous HS clock (HSCLKSETR.HSCLMD).
 */
typedef enum : uint8_t {
  k_ra8_mipi_dsi_clock_non_continuous = 0U, /**< HS clock idles in LP.  */
  k_ra8_mipi_dsi_clock_continuous     = 1U, /**< HS clock runs forever. */
} ra8_mipi_dsi_clock_mode_t;

/**
 * @enum ra8_mipi_dsi_vc_t
 * @brief DSI virtual channel identifier (DT[7:6] in the packet header).
 *
 * @details
 * Only VC0 is wired up by the EK-RA8D2 panel; the other three are
 * reserved for multi-display use cases. HUM Ch 65 lists VC0..VC3 in
 * the descriptor word A bit layout.
 */
typedef enum : uint8_t {
  k_ra8_mipi_dsi_vc0 = 0U, /**< Virtual channel 0 (default panel). */
  k_ra8_mipi_dsi_vc1 = 1U, /**< Virtual channel 1.                 */
  k_ra8_mipi_dsi_vc2 = 2U, /**< Virtual channel 2.                 */
  k_ra8_mipi_dsi_vc3 = 3U, /**< Virtual channel 3.                 */
} ra8_mipi_dsi_vc_t;

/**
 * @enum ra8_mipi_dsi_dt_t
 * @brief Frequently used DSI Data Types (cmd id) for short and long packets.
 *
 * @details
 * Drawn from the MIPI DSI 1.1 specification table 7. Both short- and
 * long-packet entries are listed so the same enum can drive
 * `ra8_mipi_dsi_send_short_packet`, `_send_long_packet`, and the
 * video-mode pixel-stream selector.
 */
typedef enum : uint8_t {
  /* Short packet writes */
  k_ra8_mipi_dsi_dt_v_sync_start        = 0x01U, /**< Sync event: VSYNC start.    */
  k_ra8_mipi_dsi_dt_v_sync_end          = 0x11U, /**< Sync event: VSYNC end.      */
  k_ra8_mipi_dsi_dt_h_sync_start        = 0x21U, /**< Sync event: HSYNC start.    */
  k_ra8_mipi_dsi_dt_h_sync_end          = 0x31U, /**< Sync event: HSYNC end.      */
  k_ra8_mipi_dsi_dt_none                = 0x00U, /**< Reserved DT: "no packet".   */
  k_ra8_mipi_dsi_dt_color_mode_off      = 0x02U, /**< CM Off command.             */
  k_ra8_mipi_dsi_dt_color_mode_on       = 0x12U, /**< CM On command.              */
  k_ra8_mipi_dsi_dt_shutdown_peripheral = 0x22U, /**< Shut down peripheral.       */
  k_ra8_mipi_dsi_dt_turn_on_peripheral  = 0x32U, /**< Turn on peripheral.         */
  k_ra8_mipi_dsi_dt_compression_mode    = 0x07U, /**< Compression mode command.   */
  k_ra8_mipi_dsi_dt_end_of_transmission = 0x08U, /**< End of Transmission packet. */
  k_ra8_mipi_dsi_dt_dcs_short_write_0   = 0x05U, /**< DCS short write, 0 args.    */
  k_ra8_mipi_dsi_dt_dcs_short_write_1   = 0x15U, /**< DCS short write, 1 arg.     */
  k_ra8_mipi_dsi_dt_gen_short_write_0   = 0x03U, /**< Generic short, 0 args.      */
  k_ra8_mipi_dsi_dt_gen_short_write_1   = 0x13U, /**< Generic short, 1 arg.       */
  k_ra8_mipi_dsi_dt_gen_short_write_2   = 0x23U, /**< Generic short, 2 args.      */
  k_ra8_mipi_dsi_dt_gen_read_0          = 0x04U, /**< Generic read, 0 args.       */
  k_ra8_mipi_dsi_dt_gen_read_1          = 0x14U, /**< Generic read, 1 arg.        */
  k_ra8_mipi_dsi_dt_gen_read_2          = 0x24U, /**< Generic read, 2 args.       */
  k_ra8_mipi_dsi_dt_dcs_read            = 0x06U, /**< DCS read, 0 args.           */
  k_ra8_mipi_dsi_dt_execute_queue       = 0x16U, /**< Execute queue.              */
  k_ra8_mipi_dsi_dt_set_max_return_pkt  = 0x37U, /**< Set max return packet size. */
  /* Long packet writes */
  k_ra8_mipi_dsi_dt_null_packet     = 0x09U, /**< Null packet (long).     */
  k_ra8_mipi_dsi_dt_blanking_packet = 0x19U, /**< Blanking packet (long). */
  k_ra8_mipi_dsi_dt_gen_long_write  = 0x29U, /**< Generic long write.     */
  k_ra8_mipi_dsi_dt_dcs_long_write  = 0x39U, /**< DCS long write.         */
  /* Pixel stream long packets */
  k_ra8_mipi_dsi_dt_pixel_rgb565       = 0x0EU, /**< 16-bit RGB packed pixel.   */
  k_ra8_mipi_dsi_dt_pixel_rgb666       = 0x1EU, /**< 18-bit RGB packed pixel.   */
  k_ra8_mipi_dsi_dt_pixel_rgb666_loose = 0x2EU, /**< 18-bit RGB loosely packed. */
  k_ra8_mipi_dsi_dt_pixel_rgb888       = 0x3EU, /**< 24-bit RGB packed pixel.   */
} ra8_mipi_dsi_dt_t;

/**
 * @enum ra8_mipi_dsi_lane_sel_t
 * @brief Lane selector for ULPS enter / exit transitions.
 *
 * @details
 * Bits can be OR'd: for example `lane = k_ra8_mipi_dsi_lane_clock |
 * k_ra8_mipi_dsi_lane_data` puts the entire link into ULPS.
 */
typedef enum : uint8_t {
  k_ra8_mipi_dsi_lane_none  = 0U,      /**< No lanes selected.       */
  k_ra8_mipi_dsi_lane_clock = 1U << 0, /**< Clock lane.              */
  k_ra8_mipi_dsi_lane_data  = 1U << 1, /**< All data lanes (1 or 2). */
  k_ra8_mipi_dsi_lane_all   = (uint8_t)((1U << 0) | (1U << 1)),
  /**< Composite mask: clock + all data lanes. */
} ra8_mipi_dsi_lane_sel_t;

/**
 * @enum ra8_mipi_dsi_event_t
 * @brief Event class delivered to the user callback.
 *
 * @details
 * Mirrors the FSP `mipi_dsi_event_t` so application code that already
 * targets FSP can be ported with a 1:1 enum substitution.
 */
typedef enum : uint8_t {
  k_ra8_mipi_dsi_event_seq0    = 0U, /**< Sequence channel 0 (LP) finished. */
  k_ra8_mipi_dsi_event_seq1    = 1U, /**< Sequence channel 1 (HS) finished. */
  k_ra8_mipi_dsi_event_video   = 2U, /**< Video-mode status changed.        */
  k_ra8_mipi_dsi_event_receive = 3U, /**< Packet / response received.       */
  k_ra8_mipi_dsi_event_fatal   = 4U, /**< Fatal-error class IRQ.            */
  k_ra8_mipi_dsi_event_phy     = 5U, /**< PPI (PHY) lane event.             */
} ra8_mipi_dsi_event_t;

/**
 * @struct ra8_mipi_dsi_timing_t
 * @brief LP / HS guard-band timing values.
 *
 * @details
 * One-to-one mapping with `mipi_dsi_timing_t` from the FSP -- carries
 * the values that ultimately land in CLSTPTSETR and LPTRNSTSETR.
 *
 * cppcheck cannot see the test TU so it flags every member as unused;
 * each value is consumed by `ra8_mipi_dsi_init()` in
 * `libs/ra8_hal/src/ra8_mipi_dsi.c`.
 */
typedef struct {
  uint16_t clock_stop_time;       /**< CLSTPTSETR.CLKSTPT[11:2].  */
  uint8_t  clock_beforehand_time; /**< CLSTPTSETR.CLKBFHT[23:16]. */
  uint8_t  clock_keep_time;       /**< CLSTPTSETR.CLKKPT[31:24].  */
  uint16_t go_lp_and_back;        /**< LPTRNSTSETR.GOLPBKT[9:0].  */
} ra8_mipi_dsi_timing_t;

/**
 * @struct ra8_mipi_dsi_timeouts_t
 * @brief Bus timeout counters (HSTXTOSETR / LRXHTOSETR / TATOSETR /
 *        PRESPTO* registers).
 */
typedef struct {
  uint32_t hs_tx_timeout;      /**< HSTXTOSETR.                        */
  uint32_t lp_rx_host_timeout; /**< LRXHTOSETR.                        */
  uint32_t turnaround_timeout; /**< TATOSETR.                          */
  uint32_t bta_timeout;        /**< PRESPTOBTASETR.                    */
  uint32_t lp_rw_timeout;      /**< PRESPTOLPSETR (split LPRTO|LPWTO). */
  uint32_t hs_rw_timeout;      /**< PRESPTOHSSETR (split HSRTO|HSWTO). */
} ra8_mipi_dsi_timeouts_t;

/**
 * @struct ra8_mipi_dsi_video_cfg_t
 * @brief Video-mode timing + format + sync configuration.
 *
 * @details
 * Aggregates the inputs to VMSET0R / VMSET1R / VMPPSETR / VMVSSETR /
 * VMVPSETR / VMHSSETR / VMHPSETR. All counts are in panel pixels /
 * lines (the hardware multiplies by the per-lane byte clock as needed).
 */
typedef struct {
  ra8_mipi_dsi_dt_t pixel_format;             /**< RGB565 / RGB666 / RGB888.  */
  ra8_mipi_dsi_vc_t virtual_channel;          /**< VC for the pixel stream.   */
  bool              sync_pulse;               /**< Non-burst with sync pulse. */
  bool              hsa_no_lp;                /**< Stay HS during HSA period. */
  bool              hbp_no_lp;                /**< Stay HS during HBP period. */
  bool              hfp_no_lp;                /**< Stay HS during HFP period. */
  bool              vsync_active_high;        /**< VMVSSETR.VSPOL.            */
  bool              hsync_active_high;        /**< VMHSSETR.HSPOL.            */
  uint16_t          vertical_sync_lines;      /**< VMVSSETR.VSA[11:0].        */
  uint16_t          vertical_active_lines;    /**< VMVSSETR.VACT[30:16].      */
  uint16_t          vertical_back_porch;      /**< VMVPSETR.VBP[12:0].        */
  uint16_t          vertical_front_porch;     /**< VMVPSETR.VFP[28:16].       */
  uint16_t          horizontal_sync_lines;    /**< VMHSSETR.HSA[11:0].        */
  uint16_t          horizontal_active_pixels; /**< VMHSSETR.HACT[30:16].      */
  uint16_t          horizontal_back_porch;    /**< VMHPSETR.HBP[12:0].        */
  uint16_t          horizontal_front_porch;   /**< VMHPSETR.HFP[28:16].       */
  uint16_t          video_mode_delay;         /**< VMSET1R.DLY[13:2] x 4.     */
} ra8_mipi_dsi_video_cfg_t;

/**
 * @struct ra8_mipi_dsi_command_t
 * @brief Generic DSI packet descriptor for `ra8_mipi_dsi_send_command`.
 *
 * @details
 * Encapsulates everything the hardware needs to assemble a sequence-
 * channel descriptor. `tx_len > 2` triggers long-packet mode; otherwise
 * the bytes are packed into the descriptor header.
 */
typedef struct {
  ra8_mipi_dsi_dt_t  cmd_id;          /**< Data Type / command opcode.    */
  ra8_mipi_dsi_vc_t  virtual_channel; /**< VC[1:0] in the packet header.  */
  ra8_mipi_dsi_bta_t bta;             /**< BTA selector (none/after/...). */
  bool               low_power;       /**< true = LP escape (ch 0).       */
  bool               ack_request;     /**< Request ack from peripheral.   */
  bool               aux_operation;   /**< AUXOP descriptor word C.       */
  uint8_t            action_code;     /**< ACTCODE (only when aux_op).    */
  uint16_t           tx_len;          /**< Bytes in p_tx_buffer.          */
  const uint8_t*     p_tx_buffer;     /**< Source bytes for long packet.  */
  uint8_t*           p_rx_buffer;     /**< Sink bytes for read responses. */
} ra8_mipi_dsi_command_t;

/**
 * @struct ra8_mipi_dsi_rx_result_t
 * @brief Decoded view of a single RXRSS slot register.
 */
typedef struct {
  uint8_t           data[2];              /**< Header DATA0 / DATA1 bytes. */
  ra8_mipi_dsi_dt_t cmd_id;               /**< DT[5:0] received.           */
  ra8_mipi_dsi_vc_t virtual_channel;      /**< VC[1:0] received.           */
  bool              long_packet;          /**< FMT bit (1 = long packet).  */
  bool              rx_success;           /**< RXSUC flag.                 */
  bool              rx_fatal_error;       /**< RXFERR flag.                */
  bool              rx_fail;              /**< RXFAIL flag.                */
  bool              rx_packet_data_fail;  /**< RXPFAIL flag.               */
  bool              rx_correctable_error; /**< RXCERR flag.                */
  bool              rx_ack_and_error;     /**< RXAKE flag.                 */
  bool              info_overwritten;     /**< INFOOW flag.                */
} ra8_mipi_dsi_rx_result_t;

/**
 * @struct ra8_mipi_dsi_link_status_t
 * @brief Decoded view of LINKSR.
 */
typedef struct {
  bool sequence_ch0_running; /**< LINKSR.SQ0RUN. */
  bool sequence_ch1_running; /**< LINKSR.SQ1RUN. */
  bool video_running;        /**< LINKSR.VRUN.   */
  bool hs_busy;              /**< LINKSR.HSBUSY. */
  bool lp_busy;              /**< LINKSR.LPBUSY. */
} ra8_mipi_dsi_link_status_t;

/**
 * @struct ra8_mipi_dsi_ack_error_t
 * @brief Decoded view of AKEPLATIR / AKEPACMSR.
 */
typedef struct {
  uint16_t          error_report;    /**< 16-bit DSI ack/error bitmap. */
  ra8_mipi_dsi_vc_t virtual_channel; /**< VC tag of the report.        */
} ra8_mipi_dsi_ack_error_t;

/**
 * @struct ra8_mipi_dsi_config_t
 * @brief Static configuration passed to ::ra8_mipi_dsi_init.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read in `ra8_mipi_dsi_init()` in
 * `libs/ra8_hal/src/ra8_mipi_dsi.c`.
 *
 * @invariant `lane_count` is one of `k_ra8_mipi_dsi_lanes_1` or
 *            `k_ra8_mipi_dsi_lanes_2` -- any other value is rejected.
 */
typedef struct {
  ra8_mipi_dsi_lane_count_t lane_count;             /**< 1 or 2.          */
  ra8_mipi_dsi_clock_mode_t clock_mode;             /**< HS clock mode.   */
  uint16_t                  max_return_packet_size; /**< DSISETR.MRPSZ.   */
  uint8_t                   ulps_wakeup_period;     /**< ULPSSETR.WKUP.   */
  bool                      ecc_check_enable;       /**< DSISETR.ECCEN.   */
  bool                      eotp_enable;            /**< DSISETR.EOTPEN.  */
  bool                      scramble_enable;        /**< DSISETR.SCREN.   */
  bool                      tearing_detect_enable;  /**< DSISETR.EXTEMD.  */
  uint8_t                   crc_check_vc_mask;      /**< Bit i = VCi CRC. */
  ra8_mipi_dsi_timing_t     timing;                 /**< Guard timings.   */
  ra8_mipi_dsi_timeouts_t   timeouts;               /**< Bus timeouts.    */
} ra8_mipi_dsi_config_t;

/**
 * @typedef ra8_mipi_dsi_event_fn_t
 * @brief MIPI DSI ISR-context callback signature.
 *
 * @details
 * `status_mask` carries the snapshot of the ISR register at the moment
 * the dispatch routine ran. The driver clears the underlying sub-status
 * registers (SQCH0SCR, SQCH1SCR, VMSCR, RXSCR, FERRSCR, PLSCR) before
 * invoking the user callback so the application sees a coherent view.
 */
typedef void (*ra8_mipi_dsi_event_fn_t)(void*                ctx,
                                        ra8_mipi_dsi_event_t event,
                                        uint32_t             status_mask);
