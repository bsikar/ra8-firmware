//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the volume-label port (`ra8_fs_label_abi.zig`): root
//! entry discovery, the deleted/long-name skips, the boot-sector fallback,
//! the boot-offset flavours, set/clear reconciliation, the argument guards,
//! and the exFAT dispatch.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const fake = @import("fs_walker_fake.zig");
const label = fs.label;

const ok: u16 = c.k_ra8_ok;
const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_invalid_state: u16 = c.k_ra8_err_invalid_state;

const label_len: usize = 11;
const attr_vol: u8 = @intCast(c.k_ra8_fs_attr_volume_id);
const attr_lfn: u8 = @intCast(c.k_ra8_fs_attr_lfn);
const marker_used: u8 = @intCast(c.k_dir_marker_free_used);

/// Stand-in for the C label-field writer: pads with spaces, writes the
/// "NO NAME    " sentinel for a null or empty label. The real one lives in
/// the C fmt unit and is exercised there.
/// Root reads land on the laid directory (dir_lba == walk_lba); boot reads
/// fall through to whatever the walker buffer last held.
fn setupRoot(fat_type: u8) *const c.ra8_fs_mount_t {
    fake.reset(fat_type);
    fake.dir_lba = fake.walk_lba;
    fake.walk_fixed = 1;
    fake.eps = 16;
    for (&fake.dir) |*e| e.* = @splat(0);
    return fake.mount();
}

/// Boot reads (lba 0) land on the laid bytes via dir_lba == 0; root reads
/// (lba 10) leave the walker buffer untouched, so it keeps the boot image.
fn setupBoot(fat_type: u8) *const c.ra8_fs_mount_t {
    fake.reset(fat_type);
    fake.dir_lba = 0;
    fake.walk_lba = 10;
    fake.walk_fixed = 1;
    fake.eps = 16;
    for (&fake.dir) |*e| e.* = @splat(0);
    return fake.mount();
}

fn layVol(i: usize, name11: *const [label_len]u8, attr: u8) void {
    fake.dir[i] = @splat(0);
    @memcpy(fake.dir[i][0..label_len], name11);
    fake.dir[i][11] = attr;
}

test "get reads the root volume entry, trimming trailing spaces" {
    const m = setupRoot(c.k_ra8_fs_type_fat16);
    layVol(0, "VOL LABEL  ", attr_vol);
    var buf: [64]u8 = @splat(0xAA);
    try std.testing.expectEqual(ok, label.ra8_fs_get_label(m, &buf, 64));
    try std.testing.expectEqualStrings("VOL LABEL", std.mem.sliceTo(&buf, 0));
}

test "get skips deleted and long-name entries before the label" {
    const m = setupRoot(c.k_ra8_fs_type_fat16);
    fake.dir[0][0] = marker_used;
    fake.dir[1][0] = 'x';
    fake.dir[1][11] = attr_lfn;
    layVol(2, "MEDIA      ", attr_vol);
    var buf: [64]u8 = @splat(0);
    try std.testing.expectEqual(ok, label.ra8_fs_get_label(m, &buf, 64));
    try std.testing.expectEqualStrings("MEDIA", std.mem.sliceTo(&buf, 0));
}

test "get reports the root NO NAME sentinel as empty" {
    const m = setupRoot(c.k_ra8_fs_type_fat16);
    layVol(0, "NO NAME    ", attr_vol);
    var buf: [64]u8 = @splat(0xFF);
    try std.testing.expectEqual(ok, label.ra8_fs_get_label(m, &buf, 64));
    try std.testing.expectEqualStrings("", std.mem.sliceTo(&buf, 0));
}

test "get falls back to an unlabelled boot sector on a free root" {
    const m = setupBoot(c.k_ra8_fs_type_fat16);
    var buf: [64]u8 = @splat(0xFF);
    try std.testing.expectEqual(ok, label.ra8_fs_get_label(m, &buf, 64));
    try std.testing.expectEqualStrings("", std.mem.sliceTo(&buf, 0));
}

test "get reads the NO NAME sentinel from the boot fallback as empty" {
    const m = setupBoot(c.k_ra8_fs_type_fat16);
    @memcpy(fake.dir[1][11..22], "NO NAME    ");
    var buf: [64]u8 = @splat(0xFF);
    try std.testing.expectEqual(ok, label.ra8_fs_get_label(m, &buf, 64));
    try std.testing.expectEqualStrings("", std.mem.sliceTo(&buf, 0));
}

