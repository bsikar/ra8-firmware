//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C membrane for the Zig side of `ra8_mem`: every symbol
//! `inc/ra8_slab.h`, `inc/ra8_vmem_stream.h` and `inc/ra8_glyph_atlas.h`
//! declare, and nothing else. Those headers are unchanged, so the host suite,
//! `mem_subsystem`, `reflow`, `glyph_bench` and the rest of `libs/ra8_mem` link
//! against this archive without knowing the bodies moved.
//!
//! Raw pointers stop here. Everything past this file works in slices, typed
//! enums and non-optional references.

const std = @import("std");

const glyph_atlas = @import("internal/glyph_atlas.zig");
const keycache = @import("internal/keycache.zig");
const slab = @import("internal/slab.zig");
const vmem_stream = @import("internal/vmem_stream.zig");
const vocab = @import("internal/vocab.zig");

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

/// The page cache, still C (`src/ra8_vmem.c`). These are the only two symbols
/// the stream adapter needs from it; the archive leaves them undefined and the
/// link resolves them, exactly as the C TU did.
extern fn ra8_vmem_get(
    vm: ?*vmem_stream.Vmem,
    object_id: u32,
    offset: u64,
    out_page: *?*anyopaque,
) u16;
extern fn ra8_vmem_put(vm: ?*vmem_stream.Vmem, page: ?*anyopaque) u16;

/// Adapts those two into what the implementation works in: a frame arrives as
/// a slice of its real length, so the in-frame copy is bounds-checked rather
/// than trusted the way `(const uint8_t*)page + in_frame` was.
const Cache = struct {
    pub fn get(vm: ?*vmem_stream.Vmem, object_id: u32, offset: u64, frame_bytes: u32) vmem_stream.Frame {
        var page: ?*anyopaque = null;
        const code = ra8_vmem_get(vm, object_id, offset, &page);
        if (code != Err.ok.code()) return .{ .failed = Err.from(code) };
        const frame = page orelse return .{ .failed = .null_ptr };
        return .{ .page = @as([*]const u8, @ptrCast(frame))[0..frame_bytes] };
    }

    pub fn put(vm: ?*vmem_stream.Vmem, page: []const u8) Err {
        return Err.from(ra8_vmem_put(vm, @constCast(@as(*const anyopaque, @ptrCast(page.ptr)))));
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
