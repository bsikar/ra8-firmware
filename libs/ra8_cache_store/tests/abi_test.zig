//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the C ABI membrane: put / get / read / evict / pin /
//! sync / close over a RAM medium, plus the argument-guard order the C host
//! suite depends on to tell one error code from another.
//!
//! The seven `priv_cache_store_*` helpers still live in the C mount TU, so this
//! file exports its own implementations of them over a fake sector array. That
//! is the same substitution the linker performs in the real build, which means
//! the membrane under test is byte-identical to the shipped one.

const std = @import("std");
const abi = @import("abi");

const Entry = abi.Entry;
const Store = abi.Store;
const Reader = abi.Reader;

const total_sectors: u32 = 64;
const sector_bytes: u32 = abi.sector_bytes;

var medium: [total_sectors][sector_bytes]u8 = undefined;
var mapped: [total_sectors]bool = @splat(false);
var index_slots: [4]Entry = @splat(Entry{});
var staging: [sector_bytes]u8 = @splat(0);

/// -1 means unlimited; otherwise the number of sector writes still allowed.
var write_budget: i32 = -1;
var release_fails: bool = false;
var super_writes: u32 = 0;
var last_clean: u32 = 0xFFFF_FFFF;
var dir_saves: u32 = 0;
var closes: u32 = 0;
var super_fails: bool = false;
var dir_fails: bool = false;

fn resetMedium() void {
    for (&medium) |*sector| @memset(sector, 0);
    mapped = @splat(false);
    index_slots = @splat(Entry{});
    staging = @splat(0);
    write_budget = -1;
    release_fails = false;
    super_writes = 0;
    last_clean = 0xFFFF_FFFF;
    dir_saves = 0;
    closes = 0;
    super_fails = false;
    dir_fails = false;
}

fn mountedStore() Store {
    return .{
        .flash = @ptrFromInt(0x1000),
        .index = &index_slots,
        .staging = &staging,
        .index_cap = index_slots.len,
        .checkpoint_dirs = 1,
        .logical_sectors = total_sectors,
        .log_start = 2,
        .data_capacity = 32,
        .live_sectors = 0,
        .next_seq = 1,
        .flash_state = @intCast(abi.clean_clean),
        .inited = true,
    };
}

// --- the mount TU's helpers, standing in over a RAM medium -----------------

export fn priv_cache_store_crc32(data: ?[*]const u8, len: u32) u32 {
    const bytes = data orelse return 0;
    if (len == 0) return 0;
    var crc: u32 = 0xFFFF_FFFF;
    for (bytes[0..len]) |byte| {
        crc ^= byte;
        var bit: u8 = 0;
        while (bit < 8) : (bit += 1) {
            const lsb_set = (crc & 1) != 0;
            crc >>= 1;
            if (lsb_set) crc ^= 0xEDB8_8320;
        }
    }
    return crc ^ 0xFFFF_FFFF;
}

export fn priv_cache_store_sector_read(store: ?*const Store, sector: u32, out512: ?[*]u8) u16 {
    if (store == null) return abi.err_null_ptr;
    const dst = out512 orelse return abi.err_null_ptr;
    if (sector >= total_sectors) return abi.err_hw_init_failed;
    if (!mapped[sector]) return abi.err_not_found;
    @memcpy(dst[0..sector_bytes], &medium[sector]);
    return abi.ok;
}

export fn priv_cache_store_sector_write(store: ?*Store, sector: u32, in512: ?[*]const u8) u16 {
    if (store == null) return abi.err_null_ptr;
    const src = in512 orelse return abi.err_null_ptr;
    if (sector >= total_sectors) return abi.err_hw_init_failed;
    if (write_budget == 0) return abi.err_hw_init_failed;
    if (write_budget > 0) write_budget -= 1;
    @memcpy(&medium[sector], src[0..sector_bytes]);
    mapped[sector] = true;
    return abi.ok;
}

