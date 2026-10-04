//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/bkup_tamper.zig (RA8FW-599).

const std = @import("std");
const t = @import("bkup_tamper");

const Op = struct { off: usize, value: u16, kind: u8 };

const Fake = struct {
    mem: [0x1000]u8 = [_]u8{0} ** 0x1000,
    ops: [32]Op = undefined,
    n: usize = 0,
    fn log(self: *Fake, kind: u8, off: usize, value: u16) void {
        self.ops[self.n] = .{ .off = off, .value = value, .kind = kind };
        self.n += 1;
    }
    pub fn read(self: *Fake, off: usize) u8 {
        self.log('r', off, 0);
        return self.mem[off];
    }
    pub fn write(self: *Fake, off: usize, value: u8) void {
        self.log('w', off, value);
        self.mem[off] = value;
    }
    pub fn prcr(self: *Fake, value: u16) void {
        self.log('p', t.off_prcr, value);
    }
};

const Ctx = struct {
    errs: usize = 0,
    fails: usize = 0,
    infos: usize = 0,
    initialized: bool = false,
    rmw_off: usize = 0,
    rmw_mask: u8 = 0,
    rmw_enable: bool = false,
    rmw_unlock: u16 = 0,
    pub fn err(self: *Ctx, _: [*:0]const u8) void {
        self.errs += 1;
    }
    pub fn fail(self: *Ctx, _: [*:0]const u8, _: u16) void {
        self.fails += 1;
    }
    pub fn info(self: *Ctx, _: [*:0]const u8) void {
        self.infos += 1;
    }
    pub fn setInitialized(self: *Ctx) void {
        self.initialized = true;
    }
    pub fn rmw(self: *Ctx, off: usize, mask: u8, enable: bool, unlock: u16) void {
        self.rmw_off = off;
        self.rmw_mask = mask;
        self.rmw_enable = enable;
        self.rmw_unlock = unlock;
    }
};

fn sample() t.Config {
    var cfg = t.Config{ .nc_width = 9 & 7 };
    cfg.channels[0] = .{ .input_enable = true, .noise_canceller_en = true, .irq_enable = true };
    cfg.channels[1] = .{ .edge = 1, .clear_backup = true, .capture_src = 1 };
    cfg.channels[2] = .{ .input_enable = true, .edge = 1, .zeroize_huk = true };
    return cfg;
}

test "config layout matches the C struct" {
    try std.testing.expectEqual(@as(usize, 7), @sizeOf(t.ChanCfg));
    try std.testing.expectEqual(@as(usize, 22), @sizeOf(t.Config));
    try std.testing.expectEqual(@as(usize, 1), @offsetOf(t.Config, "channels"));
    try std.testing.expectEqual(@as(usize, 6), @offsetOf(t.ChanCfg, "capture_src"));
}

test "compose puts each channel on its own bit" {
    const cfg = sample();
    try std.testing.expectEqual(@as(u8, 0x05), t.vbtictlr(&cfg));
    try std.testing.expectEqual(@as(u8, 0x61), t.vbtictlr2(&cfg));
    try std.testing.expectEqual(@as(u8, 0x21), t.vbtadcr1(&cfg));
    try std.testing.expectEqual(@as(u8, 0x02), t.vbtadcr2(&cfg));
    try std.testing.expectEqual(@as(u8, 0x04), t.vbtadcr3(&cfg));
}

test "init writes the HUM 12.3.7.4 sequence inside one PRCR window" {
    var f = Fake{};
    var c = Ctx{};
    var cfg = sample();
    cfg.nc_width = 5;
    try std.testing.expectEqual(t.ok, t.init(&f, &c, &cfg));
    const want = [_]Op{
        .{ .kind = 'p', .off = t.off_prcr, .value = 0xA502 },
        .{ .kind = 'w', .off = t.off_vbtictlr2, .value = 0 },
        .{ .kind = 'w', .off = t.off_vbtadcr1, .value = 0 },
        .{ .kind = 'w', .off = t.off_vbtadcr2, .value = 0 },
        .{ .kind = 'w', .off = t.off_vbtadcr3, .value = 0 },
        .{ .kind = 'w', .off = t.off_vbtictlr, .value = 0x05 },
        .{ .kind = 'w', .off = t.off_vbtncwcr, .value = 5 },
        .{ .kind = 'w', .off = t.off_vbtictlr2, .value = 0x61 },
        .{ .kind = 'r', .off = t.off_vbtadsr, .value = 0 },
        .{ .kind = 'w', .off = t.off_vbtadsr, .value = 0 },
        .{ .kind = 'w', .off = t.off_vbtadcr1, .value = 0x21 },
        .{ .kind = 'w', .off = t.off_vbtadcr2, .value = 0x02 },
        .{ .kind = 'w', .off = t.off_vbtadcr3, .value = 0x04 },
        .{ .kind = 'p', .off = t.off_prcr, .value = 0xA500 },
    };
    try std.testing.expectEqual(want.len, f.n);
    for (want, f.ops[0..f.n]) |w, got| try std.testing.expectEqual(w, got);
    try std.testing.expect(c.initialized);
    try std.testing.expectEqual(@as(usize, 1), c.infos);
}

