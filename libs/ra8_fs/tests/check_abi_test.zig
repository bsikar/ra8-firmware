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

const bad_fat_value: u8 = @intCast(c.k_ra8_fs_check_fault_bad_fat_value);
const crosslink: u8 = @intCast(c.k_ra8_fs_check_fault_crosslink);
const bad_dir_entry: u8 = @intCast(c.k_ra8_fs_check_fault_bad_dir_entry);
const lost_cluster: u8 = @intCast(c.k_ra8_fs_check_fault_lost_cluster);
const bitmap_ref_unset: u8 = c.k_ra8_fs_check_fault_crosslink; // nearest existing fault kind; the real check reports crosslink for a bitmap ref to an unset cluster
const free_count_bad: u8 = @intCast(c.k_ra8_fs_check_fault_free_count_bad);
const bad_set_checksum: u8 = @intCast(c.k_ra8_fs_check_fault_bad_set_checksum);
const bad_name_hash: u8 = @intCast(c.k_ra8_fs_check_fault_bad_name_hash);

const Ctx = struct {
    rep: c.ra8_fs_check_report_t,
    bmp: [16]u8,
    ctx: c.ra8_fs_check_ctx_t,
};

fn freshCtx(t: *Ctx, total: u32, bitmap_bits: u32) void {
    t.rep = std.mem.zeroes(c.ra8_fs_check_report_t);
    t.bmp = @splat(0);
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

fn mkEntry(ent: *[32]u8, name0: u8, attr: u8, first: u32, size: u32) void {
    ent[0] = name0;
    ent[11] = attr;
    c.priv_entry_set_cluster_size(ent, first, size);
}

test "the entry processor stops on the end-of-directory marker" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    var ent = std.mem.zeroes([32]u8);
    var stack = std.mem.zeroes(c.fat_dir_stack_t);
    var eod: u8 = 0;
    try std.testing.expectEqual(fe_ok, check.internal_fat_entry(&t.ctx, &stack, &ent, 100, 32, &eod));
    try std.testing.expectEqual(@as(u8, 1), eod);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
    try std.testing.expectEqual(@as(u32, 0), t.rep.files_visited);
}

test "a deleted slot and an LFN entry pass through untouched" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    var stack = std.mem.zeroes(c.fat_dir_stack_t);
    var eod: u8 = 0;
    var deleted = std.mem.zeroes([32]u8);
    deleted[0] = 0xE5;
    try std.testing.expectEqual(fe_ok, check.internal_fat_entry(&t.ctx, &stack, &deleted, 100, 0, &eod));
    var lfn = std.mem.zeroes([32]u8);
    lfn[0] = 0x41;
    lfn[11] = @intCast(c.k_ra8_fs_attr_lfn);
    try std.testing.expectEqual(fe_ok, check.internal_fat_entry(&t.ctx, &stack, &lfn, 100, 32, &eod));
    try std.testing.expectEqual(@as(u8, 0), eod);
    try std.testing.expectEqual(@as(u32, 0), t.rep.files_visited);
    try std.testing.expectEqual(@as(u32, 0), stack.top);
}

test "a dot entry and a volume label own no chain" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    var stack = std.mem.zeroes(c.fat_dir_stack_t);
    var eod: u8 = 0;
    var dot = std.mem.zeroes([32]u8);
    dot[0] = '.';
    try std.testing.expectEqual(fe_ok, check.internal_fat_entry(&t.ctx, &stack, &dot, 100, 0, &eod));
    var label = std.mem.zeroes([32]u8);
    label[0] = 'V';
    label[11] = @intCast(c.k_ra8_fs_attr_volume_id);
    try std.testing.expectEqual(fe_ok, check.internal_fat_entry(&t.ctx, &stack, &label, 100, 32, &eod));
    try std.testing.expectEqual(@as(u32, 0), t.rep.files_visited);
    try std.testing.expectEqual(@as(u32, 0), stack.top);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "a file entry whose first cluster is out of range faults" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    var stack = std.mem.zeroes(c.fat_dir_stack_t);
    var eod: u8 = 0;
    var ent = std.mem.zeroes([32]u8);
    mkEntry(&ent, 'F', 0x20, 11, 10);
    try std.testing.expectEqual(fe_ok, check.internal_fat_entry(&t.ctx, &stack, &ent, 100, 64, &eod));
    try std.testing.expectEqual(@as(u32, 1), t.rep.entries_bad);
    try std.testing.expectEqual(bad_dir_entry, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 11), t.rep.first_fault.cluster);
    try std.testing.expectEqual(@as(u64, 100), t.rep.first_fault.lba);
    try std.testing.expectEqual(@as(u32, 64), t.rep.first_fault.entry_off);
    try std.testing.expectEqual(@as(u32, 0), t.rep.files_visited);
}

