/**
 * @file ra8_board_ek_ra8d2_ethernet.c
 * @brief EK-RA8D2 BSP -- on-board RGMII Ethernet (GPY111/PEF7071) bring-up
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Sibling translation unit of ``ra8_board_ek_ra8d2.c`` carrying the full
 * on-board Ethernet bring-up sequence: PHY hardware reset, RGMII pin
 * routing, ESWM/COMA clock + power gates, ETHA mode walk, RMAC profile
 * programming, and the MaxLinear GPY111 (Renesas "PEF7071") PHY chip-init
 * (soft-reset, RGMII RX skew, auto-negotiation restart). See UM Table 26
 * + 27 (page 33-34).
 *
 * The BSP itself never touches MCU registers except for the ESWM/COMA
 * agent windows; ``ra8_etha_*`` / ``ra8_rmac_*`` / ``ra8_cgc_*`` / ``ra8_mstp_*``
 * / ``ra8_pfs_route_peripheral`` carry the per-peripheral register writes.
 * Source-of-truth for the pin tables is
 * ``docs/reference/ek-ra8d2-v1-users-manual.pdf`` (R20UT5523EG0101
 * Rev 1.01, October 2025); register citations name the RA8D2 Hardware
 * User's Manual.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_etha.h"
#include "ra8_etha_regs.h"
#include "ra8_ether_regs.h"
#include "ra8_gpio_constants.h"
#include "ra8_hw_err.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_rmac.h"
#include "ra8_rmac_regs.h"

/* =============================================================================
 * 12. On-board RGMII Ethernet PHY (UM Table 26 + 27, page 33-34)
 * =============================================================================
 */

/**
 * @brief Pin table walked by ``ra8_board_ethernet_init``.
 *
 * @details
 * Every entry is one of the sixteen Ethernet signals from UM Table 26
 * p 33. All sixteen need ``k_ra8_psel_ether_rmii`` (the RA8D2 PSEL slot
 * 0x11 covers both RMII and RGMII -- the per-pin mux is the same; the
 * RMAC.MPIC.PIS field selects RGMII vs RMII for the data path).
 */
static const struct {
  ra8_board_eth_pin_t pin;   /**< Pin.   */
  const char*         owner; /**< Owner. */
} s_eth_routes[] = {
  {k_ra8_board_eth_pin_mdint, "ra8_board.eth.mdint"},
  {k_ra8_board_eth_pin_mdc, "ra8_board.eth.mdc"},
  {k_ra8_board_eth_pin_mdio, "ra8_board.eth.mdio"},
  {k_ra8_board_eth_pin_txd0, "ra8_board.eth.txd0"},
  {k_ra8_board_eth_pin_txd1, "ra8_board.eth.txd1"},
  {k_ra8_board_eth_pin_txd2, "ra8_board.eth.txd2"},
  {k_ra8_board_eth_pin_txd3, "ra8_board.eth.txd3"},
  {k_ra8_board_eth_pin_tx_ctl, "ra8_board.eth.tx_ctl"},
  {k_ra8_board_eth_pin_tx_clk, "ra8_board.eth.tx_clk"},
  {k_ra8_board_eth_pin_rxd0, "ra8_board.eth.rxd0"},
  {k_ra8_board_eth_pin_rxd1, "ra8_board.eth.rxd1"},
  {k_ra8_board_eth_pin_rxd2, "ra8_board.eth.rxd2"},
  {k_ra8_board_eth_pin_rxd3, "ra8_board.eth.rxd3"},
  {k_ra8_board_eth_pin_rx_ctl, "ra8_board.eth.rx_ctl"},
  {k_ra8_board_eth_pin_rx_clk, "ra8_board.eth.rx_clk"},
  {k_ra8_board_eth_pin_rstn, "ra8_board.eth.rstn"},
};

/**
 * @enum ra8_board_eth_phy_reset_cycles_t
 * @brief Cycle-counted PHY reset timings (Cortex-M85 @ ~1 GHz).
 *
 * @details
 * The PEF7071 datasheet sec 6.3 "Reset" requires:
 *   - RST_N held LOW for >= 10 ms (we use >= 15 ms).
 *   - Post-release wait >= 50 ms before any MDIO access (we use >= 60 ms).
 *
 * `ra8_delay_ms` is NOT usable here -- this function may run before
 * global IRQs are enabled (the typical app order is
 * setup_or_halt -> ra8_isr_globals_enable), and ra8_delay_ms is
 * WFI + SysTick, which hangs with IRQs masked. A `volatile nop`
 * loop is ~3 cycles/iter at 1 GHz so:
 *   - 5_000_000 iters >= 15 ms
 *   - 20_000_000 iters >= 60 ms
 */
typedef enum : uint32_t {
  k_ra8_board_eth_phy_rst_low_iters  = 5000000UL,  /**< >= 15 ms LOW.  */
  k_ra8_board_eth_phy_rst_post_iters = 20000000UL, /**< >= 60 ms HIGH. */
} ra8_board_eth_phy_reset_cycles_t;

