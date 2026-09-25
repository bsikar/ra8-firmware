//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Mount, crash recovery and the shared on-flash helpers for `ra8_cache_store`:
//! superblock read/write, the directory checkpoint, the append-log replay and
//! `ra8_cache_store_init`. This is the other half of the library; the runtime
//! path (put / get / read / evict / pin / sync / close) calls these helpers
//! through a Zig module boundary. They are not exported in the C ABI archive.
//!
//! LevelX is reached through its public `lx_nor_flash_*` API, but those names
//! are macros in `lx_api.h`, so the externs below name the `_lx_*`
//! implementation symbols the macros expand to.

const std = @import("std");
const abi = @import("cache_store_types");

/// Pure logic shared with the runtime half (CRC-32, record layouts, flags).
pub const implementation = @import("cache_store_impl");

/// Mounted store handle (`ra8_cache_store_t`).
pub const Store = abi.Store;
/// One in-RAM index slot (`ra8_cache_store_entry_t`).
pub const Entry = abi.Entry;
/// On-flash entry header (`ra8_cs_entry_hdr_t`).
pub const EntryHeader = abi.EntryHeader;
/// Raw `ra8_err_t` as it crosses the ABI.
pub const RawErr = abi.RawErr;
pub const err_invalid_arg = abi.err_invalid_arg;
pub const err_invalid_state = abi.err_invalid_state;

const sector_bytes = implementation.sector_bytes;

pub const super_magic = abi.super_magic;
pub const format_version = abi.format_version;
pub const dir_ent_bytes = abi.dir_ent_bytes;
pub const dir_per_sector = abi.dir_per_sector;
pub const min_sectors = abi.min_sectors;
pub const overprov_pct_default = abi.overprov_pct_default;
pub const max_overprov = abi.max_overprov;
pub const pct_full = abi.pct_full;
pub const lx_success = abi.lx_success;
pub const lx_sector_not_found = abi.lx_sector_not_found;
pub const NorInitFn = abi.NorInitFn;
pub const Super = abi.Super;
pub const DirEntry = abi.DirEntry;
pub const Config = abi.Config;
pub const super_crc_span = abi.super_crc_span;

extern fn _lx_nor_flash_initialize() c_uint;
extern fn _lx_nor_flash_format(
    flash: ?*anyopaque,
    name: ?[*:0]u8,
    driver_init: ?NorInitFn,
    driver_info: ?*anyopaque,
) c_uint;
extern fn _lx_nor_flash_open(flash: ?*anyopaque, name: ?[*:0]u8, driver_init: ?NorInitFn) c_uint;
extern fn _lx_nor_flash_sector_read(flash: ?*anyopaque, sector: c_ulong, buffer: ?*anyopaque) c_uint;
extern fn _lx_nor_flash_sector_write(flash: ?*anyopaque, sector: c_ulong, buffer: ?*anyopaque) c_uint;
extern fn _lx_nor_flash_sector_release(flash: ?*anyopaque, sector: c_ulong) c_uint;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;

const tag: [*:0]const u8 = "ra8_cache_store";

/// One-shot latch so LevelX's global open-list is initialised once
/// (`s_cs_lx_system_inited`).
var lx_system_inited: bool = false;

/// `RA8_CHECK_NULL_PTR`: log and hand back the code.
fn reject(message: [*:0]const u8, code: RawErr) RawErr {
    ra8_log_emit_error(tag, message);
    return code;
}

/// `RA8_RETURN_ON_ERROR`: log the site and the value, then propagate.
fn propagate(code: RawErr, message: [*:0]const u8) ?RawErr {
    if (code == abi.ok) return null;
    ra8_log_emit_error(tag, message);
    ra8_log_emit_error_val(tag, "Error", code);
    return code;
}

// -------------------------------------------------------------------------
// Shared low-level helpers (the seam the runtime half links against)
// -------------------------------------------------------------------------

/// CRC-32 over `data`; 0 for a null pointer or an empty span.
pub fn priv_cache_store_crc32(data: ?[*]const u8, len: u32) u32 {
    const base = data orelse return 0;
    if (len == 0) return 0;
    return implementation.crc32(base[0..len]);
}