export fn priv_cache_store_sector_release(store: ?*Store, sector: u32) u16 {
    if (store == null) return abi.err_null_ptr;
    if (release_fails) return abi.err_hw_init_failed;
    if (sector >= total_sectors) return abi.err_hw_init_failed;
    mapped[sector] = false;
    return abi.ok;
}

export fn priv_cache_store_index_find(store: ?*const Store, key: u32) i32 {
    const handle = store orelse return -1;
    const base = handle.index orelse return -1;
    var slot: u16 = 0;
    while (slot < handle.index_cap) : (slot += 1) {
        const entry = base[slot];
        if ((entry.flags & 1) == 0) continue;
        if (entry.key == key) return @intCast(slot);
    }
    return -1;
}

export fn priv_cache_store_index_add(
    store: ?*Store,
    key: u32,
    start_sector: u32,
    sector_count: u16,
    byte_len: u32,
    pinned: bool,
) i32 {
    const handle = store orelse return -1;
    const base = handle.index orelse return -1;
    var slot: u16 = 0;
    while (slot < handle.index_cap) : (slot += 1) {
        if ((base[slot].flags & 1) != 0) continue;
        base[slot] = .{
            .key = key,
            .start_sector = start_sector,
            .byte_len = byte_len,
            .sector_count = sector_count,
            .flags = if (pinned) 0b11 else 0b01,
        };
        return @intCast(slot);
    }
    return -1;
}

export fn priv_cache_store_super_write(store: ?*Store, clean: u32) u16 {
    if (store == null) return abi.err_null_ptr;
    if (super_fails) return abi.err_hw_init_failed;
    super_writes += 1;
    last_clean = clean;
    return abi.ok;
}

export fn priv_cache_store_dir_save(store: ?*Store, out_entry_count: ?*u32) u16 {
    const handle = store orelse return abi.err_null_ptr;
    const out = out_entry_count orelse return abi.err_null_ptr;
    if (dir_fails) return abi.err_hw_init_failed;
    dir_saves += 1;
    var used: u32 = 0;
    if (handle.index) |base| {
        var slot: u16 = 0;
        while (slot < handle.index_cap) : (slot += 1) {
            if ((base[slot].flags & 1) != 0) used += 1;
        }
    }
    out.* = used;
    return abi.ok;
}

export fn _lx_nor_flash_close(flash: ?*anyopaque) c_uint {
    _ = flash;
    closes += 1;
    return 0;
}

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    _ = message;
}

export fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void {
    _ = tag;
    _ = message;
    _ = value;
}

// --- the exported ABI under test -------------------------------------------

extern fn ra8_cache_store_put(store: ?*Store, key: u32, data: ?[*]const u8, len: u32) u16;
extern fn ra8_cache_store_get(store: ?*const Store, key: u32, out_reader: ?*Reader) u16;
extern fn ra8_cache_store_read(ctx: ?*anyopaque, offset: u64, buf: ?[*]u8, len: u32) u16;
extern fn ra8_cache_store_evict(store: ?*Store, key: u32) u16;
extern fn ra8_cache_store_pin(store: ?*Store, key: u32, pin: bool) u16;
extern fn ra8_cache_store_sync(store: ?*Store) u16;
extern fn ra8_cache_store_close(store: ?*Store) u16;

const key_a: u32 = 0xA1A1_A1A1;
const key_b: u32 = 0xB2B2_B2B2;

fn payload(comptime len: u32, seed: u8) [len]u8 {
    var bytes: [len]u8 = undefined;
    for (&bytes, 0..) |*byte, i| byte.* = seed +% @as(u8, @truncate(i));
    return bytes;
}

test "put then get exposes the payload geometry" {
    resetMedium();
    var store = mountedStore();
    const data = payload(600, 3);

    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expectEqual(@as(u32, 3), store.live_sectors);
    try std.testing.expectEqual(@as(u32, 2), store.next_seq);

    var reader = Reader{};
    try std.testing.expectEqual(abi.ok, ra8_cache_store_get(&store, key_a, &reader));
    try std.testing.expectEqual(@as(u32, 3), reader.data_start);
    try std.testing.expectEqual(@as(u32, 2), reader.data_sectors);
    try std.testing.expectEqual(@as(u32, 600), reader.byte_len);
}

