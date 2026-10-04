//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/sdramc.zig (RA8FW-608), on a RAM register file.

const std = @import("std");
const sd = @import("sdramc");

const Fake = struct {
    regs: [0x60]u8 = [_]u8{0} ** 0x60,
    prcr_log: [4]u16 = [_]u16{0} ** 4,
    prcr_n: usize = 0,
    sdckocr_val: u8 = 0,
    routed: usize = 0,
    driven: usize = 0,
    first_route: u16 = 0,
    last_route: u16 = 0,
    route_fail_at: ?usize = null,
    drive_fail_at: ?usize = null,
    zero_calls: usize = 0,
    zero_result: u16 = sd.ok,
    sdsr_stuck: u8 = 0,
    errors: usize = 0,
    last_err: []const u8 = "",
    infos: usize = 0,

    pub fn read8(self: *Fake, off: usize) u8 {
        if (off == sd.off_sdsr) return self.regs[off] | self.sdsr_stuck;
        return self.regs[off];
    }
    pub fn write8(self: *Fake, off: usize, v: u8) void {
        self.regs[off] = v;
    }
    pub fn write16(self: *Fake, off: usize, v: u16) void {
        std.mem.writeInt(u16, self.regs[off..][0..2], v, .little);
    }
    pub fn write32(self: *Fake, off: usize, v: u32) void {
        std.mem.writeInt(u32, self.regs[off..][0..4], v, .little);
    }
    pub fn prcr(self: *Fake, v: u16) void {
        self.prcr_log[self.prcr_n] = v;
        self.prcr_n += 1;
    }
    pub fn sdckocr(self: *Fake, v: u8) void {
        self.sdckocr_val = v;
    }
    pub fn route(self: *Fake, p: u16) u16 {
        if (self.route_fail_at) |at| if (self.routed == at) return 0x205;
        if (self.routed == 0) self.first_route = p;
        self.last_route = p;
        self.routed += 1;
        return sd.ok;
    }
    pub fn drive(self: *Fake, _: u16) u16 {
        if (self.drive_fail_at) |at| if (self.driven == at) return 0x103;
        self.driven += 1;
        return sd.ok;
    }
    pub fn zeroBss(self: *Fake) u16 {
        self.zero_calls += 1;
        return self.zero_result;
    }
    pub fn err(self: *Fake, msg: [*:0]const u8) void {
        self.errors += 1;
        self.last_err = std.mem.span(msg);
    }
    pub fn info(self: *Fake, _: [*:0]const u8) void {
        self.infos += 1;
    }
    fn r16(self: *Fake, off: usize) u16 {
        return std.mem.readInt(u16, self.regs[off..][0..2], .little);
    }
    fn r32(self: *Fake, off: usize) u32 {
        return std.mem.readInt(u32, self.regs[off..][0..4], .little);
    }
};

test "bus pin table matches the C order" {
    try std.testing.expectEqual(@as(usize, 57), sd.bus_pins.len);
    try std.testing.expectEqual(@as(u16, 0x0A03), sd.bus_pins[0]);
    try std.testing.expectEqual(@as(u16, 0x0C0F), sd.bus_pins[14]);
    try std.testing.expectEqual(@as(u16, 0x080D), sd.bus_pins[56]);
}

test "init programs the IS42S32160F register image" {
    var f = Fake{};
    try std.testing.expectEqual(sd.ok, sd.init(&f));
    try std.testing.expectEqual(@as(usize, 57), f.routed);
    try std.testing.expectEqual(@as(usize, 57), f.driven);
    try std.testing.expectEqual(@as(u16, 0x0A03), f.first_route);
    try std.testing.expectEqual(@as(u16, 0x080D), f.last_route);
    try std.testing.expectEqual(@as(u16, 0x0088), f.r16(sd.off_sdir));
    try std.testing.expectEqual(@as(u8, 0x11), f.regs[sd.off_sdccr]);
    try std.testing.expectEqual(@as(u8, 1), f.regs[sd.off_sdicr]);
    try std.testing.expectEqual(@as(u8, 1), f.regs[sd.off_sdamod]);
    try std.testing.expectEqual(@as(u8, 0), f.regs[sd.off_sdcmod]);
    try std.testing.expectEqual(@as(u16, 0x0230), f.r16(sd.off_sdmod));
    try std.testing.expectEqual(@as(u32, 0x0005_3703), f.r32(sd.off_sdtr));
    try std.testing.expectEqual(@as(u8, 1), f.regs[sd.off_sdadr]);
    try std.testing.expectEqual(@as(u16, 0xB383), f.r16(sd.off_sdrfcr));
    try std.testing.expectEqual(@as(u8, 1), f.regs[sd.off_sdrfen]);
    try std.testing.expectEqual(@as(usize, 1), f.zero_calls);
    try std.testing.expectEqual(@as(usize, 1), f.infos);
}

test "init unlocks PRCR around the SDCLK output kick" {
    var f = Fake{};
    try std.testing.expectEqual(sd.ok, sd.init(&f));
    try std.testing.expectEqual(@as(usize, 2), f.prcr_n);
    try std.testing.expectEqual(@as(u16, 0xA501), f.prcr_log[0]);
    try std.testing.expectEqual(@as(u16, 0xA500), f.prcr_log[1]);
    try std.testing.expectEqual(@as(u8, 1), f.sdckocr_val);
}

