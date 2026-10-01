//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the shared `priv_cache_store_*` primitives the mount
//! half is built on: the reflected CRC-32, the sector read / write / release
//! helpers over the fake fake.medium, the in-memory index, the superblock record
//! and the directory checkpoint. The mount and recovery path itself is in
//! `mount_test.zig`; both drive the fake fake.medium in `cache_fake.zig`.

const std = @import("std");
const mount = @import("mount");
const fake = @import("cache_fake.zig");

const Super = mount.Super;
const DirEntry = mount.DirEntry;
const implementation = mount.implementation;

const ok = fake.ok;
const err_not_found = fake.err_not_found;
const err_hw_init_failed = fake.err_hw_init_failed;
const err_null_ptr = fake.err_null_ptr;
const sector_bytes = fake.sector_bytes;
const Fixture = fake.Fixture;
const resetMedium = fake.resetMedium;
const readSuper = fake.readSuper;
// -------------------------------------------------------------------------
// crc32
// -------------------------------------------------------------------------

test "crc32: a null pointer folds to zero" {
    try std.testing.expectEqual(@as(u32, 0), mount.priv_cache_store_crc32(null, 8));
}

test "crc32: an empty span folds to zero" {
    const data = [_]u8{ 1, 2, 3 };
    try std.testing.expectEqual(@as(u32, 0), mount.priv_cache_store_crc32(&data, 0));
}

test "crc32: matches the shared reflected CRC-32" {
    const data = "ra8_cache_store";
    try std.testing.expectEqual(
        implementation.crc32(data),
        mount.priv_cache_store_crc32(data, data.len),
    );
}

test "crc32: only folds the requested prefix" {
    const data = [_]u8{ 9, 8, 7, 6 };
    try std.testing.expectEqual(
        implementation.crc32(data[0..2]),
        mount.priv_cache_store_crc32(&data, 2),
    );
}

// -------------------------------------------------------------------------
// sector helpers
// -------------------------------------------------------------------------

test "sector_read: a null store is rejected before the fake.medium is touched" {
    resetMedium();
    var buffer: [sector_bytes]u8 = undefined;
    try std.testing.expectEqual(
        err_null_ptr,
        mount.priv_cache_store_sector_read(null, 0, &buffer),
    );
}

test "sector_read: a null destination is rejected" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    try std.testing.expectEqual(
        err_null_ptr,
        mount.priv_cache_store_sector_read(&fixture.store, 0, null),
    );
}

test "sector_read: an unmapped sector reports not_found" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var buffer: [sector_bytes]u8 = undefined;
    try std.testing.expectEqual(
        err_not_found,
        mount.priv_cache_store_sector_read(&fixture.store, 4, &buffer),
    );
}

test "sector_read: any other driver failure is hardware failure" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var buffer: [sector_bytes]u8 = undefined;
    fake.forced_read_rc = 1;
    try std.testing.expectEqual(
        err_hw_init_failed,
        mount.priv_cache_store_sector_read(&fixture.store, 4, &buffer),
    );
}

test "sector_write then sector_read round-trips the payload" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var out: [sector_bytes]u8 = undefined;
    @memset(&fixture.staging, 0xA5);
    try std.testing.expectEqual(
        ok,
        mount.priv_cache_store_sector_write(&fixture.store, 7, &fixture.staging),
    );
    try std.testing.expectEqual(ok, mount.priv_cache_store_sector_read(&fixture.store, 7, &out));
    try std.testing.expectEqual(@as(u8, 0xA5), out[0]);
    try std.testing.expectEqual(@as(u8, 0xA5), out[sector_bytes - 1]);
}

test "sector_write: null store and null source are both rejected" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    try std.testing.expectEqual(
        err_null_ptr,
        mount.priv_cache_store_sector_write(null, 0, &fixture.staging),
    );
    try std.testing.expectEqual(
        err_null_ptr,
        mount.priv_cache_store_sector_write(&fixture.store, 0, null),
    );
}

test "sector_write: a driver failure is hardware failure" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fake.forced_write_rc = 1;
    try std.testing.expectEqual(
        err_hw_init_failed,
        mount.priv_cache_store_sector_write(&fixture.store, 3, &fixture.staging),
    );
}

