//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the tile-cache facade. The engine is a fake, which is the
//! point: it can refuse a warm and fail a decode on demand, neither of which
//! the real `ra8_keycache` plus a real decoder will do when asked. What is
//! under test is the facade's own behaviour -- what it puts into the engine
//! config, what the decode trampoline records and when, and where the
//! best-effort pan sweep stops.

const std = @import("std");
const tile_cache = @import("tile_cache");

const keycache = tile_cache.keycache;
const Err = tile_cache.Err;
const Key = tile_cache.Key;
const Dims = tile_cache.Dims;
const PrefetchReq = tile_cache.PrefetchReq;

/// A code `ra8_err.h` publishes but this library never raises itself, used to
/// prove a decoder's verdict arrives at the caller unflattened.
const decoder_own_code: u16 = 0x311;

/// Cell storage plus the knobs the fake engine reads.
const Fake = struct {
    var cells: [4][8]u8 = undefined;
    var dims: [4]Dims = undefined;
    var keys: [4]Key = undefined;
    var meta: [4]keycache.Cell = undefined;
    var buckets: [8]i32 = undefined;

    var decode_calls: u32 = 0;
    var decode_result: u16 = 0;
    var decode_w: u16 = 64;
    var decode_h: u16 = 48;
    var resident: bool = false;
    var get_result: Err = .ok;
    var prefetch_calls: u32 = 0;
    var prefetch_budget: u32 = 1000;
    var prefetched: [16]Key = undefined;
    var stats_calls: u32 = 0;

    fn reset() void {
        cells = std.mem.zeroes(@TypeOf(cells));
        dims = std.mem.zeroes(@TypeOf(dims));
        keys = std.mem.zeroes(@TypeOf(keys));
        meta = std.mem.zeroes(@TypeOf(meta));
        buckets = std.mem.zeroes(@TypeOf(buckets));
        prefetched = std.mem.zeroes(@TypeOf(prefetched));
        decode_calls = 0;
        decode_result = 0;
        decode_w = 64;
        decode_h = 48;
        resident = false;
        get_result = .ok;
        prefetch_calls = 0;
        prefetch_budget = 1000;
        stats_calls = 0;
    }

    fn cfg() tile_cache.Cfg {
        return .{
            .cell_mem = @ptrCast(&cells),
            .cell_bytes = cells[0].len,
            .cell_count = cells.len,
            .meta = &meta,
            .keys = &keys,
            .dims = &dims,
            .buckets = &buckets,
            .bucket_count = buckets.len,
            .decode = &decode,
            .decode_ctx = @ptrCast(&decode_calls),
        };
    }

    /// The public tile decoder. Writes the cell so a test can tell a real
    /// decode from a hit, and reports its own code when asked to fail.
    fn decode(
        ctx: ?*anyopaque,
        key: ?*const Key,
        cell: ?[*]u8,
        cell_bytes: u32,
        out_w: ?*u16,
        out_h: ?*u16,
    ) callconv(.c) u16 {
        _ = ctx;
        decode_calls += 1;
        if (decode_result != Err.ok.code()) return decode_result;
        std.debug.assert(cell_bytes == cells[0].len);
        cell.?[0] = @truncate(key.?.tile_x);
        cell.?[1] = @truncate(key.?.tile_y);
        out_w.?.* = decode_w;
        out_h.?.* = decode_h;
        return Err.ok.code();
    }
};

