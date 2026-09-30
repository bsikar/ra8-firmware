//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C membrane for the Zig side of `ra8_mem`: every symbol
//! `inc/ra8_slab.h`, `inc/ra8_vmem.h`, `inc/ra8_vmem_stream.h`,
//! `inc/ra8_glyph_atlas.h`, `inc/ra8_tile_cache.h` and `inc/ra8_vsource.h`
//! declare, and nothing else. Those headers are unchanged,
//! so the host suite, `mem_subsystem`, `reflow`, `glyph_bench`, `cache_bench`,
//! `reader_vmem` and the rest of `libs/ra8_mem` link against this archive
//! without knowing the bodies moved.
//!
//! Raw pointers stop here. Everything past this file works in slices, typed
//! enums and non-optional references.

const std = @import("std");

const arena = @import("internal/arena.zig");
const glyph_atlas = @import("internal/glyph_atlas.zig");
const keycache = @import("internal/keycache.zig");
const slab = @import("internal/slab.zig");
const tile_cache = @import("internal/tile_cache.zig");
const vmem = @import("internal/vmem.zig");
const vmem_stream = @import("internal/vmem_stream.zig");
const vocab = @import("internal/vocab.zig");
const vsource = @import("internal/vsource.zig");

const Err = vocab.Err;

comptime {
    // `ra8_err.h` spells `ra8_err_t` as `enum : uint16_t`, so the return width
    // has to match on every target the archive is built for.
    std.debug.assert(@sizeOf(Err) == 2);
}

// ---------------------------------------------------------------------------
// ra8_arena.h
// ---------------------------------------------------------------------------

export fn ra8_arena_init(handle: ?*arena.Arena, base: ?*anyopaque, size: u32) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const region = base orelse return Err.null_ptr.code();
    return arena.init(self, @ptrCast(region), size).code();
}

export fn ra8_arena_carve(
    handle: ?*arena.Arena,
    bytes: u32,
    alignment: u32,
    out_ptr: ?*?*anyopaque,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const dst = out_ptr orelse return Err.null_ptr.code();
    return arena.carve(self, bytes, alignment, dst).code();
}

export fn ra8_arena_remaining(handle: ?*const arena.Arena, out_remaining: ?*u32) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const dst = out_remaining orelse return Err.null_ptr.code();
    dst.* = arena.remaining(self);
    return Err.ok.code();
}

export fn ra8_arena_carve_all(
    handle: ?*arena.Arena,
    slots: ?[*]const arena.Slot,
    slot_count: u32,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const table = slots orelse return Err.null_ptr.code();
    // The count is bounded inside, so the slice is only formed once the cap
    // has been honoured: a forged count cannot make a slice out of nothing.
    if (slot_count == 0 or slot_count > arena.Limits.slot_cap) return Err.invalid_arg.code();
    return arena.carveAll(self, table[0..slot_count]).code();
}

export fn ra8_arena_carve_remaining(
    handle: ?*arena.Arena,
    alignment: u32,
    out_ptr: ?*?*anyopaque,
    out_bytes: ?*u32,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const dst = out_ptr orelse return Err.null_ptr.code();
    const len = out_bytes orelse return Err.null_ptr.code();
    return arena.carveRemaining(self, alignment, dst, len).code();
}

export fn ra8_arena_high_water(handle: ?*const arena.Arena, out_high_water: ?*u32) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const dst = out_high_water orelse return Err.null_ptr.code();
    dst.* = arena.highWater(self);
    return Err.ok.code();
}

export fn ra8_arena_reset(handle: ?*arena.Arena) u16 {
    const self = handle orelse return Err.null_ptr.code();
    arena.reset(self);
    return Err.ok.code();
}

// ---------------------------------------------------------------------------
// ra8_slab.h
// ---------------------------------------------------------------------------

export fn ra8_slab_init(
    handle: ?*slab.Slab,
    buffer: ?*anyopaque,
    buffer_bytes: u32,
    cell_bytes: u32,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const pool = buffer orelse return Err.null_ptr.code();
    return slab.init(self, @ptrCast(pool), buffer_bytes, cell_bytes).code();
}

