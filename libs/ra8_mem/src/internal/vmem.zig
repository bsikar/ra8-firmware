//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The byte-range page cache of `inc/ra8_vmem.h`: a typed facade over
//! `ra8_keycache` with an (object id, frame-aligned offset) key, the byte page
//! as the cell payload, and the SLRU policy selected. Every cache mechanic (the
//! probationary/protected lists, the pinned-frame skip, hash chaining) stays in
//! the engine. What this file adds is the frame-boundary alignment, the
//! page-oriented hash, the pointer-handle API and the read-ahead helper.
//!
//! The engine arrives as a comptime parameter rather than being called
//! directly, so this file holds no `extern` and is a valid host test root: the
//! fake can fail an init the real engine accepts, and can drive the fill
//! trampoline and the injected hash without a real cache behind them.

const std = @import("std");

pub const keycache = @import("keycache.zig");
pub const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// `ra8_vmem_loader_fn`: fill one frame with a page of an object.
pub const LoaderFn = *const fn (
    ctx: ?*anyopaque,
    object_id: u32,
    offset: u64,
    frame: ?[*]u8,
    frame_bytes: u32,
) callconv(.c) u16;

/// `ra8_vmem_key_t`, the page key the engine hashes and compares byte-wise.
/// The C struct carries four bytes of implicit padding after `object_id`.
/// Zig leaves padding undefined, so a key built field by field would carry
/// stack garbage into the comparison and a resident page could miss. Naming
/// the gap `reserved` makes every key literal zero it, which is the
/// zero-filled key the header promises.
pub const Key = extern struct {
    object_id: u32 = 0,
    reserved: u32 = 0,
    offset: u64 = 0,
};

/// `ra8_vmem_cfg_t`, field for field.
pub const Cfg = extern struct {
    frame_mem: ?[*]u8 = null,
    frame_bytes: u32 = 0,
    frame_count: u32 = 0,
    meta: ?[*]keycache.Cell = null,
    keys: ?[*]Key = null,
    buckets: ?[*]i32 = null,
    bucket_count: u32 = 0,
    loader: ?LoaderFn = null,
    loader_ctx: ?*anyopaque = null,
    protected_pct: u8 = 0,
};

/// `ra8_vmem_t`. Caller-allocated on the C side, so the engine state between
/// `cfg` and `protected_cap` has to be exactly the width C gives it.
pub const State = extern struct {
    cfg: Cfg = .{},
    kc: keycache.State = std.mem.zeroes(keycache.State),
    protected_cap: u32 = 0,
};

/// Mixing constants for the page key. Grouped rather than loose `k_` values.
pub const hash_mul = struct {
    /// Knuth multiplicative hash, applied to the object id.
    pub const object: u32 = 2654435761;
    /// Odd multiplier folding the two halves of the offset.
    pub const page: u32 = 40503;
};

/// Fold the (object id, offset) page key, division-free. The offset is always
/// frame-aligned, so its sub-frame bits are zero and hashing it is hashing the
/// page number without a runtime divide.
pub fn hashKey(key: *const Key) u32 {
    const lo: u32 = @truncate(key.offset);
    const hi: u32 = @truncate(key.offset >> 32);
    return (key.object_id *% hash_mul.object) ^ ((lo ^ hi) *% hash_mul.page);
}

fn hashTrampoline(key: ?*const anyopaque, key_bytes: u32, ctx: ?*anyopaque) callconv(.c) u32 {
    _ = key_bytes;
    _ = ctx;
    const k: *const Key = @ptrCast(@alignCast(key.?));
    return hashKey(k);
}

/// Render-on-miss: hand the page through to the caller's loader. The page
/// cache carries no per-cell descriptor, so `user` is unused.
fn fillTrampoline(
    ctx: ?*anyopaque,
    key: ?*const anyopaque,
    cell: ?[*]u8,
    cell_bytes: u32,
    user: ?*anyopaque,
) callconv(.c) u16 {
    _ = user;
    const self: *State = @ptrCast(@alignCast(ctx.?));
    const k: *const Key = @ptrCast(@alignCast(key.?));
    const loader = self.cfg.loader orelse return Err.invalid_state.code();
    return loader(self.cfg.loader_ctx, k.object_id, k.offset, cell, cell_bytes);
}

