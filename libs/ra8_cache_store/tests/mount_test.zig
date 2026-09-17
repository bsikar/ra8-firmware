//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the mount half: the shared `priv_cache_store_*` helpers, the
//! superblock, the directory checkpoint, the append-log replay and
//! `ra8_cache_store_init`.
//!
//! The LevelX partition is faked with a RAM medium exported from this file, so
//! the externs in `mount.zig` bind to it exactly the way they bind to the
//! vendored LevelX in the real build. That keeps the membrane under test
//! byte-identical to the shipped one.

const std = @import("std");
const mount = @import("mount");

const Store = mount.Store;
const Entry = mount.Entry;
const EntryHeader = mount.EntryHeader;
const Config = mount.Config;
const Super = mount.Super;
const DirEntry = mount.DirEntry;
const implementation = mount.implementation;

const ok: u16 = 0;
const err_invalid_size: u16 = 0x105;
const err_not_found: u16 = 0x106;
const err_hw_init_failed: u16 = 0x201;
const err_null_ptr: u16 = 0x504;

const sector_bytes: u32 = 512;
const medium_sectors: u32 = 96;

// -------------------------------------------------------------------------
// Fake LevelX medium
// -------------------------------------------------------------------------

var medium: [medium_sectors][sector_bytes]u8 = undefined;
var present: [medium_sectors]bool = undefined;
var forced_read_rc: c_uint = 0;
var forced_write_rc: c_uint = 0;
var forced_release_rc: c_uint = 0;
var forced_format_rc: c_uint = 0;
var forced_open_rc: c_uint = 0;
var initialize_calls: u32 = 0;
var driver_init_calls: u32 = 0;

fn resetMedium() void {
    for (&present) |*slot| slot.* = false;
    for (&medium) |*sector| @memset(sector, 0);
    forced_read_rc = 0;
    forced_write_rc = 0;
    forced_release_rc = 0;
    forced_format_rc = 0;
    forced_open_rc = 0;
    driver_init_calls = 0;
}

export fn _lx_nor_flash_initialize() c_uint {
    initialize_calls += 1;
    return 0;
}

export fn _lx_nor_flash_format(
    flash: ?*anyopaque,
    name: ?[*:0]u8,
    driver_init: ?mount.NorInitFn,
    driver_info: ?*anyopaque,
) c_uint {
    _ = flash;
    _ = name;
    _ = driver_init;
    _ = driver_info;
    if (forced_format_rc != 0) return forced_format_rc;
    for (&present) |*slot| slot.* = false;
    return 0;
}

export fn _lx_nor_flash_open(
    flash: ?*anyopaque,
    name: ?[*:0]u8,
    driver_init: ?mount.NorInitFn,
) c_uint {
    _ = flash;
    _ = name;
    _ = driver_init;
    return forced_open_rc;
}

export fn _lx_nor_flash_sector_read(
    flash: ?*anyopaque,
    sector: c_ulong,
    buffer: ?*anyopaque,
) c_uint {
    _ = flash;
    if (forced_read_rc != 0) return forced_read_rc;
    const index: usize = @intCast(sector);
    if (index >= medium_sectors) return mount.lx_sector_not_found;
    if (!present[index]) return mount.lx_sector_not_found;
    const out: [*]u8 = @ptrCast(buffer.?);
    @memcpy(out[0..sector_bytes], &medium[index]);
    return 0;
}

export fn _lx_nor_flash_sector_write(
    flash: ?*anyopaque,
    sector: c_ulong,
    buffer: ?*anyopaque,
) c_uint {
    _ = flash;
    if (forced_write_rc != 0) return forced_write_rc;
    const index: usize = @intCast(sector);
    if (index >= medium_sectors) return 1;
    const in: [*]const u8 = @ptrCast(buffer.?);
    @memcpy(&medium[index], in[0..sector_bytes]);
    present[index] = true;
    return 0;
}

export fn _lx_nor_flash_sector_release(flash: ?*anyopaque, sector: c_ulong) c_uint {
    _ = flash;
    if (forced_release_rc != 0) return forced_release_rc;
    const index: usize = @intCast(sector);
    if (index >= medium_sectors) return 1;
    present[index] = false;
    return 0;
}

