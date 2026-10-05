//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_blockdev_cache.h (RA8FW-728): an LRU write-through
//! sector cache wrapped around another block device. Reads fill a victim
//! slot on a miss; writes go to the backend first, then refresh the cache.
//! Replaces ra8_io_blockdev_cache.c, which is deleted.

const sdhi = @import("ra8_io_blockdev_sdhi_abi.zig");

const tag = "ra8_io_blockdev_cache";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok = sdhi.ok;
pub const err_null_ptr = sdhi.err_null_ptr;
pub const err_invalid_size: c_int = 0x105;

pub const Caps = sdhi.Caps;
pub const Iface = sdhi.Iface;
pub const Device = sdhi.Device;
const block_bytes: usize = sdhi.block_bytes;

/// Mirror of ra8_io_blockdev_cache_slot_t.
pub const Slot = extern struct {
    lba: u32,
    last_use: u32,
    valid: bool,
};

/// Mirror of ra8_io_blockdev_cache_state_t.
pub const State = extern struct {
    under: ?*const Device,
    data: ?[*]u8,
    slots: ?[*]Slot,
    n_slots: u32,
    clock: u32,
    hits: u32,
    misses: u32,
};

comptime {
    if (@sizeOf(Slot) != 12) @compileError("ra8_io_blockdev_cache_slot_t is 12 bytes");
    if (@offsetOf(State, "n_slots") != 3 * @sizeOf(usize)) @compileError("cache state layout drifted");
}

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;
extern fn ra8_io_blockdev_read(bd: ?*const Device, lba: u32, count: u32, buf: ?[*]u8) c_int;
extern fn ra8_io_blockdev_write(bd: ?*const Device, lba: u32, count: u32, buf: ?[*]const u8) c_int;
extern fn ra8_io_blockdev_erase(bd: ?*const Device, lba: u32, count: u32) c_int;
extern fn ra8_io_blockdev_get_caps(bd: ?*const Device, out: ?*Caps) c_int;
extern fn ra8_io_blockdev_sync(bd: ?*const Device) c_int;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

/// RA8_RETURN_ON_ERROR's logging: the message, then "Error" with the code.
fn logged(rc: c_int, message: [*:0]const u8) c_int {
    if (rc != ok) {
        ra8_log_emit_error(tag, message);
        ra8_log_emit_error_val(tag, "Error", @bitCast(rc));
    }
    return rc;
}

fn slotsOf(st: *const State) []Slot {
    return st.slots.?[0..st.n_slots];
}

fn sector(st: *const State, idx: u32) []u8 {
    const at = @as(usize, idx) * block_bytes;
    return st.data.?[at .. at + block_bytes];
}

/// Index of the valid slot caching `lba`, or n_slots on a miss.
pub fn find(st: *const State, lba: u32) u32 {
    for (slotsOf(st), 0..) |s, i| {
        if (s.valid and s.lba == lba) return @intCast(i);
    }
    return st.n_slots;
}

/// The first free slot, else the least recently used one.
pub fn pickVictim(st: *const State) u32 {
    const slots = slotsOf(st);
    var best: u32 = 0;
    for (slots, 0..) |s, i| {
        if (!s.valid) return @intCast(i);
        if (s.last_use < slots[best].last_use) best = @intCast(i);
    }
    return best;
}

fn claim(st: *State, idx: u32, lba: u32) void {
    st.slots.?[idx] = .{ .lba = lba, .valid = true, .last_use = st.clock };
}

fn readBlock(st: *State, lba: u32, dst: []u8) c_int {
    st.clock +%= 1;
    const hit = find(st, lba);
    if (hit != st.n_slots) {
        st.hits +%= 1;
        @memcpy(dst, sector(st, hit));
        st.slots.?[hit].last_use = st.clock;
        return ok;
    }
    st.misses +%= 1;
    const v = pickVictim(st);
    const rc = ra8_io_blockdev_read(st.under, lba, 1, sector(st, v).ptr);
    if (logged(rc, "backend read") != ok) return rc;
    claim(st, v, lba);
    @memcpy(dst, sector(st, v));
    return ok;
}