export fn ra8_slab_alloc(handle: ?*slab.Slab, out_cell: ?*?*anyopaque) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const dst = out_cell orelse return Err.null_ptr.code();
    return slab.alloc(self, dst).code();
}

export fn ra8_slab_free(handle: ?*slab.Slab, cell: ?*anyopaque) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const target = cell orelse return Err.null_ptr.code();
    return slab.free(self, target).code();
}

export fn ra8_slab_stats(handle: ?*const slab.Slab, out_free: ?*u32, out_total: ?*u32) u16 {
    const self = handle orelse return Err.null_ptr.code();
    return slab.stats(self, out_free, out_total).code();
}

// ---------------------------------------------------------------------------
// ra8_vmem_stream.h
// ---------------------------------------------------------------------------

/// Adapts the page cache into what the stream adapter works in: a frame
/// arrives as a slice of its real length, so the in-frame copy is
/// bounds-checked rather than trusted the way `(const uint8_t*)page +
/// in_frame` was. Both sides are Zig in this archive now, so the calls are
/// direct rather than through the two `ra8_vmem_*` externs the C TU needed.
const Cache = struct {
    pub fn get(vm: ?*vmem_stream.Vmem, object_id: u32, offset: u64, frame_bytes: u32) vmem_stream.Frame {
        const cache = vm orelse return .{ .failed = .null_ptr };
        var page: ?*anyopaque = null;
        const err = Pages.get(cache, object_id, offset, &page);
        if (err != .ok) return .{ .failed = err };
        const frame = page orelse return .{ .failed = .null_ptr };
        return .{ .page = @as([*]const u8, @ptrCast(frame))[0..frame_bytes] };
    }

    pub fn put(vm: ?*vmem_stream.Vmem, page: []const u8) Err {
        const cache = vm orelse return .null_ptr;
        return Pages.put(cache, page.ptr);
    }
};

export fn ra8_vmem_stream_init(
    handle: ?*vmem_stream.Stream,
    vm: ?*vmem_stream.Vmem,
    object_id: u32,
    size: u64,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const cache = vm orelse return Err.null_ptr.code();
    return vmem_stream.init(self, cache, object_id, size).code();
}

export fn ra8_vmem_stream_read_checked(
    handle: ?*vmem_stream.Stream,
    offset: u64,
    buf: ?*anyopaque,
    len: u32,
    out_read: ?*u32,
) u16 {
    const copied = out_read orelse return Err.null_ptr.code();
    copied.* = 0;
    const self = handle orelse return Err.null_ptr.code();
    const dst = buf orelse return Err.null_ptr.code();

    const bytes = @as([*]u8, @ptrCast(dst))[0..len];
    const result = vmem_stream.readChecked(Cache, self, offset, bytes);
    copied.* = result.copied;
    return result.err.code();
}

export fn ra8_vmem_stream_read(
    ctx: ?*anyopaque,
    offset: u64,
    buf: ?*anyopaque,
    len: u32,
    out_read: ?*u32,
) u16 {
    return ra8_vmem_stream_read_checked(@ptrCast(@alignCast(ctx)), offset, buf, len, out_read);
}

// ---------------------------------------------------------------------------
// ra8_glyph_atlas.h
// ---------------------------------------------------------------------------

/// The engine seam the facades are written against. Generic in the key, so
/// the glyph atlas, the page cache and the tile cache share one seam.
///
/// The engine is Zig now (`internal/keycache.zig`), so these are direct calls
/// rather than the five `extern fn` the archive used to leave undefined. Past
/// this seam the engine works in slices: a facade hands over a typed key
/// pointer and the seam widens it to the bytes the engine compares, rather
/// than every caller casting to `*const anyopaque`.
const Engine = struct {
    pub fn init(state: *keycache.State, cfg: *const keycache.Cfg) Err {
        return keycache.init(state, cfg);
    }

    pub fn get(state: *keycache.State, key: anytype, out_view: *keycache.View) Err {
        return keycache.get(state, std.mem.asBytes(key), out_view);
    }

    pub fn put(state: *keycache.State, data: [*]const u8) Err {
        return keycache.put(state, data);
    }

    pub fn prefetch(state: *keycache.State, key: anytype) Err {
        return keycache.prefetch(state, std.mem.asBytes(key));
    }

    pub fn stats(
        state: *const keycache.State,
        out_hits: ?*u32,
        out_misses: ?*u32,
        out_evictions: ?*u32,
    ) Err {
        return keycache.stats(state, out_hits, out_misses, out_evictions);
    }
};