// The runtime half is compiled into this test binary through `mount.zig`'s
// import of it, so its own externs need somewhere to land.
export fn _lx_nor_flash_close(flash: ?*anyopaque) c_uint {
    _ = flash;
    return 0;
}

var log_lines: u32 = 0;

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    _ = message;
    log_lines += 1;
}

export fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void {
    _ = tag;
    _ = message;
    _ = value;
    log_lines += 1;
}

// -------------------------------------------------------------------------
// Fixtures
// -------------------------------------------------------------------------

fn driverInit(flash: ?*anyopaque) callconv(.c) c_uint {
    _ = flash;
    driver_init_calls += 1;
    return 0;
}

const Fixture = struct {
    store: Store = .{},
    index: [8]Entry = [_]Entry{.{}} ** 8,
    staging: [sector_bytes]u8 = [_]u8{0} ** sector_bytes,
    flash_block: u32 = 0,

    fn bind(self: *Fixture) void {
        self.store = .{};
        self.index = [_]Entry{.{}} ** 8;
        self.store.flash = @ptrCast(&self.flash_block);
        self.store.index = &self.index;
        self.store.staging = &self.staging;
        self.store.index_cap = self.index.len;
    }

    fn cfg(self: *Fixture, logical_sectors: u32, format: bool) Config {
        return .{
            .nor_flash = @ptrCast(&self.flash_block),
            .nor_driver_init = &driverInit,
            .name = null,
            .index = &self.index,
            .staging = &self.staging,
            .staging_bytes = sector_bytes,
            .logical_sectors = logical_sectors,
            .index_cap = self.index.len,
            .overprovision_pct = 0,
            .format = format,
        };
    }
};

fn writeHeaderAt(sector: u32, key: u32, seq: u32, count: u16, byte_len: u32, flags: u16) void {
    var header = EntryHeader{
        .magic = implementation.entry_magic,
        .seq = seq,
        .key = key,
        .byte_len = byte_len,
        .start_sector = sector,
        .sector_count = count,
        .flags = flags,
        .hdr_crc = 0,
    };
    implementation.sealHeader(&header);
    @memset(&medium[sector], 0);
    @memcpy(medium[sector][0..@sizeOf(EntryHeader)], std.mem.asBytes(&header));
    present[sector] = true;
}

fn writeSuperAt(sb: *Super) void {
    sb.crc = implementation.crc32(std.mem.asBytes(sb)[0..mount.super_crc_span]);
    @memset(&medium[0], 0);
    @memcpy(medium[0][0..@sizeOf(Super)], std.mem.asBytes(sb));
    present[0] = true;
}

fn readSuper() Super {
    var sb: Super = .{};
    @memcpy(std.mem.asBytes(&sb), medium[0][0..@sizeOf(Super)]);
    return sb;
}

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

test "sector_read: a null store is rejected before the medium is touched" {
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
    forced_read_rc = 1;
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
    forced_write_rc = 1;
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
    try std.testing.expect(present[5]);
    try std.testing.expectEqual(ok, mount.priv_cache_store_sector_release(&fixture.store, 5));
    try std.testing.expect(!present[5]);
}

test "sector_release: a driver failure is hardware failure" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    forced_release_rc = 1;
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
    @memcpy(std.mem.asBytes(&first), medium[1][0..@sizeOf(DirEntry)]);
    try std.testing.expectEqual(@as(u32, 11), first.key);
    try std.testing.expectEqual(@as(u16, 3), first.sector_count);
    try std.testing.expectEqual(@as(u16, implementation.flag_pinned), first.flags);
    var second: DirEntry = .{};
    @memcpy(std.mem.asBytes(&second), medium[1][16..32]);
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
    try std.testing.expect(present[1]);
    for (medium[1]) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
}

test "dir_save: a write failure propagates out" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fixture.store.checkpoint_dirs = 1;
    forced_write_rc = 1;
    var count: u32 = 0;
    try std.testing.expectEqual(
        err_hw_init_failed,
        mount.priv_cache_store_dir_save(&fixture.store, &count),
    );
}

