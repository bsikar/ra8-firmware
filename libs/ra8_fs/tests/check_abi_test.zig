//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The check scaffolding port (RA8FW-1108): the finding recorder and the
//! visited-bitmap primitives, against a hand-built context.

const std = @import("std");
const fs = @import("ra8_fs");
const check = fs.check;
const c = fs.c;
comptime {
    _ = @import("fs_walker_fake.zig");
}

export fn priv_alloc_eoc_cluster(m: [*c]const u8, out_c: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_c.* = 6;
    return 0;
}

const bad_fat_value: u8 = @intCast(c.k_ra8_fs_check_fault_bad_fat_value);
const crosslink: u8 = @intCast(c.k_ra8_fs_check_fault_crosslink);
const bad_dir_entry: u8 = @intCast(c.k_ra8_fs_check_fault_bad_dir_entry);

const Ctx = struct {
    rep: c.ra8_fs_check_report_t,
    bmp: [8]u8,
    ctx: c.ra8_fs_check_ctx_t,
};

fn freshCtx(t: *Ctx, total: u32, bitmap_bits: u32) void {
    t.rep = std.mem.zeroes(c.ra8_fs_check_report_t);
    t.bmp = .{ 0, 0, 0, 0, 0, 0, 0, 0 };
    t.ctx = std.mem.zeroes(c.ra8_fs_check_ctx_t);
    t.ctx.rep = &t.rep;
    t.ctx.bitmap = &t.bmp;
    t.ctx.bitmap_bits = bitmap_bits;
    t.rep.clusters_total = total;
}

test "the recorder stamps the first finding and counts every finding" {
    var t: Ctx = undefined;
    freshCtx(&t, 4, 0);
    check.priv_check_fault(&t.ctx, bad_fat_value, 7, 9, 3);
    try std.testing.expectEqual(bad_fat_value, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 7), t.rep.first_fault.cluster);
    try std.testing.expectEqual(@as(u64, 9), t.rep.first_fault.lba);
    try std.testing.expectEqual(@as(u32, 3), t.rep.first_fault.entry_off);
    check.priv_check_fault(&t.ctx, crosslink, 2, 0, 0);
    try std.testing.expectEqual(bad_fat_value, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 2), t.rep.faults_total);
}

test "in range brackets the data region [2, 2 + clusters_total)" {
    var t: Ctx = undefined;
    freshCtx(&t, 4, 0);
    try std.testing.expectEqual(@as(u8, 0), check.priv_check_in_range(&t.ctx, 1));
    try std.testing.expectEqual(@as(u8, 1), check.priv_check_in_range(&t.ctx, 2));
    try std.testing.expectEqual(@as(u8, 1), check.priv_check_in_range(&t.ctx, 5));
    try std.testing.expectEqual(@as(u8, 0), check.priv_check_in_range(&t.ctx, 6));
}

test "mark sets one bit per cluster and reports a revisit" {
    var t: Ctx = undefined;
    freshCtx(&t, 64, 64);
    try std.testing.expectEqual(@as(u8, 0), check.priv_check_mark(&t.ctx, 2));
    try std.testing.expectEqual(@as(u8, 1), t.bmp[0] & 1);
    try std.testing.expectEqual(@as(u8, 0), check.priv_check_mark(&t.ctx, 10));
    try std.testing.expectEqual(@as(u8, 1), t.bmp[1] & 1);
    try std.testing.expectEqual(@as(u8, 1), check.priv_check_mark(&t.ctx, 2));
    try std.testing.expectEqual(@as(u8, 0), check.priv_check_mark(&t.ctx, 3));
    try std.testing.expectEqual(@as(u8, 3), t.bmp[0]);
}

test "visit stops on an out-of-range cluster and on a cross-link" {
    var t: Ctx = undefined;
    freshCtx(&t, 4, 8);
    try std.testing.expectEqual(@as(u8, 1), check.priv_check_visit(&t.ctx, 99, bad_dir_entry));
    try std.testing.expectEqual(@as(u32, 1), t.rep.entries_bad);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(bad_dir_entry, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u8, 0), check.priv_check_visit(&t.ctx, 2, bad_dir_entry));
    try std.testing.expectEqual(@as(u8, 1), t.bmp[0] & 1);
    try std.testing.expectEqual(@as(u8, 1), check.priv_check_visit(&t.ctx, 2, bad_dir_entry));
    try std.testing.expectEqual(@as(u32, 1), t.rep.chains_crosslinked);
    try std.testing.expectEqual(bad_dir_entry, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 2), t.rep.faults_total);
}

test "zero bitmap clears exactly the valid range's bytes" {
    var t: Ctx = undefined;
    freshCtx(&t, 33, 33);
    t.bmp[0] = 0xAA;
    t.bmp[1] = 0xAA;
    t.bmp[2] = 0xAA;
    t.bmp[3] = 0xAA;
    t.bmp[4] = 0xAA;
    t.bmp[5] = 0xAA;
    check.priv_check_zero_bitmap(&t.ctx);
    try std.testing.expectEqual(@as(u8, 0), t.bmp[0]);
    try std.testing.expectEqual(@as(u8, 0), t.bmp[1]);
    try std.testing.expectEqual(@as(u8, 0), t.bmp[2]);
    try std.testing.expectEqual(@as(u8, 0), t.bmp[3]);
    try std.testing.expectEqual(@as(u8, 0), t.bmp[4]);
    try std.testing.expectEqual(@as(u8, 0xAA), t.bmp[5]);
}
