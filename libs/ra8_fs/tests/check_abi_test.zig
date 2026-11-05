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
const fake = @import("fs_walker_fake.zig");

const fat32_bad: u32 = 0x0FFF_FFF7;
const fe_ok: u16 = c.k_ra8_ok;

fn mkMount(comptime t: u32) *c.ra8_fs_mount_t {
    fake.reset(@intCast(t));
    const mp = @constCast(fake.mount());
    mp.first_fat_lba = fake.fat_lba;
    mp.fat_size_sectors = fake.fat_secs;
    mp.num_fats = 1;
    return mp;
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

test "the defective-cluster marker follows the mounted FAT type" {
    try std.testing.expectEqual(@as(u32, 0x0FF7), check.internal_fat_bad_marker(mkMount(c.k_ra8_fs_type_fat12)));
    try std.testing.expectEqual(@as(u32, 0xFFF7), check.internal_fat_bad_marker(mkMount(c.k_ra8_fs_type_fat16)));
    try std.testing.expectEqual(fat32_bad, check.internal_fat_bad_marker(mkMount(c.k_ra8_fs_type_fat32)));
}

test "classify tallies free, bad and used, and faults the reserved 1" {
    var t: Ctx = undefined;
    freshCtx(&t, 6, 0);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    check.internal_fat_classify_one(&t.ctx, 2, 0);
    check.internal_fat_classify_one(&t.ctx, 3, fat32_bad);
    check.internal_fat_classify_one(&t.ctx, 4, 0x0FFF_FFFF);
    check.internal_fat_classify_one(&t.ctx, 5, 7);
    check.internal_fat_classify_one(&t.ctx, 6, 1);
    check.internal_fat_classify_one(&t.ctx, 7, 9);
    try std.testing.expectEqual(@as(u32, 1), t.rep.clusters_free);
    try std.testing.expectEqual(@as(u32, 1), t.rep.clusters_bad);
    try std.testing.expectEqual(@as(u32, 4), t.rep.clusters_used);
    try std.testing.expectEqual(@as(u32, 2), t.rep.faults_total);
    try std.testing.expectEqual(bad_fat_value, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 6), t.rep.first_fault.cluster);
}

test "the classify pass sweeps every data cluster through the real getter" {
    var t: Ctx = undefined;
    freshCtx(&t, 6, 0);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.fat[2] = 0;
    fake.fat[3] = fat32_bad;
    fake.fat[4] = 0x0FFF_FFFF;
    fake.fat[5] = 6;
    fake.fat[6] = 0x0FFF_FFFF;
    fake.fat[7] = 1;
    try std.testing.expectEqual(@as(u16, fe_ok), check.internal_fat_classify(&t.ctx));
    try std.testing.expectEqual(@as(u32, 1), t.rep.clusters_free);
    try std.testing.expectEqual(@as(u32, 1), t.rep.clusters_bad);
    try std.testing.expectEqual(@as(u32, 4), t.rep.clusters_used);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(@as(u32, 7), t.rep.first_fault.cluster);
}

const bad_chain: u8 = @intCast(c.k_ra8_fs_check_fault_bad_chain);
const scan_truncated: u8 = @intCast(c.k_ra8_fs_check_fault_scan_truncated);

test "mark chain walks a clean chain to its end of chain" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.fat[2] = 3;
    fake.fat[3] = 4;
    fake.fat[4] = 0x0FFF_FFFF;
    try std.testing.expectEqual(fe_ok, check.internal_fat_mark_chain(&t.ctx, 2));
    try std.testing.expectEqual(@as(u8, 0b0000_0111), t.bmp[0]);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "mark chain reports a cross-link and stops the walk" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.fat[2] = 3;
    fake.fat[3] = 2;
    try std.testing.expectEqual(fe_ok, check.internal_fat_mark_chain(&t.ctx, 2));
    try std.testing.expectEqual(@as(u32, 1), t.rep.chains_crosslinked);
    try std.testing.expectEqual(crosslink, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 2), t.rep.first_fault.cluster);
    try std.testing.expectEqual(@as(u8, 0b0000_0011), t.bmp[0]);
}

test "mark chain faults a broken next-pointer" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.fat[2] = 3;
    fake.fat[3] = 0;
    try std.testing.expectEqual(fe_ok, check.internal_fat_mark_chain(&t.ctx, 2));
    try std.testing.expectEqual(bad_chain, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 3), t.rep.first_fault.cluster);
    try std.testing.expectEqual(@as(u8, 0b0000_0011), t.bmp[0]);
}

test "an out-of-range successor faults in the walk itself" {
    var t: Ctx = undefined;
    freshCtx(&t, 3, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.fat[2] = 3;
    fake.fat[3] = 4;
    fake.fat[4] = 5;
    try std.testing.expectEqual(fe_ok, check.internal_fat_mark_chain(&t.ctx, 2));
    try std.testing.expectEqual(bad_chain, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 4), t.rep.first_fault.cluster);
}

test "a cycle that survives the hop bound is caught by the final visit" {
    var t: Ctx = undefined;
    freshCtx(&t, 3, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.fat[2] = 3;
    fake.fat[3] = 4;
    fake.fat[4] = 2;
    try std.testing.expectEqual(fe_ok, check.internal_fat_mark_chain(&t.ctx, 2));
    try std.testing.expectEqual(crosslink, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 2), t.rep.first_fault.cluster);
}

test "push queues directories and caps the stack once" {
    var t: Ctx = undefined;
    freshCtx(&t, 4, 0);
    var st = std.mem.zeroes(c.fat_dir_stack_t);
    check.internal_fat_push(&t.ctx, &st, 9);
    check.internal_fat_push(&t.ctx, &st, 10);
    try std.testing.expectEqual(@as(u32, 2), st.top);
    try std.testing.expectEqual(@as(u8, 0), st.items[0].is_root);
    try std.testing.expectEqual(@as(u32, 9), st.items[0].cluster);
    try std.testing.expectEqual(@as(u32, 10), st.items[1].cluster);
    st.top = @intCast(c.k_ra8_fs_check_max_dirs);
    check.internal_fat_push(&t.ctx, &st, 11);
    try std.testing.expectEqual(@as(u8, 1), st.truncated);
    try std.testing.expectEqual(scan_truncated, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 11), t.rep.first_fault.cluster);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    check.internal_fat_push(&t.ctx, &st, 12);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
}