// ---------------------------------------------------------------------------
// ra8_keycache.h
// ---------------------------------------------------------------------------

/// The key blob a C caller handed in, as the engine's `key_bytes` of it.
fn keyBytes(state: *const keycache.State, key: *const anyopaque) []const u8 {
    return @as([*]const u8, @ptrCast(key))[0..state.cfg.key_bytes];
}

export fn ra8_keycache_init(kc: ?*keycache.State, cfg: ?*const keycache.Cfg) u16 {
    const self = kc orelse return Err.null_ptr.code();
    const config = cfg orelse return Err.null_ptr.code();
    return keycache.init(self, config).code();
}

export fn ra8_keycache_get(
    kc: ?*keycache.State,
    key: ?*const anyopaque,
    out_view: ?*keycache.View,
) u16 {
    const self = kc orelse return Err.null_ptr.code();
    const blob = key orelse return Err.null_ptr.code();
    const dst = out_view orelse return Err.null_ptr.code();
    return keycache.get(self, keyBytes(self, blob), dst).code();
}

export fn ra8_keycache_prefetch(kc: ?*keycache.State, key: ?*const anyopaque) u16 {
    const self = kc orelse return Err.null_ptr.code();
    const blob = key orelse return Err.null_ptr.code();
    return keycache.prefetch(self, keyBytes(self, blob)).code();
}

export fn ra8_keycache_put(kc: ?*keycache.State, data: ?[*]const u8) u16 {
    const self = kc orelse return Err.null_ptr.code();
    const cell = data orelse return Err.null_ptr.code();
    return keycache.put(self, cell).code();
}

export fn ra8_keycache_stats(
    kc: ?*const keycache.State,
    out_hits: ?*u32,
    out_misses: ?*u32,
    out_evictions: ?*u32,
) u16 {
    const self = kc orelse return Err.null_ptr.code();
    return keycache.stats(self, out_hits, out_misses, out_evictions).code();
}

// ---------------------------------------------------------------------------
// ra8_glyph_atlas.h (continued)
// ---------------------------------------------------------------------------

const Atlas = glyph_atlas.Atlas(Engine);

// ---------------------------------------------------------------------------
// ra8_vmem.h
// ---------------------------------------------------------------------------

/// The byte-range page cache: the second facade over the same four symbols,
/// SLRU rather than LRU, keyed on (object id, frame-aligned offset).
const Pages = vmem.Vmem(Engine);

comptime {
    // The stream adapter streams over this same handle, so there is one mirror
    // of `ra8_vmem_t` rather than two that could drift.
    std.debug.assert(vmem.State == vmem_stream.Vmem);
}

export fn ra8_vmem_init(handle: ?*vmem.State, cfg: ?*const vmem.Cfg) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const config = cfg orelse return Err.null_ptr.code();
    return Pages.init(self, config).code();
}

export fn ra8_vmem_get(
    handle: ?*vmem.State,
    object_id: u32,
    offset: u64,
    out_page: ?*?*anyopaque,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const dst = out_page orelse return Err.null_ptr.code();
    return Pages.get(self, object_id, offset, dst).code();
}

export fn ra8_vmem_put(handle: ?*vmem.State, page: ?*anyopaque) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const frame = page orelse return Err.null_ptr.code();
    return Pages.put(self, @ptrCast(frame)).code();
}