test "sector_release: guards the store, then the flash handle" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    try std.testing.expectEqual(err_null_ptr, mount.priv_cache_store_sector_release(null, 1));
    fixture.store.flash = null;
    try std.testing.expectEqual(
        err_null_ptr,
        mount.priv_cache_store_sector_release(&fixture.store, 1),
    );
}

test "sector_release: drops a mapped sector" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    try std.testing.expectEqual(
        ok,
        mount.priv_cache_store_sector_write(&fixture.store, 5, &fixture.staging),
    );
    try std.testing.expect(fake.present[5]);
    try std.testing.expectEqual(ok, mount.priv_cache_store_sector_release(&fixture.store, 5));
    try std.testing.expect(!fake.present[5]);
}

test "sector_release: a driver failure is hardware failure" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fake.forced_release_rc = 1;
    try std.testing.expectEqual(
        err_hw_init_failed,
        mount.priv_cache_store_sector_release(&fixture.store, 2),
    );
}

// -------------------------------------------------------------------------
// index helpers
// -------------------------------------------------------------------------

test "index_find: a null store or a null index array misses" {
    var fixture = Fixture{};
    fixture.bind();
    try std.testing.expectEqual(@as(i32, -1), mount.priv_cache_store_index_find(null, 1));
    fixture.store.index = null;
    try std.testing.expectEqual(
        @as(i32, -1),
        mount.priv_cache_store_index_find(&fixture.store, 1),
    );
}

test "index_find: only in-use slots match" {
    var fixture = Fixture{};
    fixture.bind();
    fixture.index[3] = .{ .key = 42, .flags = 0 };
    try std.testing.expectEqual(
        @as(i32, -1),
        mount.priv_cache_store_index_find(&fixture.store, 42),
    );
    fixture.index[3].flags = implementation.flag_in_use;
    try std.testing.expectEqual(
        @as(i32, 3),
        mount.priv_cache_store_index_find(&fixture.store, 42),
    );
}

test "index_add: a null store or a null index array cannot claim a slot" {
    var fixture = Fixture{};
    fixture.bind();
    try std.testing.expectEqual(
        @as(i32, -1),
        mount.priv_cache_store_index_add(null, 1, 2, 3, 4, false),
    );
    fixture.store.index = null;
    try std.testing.expectEqual(
        @as(i32, -1),
        mount.priv_cache_store_index_add(&fixture.store, 1, 2, 3, 4, false),
    );
}

test "index_add: claims the first free slot and records the run" {
    var fixture = Fixture{};
    fixture.bind();
    fixture.index[0].flags = implementation.flag_in_use;
    const slot = mount.priv_cache_store_index_add(&fixture.store, 77, 12, 3, 900, false);
    try std.testing.expectEqual(@as(i32, 1), slot);
    try std.testing.expectEqual(@as(u32, 77), fixture.index[1].key);
    try std.testing.expectEqual(@as(u32, 12), fixture.index[1].start_sector);
    try std.testing.expectEqual(@as(u16, 3), fixture.index[1].sector_count);
    try std.testing.expectEqual(@as(u32, 900), fixture.index[1].byte_len);
    try std.testing.expectEqual(implementation.flag_in_use, fixture.index[1].flags);
}

test "index_add: the pinned bit rides along" {
    var fixture = Fixture{};
    fixture.bind();
    _ = mount.priv_cache_store_index_add(&fixture.store, 1, 2, 1, 1, true);
    try std.testing.expect(implementation.isPinned(fixture.index[0]));
}

test "index_add: a full index refuses" {
    var fixture = Fixture{};
    fixture.bind();
    for (&fixture.index) |*entry| entry.flags = implementation.flag_in_use;
    try std.testing.expectEqual(
        @as(i32, -1),
        mount.priv_cache_store_index_add(&fixture.store, 1, 2, 1, 1, false),
    );
}

// -------------------------------------------------------------------------
// superblock
// -------------------------------------------------------------------------

test "super_write: null store and null staging are rejected" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    try std.testing.expectEqual(err_null_ptr, mount.priv_cache_store_super_write(null, 1));
    fixture.store.staging = null;
    try std.testing.expectEqual(
        err_null_ptr,
        mount.priv_cache_store_super_write(&fixture.store, 1),
    );
}