test "a file entry marks its chain and counts the visit" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.fat[2] = 3;
    fake.fat[3] = 0x0FFF_FFFF;
    var stack = std.mem.zeroes(c.fat_dir_stack_t);
    var eod: u8 = 0;
    var ent = std.mem.zeroes([32]u8);
    mkEntry(&ent, 'F', 0x20, 2, 10);
    try std.testing.expectEqual(fe_ok, check.internal_fat_entry(&t.ctx, &stack, &ent, 100, 0, &eod));
    try std.testing.expectEqual(@as(u32, 1), t.rep.files_visited);
    try std.testing.expectEqual(@as(u8, 0b0000_0011), t.bmp[0]);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "a directory entry is queued on the worklist" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    var stack = std.mem.zeroes(c.fat_dir_stack_t);
    var eod: u8 = 0;
    var ent = std.mem.zeroes([32]u8);
    mkEntry(&ent, 'D', @intCast(c.k_ra8_fs_attr_directory), 5, 0);
    try std.testing.expectEqual(fe_ok, check.internal_fat_entry(&t.ctx, &stack, &ent, 100, 0, &eod));
    try std.testing.expectEqual(@as(u32, 1), stack.top);
    try std.testing.expectEqual(@as(u32, 5), stack.items[0].cluster);
    try std.testing.expectEqual(@as(u32, 0), t.rep.files_visited);
}

test "a zero-cluster file counts a visit with nothing to mark" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    var stack = std.mem.zeroes(c.fat_dir_stack_t);
    var eod: u8 = 0;
    var ent = std.mem.zeroes([32]u8);
    mkEntry(&ent, 'F', 0x20, 0, 0);
    try std.testing.expectEqual(fe_ok, check.internal_fat_entry(&t.ctx, &stack, &ent, 100, 0, &eod));
    try std.testing.expectEqual(@as(u32, 1), t.rep.files_visited);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
    try std.testing.expectEqual(@as(u8, 0), t.bmp[0]);
}

test "the sector walk stops at the first end-of-directory marker" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.fat[2] = 0x0FFF_FFFF;
    var buf = std.mem.zeroes([512]u8);
    var stack = std.mem.zeroes(c.fat_dir_stack_t);
    mkEntry(buf[0..32], 'F', 0x20, 2, 1);
    buf[32] = 0xE5;
    buf[64] = 0xE5;
    var eod: u8 = 0;
    try std.testing.expectEqual(fe_ok, check.internal_fat_visit_sector(&t.ctx, &stack, &buf, 300, &eod));
    try std.testing.expectEqual(@as(u8, 1), eod);
    try std.testing.expectEqual(@as(u32, 1), t.rep.files_visited);
    try std.testing.expectEqual(@as(u8, 0b0000_0001), t.bmp[0]);
}

test "a full sector without an end marker walks every entry" {
    var t: Ctx = undefined;
    freshCtx(&t, 32, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    var i: u32 = 0;
    while (i < 16) : (i += 1) {
        fake.fat[2 + i] = 0x0FFF_FFFF;
    }
    var buf = std.mem.zeroes([512]u8);
    var stack = std.mem.zeroes(c.fat_dir_stack_t);
    i = 0;
    while (i < 16) : (i += 1) {
        mkEntry(buf[@as(usize, i) * 32 ..][0..32], 'F', 0x20, 2 + i, 1);
    }
    var eod: u8 = 9;
    try std.testing.expectEqual(fe_ok, check.internal_fat_visit_sector(&t.ctx, &stack, &buf, 300, &eod));
    try std.testing.expectEqual(@as(u8, 0), eod);
    try std.testing.expectEqual(@as(u32, 16), t.rep.files_visited);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
    try std.testing.expectEqual(@as(u8, 0xFF), t.bmp[0]);
    try std.testing.expectEqual(@as(u8, 0xFF), t.bmp[1]);
}

test "the fixed-root walk serves the root region and marks one file" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat16);
    fake.sector[0..512].* = @splat(0);
    var ent = std.mem.zeroes([32]u8);
    mkEntry(&ent, 'F', 0x20, 2, 1);
    @memcpy(fake.sector[0..32], &ent);
    fake.fat[2] = 0x0000_FFF8;
    var stack = std.mem.zeroes(c.fat_dir_stack_t);
    try std.testing.expectEqual(fe_ok, check.internal_fat_scan_fixed_root(&t.ctx, &stack));
    try std.testing.expectEqual(@as(u32, 1), t.rep.files_visited);
    try std.testing.expectEqual(@as(u8, 0b0000_0001), t.bmp[0]);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "the tree walks the FAT32 root and marks the file chain" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.sector[0..512].* = @splat(0);
    var ent = std.mem.zeroes([32]u8);
    mkEntry(&ent, 'F', 0x20, 2, 1);
    @memcpy(fake.sector[0..32], &ent);
    fake.fat[2] = 0x0FFF_FFFF;
    try std.testing.expectEqual(fe_ok, check.internal_fat_tree(&t.ctx));
    try std.testing.expectEqual(@as(u32, 1), t.rep.dirs_visited);
    try std.testing.expectEqual(@as(u32, 1), t.rep.files_visited);
    try std.testing.expectEqual(@as(u8, 0b0000_1001), t.bmp[0]);
}