fn writeBlock(st: *State, lba: u32, src: []const u8) c_int {
    const rc = ra8_io_blockdev_write(st.under, lba, 1, src.ptr);
    if (logged(rc, "backend write") != ok) return rc;
    st.clock +%= 1;
    var idx = find(st, lba);
    if (idx == st.n_slots) idx = pickVictim(st);
    @memcpy(sector(st, idx), src);
    claim(st, idx, lba);
    return ok;
}

fn cacheRead(ctx: ?*anyopaque, lba: u32, count: u32, buf: ?[*]u8) callconv(.c) c_int {
    const st: *State = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx must not be nullptr")));
    const out = buf orelse return nullPtr("buf must not be nullptr");
    var i: u32 = 0;
    while (i < count) : (i += 1) {
        const at = @as(usize, i) * block_bytes;
        const rc = readBlock(st, lba +% i, out[at .. at + block_bytes]);
        if (logged(rc, "read block") != ok) return rc;
    }
    return ok;
}

fn cacheWrite(ctx: ?*anyopaque, lba: u32, count: u32, buf: ?[*]const u8) callconv(.c) c_int {
    const st: *State = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx must not be nullptr")));
    const in = buf orelse return nullPtr("buf must not be nullptr");
    var i: u32 = 0;
    while (i < count) : (i += 1) {
        const at = @as(usize, i) * block_bytes;
        const rc = writeBlock(st, lba +% i, in[at .. at + block_bytes]);
        if (logged(rc, "write block") != ok) return rc;
    }
    return ok;
}

fn cacheErase(ctx: ?*anyopaque, lba: u32, count: u32) callconv(.c) c_int {
    const st: *State = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx must not be nullptr")));
    const rc = ra8_io_blockdev_erase(st.under, lba, count);
    if (logged(rc, "backend erase") != ok) return rc;
    const end = lba +% count;
    for (slotsOf(st)) |*s| {
        if (s.valid and s.lba >= lba and s.lba < end) s.valid = false;
    }
    return ok;
}

fn cacheGetCaps(ctx: ?*const anyopaque, out: ?*Caps) callconv(.c) c_int {
    const st: *const State = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx must not be nullptr")));
    if (out == null) return nullPtr("out must not be nullptr");
    return ra8_io_blockdev_get_caps(st.under, out);
}

fn cacheSync(ctx: ?*anyopaque) callconv(.c) c_int {
    const st: *const State = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx must not be nullptr")));
    return ra8_io_blockdev_sync(st.under);
}

const cache_iface = Iface{
    .read = cacheRead,
    .write = cacheWrite,
    .erase = cacheErase,
    .get_caps = cacheGetCaps,
    .sync = cacheSync,
};

pub export fn ra8_io_blockdev_cache_init(
    bd: ?*Device,
    state: ?*State,
    under: ?*const Device,
    data: ?[*]u8,
    slots: ?[*]Slot,
    n_slots: u32,
) c_int {
    const dev = bd orelse return nullPtr("bd must not be nullptr");
    const st = state orelse return nullPtr("state must not be nullptr");
    if (under == null) return nullPtr("under must not be nullptr");
    if (data == null) return nullPtr("data must not be nullptr");
    const sl = slots orelse return nullPtr("slots must not be nullptr");
    if (n_slots == 0) return err_invalid_size;
    st.* = .{ .under = under, .data = data, .slots = sl, .n_slots = n_slots, .clock = 0, .hits = 0, .misses = 0 };
    for (sl[0..n_slots]) |*s| s.* = .{ .lba = 0, .last_use = 0, .valid = false };
    dev.iface = &cache_iface;
    dev.ctx = st;
    return ok;
}

pub export fn ra8_io_blockdev_cache_stats(state: ?*const State, out_hits: ?*u32, out_misses: ?*u32) c_int {
    const st = state orelse return nullPtr("state must not be nullptr");
    if (out_hits) |h| h.* = st.hits;
    if (out_misses) |m| m.* = st.misses;
    return ok;
}