/**
 * @brief Hardware-reset the on-board PEF7071 PHY via GPIO.
 *
 * @details
 * Routes the board's PHY_RSTN net (P708) as a plain GPIO output,
 * asserts LOW for the required hold time, releases HIGH, and leaves
 * the pin in GPIO mode driven HIGH for the rest of the application.
 * The ETHERC alternate mux does NOT source RSTN, so the pin MUST
 * stay a GPIO to keep the PHY out of reset.
 *
 * @return Result code.
 * @retval k_ra8_ok               PHY reset cycle completed.
 * @retval k_ra8_err_invalid_arg  GPIO init / write rejected.
 *
 * @pre RA8D2 IOPORT clock is on (chip-default reset state is fine).
 * @pre Caller serialises (single-threaded init context).
 * @post P708 is in GPIO-output mode driven HIGH.
 * @post >= 60 ms has elapsed since RST_N rising edge.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_eth_phy_hw_reset(void)
{
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- RA8_PIN()-packed board pin is valid ra8_port_pin_t data outside the enumerator list. */
  const ra8_port_pin_t rstn_pin = (ra8_port_pin_t)k_ra8_board_eth_pin_rstn;
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  ra8_err_t err = ra8_gpio_output_init(rstn_pin, k_ra8_level_low);
  if (err != k_ra8_ok) {
    return err;
  }
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_board_eth_phy_rst_low_iters; ++i) {
    __asm__ volatile("nop");
  }
  err = ra8_gpio_write(rstn_pin, k_ra8_level_high);
  if (err != k_ra8_ok) {
    /* ra8_gpio_write on a valid in-range mapped port/pin always returns k_ra8_ok off-target. */
    return err; /* GCOVR_EXCL_LINE */
  }
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_board_eth_phy_rst_post_iters; ++i) {
    __asm__ volatile("nop");
  }
  return k_ra8_ok;
}

/**
 * @brief Route the 15 non-RSTN ETHERC pins to their ETHERC alternate.
 *
 * @details
 * Walks `s_eth_routes` and assigns every pin EXCEPT RSTN to the RGMII
 * PSEL slot. The EK-RA8D2 wires its PEF7071 / GPY111 PHY in RGMII
 * mode (data pins: 4xTXD + 4xRXD + TXC + RXC + TX_CTL + RX_CTL plus
 * MDC / MDIO / MDINT / RSTN) -- the FSP config.xml for the canonical
 * ``ethernet_ek_ra8d2_ep`` example routes every Ethernet pin via the
 * ``eswm_rgmii1`` PSEL, which corresponds to ``k_ra8_psel_ether_rgmii``
 * (PSEL field value 0x18, comment "PDC / AGT1 / Ether RGMII" in
 * libs/ra8_hal/inc/ra8_mpc.h).
 *
 * After routing, the six RGMII *transmit* pins (TXD0..3, TX_CTL,
 * TX_CLK) get their PmnPFS.DSCR bumped from the reset-default low
 * drive to middle drive. HUM Ch 20.2.6 mandates DSCR = 01b for an
 * RGMII/3.3 V transmit pin; leaving it at 00b is an unsupported
 * combination -- bench-confirmed on EK-RA8D2 to corrupt ~70 % of
 * transmitted frames at 100 Mbps and 100 % at 1 Gbps (the receive
 * pins are inputs, so their drive strength is irrelevant and they
 * are left at the default). The on-board PEF7071 runs its MII rail
 * at 3.3 V (MIICTRL.V25_33 strap = 0), hence the 3.3 V row of the
 * HUM table -> ::k_ra8_pfs_dscr_middle.
 *
 * @return Result code from `ra8_pfs_route_peripheral`.
 * @retval k_ra8_ok               All 15 alt-function pins routed.
 * @retval k_ra8_err_invalid_arg  ra8_pfs_route_peripheral rejected one pin.
 *
 * @pre internal_eth_phy_hw_reset has run.
 * @pre PFS write-protect is unlocked by ra8_pfs_route_peripheral.
 * @post 15 pins (MDC/MDIO/TXDx/RXDx/TX_CTL/RX_CTL/TXCLK/RXCLK/MDINT)
 *       are routed to the RGMII ETHERC alternate.
 * @post The six RGMII transmit pins are at DSCR = middle drive.
 * @post RSTN stays a GPIO (never touched here).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_eth_route_alt_pins(void)
{
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- RA8_PIN()-packed board pin is valid ra8_port_pin_t data outside the enumerator list. */
  const ra8_port_pin_t rstn_pin = (ra8_port_pin_t)k_ra8_board_eth_pin_rstn;
  for (uint32_t i = 0U; i < sizeof(s_eth_routes) / sizeof(s_eth_routes[0]); ++i) {
    if ((ra8_port_pin_t)s_eth_routes[i].pin == rstn_pin) {
      continue;
    }
    const ra8_err_t err = ra8_pfs_route_peripheral((ra8_port_pin_t)s_eth_routes[i].pin,
                                                   k_ra8_psel_ether_rgmii,
                                                   s_eth_routes[i].owner);
    if (err != k_ra8_ok) {
      return err;
    }
  }

  /* RGMII transmit pins need DSCR = 01b middle drive strength. */
  /* HUM Ch 20.2.6 "PmnPFS" p 845 */
  static const uint16_t s_eth_tx_pins[] = {
    (uint16_t)k_ra8_board_eth_pin_txd0,
    (uint16_t)k_ra8_board_eth_pin_txd1,
    (uint16_t)k_ra8_board_eth_pin_txd2,
    (uint16_t)k_ra8_board_eth_pin_txd3,
    (uint16_t)k_ra8_board_eth_pin_tx_ctl,
    (uint16_t)k_ra8_board_eth_pin_tx_clk,
  };
  for (uint32_t i = 0U; i < sizeof(s_eth_tx_pins) / sizeof(s_eth_tx_pins[0]); ++i) {
    const ra8_err_t err =
      ra8_pfs_set_drive_strength((ra8_port_pin_t)s_eth_tx_pins[i], k_ra8_pfs_dscr_middle);
    if (err != k_ra8_ok) {
      /* ra8_pfs_set_drive_strength on valid mapped tx pins always returns k_ra8_ok off-target. */
      return err; /* GCOVR_EXCL_LINE */
    }
  }
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  return k_ra8_ok;
}

