//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The J32 MIPI panel: the D-PHY and DSI link configs for the 2-lane
//! 480x854 panel, and the bring-up order HUM Ch 64.3.1 asks for: PHY
//! first, then the DSI link layer (HUM Ch 65), then the HS clock.
//!
//! The panel-datasheet timings are still placeholders (zero DSI guard
//! timings and timeouts, `tinit = 1` on the PHY), exactly as in the C
//! this replaces.

const hal = @import("hal.zig");

const ErrCode = hal.ErrCode;
const ok: ErrCode = 0;

/// Panel geometry and link rate (J32, 2 lanes).
pub const h_active: u16 = 480;
pub const v_active: u16 = 854;
pub const line_rate_mbps: u16 = 480;

/// PCLKA the PHY PLL assumes (the CGC reset default) and the PLL's integer
/// multiplier; `k_panel_pclka_mhz` / `k_panel_pll_nmul` in the internal header.
pub const pclka_mhz: u8 = 60;
pub const pll_nmul: u16 = 48;

/// Mirrors `ra8_mipi_phy_pll_t`; enums are carried as their u8 storage.
pub const PhyPll = extern struct {
    idiv: u8,
    pmul: u8,
    nfmul: u8,
    nmul_int: u16,
};

/// Mirrors `ra8_mipi_phy_timing_t`.
pub const PhyTiming = extern struct {
    tinit: u32,
    tclkprep: u8 = 0,
    tclksett: u8 = 0,
    tclkmiss: u8 = 0,
    thsprep: u8 = 0,
    thssett: u8 = 0,
    tclkzero: u8 = 0,
    tclkpre: u8 = 0,
    tclkpost: u8 = 0,
    tclktrl: u8 = 0,
    thszero: u8 = 0,
    thstrl: u8 = 0,
    thsexit: u8 = 0,
    tlpx: u8 = 0,
};

/// Mirrors `ra8_mipi_phy_config_t`.
pub const PhyConfig = extern struct {
    mode: u8,
    pclka_mhz: u8,
    line_rate_mbps: u16,
    lane_count: u8,
    clk_mode: u8,
    eotp: u8,
    pll: PhyPll,
    escdiv: u8,
    p_timing: *const PhyTiming,
};

/// Mirrors `ra8_mipi_dsi_timing_t`.
pub const DsiTiming = extern struct {
    clock_stop_time: u16 = 0,
    clock_beforehand_time: u8 = 0,
    clock_keep_time: u8 = 0,
    go_lp_and_back: u16 = 0,
};

/// Mirrors `ra8_mipi_dsi_timeouts_t`.
pub const DsiTimeouts = extern struct {
    hs_tx_timeout: u32 = 0,
    lp_rx_host_timeout: u32 = 0,
    turnaround_timeout: u32 = 0,
    bta_timeout: u32 = 0,
    lp_rw_timeout: u32 = 0,
    hs_rw_timeout: u32 = 0,
};

/// Mirrors `ra8_mipi_dsi_config_t`.
pub const DsiConfig = extern struct {
    lane_count: u8,
    clock_mode: u8,
    max_return_packet_size: u16,
    ulps_wakeup_period: u8,
    ecc_check_enable: bool,
    eotp_enable: bool,
    scramble_enable: bool,
    tearing_detect_enable: bool,
    crc_check_vc_mask: u8,
    timing: DsiTiming,
    timeouts: DsiTimeouts,
};

comptime {
    if (@sizeOf(PhyPll) != 6) @compileError("PhyPll must match ra8_mipi_phy_pll_t");
    if (@sizeOf(PhyTiming) != 20) @compileError("PhyTiming must match ra8_mipi_phy_timing_t");
    if (@offsetOf(PhyConfig, "pll") != 8) @compileError("PhyConfig.pll offset");
    if (@offsetOf(PhyConfig, "escdiv") != 14) @compileError("PhyConfig.escdiv offset");
    if (@offsetOf(PhyConfig, "p_timing") != 16) @compileError("PhyConfig.p_timing offset");
    if (@sizeOf(DsiConfig) != 40) @compileError("DsiConfig must match ra8_mipi_dsi_config_t (40 B)");
    if (@offsetOf(DsiConfig, "timing") != 10) @compileError("DsiConfig.timing offset");
    if (@offsetOf(DsiConfig, "timeouts") != 16) @compileError("DsiConfig.timeouts offset");
}

/// `ra8_mipi_phy_mode_t` / `_lane_count_t` / `_clk_mode_t` / `_eotp_t` and
/// the PLL divider codes this panel uses.
const phy_mode_dsi_host: u8 = 1;
const phy_lane_count_2: u8 = 2;
const phy_clk_noncontinuous: u8 = 0;
const phy_eotp_enabled: u8 = 1;
const phy_idiv_1: u8 = 0;
const phy_pmul_4: u8 = 2;
const phy_nfmul_0_00: u8 = 0;

/// `ra8_mipi_dsi_lane_count_t` / `_clock_mode_t`.
const dsi_lanes_2: u8 = 2;
const dsi_clock_non_continuous: u8 = 0;

pub const phy_timing = PhyTiming{ .tinit = 1 };

pub const phy_config = PhyConfig{
    .mode = phy_mode_dsi_host,
    .pclka_mhz = pclka_mhz,
    .line_rate_mbps = line_rate_mbps,
    .lane_count = phy_lane_count_2,
    .clk_mode = phy_clk_noncontinuous,
    .eotp = phy_eotp_enabled,
    .pll = .{ .idiv = phy_idiv_1, .pmul = phy_pmul_4, .nfmul = phy_nfmul_0_00, .nmul_int = pll_nmul },
    .escdiv = 0,
    .p_timing = &phy_timing,
};

pub const dsi_config = DsiConfig{
    .lane_count = dsi_lanes_2,
    .clock_mode = dsi_clock_non_continuous,
    .max_return_packet_size = 16,
    .ulps_wakeup_period = 0,
    .ecc_check_enable = true,
    .eotp_enable = true,
    .scramble_enable = false,
    .tearing_detect_enable = true,
    .crc_check_vc_mask = 0x01, // VC0 only: the only virtual channel J32 wires up.
    .timing = .{},
    .timeouts = .{},
};

extern fn ra8_mipi_phy_init(cfg: *const PhyConfig) ErrCode;
extern fn ra8_mipi_dsi_init(cfg: *const DsiConfig) ErrCode;
extern fn ra8_mipi_dsi_hs_clock_start() ErrCode;

/// PHY, then the DSI link layer, then the HS clock; the first failure is
/// returned and nothing after it runs.
pub fn init() u32 {
    var err = ra8_mipi_phy_init(&phy_config);
    if (err != ok) return err;
    err = ra8_mipi_dsi_init(&dsi_config);
    if (err != ok) return err;
    return ra8_mipi_dsi_hs_clock_start();
}