/// The engine seam: enough of `ra8_keycache` to drive the facade, no more.
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
            const code = state.cfg.render.?(
                state.cfg.render_ctx,
                @ptrCast(key),
                cell,
                state.cfg.cell_bytes,
                user,
            );
            if (code != Err.ok.code()) return Err.from(code);
            state.misses += 1;
            Fake.resident = true;
        } else {
            state.hits += 1;
        }
        out_view.* = .{ .data = cell, .user = user };
        return .ok;
    }

    pub fn put(state: *keycache.State, pixels: [*]const u8) Err {
        if (pixels != state.cfg.cell_mem.?) return .invalid_arg;
        return .ok;
    }

    /// Warming is its own engine call, not get-then-put at the facade: the
    /// real engine drops the pin itself. Records what it was asked for so a
    /// test can read the sweep back, and refuses once the budget runs out.
    pub fn prefetch(state: *keycache.State, key: *const Key) Err {
        _ = state;
        if (Fake.prefetch_calls >= Fake.prefetch_budget) return .no_mem;
        Fake.prefetched[Fake.prefetch_calls] = key.*;
        Fake.prefetch_calls += 1;
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

const Cache = tile_cache.Cache(FakeEngine);

/// Binds in place. The cache cannot be returned by value or moved after init:
/// it hands the engine a pointer back to itself for the decode trampoline, so
/// a copy leaves the engine pointing at the original.
fn bind(cache: *Cache) void {
    Fake.reset();
    cache.* = std.mem.zeroes(Cache);
    const cfg = Fake.cfg();
    std.debug.assert(cache.init(&cfg) == .ok);
}

/// A viewport of 2 columns by 3 rows inside a 10x8 grid, panning right.
fn request() PrefetchReq {
    return .{
        .image_id = 7,
        .view = .{ .tx0 = 2, .ty0 = 2, .tx1 = 3, .ty1 = 4 },
        .tile_cols = 10,
        .tile_rows = 8,
        .zoom = 1,
        .max_tiles = 16,
        .dir = .right,
    };
}

test "init refuses a config with no decoder" {
    Fake.reset();
    var cache = std.mem.zeroes(Cache);
    var cfg = Fake.cfg();
    cfg.decode = null;
    try std.testing.expectEqual(Err.null_ptr, cache.init(&cfg));
}

test "init describes the tile key and the descriptor to the engine" {
    Fake.reset();
    var cache = std.mem.zeroes(Cache);
    const cfg = Fake.cfg();
    try std.testing.expectEqual(Err.ok, cache.init(&cfg));

    try std.testing.expectEqual(@as(u32, 12), FakeEngine.last_cfg.key_bytes);
    try std.testing.expectEqual(@as(u32, 4), FakeEngine.last_cfg.user_bytes);
    try std.testing.expectEqual(cfg.cell_bytes, FakeEngine.last_cfg.cell_bytes);
    try std.testing.expectEqual(cfg.cell_count, FakeEngine.last_cfg.cell_count);

    // Tiles are LRU, not the page cache's SLRU: the facade leaves the policy
    // at the engine's default rather than selecting one.
    try std.testing.expectEqual(keycache.Evict.lru, FakeEngine.last_cfg.evict);
    try std.testing.expect(FakeEngine.last_cfg.hash == null);

    // The engine decodes through the facade's trampoline, not the caller's
    // decoder, and the context it gets back is the cache itself.
    try std.testing.expect(FakeEngine.last_cfg.render != null);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&cache)), FakeEngine.last_cfg.render_ctx);
}

test "a rejected init leaves the cache unbound, not half-bound" {
    Fake.reset();
    var cache = std.mem.zeroes(Cache);
    var cfg = Fake.cfg();
    cfg.cell_count = 0;
    try std.testing.expectEqual(Err.invalid_size, cache.init(&cfg));

    // The decoder must not survive a refused init: a cache that never opened
    // reports no capacity and no counters.
    try std.testing.expect(cache.capacity() == null);
    try std.testing.expectEqual(Err.invalid_state, cache.stats(null, null, null));
}

test "a miss decodes once and the view carries the decoded size" {
    var cache: Cache = undefined;
    bind(&cache);
    const key: Key = .{ .image_id = 3, .tile_x = 2, .tile_y = 5, .zoom = 1 };

    const first = cache.get(&key);
    try std.testing.expectEqual(@as(u32, 1), Fake.decode_calls);
    try std.testing.expectEqual(@as(u16, 64), first.tile.width);
    try std.testing.expectEqual(@as(u16, 48), first.tile.height);
    try std.testing.expectEqual(@as(u8, 2), first.tile.pixels.?[0]);
    try std.testing.expectEqual(@as(u8, 5), first.tile.pixels.?[1]);
}