test "init with a null cfg logs once and touches nothing" {
    var f = Fake{};
    var c = Ctx{};
    try std.testing.expectEqual(t.null_ptr, t.init(&f, &c, null));
    try std.testing.expectEqual(@as(usize, 1), c.errs);
    try std.testing.expectEqual(@as(usize, 0), f.n);
}

test "init refuses a noise width above 1 Hz silently" {
    var f = Fake{};
    var c = Ctx{};
    const cfg = t.Config{ .nc_width = 8 };
    try std.testing.expectEqual(t.invalid_arg, t.init(&f, &c, &cfg));
    try std.testing.expectEqual(@as(usize, 0), c.fails + c.errs + f.n);
    try std.testing.expect(!c.initialized);
}

test "init refuses a bad edge or capture source with two error pairs" {
    var f = Fake{};
    var c = Ctx{};
    var cfg = t.Config{};
    cfg.channels[2].edge = 2;
    try std.testing.expectEqual(t.invalid_arg, t.init(&f, &c, &cfg));
    cfg.channels[2].edge = 0;
    cfg.channels[1].capture_src = 2;
    try std.testing.expectEqual(t.invalid_arg, t.init(&f, &c, &cfg));
    try std.testing.expectEqual(@as(usize, 4), c.fails);
    try std.testing.expectEqual(@as(usize, 0), f.n);
}

test "disable zeroes every tamper register under PRCR" {
    var f = Fake{};
    @memset(&f.mem, 0xFF);
    try std.testing.expectEqual(t.ok, t.disable(&f));
    try std.testing.expectEqual(@as(usize, 8), f.n);
    try std.testing.expectEqual(@as(u16, 0xA502), f.ops[0].value);
    try std.testing.expectEqual(@as(u16, 0xA500), f.ops[7].value);
    for ([_]usize{ t.off_vbtadcr1, t.off_vbtadcr2, t.off_vbtadcr3, t.off_vbtictlr2, t.off_vbtictlr, t.off_vbtadsr }) |off| {
        try std.testing.expectEqual(@as(u8, 0), f.mem[off]);
    }
}

test "readInput reports the VCHnMON bit per channel" {
    var f = Fake{};
    var c = Ctx{};
    f.mem[t.off_vbtimonr] = 0x04;
    var high = false;
    try std.testing.expectEqual(t.ok, t.readInput(&f, &c, 2, &high));
    try std.testing.expect(high);
    try std.testing.expectEqual(t.ok, t.readInput(&f, &c, 0, &high));
    try std.testing.expect(!high);
}

test "readInput checks the pointer before the channel" {
    var f = Fake{};
    var c = Ctx{};
    var high = false;
    try std.testing.expectEqual(t.null_ptr, t.readInput(&f, &c, 9, null));
    try std.testing.expectEqual(@as(usize, 1), c.errs);
    try std.testing.expectEqual(t.invalid_arg, t.readInput(&f, &c, 3, &high));
}

test "setInputEnable hands VBTICTLR to the shared protected rmw" {
    var c = Ctx{};
    try std.testing.expectEqual(t.ok, t.setInputEnable(&c, 1, true));
    try std.testing.expectEqual(t.off_vbtictlr, c.rmw_off);
    try std.testing.expectEqual(@as(u8, 0x02), c.rmw_mask);
    try std.testing.expect(c.rmw_enable);
    try std.testing.expectEqual(@as(u16, 0xA502), c.rmw_unlock);
    try std.testing.expectEqual(t.invalid_arg, t.setInputEnable(&c, 3, false));
}
