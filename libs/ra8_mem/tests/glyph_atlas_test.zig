//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the glyph-cache facade. The engine is a fake, which is the
//! point: it can hand back a descriptor and it can fail a render on demand,
//! neither of which the real `ra8_keycache` plus the real rasteriser will do
//! when asked. What is under test is the facade's own behaviour -- what it puts
//! into the engine config, what the render trampoline records and when, and
//! what it reads back out of a view.

const std = @import("std");
const glyph_atlas = @import("glyph_atlas");

const keycache = glyph_atlas.keycache;
const Err = glyph_atlas.Err;
const Key = glyph_atlas.Key;
const Dims = glyph_atlas.Dims;

/// A code `ra8_err.h` publishes but this library never raises itself, used to
/// prove a renderer's verdict arrives at the caller unflattened.
const renderer_own_code: u16 = 0x311;

/// One cell's worth of storage plus the knobs the fake engine reads.
const Fake = struct {
    var cells: [3][8]u8 = undefined;
    var dims: [3]Dims = undefined;
    var keys: [3]Key = undefined;
    var meta: [3]keycache.Cell = undefined;
    var buckets: [4]i32 = undefined;

    var render_calls: u32 = 0;
    var render_result: u16 = 0;
    var render_w: u16 = 5;
    var render_h: u16 = 7;
    var resident: bool = false;
    var get_result: Err = .ok;
    var stats_calls: u32 = 0;

    fn reset() void {
        cells = std.mem.zeroes(@TypeOf(cells));
        dims = std.mem.zeroes(@TypeOf(dims));
        keys = std.mem.zeroes(@TypeOf(keys));
        meta = std.mem.zeroes(@TypeOf(meta));
        buckets = std.mem.zeroes(@TypeOf(buckets));
        render_calls = 0;
        render_result = 0;
        render_w = 5;
        render_h = 7;
        resident = false;
        get_result = .ok;
        stats_calls = 0;
    }

    fn cfg() glyph_atlas.Cfg {
        return .{
            .cell_mem = @ptrCast(&cells),
            .cell_bytes = cells[0].len,
            .cell_count = cells.len,
            .meta = &meta,
            .keys = &keys,
            .dims = &dims,
            .buckets = &buckets,
            .bucket_count = buckets.len,
            .render = &render,
            .render_ctx = @ptrCast(&render_calls),
        };
    }

    /// The public glyph renderer. Writes the cell so a test can tell a real
    /// render from a hit, and reports its own code when asked to fail.
    fn render(
        ctx: ?*anyopaque,
        key: ?*const Key,
        cell: ?[*]u8,
        cell_bytes: u32,
        out_w: ?*u16,
        out_h: ?*u16,
    ) callconv(.c) u16 {
        _ = ctx;
        render_calls += 1;
        if (render_result != Err.ok.code()) return render_result;
        cell.?[0] = @truncate(key.?.glyph_id);
        std.debug.assert(cell_bytes == cells[0].len);
        out_w.?.* = render_w;
        out_h.?.* = render_h;
        return Err.ok.code();
    }
};

/// The engine seam: enough of `ra8_keycache` to drive the facade, and no more.
const FakeEngine = struct {
    var last_cfg: keycache.Cfg = undefined;

    pub fn init(state: *keycache.State, engine: *const keycache.Cfg) Err {
        last_cfg = engine.*;
        if (engine.cell_count == 0 or engine.cell_bytes == 0 or engine.bucket_count == 0) {
            return .invalid_size;
        }
        state.* = std.mem.zeroes(keycache.State);
        state.cfg = engine.*;
        return .ok;
    }

    pub fn get(state: *keycache.State, key: *const Key, out_view: *keycache.View) Err {
        if (Fake.get_result != .ok) return Fake.get_result;
        const cell = state.cfg.cell_mem.?;
        const user: *anyopaque = @ptrCast(state.cfg.user_mem.?);
        if (!Fake.resident) {
            const code = state.cfg.render.?(state.cfg.render_ctx, @ptrCast(key), cell, state.cfg.cell_bytes, user);
            if (code != Err.ok.code()) return Err.from(code);
            state.misses += 1;
            Fake.resident = true;
        } else {
            state.hits += 1;
        }
        out_view.* = .{ .data = cell, .user = user };
        return .ok;
    }

    pub fn put(state: *keycache.State, bitmap: [*]const u8) Err {
        if (bitmap != state.cfg.cell_mem.?) return .invalid_arg;
        return .ok;
    }

    pub fn stats(
        state: *const keycache.State,
        out_hits: ?*u32,
        out_misses: ?*u32,
        out_evictions: ?*u32,
    ) Err {
        Fake.stats_calls += 1;
        if (out_hits) |h| h.* = state.hits;
        if (out_misses) |m| m.* = state.misses;
        if (out_evictions) |e| e.* = state.evictions;
        return .ok;
    }
};