/**
 * @enum ra8_board_eth_coma_delay_t
 * @brief Busy-wait iteration counts for the COMA bring-up sequence.
 *
 * @details
 * Cortex-M85 at ~1 GHz, ~3 cycles per ``nop`` -> 3,000,000 iters lands
 * around 9 ms (FSP r_layer3_switch_reset_coma uses
 * ``R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS)`` for the
 * same purpose, so ~1 ms is sufficient -- we keep ~3 ms for margin).
 * 200_000 iters is the equivalent of the FSP 1-us settle delay used
 * between the ETHA EAMC writes. ``bpr_poll_max`` bounds the
 * CABPIRM.BPR poll: HUM Ch 31.3.2.7 says BPR sets at clk_period x 512
 * from the start of buffer-pool init -- well under a microsecond --
 * so 1,000,000 register-read iterations is a multi-millisecond
 * safety ceiling that satisfies NASA P10 Rule 2.
 */
typedef enum : uint32_t {
  k_ra8_board_eth_coma_delay_iters  = 3000000UL, /**< ~1-3 ms busy wait between COMA writes.  */
  k_ra8_board_eth_etha_step_iters   = 200000UL,  /**< ~50 us settle between ETHA mode writes. */
  k_ra8_board_eth_coma_bpr_poll_max = 1000000UL, /**< CABPIRM.BPR poll upper bound.           */
} ra8_board_eth_coma_delay_t;

/**
 * @brief Bring the COMA (Common Agent) IP out of reset, initialise the
 *        buffer pool, and enable per-agent clock fan-out.
 *
 * @details
 * Faithful port of FSP r_layer3_switch_reset_coma. Two effects are
 * essential and were both bench-confirmed on EK-RA8D2:
 *  - Without the switch + agent clocks the per-port RMAC / ETHA
 *    register windows read back 0 and writes are silently dropped.
 *  - Without the CABPIRM.BPIOG -> BPR buffer-pool init the shared
 *    MFAB pointer pool stays non-operational: the RMAC cannot obtain
 *    a buffer for an inbound frame, so every RX frame overflows
 *    (MEIS.REOES, MROVFC climbs) and MRGFCE stays 0.
 *
 * Sequence (HUM Ch 31.4 software flows):
 *   1. Pulse COMA.RRC.RR (1 then 0): reset the ESWM IP.
 *   2. Set COMA.RCEC.RCE: enable the switch clock alone.
 *   3. Write COMA.CABPIRM.BPIOG = 1, poll until CABPIRM.BPR == 1.
 *   4. Set COMA.RCEC = RCE | ACE[6:0]: fan every per-agent clock out.
 *
 * @return Result code.
 * @retval k_ra8_ok             COMA out of reset, buffer pool ready.
 * @retval k_ra8_err_hw_timeout CABPIRM.BPR never asserted.
 *
 * @pre ESWM MSTP gates released (MSTPC30 + MSTPC28).
 * @pre PDCTRESWM.PDDE = 0 (peripheral domain powered).
 * @post COMA.RCEC.RCE = 1, ACE[6:0] = all-ones, CABPIRM.BPR = 1.
 * @post Per-port RMAC / ETHA register windows are accessible.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_eth_coma_reset(void)
{
  /* Faithful port of FSP r_layer3_switch_reset_coma. */

  /* Step 1: RRC pulse + ~1 ms delay. */
  *ra8_coma_rrc() = (uint32_t)k_ra8_coma_rrc_rr;
  *ra8_coma_rrc() = 0U;
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_board_eth_coma_delay_iters; ++i) {
    __asm__ volatile("nop");
  }

  /* Step 2: enable the switch clock (RCE) alone, then settle. */
  *ra8_coma_rcec() = (uint32_t)k_ra8_coma_rcec_rce;
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_board_eth_coma_delay_iters; ++i) {
    __asm__ volatile("nop");
  }

  /* Step 3: kick the COMA buffer-pool init and wait for BPR.
   * HUM Ch 31.3.2.7 "CABPIRM" p 1599: writing BPIOG=1 starts the pool
   * reset; BPR self-sets clk_period x 512 later. On the host build the
   * CABPIRM register is mmap'd RAM with no hardware to self-set BPR, so
   * the shared bounded waiter consults the ra8_fake_mmio fault seam --
   * first-poll success unless a test arms a timeout on this register. */
  *ra8_coma_cabpirm() = (uint32_t)k_ra8_coma_cabpirm_bpiog;
  /* HUM Ch 31.3.2.7 "CABPIRM" p 1599 */
  const ra8_err_t bpr_err = ra8_hw_wait_flag_set32(ra8_coma_cabpirm(),
                                                   (uint32_t)k_ra8_coma_cabpirm_bpr,
                                                   (uint32_t)k_ra8_board_eth_coma_bpr_poll_max);
  if (bpr_err != k_ra8_ok) {
    return bpr_err;
  }

  /* Step 4: fan out every per-agent clock (RCE | ACE[6:0]=ALL) so
   * RMAC0/1 + ETHA0/1 + GWCA + MFWD + GPTP all become accessible. */
  *ra8_coma_rcec() = (uint32_t)k_ra8_coma_rcec_rce | (uint32_t)k_ra8_coma_rcec_ace_mask;
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_board_eth_coma_delay_iters; ++i) {
    __asm__ volatile("nop");
  }
  return k_ra8_ok;
}