test "a directory that points at itself faults on the revisit" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.sector[0..512].* = @splat(0);
    var ent = std.mem.zeroes([32]u8);
    mkEntry(&ent, 'D', @intCast(c.k_ra8_fs_attr_directory), 6, 0);
    @memcpy(fake.sector[0..32], &ent);
    fake.fat[6] = 0x0FFF_FFFF;
    try std.testing.expectEqual(fe_ok, check.internal_fat_tree(&t.ctx));
    try std.testing.expectEqual(@as(u32, 3), t.rep.dirs_visited);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(crosslink, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 6), t.rep.first_fault.cluster);
    try std.testing.expectEqual(@as(u8, 0b0001_1000), t.bmp[0]);
}

test "a mutual cross-link between directory chains stops the walk" {
    var t: Ctx = undefined;
    freshCtx(&t, 32, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.sector[0..512].* = @splat(0xE5);
    fake.fat[5] = 6;
    fake.fat[6] = 5;
    var stack = std.mem.zeroes(c.fat_dir_stack_t);
    try std.testing.expectEqual(fe_ok, check.internal_fat_scan_cluster_dir(&t.ctx, &stack, 5));
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(crosslink, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 5), t.rep.first_fault.cluster);
    try std.testing.expectEqual(@as(u8, 0b0001_1000), t.bmp[0]);
}

test "the diff pass flags allocated clusters no chain reached" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.fat = @splat(0);
    t.bmp[0] = 0b0000_0001; // cluster 2 was visited by the tree walk
    fake.fat[3] = 0x0FFF_FFFF; // allocated, unvisited: lost
    fake.fat[4] = 0; // free, skipped
    fake.fat[6] = 0x0FFF_FFF7; // defective marker, skipped
    try std.testing.expectEqual(fe_ok, check.internal_fat_diff(&t.ctx));
    try std.testing.expectEqual(@as(u32, 1), t.rep.clusters_lost);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(lost_cluster, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 3), t.rep.first_fault.cluster);
}

test "the FSInfo compare flags a mismatched free count" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    t.ctx.m[0].reserved_sectors = 8;
    t.rep.clusters_free = 0x1235;
    fake.sector[0..512].* = @splat(0);
    fake.sector[@as(usize, @intCast(c.k_fmt_off_f32_fsinfo))] = 3;
    const lead: usize = @intCast(c.k_fmt_fsi_off_lead);
    const strt: usize = @intCast(c.k_fmt_fsi_off_struct);
    const free: usize = @intCast(c.k_fmt_fsi_off_free);
    const tail: usize = @intCast(c.k_fmt_fsi_off_trail);
    fake.disk[2][lead..][0..4].* = .{ 0x52, 0x52, 0x61, 0x41 }; // 0x41615252
    fake.disk[2][strt..][0..4].* = .{ 0x72, 0x72, 0x41, 0x61 }; // 0x61417272
    fake.disk[2][free..][0..4].* = .{ 0x34, 0x12, 0x00, 0x00 }; // 0x1234
    fake.disk[2][tail..][0..4].* = .{ 0x00, 0x00, 0x55, 0xAA }; // 0xAA550000
    try std.testing.expectEqual(fe_ok, check.internal_fat_fsinfo(&t.ctx));
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(free_count_bad, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 3), t.rep.first_fault.lba);
}

test "an untrusted FSInfo sector is not a fault" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    t.ctx.m[0].reserved_sectors = 8;
    t.rep.clusters_free = 0x1234;
    fake.sector[0..512].* = @splat(0);
    fake.sector[@as(usize, @intCast(c.k_fmt_off_f32_fsinfo))] = 3;
    fake.disk[2][488..][0..4].* = .{ 0x99, 0x99, 0x00, 0x00 }; // disagrees
    // no signatures: nothing to compare, no fault
    try std.testing.expectEqual(fe_ok, check.internal_fat_fsinfo(&t.ctx));
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "priv_check_fat runs classify, tree, diff and fsinfo in order" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.sector[0..512].* = @splat(0); // boot sector: no FSInfo pointer, skipped
    fake.fat = @splat(0);
    fake.fat[2] = 0; // free
    fake.fat[3] = 0x0FFF_FFFF; // allocated, never reached: lost
    fake.fat[5] = 0x0FFF_FFFF; // the FAT32 root chain ends after its first cluster
    try std.testing.expectEqual(fe_ok, check.priv_check_fat(&t.ctx));
    try std.testing.expectEqual(@as(u32, 1), t.rep.dirs_visited);
    try std.testing.expectEqual(@as(u32, 1), t.rep.clusters_lost);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(lost_cluster, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 3), t.rep.first_fault.cluster);
}

// ---------------------------------------------------------------------------
// exFAT marking primitives (RA8FW-1114)
// ---------------------------------------------------------------------------