/// Read one logical sector into `out512`.
pub fn priv_cache_store_sector_read(
    store_arg: ?*const Store,
    sector: u32,
    out512: ?[*]u8,
) RawErr {
    const store = store_arg orelse return reject("store", abi.err_null_ptr);
    const out = out512 orelse return reject("out512", abi.err_null_ptr);
    const rc = _lx_nor_flash_sector_read(store.flash, sector, @ptrCast(out));
    if (rc == lx_success) return abi.ok;
    if (rc == lx_sector_not_found) return abi.err_not_found;
    return abi.err_hw_init_failed;
}

/// Write one logical sector from `in512`.
pub fn priv_cache_store_sector_write(
    store_arg: ?*Store,
    sector: u32,
    in512: ?[*]const u8,
) RawErr {
    const store = store_arg orelse return reject("store", abi.err_null_ptr);
    const in = in512 orelse return reject("in512", abi.err_null_ptr);
    // LevelX writes take a mutable buffer pointer but do not modify it.
    const rc = _lx_nor_flash_sector_write(store.flash, sector, @constCast(@ptrCast(in)));
    if (rc != lx_success) return abi.err_hw_init_failed;
    return abi.ok;
}

/// Release one logical sector back to LevelX.
/// The `flash` guard here has no counterpart in read/write; that asymmetry is
/// the C's and the MC/DC vectors depend on it.
pub fn priv_cache_store_sector_release(store_arg: ?*Store, sector: u32) RawErr {
    const store = store_arg orelse return reject("store", abi.err_null_ptr);
    const flash = store.flash orelse return reject("flash", abi.err_null_ptr);
    const rc = _lx_nor_flash_sector_release(flash, sector);
    if (rc != lx_success) return abi.err_hw_init_failed;
    return abi.ok;
}

/// Slot holding `key`, or -1 when absent.
pub fn priv_cache_store_index_find(store_arg: ?*const Store, key: u32) i32 {
    const store = store_arg orelse return -1;
    const base = store.index orelse return -1;
    const slot = implementation.findKey(base[0..store.index_cap], key) orelse return -1;
    return @intCast(slot);
}

/// Claim the first free index slot, or -1 when the index is full.
pub fn priv_cache_store_index_add(
    store_arg: ?*Store,
    key: u32,
    start_sector: u32,
    sector_count: u16,
    byte_len: u32,
    pinned: bool,
) i32 {
    const store = store_arg orelse return -1;
    const base = store.index orelse return -1;
    var slot: u16 = 0;
    while (slot < store.index_cap) : (slot += 1) {
        const entry = &base[slot];
        if (implementation.inUse(entry.*)) continue;
        var flags: u8 = implementation.flag_in_use;
        if (pinned) flags |= implementation.flag_pinned;
        entry.* = .{
            .key = key,
            .start_sector = start_sector,
            .byte_len = byte_len,
            .sector_count = sector_count,
            .flags = flags,
            .reserved = 0,
        };
        return @intCast(slot);
    }
    return -1;
}

// -------------------------------------------------------------------------
// Superblock
// -------------------------------------------------------------------------

/// Stamp sector 0 with the current geometry and the `clean` marker.
pub fn priv_cache_store_super_write(store_arg: ?*Store, clean: u32) RawErr {
    const store = store_arg orelse return reject("store", abi.err_null_ptr);
    const staging = store.staging orelse return reject("staging", abi.err_null_ptr);
    var sb = Super{
        .magic = super_magic,
        .version = format_version,
        .seq = store.next_seq,
        .clean = clean,
        .entry_count = 0,
        .live_sectors = store.live_sectors,
        .next_seq = store.next_seq,
        .log_start = store.log_start,
        .data_capacity = store.data_capacity,
        .logical_sectors = store.logical_sectors,
        .crc = 0,
    };
    if (store.index) |base| {
        for (base[0..store.index_cap]) |entry| {
            if (implementation.inUse(entry)) sb.entry_count += 1;
        }
    }
    sb.crc = implementation.crc32(std.mem.asBytes(&sb)[0..super_crc_span]);
    const sector = staging[0..sector_bytes];
    @memset(sector, 0);
    @memcpy(sector[0..@sizeOf(Super)], std.mem.asBytes(&sb));
    return priv_cache_store_sector_write(store, 0, staging);
}