/**
 * @brief Programme ESWM.MIICR1 + MIIRR for RGMII operation on port 1.
 *
 * @details
 * The EK-RA8D2 wires its PEF7071 / GPY111 PHY to RMAC port **1** -- the
 * canonical FSP example project ``ethernet_ek_ra8d2_ep`` confirms this:
 * every Ethernet pin is configured as ``eswm_rgmii1`` (note the trailing
 * "1") in the pincfg, and the r_rmac module is instantiated on
 * Channel 1 (ra8_cfg.txt "Channel: 1"). RMAC0 / ETHA0 is left unused on
 * this evaluation board.
 *
 * The Ethernet Switch Module wraps the per-port MAC pins with a media-
 * interface multiplexer. After power-on reset MIIRR.RGRST1 reads 0
 * (RGMII1 block held in reset) and MIICR1.MIISEL reads 0 (GMII/MII),
 * so the RGMII data path is dead until the driver explicitly:
 *
 *   1. Sets MIICR1.MIISEL = 1 (RGMII) plus TXCIDE = 1 (on-chip TX
 *      delay), matching the FSP "RGMII + 2 ns TX skew" board profile.
 *   2. Sets MIIRR.RGRST1 = 1 (HUM 29.2.1.2: 1 = Enable, 0 = Reset --
 *      this is an enable bit, not an active-high reset).
 *
 * Without the RGRST1 enable, TXC is never generated and the RMAC RX
 * state machine is unclocked -- every RMAC RX counter stays at 0.
 *
 * @return ::k_ra8_ok. Two MMIO writes; no failure paths.
 * @retval k_ra8_ok Always returned -- no error path.
 * @pre  ESWM module-stop has been released (the RMAC and ESWM share
 *       MSTPCRC.MSTPCESWM; ra8_rmac_init takes the ref-count up).
 * @pre  COMA has been brought out of reset (internal_eth_coma_reset).
 * @post MIICR1 = TXCIDE | RGMII, MIIRR.RGRST1 = 1 (RGMII1 enabled).
 * @post RMAC1 / ETHA1 data pins ready to clock.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_eth_eswm_select_rgmii(void)
{
  /* HUM Ch 29 "ESWM" + FSP r_rmac_phy_set_mii_type_configuration:
   * write MIICR before enabling the per-port RGMII block so the pin
   * mux is in the correct mode the instant the data pins go live.
   * MIICR / MIIRR live at the +0x19400 sub-block of the ESWM window
   * (see ra8_ether_regs.h ra8_eswm_off_miicr1 / ra8_eswm_off_miirr). */
  *ra8_eswm_miicr1() = (uint32_t)k_ra8_eswm_miicr_txcide | (uint32_t)k_ra8_eswm_miicr_miisel_rgmii;

  /* HUM Ch 29.2.1.2 "MIIRR : Media-independent Interface Reset
   * Register" p 1289: RGRST1 is **0 = Reset, 1 = Enable** -- it is an
   * enable, not an active-high reset. Bench-confirmed on EK-RA8D2:
   * with RGRST1 = 0 the RGMII1 block stays in reset, TXC is never
   * generated, the RMAC RX state machine is unclocked, and every
   * RMAC RX counter (MRFC, MRGFCE ...) stays at 0. FSP's
   * r_rmac_phy_set_mii_type_configuration sets this bit with the
   * comment "Enable TXC generation". SET it. */
  uint32_t miirr = *ra8_eswm_miirr();
  miirr |= (uint32_t)k_ra8_eswm_miirr_rgrst1;
  *ra8_eswm_miirr() = miirr;
  return k_ra8_ok;
}

