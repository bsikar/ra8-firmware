//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one hash + pin + evict cache engine of `inc/ra8_keycache.h`. A keycache
//! maps a fixed-size opaque key to a fixed-size opaque cell:
//! a hit returns a pinned view, a miss evicts an unpinned victim, fills the
//! cell through the caller's render callback, inserts it and pins it.
//!
//! This file is the engine's surface and its five entry points. The parts it
//! is built from live beside it, one purpose each: `keycache_list.zig` owns the
//! recency links, `keycache_policy.zig` owns LRU vs SLRU and victim choice,
//! `keycache_index.zig` owns hashing and the bucket chains.
//!
//! Zero allocation (NASA P10 Rule 3): every array is caller-owned and arrives
//! through the config. No `extern` lives here, so a facade importing this file
//! is still a valid host test root.

const std = @import("std");

const index = @import("keycache_index.zig");
const list = @import("keycache_list.zig");
const policy = @import("keycache_policy.zig");
const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// `ra8_keycache_cell_t`, `ra8_keycache_evict_t` and the segment tag are the
/// caller's to see: sizing the metadata array needs `Cell`, and selecting a
/// policy needs `Evict`.
pub const Cell = list.Cell;
pub const Evict = policy.Evict;
pub const Seg = policy.Seg;

/// `ra8_keycache_hash_fn`. Null in the config selects the built-in FNV-1a.
pub const HashFn = index.HashFn;

/// `ra8_keycache_render_fn`, the render-on-miss seam.
pub const RenderFn = *const fn (
    ctx: ?*anyopaque,
    key: ?*const anyopaque,
    cell: ?[*]u8,
    cell_bytes: u32,
    user: ?*anyopaque,
) callconv(.c) u16;

/// `ra8_keycache_cfg_t`: caller-owned storage, policy and renderer.
pub const Cfg = extern struct {
    cell_mem: ?[*]u8,
    cell_bytes: u32,
    cell_count: u32,
    key_mem: ?[*]u8,
    key_bytes: u32,
    user_mem: ?[*]u8,
    user_bytes: u32,
    meta: ?[*]Cell,
    buckets: ?[*]i32,
    bucket_count: u32,
    render: ?RenderFn,
    render_ctx: ?*anyopaque,
    evict: Evict,
    protected_pct: u8,
    hash: ?HashFn,
    hash_ctx: ?*anyopaque,
};

/// `ra8_keycache_t`. A facade embeds this as its first member, so its size is
/// what decides where the facade's own fields land.
///
/// The six recency words C spells loose on this struct (`pb_head`, `pb_tail`,
/// `pt_head`, `pt_tail`, `protected_count`, `protected_cap`) are grouped into
/// `sets`; the bytes are identical and the asserts below pin that down.
pub const State = extern struct {
    cfg: Cfg,
    sets: policy.Sets,
    hits: u32,
    misses: u32,
    evictions: u32,

    /// The cell metadata as a slice. Only valid once `init` has bound `cfg`.
    fn meta(self: anytype) []Cell {
        return self.cfg.meta.?[0..self.cfg.cell_count];
    }

    /// Cell `idx`'s payload.
    fn cellPtr(self: *State, idx: u32) [*]u8 {
        return self.cfg.cell_mem.? + idx * self.cfg.cell_bytes;
    }

    /// Cell `idx`'s user descriptor, or null when the cache carries none.
    fn userPtr(self: *State, idx: u32) ?*anyopaque {
        if (self.cfg.user_bytes == 0) return null;
        return @ptrCast(self.cfg.user_mem.? + idx * self.cfg.user_bytes);
    }

    /// The key-to-cell map over this cache's storage.
    fn table(self: *State) index.Table {
        return .{
            .buckets = self.cfg.buckets.?[0..self.cfg.bucket_count],
            .meta = self.meta(),
            .keys = self.cfg.key_mem.?[0 .. self.cfg.cell_count * self.cfg.key_bytes],
            .key_bytes = self.cfg.key_bytes,
            .hash = self.cfg.hash,
            .hash_ctx = self.cfg.hash_ctx,
        };
    }
};