export fn ra8_vmem_prefetch(handle: ?*vmem.State, object_id: u32, offset: u64) u16 {
    const self = handle orelse return Err.null_ptr.code();
    return Pages.prefetch(self, object_id, offset).code();
}

export fn ra8_vmem_stats(
    handle: ?*const vmem.State,
    out_hits: ?*u32,
    out_misses: ?*u32,
    out_evictions: ?*u32,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    return Pages.stats(self, out_hits, out_misses, out_evictions).code();
}

comptime {
    // `ra8_glyph_atlas_t` is caller-allocated (a `static` in the ereader UI, a
    // stack local in the tests), so the engine state in front of these two
    // fields has to be exactly the width C gives it.
    std.debug.assert(@offsetOf(Atlas, "kc") == 0);
    std.debug.assert(@offsetOf(Atlas, "render") == @sizeOf(keycache.State));
    std.debug.assert(@sizeOf(Atlas) == @sizeOf(keycache.State) + 2 * @sizeOf(usize));
}

export fn ra8_glyph_atlas_init(handle: ?*Atlas, cfg: ?*const glyph_atlas.Cfg) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const config = cfg orelse return Err.null_ptr.code();
    return self.init(config).code();
}

export fn ra8_glyph_atlas_get(
    handle: ?*Atlas,
    key: ?*const glyph_atlas.Key,
    out_glyph: ?*glyph_atlas.Glyph,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const wanted = key orelse return Err.null_ptr.code();
    const dst = out_glyph orelse return Err.null_ptr.code();
    switch (self.get(wanted)) {
        .glyph => |g| {
            dst.* = g;
            return Err.ok.code();
        },
        .failed => |err| return err.code(),
    }
}

export fn ra8_glyph_atlas_put(handle: ?*Atlas, bitmap: ?[*]const u8) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const cell = bitmap orelse return Err.null_ptr.code();
    return self.put(cell).code();
}

export fn ra8_glyph_atlas_stats(
    handle: ?*const Atlas,
    out_hits: ?*u32,
    out_misses: ?*u32,
    out_evictions: ?*u32,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    return self.stats(out_hits, out_misses, out_evictions).code();
}

// ---------------------------------------------------------------------------
// ra8_tile_cache.h
// ---------------------------------------------------------------------------

/// The image-tile cache: the third facade over the same engine, LRU like the
/// glyph atlas but at tile scale, and the only one that warms through the
/// engine's own prefetch rather than a get/put pair.
const Tiles = tile_cache.Cache(Engine);

comptime {
    // `ra8_tile_cache_t` is caller-allocated (a `static` in the manga and
    // zoom readers, a stack local in the tests), so the engine state in front
    // of these two fields has to be exactly the width C gives it.
    std.debug.assert(@offsetOf(Tiles, "kc") == 0);
    std.debug.assert(@offsetOf(Tiles, "decode") == @sizeOf(keycache.State));
    std.debug.assert(@sizeOf(Tiles) == @sizeOf(keycache.State) + 2 * @sizeOf(usize));
}

export fn ra8_tile_rect_of_pixels(
    px: u32,
    py: u32,
    pw: u32,
    ph: u32,
    tile_w: u16,
    tile_h: u16,
    tile_cols: u16,
    tile_rows: u16,
    out: ?*tile_cache.Rect,
) u16 {
    const dst = out orelse return Err.null_ptr.code();
    const grid: tile_cache.geometry.Grid = .{
        .tile_w = tile_w,
        .tile_h = tile_h,
        .cols = tile_cols,
        .rows = tile_rows,
    };
    dst.* = tile_cache.geometry.rectOfPixels(px, py, pw, ph, grid) orelse
        return Err.invalid_arg.code();
    return Err.ok.code();
}

export fn ra8_tile_cache_init(handle: ?*Tiles, cfg: ?*const tile_cache.Cfg) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const config = cfg orelse return Err.null_ptr.code();
    return self.init(config).code();
}

