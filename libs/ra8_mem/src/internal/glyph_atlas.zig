//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The glyph cache of `inc/ra8_glyph_atlas.h` (#147): a typed facade over
//! `ra8_keycache`, where the glyph key is the cache key, the glyph bitmap is
//! the cell payload, and the rendered width and height ride in the per-cell
//! descriptor. Every cache mechanic (the LRU list, the pinned-cell skip, hash
//! chaining, eviction) stays in the engine.
//!
//! The engine arrives as a comptime parameter rather than being called
//! directly, so this file holds no `extern` and is a valid host test root: the
//! fake can return a descriptor the real engine would never produce, and can
//! fail a render the real rasteriser will not fail on demand.

const std = @import("std");

/// The engine's types are part of this facade's surface: a caller sizing the
/// storage needs `keycache.Cell`, and every call reports a `vocab.Err`.
pub const keycache = @import("keycache.zig");
pub const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// `ra8_glyph_key_t`: what identifies one rendered glyph. Compared byte-wise by
/// the engine, so the layout is padding-free and `reserved` must stay zero.
pub const Key = extern struct {
    glyph_id: u32,
    face_id: u16,
    size_px: u16,
    mode: u16,
    reserved: u16,
};

/// `ra8_glyph_dims_t`: the per-cell descriptor the render seam writes.
pub const Dims = extern struct {
    w: u16,
    h: u16,
};

/// `ra8_glyph_t`: a pinned view of a cached bitmap.
pub const Glyph = extern struct {
    bitmap: ?[*]const u8,
    width: u16,
    height: u16,
};

/// `ra8_glyph_render_fn`: rasterise one glyph into a cell.
pub const RenderFn = *const fn (
    ctx: ?*anyopaque,
    key: ?*const Key,
    cell: ?[*]u8,
    cell_bytes: u32,
    out_w: ?*u16,
    out_h: ?*u16,
) callconv(.c) u16;

/// `ra8_glyph_atlas_cfg_t`: caller-owned storage plus the renderer.
pub const Cfg = extern struct {
    cell_mem: ?[*]u8,
    cell_bytes: u32,
    cell_count: u32,
    meta: ?[*]keycache.Cell,
    keys: ?[*]Key,
    dims: ?[*]Dims,
    buckets: ?[*]i32,
    bucket_count: u32,
    render: ?RenderFn,
    render_ctx: ?*anyopaque,
};

/// `ra8_glyph_atlas_t` over one engine seam. `KC` supplies `init`, `get`, `put`
/// and `stats` over a `keycache.State`.
///
/// A bound atlas is pinned: `init` hands the engine a pointer back to the atlas
/// so the render trampoline can reach the caller's renderer, so copying or
/// moving one after init leaves the engine pointing at the original. The C
/// facade behaves the same way and never said so.
pub fn Atlas(comptime KC: type) type {
    return extern struct {
        const Self = @This();

        kc: keycache.State,
        render: ?RenderFn,
        render_ctx: ?*anyopaque,

        /// The engine's render-on-miss seam, adapted to the public renderer.
        /// The descriptor is written only once the renderer has succeeded, so a
        /// failed render cannot leave dimensions behind for a later hit to read.
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
            const code = self.render.?(
                self.render_ctx,
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
            if (cfg.render == null) return .null_ptr;
            self.* = std.mem.zeroes(Self);
            self.render = cfg.render;
            self.render_ctx = cfg.render_ctx;
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
            return KC.init(&self.kc, &engine);
        }

        pub fn get(self: *Self, key: *const Key) union(enum) { glyph: Glyph, failed: Err } {
            var view = std.mem.zeroes(keycache.View);
            const err = KC.get(&self.kc, key, &view);
            if (err != .ok) return .{ .failed = err };
            const dims: *const Dims = @ptrCast(@alignCast(view.user.?));
            return .{ .glyph = .{ .bitmap = view.data, .width = dims.w, .height = dims.h } };
        }

        pub fn put(self: *Self, bitmap: [*]const u8) Err {
            return KC.put(&self.kc, bitmap);
        }

        /// The C guarded an uninitialised atlas here rather than in the engine,
        /// because a zeroed `ra8_glyph_atlas_t` is indistinguishable from a
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
    // The key is hashed and compared byte-wise over its whole width, so padding
    // would feed indeterminate bytes into the comparison. The C header asserts
    // the same thing.
    std.debug.assert(@sizeOf(Key) == 12);
    std.debug.assert(@offsetOf(Key, "reserved") == 10);
    std.debug.assert(@sizeOf(Dims) == 4);
}
