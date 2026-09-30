//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The image-tile cache of `inc/ra8_tile_cache.h` (#147): the third typed
//! facade over `ra8_keycache`, and the sibling of the glyph atlas at a
//! different scale. The tile key is the cache key, the decoded pixels are the
//! cell payload, and the decoded width and height ride in the per-cell
//! descriptor. Every cache mechanic (the LRU list, the pinned-cell skip, hash
//! chaining, eviction) stays in the engine; the tile grid arithmetic lives in
//! `tile_geometry.zig`. What this file adds is the decode trampoline and the
//! pan sweep that walks a lead edge through the cache.
//!
//! The engine arrives as a comptime parameter rather than being called
//! directly, so this file holds no `extern` and is a valid host test root: the
//! fake can refuse a warm the real cache would accept, which is what the
//! best-effort sweep turns on.

const std = @import("std");

pub const geometry = @import("tile_geometry.zig");
pub const keycache = @import("keycache.zig");
pub const vocab = @import("vocab.zig");

pub const Err = vocab.Err;
pub const Rect = geometry.Rect;
pub const PanDir = geometry.PanDir;
pub const PrefetchReq = geometry.PrefetchReq;

/// `ra8_tile_key_t`: what identifies one decoded tile. Compared byte-wise by
/// the engine, so the layout is padding-free and `reserved` must stay zero.
pub const Key = extern struct {
    image_id: u32 = 0,
    tile_x: u16 = 0,
    tile_y: u16 = 0,
    zoom: u16 = 0,
    reserved: u16 = 0,
};

/// `ra8_tile_dims_t`: the per-cell descriptor the decode seam writes.
pub const Dims = extern struct {
    w: u16,
    h: u16,
};

/// `ra8_tile_t`: a pinned view of a cached tile.
pub const Tile = extern struct {
    pixels: ?[*]const u8,
    width: u16,
    height: u16,
};

/// `ra8_tile_decode_fn`: decode one tile region into a cell.
pub const DecodeFn = *const fn (
    ctx: ?*anyopaque,
    key: ?*const Key,
    cell: ?[*]u8,
    cell_bytes: u32,
    out_w: ?*u16,
    out_h: ?*u16,
) callconv(.c) u16;

/// `ra8_tile_cache_cfg_t`: caller-owned storage plus the decoder.
pub const Cfg = extern struct {
    cell_mem: ?[*]u8,
    cell_bytes: u32,
    cell_count: u32,
    meta: ?[*]keycache.Cell,
    keys: ?[*]Key,
    dims: ?[*]Dims,
    buckets: ?[*]i32,
    bucket_count: u32,
    decode: ?DecodeFn,
    decode_ctx: ?*anyopaque,
};