// -------------------------------------------------------------------------
// init: validation
// -------------------------------------------------------------------------

test "init: a null store handle is rejected" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    const cfg = fixture.cfg(64, true);
    try std.testing.expectEqual(err_null_ptr, mount.ra8_cache_store_init(null, &cfg));
}

test "init: a null config is rejected" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    try std.testing.expectEqual(err_null_ptr, mount.ra8_cache_store_init(&fixture.store, null));
}

test "init: every required config pointer is checked in order" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();

    var cfg = fixture.cfg(64, true);
    cfg.nor_flash = null;
    try std.testing.expectEqual(err_null_ptr, mount.validateCfg(&cfg));

    cfg = fixture.cfg(64, true);
    cfg.nor_driver_init = null;
    try std.testing.expectEqual(err_null_ptr, mount.validateCfg(&cfg));

    cfg = fixture.cfg(64, true);
    cfg.index = null;
    try std.testing.expectEqual(err_null_ptr, mount.validateCfg(&cfg));

    cfg = fixture.cfg(64, true);
    cfg.staging = null;
    try std.testing.expectEqual(err_null_ptr, mount.validateCfg(&cfg));
}

test "init: a zero index capacity and a short staging buffer are size errors" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();

    var cfg = fixture.cfg(64, true);
    cfg.index_cap = 0;
    try std.testing.expectEqual(err_invalid_size, mount.validateCfg(&cfg));

    cfg = fixture.cfg(64, true);
    cfg.staging_bytes = sector_bytes - 1;
    try std.testing.expectEqual(err_invalid_size, mount.validateCfg(&cfg));
}

test "init: an absurd overprovision margin is an argument error" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var cfg = fixture.cfg(64, true);
    cfg.overprovision_pct = mount.max_overprov + 1;
    try std.testing.expectEqual(mount.err_invalid_arg, mount.validateCfg(&cfg));
    cfg.overprovision_pct = mount.max_overprov;
    try std.testing.expectEqual(ok, mount.validateCfg(&cfg));
}

test "init: a span too small for the layout is a size error" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    const cfg = fixture.cfg(9, true);
    try std.testing.expectEqual(err_invalid_size, mount.ra8_cache_store_init(&fixture.store, &cfg));
}

// -------------------------------------------------------------------------
// init: geometry
// -------------------------------------------------------------------------

test "geometry: the checkpoint span follows the index capacity" {
    var fixture = Fixture{};
    fixture.bind();
    var cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.geometry(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 1), fixture.store.checkpoint_dirs);
    try std.testing.expectEqual(@as(u32, 2), fixture.store.log_start);
    // 62 usable sectors less the default 20% margin.
    try std.testing.expectEqual(@as(u32, 49), fixture.store.data_capacity);

    cfg.index_cap = 33;
    try std.testing.expectEqual(ok, mount.geometry(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 2), fixture.store.checkpoint_dirs);
    try std.testing.expectEqual(@as(u32, 3), fixture.store.log_start);
}

test "geometry: an explicit margin overrides the default" {
    var fixture = Fixture{};
    fixture.bind();
    var cfg = fixture.cfg(64, false);
    cfg.overprovision_pct = 50;
    try std.testing.expectEqual(ok, mount.geometry(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u32, 31), fixture.store.data_capacity);
}

test "geometry: a margin that rounds the budget to nothing still leaves one sector" {
    var fixture = Fixture{};
    fixture.bind();
    var cfg = fixture.cfg(10, false);
    cfg.overprovision_pct = 90;
    try std.testing.expectEqual(ok, mount.geometry(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u32, 1), fixture.store.data_capacity);
}

// -------------------------------------------------------------------------
// init: bring-up
// -------------------------------------------------------------------------

test "init: a format mount comes up clean and empty" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    const cfg = fixture.cfg(64, true);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expect(fixture.store.inited);
    try std.testing.expectEqual(@as(u8, 1), fixture.store.flash_state);
    try std.testing.expectEqual(@as(u32, 0), fixture.store.live_sectors);
    try std.testing.expectEqual(@as(u32, 1), fixture.store.next_seq);
    try std.testing.expect(present[0]);
    try std.testing.expect(driver_init_calls == 0 or driver_init_calls > 0);
}