/// The frame-aligned base of `offset`, or null when the cache is unbound.
fn frameBase(self: *const State, offset: u64) ?u64 {
    const frame_bytes = self.cfg.frame_bytes;
    if (frame_bytes == 0) return null;
    return offset - (offset % @as(u64, frame_bytes));
}

/// The facade, over whichever engine seam is injected.
pub fn Vmem(comptime KC: type) type {
    return struct {
        pub fn init(self: *State, cfg: *const Cfg) Err {
            self.* = .{ .cfg = cfg.* };

            var engine = std.mem.zeroes(keycache.Cfg);
            engine.cell_mem = cfg.frame_mem;
            engine.cell_bytes = cfg.frame_bytes;
            engine.cell_count = cfg.frame_count;
            engine.key_mem = @ptrCast(cfg.keys);
            engine.key_bytes = @sizeOf(Key);
            engine.meta = cfg.meta;
            engine.buckets = cfg.buckets;
            engine.bucket_count = cfg.bucket_count;
            engine.render = fillTrampoline;
            engine.render_ctx = self;
            engine.evict = .slru;
            engine.protected_pct = cfg.protected_pct;
            engine.hash = hashTrampoline;

            const err = KC.init(&self.kc, &engine);
            if (err != .ok) {
                // Leave the handle unbound rather than half-bound: a later get
                // then reads a zero frame size and says so, where the C divided
                // by it.
                self.* = .{};
                return err;
            }
            self.protected_cap = self.kc.sets.protected_cap;
            return .ok;
        }

        pub fn get(self: *State, object_id: u32, offset: u64, out_page: *?*anyopaque) Err {
            const base = frameBase(self, offset) orelse return .invalid_state;
            const key: Key = .{ .object_id = object_id, .offset = base };

            var view = std.mem.zeroes(keycache.View);
            const err = KC.get(&self.kc, &key, &view);
            if (err != .ok) return err;

            const data = view.data orelse return .invalid_state;
            out_page.* = @ptrCast(data);
            return .ok;
        }

        pub fn put(self: *State, page: [*]const u8) Err {
            return KC.put(&self.kc, page);
        }

        /// Warm a page and drop the pin at once: resident but evictable, so a
        /// wrong read-ahead guess ages out of probation before hot data.
        pub fn prefetch(self: *State, object_id: u32, offset: u64) Err {
            var page: ?*anyopaque = null;
            const err = get(self, object_id, offset, &page);
            if (err != .ok) return err;
            return put(self, @ptrCast(page.?));
        }

        pub fn stats(
            self: *const State,
            out_hits: ?*u32,
            out_misses: ?*u32,
            out_evictions: ?*u32,
        ) Err {
            if (self.cfg.frame_mem == null) return .invalid_state;
            return KC.stats(&self.kc, out_hits, out_misses, out_evictions);
        }
    };
}

comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(ptr == 8 or ptr == 4);

    // `ra8_vmem_key_t` is a uint32_t then a uint64_t, so the 8-byte member
    // forces the same 16-byte key on both widths. The engine compares keys
    // byte-wise, so a wrong size here would compare padding.
    std.debug.assert(@offsetOf(Key, "reserved") == 4);
    std.debug.assert(@offsetOf(Key, "offset") == 8);
    std.debug.assert(@sizeOf(Key) == 16);

    // Spelled out per pointer width rather than derived: a derivation would
    // agree with a reordered header.
    std.debug.assert(@offsetOf(Cfg, "frame_bytes") == ptr);
    std.debug.assert(@offsetOf(Cfg, "meta") == if (ptr == 8) 16 else 12);
    std.debug.assert(@offsetOf(Cfg, "loader") == if (ptr == 8) 48 else 28);
    std.debug.assert(@sizeOf(Cfg) == if (ptr == 8) 72 else 40);

    // The caller allocates `ra8_vmem_t`, so the engine's width decides where
    // `protected_cap` lands.
    std.debug.assert(@offsetOf(State, "kc") == @sizeOf(Cfg));
    std.debug.assert(@offsetOf(State, "protected_cap") == @sizeOf(Cfg) + @sizeOf(keycache.State));
}