test "mark run stamps a contiguous run and reports its cross-link" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 64);
    check.internal_exchk_mark_run(&t.ctx, 2, 3);
    try std.testing.expectEqual(@as(u8, 0b0000_0111), t.bmp[0]);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
    check.internal_exchk_mark_run(&t.ctx, 3, 1);
    try std.testing.expectEqual(@as(u32, 1), t.rep.chains_crosslinked);
    try std.testing.expectEqual(crosslink, @as(u8, @intCast(t.rep.first_fault.kind)));
}

test "mark run faults an out-of-range first cluster" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 64);
    check.internal_exchk_mark_run(&t.ctx, 1, 5);
    try std.testing.expectEqual(@as(u32, 1), t.rep.entries_bad);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(@as(u8, 0), t.bmp[0]);
}

test "mark run clamps a run longer than the in-volume suffix" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 64);
    // clusters 2..9; a run starting at 6 has only 4 in-volume clusters left
    check.internal_exchk_mark_run(&t.ctx, 6, 100);
    try std.testing.expectEqual(@as(u8, 0b1111_0000), t.bmp[0]);
    try std.testing.expectEqual(@as(u32, 1), t.rep.entries_bad);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(bad_dir_entry, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 10), t.rep.first_fault.cluster);
}

test "mark fatchain follows the real FAT to its end of chain" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.fat[2] = 3;
    fake.fat[3] = 0x0FFF_FFFF;
    try std.testing.expectEqual(fe_ok, check.internal_exchk_mark_fatchain(&t.ctx, 2));
    try std.testing.expectEqual(@as(u8, 0b0000_0011), t.bmp[0]);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "mark fatchain faults a bad chain instead of walking it" {
    var t: Ctx = undefined;
    freshCtx(&t, 3, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_fat32);
    fake.fat[2] = 3;
    fake.fat[3] = 4;
    fake.fat[4] = 5;
    try std.testing.expectEqual(fe_ok, check.internal_exchk_mark_fatchain(&t.ctx, 2));
    try std.testing.expectEqual(bad_chain, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 4), t.rep.first_fault.cluster);
}

test "mark dir alloc walks the contiguous run to its end" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_exfat);
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 2;
    dir.contig_end = 4;
    try std.testing.expectEqual(fe_ok, check.internal_exchk_mark_dir_alloc(&t.ctx, &dir));
    try std.testing.expectEqual(@as(u8, 0b0000_0011), t.bmp[0]);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "mark dir alloc follows the FAT chain and stops at end of chain" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_exfat);
    fake.fat[2] = 3;
    fake.fat[3] = 0xFFFF_FFFF;
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 2;
    try std.testing.expectEqual(fe_ok, check.internal_exchk_mark_dir_alloc(&t.ctx, &dir));
    try std.testing.expectEqual(@as(u8, 0b0000_0011), t.bmp[0]);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "the system run marks ceil(DataLength / cluster) clusters" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_exfat);
    var e = std.mem.zeroes([32]u8);
    e[0x14] = 2; // FirstCluster = 2
    e[0x19] = 4; // DataLength = 0x0400 = 1024 bytes = 2 clusters
    check.internal_exchk_system_run(&t.ctx, &e);
    try std.testing.expectEqual(@as(u8, 0b0000_0011), t.bmp[0]);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "a system entry with no first cluster is a no-op" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    t.ctx.m = mkMount(c.k_ra8_fs_type_exfat);
    var e = std.mem.zeroes([32]u8);
    e[0x19] = 4;
    check.internal_exchk_system_run(&t.ctx, &e);
    try std.testing.expectEqual(@as(u8, 0), t.bmp[0]);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

// --- exFAT entry-set verification (RA8FW-1115) ---

test "extract name pulls units across the name entries" {
    var set = std.mem.zeroes([4 * 32]u8);
    const name = "ABCDEFGHIJKLMNOPQRST";
    for (name, 0..) |ch, i| {
        const ent = 2 + i / 15;
        const col = i % 15;
        set[ent * 32 + 2 + col * 2] = ch;
    }
    var units: [64]u16 = @splat(0);
    check.internal_exchk_extract_name(&set, 20, &units);
    try std.testing.expectEqual(@as(u16, 'A'), units[0]);
    try std.testing.expectEqual(@as(u16, 'O'), units[14]);
    try std.testing.expectEqual(@as(u16, 'P'), units[15]);
    try std.testing.expectEqual(@as(u16, 'T'), units[19]);
}

extern fn priv_exfat_set_checksum(set: [*c]const u8, bytes: u32) callconv(.c) u16;

/// Stamp the real exFAT SetChecksum over the first `count` entries: the
/// stub-era fixture wrote a constant that can never match the ported
/// checksum (csum32's pattern again), so every staged set faulted once.
fn restampChecksum(set: *[7 * 32]u8, count: u32) void {
    const cs = priv_exfat_set_checksum(set, count * 32);
    set[2] = @truncate(cs);
    set[3] = @truncate(cs >> 8);
}

