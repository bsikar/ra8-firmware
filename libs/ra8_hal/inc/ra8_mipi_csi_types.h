/**
 * @file ra8_mipi_csi_types.h
 * @brief MIPI CSI-2 receiver HAL driver -- public types
 * @ingroup grp_hal_camera
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public enums, configuration descriptor, callback typedefs, and
 * decoded-snapshot structs for the RA8D2 MIPI CSI-2 receiver HAL.
 * Split out of ``ra8_mipi_csi.h`` so the umbrella header stays under the
 * per-file line budget; consumers continue to include ``ra8_mipi_csi.h``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mipi_csi_regs.h"

/**
 * @enum ra8_mipi_csi_lanes_t
 * @brief Number of D-PHY data lanes the receiver should bring up.
 *
 * @details
 * Maps directly onto MCT0.VDLN[3:0]. The HUM (Ch 66.3.2 p 3936)
 * permits values 0x1 (one lane) and 0x2 (two lanes) only; any other
 * value is "Setting prohibited".
 */
typedef enum : uint8_t {
  k_ra8_mipi_csi_lanes_1 = 1U, /**< Single-lane D-PHY operation. */
  k_ra8_mipi_csi_lanes_2 = 2U, /**< Dual-lane D-PHY operation.   */
} ra8_mipi_csi_lanes_t;

/**
 * @struct ra8_mipi_csi_config_t
 * @brief Configuration descriptor for ``ra8_mipi_csi_init``.
 *
 * @details
 * Fields cover everything programmable in MCT0 / MCT2 / EMCT / EPCT
 * + the receive data-type filter masks. Per-VC, per-DL, PM, and
 * short-packet IRQ enables can be set via ``ra8_mipi_csi_init`` (the
 * matching arrays below) or applied later through the per-group
 * setters.  ``frrclk`` and ``frrskw`` are vclk/hsclk-derived rate-
 * adjust values described in HUM Ch 66.3.3 "MCT2 : Module Control
 * Register 2" p 3937; the caller is expected to compute them.
 */
typedef struct {
  /* MCT0 */
  ra8_mipi_csi_lanes_t lanes;        /**< 1 or 2 lanes (MCT0.VDLN).         */
  bool                 generic_rule; /**< MCT0.GRMD: must be 1 here.        */
  bool                 eccv13;       /**< MCT0.ECCV13 -- ECC v1.3 mode.     */
  bool                 lfsren;       /**< MCT0.LFSREN -- descrambling on.   */
  bool                 zlmd;         /**< MCT0.ZLMD -- zero-length LP.      */
  bool                 edmd;         /**< MCT0.EDMD -- ErrFrameData notify. */
  bool                 rvmd;         /**< MCT0.RVMD -- reserved-DT receive. */

  /* MCT2 */
  uint16_t frrclk; /**< MCT2.FRRCLK[8:0] rate. */
  uint16_t frrskw; /**< MCT2.FRRSKW[8:0] skew. */

  /* EPCT (EPD option control). All zero = EPD off. */
  bool     epd_enable;       /**< EPCT.EPDEN -- enable EPD.                 */
  bool     epd_option_2;     /**< EPCT.EPDOP -- 1 = Option 2 (recommended). */
  uint16_t epd_long_spacer;  /**< EPCT.SLP[14:0].                           */
  uint16_t epd_short_spacer; /**< EPCT.SSP[14:0].                           */

  /* EMCT (EPD misc options). */
  ra8_mipi_csi_vlsien_t vlsien;      /**< EMCT.VLSIEN[5:4] LRTE spacer. */
  bool                  eotp_enable; /**< EMCT.EOTPEN bit 6.            */

  /* DTEL / DTEH (data-type filter). 0 = accept reset defaults. */
  uint32_t dt_low_mask;  /**< DTEL value to programme. */
  uint32_t dt_high_mask; /**< DTEH value to programme. */

  /* RXIE / DLIEx / VCIE[16] / PMIE / GSIE -- masks for IRQ enable. */
  uint32_t rx_irq_mask;     /**< RXIE value.           */
  uint32_t dl_irq_mask[2];  /**< DLIE0 / DLIE1 values. */
  uint32_t vc_irq_mask[16]; /**< VCIE0..VCIE15 values. */
  uint32_t pm_irq_mask;     /**< PMIE value.           */
  uint32_t short_irq_mask;  /**< GSIE value.           */

  /* GSCT (short-packet FIFO control). */
  uint8_t short_threshold;    /**< GSCT.SHTH (threshold-1 in stages). */
  bool    short_store_enable; /**< GSCT.GFIF -- 1 = store in FIFO.    */
} ra8_mipi_csi_config_t;

/**
 * @typedef ra8_mipi_csi_event_fn_t
 * @brief MIPI CSI receive-status event callback.
 * @param[in] ctx       Caller context passed to ``attach_handler``.
 * @param[in] rxst_mask The RXST register value at dispatch time.
 */
typedef void (*ra8_mipi_csi_event_fn_t)(void* ctx, uint32_t rxst_mask);

/**
 * @typedef ra8_mipi_csi_dl_event_fn_t
 * @brief Data-lane IRQ callback.
 * @param[in] ctx     Caller context.
 * @param[in] lane    Data lane index (0..1) that fired.
 * @param[in] dlst    DLST(N) snapshot delivered to the callback.
 */
typedef void (*ra8_mipi_csi_dl_event_fn_t)(void* ctx, uint8_t lane, uint32_t dlst);

