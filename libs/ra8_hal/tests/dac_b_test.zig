//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/dac_b.zig.

const std = @import("std");
const dac = @import("dac_b");

/// Two instances of four registers; DADR writes are recorded as 16-bit.
const Regs = struct {
    r: [2][4]u32 = .{ .{ 0, 0, 0, 0 }, .{ 0, 0, 0, 0 } },
    dadr16: usize = 0,

    pub fn read32(self: *Regs, ch: u8, off: usize) u32 {
        return self.r[ch][off / 4];
    }
    pub fn write32(self: *Regs, ch: u8, off: usize, value: u32) void {
        std.debug.assert(off != dac.off_dadr);
        self.r[ch][off / 4] = value;
    }
    pub fn write16(self: *Regs, ch: u8, off: usize, value: u16) void {
        std.debug.assert(off == dac.off_dadr);
        self.dadr16 += 1;
        self.r[ch][0] = value;
    }
};

const Ops = struct {
    fail_on: ?u16 = null,
    enabled: [4]u16 = undefined,
    ne: usize = 0,
    disabled: [4]u16 = undefined,
    nd: usize = 0,
    infos: usize = 0,
    last_val: u32 = 0,
    fail_msg: ?[]const u8 = null,
    fail_err: u16 = 0,

    pub fn mstpEnable(self: *Ops, id: u16) u16 {
        self.enabled[self.ne] = id;
        self.ne += 1;
        return if (self.fail_on == id) 0x104 else dac.ok;
    }
    pub fn mstpDisable(self: *Ops, id: u16) u16 {
        self.disabled[self.nd] = id;
        self.nd += 1;
        return if (id == dac.mstp_dac0) 0x55 else 0x66;
    }
    pub fn info(self: *Ops, _: [*:0]const u8) void {
        self.infos += 1;
    }
    pub fn infoVal(self: *Ops, _: [*:0]const u8, value: u32) void {
        self.last_val = value;
    }
    pub fn fail(self: *Ops, msg: [*:0]const u8, err: u16) void {
        self.fail_msg = std.mem.span(msg);
        self.fail_err = err;
    }
};

test "init releases DAC_B0 then DAC_B1 and zeroes both instances" {
    var r = Regs{};
    r.r[0] = .{ 9, 1, 2, 3 };
    r.r[1] = .{ 9, 1, 2, 3 };
    var o = Ops{};
    try std.testing.expectEqual(dac.ok, dac.init(&r, &o));
    try std.testing.expectEqualSlices(u16, &.{ dac.mstp_dac0, dac.mstp_dac1 }, o.enabled[0..o.ne]);
    for (r.r) |inst| {
        try std.testing.expectEqual(@as(u32, 0), inst[0]);
        try std.testing.expectEqual(@as(u32, 0), inst[1]);
    }
    try std.testing.expectEqual(@as(usize, 1), o.infos);
}

test "an MSTP failure in init is logged with its step and returned" {
    var r = Regs{};
    var o = Ops{ .fail_on = dac.mstp_dac1 };
    try std.testing.expectEqual(@as(u16, 0x104), dac.init(&r, &o));
    try std.testing.expectEqualStrings("dac_b_init: mstp dac1", o.fail_msg.?);
    try std.testing.expectEqual(@as(usize, 0), r.dadr16);
}

test "write clamps to 12 bits and rejects a third channel" {
    var r = Regs{};
    var o = Ops{};
    try std.testing.expectEqual(dac.ok, dac.write(&r, &o, 1, 0xFFFF));
    try std.testing.expectEqual(@as(u32, 4095), r.r[1][0]);
    try std.testing.expectEqual(@as(u32, 4095), o.last_val);
    try std.testing.expectEqual(dac.ok, dac.write(&r, &o, 0, 1234));
    try std.testing.expectEqual(@as(u32, 1234), r.r[0][0]);
    try std.testing.expectEqual(dac.invalid_arg, dac.write(&r, &o, 2, 1));
}

test "init_configured builds the FSP register image and starts the chosen channels" {
    var r = Regs{};
    var o = Ops{};
    const cfg = dac.Cfg{ .vref = 1, .data_format = 1, .internal_output_enabled = false, .enable_channel0 = false, .enable_channel1 = true };
    try std.testing.expectEqual(dac.ok, dac.initConfigured(&r, &o, &cfg));
    try std.testing.expectEqual(dac.daoutdis, r.r[0][1]);
    try std.testing.expectEqual(dac.daoutdis | dac.dacen, r.r[1][1]);
    try std.testing.expectEqual(@as(u32, 1) << 16, r.r[0][2]);
    try std.testing.expectEqual(dac.ofssel_mask, r.r[1][3]);
    try std.testing.expectEqual(@as(usize, 1), o.infos);
}

