//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the page-cache facade. The engine is a fake, so the tests
//! can fail an init the real `ra8_keycache` accepts, hand back a view the real
//! one would never produce, and call the fill and hash trampolines directly
//! with no cache behind them.

const std = @import("std");

const vmem = @import("vmem");

const Err = vmem.Err;
const Key = vmem.Key;
const State = vmem.State;

const frame_bytes: u32 = 64;
const frame_count: u32 = 4;

var frames: [frame_count * frame_bytes]u8 = undefined;
var metas: [frame_count]vmem.keycache.Cell = undefined;
var keys: [frame_count]Key = undefined;
var buckets: [frame_count]i32 = undefined;

/// What the fake engine saw and what it will answer with.
const Seen = struct {
    var cfg: vmem.keycache.Cfg = undefined;
    var key: Key = .{};
    var released: ?[*]const u8 = null;
    var init_err: Err = .ok;
    var get_err: Err = .ok;
    var put_err: Err = .ok;
    var stats_err: Err = .ok;
    var view_data: ?[*]u8 = null;
    var protected_cap: u32 = 3;
    var gets: u32 = 0;
    var puts: u32 = 0;

    fn reset() void {
        cfg = std.mem.zeroes(vmem.keycache.Cfg);
        key = .{};
        released = null;
        init_err = .ok;
        get_err = .ok;
        put_err = .ok;
        stats_err = .ok;
        view_data = &frames;
        protected_cap = 3;
        gets = 0;
        puts = 0;
    }
};

const FakeEngine = struct {
    pub fn init(state: *vmem.keycache.State, cfg: *const vmem.keycache.Cfg) Err {
        Seen.cfg = cfg.*;
        if (Seen.init_err != .ok) return Seen.init_err;
        state.* = std.mem.zeroes(vmem.keycache.State);
        state.cfg = cfg.*;
        state.protected_cap = Seen.protected_cap;
        return .ok;
    }

    pub fn get(_: *vmem.keycache.State, key: anytype, out_view: *vmem.keycache.View) Err {
        Seen.gets += 1;
        Seen.key = @as(*const Key, @ptrCast(@alignCast(key))).*;
        if (Seen.get_err != .ok) return Seen.get_err;
        out_view.* = .{ .data = Seen.view_data, .user = null };
        return .ok;
    }

    pub fn put(_: *vmem.keycache.State, data: [*]const u8) Err {
        Seen.puts += 1;
        Seen.released = data;
        return Seen.put_err;
    }

    pub fn stats(
        _: *const vmem.keycache.State,
        out_hits: ?*u32,
        _: ?*u32,
        _: ?*u32,
    ) Err {
        if (Seen.stats_err != .ok) return Seen.stats_err;
        if (out_hits) |hits| hits.* = 7;
        return .ok;
    }
};

const Cache = vmem.Vmem(FakeEngine);

var loader_calls: u32 = 0;
var loader_saw: Key = .{};
var loader_bytes: u32 = 0;
var loader_err: Err = .ok;

fn loader(
    ctx: ?*anyopaque,
    object_id: u32,
    offset: u64,
    frame: ?[*]u8,
    bytes: u32,
) callconv(.c) u16 {
    loader_calls += 1;
    loader_saw = .{ .object_id = object_id, .offset = offset };
    loader_bytes = bytes;
    if (ctx) |seen| @as(*u32, @ptrCast(@alignCast(seen))).* = object_id;
    if (frame) |dst| dst[0] = 0xAB;
    return loader_err.code();
}

fn config() vmem.Cfg {
    return .{
        .frame_mem = &frames,
        .frame_bytes = frame_bytes,
        .frame_count = frame_count,
        .meta = &metas,
        .keys = &keys,
        .buckets = &buckets,
        .bucket_count = frame_count,
        .loader = loader,
        .loader_ctx = null,
        .protected_pct = 50,
    };
}

/// Binds in place. The engine keeps a back-pointer to the handle
/// (`render_ctx`), so a `State` returned by value would leave the fake looking
/// at the dead local.
fn bind(state: *State) void {
    Seen.reset();
    loader_calls = 0;
    loader_err = .ok;
    std.debug.assert(Cache.init(state, &config()) == .ok);
}