/**
 * @brief Bring the ESWM subsystem live: clocks + power + MSTP + COMA reset.
 *
 * @details
 * Factored out of ::ra8_board_ethernet_init to keep that function under
 * the project's NASA Rule 4 / clang-tidy function-size threshold. Runs
 * the four upstream gates the RMAC / ETHA register windows hang off:
 * ESWCLK / ESWPHYCLK, PDCTRESWM power-on, MSTPC30 (ESWM) MSTP release,
 * MSTPC28 (ETHPHYCLK) MSTP release, and finally the COMA RRC/RCEC
 * bring-up. ``out_eswclk_hz`` receives the live ESWCLK frequency so
 * the caller can plumb it into ``ra8_rmac_config_t``.
 *
 * @param[out] out_eswclk_hz Live ESWCLK frequency in Hz on success.
 *
 * @return Error code from the underlying CGC / MSTP / COMA paths.
 * @retval k_ra8_ok Bring-up sequence completed.
 * @retval k_ra8_err_hw_timeout Sub-step (eswclk, MSTP) timed out.
 *
 * @pre  ``out_eswclk_hz`` is a valid 32-bit pointer.
 * @pre  Single-threaded init context.
 * @post On k_ra8_ok, ESWM is fully clocked + COMA is out of reset.
 * @post *out_eswclk_hz holds the live ESWCLK frequency.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_eth_eswm_bring_up(uint32_t* out_eswclk_hz)
{
  ra8_err_t err = ra8_cgc_eswclk_init();
  if (err != k_ra8_ok) {
    /* ra8_cgc_eswclk_init always returns k_ra8_ok in RA8_OFF_TARGET (polls auto-satisfied). */
    return err; /* GCOVR_EXCL_LINE */
  }
  err = ra8_cgc_eswclk_hz(out_eswclk_hz);
  if (err != k_ra8_ok) {
    /* ra8_cgc_eswclk_hz only fails on nullptr; out_eswclk_hz is always a valid pointer here. */
    return err; /* GCOVR_EXCL_LINE */
  }
  err = ra8_mstp_enable(k_ra8_mstp_eswm);
  if (err != k_ra8_ok) {
    /* ra8_mstp_enable returns k_ra8_ok in RA8_OFF_TARGET (readback poll excluded on host). */
    return err; /* GCOVR_EXCL_LINE */
  }
  return internal_eth_coma_reset();
}

/**
 * @brief Walk ETHA1 from RESET through DISABLE to CONFIG.
 *
 * @details
 * Per FSP r_rmac_phy_open, RMAC.MPIC is only writable while the paired
 * ETHA is in CONFIG mode (writes are silently dropped in other modes).
 * This helper drives ETHA into CONFIG so ``ra8_rmac_init`` can land the
 * MPIC programming; ``internal_eth_etha_to_operation`` flips to
 * OPERATION afterwards so MDIO traffic can start.
 *
 * @return k_ra8_ok on success; otherwise propagates ra8_etha_set_mode.
 * @retval k_ra8_ok Walked to CONFIG.
 * @retval k_ra8_err_invalid_arg ra8_etha_set_mode rejected an argument.
 *
 * @pre  ``internal_eth_eswm_bring_up`` has succeeded.
 * @pre  Single-threaded init context.
 * @post ETHA1 EAMC reflects CONFIG; busy-wait between writes mirrors
 *       FSP r_rmac_phy_set_operation_mode.
 * @post EAMS.OPS == CONFIG (best-effort; FSP does not poll either).
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_eth_etha_to_config(void)
{
  const ra8_etha_config_t etha_cfg = {
    .initial_mode = k_ra8_etha_opc_reset,
    .eaeie0_mask  = 0U,
    .eaeie1_mask  = 0U,
    .eaeie2_mask  = 0U,
  };
  ra8_err_t err = ra8_etha_init((ra8_etha_port_t)k_ra8_board_eth_etha_port, &etha_cfg);
  if (err != k_ra8_ok) {
    /* ra8_etha_init returns k_ra8_ok with a valid port (1) and non-null cfg off-target. */
    return err; /* GCOVR_EXCL_LINE */
  }
  static const ra8_etha_opc_t s_chain[] = {
    k_ra8_etha_opc_disable,
    k_ra8_etha_opc_config,
  };
  for (uint8_t step = 0U; step < (uint8_t)(sizeof(s_chain) / sizeof(s_chain[0])); ++step) {
    err = ra8_etha_set_mode((ra8_etha_port_t)k_ra8_board_eth_etha_port, s_chain[step]);
    if (err != k_ra8_ok) {
      /* ra8_etha_set_mode returns k_ra8_ok in RA8_OFF_TARGET with a valid port and mode. */
      return err; /* GCOVR_EXCL_LINE */
    }
    for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_board_eth_etha_step_iters; ++i) {
      __asm__ volatile("nop");
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Promote ETHA1 from CONFIG to OPERATION (post-MPIC programming).
 *
 * @details
 * MDIO transactions require ETHA in OPERATION mode (FSP
 * r_rmac_phy_get_operation_mode invariant). Run AFTER ra8_rmac_init has
 * programmed MPIC, never before -- MPIC writes are silently dropped in
 * any mode other than CONFIG.
 *
 * @return k_ra8_ok on success; otherwise propagates ra8_etha_set_mode.
 * @retval k_ra8_ok ETHA promoted to OPERATION.
 * @retval k_ra8_err_invalid_arg ra8_etha_set_mode rejected an argument.
 *
 * @pre  ra8_rmac_init has programmed RMAC1 MPIC.
 * @pre  Single-threaded init context.
 * @post ETHA1 EAMC = OPERATION; subsequent ra8_rmac_mdio_c22_read works.
 * @post Busy-wait gives the EAMC write time to take effect.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_eth_etha_to_operation(void)
{
  ra8_err_t err =
    ra8_etha_set_mode((ra8_etha_port_t)k_ra8_board_eth_etha_port, k_ra8_etha_opc_operation);
  if (err != k_ra8_ok) {
    return err;
  }
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_board_eth_etha_step_iters; ++i) {
    __asm__ volatile("nop");
  }
  return k_ra8_ok;
}

