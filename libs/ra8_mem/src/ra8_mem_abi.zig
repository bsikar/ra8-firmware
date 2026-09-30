//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C membrane for the Zig side of `ra8_mem`: every symbol
//! `inc/ra8_slab.h`, `inc/ra8_vmem.h`, `inc/ra8_vmem_stream.h`,
//! `inc/ra8_glyph_atlas.h` and `inc/ra8_vsource.h` declare, and nothing else. Those headers are unchanged,
//! so the host suite, `mem_subsystem`, `reflow`, `glyph_bench`, `cache_bench`,
//! `reader_vmem` and the rest of `libs/ra8_mem` link against this archive
//! without knowing the bodies moved.
//!
//! Raw pointers stop here. Everything past this file works in slices, typed
//! enums and non-optional references.

const std = @import("std");

const glyph_atlas = @import("internal/glyph_atlas.zig");
const keycache = @import("internal/keycache.zig");
const slab = @import("internal/slab.zig");
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

/// The keyed-LRU engine, still C (`src/ra8_keycache.c`). The glyph cache is a
/// typed facade over it, so these four are all it needs; the archive leaves
/// them undefined and the link resolves them, exactly as the C TU did.
extern fn ra8_keycache_init(kc: *keycache.State, cfg: *const keycache.Cfg) u16;
extern fn ra8_keycache_get(
    kc: *keycache.State,
    key: *const anyopaque,
    out_view: *keycache.View,
) u16;
extern fn ra8_keycache_put(kc: *keycache.State, data: [*]const u8) u16;
extern fn ra8_keycache_stats(
    kc: *const keycache.State,
    out_hits: ?*u32,
    out_misses: ?*u32,
    out_evictions: ?*u32,
) u16;

/// The engine seam the facades are written against. Generic in the key so the
/// image-tile cache can be the second facade over the same four symbols.
const Engine = struct {
    pub fn init(state: *keycache.State, cfg: *const keycache.Cfg) Err {
        return Err.from(ra8_keycache_init(state, cfg));
    }

    pub fn get(state: *keycache.State, key: anytype, out_view: *keycache.View) Err {
        return Err.from(ra8_keycache_get(state, @ptrCast(key), out_view));
    }

    pub fn put(state: *keycache.State, data: [*]const u8) Err {
        return Err.from(ra8_keycache_put(state, data));
    }

    pub fn stats(
        state: *const keycache.State,
        out_hits: ?*u32,
        out_misses: ?*u32,
        out_evictions: ?*u32,
    ) Err {
        return Err.from(ra8_keycache_stats(state, out_hits, out_misses, out_evictions));
    }
};

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