test "an edge tile keeps the smaller size the decoder reported" {
    var cache: Cache = undefined;
    bind(&cache);
    Fake.decode_w = 17; // an edge tile decodes narrower than a full tile
    Fake.decode_h = 9;
    const key: Key = .{ .image_id = 3, .tile_x = 9, .tile_y = 7, .zoom = 0 };

    const tile = cache.get(&key).tile;
    try std.testing.expectEqual(@as(u16, 17), tile.width);
    try std.testing.expectEqual(@as(u16, 9), tile.height);
}

test "a hit reads the descriptor back without decoding again" {
    var cache: Cache = undefined;
    bind(&cache);
    const key: Key = .{ .image_id = 3, .tile_x = 1, .tile_y = 1, .zoom = 0 };
    _ = cache.get(&key);
    Fake.decode_w = 999; // never read: a hit must not re-decode

    const second = cache.get(&key);
    try std.testing.expectEqual(@as(u32, 1), Fake.decode_calls);
    try std.testing.expectEqual(@as(u16, 64), second.tile.width);
}

test "a decoder's own error code reaches the caller unflattened" {
    var cache: Cache = undefined;
    bind(&cache);
    Fake.decode_result = decoder_own_code;
    const key: Key = .{ .image_id = 1, .tile_x = 0, .tile_y = 0, .zoom = 0 };

    try std.testing.expectEqual(decoder_own_code, cache.get(&key).failed.code());
}

test "a failed decode leaves no dimensions behind" {
    var cache: Cache = undefined;
    bind(&cache);
    Fake.decode_result = decoder_own_code;
    const key: Key = .{ .image_id = 1, .tile_x = 0, .tile_y = 0, .zoom = 0 };
    _ = cache.get(&key);

    try std.testing.expectEqual(@as(u16, 0), Fake.dims[0].w);
    try std.testing.expectEqual(@as(u16, 0), Fake.dims[0].h);
}

test "a cache with every cell pinned reports no_mem without decoding" {
    var cache: Cache = undefined;
    bind(&cache);
    Fake.get_result = .no_mem;
    const key: Key = .{ .image_id = 1, .tile_x = 0, .tile_y = 0, .zoom = 0 };

    try std.testing.expectEqual(Err.no_mem, cache.get(&key).failed);
    try std.testing.expectEqual(@as(u32, 0), Fake.decode_calls);
}

test "put hands the engine the pixels it was given" {
    var cache: Cache = undefined;
    bind(&cache);
    const key: Key = .{ .image_id = 1, .tile_x = 0, .tile_y = 0, .zoom = 0 };
    const tile = cache.get(&key).tile;

    try std.testing.expectEqual(Err.ok, cache.put(tile.pixels.?));
}

test "put rejects a pointer that is not a cell of this cache" {
    var cache: Cache = undefined;
    bind(&cache);
    var stray: u8 = 0;
    try std.testing.expectEqual(Err.invalid_arg, cache.put(@ptrCast(&stray)));
}

test "capacity is the configured cell count once bound, and none before" {
    const cold: Cache = std.mem.zeroes(Cache);
    try std.testing.expect(cold.capacity() == null);

    var cache: Cache = undefined;
    bind(&cache);
    try std.testing.expectEqual(@as(u32, 4), cache.capacity().?);
}

test "prefetch warms through the engine rather than pinning a get" {
    var cache: Cache = undefined;
    bind(&cache);
    const key: Key = .{ .image_id = 3, .tile_x = 4, .tile_y = 4, .zoom = 0 };

    try std.testing.expectEqual(Err.ok, cache.prefetch(&key));
    try std.testing.expectEqual(@as(u32, 1), Fake.prefetch_calls);
    try std.testing.expectEqual(key, Fake.prefetched[0]);
}

test "a pan sweep warms the lead column with the request's image and zoom" {
    var cache: Cache = undefined;
    bind(&cache);
    const req = request();

    try std.testing.expectEqual(@as(u16, 3), cache.prefetchPan(&req).warmed);
    try std.testing.expectEqual(@as(u32, 3), Fake.prefetch_calls);
    for (Fake.prefetched[0..3], 0..) |key, i| {
        try std.testing.expectEqual(@as(u32, 7), key.image_id);
        try std.testing.expectEqual(@as(u16, 4), key.tile_x);
        try std.testing.expectEqual(@as(u16, @intCast(2 + i)), key.tile_y);
        try std.testing.expectEqual(@as(u16, 1), key.zoom);
        try std.testing.expectEqual(@as(u16, 0), key.reserved);
    }
}

