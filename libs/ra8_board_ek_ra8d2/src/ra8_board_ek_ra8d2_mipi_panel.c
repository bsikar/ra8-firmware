/**
 * @file ra8_board_ek_ra8d2_mipi_panel.c
 * @brief EK-RA8D2 BSP -- J32 MIPI-DSI panel bring-up
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Sibling translation unit of ``ra8_board_ek_ra8d2.c`` carrying the J32
 * MIPI-DSI mezzanine (Renesas RTKMIPILCDB00000BE) panel bring-up:
 * PHY -> DSI host -> HS clock start.
 *
 * This unit was the display half of ``ra8_board_ek_ra8d2_comms.c``. The
 * serial half of that file -- the J-Link OB VCOM console on SCI8 and the
 * board clock bring-up -- moved to Zig in #3003; what is left is one
 * purpose, so the file is named for it.
 *
 * Like the primary unit, the BSP itself never touches MCU registers;
 * ``ra8_mipi_*`` carries every register write. Source-of-truth for the pin
 * tables is ``docs/reference/ek-ra8d2-v1-users-manual.pdf`` (R20UT5523EG0101
 * Rev 1.01, October 2025).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_internal.h"
#include "ra8_err.h"
#include "ra8_mipi_dsi.h"
#include "ra8_mipi_phy.h"

/* =============================================================================
 * 10. MIPI-DSI panel bring-up (J32 -- Renesas RTKMIPILCDB00000BE mezzanine)
 * =============================================================================
 */

/**
 * @brief Static placeholder geometry + line rate for the J32 panel.
 *
 * @details
 * The Renesas MIPI Graphics Expansion Board (RTKMIPILCDB00000BE) carries a
 * Focus-LCD E45RA-MW276-C 480 x 854 panel driven over a 2-lane D-PHY link.
 * The exact per-lane bit rate is panel-vendor information; the placeholder
 * 480 Mbps/lane lands in the HAL's PMUL=1/4 band so the PLL coefficient
 * block below is at least self-consistent at compile time.
 *
 * TODO(panel-datasheet): replace these three values with the row from the
 * RTKMIPILCDB00000BE / Focus E45RA-MW276-C datasheet.
 */
typedef enum : uint16_t {
  k_ra8_board_mipi_panel_h_active       = 480U, /**< RA8 board mipi panel h active.       */
  k_ra8_board_mipi_panel_v_active       = 854U, /**< RA8 board mipi panel v active.       */
  k_ra8_board_mipi_panel_line_rate_mbps = 480U, /**< RA8 board mipi panel line rate mbps. */
} ra8_board_mipi_panel_geometry_t;

/**
 * @brief MIPI DSI host link-layer config for the J32 mezzanine.
 *
 * @details
 * Only fields whose values come from the SoC side (lane count, ECC / EoTP
 * defaults, ULPS wake-up) are filled in here. The guard-band timing block
 * and bus timeouts are left at the driver power-on defaults until the
 * panel datasheet pins down concrete numbers.
 *
 * TODO(panel-datasheet): populate ``timing`` (CLSTPTSETR / LPTRNSTSETR)
 * and ``timeouts`` (HSTXTOSETR, LRXHTOSETR, TATOSETR, PRESPTO*SETR) from
 * the panel datasheet -- the empty-init values below are accepted by the
 * driver but produce conservative blanking that may not meet the panel's
 * minimum HSA / HBP / HFP windows.
 */
static const ra8_mipi_dsi_config_t s_mipi_panel_cfg = {
  .lane_count             = k_ra8_mipi_dsi_lanes_2,
  .clock_mode             = k_ra8_mipi_dsi_clock_non_continuous,
  .max_return_packet_size = 16U,
  .ulps_wakeup_period     = 0U,
  .ecc_check_enable       = true,
  .eotp_enable            = true,
  .scramble_enable        = false,
  .tearing_detect_enable  = true,
  .crc_check_vc_mask      = 0x01U, /* VC0 only -- the only VC J32 wires up. */
  .timing                 = {},    /* TODO(panel-datasheet).                */
  .timeouts               = {},    /* TODO(panel-datasheet).                */
};

