//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the directory erase-chain helpers
//! (`ra8_fs_dir_erase_abi.zig`): the chain ahead of one 8.3 entry is
//! collected by checksum, a stale chain with a different checksum and a
//! live entry between slots reset the run, the cap shift keeps the newest
//! positions, the erase writes back once to the run's sector, and a walk
//! that runs past the directory answers not_found.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const fake = @import("fs_walker_fake.zig");
const erase = fs.dir_erase;

const ok: u16 = c.k_ra8_ok;
const err_not_found: u16 = c.k_ra8_err_not_found;
const entry_bytes: u32 = c.k_ra8_fs_dir_entry_bytes;
const cap: u32 = @intCast(c.k_lfn_erase_max);
const off_attr: usize = @intCast(c.k_dir_off_attr);
const off_csum: usize = @intCast(c.k_lfn_off_checksum);
const attr_lfn: u8 = @intCast(c.k_ra8_fs_attr_lfn);
const chain_csum: u8 = 0x5A;

fn setup() *const c.ra8_fs_mount_t {
    fake.reset(c.k_ra8_fs_type_fat16);
    fake.dir_lba = fake.walk_lba;
    fake.walk_fixed = 1;
    fake.eps = 16;
    for (&fake.dir) |*e| e.* = @splat(0x85);
    return fake.mount();
}

fn setLfn(i: usize, csum: u8) void {
    fake.dir[i][off_attr] = attr_lfn;
    fake.dir[i][off_csum] = csum;
}

fn targetOff(i: usize) u32 {
    return @intCast(i * entry_bytes);
}

test "collect keeps the checksum-matching chain ahead of the target" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    loc.is_root = 1;
    var run: [cap + 1]c.dir_pos_t = undefined;
    var len: u32 = 0;
    setLfn(2, chain_csum);
    try std.testing.expectEqual(ok, erase.internal_dir_collect_chain(m, &loc, fake.dir_lba, targetOff(3), chain_csum, &run, &len));
    try std.testing.expectEqual(@as(u32, 1), len);
    try std.testing.expectEqual(fake.dir_lba, run[0].lba);
    try std.testing.expectEqual(targetOff(2), run[0].off);
}

test "a stale chain with a different checksum resets the run" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    loc.is_root = 1;
    var run: [cap + 1]c.dir_pos_t = undefined;
    var len: u32 = 0;
    setLfn(1, chain_csum +% 1);
    setLfn(2, chain_csum);
    try std.testing.expectEqual(ok, erase.internal_dir_collect_chain(m, &loc, fake.dir_lba, targetOff(3), chain_csum, &run, &len));
    try std.testing.expectEqual(@as(u32, 1), len);
    try std.testing.expectEqual(targetOff(2), run[0].off);
}

test "a live entry between slots resets the run" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    loc.is_root = 1;
    var run: [cap + 1]c.dir_pos_t = undefined;
    var len: u32 = 0;
    setLfn(2, chain_csum);
    fake.dir[3][0] = 'A';
    setLfn(4, chain_csum);
    try std.testing.expectEqual(ok, erase.internal_dir_collect_chain(m, &loc, fake.dir_lba, targetOff(5), chain_csum, &run, &len));
    try std.testing.expectEqual(@as(u32, 1), len);
    try std.testing.expectEqual(targetOff(4), run[0].off);
}

test "run_push shifts the oldest position out when full" {
    var run: [cap + 1]c.dir_pos_t = undefined;
    var len: u32 = 0;
    var i: u32 = 0;
    while (i < cap + 3) : (i += 1) {
        erase.internal_run_push(&run, &len, i, i * entry_bytes);
    }
    try std.testing.expectEqual(cap, len);
    try std.testing.expectEqual(@as(u64, 3), run[0].lba);
    try std.testing.expectEqual(cap + 2, run[cap - 1].lba);
}

test "erase_positions marks the run and writes back once" {
    const m = setup();
    var run: [2]c.dir_pos_t = undefined;
    run[0] = .{ .lba = fake.dir_lba, .off = targetOff(2) };
    run[1] = .{ .lba = fake.dir_lba, .off = targetOff(3) };
    fake.sector_written = null;
    try std.testing.expectEqual(ok, erase.internal_dir_erase_positions(m, &run, 2));
    try std.testing.expectEqual(fake.dir_lba, fake.sector_written.?);
}

test "a walk past the directory answers not_found" {
    const m = setup();
    var loc: c.dir_loc_t = std.mem.zeroes(c.dir_loc_t);
    loc.is_root = 1;
    var run: [cap + 1]c.dir_pos_t = undefined;
    var len: u32 = 0;
    setLfn(2, chain_csum);
    try std.testing.expectEqual(err_not_found, erase.internal_dir_collect_chain(m, &loc, fake.dir_lba + 2, 0, chain_csum, &run, &len));
}

export fn priv_alloc_eoc_cluster(m: [*c]const u8, out_c: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_c.* = 6;
    return 0;
}