test "the residency budget caps the sweep below the edge length" {
    var cache: Cache = undefined;
    bind(&cache);
    var req = request();
    req.max_tiles = 2;

    try std.testing.expectEqual(@as(u16, 2), cache.prefetchPan(&req).warmed);
    try std.testing.expectEqual(@as(u32, 2), Fake.prefetch_calls);
}

test "a zero budget warms nothing and still succeeds" {
    var cache: Cache = undefined;
    bind(&cache);
    var req = request();
    req.max_tiles = 0;

    try std.testing.expectEqual(@as(u16, 0), cache.prefetchPan(&req).warmed);
    try std.testing.expectEqual(@as(u32, 0), Fake.prefetch_calls);
}

test "a refused warm stops the sweep without failing the pan" {
    var cache: Cache = undefined;
    bind(&cache);
    Fake.prefetch_budget = 1;
    const req = request();

    // Best-effort: the pan reports what it managed, not the engine's refusal,
    // so a full cache never turns into a failed scroll.
    try std.testing.expectEqual(@as(u16, 1), cache.prefetchPan(&req).warmed);
}

test "a pan at the image edge warms nothing and succeeds" {
    var cache: Cache = undefined;
    bind(&cache);
    var req = request();
    req.view = .{ .tx0 = 8, .ty0 = 2, .tx1 = 9, .ty1 = 4 };

    try std.testing.expectEqual(@as(u16, 0), cache.prefetchPan(&req).warmed);
    try std.testing.expectEqual(@as(u32, 0), Fake.prefetch_calls);
}

test "an off-grid view is rejected before anything is warmed" {
    var cache: Cache = undefined;
    bind(&cache);
    var req = request();
    req.tile_cols = 3;

    try std.testing.expectEqual(Err.invalid_arg, cache.prefetchPan(&req).failed);
    try std.testing.expectEqual(@as(u32, 0), Fake.prefetch_calls);
}

test "stats on an unbound cache is a state error, not a crash" {
    Fake.reset();
    const cache: Cache = std.mem.zeroes(Cache);
    var hits: u32 = 7;

    try std.testing.expectEqual(Err.invalid_state, cache.stats(&hits, null, null));
    try std.testing.expectEqual(@as(u32, 0), Fake.stats_calls);
    try std.testing.expectEqual(@as(u32, 7), hits); // untouched
}

test "stats reports the engine's counters once bound" {
    var cache: Cache = undefined;
    bind(&cache);
    const key: Key = .{ .image_id = 1, .tile_x = 0, .tile_y = 0, .zoom = 0 };
    _ = cache.get(&key); // miss
    _ = cache.get(&key); // hit

    var hits: u32 = 0;
    var misses: u32 = 0;
    try std.testing.expectEqual(Err.ok, cache.stats(&hits, &misses, null));
    try std.testing.expectEqual(@as(u32, 1), hits);
    try std.testing.expectEqual(@as(u32, 1), misses);
}

test "the tile key is padding-free so byte-wise comparison is sound" {
    var key = std.mem.zeroes(Key);
    key.image_id = 0xAABBCCDD;
    key.tile_x = 0x1122;
    key.tile_y = 0x3344;
    key.zoom = 0x5566;

    const bytes = std.mem.asBytes(&key);
    try std.testing.expectEqual(@as(usize, 12), bytes.len);
    try std.testing.expectEqual(@as(u16, 0), key.reserved);
}

test "the engine is given a back-pointer to the cache, so the cache is pinned" {
    var cache: Cache = undefined;
    bind(&cache);

    // Documents the constraint the C shares and never stated: the decode
    // trampoline reaches the caller's decoder through this pointer, so moving
    // or copying a bound cache points the engine at the wrong object.
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&cache)), cache.kc.cfg.render_ctx);
}
