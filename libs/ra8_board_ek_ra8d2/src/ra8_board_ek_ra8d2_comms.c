/**
 * @file ra8_board_ek_ra8d2_comms.c
 * @brief EK-RA8D2 BSP -- MIPI-DSI panel + J-Link VCOM serial bring-up
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Sibling translation unit of ``ra8_board_ek_ra8d2.c`` carrying the two
 * display/serial communication links the EK-RA8D2 exposes:
 *   - The J32 MIPI-DSI mezzanine (Renesas RTKMIPILCDB00000BE) panel
 *     bring-up (PHY -> DSI host -> HS clock start).
 *   - The on-board J-Link OB VCOM serial bridge over SCI8 (TXD8/RXD8).
 *
 * Like the primary unit, the BSP itself never touches MCU registers;
 * ``ra8_mipi_*`` / ``ra8_sci_*`` / ``ra8_pfs_route_peripheral`` carry every
 * register write. Source-of-truth for the pin tables is
 * ``docs/reference/ek-ra8d2-v1-users-manual.pdf`` (R20UT5523EG0101
 * Rev 1.01, October 2025).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_internal.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_mipi_dsi.h"
#include "ra8_mipi_phy.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_time_constants.h"

/**
 * @brief Initialize the board-standard PLL1 clock tree and publish its rates.
 * @details Delegates the register-level transition to the CGC HAL, then
 *          returns the board-qualified CPUCLK0 and PCLKA constants without
 *          exposing a concrete clock driver to application code.
 * @param[out] out_rates Initialized board clock rates.
 * @return Board clock initialization status.
 * @retval k_ra8_ok Clock initialization and rate publication succeeded.
 * @retval k_ra8_err_invalid_arg @p out_rates is null.
 * @retval other Propagated clock-generator initialization error.
 * @pre Called from the single-threaded board boot path.
 * @post On success, PLL1 supplies the standard EK-RA8D2 clock tree.
 * @post On failure, @p out_rates is unchanged when nonnull.
 * @note Not thread-safe; call once during board initialization.
 * @since 0.1.0
 */