test "put writes the header last and seals it" {
    resetMedium();
    var store = mountedStore();
    const data = payload(16, 1);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));

    const header: *const abi.EntryHeader = @ptrCast(@alignCast(&medium[2]));
    try std.testing.expectEqual(@as(u32, 0x5243_5345), header.magic);
    try std.testing.expectEqual(@as(u32, 1), header.seq);
    try std.testing.expectEqual(key_a, header.key);
    try std.testing.expectEqual(@as(u32, 16), header.byte_len);
    try std.testing.expectEqual(@as(u32, 2), header.start_sector);
    try std.testing.expectEqual(@as(u16, 2), header.sector_count);
    try std.testing.expectEqual(@as(u16, 0), header.flags);
    const sealed = priv_cache_store_crc32(@ptrCast(header), 24);
    try std.testing.expectEqual(sealed, header.hdr_crc);
}

test "read streams a payload back byte for byte" {
    resetMedium();
    var store = mountedStore();
    const data = payload(1300, 7);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));

    var reader = Reader{};
    try std.testing.expectEqual(abi.ok, ra8_cache_store_get(&store, key_a, &reader));
    var out: [1300]u8 = @splat(0);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_read(&reader, 0, &out, out.len));
    try std.testing.expectEqualSlices(u8, &data, &out);
}

test "read honours an offset in the middle of a sector" {
    resetMedium();
    var store = mountedStore();
    const data = payload(1024, 9);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));

    var reader = Reader{};
    try std.testing.expectEqual(abi.ok, ra8_cache_store_get(&store, key_a, &reader));
    var out: [40]u8 = @splat(0);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_read(&reader, 500, &out, out.len));
    try std.testing.expectEqualSlices(u8, data[500..540], &out);
}

test "read rejects a slice past the payload end" {
    resetMedium();
    var store = mountedStore();
    const data = payload(64, 2);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));

    var reader = Reader{};
    try std.testing.expectEqual(abi.ok, ra8_cache_store_get(&store, key_a, &reader));
    var out: [8]u8 = @splat(0);
    try std.testing.expectEqual(abi.err_out_of_range, ra8_cache_store_read(&reader, 60, &out, out.len));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_read(&reader, 56, &out, out.len));
}

test "read of zero bytes at the very end is in range" {
    resetMedium();
    var store = mountedStore();
    const data = payload(32, 4);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    var reader = Reader{};
    try std.testing.expectEqual(abi.ok, ra8_cache_store_get(&store, key_a, &reader));
    var out: [1]u8 = @splat(0);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_read(&reader, 32, &out, 0));
}

test "read guards ctx, buf and the reader's store in that order" {
    resetMedium();
    var out: [4]u8 = @splat(0);
    try std.testing.expectEqual(abi.err_null_ptr, ra8_cache_store_read(null, 0, &out, out.len));
    var reader = Reader{};
    try std.testing.expectEqual(abi.err_null_ptr, ra8_cache_store_read(&reader, 0, null, 4));
    try std.testing.expectEqual(abi.err_null_ptr, ra8_cache_store_read(&reader, 0, &out, out.len));
}