/// `ra8_keycache_view_t`: a pinned view of a cell, valid until the matching
/// `put`.
pub const View = extern struct {
    data: ?[*]u8,
    user: ?*anyopaque,
};

/// Every pointer the config must carry, checked before anything is written.
fn checkPtrs(cfg: *const Cfg) Err {
    if (cfg.cell_mem == null) return .null_ptr;
    if (cfg.key_mem == null) return .null_ptr;
    if (cfg.meta == null) return .null_ptr;
    if (cfg.buckets == null) return .null_ptr;
    if (cfg.render == null) return .null_ptr;
    if (cfg.user_bytes != 0 and cfg.user_mem == null) return .null_ptr;
    return .ok;
}

/// Every size that must be non-zero for the storage arithmetic to mean
/// anything.
fn checkSizes(cfg: *const Cfg) Err {
    if (cfg.cell_count == 0) return .invalid_size;
    if (cfg.cell_bytes == 0) return .invalid_size;
    if (cfg.key_bytes == 0) return .invalid_size;
    if (cfg.bucket_count == 0) return .invalid_size;
    return .ok;
}

/// Initialise a cache over caller-supplied storage.
///
/// On any error the state is left untouched, so a rejected config cannot leave
/// a half-bound cache behind.
pub fn init(self: *State, cfg: *const Cfg) Err {
    const perr = checkPtrs(cfg);
    if (perr != .ok) return perr;
    const serr = checkSizes(cfg);
    if (serr != .ok) return serr;
    if (cfg.evict == .slru and cfg.protected_pct > policy.split.full_pct) return .invalid_arg;

    self.* = .{ .cfg = cfg.*, .sets = .{}, .hits = 0, .misses = 0, .evictions = 0 };
    self.sets.seed(self.meta());
    self.table().clear();
    self.sets.protected_cap = if (cfg.evict == .slru)
        policy.protectedCap(cfg.cell_count, cfg.protected_pct)
    else
        0;
    return .ok;
}

/// Take one pin on cell `f`, refusing rather than wrapping at the ceiling.
///
/// C incremented a `uint16_t` unchecked: the 65536th outstanding pin wrapped
/// the count to zero and handed back a view whose cell was immediately
/// evictable. `no_mem` is already this call's "cannot pin a cell" answer.
fn pin(cell: *Cell) Err {
    if (cell.pin_count == std.math.maxInt(u16)) return .no_mem;
    cell.pin_count += 1;
    return .ok;
}

/// Fill a victim cell with `key` and hand back a pinned view of it.
fn miss(self: *State, key: []const u8, out_view: *View) Err {
    const meta = self.meta();
    const v = self.sets.pickVictim(meta) orelse return .no_mem;

    if (meta[v].valid != 0) {
        self.table().remove(v);
        self.evictions += 1;
    }
    self.sets.detach(meta, v);

    const cell = self.cellPtr(v);
    const user = self.userPtr(v);
    const rerr = Err.from(self.cfg.render.?(self.cfg.render_ctx, key.ptr, cell, self.cfg.cell_bytes, user));
    if (rerr != .ok) {
        // The victim stays cold rather than holding a half-rendered entry.
        meta[v].valid = 0;
        meta[v].seg = @intFromEnum(Seg.probation);
        self.sets.pb.pushHead(meta, v);
        return rerr;
    }

    @memcpy(self.cfg.key_mem.?[v * self.cfg.key_bytes ..][0..self.cfg.key_bytes], key);
    meta[v].valid = 1;
    meta[v].pin_count = 1;
    meta[v].seg = @intFromEnum(Seg.probation);
    self.table().insert(v);
    self.sets.pb.pushHead(meta, v);
    out_view.* = .{ .data = cell, .user = user };
    return .ok;
}

/// Get (and pin) the cell for `key`, rendering it on a miss.
pub fn get(self: *State, key: []const u8, out_view: *View) Err {
    if (self.table().lookup(key)) |f| {
        const perr = pin(&self.meta()[f]);
        if (perr != .ok) return perr;
        self.hits += 1;
        self.sets.access(self.meta(), self.cfg.evict, f);
        out_view.* = .{ .data = self.cellPtr(f), .user = self.userPtr(f) };
        return .ok;
    }
    self.misses += 1;
    return miss(self, key, out_view);
}