test "init copies the config and configures the engine for SLRU" {
    var state: State = .{};
    bind(&state);

    try std.testing.expectEqual(frame_bytes, state.cfg.frame_bytes);
    try std.testing.expectEqual(@as(u32, 3), state.protected_cap);
    try std.testing.expectEqual(frame_bytes, Seen.cfg.cell_bytes);
    try std.testing.expectEqual(frame_count, Seen.cfg.cell_count);
    try std.testing.expectEqual(@as(u32, @sizeOf(Key)), Seen.cfg.key_bytes);
    try std.testing.expectEqual(vmem.keycache.Evict.slru, Seen.cfg.evict);
    try std.testing.expectEqual(@as(u8, 50), Seen.cfg.protected_pct);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&state)), Seen.cfg.render_ctx);
    try std.testing.expect(Seen.cfg.render != null);
    try std.testing.expect(Seen.cfg.hash != null);
    // The page cache has no per-cell descriptor.
    try std.testing.expectEqual(@as(?[*]u8, null), Seen.cfg.user_mem);
    try std.testing.expectEqual(@as(u32, 0), Seen.cfg.user_bytes);
}

test "a failed engine init leaves the handle unbound" {
    Seen.reset();
    Seen.init_err = .invalid_size;

    var state: State = .{};
    try std.testing.expectEqual(Err.invalid_size, Cache.init(&state, &config()));
    try std.testing.expectEqual(@as(u32, 0), state.cfg.frame_bytes);
    try std.testing.expectEqual(@as(?[*]u8, null), state.cfg.frame_mem);
    try std.testing.expectEqual(@as(u32, 0), state.protected_cap);

    // And the unbound handle answers rather than dividing by a zero frame.
    var page: ?*anyopaque = null;
    try std.testing.expectEqual(Err.invalid_state, Cache.get(&state, 1, 100, &page));
    try std.testing.expectEqual(Err.invalid_state, Cache.prefetch(&state, 1, 100));
    try std.testing.expectEqual(Err.invalid_state, Cache.stats(&state, null, null, null));
}

test "get rounds the offset down to a frame boundary" {
    var state: State = .{};
    bind(&state);
    var page: ?*anyopaque = null;

    try std.testing.expectEqual(Err.ok, Cache.get(&state, 9, frame_bytes + 1, &page));
    try std.testing.expectEqual(@as(u32, 9), Seen.key.object_id);
    try std.testing.expectEqual(@as(u64, frame_bytes), Seen.key.offset);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&frames)), page);

    try std.testing.expectEqual(Err.ok, Cache.get(&state, 9, frame_bytes - 1, &page));
    try std.testing.expectEqual(@as(u64, 0), Seen.key.offset);

    try std.testing.expectEqual(Err.ok, Cache.get(&state, 9, 10 * frame_bytes, &page));
    try std.testing.expectEqual(@as(u64, 10 * frame_bytes), Seen.key.offset);
}

test "get returns the engine error and a null view is invalid state" {
    var state: State = .{};
    bind(&state);
    var page: ?*anyopaque = null;

    Seen.get_err = .no_mem;
    try std.testing.expectEqual(Err.no_mem, Cache.get(&state, 1, 0, &page));
    try std.testing.expectEqual(@as(?*anyopaque, null), page);

    Seen.get_err = .ok;
    Seen.view_data = null;
    try std.testing.expectEqual(Err.invalid_state, Cache.get(&state, 1, 0, &page));
}

test "put releases the page it was handed" {
    var state: State = .{};
    bind(&state);

    try std.testing.expectEqual(Err.ok, Cache.put(&state, &frames));
    try std.testing.expectEqual(@as(?[*]const u8, &frames), Seen.released);

    Seen.put_err = .invalid_arg;
    try std.testing.expectEqual(Err.invalid_arg, Cache.put(&state, &frames));
}

test "prefetch warms the page and drops the pin" {
    var state: State = .{};
    bind(&state);

    try std.testing.expectEqual(Err.ok, Cache.prefetch(&state, 4, frame_bytes + 7));
    try std.testing.expectEqual(@as(u64, frame_bytes), Seen.key.offset);
    try std.testing.expectEqual(@as(u32, 1), Seen.gets);
    try std.testing.expectEqual(@as(u32, 1), Seen.puts);
}