test "put is write-once: a duplicate key is refused" {
    resetMedium();
    var store = mountedStore();
    const data = payload(32, 5);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expectEqual(abi.err_exists, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expectEqual(@as(u32, 2), store.live_sectors);
}

test "put guards store then data then init then size" {
    resetMedium();
    const data = payload(8, 1);
    try std.testing.expectEqual(abi.err_null_ptr, ra8_cache_store_put(null, key_a, &data, data.len));

    var store = mountedStore();
    try std.testing.expectEqual(abi.err_null_ptr, ra8_cache_store_put(&store, key_a, null, data.len));

    store.inited = false;
    try std.testing.expectEqual(abi.err_not_initialized, ra8_cache_store_put(&store, key_a, &data, data.len));

    store.inited = true;
    try std.testing.expectEqual(abi.err_invalid_size, ra8_cache_store_put(&store, key_a, &data, 0));
}

test "put refuses a run that would overflow sector_count" {
    resetMedium();
    var store = mountedStore();
    const data = payload(8, 1);
    const too_long: u32 = abi.sector_bytes * 0xFFFF;
    try std.testing.expectEqual(abi.err_invalid_size, ra8_cache_store_put(&store, key_a, &data, too_long));
}

test "put refuses to exceed the overprovisioned budget" {
    resetMedium();
    var store = mountedStore();
    store.data_capacity = 3;
    const data = payload(1200, 1);
    try std.testing.expectEqual(abi.err_no_mem, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expectEqual(@as(u32, 0), store.live_sectors);
}

test "put refuses once every index slot is in use" {
    resetMedium();
    var store = mountedStore();
    store.index_cap = 1;
    const data = payload(16, 1);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expectEqual(abi.err_no_mem, ra8_cache_store_put(&store, key_b, &data, data.len));
}

test "put reports no_mem when the log region cannot fit the run" {
    resetMedium();
    var store = mountedStore();
    store.logical_sectors = 3;
    const data = payload(1200, 1);
    try std.testing.expectEqual(abi.err_no_mem, ra8_cache_store_put(&store, key_a, &data, data.len));
}

test "put with no index array cannot allocate" {
    resetMedium();
    var store = mountedStore();
    store.index = null;
    const data = payload(16, 1);
    try std.testing.expectEqual(abi.err_no_mem, ra8_cache_store_put(&store, key_a, &data, data.len));
}

test "a torn payload write leaves no header behind" {
    resetMedium();
    var store = mountedStore();
    const data = payload(1200, 6);
    write_budget = 1; // one payload sector lands, the rest fails
    try std.testing.expectEqual(
        abi.err_hw_init_failed,
        ra8_cache_store_put(&store, key_a, &data, data.len),
    );
    try std.testing.expect(!mapped[2]); // the header sector was never written
    try std.testing.expectEqual(@as(u32, 0), store.live_sectors);
}

test "put marks the superblock dirty before touching the log" {
    resetMedium();
    var store = mountedStore();
    const data = payload(16, 1);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expectEqual(@as(u32, 1), super_writes);
    try std.testing.expectEqual(abi.clean_dirty, last_clean);
    try std.testing.expectEqual(@as(u8, 0), store.flash_state);
}

test "a second mutation does not restamp an already dirty superblock" {
    resetMedium();
    var store = mountedStore();
    const data = payload(16, 1);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_b, &data, data.len));
    try std.testing.expectEqual(@as(u32, 1), super_writes);
}

test "put propagates a failed dirty stamp" {
    resetMedium();
    var store = mountedStore();
    super_fails = true;
    const data = payload(16, 1);
    try std.testing.expectEqual(
        abi.err_hw_init_failed,
        ra8_cache_store_put(&store, key_a, &data, data.len),
    );
}

test "get guards store then out_reader then init" {
    resetMedium();
    var reader = Reader{};
    try std.testing.expectEqual(abi.err_null_ptr, ra8_cache_store_get(null, key_a, &reader));
    var store = mountedStore();
    try std.testing.expectEqual(abi.err_null_ptr, ra8_cache_store_get(&store, key_a, null));
    store.inited = false;
    try std.testing.expectEqual(abi.err_not_initialized, ra8_cache_store_get(&store, key_a, &reader));
}

test "get misses on an unknown key" {
    resetMedium();
    var store = mountedStore();
    var reader = Reader{};
    try std.testing.expectEqual(abi.err_not_found, ra8_cache_store_get(&store, key_a, &reader));
}