fn stageSetName(set: *[7 * 32]u8, name: []const u8, count: u32) u16 {
    set[4] = 0x20; // File attributes
    for (name, 0..) |ch, i| {
        const ent = 2 + i / 15;
        const col = i % 15;
        set[ent * 32 + 2 + col * 2] = ch;
    }
    set[32 + 3] = @intCast(name.len);
    var units: [64]u16 = @splat(0);
    for (name, 0..) |ch, i| units[i] = ch;
    const h = fs.exfat_hash.priv_exfat_name_hash(&units, @intCast(name.len));
    set[32 + 4] = @truncate(h);
    set[32 + 5] = @truncate(h >> 8);
    restampChecksum(set, count);
    return h;
}

test "verify set accepts a clean entry set" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    var set = std.mem.zeroes([7 * 32]u8);
    _ = stageSetName(&set, "HELLO", 3);
    check.internal_exchk_verify_set(&t.ctx, &set, 3, 9, 7);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
    try std.testing.expectEqual(@as(u32, 0), t.rep.entries_bad);
}

test "verify set faults a bad set checksum at the set's offset" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    var set = std.mem.zeroes([7 * 32]u8);
    _ = stageSetName(&set, "HELLO", 3);
    set[2] = 0x21;
    check.internal_exchk_verify_set(&t.ctx, &set, 3, 9, 7);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(@as(u32, 1), t.rep.entries_bad);
    try std.testing.expectEqual(bad_set_checksum, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u64, 9), t.rep.first_fault.lba);
    try std.testing.expectEqual(@as(u32, 7), t.rep.first_fault.entry_off);
}

test "verify set faults a bad name hash" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    var set = std.mem.zeroes([7 * 32]u8);
    const h = stageSetName(&set, "HELLO", 3);
    set[32 + 4] = @truncate(h +% 1);
    restampChecksum(&set, 3); // the corrupt hash keeps a self-consistent checksum, so only the hash faults
    check.internal_exchk_verify_set(&t.ctx, &set, 3, 9, 7);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(@as(u32, 1), t.rep.entries_bad);
    try std.testing.expectEqual(bad_name_hash, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u64, 9), t.rep.first_fault.lba);
    try std.testing.expectEqual(@as(u32, 7), t.rep.first_fault.entry_off);
}

test "verify set clamps nlen to the name cap" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    var set = std.mem.zeroes([7 * 32]u8);
    const name64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    const h = stageSetName(&set, name64, 7);
    set[32 + 3] = 0xFF; // a corrupt length: the clamp bounds the read at 64
    restampChecksum(&set, 7); // re-stamp over the corrupt length: only the clamp behavior is under test
    try std.testing.expectEqual(@as(u32, 64), name64.len);
    check.internal_exchk_verify_set(&t.ctx, &set, 7, 0, 0);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
    _ = h;
}

fn stageExfatStream(set: *[7 * 32]u8, first: u32, dlen: u64, no_fat: bool) void {
    const so_flags: usize = @intCast(c.k_exfat_strm_off_flags);
    const so_clus: usize = @intCast(c.k_exfat_strm_off_clus);
    const so_dlen: usize = @intCast(c.k_exfat_strm_off_dlen);
    if (no_fat) set[32 + so_flags] |= @as(u8, @intCast(c.k_exfat_secflag_no_fat));
    var v: u32 = first;
    for (0..4) |i| {
        set[32 + so_clus + i] = @truncate(v);
        v >>= 8;
    }
    var d: u64 = dlen;
    for (0..8) |i| {
        set[32 + so_dlen + i] = @truncate(d);
        d >>= 8;
    }
}

test "the exFAT push queues directories and caps the stack once" {
    var t: Ctx = undefined;
    freshCtx(&t, 4, 0);
    var items: [4]c.exfat_dir_t = undefined;
    var st = std.mem.zeroes(c.exfat_dir_stack_t);
    st.items = &items;
    var d = std.mem.zeroes(c.exfat_dir_t);
    d.cluster = 9;
    check.internal_exchk_push(&t.ctx, &st, &d);
    d.cluster = 10;
    check.internal_exchk_push(&t.ctx, &st, &d);
    try std.testing.expectEqual(@as(u32, 2), st.top);
    try std.testing.expectEqual(@as(u32, 9), st.items[0].cluster);
    try std.testing.expectEqual(@as(u32, 10), st.items[1].cluster);
    st.top = @intCast(c.k_ra8_fs_check_max_dirs);
    d.cluster = 11;
    check.internal_exchk_push(&t.ctx, &st, &d);
    try std.testing.expectEqual(@as(u8, 1), st.truncated);
    try std.testing.expectEqual(scan_truncated, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 11), t.rep.first_fault.cluster);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    d.cluster = 12;
    check.internal_exchk_push(&t.ctx, &st, &d);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
}

