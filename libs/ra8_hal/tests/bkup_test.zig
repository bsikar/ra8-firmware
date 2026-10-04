//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/bkup.zig (RA8FW-601).

const std = @import("std");
const b = @import("bkup");

const Op = struct { kind: u8, off: usize, value: u32 };

const Fake = struct {
    mem: [0x1000]u8 = [_]u8{0} ** 0x1000,
    ops: [160]Op = undefined,
    n: usize = 0,
    reads: usize = 0,
    fn log(self: *Fake, kind: u8, off: usize, value: u32) void {
        if (self.n < self.ops.len) self.ops[self.n] = .{ .kind = kind, .off = off, .value = value };
        self.n += 1;
    }
    pub fn read8(self: *Fake, off: usize) u8 {
        self.reads += 1;
        return self.mem[off];
    }
    pub fn write8(self: *Fake, off: usize, value: u8) void {
        self.log('w', off, value);
        self.mem[off] = value;
    }
    pub fn read32(self: *Fake, off: usize) u32 {
        return std.mem.readInt(u32, self.mem[off..][0..4], .little);
    }
    pub fn write32(self: *Fake, off: usize, value: u32) void {
        self.log('W', off, value);
        std.mem.writeInt(u32, self.mem[off..][0..4], value, .little);
    }
    pub fn prcr(self: *Fake, value: u16) void {
        self.log('p', b.off_prcr, value);
    }
};

const Ctx = struct {
    errs: usize = 0,
    fails: usize = 0,
    infos: usize = 0,
    settles: usize = 0,
    initialized: bool = false,
    dispatched: ?u8 = null,
    pub fn err(self: *Ctx, _: [*:0]const u8) void {
        self.errs += 1;
    }
    pub fn fail(self: *Ctx, _: [*:0]const u8, _: u16) void {
        self.fails += 1;
    }
    pub fn info(self: *Ctx, _: [*:0]const u8) void {
        self.infos += 1;
    }
    pub fn settle(self: *Ctx) void {
        self.settles += 1;
    }
    pub fn setInitialized(self: *Ctx, value: bool) void {
        self.initialized = value;
    }
    pub fn isInitialized(self: *Ctx) bool {
        return self.initialized;
    }
    pub fn dispatch(self: *Ctx, flags: u8) void {
        self.dispatched = flags;
    }
};

fn expectOps(f: *const Fake, want: []const Op) !void {
    try std.testing.expectEqual(want.len, f.n);
    for (want, f.ops[0..f.n]) |w, got| try std.testing.expectEqual(w, got);
}

test "struct layouts match the C header" {
    try std.testing.expectEqual(@as(usize, 3), @sizeOf(b.Config));
    try std.testing.expectEqual(@as(usize, 1), @offsetOf(b.Config, "enable_switch"));
    try std.testing.expectEqual(@as(usize, 5), @sizeOf(b.Status));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(b.Status, "raw_vbtbpsr"));
}

test "init with switch and backup arms VDET, enables VBAE and settles" {
    var f = Fake{};
    var c = Ctx{};
    const cfg = b.Config{ .vdet_level = 3, .enable_switch = true, .enable_backup = true };
    try std.testing.expectEqual(b.ok, b.init(&f, &c, &cfg));
    try expectOps(&f, &.{
        .{ .kind = 'p', .off = b.off_prcr, .value = 0xA502 },
        .{ .kind = 'w', .off = b.off_vbtbpcr2, .value = 0x03 },
        .{ .kind = 'w', .off = b.off_vbtbpcr2, .value = 0x13 },
        .{ .kind = 'w', .off = b.off_vbtber, .value = 0x08 },
        .{ .kind = 'w', .off = b.off_vbtbpsr, .value = 0xFE },
        .{ .kind = 'w', .off = b.off_vbtadsr, .value = 0 },
        .{ .kind = 'p', .off = b.off_prcr, .value = 0xA500 },
    });
    try std.testing.expectEqual(@as(usize, 1), c.settles);
    try std.testing.expect(c.initialized);
}