test "super_write: stamps a sealed record that reads back clean" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fixture.store.next_seq = 9;
    fixture.store.log_start = 2;
    fixture.store.data_capacity = 40;
    fixture.store.logical_sectors = 64;
    fixture.store.live_sectors = 6;
    fixture.index[0].flags = implementation.flag_in_use;
    fixture.index[4].flags = implementation.flag_in_use;

    try std.testing.expectEqual(ok, mount.priv_cache_store_super_write(&fixture.store, 1));
    var sb = readSuper();
    try std.testing.expectEqual(mount.super_magic, sb.magic);
    try std.testing.expectEqual(mount.format_version, sb.version);
    try std.testing.expectEqual(@as(u32, 2), sb.entry_count);
    try std.testing.expectEqual(@as(u32, 9), sb.next_seq);
    try std.testing.expectEqual(@as(u32, 6), sb.live_sectors);
    try std.testing.expect(mount.superIsClean(&sb));
}

test "super_write: a dirty marker is not a clean superblock" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    try std.testing.expectEqual(ok, mount.priv_cache_store_super_write(&fixture.store, 0));
    var sb = readSuper();
    try std.testing.expect(!mount.superIsClean(&sb));
}

test "superIsClean: rejects a foreign magic and a broken CRC" {
    var sb = Super{ .magic = 0xDEADBEEF, .clean = 1 };
    try std.testing.expect(!mount.superIsClean(&sb));
    sb.magic = mount.super_magic;
    sb.crc = implementation.crc32(std.mem.asBytes(&sb)[0..mount.super_crc_span]);
    try std.testing.expect(mount.superIsClean(&sb));
    sb.crc +%= 1;
    try std.testing.expect(!mount.superIsClean(&sb));
}

// -------------------------------------------------------------------------
// directory checkpoint
// -------------------------------------------------------------------------

test "dir_save: null store and null out-count are rejected" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var count: u32 = 0;
    try std.testing.expectEqual(err_null_ptr, mount.priv_cache_store_dir_save(null, &count));
    try std.testing.expectEqual(
        err_null_ptr,
        mount.priv_cache_store_dir_save(&fixture.store, null),
    );
}

test "dir_save: packs every in-use slot into the checkpoint sector" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fixture.store.checkpoint_dirs = 1;
    fixture.index[1] = .{
        .key = 11,
        .start_sector = 20,
        .byte_len = 700,
        .sector_count = 3,
        .flags = implementation.flag_in_use | implementation.flag_pinned,
    };
    fixture.index[6] = .{
        .key = 12,
        .start_sector = 30,
        .byte_len = 100,
        .sector_count = 2,
        .flags = implementation.flag_in_use,
    };
    var count: u32 = 0;
    try std.testing.expectEqual(ok, mount.priv_cache_store_dir_save(&fixture.store, &count));
    try std.testing.expectEqual(@as(u32, 2), count);

    var first: DirEntry = .{};
    @memcpy(std.mem.asBytes(&first), fake.medium[1][0..@sizeOf(DirEntry)]);
    try std.testing.expectEqual(@as(u32, 11), first.key);
    try std.testing.expectEqual(@as(u16, 3), first.sector_count);
    try std.testing.expectEqual(@as(u16, implementation.flag_pinned), first.flags);
    var second: DirEntry = .{};
    @memcpy(std.mem.asBytes(&second), fake.medium[1][16..32]);
    try std.testing.expectEqual(@as(u32, 12), second.key);
    try std.testing.expectEqual(@as(u16, 0), second.flags);
}

test "dir_save: an empty index writes a zeroed directory sector" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fixture.store.checkpoint_dirs = 1;
    var count: u32 = 0;
    try std.testing.expectEqual(ok, mount.priv_cache_store_dir_save(&fixture.store, &count));
    try std.testing.expectEqual(@as(u32, 0), count);
    try std.testing.expect(fake.present[1]);
    for (fake.medium[1]) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
}

test "dir_save: a write failure propagates out" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fixture.store.checkpoint_dirs = 1;
    fake.forced_write_rc = 1;
    var count: u32 = 0;
    try std.testing.expectEqual(
        err_hw_init_failed,
        mount.priv_cache_store_dir_save(&fixture.store, &count),
    );
}