const Atlas = glyph_atlas.Atlas(FakeEngine);

/// Binds in place. The atlas cannot be returned by value or moved after init:
/// it hands the engine a pointer back to itself for the render trampoline, so a
/// copy leaves the engine pointing at the original. The C facade has the same
/// property, which is why every consumer holds the atlas in a stable slot.
fn bind(atlas: *Atlas) void {
    Fake.reset();
    atlas.* = std.mem.zeroes(Atlas);
    const cfg = Fake.cfg();
    std.debug.assert(atlas.init(&cfg) == .ok);
}

test "init refuses a config with no renderer" {
    Fake.reset();
    var atlas = std.mem.zeroes(Atlas);
    var cfg = Fake.cfg();
    cfg.render = null;
    try std.testing.expectEqual(Err.null_ptr, atlas.init(&cfg));
}

test "init describes the glyph key and the descriptor to the engine" {
    Fake.reset();
    var atlas = std.mem.zeroes(Atlas);
    const cfg = Fake.cfg();
    try std.testing.expectEqual(Err.ok, atlas.init(&cfg));

    // The whole reason the facade exists: the engine is told the key is 12
    // bytes wide and each cell carries a 4-byte descriptor.
    try std.testing.expectEqual(@as(u32, 12), FakeEngine.last_cfg.key_bytes);
    try std.testing.expectEqual(@as(u32, 4), FakeEngine.last_cfg.user_bytes);
    try std.testing.expectEqual(cfg.cell_bytes, FakeEngine.last_cfg.cell_bytes);
    try std.testing.expectEqual(cfg.cell_count, FakeEngine.last_cfg.cell_count);
    try std.testing.expectEqual(cfg.bucket_count, FakeEngine.last_cfg.bucket_count);

    // The engine renders through the facade's trampoline, not the caller's
    // renderer, and the context it gets back is the atlas itself.
    try std.testing.expect(FakeEngine.last_cfg.render != null);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&atlas)), FakeEngine.last_cfg.render_ctx);
}

test "init passes an engine sizing rejection straight back" {
    Fake.reset();
    var atlas = std.mem.zeroes(Atlas);
    var cfg = Fake.cfg();
    cfg.cell_count = 0;
    try std.testing.expectEqual(Err.invalid_size, atlas.init(&cfg));
}

test "a miss renders once and the view carries the rendered size" {
    var atlas: Atlas = undefined;
    bind(&atlas);
    const key: Key = .{ .glyph_id = 0x41, .face_id = 1, .size_px = 16, .mode = 0, .reserved = 0 };

    const first = atlas.get(&key);
    try std.testing.expectEqual(@as(u32, 1), Fake.render_calls);
    try std.testing.expectEqual(@as(u16, 5), first.glyph.width);
    try std.testing.expectEqual(@as(u16, 7), first.glyph.height);
    try std.testing.expectEqual(@as(u8, 0x41), first.glyph.bitmap.?[0]);
}

test "a hit reads the descriptor back without rendering again" {
    var atlas: Atlas = undefined;
    bind(&atlas);
    const key: Key = .{ .glyph_id = 0x42, .face_id = 1, .size_px = 16, .mode = 0, .reserved = 0 };
    _ = atlas.get(&key);
    Fake.render_w = 99; // never read: a hit must not re-render
    Fake.render_h = 98;

    const second = atlas.get(&key);
    try std.testing.expectEqual(@as(u32, 1), Fake.render_calls);
    try std.testing.expectEqual(@as(u16, 5), second.glyph.width);
    try std.testing.expectEqual(@as(u16, 7), second.glyph.height);
}

test "a renderer's own error code reaches the caller unflattened" {
    var atlas: Atlas = undefined;
    bind(&atlas);
    Fake.render_result = renderer_own_code;
    const key: Key = .{ .glyph_id = 1, .face_id = 1, .size_px = 16, .mode = 0, .reserved = 0 };

    const result = atlas.get(&key);
    try std.testing.expectEqual(renderer_own_code, result.failed.code());
}