test "init without switch stops it and leaves VBAE clear" {
    var f = Fake{};
    var c = Ctx{};
    try std.testing.expectEqual(b.ok, b.init(&f, &c, &b.Config{}));
    try std.testing.expectEqual(@as(u32, 0x01), f.ops[1].value);
    try std.testing.expectEqual(b.off_vbtbpcr1, f.ops[1].off);
    try std.testing.expectEqual(@as(u8, 0), f.mem[b.off_vbtber]);
    try std.testing.expectEqual(@as(usize, 0), c.settles);
}

test "init rejects null and out-of-range configs before any write" {
    var f = Fake{};
    var c = Ctx{};
    try std.testing.expectEqual(b.null_ptr, b.init(&f, &c, null));
    try std.testing.expectEqual(b.invalid_arg, b.init(&f, &c, &b.Config{ .vdet_level = 6 }));
    try std.testing.expectEqual(@as(usize, 1), c.errs);
    try std.testing.expectEqual(@as(usize, 1), c.fails);
    try std.testing.expectEqual(@as(usize, 0), f.n);
}

test "deinit drops VBAE, stops the switch and clears the flag" {
    var f = Fake{};
    var c = Ctx{ .initialized = true };
    try std.testing.expectEqual(b.ok, b.deinit(&f, &c));
    try std.testing.expectEqual(@as(u8, 0x01), f.mem[b.off_vbtbpcr1]);
    try std.testing.expect(!c.initialized);
}

test "cold start waits for VBPORM then programs the switch" {
    var f = Fake{};
    var c = Ctx{};
    try std.testing.expectEqual(b.hw_timeout, b.coldStartInit(&f, &c, 2, 5));
    try std.testing.expectEqual(@as(usize, 5), f.reads);
    f.mem[b.off_vbtbpsr] = b.vbporm;
    try std.testing.expectEqual(b.ok, b.coldStartInit(&f, &c, 2, 5));
    try std.testing.expectEqual(@as(u8, 0x12), f.mem[b.off_vbtbpcr2]);
    try std.testing.expectEqual(@as(u8, 0), f.mem[b.off_vbtbpcr1]);
    try std.testing.expect(c.initialized);
    try std.testing.expectEqual(b.invalid_arg, b.coldStartInit(&f, &c, 6, 5));
    try std.testing.expectEqual(b.invalid_arg, b.coldStartInit(&f, &c, 1, 0));
}

test "warm start reports VBPORF once VBPORM is up" {
    var f = Fake{};
    var c = Ctx{};
    var reinit = false;
    f.mem[b.off_vbtbpsr] = b.vbporm | b.vbporf;
    try std.testing.expectEqual(b.ok, b.warmStartCheck(&f, &c, &reinit, 1));
    try std.testing.expect(reinit);
    try std.testing.expectEqual(b.null_ptr, b.warmStartCheck(&f, &c, null, 1));
    try std.testing.expectEqual(b.invalid_arg, b.warmStartCheck(&f, &c, &reinit, 0));
}

test "no-switch init waits for VBPORM low and zeroes the tamper paths" {
    var f = Fake{};
    var c = Ctx{};
    f.mem[b.off_vbtbpsr] = b.vbporm;
    try std.testing.expectEqual(b.hw_timeout, b.noSwitchInit(&f, &c, 3));
    f.mem[b.off_vbtbpsr] = 0;
    @memset(f.mem[b.off_vbtadsr .. b.off_vbtictlr2 + 1], 0xFF);
    try std.testing.expectEqual(b.ok, b.noSwitchInit(&f, &c, 3));
    try std.testing.expectEqual(@as(u8, 0x06), f.mem[b.off_vbtbpcr2]);
    for ([_]usize{ b.off_vbtictlr, b.off_vbtictlr2, b.off_vbtadsr, b.off_vbtadcr1, b.off_vbtadcr2 }) |off| {
        try std.testing.expectEqual(@as(u8, 0), f.mem[off]);
    }
    try std.testing.expect(c.initialized);
}

test "get status decodes VBTBPSR and the tamper flags" {
    var f = Fake{};
    var c = Ctx{};
    var st = b.Status{};
    f.mem[b.off_vbtbpsr] = 0x31;
    f.mem[b.off_vbtadsr] = 0xFD;
    try std.testing.expectEqual(b.ok, b.getStatus(&f, &c, &st));
    try std.testing.expectEqual(b.Status{ .source = 1, .vbatt_r_ok = true, .por_detected = true, .tamper_flags = 0x05, .raw_vbtbpsr = 0x31 }, st);
    try std.testing.expectEqual(b.null_ptr, b.getStatus(&f, &c, null));
}