test "evict frees the sectors and the slot" {
    resetMedium();
    var store = mountedStore();
    const data = payload(600, 8);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expect(mapped[2]);

    try std.testing.expectEqual(abi.ok, ra8_cache_store_evict(&store, key_a));
    try std.testing.expectEqual(@as(u32, 0), store.live_sectors);
    try std.testing.expect(!mapped[2]);
    try std.testing.expect(!mapped[3]);
    try std.testing.expect(!mapped[4]);

    var reader = Reader{};
    try std.testing.expectEqual(abi.err_not_found, ra8_cache_store_get(&store, key_a, &reader));
}

test "evict refuses a pinned entry" {
    resetMedium();
    var store = mountedStore();
    const data = payload(16, 1);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_pin(&store, key_a, true));
    try std.testing.expectEqual(abi.err_busy, ra8_cache_store_evict(&store, key_a));
    try std.testing.expectEqual(@as(u32, 2), store.live_sectors);
}

test "unpinning makes an entry evictable again" {
    resetMedium();
    var store = mountedStore();
    const data = payload(16, 1);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_pin(&store, key_a, true));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_pin(&store, key_a, false));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_evict(&store, key_a));
}

test "evict guards store then init then the key, and propagates a release failure" {
    resetMedium();
    try std.testing.expectEqual(abi.err_null_ptr, ra8_cache_store_evict(null, key_a));
    var store = mountedStore();
    store.inited = false;
    try std.testing.expectEqual(abi.err_not_initialized, ra8_cache_store_evict(&store, key_a));
    store.inited = true;
    try std.testing.expectEqual(abi.err_not_found, ra8_cache_store_evict(&store, key_a));

    const data = payload(16, 1);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    release_fails = true;
    try std.testing.expectEqual(abi.err_hw_init_failed, ra8_cache_store_evict(&store, key_a));
    try std.testing.expectEqual(@as(u32, 2), store.live_sectors);
}

test "pin guards store then init then the key" {
    resetMedium();
    try std.testing.expectEqual(abi.err_null_ptr, ra8_cache_store_pin(null, key_a, true));
    var store = mountedStore();
    store.inited = false;
    try std.testing.expectEqual(abi.err_not_initialized, ra8_cache_store_pin(&store, key_a, true));
    store.inited = true;
    try std.testing.expectEqual(abi.err_not_found, ra8_cache_store_pin(&store, key_a, true));
}

test "pin leaves the in-use bit alone" {
    resetMedium();
    var store = mountedStore();
    const data = payload(16, 1);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_pin(&store, key_a, true));
    try std.testing.expectEqual(@as(u8, 0b11), index_slots[0].flags);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_pin(&store, key_a, false));
    try std.testing.expectEqual(@as(u8, 0b01), index_slots[0].flags);
}

test "sync checkpoints dirty first, then the directory, then a clean super" {
    resetMedium();
    var store = mountedStore();
    const data = payload(16, 1);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    super_writes = 0;

    try std.testing.expectEqual(abi.ok, ra8_cache_store_sync(&store));
    try std.testing.expectEqual(@as(u32, 1), dir_saves);
    try std.testing.expectEqual(@as(u32, 1), super_writes); // already dirty, so only the clean one
    try std.testing.expectEqual(abi.clean_clean, last_clean);
    try std.testing.expectEqual(@as(u8, 1), store.flash_state);
}

test "sync on a clean store stamps dirty before the directory" {
    resetMedium();
    var store = mountedStore();
    try std.testing.expectEqual(abi.ok, ra8_cache_store_sync(&store));
    try std.testing.expectEqual(@as(u32, 2), super_writes);
    try std.testing.expectEqual(abi.clean_clean, last_clean);
}

test "sync guards store then init and propagates a directory failure" {
    resetMedium();
    try std.testing.expectEqual(abi.err_null_ptr, ra8_cache_store_sync(null));
    var store = mountedStore();
    store.inited = false;
    try std.testing.expectEqual(abi.err_not_initialized, ra8_cache_store_sync(&store));
    store.inited = true;
    dir_fails = true;
    try std.testing.expectEqual(abi.err_hw_init_failed, ra8_cache_store_sync(&store));
    try std.testing.expectEqual(@as(u8, 0), store.flash_state); // left dirty, so mount replays
}

