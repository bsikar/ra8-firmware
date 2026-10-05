//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT up-case table and fold (RA8FW-750).

const std = @import("std");
const fs = @import("ra8_fs");
const up = fs.upcase;
const c = fs.c;
comptime {
    _ = @import("fs_walker_fake.zig");
}

test "ASCII folds only a..z" {
    try std.testing.expectEqual(@as(u16, 'A'), up.priv_exfat_upcase_unit('a'));
    try std.testing.expectEqual(@as(u16, 'Z'), up.priv_exfat_upcase_unit('z'));
    try std.testing.expectEqual(@as(u16, 'A'), up.priv_exfat_upcase_unit('A'));
    try std.testing.expectEqual(@as(u16, '{'), up.priv_exfat_upcase_unit('{'));
    try std.testing.expectEqual(@as(u16, '`'), up.priv_exfat_upcase_unit('`'));
}

test "non-ASCII folds come from the table" {
    const pairs = [_][2]u16{ .{ 0x00E9, 0x00C9 }, .{ 0x00FF, 0x0178 }, .{ 0x03B1, 0x0391 }, .{ 0x0430, 0x0410 }, .{ 0xFF41, 0xFF21 } };
    for (pairs) |p| try std.testing.expectEqual(p[1], up.priv_exfat_upcase_unit(p[0]));
}

test "upper-case, identity-run and past-the-table units map to themselves" {
    const same = [_]u16{ 0x00C9, 0x0391, 0x00D7, 0x4E00, 0xD800, 0xFFFF };
    for (same) |u| try std.testing.expectEqual(u, up.priv_exfat_upcase_unit(u));
}

test "table checksum is the canonical Microsoft value" {
    try std.testing.expectEqual(@as(u32, 0xE619D30D), up.priv_exfat_upcase_checksum());
}

const Disk = struct {
    data: [12 * 512]u8 = undefined,
    writes: u32 = 0,
    fail_at: ?u32 = null,
};

fn writeBlock(ctx: ?*anyopaque, lba: u64, count: u32, buf: [*c]const u8) callconv(.C) u16 {
    const d: *Disk = @ptrCast(@alignCast(ctx.?));
    if (d.fail_at) |f| if (d.writes == f) return c.k_ra8_err_hw_error;
    std.debug.assert(count == 1);
    const at: usize = @intCast((lba - 1000) * 512);
    @memcpy(d.data[at..][0..512], buf[0..512]);
    d.writes += 1;
    return up.ok;
}

fn backendFor(d: *Disk) c.ra8_fs_backend_t {
    var b = std.mem.zeroes(c.ra8_fs_backend_t);
    b.ctx = d;
    b.write_block = writeBlock;
    return b;
}

test "formatter writes the table and zero-pads the last sector" {
    var d = Disk{};
    @memset(&d.data, 0xAA);
    const b = backendFor(&d);
    var cs: u32 = 0;
    try std.testing.expectEqual(up.ok, up.priv_exfat_write_upcase(&b, 1000, 512, &cs));
    try std.testing.expectEqual(@as(u32, 12), d.writes);
    try std.testing.expectEqualSlices(u8, &up.table, d.data[0..up.table_bytes]);
    for (d.data[up.table_bytes..]) |x| try std.testing.expectEqual(@as(u8, 0), x);
    try std.testing.expectEqual(up.priv_exfat_upcase_checksum(), cs);
}

test "formatter stops at the first failed sector write" {
    var d = Disk{ .fail_at = 3 };
    const b = backendFor(&d);
    var cs: u32 = 0x1234;
    try std.testing.expectEqual(@as(u16, c.k_ra8_err_hw_error), up.priv_exfat_write_upcase(&b, 1000, 512, &cs));
    try std.testing.expectEqual(@as(u32, 3), d.writes);
    try std.testing.expectEqual(@as(u32, 0x1234), cs);
}