test "the exFAT set walk counts an empty file and marks nothing" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    var set = std.mem.zeroes([7 * 32]u8);
    _ = stageSetName(&set, "EMPTY", 7);
    var st = std.mem.zeroes(c.exfat_dir_stack_t);
    var items: [4]c.exfat_dir_t = undefined;
    st.items = &items;
    const r = check.internal_exchk_set_clusters(&t.ctx, &set, &set, &st, 9, 7);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u32, 1), t.rep.files_visited);
    try std.testing.expectEqual(@as(u8, 0), t.bmp[0]);
}

test "the exFAT set walk faults an out-of-range first cluster" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    var set = std.mem.zeroes([7 * 32]u8);
    _ = stageSetName(&set, "FAR", 7);
    stageExfatStream(&set, 55, 512, false);
    var st = std.mem.zeroes(c.exfat_dir_stack_t);
    var items: [4]c.exfat_dir_t = undefined;
    st.items = &items;
    const r = check.internal_exchk_set_clusters(&t.ctx, &set, &set, &st, 9, 7);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u32, 1), t.rep.entries_bad);
    try std.testing.expectEqual(bad_dir_entry, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 55), t.rep.first_fault.cluster);
}

test "the exFAT set walk queues a subdirectory on the worklist" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    const mp = mkMount(c.k_ra8_fs_type_exfat);
    t.ctx.m = mp;
    var set = std.mem.zeroes([7 * 32]u8);
    _ = stageSetName(&set, "SUBDIR", 7);
    set[4] |= @as(u8, @intCast(c.k_ra8_fs_attr_directory));
    stageExfatStream(&set, 5, 0, false);
    var st = std.mem.zeroes(c.exfat_dir_stack_t);
    var items: [4]c.exfat_dir_t = undefined;
    st.items = &items;
    const r = check.internal_exchk_set_clusters(&t.ctx, &set, &set, &st, 9, 7);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u32, 1), st.top);
    try std.testing.expectEqual(@as(u32, 5), st.items[0].cluster);
    try std.testing.expectEqual(@as(u32, 0), t.rep.files_visited);
}

test "the exFAT set walk marks a contiguous file run" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    const mp = mkMount(c.k_ra8_fs_type_exfat);
    t.ctx.m = mp;
    var set = std.mem.zeroes([7 * 32]u8);
    _ = stageSetName(&set, "CONTIG", 7);
    stageExfatStream(&set, 5, 512, true);
    var st = std.mem.zeroes(c.exfat_dir_stack_t);
    var items: [4]c.exfat_dir_t = undefined;
    st.items = &items;
    const r = check.internal_exchk_set_clusters(&t.ctx, &set, &set, &st, 9, 7);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u32, 1), t.rep.files_visited);
    try std.testing.expectEqual(@as(u8, 0x08), t.bmp[0]);
}

test "the exFAT set walk follows the FAT to a chained file's clusters" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    const mp = mkMount(c.k_ra8_fs_type_exfat);
    t.ctx.m = mp;
    fake.fat[5] = 0xFFFF_FFFF;
    var set = std.mem.zeroes([7 * 32]u8);
    _ = stageSetName(&set, "CHAIN", 7);
    stageExfatStream(&set, 5, 512, false);
    var st = std.mem.zeroes(c.exfat_dir_stack_t);
    var items: [4]c.exfat_dir_t = undefined;
    st.items = &items;
    const r = check.internal_exchk_set_clusters(&t.ctx, &set, &set, &st, 9, 7);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u32, 1), t.rep.files_visited);
    try std.testing.expectEqual(@as(u8, 0x08), t.bmp[0]);
}

test "the exFAT scan stops on the end-of-directory marker" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    const mp = mkMount(c.k_ra8_fs_type_exfat);
    t.ctx.m = mp;
    mp.first_data_lba = fake.dir_lba;
    for (&fake.dir) |*row| @memset(row, 0);
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 2;
    var st = std.mem.zeroes(c.exfat_dir_stack_t);
    var items: [4]c.exfat_dir_t = undefined;
    st.items = &items;
    const r = check.internal_exchk_scan_dir(&t.ctx, &dir, &st);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "the exFAT scan dispatches a system entry and keeps walking" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    const mp = mkMount(c.k_ra8_fs_type_exfat);
    t.ctx.m = mp;
    mp.first_data_lba = fake.dir_lba;
    for (&fake.dir) |*row| @memset(row, 0);
    fake.dir[0][0] = @intCast(c.k_exfat_entry_upcase);
    fake.dir[0][1] = @intCast(c.k_exfat_secflag_no_fat);
    var v: u32 = 5;
    for (0..4) |i| {
        fake.dir[0][20 + i] = @truncate(v);
        v >>= 8;
    }
    var d: u64 = 512;
    for (0..8) |i| {
        fake.dir[0][24 + i] = @truncate(d);
        d >>= 8;
    }
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 2;
    var st = std.mem.zeroes(c.exfat_dir_stack_t);
    var items: [4]c.exfat_dir_t = undefined;
    st.items = &items;
    const r = check.internal_exchk_scan_dir(&t.ctx, &dir, &st);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u8, 0x08), t.bmp[0]);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "the exFAT terminal probe accepts an end marker past the ceiling" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    const mp = mkMount(c.k_ra8_fs_type_exfat);
    t.ctx.m = mp;
    mp.first_data_lba = fake.dir_lba;
    for (&fake.dir) |*row| @memset(row, 0);
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 2;
    var cur = std.mem.zeroes(c.exfat_cursor_t);
    cur.cluster = 2;
    cur.scanned = @intCast(c.k_exfat_scan_limit);
    const r = check.internal_exchk_scan_dir_terminal(&t.ctx, &cur, &dir);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "the exFAT terminal probe faults a live entry past the ceiling" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    const mp = mkMount(c.k_ra8_fs_type_exfat);
    t.ctx.m = mp;
    mp.first_data_lba = fake.dir_lba;
    for (&fake.dir) |*row| @memset(row, 0);
    fake.dir[0][0] = 0x83; // a live volume-label entry past the bound
    var dir = std.mem.zeroes(c.exfat_dir_t);
    dir.cluster = 2;
    var cur = std.mem.zeroes(c.exfat_cursor_t);
    cur.cluster = 2;
    cur.scanned = @intCast(c.k_exfat_scan_limit);
    const r = check.internal_exchk_scan_dir_terminal(&t.ctx, &cur, &dir);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(scan_truncated, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 2), t.rep.first_fault.cluster);
}