/// Read and parse sector 0; an unreadable sector yields a zeroed record so the
/// caller falls back to a log replay (`internal_super_read`).
fn superRead(store: *Store, out_sb: *Super) RawErr {
    if (priv_cache_store_sector_read(store, 0, store.staging) != abi.ok) {
        out_sb.* = .{};
        return abi.ok;
    }
    const staging = store.staging orelse return reject("staging", abi.err_null_ptr);
    @memcpy(std.mem.asBytes(out_sb), staging[0..@sizeOf(Super)]);
    return abi.ok;
}

/// True when `sb` is a valid, clean-shutdown superblock
/// (`internal_super_is_clean`). Single-condition checks, so there is no
/// compound decision to MC/DC.
pub fn superIsClean(sb: *const Super) bool {
    if (sb.magic != super_magic) return false;
    const want = implementation.crc32(std.mem.asBytes(sb)[0..super_crc_span]);
    if (sb.crc != want) return false;
    if (sb.clean != abi.clean_clean) return false;
    return true;
}

// -------------------------------------------------------------------------
// Directory checkpoint save / load
// -------------------------------------------------------------------------

/// Pack the next run of in-use slots into one sector, advancing `slot`
/// (`internal_dir_pack_sector`). Returns how many entries landed.
fn dirPackSector(store: *Store, slot: *u16, out512: [*]u8) u32 {
    const sector = out512[0..sector_bytes];
    @memset(sector, 0);
    const base = store.index orelse return 0;
    var count: u32 = 0;
    var e: u32 = 0;
    while (e < dir_per_sector) : (e += 1) {
        while (slot.* < store.index_cap and !implementation.inUse(base[slot.*])) : (slot.* += 1) {}
        if (slot.* >= store.index_cap) break;
        const src = base[slot.*];
        const ent = DirEntry{
            .key = src.key,
            .start_sector = src.start_sector,
            .byte_len = src.byte_len,
            .sector_count = src.sector_count,
            .flags = src.flags & implementation.flag_pinned,
        };
        @memcpy(sector[e * dir_ent_bytes ..][0..@sizeOf(DirEntry)], std.mem.asBytes(&ent));
        slot.* += 1;
        count += 1;
    }
    return count;
}

/// Write the whole checkpoint directory and report the entry count.
pub fn priv_cache_store_dir_save(store_arg: ?*Store, out_entry_count: ?*u32) RawErr {
    const store = store_arg orelse return reject("store", abi.err_null_ptr);
    const out = out_entry_count orelse return reject("out_entry_count", abi.err_null_ptr);
    var count: u32 = 0;
    var slot: u16 = 0;
    var d: u16 = 0;
    while (d < store.checkpoint_dirs) : (d += 1) {
        // A null staging buffer packs nothing and fails on the write below,
        // which is the code path (and the error code) the C produced.
        if (store.staging) |staging| count += dirPackSector(store, &slot, staging);
        const wrote = priv_cache_store_sector_write(store, 1 + @as(u32, d), store.staging);
        if (propagate(wrote, "dir")) |code| return code;
    }
    out.* = count;
    return abi.ok;
}

/// Unpack one directory sector into the index (`internal_dir_unpack_sector`).
fn dirUnpackSector(store: *Store, in512: [*]const u8, loaded: *u32, entry_count: u32) void {
    var e: u32 = 0;
    while (e < dir_per_sector) : (e += 1) {
        if (loaded.* >= entry_count) break;
        var ent: DirEntry = .{};
        @memcpy(std.mem.asBytes(&ent), in512[e * dir_ent_bytes ..][0..@sizeOf(DirEntry)]);
        const pinned = (ent.flags & implementation.flag_pinned) != 0;
        _ = priv_cache_store_index_add(
            store,
            ent.key,
            ent.start_sector,
            ent.sector_count,
            ent.byte_len,
            pinned,
        );
        store.live_sectors += ent.sector_count;
        loaded.* += 1;
    }
}