/// Warm the cell for `key` into the cache without holding a pin.
pub fn prefetch(self: *State, key: []const u8) Err {
    var view: View = .{ .data = null, .user = null };
    const gerr = get(self, key, &view);
    if (gerr != .ok) return gerr;
    // Drop the pin now: the cell is resident but evictable, so a wrong
    // read-ahead guess ages out before hot data is displaced.
    return put(self, view.data.?);
}

/// Which cell `data` is the payload of, or null when it is not one of ours.
fn cellIndexOf(self: *State, data: [*]const u8) ?u32 {
    const base = @intFromPtr(self.cfg.cell_mem.?);
    const addr = @intFromPtr(data);
    if (addr < base) return null;
    const off = addr - base;
    if (off >= @as(usize, self.cfg.cell_count) * self.cfg.cell_bytes) return null;
    if (off % self.cfg.cell_bytes != 0) return null;
    return @intCast(off / self.cfg.cell_bytes);
}

/// Release one pin taken by `get`.
pub fn put(self: *State, data: [*]const u8) Err {
    const idx = cellIndexOf(self, data) orelse return .invalid_arg;
    const cell = &self.meta()[idx];
    if (cell.pin_count == 0) return .invalid_arg;
    cell.pin_count -= 1;
    return .ok;
}

/// Report the hit / miss / eviction counters. Pure read.
pub fn stats(
    self: *const State,
    out_hits: ?*u32,
    out_misses: ?*u32,
    out_evictions: ?*u32,
) Err {
    if (self.cfg.cell_mem == null) return .invalid_state;
    if (out_hits) |h| h.* = self.hits;
    if (out_misses) |m| m.* = self.misses;
    if (out_evictions) |e| e.* = self.evictions;
    return .ok;
}

comptime {
    const ptr = @sizeOf(usize);

    // `ra8_keycache_cfg_t` is 9 pointers, 5 uint32_t and 2 uint8_t in the
    // header's order. The offsets are spelled out per pointer width rather
    // than derived, because a derivation would happily agree with a reordered
    // header; these numbers only hold for the order C declares.
    std.debug.assert(ptr == 8 or ptr == 4);
    std.debug.assert(@offsetOf(Cfg, "cell_mem") == 0);
    std.debug.assert(@offsetOf(Cfg, "cell_bytes") == ptr);
    std.debug.assert(@offsetOf(Cfg, "cell_count") == ptr + @sizeOf(u32));
    std.debug.assert(@offsetOf(Cfg, "render") == if (ptr == 8) 72 else 40);
    std.debug.assert(@offsetOf(Cfg, "render_ctx") == if (ptr == 8) 80 else 44);
    std.debug.assert(@offsetOf(Cfg, "hash") == if (ptr == 8) 96 else 52);
    std.debug.assert(@sizeOf(Cfg) == if (ptr == 8) 112 else 60);

    // The grouped `sets` has to land exactly where C's four list words and two
    // protected counters did, and the facade's own fields sit after all of it.
    std.debug.assert(@offsetOf(State, "cfg") == 0);
    std.debug.assert(@offsetOf(State, "sets") == @sizeOf(Cfg));
    std.debug.assert(@offsetOf(State, "hits") == @sizeOf(Cfg) + 24);
    // Nine u32 of payload follow `cfg` (six recency words in `sets`, then the
    // three counters). On LP64 the struct is pointer-aligned, so it carries one
    // word of tail padding and measures ten u32 past `cfg`; on 32-bit ARM the
    // alignment is already 4 and there is no tail padding. Spelling both out
    // rather than the host's number, which is what hid this until ra8_mem was
    // first cross-compiled for cortex-m85.
    std.debug.assert(@sizeOf(State) == @sizeOf(Cfg) + (if (ptr == 8) 10 else 9) * @sizeOf(u32));

    std.debug.assert(@sizeOf(View) == 2 * ptr);
}
