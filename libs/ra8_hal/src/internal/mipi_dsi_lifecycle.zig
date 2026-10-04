//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI DSI-2 host lifecycle (RA8FW-645): init, deinit and module-stop
//! entry/exit, the last part of ra8_mipi_dsi.c. Registers, module stop and
//! logging go through a `dsi` ops value so host tests can use a fake
//! register file; driver state goes through `State` pointers.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;
pub const null_ptr: u16 = 0x504;

pub const off_txsetr: u16 = 0x100;
pub const off_ulpssetr: u16 = 0x108;
pub const off_rstcr: u16 = 0x110;
pub const off_dsisetr: u16 = 0x120;
pub const off_rxscr: u16 = 0x204;
pub const off_presptobtasetr: u16 = 0x210;
pub const off_presptolpsetr: u16 = 0x214;
pub const off_presptohssetr: u16 = 0x218;
pub const off_hstxtosetr: u16 = 0x2E0;
pub const off_lrxhtosetr: u16 = 0x2E4;
pub const off_tatosetr: u16 = 0x2E8;
pub const off_ferrscr: u16 = 0x304;
pub const off_clstptsetr: u16 = 0x314;
pub const off_lptrnstsetr: u16 = 0x318;
pub const off_plscr: u16 = 0x324;
pub const off_vmscr: u16 = 0x414;
pub const off_sqch0scr: u16 = 0x5D4;
pub const off_sqch1scr: u16 = 0x614;

pub const rstcr_swrst: u32 = 1 << 0;
pub const txset_lane2: u32 = 1 << 0;
pub const txset_clen: u32 = 1 << 8;
pub const txset_dlen: u32 = 1 << 9;
pub const dsisetr_mrpsz_mask: u32 = 0xFFFF;
pub const dsisetr_eccen: u32 = 1 << 16;
pub const dsisetr_vc_crc_shift: u5 = 20;
pub const dsisetr_vc_crc_mask: u32 = 0x00F0_0000;
pub const dsisetr_scren: u32 = 1 << 29;
pub const dsisetr_extemd: u32 = 1 << 30;
pub const dsisetr_eotpen: u32 = 1 << 31;
pub const clkstpt_shift: u5 = 2;
pub const clkstpt_mask: u32 = 0xFFC;
pub const clkbfht_shift: u5 = 16;
pub const clkbfht_mask: u32 = 0xFF << 16;
pub const clkkpt_shift: u5 = 24;
pub const clkkpt_mask: u32 = 0xFF << 24;
pub const golpbkt_mask: u32 = 0x3FF;
pub const sqch_clear_all: u32 = 0x7D09_0110;
pub const vmsr_clear_all: u32 = 0x00D0_0111;
pub const rxsr_clear_all: u32 = 0x57F7_E507;
pub const ferrsr_clear_all: u32 = 0x001F_0007;
pub const plsr_clear_all: u32 = 0x3F00_3000;

pub const lanes_1: u8 = 1;
pub const lanes_2: u8 = 2;
pub const clock_continuous: u8 = 1;

/// Mirrors `ra8_mipi_dsi_timing_t` (inc/ra8_mipi_dsi_types.h).
pub const Timing = extern struct {
    clock_stop_time: u16,
    clock_beforehand_time: u8,
    clock_keep_time: u8,
    go_lp_and_back: u16,
};

/// Mirrors `ra8_mipi_dsi_timeouts_t`.
pub const Timeouts = extern struct {
    hs_tx_timeout: u32,
    lp_rx_host_timeout: u32,
    turnaround_timeout: u32,
    bta_timeout: u32,
    lp_rw_timeout: u32,
    hs_rw_timeout: u32,
};

/// Mirrors `ra8_mipi_dsi_config_t`; enums and bools are carried as their
/// u8 storage so out-of-range values reach validation unchanged.
pub const Config = extern struct {
    lane_count: u8,
    clock_mode: u8,
    max_return_packet_size: u16,
    ulps_wakeup_period: u8,
    ecc_check_enable: u8,
    eotp_enable: u8,
    scramble_enable: u8,
    tearing_detect_enable: u8,
    crc_check_vc_mask: u8,
    timing: Timing,
    timeouts: Timeouts,
};

comptime {
    if (@sizeOf(Config) != 40) @compileError("Config must match ra8_mipi_dsi_config_t (40 B)");
    if (@offsetOf(Config, "timing") != 10) @compileError("timing offset");
    if (@offsetOf(Config, "timeouts") != 16) @compileError("timeouts offset");
}

/// Driver state shared with ra8_mipi_dsi_dispatch.c and the lane controls.
pub const State = struct {
    initialized: *bool,
    continuous_clock: *bool,
    clock_ulps: *bool,
    data_ulps: *bool,
    event_fn: *?*const anyopaque,
    event_ctx: *?*anyopaque,
    rx_buffer: *?[*]u8,
    rx_len: *u16,

    fn clear(st: State) void {
        st.event_fn.* = null;
        st.event_ctx.* = null;
        st.rx_buffer.* = null;
        st.rx_len.* = 0;
        st.clock_ulps.* = false;
        st.data_ulps.* = false;
    }
};