test "init stops on a route conflict before touching registers" {
    var f = Fake{ .route_fail_at = 0 };
    try std.testing.expectEqual(@as(u16, 0x205), sd.init(&f));
    try std.testing.expectEqualStrings("sdramc: bus-pin routing failed", f.last_err);
    try std.testing.expectEqual(@as(u8, 0), f.regs[sd.off_sdccr]);
    try std.testing.expectEqual(@as(usize, 0), f.prcr_n);
}

test "init returns the drive-strength error" {
    var f = Fake{ .drive_fail_at = 3 };
    try std.testing.expectEqual(@as(u16, 0x103), sd.init(&f));
    try std.testing.expectEqual(@as(usize, 4), f.routed);
}

test "init times out on a stuck SDSR bit" {
    var f = Fake{ .sdsr_stuck = sd.sdsr_inist };
    try std.testing.expectEqual(sd.hw_timeout, sd.init(&f));
    try std.testing.expectEqualStrings("sdramc: SDSR status bits never cleared", f.last_err);
    try std.testing.expectEqual(@as(u16, 0), f.r16(sd.off_sdir));
}

test "init reports a zero-fill failure" {
    var f = Fake{ .zero_result = 0x104 };
    try std.testing.expectEqual(@as(u16, 0x104), sd.init(&f));
    try std.testing.expectEqualStrings("sdramc: .sdram_data zero-fill failed", f.last_err);
    try std.testing.expectEqual(@as(usize, 0), f.infos);
}

test "deinit clears SDRFEN and SDCCR" {
    var f = Fake{};
    f.regs[sd.off_sdrfen] = 1;
    f.regs[sd.off_sdccr] = 0x11;
    try std.testing.expectEqual(sd.ok, sd.deinit(&f));
    try std.testing.expectEqual(@as(u8, 0), f.regs[sd.off_sdrfen]);
    try std.testing.expectEqual(@as(u8, 0), f.regs[sd.off_sdccr]);
}

test "set_refresh_interval writes SDRFCR" {
    var f = Fake{};
    try std.testing.expectEqual(sd.ok, sd.setRefreshInterval(&f, 0x1234));
    try std.testing.expectEqual(@as(u16, 0x1234), f.r16(sd.off_sdrfcr));
}

test "get_status reads SDRFEN and rejects null" {
    var f = Fake{};
    f.regs[sd.off_sdrfen] = 1;
    var out: u8 = 0;
    try std.testing.expectEqual(sd.ok, sd.getStatus(&f, &out));
    try std.testing.expectEqual(@as(u8, 1), out);
    try std.testing.expectEqual(sd.null_ptr, sd.getStatus(&f, null));
    try std.testing.expectEqualStrings("out_enabled must not be nullptr", f.last_err);
}

test "stop mode toggles auto-refresh" {
    var f = Fake{};
    try std.testing.expectEqual(sd.ok, sd.exitStop(&f));
    try std.testing.expectEqual(@as(u8, 1), f.regs[sd.off_sdrfen]);
    try std.testing.expectEqual(sd.ok, sd.enterStop(&f));
    try std.testing.expectEqual(@as(u8, 0), f.regs[sd.off_sdrfen]);
}

test "enter_self_refresh checks SDSR and SFEN" {
    var f = Fake{};
    f.regs[sd.off_sdrfen] = 1;
    try std.testing.expectEqual(sd.ok, sd.enterSelfRefresh(&f));
    try std.testing.expectEqual(@as(u8, 0), f.regs[sd.off_sdrfen]);
    try std.testing.expectEqual(@as(u8, 1), f.regs[sd.off_sdself]);
    try std.testing.expectEqual(sd.invalid_state, sd.enterSelfRefresh(&f));
    try std.testing.expectEqualStrings("sdramc: already in self-refresh", f.last_err);
    var busy = Fake{ .sdsr_stuck = sd.sdsr_inist };
    try std.testing.expectEqual(sd.invalid_state, sd.enterSelfRefresh(&busy));
    try std.testing.expectEqualStrings("sdramc: SDSR busy on self-refresh entry", busy.last_err);
}

test "exit_self_refresh checks SFEN and SRFST" {
    var f = Fake{};
    try std.testing.expectEqual(sd.invalid_state, sd.exitSelfRefresh(&f));
    try std.testing.expectEqualStrings("sdramc: not in self-refresh on exit", f.last_err);
    f.regs[sd.off_sdself] = 1;
    try std.testing.expectEqual(sd.ok, sd.exitSelfRefresh(&f));
    try std.testing.expectEqual(@as(u8, 0), f.regs[sd.off_sdself]);
    try std.testing.expectEqual(@as(u8, 1), f.regs[sd.off_sdrfen]);
    var busy = Fake{ .sdsr_stuck = sd.sdsr_srfst };
    busy.regs[sd.off_sdself] = 1;
    try std.testing.expectEqual(sd.invalid_state, sd.exitSelfRefresh(&busy));
    try std.testing.expectEqualStrings("sdramc: SDSR SRFST busy on self-refresh exit", busy.last_err);
}
