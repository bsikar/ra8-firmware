//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const lc = @import("mipi_csi_lifecycle");

const Fake = struct {
    regs: [0x300 / 4]u32 = @splat(0),
    rtst_busy_reads: u16 = 0,
    mstp_rc: u16 = 0,
    mstp_on: bool = false,
    detached: bool = false,
    errs: u8 = 0,
    last_err: u16 = 0,
    infos: u8 = 0,
    info_val: u32 = 0,

    pub fn read32(f: *Fake, off: u16) u32 {
        if (off == lc.off_rtst and f.rtst_busy_reads > 0) {
            f.rtst_busy_reads -= 1;
            return lc.rtst_vsrsts;
        }
        return f.regs[off / 4];
    }
    pub fn write32(f: *Fake, off: u16, value: u32) void {
        f.regs[off / 4] = value;
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
    pub fn infoVal(f: *Fake, _: [*:0]const u8, value: u32) void {
        f.infos += 1;
        f.info_val = value;
    }
    pub fn mstpEnable(f: *Fake) u16 {
        f.mstp_on = f.mstp_rc == 0;
        return f.mstp_rc;
    }
    pub fn mstpDisable(f: *Fake) u16 {
        f.mstp_on = false;
        return f.mstp_rc;
    }
    pub fn detachAll(f: *Fake) void {
        f.detached = true;
    }
};

fn goodCfg() lc.Config {
    var c = std.mem.zeroes(lc.Config);
    c.lanes = 2;
    c.generic_rule = true;
    c.eccv13 = true;
    c.frrclk = 0x1AB;
    c.frrskw = 0x3FF;
    c.epd_enable = true;
    c.epd_long_spacer = 0x1234;
    c.epd_short_spacer = 0x0042;
    c.vlsien = 2;
    c.eotp_enable = true;
    c.dt_low_mask = 0xDEAD_BEEF;
    c.dt_high_mask = 0x0000_00FF;
    c.rx_irq_mask = 0x11;
    c.dl_irq_mask = .{ 0x22, 0x33 };
    c.vc_irq_mask[15] = 0x44;
    c.pm_irq_mask = 0xFF;
    c.short_irq_mask = 0x55;
    c.short_threshold = 7;
    c.short_store_enable = true;
    return c;
}

test "Config layout matches the C struct" {
    try std.testing.expectEqual(@as(usize, 116), @sizeOf(lc.Config));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(lc.Config, "frrclk"));
    try std.testing.expectEqual(@as(usize, 18), @offsetOf(lc.Config, "vlsien"));
    try std.testing.expectEqual(@as(usize, 104), @offsetOf(lc.Config, "pm_irq_mask"));
}

test "validate rejects each out-of-range field" {
    var c = goodCfg();
    try std.testing.expectEqual(lc.ok, lc.validate(&c));
    c.lanes = 3;
    try std.testing.expectEqual(lc.invalid_arg, lc.validate(&c));
    c = goodCfg();
    c.vlsien = 4;
    try std.testing.expectEqual(lc.invalid_arg, lc.validate(&c));
    c = goodCfg();
    c.epd_long_spacer = 0x8000;
    try std.testing.expectEqual(lc.invalid_arg, lc.validate(&c));
    c = goodCfg();
    c.epd_short_spacer = 0x8000;
    try std.testing.expectEqual(lc.invalid_arg, lc.validate(&c));
    c = goodCfg();
    c.short_threshold = 0x10;
    try std.testing.expectEqual(lc.invalid_arg, lc.validate(&c));
}