/**
 * @brief Programme RMAC1 with the RGMII / 1Gb full-duplex profile.
 *
 * @details
 * ``ra8_rmac_init`` is the API that lands MPIC; this helper builds the
 * RGMII config struct and hands it off so the bring-up function below
 * stays under the function-size threshold.
 *
 * @param[in] eswclk_hz Live ESWCLK frequency in Hz (from
 *                      ::internal_eth_eswm_bring_up).
 * @return k_ra8_ok on success; otherwise propagates ra8_rmac_init.
 * @retval k_ra8_ok RMAC1 initialised.
 * @retval k_ra8_err_invalid_arg ra8_rmac_init rejected the cfg block.
 *
 * @pre  ETHA1 is in CONFIG mode (::internal_eth_etha_to_config has run).
 * @pre  ``eswclk_hz`` > 0.
 * @post RMAC1 MPIC programmed for RGMII / 1Gb / full / 1 MHz MDC.
 * @post MAC address filter set to unicast + broadcast accept.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_eth_rmac_program(uint32_t eswclk_hz)
{
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- OR-combined MRAFC flags and the board RMAC port are valid values outside the enumerator lists. */
  /* HUM Ch 33.4 "MRAFC : MAC Reception Address Filter Configuration
   * Register" p 1707: each frame class has an ENABLE bit (UCENE/BCENE
   * /MCENE in the [10:0] half + matching UCENP/BCENP/MCENP in the
   * [26:16] half) AND a separate ACCEPT bit (BCACE bit 6, MCACE bit 5)
   * that controls whether the MAC actually forwards the matched frame
   * to the descriptor ring. Without BCACE the chip latches the ARP
   * broadcast in the perfect-match comparator and then drops it before
   * the GWCA agent ever sees it, so the host's ARP responder never
   * sees a "who-has 192.168.1.42" request and never replies. FSP
   * r_rmac.c::rmac_configure_reception_filter ORs both bits into the
   * canonical promiscuous mask for the same reason. */
  /* HUM Table 29.11: for an external RGMII link the RMAC's internal
   * xMII (MPIC.PIS) is MII for 10/100 Mbps and GMII for 1 Gbps -- the
   * ESWM media mux (MIICR1.MIISEL = 01b) does the RGMII conversion.
   * Default to MII (10/100 Mbps) at init so the on-chip RGMII RX
   * sample timing matches the most common PHY auto-neg outcome on
   * this board. Issue #1 bench: starting in GMII (1 Gbps) silently
   * dropped every RX frame when the PHY negotiated to 100 Mbps with
   * the Pi USB-Ethernet adapter, and the link-up-gated resync inside
   * ra8_eth_open never fired because chip-side MDIO reads of BMSR
   * return 0x0000 (the PHY's BMSR.link bit is invisible until skew
   * + clock are correct). Starting in MII makes RX work for any
   * 10/100 link the PHY auto-negotiates; for 1 Gbps links the
   * resync inside ra8_eth_link_status will promote PIS to GMII once
   * MDIO becomes usable. */
  const ra8_rmac_config_t rmac_cfg = {
    .rx_filter = (ra8_rmac_mrafc_t)(k_ra8_rmac_mrafc_unicast_match | k_ra8_rmac_mrafc_broadcast |
                                    k_ra8_rmac_mrafc_bc_accept),
    .err_irq_enable  = 0U,
    .mon0_irq_enable = 0U,
    .mon1_irq_enable = 0U,
    .mon2_irq_enable = 0U,
    .phy_interface   = k_ra8_rmac_pis_mii,
    .link_speed      = k_ra8_rmac_lsc_100mbit,
    .duplex          = k_ra8_rmac_duplex_full,
    .eswclk_hz       = eswclk_hz,
    .mdc_hz          = (uint32_t)k_ra8_rmac_mdc_default_hz,
  };
  return ra8_rmac_init((ra8_rmac_port_t)k_ra8_board_eth_rmac_port, &rmac_cfg);
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
}

/**
 * @enum ra8_board_eth_phy_reg_t
 * @brief GPY111 PHY register addresses + bit constants for chip-init.
 *
 * @details
 * The EK-RA8D2's on-board MaxLinear GPY111 PHY (Renesas marking
 * "PEF7071") is brought up by mirroring FSP's R_RMAC_PHY_ChipInit +
 * R_RMAC_PHY_StartAutoNegotiate: soft-reset the PHY, set the RGMII
 * receive-timing skew, program the auto-negotiation advertisement,
 * then restart auto-negotiation so the link re-converges with the
 * skew applied. The RA8D2 ESWM MIICR1 can add only a *transmit*
 * clock delay (TXCIDE); the RGMII receive delay MUST come from the
 * PHY (MIICTRL.RXSKEW).
 */
