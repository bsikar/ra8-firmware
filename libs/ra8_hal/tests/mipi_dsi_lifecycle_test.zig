//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const lc = @import("mipi_dsi_lifecycle");

const Fake = struct {
    regs: [0x620 / 4]u32 = @splat(0),
    rstcr_writes: [4]u32 = .{ 0, 0, 0, 0 },
    rstcr_count: u8 = 0,
    mstp_rc: u16 = 0,
    mstp_on: bool = false,
    errs: u8 = 0,
    last_err: u16 = 0,
    infos: u8 = 0,

    pub fn write32(f: *Fake, off: u16, value: u32) void {
        if (off == lc.off_rstcr and f.rstcr_count < f.rstcr_writes.len) {
            f.rstcr_writes[f.rstcr_count] = value;
            f.rstcr_count += 1;
        }
        f.regs[off / 4] = value;
    }
    pub fn reg(f: *const Fake, off: u16) u32 {
        return f.regs[off / 4];
    }
    pub fn err(f: *Fake, _: [*:0]const u8) void {
        f.errs += 1;
    }
    pub fn errVal(f: *Fake, _: [*:0]const u8, code: u16) void {
        f.errs += 1;
        f.last_err = code;
    }
    pub fn info(f: *Fake, _: [*:0]const u8) void {
        f.infos += 1;
    }
    pub fn mstpEnable(f: *Fake) u16 {
        f.mstp_on = f.mstp_rc == 0;
        return f.mstp_rc;
    }
    pub fn mstpDisable(f: *Fake) u16 {
        if (f.mstp_rc == 0) f.mstp_on = false;
        return f.mstp_rc;
    }
};

const Vars = struct {
    initialized: bool = false,
    continuous: bool = false,
    clock_ulps: bool = true,
    data_ulps: bool = true,
    event_fn: ?*const anyopaque = null,
    event_ctx: ?*anyopaque = null,
    rx_buffer: ?[*]u8 = null,
    rx_len: u16 = 0,

    fn state(v: *Vars) lc.State {
        return .{
            .initialized = &v.initialized,
            .continuous_clock = &v.continuous,
            .clock_ulps = &v.clock_ulps,
            .data_ulps = &v.data_ulps,
            .event_fn = &v.event_fn,
            .event_ctx = &v.event_ctx,
            .rx_buffer = &v.rx_buffer,
            .rx_len = &v.rx_len,
        };
    }
};

var marker: u8 = 0;
var rx_store: [4]u8 = .{ 0, 0, 0, 0 };

fn sampleCfg() lc.Config {
    return .{
        .lane_count = lc.lanes_2,
        .clock_mode = lc.clock_continuous,
        .max_return_packet_size = 0x1_2345 & 0xFFFF,
        .ulps_wakeup_period = 0x5A,
        .ecc_check_enable = 1,
        .eotp_enable = 1,
        .scramble_enable = 0,
        .tearing_detect_enable = 1,
        .crc_check_vc_mask = 0x1B,
        .timing = .{ .clock_stop_time = 0x7FF, .clock_beforehand_time = 0x12, .clock_keep_time = 0x34, .go_lp_and_back = 0x7FF },
        .timeouts = .{ .hs_tx_timeout = 1, .lp_rx_host_timeout = 2, .turnaround_timeout = 3, .bta_timeout = 4, .lp_rw_timeout = 5, .hs_rw_timeout = 6 },
    };
}