ra8_err_t ra8_board_clocks_init(ra8_board_clock_rates_t* out_rates)
{
  if (out_rates == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t err = ra8_cgc_init();
  if (err != k_ra8_ok) {
    return err;
  }
  *out_rates = (ra8_board_clock_rates_t){
    .cpuclk0_hz = (uint32_t)k_ra8_cpuclk0_hz,
    .pclka_hz   = (uint32_t)k_ra8_pclka_hz,
  };
  return k_ra8_ok;
}

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

/* =============================================================================
 * 11. J-Link OB VCOM serial bridge (UM Table 13, page 24)
 * =============================================================================
 */

/**
 * @brief Tracks whether ``ra8_board_uart_console_init`` has succeeded.
 *
 * @details
 * Set to true after ``ra8_sci_init`` returns ok and the PFS routes are
 * programmed. The write/read helpers refuse to forward to ra8_sci when
 * this flag is false so callers cannot accidentally drive an
 * unconfigured channel.
 */
static bool s_uart_console_initialized = false;

/**
 * @brief Minimum SCI module operating clock accepted by the console init.
 *
 * @details
 * The RA8D2 SCI_B peripheral takes its baud-rate generator clock from
 * PCLKA (chip HUM Ch 38.2 "SCI registers" -- the SCI module operating
 * clock is the high-speed peripheral clock PCLKA, NOT PCLKB as some
 * lower-end RA chips). After ``ra8_cgc_init()`` brings PLL1 up,
 * PCLKA = PLL1P/8 = 125 MHz on this project's CGC tree
 * (``k_ra8_pclka_hz`` in ``ra8_time_constants.h``). Before that, the chip
 * sits on MOCO (~8 MHz), which is far too low for a usable 115200 baud
 * BRR -- so the console init refuses to come up until CGC has published
 * a post-PLL PCLKA value. This minimum is set well above MOCO and well
 * below the 125 MHz target so a different CGC tree can still bring the
 * console up as long as PCLKA is in a reasonable UART range.
 */
typedef enum : uint32_t {
  k_ra8_board_uart_console_min_pclka_hz =
    16000000UL, /**< RA8 board UART console minimum pclka Hz. */
} ra8_board_uart_console_clock_t;

ra8_err_t ra8_board_uart_console_init(uint32_t baud)
{
  if (baud == 0U) {
    return k_ra8_err_invalid_arg;
  }

  /* SCI8's baud generator is fed by PCLKA on RA8D2 (chip HUM Ch 38.2
   * "SCI registers"). Hardcoding a constant here was the bug behind
   * every garbled UART symptom on apps that retune CGC: the BRR was
   * being computed for an assumed 60 MHz against a real 125 MHz clock,
   * so the line rate came out 2x too fast. Read PCLKA from CGC at
   * runtime instead so the BRR tracks whatever the application's CGC
   * tree settled on. */
  uint32_t  pclka_hz = 0U;
  ra8_err_t err      = ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz);
  if (err != k_ra8_ok) {
    /* ra8_cgc_get_clock_hz with a valid clock-id and non-null out always returns k_ra8_ok. */
    return err; /* GCOVR_EXCL_LINE -- fixed PCLKA id and nonnull local satisfy get_clock_hz's only guards */
  }
  if (pclka_hz < (uint32_t)k_ra8_board_uart_console_min_pclka_hz) {
    return k_ra8_err_not_initialized;
  }

  /* Route PD02 -> TXD8, PD03 -> RXD8 (UM Table 13 p 24). PSEL value
   * k_ra8_psel_sci_async = 0x04 covers SCI async TXD/RXD per the chip
   * HUM Multiplexed Pin Function Selector. */
  err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_uart_console_pin_txd,
                                 k_ra8_psel_sci_async,
                                 "ra8_board.uart.console.txd");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_uart_console_pin_rxd,
                                 k_ra8_psel_sci_async,
                                 "ra8_board.uart.console.rxd");
  if (err != k_ra8_ok) {
    return err;
  }

  const ra8_sci_cfg_t cfg = {
    .baud      = baud,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  err = ra8_sci_init((uint8_t)k_ra8_board_uart_console_sci_channel, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  s_uart_console_initialized = true;
  return k_ra8_ok;
}

ra8_err_t ra8_board_uart_console_write(const uint8_t* data, size_t len)
{
  if (len == 0U) {
    return k_ra8_ok;
  }
  if (data == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_uart_console_initialized) {
    return k_ra8_err_not_initialized;
  }
  return ra8_sci_write_polling((uint8_t)k_ra8_board_uart_console_sci_channel, data, (uint32_t)len);
}

ra8_err_t ra8_board_uart_console_read(uint8_t* out, size_t cap, size_t* out_len)
{
  if (out_len == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_len = 0U;
  if (cap == 0U) {
    return k_ra8_ok;
  }
  if (out == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_uart_console_initialized) {
    return k_ra8_err_not_initialized;
  }

  /* Polled non-blocking drain: pull bytes while RDRF stays set, stop
   * the moment ra8_sci_getc_polling reports nothing available. The cap
   * bounds the loop (NASA Rule 2). */
  for (size_t i = 0U; i < cap; ++i) {
    uint8_t         byte = 0U;
    const ra8_err_t err =
      ra8_sci_getc_polling((uint8_t)k_ra8_board_uart_console_sci_channel, &byte);
    if (err != k_ra8_ok) {
      /* No byte available yet -- treat as a non-blocking stop. */
      return k_ra8_ok;
    }
    out[i] = byte;
    *out_len += 1U;
  }
  return k_ra8_ok;
}

/** @brief Implementation of `priv_ra8_board_uart_console_is_up()` -- reads the
 *         same flag the write / read / flush entry points below consult. */
RA8_PRIV bool priv_ra8_board_uart_console_is_up(void)
{
  return s_uart_console_initialized;
}

ra8_err_t ra8_board_uart_console_flush(void)
{
  if (!s_uart_console_initialized) {
    return k_ra8_err_not_initialized;
  }
  /* HUM Ch 38.2.17 "CSR : Common Status Register", p 2225 -- defer to
   * ra8_sci_flush which spins on CSR.TEND for the SCI8 channel that
   * backs this console. Lets panic_halt() drain the failure log before
   * WFI gates the SCI clock. */
  return ra8_sci_flush((uint8_t)k_ra8_board_uart_console_sci_channel);
}