/// `ra8_tile_cache_t` over one engine seam. `KC` supplies `init`, `get`,
/// `put`, `prefetch` and `stats` over a `keycache.State`.
///
/// A bound cache is pinned: `init` hands the engine a pointer back to the
/// cache so the decode trampoline can reach the caller's decoder, so copying
/// or moving one after init leaves the engine pointing at the original.
pub fn Cache(comptime KC: type) type {
    return extern struct {
        const Self = @This();

        kc: keycache.State,
        decode: ?DecodeFn,
        decode_ctx: ?*anyopaque,

        /// The engine's render-on-miss seam, adapted to the public decoder.
        /// The descriptor is written only once the decoder has succeeded, so
        /// a failed decode cannot leave dimensions behind for a later hit.
        fn trampoline(
            ctx: ?*anyopaque,
            key: ?*const anyopaque,
            cell: ?[*]u8,
            cell_bytes: u32,
            user: ?*anyopaque,
        ) callconv(.c) u16 {
            const self: *const Self = @ptrCast(@alignCast(ctx.?));
            var w: u16 = 0;
            var h: u16 = 0;
            const code = self.decode.?(
                self.decode_ctx,
                @ptrCast(@alignCast(key.?)),
                cell,
                cell_bytes,
                &w,
                &h,
            );
            if (code != Err.ok.code()) return code;
            const dims: *Dims = @ptrCast(@alignCast(user.?));
            dims.* = .{ .w = w, .h = h };
            return Err.ok.code();
        }

        pub fn init(self: *Self, cfg: *const Cfg) Err {
            if (cfg.decode == null) return .null_ptr;
            self.* = std.mem.zeroes(Self);
            self.decode = cfg.decode;
            self.decode_ctx = cfg.decode_ctx;
            var engine = std.mem.zeroes(keycache.Cfg);
            engine.cell_mem = cfg.cell_mem;
            engine.cell_bytes = cfg.cell_bytes;
            engine.cell_count = cfg.cell_count;
            engine.key_mem = @ptrCast(cfg.keys);
            engine.key_bytes = @sizeOf(Key);
            engine.user_mem = @ptrCast(cfg.dims);
            engine.user_bytes = @sizeOf(Dims);
            engine.meta = cfg.meta;
            engine.buckets = cfg.buckets;
            engine.bucket_count = cfg.bucket_count;
            engine.render = &trampoline;
            engine.render_ctx = self;
            const err = KC.init(&self.kc, &engine);
            // Leave a rejected cache unbound rather than half-bound, so
            // `capacity` and `stats` answer for a cache that never opened.
            if (err != .ok) self.* = std.mem.zeroes(Self);
            return err;
        }

        pub fn get(self: *Self, key: *const Key) union(enum) { tile: Tile, failed: Err } {
            var view = std.mem.zeroes(keycache.View);
            const err = KC.get(&self.kc, key, &view);
            if (err != .ok) return .{ .failed = err };
            const dims: *const Dims = @ptrCast(@alignCast(view.user.?));
            return .{ .tile = .{ .pixels = view.data, .width = dims.w, .height = dims.h } };
        }

        pub fn put(self: *Self, pixels: [*]const u8) Err {
            return KC.put(&self.kc, pixels);
        }

        /// The configured cell count, or none when the cache was never bound.
        pub fn capacity(self: *const Self) ?u32 {
            if (self.kc.cfg.cell_mem == null) return null;
            return self.kc.cfg.cell_count;
        }

        /// Warm one tile and drop the pin at once: resident but evictable, so
        /// a wrong read-ahead guess ages out before hot tiles.
        pub fn prefetch(self: *Self, key: *const Key) Err {
            return KC.prefetch(&self.kc, key);
        }

        /// Warm the row or column one step ahead of a panning viewport, and
        /// report how many tiles that took. Best-effort: the first refusal
        /// ends the sweep without failing the pan.
        pub fn prefetchPan(
            self: *Self,
            req: *const PrefetchReq,
        ) union(enum) { warmed: u16, failed: Err } {
            if (!geometry.viewIsSane(req)) return .{ .failed = .invalid_arg };
            const line = geometry.panLine(req) orelse return .{ .warmed = 0 };
            const cap = @min(line.count, req.max_tiles);
            var warmed: u16 = 0;
            while (warmed < cap) : (warmed += 1) {
                const tile = line.tileAt(warmed);
                const key: Key = .{
                    .image_id = req.image_id,
                    .tile_x = tile.x,
                    .tile_y = tile.y,
                    .zoom = req.zoom,
                };
                if (self.prefetch(&key) != .ok) break;
            }
            return .{ .warmed = warmed };
        }

        /// The C guarded an uninitialised cache here rather than in the
        /// engine: a zeroed `ra8_tile_cache_t` is indistinguishable from a
        /// bound one until you look at the storage it was given.
        pub fn stats(
            self: *const Self,
            out_hits: ?*u32,
            out_misses: ?*u32,
            out_evictions: ?*u32,
        ) Err {
            if (self.kc.cfg.cell_mem == null) return .invalid_state;
            return KC.stats(&self.kc, out_hits, out_misses, out_evictions);
        }
    };
}

comptime {
    // The key is hashed and compared byte-wise over its whole width, so
    // padding would feed indeterminate bytes into the comparison. The C header
    // asserts the same thing.
    std.debug.assert(@sizeOf(Key) == 12);
    std.debug.assert(@offsetOf(Key, "reserved") == 10);
    std.debug.assert(@sizeOf(Dims) == 4);
    std.debug.assert(@offsetOf(Tile, "width") == @sizeOf(usize));
}
