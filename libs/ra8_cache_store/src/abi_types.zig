//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared C-layout and status definitions for the cache-store ABI and mount
//! implementation. Keeping these independent lets the production Zig modules
//! call one another without exporting private helpers from the C ABI archive.

const std = @import("std");
const implementation = @import("cache_store_impl");

pub const Entry = implementation.Entry;
pub const EntryHeader = implementation.EntryHeader;
pub const sector_bytes = implementation.sector_bytes;
pub const RawErr = u16;

pub const ok: RawErr = 0;
pub const err_no_mem: RawErr = 0x102;
pub const err_invalid_arg: RawErr = 0x103;
pub const err_invalid_state: RawErr = 0x104;
pub const err_invalid_size: RawErr = 0x105;
pub const err_not_found: RawErr = 0x106;
pub const err_busy: RawErr = 0x109;
pub const err_exists: RawErr = 0x10C;
pub const err_not_initialized: RawErr = 0x10F;
pub const err_hw_init_failed: RawErr = 0x201;
pub const err_out_of_range: RawErr = 0x208;
pub const err_null_ptr: RawErr = 0x504;

pub const clean_dirty: u32 = 0;
pub const clean_clean: u32 = 1;

pub const Store = extern struct {
    flash: ?*anyopaque = null,
    index: ?[*]Entry = null,
    staging: ?[*]u8 = null,
    index_cap: u16 = 0,
    checkpoint_dirs: u16 = 0,
    logical_sectors: u32 = 0,
    log_start: u32 = 0,
    data_capacity: u32 = 0,
    live_sectors: u32 = 0,
    next_seq: u32 = 0,
    flash_state: u8 = 0,
    inited: u8 = 0,
};

pub const Reader = extern struct {
    store: ?*const Store = null,
    data_start: u32 = 0,
    data_sectors: u32 = 0,
    byte_len: u32 = 0,
};

pub const NorInitFn = *const fn (?*anyopaque) callconv(.c) c_uint;

pub const Config = extern struct {
    nor_flash: ?*anyopaque = null,
    nor_driver_init: ?NorInitFn = null,
    name: ?[*:0]const u8 = null,
    index: ?[*]Entry = null,
    staging: ?[*]u8 = null,
    staging_bytes: u32 = 0,
    logical_sectors: u32 = 0,
    index_cap: u16 = 0,
    overprovision_pct: u8 = 0,
    format: u8 = 0,
};

pub const super_magic: u32 = 0x52435331;
pub const format_version: u32 = 1;
pub const dir_ent_bytes: u32 = 16;
pub const dir_per_sector: u32 = 32;
pub const min_sectors: u32 = 8;
pub const overprov_pct_default: u32 = 20;
pub const max_overprov: u8 = 90;
pub const pct_full: u32 = 100;
pub const lx_success: c_uint = 0;
pub const lx_sector_not_found: c_uint = 3;

pub const Super = extern struct {
    magic: u32 = 0,
    version: u32 = 0,
    seq: u32 = 0,
    clean: u32 = 0,
    entry_count: u32 = 0,
    live_sectors: u32 = 0,
    next_seq: u32 = 0,
    log_start: u32 = 0,
    data_capacity: u32 = 0,
    logical_sectors: u32 = 0,
    crc: u32 = 0,
};

pub const DirEntry = extern struct {
    key: u32 = 0,
    start_sector: u32 = 0,
    byte_len: u32 = 0,
    sector_count: u16 = 0,
    flags: u16 = 0,
};

pub const super_crc_span: u32 = @sizeOf(Super) - @sizeOf(u32);

comptime {
    const word = @sizeOf(usize);
    std.debug.assert(@offsetOf(Store, "flash") == 0);
    std.debug.assert(@offsetOf(Store, "index") == word);
    std.debug.assert(@offsetOf(Store, "staging") == 2 * word);
    std.debug.assert(@offsetOf(Store, "index_cap") == 3 * word);
    std.debug.assert(@offsetOf(Store, "checkpoint_dirs") == 3 * word + 2);
    std.debug.assert(@offsetOf(Store, "logical_sectors") == 3 * word + 4);
    std.debug.assert(@offsetOf(Store, "log_start") == 3 * word + 8);
    std.debug.assert(@offsetOf(Store, "data_capacity") == 3 * word + 12);
    std.debug.assert(@offsetOf(Store, "live_sectors") == 3 * word + 16);
    std.debug.assert(@offsetOf(Store, "next_seq") == 3 * word + 20);
    std.debug.assert(@offsetOf(Store, "flash_state") == 3 * word + 24);
    std.debug.assert(@offsetOf(Store, "inited") == 3 * word + 25);
    std.debug.assert(@offsetOf(Reader, "store") == 0);
    std.debug.assert(@offsetOf(Reader, "data_start") == word);
    std.debug.assert(@offsetOf(Reader, "data_sectors") == word + 4);
    std.debug.assert(@offsetOf(Reader, "byte_len") == word + 8);
    std.debug.assert(@sizeOf(Super) == 44);
    std.debug.assert(super_crc_span == 40);
    std.debug.assert(@sizeOf(Super) <= sector_bytes);
    std.debug.assert(@sizeOf(DirEntry) == dir_ent_bytes);
    std.debug.assert(@offsetOf(DirEntry, "sector_count") == 12);
    std.debug.assert(@offsetOf(DirEntry, "flags") == 14);
    std.debug.assert(dir_ent_bytes * dir_per_sector == sector_bytes);
    std.debug.assert(@offsetOf(Config, "nor_flash") == 0);
    std.debug.assert(@offsetOf(Config, "nor_driver_init") == word);
    std.debug.assert(@offsetOf(Config, "name") == 2 * word);
    std.debug.assert(@offsetOf(Config, "index") == 3 * word);
    std.debug.assert(@offsetOf(Config, "staging") == 4 * word);
    std.debug.assert(@offsetOf(Config, "staging_bytes") == 5 * word);
    std.debug.assert(@offsetOf(Config, "logical_sectors") == 5 * word + 4);
    std.debug.assert(@offsetOf(Config, "index_cap") == 5 * word + 8);
    std.debug.assert(@offsetOf(Config, "overprovision_pct") == 5 * word + 10);
    std.debug.assert(@offsetOf(Config, "format") == 5 * word + 11);
}