/**
 * @typedef ra8_mipi_csi_vc_event_fn_t
 * @brief Per-virtual-channel IRQ callback.
 * @param[in] ctx        Caller context.
 * @param[in] vc         Virtual channel index (0..15) or 0xFF if generic.
 * @param[in] vcst       VCST(M) snapshot for the channel; for generic
 *                       events (vc == 0xFF) only MLF + ECD bits are set.
 */
typedef void (*ra8_mipi_csi_vc_event_fn_t)(void* ctx, uint8_t vc, uint32_t vcst);

/**
 * @typedef ra8_mipi_csi_pm_event_fn_t
 * @brief Power-management IRQ callback.
 * @param[in] ctx      Caller context.
 * @param[in] pmst     PMST snapshot delivered to the callback.
 */
typedef void (*ra8_mipi_csi_pm_event_fn_t)(void* ctx, uint32_t pmst);

/**
 * @typedef ra8_mipi_csi_short_event_fn_t
 * @brief Generic-short-packet FIFO IRQ callback.
 * @param[in] ctx      Caller context.
 * @param[in] gsst     GSST snapshot delivered to the callback.
 */
typedef void (*ra8_mipi_csi_short_event_fn_t)(void* ctx, uint32_t gsst);

/**
 * @struct ra8_mipi_csi_short_packet_t
 * @brief One drained generic short-packet header.
 *
 * @details Decoded fields from a single GSHT read. Matches the
 *  format described in HUM Ch 66.3.28 p 3959.
 */
typedef struct {
  uint16_t payload;   /**< GSHT.SPDT[15:0]  -- 16-bit user data.            */
  uint8_t  data_type; /**< GSHT.DTYP[21:16] -- short-packet DT (0x08-0x0F). */
  uint8_t  vc;        /**< GSHT.SPVC[27:24] -- virtual channel.             */
  uint32_t raw;       /**< Unprocessed register snapshot.                   */
} ra8_mipi_csi_short_packet_t;

/**
 * @struct ra8_mipi_csi_module_info_t
 * @brief Decoded MCG (Module Configuration) RO snapshot.
 */
typedef struct {
  uint8_t  version;     /**< MCG.VER[3:0]    -- IP-core version.         */
  uint8_t  lanes_max;   /**< MCG.SDLN[11:8]  -- max supported lanes.     */
  uint8_t  fifo_stages; /**< MCG.GSNM[23:16] -- short-packet FIFO depth. */
  uint32_t raw;         /**< Unprocessed register snapshot.              */
} ra8_mipi_csi_module_info_t;

/**
 * @enum ra8_mipi_csi_data_format_t
 * @brief Per-virtual-channel payload format used by ::ra8_mipi_csi_set_data_format.
 *
 * @details
 * Maps onto the CSI-2 data-type byte (HUM Ch 66.3.10 "DTEL" p 3943
 * and 66.3.11 "DTEH" p 3944). Selecting a format on a VC sets the
 * matching bit in DTEL/DTEH so the receiver accepts that format
 * within the CSI-2 packet header.  Selecting ``k_ra8_mipi_csi_format_off``
 * removes the contribution that VC made, allowing graceful narrowing.
 */
typedef enum : uint8_t {
  k_ra8_mipi_csi_format_off       = 0U,    /**< Disable -- accept nothing for VC.  */
  k_ra8_mipi_csi_format_yuv422_8  = 0x1EU, /**< CSI-2 DT 0x1E -- YUV 4:2:2 8-bit.  */
  k_ra8_mipi_csi_format_yuv422_10 = 0x1FU, /**< CSI-2 DT 0x1F -- YUV 4:2:2 10-bit. */
  k_ra8_mipi_csi_format_rgb888    = 0x24U, /**< CSI-2 DT 0x24 -- RGB888.           */
  k_ra8_mipi_csi_format_raw8      = 0x2AU, /**< CSI-2 DT 0x2A -- RAW8.             */
  k_ra8_mipi_csi_format_raw10     = 0x2BU, /**< CSI-2 DT 0x2B -- RAW10 (FSP-only). */
  k_ra8_mipi_csi_format_yuv420    = 0x18U, /**< CSI-2 DT 0x18 -- YUV 4:2:0 8-bit.  */
} ra8_mipi_csi_data_format_t;

/**
 * @struct ra8_mipi_csi_error_report_t
 * @brief One ECC / CRC error notification from the receiver.
 *
 * @details
 * Decoded from VCST(M) (HUM Ch 66.3.18 p 3949) -- the same bits the
 * VC dispatcher already surfaces, but reshaped so callers do not have
 * to re-decode them. ``vc == 0xFF`` is used for the catch-all "MLF/ECD"
 * flags which apply to every channel.
 */
typedef struct {
  uint8_t  vc;                /**< Virtual channel that reported, or 0xFF.   */
  bool     ecc_corrected;     /**< VCST.ECC -- 1-bit ECC corrected.          */
  bool     ecc_two_bit_error; /**< VCST.ECD -- 2-bit ECC error.              */
  bool     crc_error;         /**< VCST.CRC -- payload CRC mismatch.         */
  uint32_t raw_vcst;          /**< Original VCST snapshot (bits as decoded). */
} ra8_mipi_csi_error_report_t;

/**
 * @typedef ra8_mipi_csi_error_fn_t
 * @brief Callback invoked when the receiver reports an ECC or CRC error.
 * @param[in] ctx    Caller context registered with ::ra8_mipi_csi_attach_error_handler.
 * @param[in] report Decoded error descriptor (only valid for the call).
 */
typedef void (*ra8_mipi_csi_error_fn_t)(void* ctx, const ra8_mipi_csi_error_report_t* report);