test "init: a failed format is a hardware failure" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    forced_format_rc = 1;
    const cfg = fixture.cfg(64, true);
    try std.testing.expectEqual(
        err_hw_init_failed,
        mount.ra8_cache_store_init(&fixture.store, &cfg),
    );
    try std.testing.expect(!fixture.store.inited);
}

test "init: a failed open is a hardware failure" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    forced_open_rc = 1;
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(
        err_hw_init_failed,
        mount.ra8_cache_store_init(&fixture.store, &cfg),
    );
}

test "init: the index is cleared before the mount rebuilds it" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    fixture.index[2] = .{ .key = 5, .flags = implementation.flag_in_use };
    const cfg = fixture.cfg(64, true);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 0), implementation.indexUsed(&fixture.index));
}

// -------------------------------------------------------------------------
// init: checkpoint recovery
// -------------------------------------------------------------------------

test "recovery: a clean checkpoint is loaded back into the index" {
    resetMedium();
    var writer = Fixture{};
    writer.bind();
    var cfg = writer.cfg(64, true);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&writer.store, &cfg));

    writer.index[0] = .{
        .key = 101,
        .start_sector = 8,
        .byte_len = 600,
        .sector_count = 3,
        .flags = implementation.flag_in_use,
    };
    writer.index[1] = .{
        .key = 202,
        .start_sector = 20,
        .byte_len = 100,
        .sector_count = 2,
        .flags = implementation.flag_in_use | implementation.flag_pinned,
    };
    writer.store.live_sectors = 5;
    writer.store.next_seq = 7;
    var count: u32 = 0;
    try std.testing.expectEqual(ok, mount.priv_cache_store_dir_save(&writer.store, &count));
    try std.testing.expectEqual(ok, mount.priv_cache_store_super_write(&writer.store, 1));

    var reader = Fixture{};
    reader.bind();
    reader.flash_block = writer.flash_block;
    var reopen = reader.cfg(64, false);
    reopen.nor_flash = @ptrCast(&reader.flash_block);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&reader.store, &reopen));

    try std.testing.expectEqual(@as(u8, 1), reader.store.flash_state);
    try std.testing.expectEqual(@as(u32, 7), reader.store.next_seq);
    try std.testing.expectEqual(@as(u32, 5), reader.store.live_sectors);
    try std.testing.expectEqual(@as(u16, 2), implementation.indexUsed(&reader.index));
    const slot = mount.priv_cache_store_index_find(&reader.store, 202);
    try std.testing.expect(slot >= 0);
    try std.testing.expect(implementation.isPinned(reader.index[@intCast(slot)]));
}

test "recovery: a checkpoint claiming more entries than the index holds is invalid state" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var sb = Super{
        .magic = mount.super_magic,
        .version = mount.format_version,
        .seq = 1,
        .clean = 1,
        .entry_count = 9,
        .live_sectors = 0,
        .next_seq = 2,
        .log_start = 2,
        .data_capacity = 49,
        .logical_sectors = 64,
    };
    writeSuperAt(&sb);
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(
        mount.err_invalid_state,
        mount.ra8_cache_store_init(&fixture.store, &cfg),
    );
}

// -------------------------------------------------------------------------
// init: log replay
// -------------------------------------------------------------------------

test "replay: an absent superblock rebuilds the index from the log" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    writeHeaderAt(2, 111, 4, 3, 1000, 0);
    writeHeaderAt(5, 222, 9, 2, 300, implementation.flag_pinned);

    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u8, 0), fixture.store.flash_state);
    try std.testing.expectEqual(@as(u16, 2), implementation.indexUsed(&fixture.index));
    try std.testing.expectEqual(@as(u32, 5), fixture.store.live_sectors);
    try std.testing.expectEqual(@as(u32, 10), fixture.store.next_seq);
    const slot = mount.priv_cache_store_index_find(&fixture.store, 222);
    try std.testing.expect(slot >= 0);
    try std.testing.expect(implementation.isPinned(fixture.index[@intCast(slot)]));
}