test "init_configured with internal output keeps DAOUTDIS clear" {
    var r = Regs{};
    var o = Ops{ .fail_on = dac.mstp_dac0 };
    const cfg = dac.Cfg{ .vref = 0, .data_format = 0, .internal_output_enabled = true, .enable_channel0 = true, .enable_channel1 = false };
    try std.testing.expectEqual(@as(u16, 0x104), dac.initConfigured(&r, &o, &cfg));
    try std.testing.expectEqualStrings("dac_b_init_cfg: mstp dac0", o.fail_msg.?);
    o.fail_on = null;
    try std.testing.expectEqual(dac.ok, dac.initConfigured(&r, &o, &cfg));
    try std.testing.expectEqual(dac.dacen, r.r[0][1]);
    try std.testing.expectEqual(@as(u32, 0), r.r[1][1]);
}

test "deinit disables both outputs and returns the DAC_B0 MSTP result" {
    var r = Regs{};
    r.r[0][0] = 7;
    r.r[1][1] = dac.dacen;
    var o = Ops{};
    try std.testing.expectEqual(@as(u16, 0x55), dac.deinit(&r, &o));
    try std.testing.expectEqual(dac.daoutdis, r.r[0][1]);
    try std.testing.expectEqual(dac.daoutdis, r.r[1][1]);
    try std.testing.expectEqual(@as(u32, 0), r.r[0][0]);
    try std.testing.expectEqualSlices(u16, &.{ dac.mstp_dac1, dac.mstp_dac0 }, o.disabled[0..o.nd]);
}

test "set_vref changes only OFSSEL on both instances" {
    var r = Regs{};
    r.r[0][3] = 0xFFFF_FEFF;
    try std.testing.expectEqual(dac.ok, dac.setVref(&r, 1));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), r.r[0][3]);
    try std.testing.expectEqual(dac.ofssel_mask, r.r[1][3]);
    try std.testing.expectEqual(dac.ok, dac.setVref(&r, 0));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FEFF), r.r[0][3]);
}

test "output enable, status and clear_status track DACEN" {
    var r = Regs{};
    r.r[0][1] = dac.daoutdis;
    try std.testing.expectEqual(dac.ok, dac.setOutputEnable(&r, 0, true));
    try std.testing.expectEqual(dac.ok, dac.setOutputEnable(&r, 1, true));
    try std.testing.expectEqual(@as(u8, 3), dac.status(&r));
    try std.testing.expectEqual(dac.ok, dac.setOutputEnable(&r, 1, false));
    try std.testing.expectEqual(@as(u8, 1), dac.status(&r));
    try std.testing.expectEqual(dac.invalid_arg, dac.setOutputEnable(&r, 2, true));
    try std.testing.expectEqual(dac.ok, dac.clearStatus(&r));
    try std.testing.expectEqual(@as(u8, 0), dac.status(&r));
    try std.testing.expectEqual(dac.daoutdis, r.r[0][1]);
}

test "enter_stop zeroes both instances and gates DAC_B1 then DAC_B0" {
    var r = Regs{};
    r.r[1] = .{ 5, dac.dacen | dac.daoutdis, 0, 0 };
    var o = Ops{};
    try std.testing.expectEqual(@as(u16, 0x55), dac.enterStop(&r, &o));
    try std.testing.expectEqual(@as(u32, 0), r.r[1][0]);
    try std.testing.expectEqual(@as(u32, 0), r.r[1][1]);
    try std.testing.expectEqualSlices(u16, &.{ dac.mstp_dac1, dac.mstp_dac0 }, o.disabled[0..o.nd]);
}

test "exit_stop stops at a DAC_B0 failure and otherwise returns DAC_B1's result" {
    var o = Ops{ .fail_on = dac.mstp_dac0 };
    try std.testing.expectEqual(@as(u16, 0x104), dac.exitStop(&o));
    try std.testing.expectEqualStrings("dac_b_exit_stop: mstp0", o.fail_msg.?);
    try std.testing.expectEqual(@as(usize, 1), o.ne);
    var o2 = Ops{ .fail_on = dac.mstp_dac1 };
    try std.testing.expectEqual(@as(u16, 0x104), dac.exitStop(&o2));
    try std.testing.expectEqual(@as(?[]const u8, null), o2.fail_msg);
}

test "the config struct matches ra8_dac_b_cfg_t" {
    try std.testing.expectEqual(@as(usize, 5), @sizeOf(dac.Cfg));
    try std.testing.expectEqual(@as(usize, 2), @offsetOf(dac.Cfg, "internal_output_enabled"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(dac.Cfg, "enable_channel1"));
}