/// Rebuild the index from the checkpoint directory (`internal_dir_load`).
fn dirLoad(store: *Store, entry_count: u32) RawErr {
    const staging = store.staging orelse return reject("staging", abi.err_null_ptr);
    if (entry_count > store.index_cap) return err_invalid_state;
    store.live_sectors = 0;
    var loaded: u32 = 0;
    var d: u16 = 0;
    while (d < store.checkpoint_dirs) : (d += 1) {
        if (loaded >= entry_count) break;
        const read = priv_cache_store_sector_read(store, 1 + @as(u32, d), staging);
        if (propagate(read, "dir")) |code| return code;
        dirUnpackSector(store, staging, &loaded, entry_count);
    }
    return abi.ok;
}

// -------------------------------------------------------------------------
// Append-log scan (unclean-shutdown replay)
// -------------------------------------------------------------------------

/// Read sector `s` and accept it only as a self-anchored, CRC-sealed, in-bounds
/// entry header (`internal_hdr_read`).
fn hdrRead(store: *Store, s: u32, out_hdr: *EntryHeader) bool {
    if (priv_cache_store_sector_read(store, s, store.staging) != abi.ok) return false;
    const staging = store.staging orelse return false;
    var h: EntryHeader = .{};
    @memcpy(std.mem.asBytes(&h), staging[0..@sizeOf(EntryHeader)]);
    if (h.magic != implementation.entry_magic) return false;
    if (h.start_sector != s) return false;
    if (h.sector_count == 0) return false;
    if (@as(u64, s) + h.sector_count > store.logical_sectors) return false;
    const want = implementation.crc32(std.mem.asBytes(&h)[0..implementation.header_crc_span]);
    if (h.hdr_crc != want) return false;
    out_hdr.* = h;
    return true;
}

/// Fold one validated header into the index, tracking the max sequence
/// (`internal_scan_accept`).
fn scanAccept(store: *Store, h: *const EntryHeader, max_seq: *u32) void {
    const pinned = (h.flags & implementation.flag_pinned) != 0;
    // Write-once: a duplicate key on flash is ignored (keep the first).
    if (priv_cache_store_index_find(store, h.key) >= 0) return;
    const slot = priv_cache_store_index_add(
        store,
        h.key,
        h.start_sector,
        h.sector_count,
        h.byte_len,
        pinned,
    );
    // Index full: stop indexing further entries.
    if (slot < 0) return;
    store.live_sectors += h.sector_count;
    if (h.seq > max_seq.*) max_seq.* = h.seq;
}

/// Replay the append log to rebuild the index (`internal_scan_log`). A torn
/// tail (payload written, header not) has no valid header and is discarded.
fn scanLog(store: *Store) RawErr {
    if (store.staging == null) return reject("staging", abi.err_null_ptr);
    store.live_sectors = 0;
    var max_seq: u32 = 0;
    var s: u32 = store.log_start;
    var guard: u32 = store.log_start;
    while (guard < store.logical_sectors) : (guard += 1) {
        if (s >= store.logical_sectors) break;
        var h: EntryHeader = .{};
        if (!hdrRead(store, s, &h)) {
            s += 1;
            continue;
        }
        scanAccept(store, &h, &max_seq);
        s += h.sector_count;
    }
    store.next_seq = max_seq + 1;
    return abi.ok;
}

// -------------------------------------------------------------------------
// init
// -------------------------------------------------------------------------

/// Validate the config's required members and sizes (`internal_validate_cfg`).
pub fn validateCfg(cfg_arg: ?*const Config) RawErr {
    const cfg = cfg_arg orelse return reject("cfg", abi.err_null_ptr);
    if (cfg.nor_flash == null) return reject("nor_flash", abi.err_null_ptr);
    if (cfg.nor_driver_init == null) return reject("nor_driver_init", abi.err_null_ptr);
    if (cfg.index == null) return reject("index", abi.err_null_ptr);
    if (cfg.staging == null) return reject("staging", abi.err_null_ptr);
    if (cfg.index_cap == 0) return abi.err_invalid_size;
    if (cfg.staging_bytes < sector_bytes) return abi.err_invalid_size;
    if (cfg.overprovision_pct > max_overprov) return err_invalid_arg;
    return abi.ok;
}