test "close checkpoints, closes LevelX and clears inited" {
    resetMedium();
    var store = mountedStore();
    const data = payload(16, 1);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));

    try std.testing.expectEqual(abi.ok, ra8_cache_store_close(&store));
    try std.testing.expectEqual(@as(u32, 1), dir_saves);
    try std.testing.expectEqual(abi.clean_clean, last_clean);
    try std.testing.expectEqual(@as(u32, 1), closes);
    try std.testing.expect(!store.inited);
}

test "close refuses a second call" {
    resetMedium();
    var store = mountedStore();
    try std.testing.expectEqual(abi.ok, ra8_cache_store_close(&store));
    try std.testing.expectEqual(abi.err_not_initialized, ra8_cache_store_close(&store));
    try std.testing.expectEqual(@as(u32, 1), closes);
}

test "close leaves the store open when the checkpoint fails" {
    resetMedium();
    var store = mountedStore();
    dir_fails = true;
    try std.testing.expectEqual(abi.err_hw_init_failed, ra8_cache_store_close(&store));
    try std.testing.expect(store.inited);
    try std.testing.expectEqual(@as(u32, 0), closes);
}

test "close guards a null store" {
    resetMedium();
    try std.testing.expectEqual(abi.err_null_ptr, ra8_cache_store_close(null));
}

test "two entries land in disjoint runs and both read back" {
    resetMedium();
    var store = mountedStore();
    const first = payload(700, 11);
    const second = payload(300, 23);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &first, first.len));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_b, &second, second.len));
    try std.testing.expectEqual(@as(u32, 5), store.live_sectors);

    var reader_a = Reader{};
    var reader_b = Reader{};
    try std.testing.expectEqual(abi.ok, ra8_cache_store_get(&store, key_a, &reader_a));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_get(&store, key_b, &reader_b));
    try std.testing.expect(reader_a.data_start != reader_b.data_start);

    var out_a: [700]u8 = @splat(0);
    var out_b: [300]u8 = @splat(0);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_read(&reader_a, 0, &out_a, out_a.len));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_read(&reader_b, 0, &out_b, out_b.len));
    try std.testing.expectEqualSlices(u8, &first, &out_a);
    try std.testing.expectEqualSlices(u8, &second, &out_b);
}

test "an evicted run is reused by the next put" {
    resetMedium();
    var store = mountedStore();
    const data = payload(300, 31);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    var reader = Reader{};
    try std.testing.expectEqual(abi.ok, ra8_cache_store_get(&store, key_a, &reader));
    const first_start = reader.data_start;

    try std.testing.expectEqual(abi.ok, ra8_cache_store_evict(&store, key_a));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_b, &data, data.len));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_get(&store, key_b, &reader));
    try std.testing.expectEqual(first_start, reader.data_start);
}

test "the append sequence keeps climbing across entries" {
    resetMedium();
    var store = mountedStore();
    const data = payload(16, 1);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_b, &data, data.len));

    const first: *const abi.EntryHeader = @ptrCast(@alignCast(&medium[2]));
    const second: *const abi.EntryHeader = @ptrCast(@alignCast(&medium[4]));
    try std.testing.expectEqual(@as(u32, 1), first.seq);
    try std.testing.expectEqual(@as(u32, 2), second.seq);
    try std.testing.expectEqual(@as(u32, 3), store.next_seq);
}

test "the payload tail sector is zero padded" {
    resetMedium();
    var store = mountedStore();
    const data = payload(10, 42);
    try std.testing.expectEqual(abi.ok, ra8_cache_store_put(&store, key_a, &data, data.len));
    try std.testing.expectEqualSlices(u8, &data, medium[3][0..10]);
    for (medium[3][10..]) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
}
