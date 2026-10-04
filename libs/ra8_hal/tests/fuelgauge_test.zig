//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const fg = @import("fuelgauge");

/// Fake MAX17048: 256 big-endian registers, an optional failing register.
const Chip = struct {
    regs: [256]u16 = [_]u16{0} ** 256,
    fail_reg: ?u8 = null,
    fail_code: u16 = 0x205,
    last_addr: u8 = 0,
    reads: u32 = 0,

    fn transfer(ctx: ?*anyopaque, addr: u8, wr: [*]const u8, wr_len: u32, rd: [*]u8, rd_len: u32) callconv(.C) u16 {
        const chip: *Chip = @ptrCast(@alignCast(ctx.?));
        if (wr_len != 1 or rd_len != 2) return 0x103;
        chip.last_addr = addr;
        chip.reads += 1;
        if (chip.fail_reg == wr[0]) return chip.fail_code;
        rd[0] = @truncate(chip.regs[wr[0]] >> 8);
        rd[1] = @truncate(chip.regs[wr[0]]);
        return 0;
    }

    fn cfg(chip: *Chip) fg.Cfg {
        return .{ .bus = .{ .transfer = transfer, .ctx = chip }, .target_7b = fg.default_addr_7b };
    }
};

test "decode scales VCELL, takes the SOC high byte and signs CRATE" {
    const s = fg.decode(0xC800, 0x5A80, 0xFFF0);
    try std.testing.expectEqual(@as(u16, 4000), s.vcell_mv);
    try std.testing.expectEqual(@as(u8, 90), s.soc_pct);
    try std.testing.expectEqual(@as(i16, -16), s.crate_raw);
    try std.testing.expect(!s.charging);
    try std.testing.expect(fg.decode(0, 0, 0).charging);
}

test "open probes VERSION at the configured address and latches the handle" {
    var chip: Chip = .{};
    chip.regs[fg.reg_version] = 0x0012;
    const c = chip.cfg();
    var h: fg.Handle = .{};
    try std.testing.expectEqual(fg.Status.ok, fg.open(&h, &c));
    try std.testing.expect(h.opened);
    try std.testing.expectEqual(@as(u8, 0x36), chip.last_addr);
    try std.testing.expectEqual(@as(u8, 0x36), h.target_7b);
}

test "open rejects a missing transfer, a stuck bus and passes bus errors through" {
    var chip: Chip = .{};
    var h: fg.Handle = .{};
    const no_bus: fg.Cfg = .{ .bus = .{}, .target_7b = 0x36 };
    try std.testing.expectEqual(fg.Status.invalid_arg, fg.open(&h, &no_bus));
    const c = chip.cfg();
    try std.testing.expectEqual(fg.Status.hw_not_ready, fg.open(&h, &c));
    chip.regs[fg.reg_version] = 0xFFFF;
    try std.testing.expectEqual(fg.Status.hw_not_ready, fg.open(&h, &c));
    chip.fail_reg = fg.reg_version;
    try std.testing.expectEqual(@as(u16, 0x205), @intFromEnum(fg.open(&h, &c)));
    try std.testing.expect(!h.opened);
}

test "read decodes the three registers and close clears the handle" {
    var chip: Chip = .{};
    chip.regs[fg.reg_version] = 0x0012;
    chip.regs[fg.reg_vcell] = 0xA000;
    chip.regs[fg.reg_soc] = 0x3200;
    chip.regs[fg.reg_crate] = 0x0010;
    const c = chip.cfg();
    var h: fg.Handle = .{};
    try std.testing.expectEqual(fg.Status.ok, fg.open(&h, &c));
    var s: fg.State = undefined;
    try std.testing.expectEqual(fg.Status.ok, fg.read(&h, &s));
    try std.testing.expectEqual(@as(u16, 3200), s.vcell_mv);
    try std.testing.expectEqual(@as(u8, 50), s.soc_pct);
    try std.testing.expect(s.charging);
    try std.testing.expectEqual(fg.Status.ok, fg.close(&h));
    try std.testing.expect(!h.opened);
    try std.testing.expectEqual(fg.Status.not_initialized, fg.read(&h, &s));
    try std.testing.expectEqual(fg.Status.not_initialized, fg.close(&h));
}

test "read stops at the first failing register" {
    var chip: Chip = .{};
    chip.regs[fg.reg_version] = 0x0012;
    const c = chip.cfg();
    var h: fg.Handle = .{};
    try std.testing.expectEqual(fg.Status.ok, fg.open(&h, &c));
    chip.fail_reg = fg.reg_soc;
    chip.reads = 0;
    var s: fg.State = undefined;
    try std.testing.expectEqual(@as(u16, 0x205), @intFromEnum(fg.read(&h, &s)));
    try std.testing.expectEqual(@as(u32, 2), chip.reads);
}