typedef enum : uint16_t {
  k_ra8_board_eth_phy_reg_bmcr     = 0U,      /**< BMCR (basic control).         */
  k_ra8_board_eth_phy_reg_anar     = 4U,      /**< Auto-neg advertisement.       */
  k_ra8_board_eth_phy_reg_gbcr     = 9U,      /**< 1000BASE-T control.           */
  k_ra8_board_eth_phy_reg_miictrl  = 0x17U,   /**< GPY111 MIICTRL vendor reg.    */
  k_ra8_board_eth_phy_bmcr_reset   = 0x8000U, /**< BMCR.RESET (self-clearing).   */
  k_ra8_board_eth_phy_bmcr_an_en   = 0x1000U, /**< BMCR.AUTONEG_ENABLE.          */
  k_ra8_board_eth_phy_bmcr_an_rst  = 0x0200U, /**< BMCR.AUTONEG_RESTART.         */
  k_ra8_board_eth_phy_rxskew_mask  = 0x7000U, /**< MIICTRL RXSKEW field [14:12]. */
  k_ra8_board_eth_phy_rxskew_1p0ns = 0x2000U, /**< RXSKEW = 0b010 -> 1.0 ns.     */
  k_ra8_board_eth_phy_anar_value   = 0x01E1U, /**< Advertise 100F/100H/10F/10H.  */
  /* GBCR = 0: 1000BASE-T is not advertised, so the link settles at
   * 100M full-duplex where it runs at 0 % loss. At 1 Gbps the RGMII
   * TXC is 125 MHz DDR and the clock-to-data window shrinks to ~1 ns;
   * bench testing shows 100 % TX loss at gigabit even with the TX
   * pins at high drive (DSCR = 11b). The remaining skew lives in the
   * PEF7071's pinstrapped RGMII delay (no EEPROM is fitted, so the
   * PHY's MIICTRL skew fields are read-only) and the fixed MAC-side
   * MIICR1.TXCIDE delay -- neither is tunable in firmware. Closing
   * the gigabit eye needs a scope on the TX pins or an EEPROM to
   * override the PHY straps; until then the link stays at 100M. */
  k_ra8_board_eth_phy_gbcr_value = 0x0000U, /**< 1000BASE-T NOT advertised (see above). */
  k_ra8_board_eth_phy_reset_spin = 4096U,   /**< BMCR.RESET self-clear cap.             */
} ra8_board_eth_phy_reg_t;