test "the exFAT tree walks the root and marks a file's run" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    const mp = mkMount(c.k_ra8_fs_type_exfat);
    t.ctx.m = mp;
    mp.first_data_lba = fake.dir_lba;
    mp.root_cluster = 2;
    fake.fat[2] = 0xFFFF_FFFF;
    var set = std.mem.zeroes([7 * 32]u8);
    _ = stageSetName(&set, "REPORT", 7);
    set[0] = @intCast(c.k_exfat_entry_file); // the File entry type byte the scan dispatches on
    set[1] = 6;
    stageExfatStream(&set, 5, 512, true);
    restampChecksum(&set, 7);
    for (&fake.dir) |*row| @memset(row, 0);
    for (0..7) |i| fake.dir[i] = set[i * 32 ..][0..32].*;
    const r = check.internal_exchk_tree(&t.ctx);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u32, 1), t.rep.dirs_visited);
    try std.testing.expectEqual(@as(u32, 1), t.rep.files_visited);
    try std.testing.expectEqual(@as(u8, 0x09), t.bmp[0]);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "the exFAT bitmap pass counts used and free clusters" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    const mp = mkMount(c.k_ra8_fs_type_exfat);
    t.ctx.m = mp;
    t.ctx.bitmap = null;
    @memset(&fake.bitmap, 0);
    fake.bitmap[0] = 0x09;
    fake.bitmap[1] = 0x80;
    fake.bitmap[2] = 0xF0;
    const r = check.internal_exchk_bitmap_pass(&t.ctx, fake.bitmap_lba);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u32, 3), t.rep.clusters_used);
    try std.testing.expectEqual(@as(u32, 17), t.rep.clusters_free);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "the exFAT bitmap diff flags lost clusters and unset references" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    const mp = mkMount(c.k_ra8_fs_type_exfat);
    t.ctx.m = mp;
    t.bmp[0] = 0x09;
    @memset(&fake.bitmap, 0);
    fake.bitmap[0] = 0x03;
    const r = check.internal_exchk_bitmap_pass(&t.ctx, fake.bitmap_lba);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u32, 1), t.rep.clusters_lost);
    try std.testing.expectEqual(@as(u32, 1), t.rep.bitmap_mismatches);
    try std.testing.expectEqual(@as(u32, 2), t.rep.faults_total);
    try std.testing.expectEqual(lost_cluster, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 3), t.rep.first_fault.cluster);
    // the ported report carries faults_total + first_fault only; the second
    // fault (bitmap_ref_unset) is covered by the count assertion above
}

test "the exFAT check reports a missing allocation bitmap entry set" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    const mp = mkMount(c.k_ra8_fs_type_exfat);
    t.ctx.m = mp;
    for (&fake.dir) |*row| @memset(row, 0); // a root with no bitmap entry: find_bitmap ends at the end marker
    fake.bitmap_err = @intCast(c.k_ra8_err_not_found);
    defer fake.bitmap_err = 0;
    const r = check.priv_check_exfat(&t.ctx);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u32, 1), t.rep.entries_bad);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(bad_dir_entry, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 0), t.rep.dirs_visited);
}

