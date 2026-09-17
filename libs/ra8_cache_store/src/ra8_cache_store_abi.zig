//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the runtime half of `libs/ra8_cache_store/inc/ra8_cache_store.h`:
//! put / get / read / evict / pin / sync / close. The allocator, the CRC and the
//! sector arithmetic live in `internal/root.zig`; this file owns the store
//! layout, the argument guards, the log lines and the `ra8_err_t` mapping.
//!
//! The mount and recovery half stays in `ra8_cache_store_mount.c` on this
//! branch, so the seven `priv_cache_store_*` helpers it defines are declared
//! here as externs. That is the whole seam: nothing in this file speaks to
//! LevelX except `lx_nor_flash_close` on the close path, which is what keeps
//! the runtime path testable over a RAM medium.
//!
//! Guard order is part of the contract. `tests/misc/src/test_ra8_cache_store.c`
//! tells `null_ptr` from `not_initialized` and `invalid_size` from `exists` by
//! which check fires first, so the checks below run in exactly the order the C
//! wrote them.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// One in-RAM index slot (`ra8_cache_store_entry_t`).
pub const Entry = implementation.Entry;
/// On-flash entry header (`ra8_cs_entry_hdr_t`).
pub const EntryHeader = implementation.EntryHeader;
/// LevelX logical-sector size.
pub const sector_bytes = implementation.sector_bytes;

/// Raw `ra8_err_t` as it crosses the ABI.
pub const RawErr = u16;

pub const ok: RawErr = 0;
pub const err_no_mem: RawErr = 0x102;
pub const err_invalid_size: RawErr = 0x105;
pub const err_not_found: RawErr = 0x106;
pub const err_busy: RawErr = 0x109;
pub const err_exists: RawErr = 0x10C;
pub const err_not_initialized: RawErr = 0x10F;
pub const err_hw_init_failed: RawErr = 0x201;
pub const err_out_of_range: RawErr = 0x208;
pub const err_null_ptr: RawErr = 0x504;

/// Session open or crashed (`k_ra8_cs_dirty`).
pub const clean_dirty: u32 = 0;
/// Clean shutdown (`k_ra8_cs_clean`).
pub const clean_clean: u32 = 1;

/// Mounted store handle (`ra8_cache_store_t`).
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
    inited: bool = false,
};

/// Streaming read cursor over one entry (`ra8_cache_store_reader_t`).
pub const Reader = extern struct {
    store: ?*const Store = null,
    data_start: u32 = 0,
    data_sectors: u32 = 0,
    byte_len: u32 = 0,
};

comptime {
    // Pointer-width aware, so the asserts hold on the host and on Arm alike.
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
}

// The mount / recovery translation unit owns these; they are the only way this
// file reaches the medium.
extern fn priv_cache_store_crc32(data: ?[*]const u8, len: u32) u32;
extern fn priv_cache_store_sector_read(store: ?*const Store, sector: u32, out512: ?[*]u8) RawErr;
extern fn priv_cache_store_sector_write(store: ?*Store, sector: u32, in512: ?[*]const u8) RawErr;
extern fn priv_cache_store_sector_release(store: ?*Store, sector: u32) RawErr;
extern fn priv_cache_store_index_find(store: ?*const Store, key: u32) i32;
extern fn priv_cache_store_index_add(
    store: ?*Store,
    key: u32,
    start_sector: u32,
    sector_count: u16,
    byte_len: u32,
    pinned: bool,
) i32;
extern fn priv_cache_store_super_write(store: ?*Store, clean: u32) RawErr;
extern fn priv_cache_store_dir_save(store: ?*Store, out_entry_count: ?*u32) RawErr;

// `lx_api.h` maps the public `lx_nor_flash_close()` name onto this symbol, so
// the Zig side has to name the implementation rather than the macro.
extern fn _lx_nor_flash_close(flash: ?*anyopaque) c_uint;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;

const tag: [*:0]const u8 = "ra8_cache_store";

/// `RA8_CHECK_NULL_PTR` / `RA8_VALIDATE_INIT`: log and hand back the code.
fn reject(message: [*:0]const u8, code: RawErr) RawErr {
    ra8_log_emit_error(tag, message);
    return code;
}

/// `RA8_RETURN_ON_ERROR`: log the site and the value, then propagate.
fn propagate(code: RawErr, message: [*:0]const u8) ?RawErr {
    if (code == ok) return null;
    ra8_log_emit_error(tag, message);
    ra8_log_emit_error_val(tag, "Error", code);
    return code;
}

/// The live index as a slice, or null when the caller supplied no index array.
fn indexSlice(store: *const Store) ?[]Entry {
    const base = store.index orelse return null;
    return base[0..store.index_cap];
}

/// In-use slot count; 0 when there is no index, as `internal_index_used` did.
fn indexUsed(store: *const Store) u16 {
    const entries = indexSlice(store) orelse return 0;
    return implementation.indexUsed(entries);
}