test "init programs every register and logs the lane count" {
    var f = Fake{ .rtst_busy_reads = 3 };
    f.regs[lc.off_mct3 / 4] = lc.mct3_rxen;
    const c = goodCfg();
    try std.testing.expectEqual(lc.ok, lc.init(&f, &c));
    try std.testing.expect(f.mstp_on);
    try std.testing.expectEqual(@as(u32, 0), f.regs[lc.off_mct3 / 4]);
    try std.testing.expectEqual(@as(u32, 2 | lc.mct0_grmd | lc.mct0_eccv13), f.regs[lc.off_mct0 / 4]);
    try std.testing.expectEqual(@as(u32, (0x1FF << 16) | 0x1AB), f.regs[lc.off_mct2 / 4]);
    try std.testing.expectEqual(@as(u32, 0x1234 | (0x42 << 16) | lc.epct_epden), f.regs[lc.off_epct / 4]);
    try std.testing.expectEqual(@as(u32, 0x20 | lc.emct_eotpen), f.regs[lc.off_emct / 4]);
    try std.testing.expectEqual(@as(u32, 0xDEAD_BEEF), f.regs[lc.off_dtel / 4]);
    try std.testing.expectEqual(@as(u32, 7 | lc.gsct_gfif), f.regs[lc.off_gsct / 4]);
    try std.testing.expectEqual(@as(u32, 0x33), f.regs[lc.off_dlie1 / 4]);
    try std.testing.expectEqual(@as(u32, 0x44), f.regs[(lc.off_vcie0 + 15 * lc.vc_stride) / 4]);
    try std.testing.expectEqual(@as(u32, 0x55), f.regs[lc.off_gsie / 4]);
    try std.testing.expectEqual(@as(u32, 2), f.info_val);
}

test "init null cfg logs once and returns null_ptr" {
    var f = Fake{};
    try std.testing.expectEqual(lc.null_ptr, lc.init(&f, null));
    try std.testing.expectEqual(@as(u8, 1), f.errs);
    try std.testing.expect(!f.mstp_on);
}

test "init stops at bad cfg, mstp failure and reset timeout" {
    var f = Fake{};
    var c = goodCfg();
    c.lanes = 0;
    try std.testing.expectEqual(lc.invalid_arg, lc.init(&f, &c));
    try std.testing.expect(!f.mstp_on);
    f = Fake{ .mstp_rc = 0x201 };
    c = goodCfg();
    try std.testing.expectEqual(@as(u16, 0x201), lc.init(&f, &c));
    try std.testing.expectEqual(@as(u16, 0x201), f.last_err);
    f = Fake{ .rtst_busy_reads = lc.reset_spin_max };
    try std.testing.expectEqual(lc.hw_timeout, lc.init(&f, &c));
    try std.testing.expectEqual(lc.hw_timeout, f.last_err);
    try std.testing.expectEqual(@as(u32, 0), f.regs[lc.off_mct0 / 4]);
}

test "deinit clears masks, resets, detaches and gates the clock" {
    var f = Fake{ .mstp_on = true };
    const c = goodCfg();
    lc.programIrqMasks(&f, &c);
    f.regs[lc.off_mct3 / 4] = lc.mct3_rxen;
    try std.testing.expectEqual(lc.ok, lc.deinit(&f));
    try std.testing.expectEqual(@as(u32, 0), f.regs[lc.off_mct3 / 4]);
    try std.testing.expectEqual(lc.rtct_vsrst, f.regs[lc.off_rtct / 4]);
    try std.testing.expectEqual(@as(u32, 0), f.regs[lc.off_pmie / 4]);
    try std.testing.expectEqual(@as(u32, 0), f.regs[(lc.off_vcie0 + 15 * lc.vc_stride) / 4]);
    try std.testing.expect(f.detached and !f.mstp_on);
}

test "reset writes VSRST and waits for idle" {
    var f = Fake{ .rtst_busy_reads = 5 };
    try std.testing.expectEqual(lc.ok, lc.reset(&f));
    try std.testing.expectEqual(lc.rtct_vsrst, f.regs[lc.off_rtct / 4]);
    f = Fake{ .rtst_busy_reads = lc.reset_spin_max };
    try std.testing.expectEqual(lc.hw_timeout, lc.reset(&f));
    try std.testing.expectEqual(@as(u8, 0), f.errs);
}

test "start refuses when running; stop clears RXEN and resets" {
    var f = Fake{};
    try std.testing.expectEqual(lc.ok, lc.startReceive(&f));
    try std.testing.expectEqual(lc.mct3_rxen, f.regs[lc.off_mct3 / 4]);
    try std.testing.expectEqual(lc.invalid_state, lc.startReceive(&f));
    try std.testing.expectEqual(lc.invalid_state, f.last_err);
    try std.testing.expectEqual(lc.ok, lc.stopReceive(&f));
    try std.testing.expectEqual(@as(u32, 0), f.regs[lc.off_mct3 / 4]);
    try std.testing.expectEqual(lc.rtct_vsrst, f.regs[lc.off_rtct / 4]);
    try std.testing.expectEqual(@as(u8, 2), f.infos);
}