test "the exFAT check walks the tree and diffs a consistent bitmap" {
    var t: Ctx = undefined;
    // The visited bitmap must reach the bitmap entry's own run: 112 data
    // clusters so cluster 98 stays in range, 128 bitmap bits to hold it.
    freshCtx(&t, 112, 128);
    const mp = mkMount(c.k_ra8_fs_type_exfat);
    t.ctx.m = mp;
    mp.first_data_lba = fake.dir_lba;
    mp.root_cluster = 2;
    fake.fat[2] = 0xFFFF_FFFF;
    // The bitmap entry's cluster must map into the fake's bitmap window:
    // cluster_to_lba is dir_lba + (cluster - 2), so cluster 98 reads LBA 100.
    std.mem.writeInt(u32, fake.dir[0][@intCast(c.k_exfat_strm_off_clus)..][0..4], 98, .little);
    var set = std.mem.zeroes([7 * 32]u8);
    // A File + Stream set: the 8-slot dir fixture holds the bitmap entry, the
    // up-case entry and this set, and the name carries no units so the stream
    // entry alone defines the set.
    _ = stageSetName(&set, "", 2);
    set[0] = @intCast(c.k_exfat_entry_file);
    set[1] = 1;
    stageExfatStream(&set, 5, 512, true);
    restampChecksum(&set, 2);
    fake.dir[2] = set[0..32][0..32].*;
    fake.dir[3] = set[32..64][0..32].*;
    @memset(&fake.bitmap, 0);
    fake.bitmap[0] = 0x09; // the root cluster 2 and the file's cluster 5
    fake.bitmap[12] = 0x03; // the bitmap entry's own run, clusters 98-99
    const r = check.priv_check_exfat(&t.ctx);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_ok)), r);
    try std.testing.expectEqual(@as(u32, 1), t.rep.dirs_visited);
    try std.testing.expectEqual(@as(u32, 1), t.rep.files_visited);
    try std.testing.expectEqual(@as(u32, 4), t.rep.clusters_used);
    try std.testing.expectEqual(@as(u32, 108), t.rep.clusters_free);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "the bitmap gate requires one bit per data cluster" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    try std.testing.expectEqual(false, check.internal_check_bitmap_ok(20, null, 8));
    try std.testing.expectEqual(false, check.internal_check_bitmap_ok(20, &t.bmp, 2));
    try std.testing.expectEqual(true, check.internal_check_bitmap_ok(20, &t.bmp, 3));
    try std.testing.expectEqual(true, check.internal_check_bitmap_ok(20, &t.bmp, 8));
}

test "the locked body rejects null arguments and an unmounted handle" {
    var t: Ctx = undefined;
    freshCtx(&t, 20, 64);
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_err_null_ptr)), check.internal_check_locked(null, &t.bmp, 8, &t.rep));
    const mp = mkMount(c.k_ra8_fs_type_fat32);
    mp.in_use = 0; // the fake mounts in_use = 1; the guard wants an unmounted handle
    try std.testing.expectEqual(@as(u16, @intCast(c.k_ra8_err_invalid_state)), check.internal_check_locked(mp, &t.bmp, 8, &t.rep));
}

test "the entry point seeds the report and dispatches to the FAT scan" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    const mp = mkMount(c.k_ra8_fs_type_fat32);
    mp.in_use = 1;
    mp.count_of_clusters = 8;
    fake.sector[0..512].* = @splat(0);
    var ent = std.mem.zeroes([32]u8);
    mkEntry(&ent, 'F', 0x20, 2, 1);
    @memcpy(fake.sector[0..32], &ent);
    fake.fat[2] = 0x0FFF_FFFF;
    try std.testing.expectEqual(@as(u16, fe_ok), check.ra8_fs_check(mp, &t.bmp, 8, &t.rep));
    try std.testing.expectEqual(@as(u32, 1), t.rep.dirs_visited);
    try std.testing.expectEqual(@as(u32, 1), t.rep.files_visited);
    try std.testing.expectEqual(@as(u32, 8), t.rep.clusters_total);
    try std.testing.expectEqual(true, t.rep.referenced_scan);
    try std.testing.expectEqual(@as(u32, 0), t.rep.clusters_lost);
    try std.testing.expectEqual(@as(u32, 0), t.rep.faults_total);
}

test "the entry point without a bitmap reports unknown lost clusters" {
    var t: Ctx = undefined;
    freshCtx(&t, 8, 8);
    const mp = mkMount(c.k_ra8_fs_type_fat32);
    mp.in_use = 1;
    mp.count_of_clusters = 8;
    fake.sector[0..512].* = @splat(0);
    fake.fat[2] = 0x0FFF_FFFF;
    try std.testing.expectEqual(@as(u16, fe_ok), check.ra8_fs_check(mp, null, 0, &t.rep));
    try std.testing.expectEqual(false, t.rep.referenced_scan);
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), t.rep.clusters_lost);
}

test "the push-overflow seam caps once and honors the pre-truncated flag" {
    var t: Ctx = undefined;
    freshCtx(&t, 4, 0);
    check.ra8_fs_check_test_fat_push_overflow(&t.ctx, 9, false);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
    try std.testing.expectEqual(scan_truncated, @as(u8, @intCast(t.rep.first_fault.kind)));
    try std.testing.expectEqual(@as(u32, 9), t.rep.first_fault.cluster);
    check.ra8_fs_check_test_fat_push_overflow(&t.ctx, 10, true);
    try std.testing.expectEqual(@as(u32, 1), t.rep.faults_total);
}