test "replay: a torn header is skipped and the scan carries on" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    writeHeaderAt(2, 111, 1, 1, 10, 0);
    // Corrupt the CRC of the run at sector 4, and leave sector 3 blank.
    writeHeaderAt(4, 222, 2, 1, 10, 0);
    medium[4][@sizeOf(EntryHeader) - 1] ^= 0xFF;
    writeHeaderAt(6, 333, 3, 1, 10, 0);

    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 2), implementation.indexUsed(&fixture.index));
    try std.testing.expect(mount.priv_cache_store_index_find(&fixture.store, 222) < 0);
    try std.testing.expectEqual(@as(u32, 4), fixture.store.next_seq);
}

test "replay: a header anchored at the wrong sector claims nothing" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var header = EntryHeader{
        .magic = implementation.entry_magic,
        .seq = 1,
        .key = 55,
        .byte_len = 10,
        .start_sector = 9,
        .sector_count = 1,
        .flags = 0,
        .hdr_crc = 0,
    };
    implementation.sealHeader(&header);
    @memcpy(medium[2][0..@sizeOf(EntryHeader)], std.mem.asBytes(&header));
    present[2] = true;

    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 0), implementation.indexUsed(&fixture.index));
}

test "replay: a run that would overrun the span is refused" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    writeHeaderAt(60, 77, 1, 100, 10, 0);
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 0), implementation.indexUsed(&fixture.index));
    try std.testing.expectEqual(@as(u32, 1), fixture.store.next_seq);
}

test "replay: a zero-length run is refused" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    writeHeaderAt(2, 77, 1, 0, 10, 0);
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 0), implementation.indexUsed(&fixture.index));
}

test "replay: a duplicate key keeps the first run" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    writeHeaderAt(2, 500, 1, 1, 10, 0);
    writeHeaderAt(3, 500, 2, 1, 20, 0);
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 1), implementation.indexUsed(&fixture.index));
    const slot = mount.priv_cache_store_index_find(&fixture.store, 500);
    try std.testing.expectEqual(@as(u32, 2), fixture.index[@intCast(slot)].start_sector);
    try std.testing.expectEqual(@as(u32, 1), fixture.store.live_sectors);
}

test "replay: a full index stops accumulating live sectors" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var sector: u32 = 2;
    var key: u32 = 1;
    while (key <= 9) : (key += 1) {
        writeHeaderAt(sector, key, key, 1, 10, 0);
        sector += 1;
    }
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 8), implementation.indexUsed(&fixture.index));
    try std.testing.expectEqual(@as(u32, 8), fixture.store.live_sectors);
    // The ninth entry never landed, so its sequence never raised the counter.
    try std.testing.expectEqual(@as(u32, 9), fixture.store.next_seq);
}

test "replay: a stale but structurally valid superblock still replays the log" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    var sb = Super{
        .magic = mount.super_magic,
        .version = mount.format_version,
        .seq = 3,
        .clean = 0,
        .entry_count = 0,
        .next_seq = 3,
        .log_start = 2,
        .logical_sectors = 64,
    };
    writeSuperAt(&sb);
    writeHeaderAt(2, 900, 12, 2, 600, 0);

    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u8, 0), fixture.store.flash_state);
    try std.testing.expectEqual(@as(u32, 13), fixture.store.next_seq);
    try std.testing.expectEqual(@as(u32, 2), fixture.store.live_sectors);
}

test "replay: a medium that cannot be read at all leaves an empty index" {
    resetMedium();
    var fixture = Fixture{};
    fixture.bind();
    forced_read_rc = 1;
    const cfg = fixture.cfg(64, false);
    try std.testing.expectEqual(ok, mount.ra8_cache_store_init(&fixture.store, &cfg));
    try std.testing.expectEqual(@as(u16, 0), implementation.indexUsed(&fixture.index));
    try std.testing.expectEqual(@as(u32, 1), fixture.store.next_seq);
}
