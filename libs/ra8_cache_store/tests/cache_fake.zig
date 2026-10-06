//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The fake LevelX medium every `ra8_cache_store` mount test runs against, plus
//! the mount fixture and the sector-level helpers the tests build their state
//! with. The medium is a fixed RAM array with per-call return-code overrides
//! and call counters, so a test can force one native failure and observe
//! exactly what the mount path did with it.
//!
//! The LevelX externs in `mount.zig` bind to the `_lx_nor_flash_*` symbols
//! exported here exactly the way they bind to the vendored LevelX in the real
//! build, which keeps the membrane under test byte-identical to the shipped
//! one. No test blocks live here: each test root imports this module.

const std = @import("std");
const mount = @import("mount");

const Store = mount.Store;
const Entry = mount.Entry;
const EntryHeader = mount.EntryHeader;
const Config = mount.Config;
const Super = mount.Super;
const DirEntry = mount.DirEntry;
const implementation = mount.implementation;

pub const ok: u16 = 0;
pub const err_invalid_size: u16 = 0x105;
pub const err_not_found: u16 = 0x106;
pub const err_hw_init_failed: u16 = 0x201;
pub const err_null_ptr: u16 = 0x504;

pub const sector_bytes: u32 = 512;
pub const medium_sectors: u32 = 96;

// -------------------------------------------------------------------------
// Fake LevelX medium
// -------------------------------------------------------------------------

pub var medium: [medium_sectors][sector_bytes]u8 = undefined;
pub var present: [medium_sectors]bool = undefined;
pub var forced_read_rc: c_uint = 0;
pub var forced_write_rc: c_uint = 0;
pub var forced_release_rc: c_uint = 0;
pub var forced_format_rc: c_uint = 0;
pub var forced_open_rc: c_uint = 0;
pub var initialize_calls: u32 = 0;
pub var driver_init_calls: u32 = 0;

pub fn resetMedium() void {
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

pub var log_lines: u32 = 0;

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

pub const Fixture = struct {
    store: Store = .{},
    index: [8]Entry = @splat(.{}),
    staging: [sector_bytes]u8 = @splat(0),
    flash_block: u32 = 0,

    pub fn bind(self: *Fixture) void {
        self.store = .{};
        self.index = @splat(.{});
        self.store.flash = @ptrCast(&self.flash_block);
        self.store.index = &self.index;
        self.store.staging = &self.staging;
        self.store.index_cap = self.index.len;
    }

    pub fn cfg(self: *Fixture, logical_sectors: u32, format: bool) Config {
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

pub fn writeHeaderAt(sector: u32, key: u32, seq: u32, count: u16, byte_len: u32, flags: u16) void {
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

pub fn writeSuperAt(sb: *Super) void {
    sb.crc = implementation.crc32(std.mem.asBytes(sb)[0..mount.super_crc_span]);
    @memset(&medium[0], 0);
    @memcpy(medium[0][0..@sizeOf(Super)], std.mem.asBytes(sb));
    present[0] = true;
}

pub fn readSuper() Super {
    var sb: Super = .{};
    @memcpy(std.mem.asBytes(&sb), medium[0][0..@sizeOf(Super)]);
    return sb;
}