test "a failed render leaves no dimensions behind" {
    var atlas: Atlas = undefined;
    bind(&atlas);
    Fake.render_result = renderer_own_code;
    const key: Key = .{ .glyph_id = 1, .face_id = 1, .size_px = 16, .mode = 0, .reserved = 0 };
    _ = atlas.get(&key);

    // The descriptor is written only after the renderer succeeds, so a later
    // hit cannot read a size that was never rasterised.
    try std.testing.expectEqual(@as(u16, 0), Fake.dims[0].w);
    try std.testing.expectEqual(@as(u16, 0), Fake.dims[0].h);
}

test "an engine that cannot evict reports no_mem" {
    var atlas: Atlas = undefined;
    bind(&atlas);
    Fake.get_result = .no_mem;
    const key: Key = .{ .glyph_id = 1, .face_id = 1, .size_px = 16, .mode = 0, .reserved = 0 };

    try std.testing.expectEqual(Err.no_mem, atlas.get(&key).failed);
    try std.testing.expectEqual(@as(u32, 0), Fake.render_calls);
}

test "put hands the engine the bitmap it was given" {
    var atlas: Atlas = undefined;
    bind(&atlas);
    const key: Key = .{ .glyph_id = 1, .face_id = 1, .size_px = 16, .mode = 0, .reserved = 0 };
    const glyph = atlas.get(&key).glyph;

    try std.testing.expectEqual(Err.ok, atlas.put(glyph.bitmap.?));
}

test "put rejects a pointer that is not a cell of this atlas" {
    var atlas: Atlas = undefined;
    bind(&atlas);
    var stray: u8 = 0;
    try std.testing.expectEqual(Err.invalid_arg, atlas.put(@ptrCast(&stray)));
}

test "stats on an unbound atlas is a state error, not a crash" {
    Fake.reset();
    const atlas: Atlas = std.mem.zeroes(Atlas);
    var hits: u32 = 7;

    try std.testing.expectEqual(Err.invalid_state, atlas.stats(&hits, null, null));
    try std.testing.expectEqual(@as(u32, 0), Fake.stats_calls);
    try std.testing.expectEqual(@as(u32, 7), hits); // untouched
}

test "stats reports the engine's counters once bound" {
    var atlas: Atlas = undefined;
    bind(&atlas);
    const key: Key = .{ .glyph_id = 1, .face_id = 1, .size_px = 16, .mode = 0, .reserved = 0 };
    _ = atlas.get(&key); // miss
    _ = atlas.get(&key); // hit

    var hits: u32 = 0;
    var misses: u32 = 0;
    try std.testing.expectEqual(Err.ok, atlas.stats(&hits, &misses, null));
    try std.testing.expectEqual(@as(u32, 1), hits);
    try std.testing.expectEqual(@as(u32, 1), misses);
}

test "stats accepts a caller that wants only some counters" {
    var atlas: Atlas = undefined;
    bind(&atlas);
    try std.testing.expectEqual(Err.ok, atlas.stats(null, null, null));
    try std.testing.expectEqual(@as(u32, 1), Fake.stats_calls);
}

test "the glyph key is padding-free so byte-wise comparison is sound" {
    // A zeroed key plus the four real fields must leave no indeterminate byte:
    // the engine hashes and compares all 12 of them.
    var key = std.mem.zeroes(Key);
    key.glyph_id = 0xAABBCCDD;
    key.face_id = 0x1122;
    key.size_px = 0x3344;
    key.mode = 0x5566;

    const bytes = std.mem.asBytes(&key);
    try std.testing.expectEqual(@as(usize, 12), bytes.len);
    try std.testing.expectEqual(@as(u16, 0), key.reserved);
}

test "the engine is given a back-pointer to the atlas, so the atlas is pinned" {
    var atlas: Atlas = undefined;
    bind(&atlas);

    // Documents the constraint the C shares and never stated: the render
    // trampoline reaches the caller's renderer through this pointer, so moving
    // or copying a bound atlas points the engine at the wrong object.
    try std.testing.expectEqual(
        @as(?*anyopaque, @ptrCast(&atlas)),
        atlas.kc.cfg.render_ctx,
    );
}