/// Derive the checkpoint span, the log start and the live-sector budget
/// (`internal_geometry`).
pub fn geometry(store: *Store, cfg: *const Config) RawErr {
    const dirs: u32 = (@as(u32, cfg.index_cap) + dir_per_sector - 1) / dir_per_sector;
    store.checkpoint_dirs = @intCast(dirs);
    store.log_start = 1 + dirs;
    store.logical_sectors = cfg.logical_sectors;
    if (cfg.logical_sectors < store.log_start + min_sectors) return abi.err_invalid_size;
    const op: u32 = if (cfg.overprovision_pct != 0) cfg.overprovision_pct else overprov_pct_default;
    const usable = cfg.logical_sectors - store.log_start;
    store.data_capacity = (usable * (pct_full - op)) / pct_full;
    if (store.data_capacity == 0) store.data_capacity = 1;
    return abi.ok;
}

/// Bring the injected LevelX NOR partition up: initialise once, format on
/// request, open (`internal_open_levelx`).
fn openLevelx(store: *Store, cfg: *const Config) RawErr {
    if (!lx_system_inited) {
        _ = _lx_nor_flash_initialize();
        lx_system_inited = true;
    }
    // LevelX takes a mutable CHAR* name but never writes it.
    const default_name: [*:0]const u8 = "ra8_cache";
    const name: ?[*:0]u8 = @constCast(cfg.name orelse default_name);
    if (cfg.format != 0) {
        if (_lx_nor_flash_format(store.flash, name, cfg.nor_driver_init, null) != lx_success) {
            return abi.err_hw_init_failed;
        }
    }
    if (_lx_nor_flash_open(store.flash, name, cfg.nor_driver_init) != lx_success) {
        return abi.err_hw_init_failed;
    }
    // Stamp an empty clean superblock at format time so a fresh mount loads the
    // (empty) checkpoint instead of scanning the whole -- as-yet unmapped -- log
    // region, which on LevelX would allocate a physical sector per read.
    if (cfg.format != 0) {
        const wrote = priv_cache_store_super_write(store, abi.clean_clean);
        if (propagate(wrote, "format super")) |code| return code;
    }
    return abi.ok;
}

/// Checkpoint load on a clean superblock, log replay on anything else
/// (`internal_recover`).
fn recover(store: *Store, sb: *const Super) RawErr {
    if (superIsClean(sb)) {
        store.next_seq = sb.next_seq;
        store.flash_state = @intCast(abi.clean_clean);
        return dirLoad(store, sb.entry_count);
    }
    store.flash_state = @intCast(abi.clean_dirty);
    return scanLog(store);
}

/// Read sector 0, then recover the index (`internal_mount`).
fn mountStore(store: *Store) RawErr {
    if (store.staging == null) return reject("staging", abi.err_null_ptr);
    var sb: Super = .{};
    if (propagate(superRead(store, &sb), "super")) |code| return code;
    return recover(store, &sb);
}

/// Bind the caller-owned buffers and reset the counters (`internal_init_fields`).
fn initFields(store: *Store, cfg: *const Config) void {
    store.* = .{};
    store.flash = cfg.nor_flash;
    store.index = cfg.index;
    store.staging = cfg.staging;
    store.index_cap = cfg.index_cap;
    if (store.index) |base| {
        for (base[0..store.index_cap]) |*entry| entry.* = .{};
    }
    store.next_seq = 1;
    store.live_sectors = 0;
}

/// Geometry, LevelX open, mount (`internal_bringup`).
fn bringup(store: *Store, cfg: *const Config) RawErr {
    if (propagate(geometry(store, cfg), "geometry")) |code| return code;
    if (propagate(openLevelx(store, cfg), "levelx open")) |code| return code;
    if (propagate(mountStore(store), "mount")) |code| return code;
    return abi.ok;
}

/// Mount the cache store over an injected LevelX NOR partition.
pub fn initStore(store_arg: ?*Store, cfg_arg: ?*const Config) RawErr {
    const store = store_arg orelse return reject("store", abi.err_null_ptr);
    if (propagate(validateCfg(cfg_arg), "cfg")) |code| return code;
    const cfg = cfg_arg.?;
    initFields(store, cfg);
    if (propagate(bringup(store, cfg), "bringup")) |code| return code;
    store.inited = 1;
    return abi.ok;
}