/**
 * @brief Soft-reset the GPY111 PHY and wait for the reset to clear.
 *
 * @details Mirrors the soft-reset step in FSP R_RMAC_PHY_ChipInit:
 * write BMCR.RESET, poll BMCR until the bit self-clears.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             PHY reset completed.
 * @retval k_ra8_err_hw_timeout BMCR.RESET never self-cleared.
 *
 * @pre ETHA1 is in OPERATION mode (MDIO can drive MDC).
 * @pre RMAC1 MPIC.PSMCS is non-zero.
 * @post The PHY has completed a register-level soft reset.
 * @post BMCR.RESET reads 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_eth_phy_soft_reset(void)
{
  ra8_err_t err = ra8_rmac_mdio_c22_write((ra8_rmac_port_t)k_ra8_board_eth_rmac_port,
                                          (uint8_t)k_ra8_board_eth_phy_addr,
                                          (uint8_t)k_ra8_board_eth_phy_reg_bmcr,
                                          (uint16_t)k_ra8_board_eth_phy_bmcr_reset);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_board_eth_phy_reset_spin; ++i) {
    uint16_t bmcr = 0U;
    err           = ra8_rmac_mdio_c22_read((ra8_rmac_port_t)k_ra8_board_eth_rmac_port,
                                           (uint8_t)k_ra8_board_eth_phy_addr,
                                           (uint8_t)k_ra8_board_eth_phy_reg_bmcr,
                                           &bmcr);
    if (err != k_ra8_ok) {
      return err;
    }
    if ((bmcr & (uint16_t)k_ra8_board_eth_phy_bmcr_reset) == 0U) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Program the GPY111 RGMII receive-timing skew to 1.0 ns.
 *
 * @details Read-modify-write of MIICTRL (vendor reg 0x17): clears the
 * RXSKEW field (bits 14:12), sets it to 0b010 = 1.0 ns. Mirrors FSP
 * rmac_phy_target_gpy111_initialize.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             RXSKEW programmed to 1.0 ns.
 * @retval k_ra8_err_hw_timeout An MDIO transaction timed out.
 *
 * @pre ETHA1 is in OPERATION mode.
 * @pre internal_eth_phy_soft_reset has completed.
 * @post GPY111 MIICTRL.RXSKEW == 0b010 (1.0 ns).
 * @post No other MIICTRL bits are disturbed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_eth_phy_set_rgmii_skew(void)
{
  uint16_t        miictrl  = 0U;
  const ra8_err_t read_err = ra8_rmac_mdio_c22_read((ra8_rmac_port_t)k_ra8_board_eth_rmac_port,
                                                    (uint8_t)k_ra8_board_eth_phy_addr,
                                                    (uint8_t)k_ra8_board_eth_phy_reg_miictrl,
                                                    &miictrl);
  if (read_err != k_ra8_ok) {
    return read_err;
  }
  miictrl = (uint16_t)((miictrl & (uint16_t)~k_ra8_board_eth_phy_rxskew_mask) |
                       (uint16_t)k_ra8_board_eth_phy_rxskew_1p0ns);
  return ra8_rmac_mdio_c22_write((ra8_rmac_port_t)k_ra8_board_eth_rmac_port,
                                 (uint8_t)k_ra8_board_eth_phy_addr,
                                 (uint8_t)k_ra8_board_eth_phy_reg_miictrl,
                                 miictrl);
}

/**
 * @brief Program the auto-neg advertisement and restart negotiation.
 *
 * @details Mirrors FSP R_RMAC_PHY_StartAutoNegotiate: writes ANAR
 * (10/100 abilities + selector), GBCR (1000BASE-T abilities), then
 * BMCR = AUTONEG_ENABLE | AUTONEG_RESTART. The restart forces the
 * GPY111 to re-converge the link AFTER the RGMII RX skew has been
 * applied, so the running link uses the corrected timing.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             Advertisement programmed, AN restarted.
 * @retval k_ra8_err_hw_timeout An MDIO transaction timed out.
 *
 * @pre internal_eth_phy_set_rgmii_skew has completed.
 * @pre ETHA1 is in OPERATION mode.
 * @post ANAR / GBCR hold the advertised abilities.
 * @post Auto-negotiation has been restarted.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_eth_phy_start_autoneg(void)
{
  ra8_err_t err = ra8_rmac_mdio_c22_write((ra8_rmac_port_t)k_ra8_board_eth_rmac_port,
                                          (uint8_t)k_ra8_board_eth_phy_addr,
                                          (uint8_t)k_ra8_board_eth_phy_reg_anar,
                                          (uint16_t)k_ra8_board_eth_phy_anar_value);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_rmac_mdio_c22_write((ra8_rmac_port_t)k_ra8_board_eth_rmac_port,
                                (uint8_t)k_ra8_board_eth_phy_addr,
                                (uint8_t)k_ra8_board_eth_phy_reg_gbcr,
                                (uint16_t)k_ra8_board_eth_phy_gbcr_value);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_rmac_mdio_c22_write(
    (ra8_rmac_port_t)k_ra8_board_eth_rmac_port,
    (uint8_t)k_ra8_board_eth_phy_addr,
    (uint8_t)k_ra8_board_eth_phy_reg_bmcr,
    (uint16_t)(k_ra8_board_eth_phy_bmcr_an_en | k_ra8_board_eth_phy_bmcr_an_rst));
}

/**
 * @brief Full GPY111 PHY chip-init: soft-reset, RX skew, restart AN.
 *
 * @details Sequences the three FSP-equivalent PHY bring-up steps so
 * ra8_board_ethernet_init stays under the function-size cap.
 *
 * @return ra8_err_t Error code from the first failing sub-step.
 * @retval k_ra8_ok             PHY initialised, auto-neg restarted.
 * @retval k_ra8_err_hw_timeout An MDIO transaction or reset timed out.
 *
 * @pre ETHA1 is in OPERATION mode; RMAC1 MPIC.PSMCS != 0.
 * @pre The PHY has had its hardware (RSTN) reset.
 * @post GPY111 RGMII RX skew = 1.0 ns and auto-neg is re-running.
 * @post The link will re-converge with the corrected RGMII timing.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_eth_phy_chip_init(void)
{
  ra8_err_t err = internal_eth_phy_soft_reset();
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_eth_phy_set_rgmii_skew();
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_eth_phy_start_autoneg();
}

ra8_err_t ra8_board_ethernet_init(void)
{
  /* Step 0: hardware-reset the on-board PEF7071 PHY before anything
   * else touches MDIO. */
  ra8_err_t err = internal_eth_phy_hw_reset();
  if (err != k_ra8_ok) {
    return err;
  }
  /* Step 1: route every remaining Ethernet pin to its RGMII alt mux. */
  err = internal_eth_route_alt_pins();
  if (err != k_ra8_ok) {
    return err;
  }
  /* Step 2: bring up ESWCLK + ESWPHYCLK, release PDCTRESWM, lift the
   * ETHPHYCLK / ESWM MSTP gates, and pulse the COMA agent out of
   * reset. After this the per-port RMAC / ETHA register windows
   * answer reads and accept writes. */
  uint32_t eswclk_hz = 0U;
  err                = internal_eth_eswm_bring_up(&eswclk_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  /* Step 3: ETHA1 RESET -> DISABLE -> CONFIG so RMAC.MPIC is writable. */
  err = internal_eth_etha_to_config();
  if (err != k_ra8_ok) {
    return err;
  }
  /* Step 4: select RGMII on the media-interface mux + release the
   * per-port reset. */
  err = internal_eth_eswm_select_rgmii();
  if (err != k_ra8_ok) {
    return err;
  }
  /* Step 5: RMAC1 bring-up. Programs MPIC (PSMCS / PIS / LSC / PIPP),
   * MAC address filter, and IRQ block. */
  err = internal_eth_rmac_program(eswclk_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  /* Step 6: promote ETHA1 to OPERATION so MDIO can drive MDC. */
  err = internal_eth_etha_to_operation();
  if (err != k_ra8_ok) {
    return err;
  }
  /* Step 7: GPY111 PHY chip-init -- soft-reset, program the RGMII
   * receive-timing skew (1.0 ns), then restart auto-negotiation so
   * the link re-converges with the skew applied. Mirrors FSP
   * R_RMAC_PHY_ChipInit + R_RMAC_PHY_StartAutoNegotiate. */
  return internal_eth_phy_chip_init();
}