export fn ra8_tile_cache_get(
    handle: ?*Tiles,
    key: ?*const tile_cache.Key,
    out_tile: ?*tile_cache.Tile,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const k = key orelse return Err.null_ptr.code();
    const dst = out_tile orelse return Err.null_ptr.code();
    switch (self.get(k)) {
        .tile => |tile| {
            dst.* = tile;
            return Err.ok.code();
        },
        .failed => |err| return err.code(),
    }
}

export fn ra8_tile_cache_put(handle: ?*Tiles, pixels: ?[*]const u8) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const data = pixels orelse return Err.null_ptr.code();
    return self.put(data).code();
}

export fn ra8_tile_cache_capacity(handle: ?*const Tiles) u32 {
    const self = handle orelse return 0;
    return self.capacity() orelse 0;
}

export fn ra8_tile_cache_prefetch(handle: ?*Tiles, key: ?*const tile_cache.Key) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const k = key orelse return Err.null_ptr.code();
    return self.prefetch(k).code();
}

export fn ra8_tile_cache_prefetch_pan(
    handle: ?*Tiles,
    req: ?*const tile_cache.PrefetchReq,
    out_warmed: ?*u16,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const request = req orelse return Err.null_ptr.code();
    // Zeroed before the request is judged, as the C did: a caller that reads
    // the count after a rejection sees 0 rather than its own stale value.
    if (out_warmed) |dst| dst.* = 0;
    switch (self.prefetchPan(request)) {
        .warmed => |count| {
            if (out_warmed) |dst| dst.* = count;
            return Err.ok.code();
        },
        .failed => |err| return err.code(),
    }
}

export fn ra8_tile_cache_stats(
    handle: ?*const Tiles,
    out_hits: ?*u32,
    out_misses: ?*u32,
    out_evictions: ?*u32,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    return self.stats(out_hits, out_misses, out_evictions).code();
}

// ---------------------------------------------------------------------------
// ra8_vsource.h
// ---------------------------------------------------------------------------

export fn ra8_vsource_init(
    handle: ?*vsource.Registry,
    objs: ?[*]vsource.Obj,
    cap: u32,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const slots = objs orelse return Err.null_ptr.code();
    if (cap == 0) return Err.invalid_size.code();
    return vsource.init(self, slots[0..cap]).code();
}

export fn ra8_vsource_add_paged(
    handle: ?*vsource.Registry,
    read: ?vsource.ReadFn,
    ctx: ?*anyopaque,
    base: u64,
    size: u64,
    out_id: ?*u32,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const backing = read orelse return Err.null_ptr.code();
    const dst = out_id orelse return Err.null_ptr.code();
    return vsource.addPaged(self, backing, ctx, base, size, dst).code();
}

export fn ra8_vsource_add_xip(
    handle: ?*vsource.Registry,
    xip_base: ?[*]const u8,
    size: u64,
    out_id: ?*u32,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const mapped = xip_base orelse return Err.null_ptr.code();
    const dst = out_id orelse return Err.null_ptr.code();
    return vsource.addXip(self, mapped, size, dst).code();
}

/// The `ra8_vmem_loader_fn` the page cache calls on a miss: `ctx` is the
/// registry, arriving as the cache's opaque `loader_ctx` cookie.
export fn ra8_vsource_loader(
    ctx: ?*anyopaque,
    object_id: u32,
    offset: u64,
    frame: ?[*]u8,
    frame_bytes: u32,
) u16 {
    const self: *const vsource.Registry = @ptrCast(@alignCast(ctx orelse
        return Err.null_ptr.code()));
    const dst = frame orelse return Err.null_ptr.code();
    return vsource.load(self, object_id, offset, dst[0..frame_bytes]).code();
}

export fn ra8_vsource_xip_ptr(
    handle: ?*const vsource.Registry,
    object_id: u32,
    offset: u64,
    len: u32,
    out_ptr: ?*?[*]const u8,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const dst = out_ptr orelse return Err.null_ptr.code();
    switch (vsource.xipPtr(self, object_id, offset, len)) {
        .ptr => |p| {
            dst.* = p;
            return Err.ok.code();
        },
        .failed => |err| return err.code(),
    }
}