/**
 * @brief Placeholder D-PHY HS/LP transition timing block.
 *
 * @details
 * The HAL exposes ``ra8_mipi_phy_select_timing`` to look the right
 * DPHYTIM1..6 row up automatically; using it would be the right move once
 * the line rate is locked. The placeholder below carries a single non-zero
 * TINIT so the gap is obvious in a debugger.
 *
 * TODO(panel-datasheet): swap for a ``ra8_mipi_phy_select_timing`` lookup
 * keyed on the confirmed panel line rate.
 */
static const ra8_mipi_phy_timing_t s_mipi_phy_timing_placeholder = {
  .tinit = 1U,
};

/**
 * @brief MIPI D-PHY config for the J32 mezzanine.
 *
 * @details
 * PLL coefficients solve ``f = MOSC * (1/IDIV) * (NMUL+NFMUL) * (1/PMUL)``;
 * the placeholder values below assume MOSC=20 MHz and target 240 MHz PLL
 * out (480 Mbps/lane line rate, P=1/4 band).
 *
 * TODO(panel-datasheet): re-solve once the panel datasheet pins the line
 * rate down and the actual MOSC frequency on the EK-RA8D2 board is
 * confirmed; today's pclka_mhz=60 assumes the chip's CGC reset default.
 */
static const ra8_mipi_phy_config_t s_mipi_phy_cfg = {
  .mode           = k_ra8_mipi_phy_mode_dsi_host,
  .pclka_mhz      = k_panel_pclka_mhz,
  .line_rate_mbps = (uint16_t)k_ra8_board_mipi_panel_line_rate_mbps,
  .lane_count     = k_ra8_mipi_phy_lane_count_2,
  .clk_mode       = k_ra8_mipi_phy_clk_noncontinuous,
  .eotp           = k_ra8_mipi_phy_eotp_enabled,
  .pll =
    {
      .idiv     = k_ra8_mipi_phy_idiv_1,
      .pmul     = k_ra8_mipi_phy_pmul_4,
      .nfmul    = k_ra8_mipi_phy_nfmul_0_00,
      .nmul_int = k_panel_pll_nmul, /* TODO(panel-datasheet). */
    },
  .escdiv   = 0U,
  .p_timing = &s_mipi_phy_timing_placeholder,
};

ra8_err_t ra8_board_mipi_dsi_init(void)
{
  /* Step 1: PHY first -- HUM Ch 64.3.1 startup procedure. The HAL warns
   * "The MIPI PHY (HUM Ch 64) must be brought up first" before the DSI
   * host can clock its LP/HS lanes. */
  ra8_err_t err = ra8_mipi_phy_init(&s_mipi_phy_cfg);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Step 2: DSI host link layer (HUM Ch 65). Programmes TXSETR / DSISETR
   * / guard-band timing / timeouts -- but does NOT start the HS clock
   * yet, so the application can splice in the panel-side reset pulse on
   * k_ra8_board_mipi_dsi_reset_n (P606) and backlight enable on
   * k_ra8_board_mipi_dsi_backlight (P514) between init and clock start. */
  err = ra8_mipi_dsi_init(&s_mipi_panel_cfg);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Step 3: kick the differential HS clock. After this returns the link
   * is HS-ready; callers can replay the panel DCS init stream via
   * ra8_mipi_dsi_send_command() and finally call ra8_mipi_dsi_video_start.
   *
   * TODO(panel-datasheet): the per-panel DCS command sequence (sleep-out,
   * pixel-format set, display-on, etc.) for the Focus E45RA-MW276-C is
   * not committed here -- the application currently owns it. */
  return ra8_mipi_dsi_hs_clock_start();
}