/// Every put precondition plus the run length (`internal_put_check`).
fn putCheck(store_arg: ?*const Store, key: u32, data: ?[*]const u8, len: u32, out_count: *u32) RawErr {
    const store = store_arg orelse return reject("store", err_null_ptr);
    if (data == null) return reject("data", err_null_ptr);
    if (!store.inited) return reject("store", err_not_initialized);
    if (len == 0) return err_invalid_size;
    if (priv_cache_store_index_find(store, key) >= 0) return err_exists;
    if (indexUsed(store) >= store.index_cap) return err_no_mem;
    const count = implementation.runLength(len) orelse return err_invalid_size;
    if (@as(u64, store.live_sectors) + count > store.data_capacity) return err_no_mem;
    out_count.* = count;
    return ok;
}

/// Stamp a dirty superblock before mutating, unless already dirty
/// (`internal_mark_dirty`).
fn markDirty(store: *Store) RawErr {
    if (store.flash_state == @as(u8, @intCast(clean_dirty))) return ok;
    if (propagate(priv_cache_store_super_write(store, clean_dirty), "mark dirty")) |code| return code;
    store.flash_state = @intCast(clean_dirty);
    return ok;
}

/// Directory plus a clean superblock, dirty marker first (`internal_checkpoint`).
fn checkpoint(store: *Store) RawErr {
    if (propagate(markDirty(store), "pre-dirty")) |code| return code;
    var count: u32 = 0;
    if (propagate(priv_cache_store_dir_save(store, &count), "dir save")) |code| return code;
    if (propagate(priv_cache_store_super_write(store, clean_clean), "clean super")) |code| return code;
    store.flash_state = @intCast(clean_clean);
    return ok;
}

/// First-fit a free run in the log region (`internal_alloc_run`).
fn allocRun(store: *const Store, count: u32, out_start: *u32) RawErr {
    if (count == 0) return err_invalid_size;
    // With no index array no candidate run can be proven free, which is the
    // `internal_run_free` -> false path the C fell through to no_mem on.
    const entries = indexSlice(store) orelse return err_no_mem;
    const start = implementation.allocRun(
        entries,
        store.log_start,
        store.logical_sectors,
        count,
    ) orelse return err_no_mem;
    out_start.* = start;
    return ok;
}

/// Payload sectors first, header last (`internal_write_entry`).
fn writeEntry(
    store: *Store,
    start: u32,
    seq: u32,
    key: u32,
    data: [*]const u8,
    len: u32,
    count: u16,
) RawErr {
    const staging = store.staging orelse return reject("staging", err_null_ptr);
    const data_sectors: u16 = count - 1;
    var offset: u32 = 0;
    var sector: u16 = 0;
    while (sector < data_sectors) : (sector += 1) {
        const chunk = implementation.payloadChunk(len, offset);
        @memset(staging[0..sector_bytes], 0);
        @memcpy(staging[0..chunk], data[offset..][0..chunk]);
        if (propagate(
            priv_cache_store_sector_write(store, start + 1 + @as(u32, sector), staging),
            "payload",
        )) |code| return code;
        offset += chunk;
    }

    var header = EntryHeader{
        .seq = seq,
        .key = key,
        .byte_len = len,
        .start_sector = start,
        .sector_count = count,
        .flags = 0,
    };
    header.hdr_crc = priv_cache_store_crc32(
        @ptrCast(&header),
        implementation.header_crc_span,
    );
    @memset(staging[0..sector_bytes], 0);
    @memcpy(staging[0..@sizeOf(EntryHeader)], std.mem.asBytes(&header));
    return priv_cache_store_sector_write(store, start, staging);
}

/// Copy the payload slice at `byte_pos`, bounded by its sector
/// (`internal_read_at`).
fn readAt(
    store: *const Store,
    data_start: u32,
    byte_pos: u64,
    dst: [*]u8,
    max: u32,
    out_copied: *u32,
) RawErr {
    const staging = store.staging orelse return reject("staging", err_null_ptr);
    const slice = implementation.sliceAt(data_start, byte_pos, max);
    if (propagate(priv_cache_store_sector_read(store, slice.sector, staging), "read")) |code| {
        return code;
    }
    @memcpy(dst[0..slice.chunk], staging[slice.offset..][0..slice.chunk]);
    out_copied.* = slice.chunk;
    return ok;
}

/// Stream `len` bytes sector by sector (`internal_read_stream`).
fn readStream(
    store: *const Store,
    data_start: u32,
    data_sectors: u32,
    offset: u64,
    buf: [*]u8,
    len: u32,
) RawErr {
    var done: u32 = 0;
    const max_iter: u32 = data_sectors + 1;
    var guard: u32 = 0;
    while (guard <= max_iter) : (guard += 1) {
        if (done >= len) break;
        var copied: u32 = 0;
        if (propagate(
            readAt(store, data_start, offset + done, buf + done, len - done, &copied),
            "at",
        )) |code| return code;
        done += copied;
    }
    return ok;
}