test "Config mirrors ra8_mipi_dsi_config_t layout" {
    try std.testing.expectEqual(@as(usize, 40), @sizeOf(lc.Config));
    try std.testing.expectEqual(@as(usize, 2), @offsetOf(lc.Config, "max_return_packet_size"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(lc.Config, "ulps_wakeup_period"));
    try std.testing.expectEqual(@as(usize, 9), @offsetOf(lc.Config, "crc_check_vc_mask"));
    try std.testing.expectEqual(@as(usize, 6), @sizeOf(lc.Timing));
    try std.testing.expectEqual(@as(usize, 12), @offsetOf(lc.Timeouts, "bta_timeout"));
}

test "validateCfg accepts one or two lanes only" {
    var cfg = sampleCfg();
    try std.testing.expectEqual(lc.ok, lc.validateCfg(&cfg));
    cfg.lane_count = lc.lanes_1;
    try std.testing.expectEqual(lc.ok, lc.validateCfg(&cfg));
    for ([_]u8{ 0, 3, 0xFF }) |n| {
        cfg.lane_count = n;
        try std.testing.expectEqual(lc.invalid_arg, lc.validateCfg(&cfg));
    }
}

test "TXSETR and DSISETR encode the config fields" {
    var cfg = sampleCfg();
    try std.testing.expectEqual(lc.txset_clen | lc.txset_dlen | lc.txset_lane2, lc.makeTxsetr(&cfg));
    try std.testing.expectEqual(@as(u32, 0x00B0_2345) | lc.dsisetr_eotpen | lc.dsisetr_extemd | lc.dsisetr_eccen, lc.makeDsisetr(&cfg));
    cfg.lane_count = lc.lanes_1;
    cfg.ecc_check_enable = 0;
    cfg.eotp_enable = 0;
    cfg.tearing_detect_enable = 0;
    cfg.scramble_enable = 2;
    cfg.crc_check_vc_mask = 0;
    try std.testing.expectEqual(lc.txset_clen | lc.txset_dlen, lc.makeTxsetr(&cfg));
    try std.testing.expectEqual(@as(u32, 0x2345) | lc.dsisetr_scren, lc.makeDsisetr(&cfg));
}

test "CLSTPTSETR masks each timing field" {
    const t = sampleCfg().timing;
    try std.testing.expectEqual(@as(u32, 0x3412_0FFC), lc.makeClstptsetr(t));
}

test "init rejects null and bad config without touching hardware" {
    var f = Fake{};
    var v = Vars{};
    try std.testing.expectEqual(lc.null_ptr, lc.init(&f, v.state(), null));
    var cfg = sampleCfg();
    cfg.lane_count = 4;
    try std.testing.expectEqual(lc.invalid_arg, lc.init(&f, v.state(), &cfg));
    try std.testing.expectEqual(lc.invalid_arg, f.last_err);
    f.mstp_rc = 0x201;
    cfg.lane_count = 1;
    try std.testing.expectEqual(@as(u16, 0x201), lc.init(&f, v.state(), &cfg));
    try std.testing.expectEqual(@as(u8, 3), f.errs);
    try std.testing.expectEqual(@as(u8, 0), f.rstcr_count);
    try std.testing.expect(!v.initialized);
}

test "init programs link, timeouts and clears status" {
    var f = Fake{};
    var v = Vars{ .event_fn = &marker, .event_ctx = &marker, .rx_buffer = &rx_store, .rx_len = 4 };
    const cfg = sampleCfg();
    try std.testing.expectEqual(lc.ok, lc.init(&f, v.state(), &cfg));
    try std.testing.expectEqual(@as(u8, 2), f.rstcr_count);
    try std.testing.expectEqual(lc.rstcr_swrst, f.rstcr_writes[0]);
    try std.testing.expectEqual(@as(u32, 0), f.rstcr_writes[1]);
    try std.testing.expectEqual(@as(u32, 0x5A), f.reg(lc.off_ulpssetr));
    try std.testing.expectEqual(@as(u32, 0x3FF), f.reg(lc.off_lptrnstsetr));
    try std.testing.expectEqual(@as(u32, 4), f.reg(lc.off_presptobtasetr));
    try std.testing.expectEqual(@as(u32, 6), f.reg(lc.off_presptohssetr));
    try std.testing.expectEqual(@as(u32, 3), f.reg(lc.off_tatosetr));
    try std.testing.expectEqual(lc.sqch_clear_all, f.reg(lc.off_sqch1scr));
    try std.testing.expectEqual(lc.plsr_clear_all, f.reg(lc.off_plscr));
    try std.testing.expect(v.initialized and v.continuous and !v.clock_ulps and !v.data_ulps);
    try std.testing.expect(v.event_fn == null and v.event_ctx == null and v.rx_buffer == null);
    try std.testing.expectEqual(@as(u16, 0), v.rx_len);
    try std.testing.expectEqual(@as(u8, 1), f.infos);
}

test "deinit resets, clears state and keeps init on mstp failure" {
    var f = Fake{};
    var v = Vars{};
    try std.testing.expectEqual(lc.ok, lc.deinit(&f, v.state()));
    try std.testing.expectEqual(@as(u8, 0), f.rstcr_count);
    v = .{ .initialized = true, .continuous = true, .event_fn = &marker, .rx_buffer = &rx_store, .rx_len = 4 };
    f.mstp_rc = 0x201;
    try std.testing.expectEqual(@as(u16, 0x201), lc.deinit(&f, v.state()));
    try std.testing.expect(v.initialized and !v.continuous and v.event_fn == null and v.rx_len == 0);
    f.mstp_rc = 0;
    try std.testing.expectEqual(lc.ok, lc.deinit(&f, v.state()));
    try std.testing.expect(!v.initialized);
    try std.testing.expectEqual(lc.rstcr_swrst, f.rstcr_writes[0]);
}

test "enterStop and exitStop gate on the initialized flag" {
    var f = Fake{};
    var v = Vars{};
    try std.testing.expectEqual(lc.invalid_state, lc.enterStop(&f, v.state()));
    try std.testing.expectEqual(lc.ok, lc.exitStop(&f, v.state()));
    try std.testing.expect(v.initialized and f.mstp_on);
    try std.testing.expectEqual(lc.invalid_state, lc.exitStop(&f, v.state()));
    f.mstp_rc = 0x201;
    try std.testing.expectEqual(@as(u16, 0x201), lc.enterStop(&f, v.state()));
    try std.testing.expect(v.initialized);
    f.mstp_rc = 0;
    try std.testing.expectEqual(lc.ok, lc.enterStop(&f, v.state()));
    try std.testing.expect(!v.initialized);
}