test "set stamps the boot sector at the FAT16 offset and creates the root entry" {
    const m = setupBoot(c.k_ra8_fs_type_fat16);
    try std.testing.expectEqual(ok, label.ra8_fs_set_label(m, "PROJ"));
    try std.testing.expectEqualStrings("PROJ       ", fake.sector[43..54]);
    try std.testing.expectEqualStrings("PROJ       ", fake.sector[0..11]);
    try std.testing.expectEqual(attr_vol, fake.sector[11]);
    try std.testing.expectEqual(@as(u16, 0x21), std.mem.readInt(u16, fake.sector[16..18], .little));
    try std.testing.expectEqual(@as(?u64, 10), fake.sector_written);
}

test "set stamps the boot sector at the FAT32 offset" {
    const m = setupBoot(c.k_ra8_fs_type_fat32);
    try std.testing.expectEqual(ok, label.ra8_fs_set_label(m, "PROJ"));
    try std.testing.expectEqualStrings("PROJ       ", fake.sector[71..82]);
}

test "set rewrites an existing entry in place, preserving its other bytes" {
    const m = setupRoot(c.k_ra8_fs_type_fat16);
    layVol(0, "OLD        ", attr_vol);
    fake.dir[0][30] = 0xAB;
    try std.testing.expectEqual(ok, label.ra8_fs_set_label(m, "NEW"));
    try std.testing.expectEqualStrings("NEW        ", fake.sector[0..11]);
    try std.testing.expectEqual(attr_vol, fake.sector[11]);
    try std.testing.expectEqual(@as(u8, 0xAB), fake.sector[30]);
    try std.testing.expectEqual(@as(u16, 0x21), std.mem.readInt(u16, fake.sector[24..26], .little));
}

test "clear deletes the root entry and keeps its other bytes" {
    const m = setupRoot(c.k_ra8_fs_type_fat16);
    layVol(0, "OLD        ", attr_vol);
    try std.testing.expectEqual(ok, label.ra8_fs_set_label(m, null));
    try std.testing.expectEqual(marker_used, fake.sector[0]);
    try std.testing.expectEqual(attr_vol, fake.sector[11]);
}

test "clear with no root entry writes only the boot sector" {
    const m = setupBoot(c.k_ra8_fs_type_fat16);
    try std.testing.expectEqual(ok, label.ra8_fs_set_label(m, ""));
    try std.testing.expectEqualStrings("NO NAME    ", fake.sector[43..54]);
    try std.testing.expectEqual(@as(?u64, 0), fake.sector_written);
}

test "set rejects a label longer than the 11-byte field" {
    const m = setupRoot(c.k_ra8_fs_type_fat16);
    try std.testing.expectEqual(err_invalid_arg, label.ra8_fs_set_label(m, "123456789012"));
    try std.testing.expectEqual(ok, label.ra8_fs_set_label(m, "ABCDEFGHIJK"));
}

test "get validates handle, buffer, length, and mount state" {
    const m = setupRoot(c.k_ra8_fs_type_fat16);
    var buf: [8]u8 = @splat(0);
    try std.testing.expectEqual(err_null_ptr, label.ra8_fs_get_label(null, &buf, 8));
    try std.testing.expectEqual(err_null_ptr, label.ra8_fs_get_label(m, null, 8));
    try std.testing.expectEqual(err_invalid_arg, label.ra8_fs_get_label(m, &buf, 0));
    fake.mount_store.in_use = 0;
    try std.testing.expectEqual(err_invalid_state, label.ra8_fs_get_label(m, &buf, 8));
}

test "set validates handle, mount state, and label length" {
    const m = setupRoot(c.k_ra8_fs_type_fat16);
    try std.testing.expectEqual(err_null_ptr, label.ra8_fs_set_label(null, "X"));
    fake.mount_store.in_use = 0;
    try std.testing.expectEqual(err_invalid_state, label.ra8_fs_set_label(m, "X"));
    fake.mount_store.in_use = 1;
}

test "exFAT mounts route to the exFAT label writer" {
    fake.reset(c.k_ra8_fs_type_exfat); // default exFAT root: bitmap + up-case entries
    const m = fake.mount();
    try std.testing.expectEqual(ok, label.ra8_fs_set_label(m, "XY"));
    // the exFAT route wrote the label entry at the root's end-of-directory slot
    try std.testing.expectEqual(@as(u8, 0x83), fake.dir[2][0]);
    try std.testing.expectEqual(@as(u8, 'X'), fake.dir[2][2]);
    try std.testing.expectEqual(@as(u8, 'Y'), fake.dir[2][4]);
}