test "prefetch holds no pin when the get or the put fails" {
    var state: State = .{};
    bind(&state);

    Seen.get_err = .no_mem;
    try std.testing.expectEqual(Err.no_mem, Cache.prefetch(&state, 4, 0));
    try std.testing.expectEqual(@as(u32, 0), Seen.puts);

    Seen.get_err = .ok;
    Seen.put_err = .invalid_arg;
    try std.testing.expectEqual(Err.invalid_arg, Cache.prefetch(&state, 4, 0));
    try std.testing.expectEqual(@as(u32, 1), Seen.puts);
}

test "stats needs a bound cache and otherwise forwards" {
    var state: State = .{};
    bind(&state);

    var hits: u32 = 0;
    try std.testing.expectEqual(Err.ok, Cache.stats(&state, &hits, null, null));
    try std.testing.expectEqual(@as(u32, 7), hits);

    Seen.stats_err = .invalid_state;
    try std.testing.expectEqual(Err.invalid_state, Cache.stats(&state, &hits, null, null));

    state.cfg.frame_mem = null;
    try std.testing.expectEqual(Err.invalid_state, Cache.stats(&state, &hits, null, null));
}

test "the injected hash mixes both halves of the offset" {
    const a: Key = .{ .object_id = 1, .offset = 0 };
    const b: Key = .{ .object_id = 1, .offset = frame_bytes };
    const c: Key = .{ .object_id = 2, .offset = 0 };
    const high: Key = .{ .object_id = 1, .offset = 1 << 32 };

    try std.testing.expect(vmem.hashKey(&a) != vmem.hashKey(&b));
    try std.testing.expect(vmem.hashKey(&a) != vmem.hashKey(&c));
    try std.testing.expect(vmem.hashKey(&a) != vmem.hashKey(&high));
    // Pure: the same key hashes the same way every time.
    try std.testing.expectEqual(vmem.hashKey(&b), vmem.hashKey(&b));

    // A large object id must wrap rather than trap.
    const big: Key = .{ .object_id = 0xFFFF_FFFF, .offset = 0xFFFF_FFFF_FFFF_FFC0 };
    _ = vmem.hashKey(&big);
}

test "the engine hashes through the same fold the facade published" {
    var state: State = .{};
    bind(&state);
    const key: Key = .{ .object_id = 3, .offset = 2 * frame_bytes };

    const fold = Seen.cfg.hash.?;
    try std.testing.expectEqual(vmem.hashKey(&key), fold(&key, @sizeOf(Key), null));
    _ = &state;
}

test "the fill trampoline hands the page key to the loader" {
    var state: State = .{};
    bind(&state);
    var ctx_seen: u32 = 0;
    state.cfg.loader_ctx = &ctx_seen;

    const fill = Seen.cfg.render.?;
    var cell: [frame_bytes]u8 = undefined;
    const key: Key = .{ .object_id = 12, .offset = 3 * frame_bytes };

    try std.testing.expectEqual(
        Err.ok.code(),
        fill(&state, &key, &cell, frame_bytes, null),
    );
    try std.testing.expectEqual(@as(u32, 1), loader_calls);
    try std.testing.expectEqual(@as(u32, 12), loader_saw.object_id);
    try std.testing.expectEqual(@as(u64, 3 * frame_bytes), loader_saw.offset);
    try std.testing.expectEqual(frame_bytes, loader_bytes);
    try std.testing.expectEqual(@as(u8, 0xAB), cell[0]);
    try std.testing.expectEqual(@as(u32, 12), ctx_seen);

    loader_err = .out_of_range;
    try std.testing.expectEqual(
        Err.out_of_range.code(),
        fill(&state, &key, &cell, frame_bytes, null),
    );
}

test "a forged registry with no loader fills nothing" {
    var state: State = .{};
    bind(&state);
    state.cfg.loader = null;

    const fill = Seen.cfg.render.?;
    var cell: [frame_bytes]u8 = undefined;
    const key: Key = .{ .object_id = 1, .offset = 0 };

    try std.testing.expectEqual(
        Err.invalid_state.code(),
        fill(&state, &key, &cell, frame_bytes, null),
    );
}