test "clear status is W0C under PRC1 on each register named" {
    var f = Fake{};
    f.mem[b.off_vbtbpsr] = 0x31;
    f.mem[b.off_vbtadsr] = 0x07;
    try std.testing.expectEqual(b.ok, b.clearStatus(&f, 0x03));
    try std.testing.expectEqual(@as(u8, 0x30), f.mem[b.off_vbtbpsr]);
    try std.testing.expectEqual(@as(u8, 0x04), f.mem[b.off_vbtadsr]);
    try std.testing.expectEqual(@as(usize, 6), f.n);
    try std.testing.expectEqual(@as(u32, 0xA502), f.ops[0].value);
}

test "backup words and bytes round-trip inside a PRC1 window" {
    var f = Fake{};
    var c = Ctx{};
    var w: u32 = 0;
    var by: u8 = 0;
    try std.testing.expectEqual(b.ok, b.writeWord(&f, 31, 0xDEAD_BEEF));
    try std.testing.expectEqual(@as(u32, 0xA502), f.ops[0].value);
    try std.testing.expectEqual(b.ok, b.readWord(&f, &c, 31, &w));
    try std.testing.expectEqual(@as(u32, 0xDEAD_BEEF), w);
    try std.testing.expectEqual(b.ok, b.readByte(&f, &c, 127, &by));
    try std.testing.expectEqual(@as(u8, 0xDE), by);
    try std.testing.expectEqual(b.ok, b.writeByte(&f, 0, 0x5A));
    try std.testing.expectEqual(@as(u8, 0x5A), f.mem[b.off_vbtbkr0]);
}

test "backup accessors range-check and null-check" {
    var f = Fake{};
    var c = Ctx{};
    var w: u32 = 0;
    var by: u8 = 0;
    try std.testing.expectEqual(b.invalid_arg, b.readWord(&f, &c, 32, &w));
    try std.testing.expectEqual(b.invalid_arg, b.writeWord(&f, 32, 1));
    try std.testing.expectEqual(b.invalid_arg, b.readByte(&f, &c, 128, &by));
    try std.testing.expectEqual(b.invalid_arg, b.writeByte(&f, 128, 1));
    try std.testing.expectEqual(b.null_ptr, b.readWord(&f, &c, 0, null));
    try std.testing.expectEqual(b.null_ptr, b.readByte(&f, &c, 0, null));
    try std.testing.expectEqual(@as(usize, 2), c.errs);
    try std.testing.expectEqual(@as(usize, 0), f.n);
}

test "zero all clears 128 bytes in one window" {
    var f = Fake{};
    @memset(f.mem[b.off_vbtbkr0 .. b.off_vbtbkr0 + 128], 0xAA);
    try std.testing.expectEqual(b.ok, b.zeroAll(&f));
    try std.testing.expectEqual(@as(usize, 130), f.n);
    for (f.mem[b.off_vbtbkr0 .. b.off_vbtbkr0 + 128]) |v| try std.testing.expectEqual(@as(u8, 0), v);
}

test "voltage monitor uses PRC3 and reads back" {
    var f = Fake{};
    var c = Ctx{};
    var on = false;
    try std.testing.expectEqual(b.ok, b.setVoltageMonitor(&f, true));
    try std.testing.expectEqual(@as(u32, 0xA508), f.ops[0].value);
    try std.testing.expectEqual(b.ok, b.getVoltageMonitor(&f, &c, &on));
    try std.testing.expect(on);
    try std.testing.expectEqual(b.null_ptr, b.getVoltageMonitor(&f, &c, null));
}

test "isr clears and dispatches only enabled flags that fired" {
    var f = Fake{};
    var c = Ctx{};
    try std.testing.expectEqual(b.not_initialized, b.isrHandle(&f, &c));
    c.initialized = true;
    f.mem[b.off_vbtadsr] = 0x06;
    f.mem[b.off_vbtadcr1] = 0x13;
    try std.testing.expectEqual(b.ok, b.isrHandle(&f, &c));
    try std.testing.expectEqual(@as(?u8, 0x02), c.dispatched);
    try std.testing.expectEqual(@as(u8, 0x04), f.mem[b.off_vbtadsr]);
}