pub fn validateCfg(cfg: *const Config) u16 {
    if (cfg.lane_count != lanes_1 and cfg.lane_count != lanes_2) return invalid_arg;
    return ok;
}

pub fn makeTxsetr(cfg: *const Config) u32 {
    var v: u32 = txset_clen | txset_dlen;
    if (cfg.lane_count == lanes_2) v |= txset_lane2;
    return v;
}

pub fn makeDsisetr(cfg: *const Config) u32 {
    var v: u32 = @as(u32, cfg.max_return_packet_size) & dsisetr_mrpsz_mask;
    if (cfg.ecc_check_enable != 0) v |= dsisetr_eccen;
    if (cfg.eotp_enable != 0) v |= dsisetr_eotpen;
    if (cfg.scramble_enable != 0) v |= dsisetr_scren;
    if (cfg.tearing_detect_enable != 0) v |= dsisetr_extemd;
    v |= (@as(u32, cfg.crc_check_vc_mask) << dsisetr_vc_crc_shift) & dsisetr_vc_crc_mask;
    return v;
}

pub fn makeClstptsetr(t: Timing) u32 {
    return ((@as(u32, t.clock_stop_time) << clkstpt_shift) & clkstpt_mask) |
        ((@as(u32, t.clock_beforehand_time) << clkbfht_shift) & clkbfht_mask) |
        ((@as(u32, t.clock_keep_time) << clkkpt_shift) & clkkpt_mask);
}

pub fn clearAllStatus(dsi: anytype) void {
    dsi.write32(off_sqch0scr, sqch_clear_all);
    dsi.write32(off_sqch1scr, sqch_clear_all);
    dsi.write32(off_vmscr, vmsr_clear_all);
    dsi.write32(off_rxscr, rxsr_clear_all);
    dsi.write32(off_ferrscr, ferrsr_clear_all);
    dsi.write32(off_plscr, plsr_clear_all);
}

pub fn programLink(dsi: anytype, cfg: *const Config) void {
    dsi.write32(off_rstcr, rstcr_swrst);
    dsi.write32(off_rstcr, 0);
    dsi.write32(off_txsetr, makeTxsetr(cfg));
    dsi.write32(off_ulpssetr, @as(u32, cfg.ulps_wakeup_period));
    dsi.write32(off_dsisetr, makeDsisetr(cfg));
    dsi.write32(off_clstptsetr, makeClstptsetr(cfg.timing));
    dsi.write32(off_lptrnstsetr, @as(u32, cfg.timing.go_lp_and_back) & golpbkt_mask);
}

pub fn programTimeouts(dsi: anytype, cfg: *const Config) void {
    const t = cfg.timeouts;
    dsi.write32(off_presptobtasetr, t.bta_timeout);
    dsi.write32(off_presptolpsetr, t.lp_rw_timeout);
    dsi.write32(off_presptohssetr, t.hs_rw_timeout);
    dsi.write32(off_hstxtosetr, t.hs_tx_timeout);
    dsi.write32(off_lrxhtosetr, t.lp_rx_host_timeout);
    dsi.write32(off_tatosetr, t.turnaround_timeout);
}

/// ra8_mipi_dsi_init: validate, ungate, program link/timeouts, clear status.
pub fn init(dsi: anytype, st: State, cfg_opt: ?*const Config) u16 {
    const cfg = cfg_opt orelse {
        dsi.err("cfg must not be nullptr");
        return null_ptr;
    };
    const cfg_err = validateCfg(cfg);
    if (cfg_err != ok) {
        dsi.errVal("mipi_dsi_init: cfg invalid", cfg_err);
        return cfg_err;
    }
    const mst_err = dsi.mstpEnable();
    if (mst_err != ok) {
        dsi.errVal("mipi_dsi_init: mstp enable", mst_err);
        return mst_err;
    }
    programLink(dsi, cfg);
    programTimeouts(dsi, cfg);
    clearAllStatus(dsi);
    st.clear();
    st.continuous_clock.* = cfg.clock_mode == clock_continuous;
    st.initialized.* = true;
    dsi.info("mipi_dsi_init done");
    return ok;
}

/// ra8_mipi_dsi_deinit: reset the IP, drop callbacks, gate the module.
pub fn deinit(dsi: anytype, st: State) u16 {
    if (!st.initialized.*) return ok;
    dsi.write32(off_rstcr, rstcr_swrst);
    st.clear();
    st.continuous_clock.* = false;
    const err = dsi.mstpDisable();
    if (err == ok) st.initialized.* = false;
    return err;
}

pub fn enterStop(dsi: anytype, st: State) u16 {
    if (!st.initialized.*) return invalid_state;
    const err = dsi.mstpDisable();
    if (err == ok) st.initialized.* = false;
    return err;
}

pub fn exitStop(dsi: anytype, st: State) u16 {
    if (st.initialized.*) return invalid_state;
    const err = dsi.mstpEnable();
    if (err == ok) st.initialized.* = true;
    return err;
}