/// Release every sector of a run (`internal_release_run`).
fn releaseRun(store: *Store, start: u32, count: u16) RawErr {
    var sector: u16 = 0;
    while (sector < count) : (sector += 1) {
        if (propagate(
            priv_cache_store_sector_release(store, start + @as(u32, sector)),
            "release",
        )) |code| return code;
    }
    return ok;
}

/// Append one write-once entry under `key`.
export fn ra8_cache_store_put(store_arg: ?*Store, key: u32, data: ?[*]const u8, len: u32) RawErr {
    var count: u32 = 0;
    if (propagate(putCheck(store_arg, key, data, len, &count), "check")) |code| return code;
    const store = store_arg.?;
    const payload = data.?;
    if (propagate(markDirty(store), "dirty")) |code| return code;
    var start: u32 = 0;
    if (propagate(allocRun(store, count, &start), "alloc")) |code| return code;
    if (propagate(
        writeEntry(store, start, store.next_seq, key, payload, len, @intCast(count)),
        "write",
    )) |code| return code;
    _ = priv_cache_store_index_add(store, key, start, @intCast(count), len, false);
    store.live_sectors += count;
    store.next_seq += 1;
    return ok;
}

/// Open a streaming reader over the entry under `key`.
export fn ra8_cache_store_get(store_arg: ?*const Store, key: u32, out_reader: ?*Reader) RawErr {
    const store = store_arg orelse return reject("store", err_null_ptr);
    const reader = out_reader orelse return reject("out_reader", err_null_ptr);
    if (!store.inited) return reject("store", err_not_initialized);
    const slot = priv_cache_store_index_find(store, key);
    if (slot < 0) return err_not_found;
    const entry = (store.index.?)[@intCast(slot)];
    reader.* = .{
        .store = store,
        .data_start = entry.start_sector + 1,
        .data_sectors = @as(u32, entry.sector_count) - 1,
        .byte_len = entry.byte_len,
    };
    return ok;
}

/// `ra8_vsource` read seam over a reader (`ra8_cache_store_read`).
export fn ra8_cache_store_read(ctx: ?*anyopaque, offset: u64, buf: ?[*]u8, len: u32) RawErr {
    const raw_ctx = ctx orelse return reject("ctx", err_null_ptr);
    const dst = buf orelse return reject("buf", err_null_ptr);
    const reader: *const Reader = @ptrCast(@alignCast(raw_ctx));
    const store = reader.store orelse return reject("reader store", err_null_ptr);
    if (offset + len > reader.byte_len) return err_out_of_range;
    return readStream(store, reader.data_start, reader.data_sectors, offset, dst, len);
}

/// Drop an entry and reclaim its sectors; no write-back ever happens.
export fn ra8_cache_store_evict(store_arg: ?*Store, key: u32) RawErr {
    const store = store_arg orelse return reject("store", err_null_ptr);
    if (!store.inited) return reject("store", err_not_initialized);
    const slot = priv_cache_store_index_find(store, key);
    if (slot < 0) return err_not_found;
    const entry = &(store.index.?)[@intCast(slot)];
    if (implementation.isPinned(entry.*)) return err_busy;
    if (propagate(markDirty(store), "dirty")) |code| return code;
    if (propagate(releaseRun(store, entry.start_sector, entry.sector_count), "release")) |code| {
        return code;
    }
    store.live_sectors -= entry.sector_count;
    entry.* = .{};
    return ok;
}

/// Set or clear the never-evict marker on an entry.
export fn ra8_cache_store_pin(store_arg: ?*Store, key: u32, pin: bool) RawErr {
    const store = store_arg orelse return reject("store", err_null_ptr);
    if (!store.inited) return reject("store", err_not_initialized);
    const slot = priv_cache_store_index_find(store, key);
    if (slot < 0) return err_not_found;
    if (propagate(markDirty(store), "dirty")) |code| return code;
    const entry = &(store.index.?)[@intCast(slot)];
    if (pin) {
        entry.flags |= implementation.flag_pinned;
    } else {
        entry.flags &= ~implementation.flag_pinned;
    }
    return ok;
}

/// Checkpoint the directory and stamp a clean superblock.
export fn ra8_cache_store_sync(store_arg: ?*Store) RawErr {
    const store = store_arg orelse return reject("store", err_null_ptr);
    if (!store.inited) return reject("store", err_not_initialized);
    return checkpoint(store);
}

/// Checkpoint, close the LevelX partition and mark the handle uninitialised.
export fn ra8_cache_store_close(store_arg: ?*Store) RawErr {
    const store = store_arg orelse return reject("store", err_null_ptr);
    if (!store.inited) return reject("store", err_not_initialized);
    if (propagate(checkpoint(store), "checkpoint")) |code| return code;
    _ = _lx_nor_flash_close(store.flash);
    store.inited = false;
    return ok;
}
