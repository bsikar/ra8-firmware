//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the cache engine itself: config validation, the hit and miss
//! paths, pinning and its refusal at the ceiling, eviction accounting, the
//! render failure path, warming, and the counters.

const std = @import("std");

const keycache = @import("keycache");

const Cell = keycache.Cell;
const Cfg = keycache.Cfg;
const State = keycache.State;
const View = keycache.View;
const Err = keycache.Err;

const cell_bytes = 4;
const key_bytes = 2;

/// What the render seam did, and what it should do next.
const Render = struct {
    var calls: u32 = 0;
    var fail_with: u16 = 0;
    var last_user: ?*anyopaque = null;

    fn reset() void {
        calls = 0;
        fail_with = 0;
        last_user = null;
    }

    /// Fills the cell with the key bytes repeated, so a test can tell which
    /// entry it is looking at.
    fn fill(
        _: ?*anyopaque,
        key: ?*const anyopaque,
        cell: ?[*]u8,
        bytes: u32,
        user: ?*anyopaque,
    ) callconv(.c) u16 {
        calls += 1;
        last_user = user;
        if (fail_with != 0) return fail_with;
        const k = @as([*]const u8, @ptrCast(key.?));
        for (0..bytes) |i| cell.?[i] = k[i % key_bytes];
        if (user) |u| @as(*u8, @ptrCast(u)).* = k[0];
        return 0;
    }
};

/// Caller-owned storage for a cache of `n` cells, as the engine requires.
fn Fixture(comptime n: u32) type {
    return struct {
        const Self = @This();

        state: State = undefined,
        cells: [n * cell_bytes]u8 = @splat(0),
        keys: [n * key_bytes]u8 = @splat(0),
        users: [n]u8 = @splat(0),
        meta: [n]Cell = [_]Cell{.{}} ** n,
        buckets: [n]i32 = @splat(-1),

        fn cfg(self: *Self, evict: keycache.Evict, user_bytes: u32) Cfg {
            return .{
                .cell_mem = &self.cells,
                .cell_bytes = cell_bytes,
                .cell_count = n,
                .key_mem = &self.keys,
                .key_bytes = key_bytes,
                .user_mem = if (user_bytes == 0) null else &self.users,
                .user_bytes = user_bytes,
                .meta = &self.meta,
                .buckets = &self.buckets,
                .bucket_count = n,
                .render = Render.fill,
                .render_ctx = null,
                .evict = evict,
                .protected_pct = 0,
                .hash = null,
                .hash_ctx = null,
            };
        }

        fn open(self: *Self, evict: keycache.Evict, user_bytes: u32) !void {
            Render.reset();
            const c = self.cfg(evict, user_bytes);
            try std.testing.expectEqual(Err.ok, keycache.init(&self.state, &c));
        }

        fn get(self: *Self, key: [key_bytes]u8) struct { err: Err, view: View } {
            var view: View = .{ .data = null, .user = null };
            const err = keycache.get(&self.state, &key, &view);
            return .{ .err = err, .view = view };
        }

        /// Fetch and immediately release, the way a reader touches an entry.
        fn touch(self: *Self, key: [key_bytes]u8) !void {
            const r = self.get(key);
            try std.testing.expectEqual(Err.ok, r.err);
            try std.testing.expectEqual(Err.ok, keycache.put(&self.state, r.view.data.?));
        }
    };
}

test "init rejects a config with no cell storage" {
    var fx: Fixture(2) = .{};
    var c = fx.cfg(.lru, 0);
    c.cell_mem = null;
    try std.testing.expectEqual(Err.null_ptr, keycache.init(&fx.state, &c));
}

test "init rejects a config with no renderer" {
    var fx: Fixture(2) = .{};
    var c = fx.cfg(.lru, 0);
    c.render = null;
    try std.testing.expectEqual(Err.null_ptr, keycache.init(&fx.state, &c));
}

test "init requires user storage once a descriptor is asked for" {
    var fx: Fixture(2) = .{};
    var c = fx.cfg(.lru, 0);
    c.user_bytes = 1;
    c.user_mem = null;
    try std.testing.expectEqual(Err.null_ptr, keycache.init(&fx.state, &c));
}

test "init rejects every zero size" {
    var fx: Fixture(2) = .{};
    inline for (.{ "cell_count", "cell_bytes", "key_bytes", "bucket_count" }) |field| {
        var c = fx.cfg(.lru, 0);
        @field(c, field) = 0;
        try std.testing.expectEqual(Err.invalid_size, keycache.init(&fx.state, &c));
    }
}

test "init rejects an SLRU share above 100 percent" {
    var fx: Fixture(2) = .{};
    var c = fx.cfg(.slru, 0);
    c.protected_pct = 101;
    try std.testing.expectEqual(Err.invalid_arg, keycache.init(&fx.state, &c));
}

test "an out of range share is ignored under LRU, which has no segments" {
    var fx: Fixture(2) = .{};
    var c = fx.cfg(.lru, 0);
    c.protected_pct = 101;
    try std.testing.expectEqual(Err.ok, keycache.init(&fx.state, &c));
    try std.testing.expectEqual(@as(u32, 0), fx.state.sets.protected_cap);
}

test "a rejected init leaves the state unbound" {
    var fx: Fixture(2) = .{};
    fx.state = std.mem.zeroes(State);
    var c = fx.cfg(.lru, 0);
    c.bucket_count = 0;
    try std.testing.expectEqual(Err.invalid_size, keycache.init(&fx.state, &c));
    try std.testing.expectEqual(@as(?[*]u8, null), fx.state.cfg.cell_mem);
    try std.testing.expectEqual(Err.invalid_state, keycache.stats(&fx.state, null, null, null));
}

test "an SLRU cache sizes its protected segment from the share" {
    var fx: Fixture(4) = .{};
    try fx.open(.slru, 0);
    try std.testing.expectEqual(@as(u32, 3), fx.state.sets.protected_cap);
}

test "an LRU cache has no protected segment at all" {
    var fx: Fixture(4) = .{};
    try fx.open(.lru, 0);
    try std.testing.expectEqual(@as(u32, 0), fx.state.sets.protected_cap);
}

test "the first get is a miss that renders and pins the cell" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);

    const r = fx.get(.{ 1, 2 });

    try std.testing.expectEqual(Err.ok, r.err);
    try std.testing.expectEqual(@as(u32, 1), Render.calls);
    try std.testing.expectEqualSlices(u8, &.{ 1, 2, 1, 2 }, r.view.data.?[0..cell_bytes]);
    try std.testing.expectEqual(@as(u16, 1), fx.meta[0].pin_count);
}

test "a second get of the same key is a hit and does not render again" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    try fx.touch(.{ 1, 2 });

    const r = fx.get(.{ 1, 2 });

    try std.testing.expectEqual(Err.ok, r.err);
    try std.testing.expectEqual(@as(u32, 1), Render.calls);

    var hits: u32 = 0;
    var misses: u32 = 0;
    try std.testing.expectEqual(Err.ok, keycache.stats(&fx.state, &hits, &misses, null));
    try std.testing.expectEqual(@as(u32, 1), hits);
    try std.testing.expectEqual(@as(u32, 1), misses);
}

test "the render seam writes the per-cell descriptor a facade reads back" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 1);

    const r = fx.get(.{ 9, 0 });

    try std.testing.expectEqual(Err.ok, r.err);
    try std.testing.expectEqual(@as(u8, 9), @as(*const u8, @ptrCast(r.view.user.?)).*);
}

test "a cache with no descriptor hands back a null user pointer" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);

    const r = fx.get(.{ 3, 3 });

    try std.testing.expectEqual(@as(?*anyopaque, null), r.view.user);
}

test "a full cache of pinned cells cannot take another miss" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    _ = fx.get(.{ 1, 1 });
    _ = fx.get(.{ 2, 2 });

    try std.testing.expectEqual(Err.no_mem, fx.get(.{ 3, 3 }).err);
}

test "an unpinned cell is evicted and counted" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    try fx.touch(.{ 1, 1 });
    try fx.touch(.{ 2, 2 });

    try fx.touch(.{ 3, 3 });

    var evictions: u32 = 0;
    try std.testing.expectEqual(Err.ok, keycache.stats(&fx.state, null, null, &evictions));
    try std.testing.expectEqual(@as(u32, 1), evictions);
    // The evicted key is gone from the index, so fetching it renders again.
    try fx.touch(.{ 1, 1 });
    try std.testing.expectEqual(@as(u32, 4), Render.calls);
}

test "the least recently used unpinned cell is the one that goes" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    try fx.touch(.{ 1, 1 });
    try fx.touch(.{ 2, 2 });
    // Re-touch 1 so 2 becomes the LRU.
    try fx.touch(.{ 1, 1 });

    try fx.touch(.{ 3, 3 });

    // 1 is still resident (no new render), 2 is not.
    const before = Render.calls;
    try fx.touch(.{ 1, 1 });
    try std.testing.expectEqual(before, Render.calls);
}

test "a pinned cell is skipped as a victim" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    const held = fx.get(.{ 1, 1 });
    try fx.touch(.{ 2, 2 });

    try fx.touch(.{ 3, 3 });

    // The pinned cell kept its bytes; the unpinned one was recycled.
    try std.testing.expectEqualSlices(u8, &.{ 1, 1, 1, 1 }, held.view.data.?[0..cell_bytes]);
}

test "a failed render leaves no entry behind and returns the seam's code" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    Render.fail_with = Err.not_supported.code();

    const r = fx.get(.{ 5, 5 });

    try std.testing.expectEqual(Err.not_supported, r.err);
    try std.testing.expectEqual(@as(u8, 0), fx.meta[0].valid);
    try std.testing.expectEqual(@as(u16, 0), fx.meta[0].pin_count);

    // The cell went back to the MRU of the probationary list, so the next miss
    // takes the LRU rather than reusing it, exactly as the C did.
    Render.fail_with = 0;
    try fx.touch(.{ 6, 6 });
    try std.testing.expectEqual(@as(u8, 0), fx.meta[0].valid);
    try std.testing.expectEqual(@as(u8, 1), fx.meta[1].valid);
}

test "a failed render on a resident victim still counts the eviction" {
    var fx: Fixture(1) = .{};
    try fx.open(.lru, 0);
    try fx.touch(.{ 1, 1 });
    Render.fail_with = Err.invalid_state.code();

    try std.testing.expectEqual(Err.invalid_state, fx.get(.{ 2, 2 }).err);

    var evictions: u32 = 0;
    try std.testing.expectEqual(Err.ok, keycache.stats(&fx.state, null, null, &evictions));
    try std.testing.expectEqual(@as(u32, 1), evictions);
    // And the old key really is gone rather than half-evicted.
    Render.fail_with = 0;
    const before = Render.calls;
    try fx.touch(.{ 1, 1 });
    try std.testing.expectEqual(before + 1, Render.calls);
}

test "put releases the pin so the cell becomes evictable" {
    var fx: Fixture(1) = .{};
    try fx.open(.lru, 0);
    const r = fx.get(.{ 1, 1 });

    try std.testing.expectEqual(Err.no_mem, fx.get(.{ 2, 2 }).err);
    try std.testing.expectEqual(Err.ok, keycache.put(&fx.state, r.view.data.?));
    try std.testing.expectEqual(Err.ok, fx.get(.{ 2, 2 }).err);
}

test "put refuses a pointer that is not one of our cells" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    var stray: [cell_bytes]u8 = @splat(0);

    try std.testing.expectEqual(Err.invalid_arg, keycache.put(&fx.state, &stray));
}

test "put refuses a pointer into the middle of a cell" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    const r = fx.get(.{ 1, 1 });

    try std.testing.expectEqual(Err.invalid_arg, keycache.put(&fx.state, r.view.data.? + 1));
}

test "put refuses a cell nobody is holding" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    try fx.touch(.{ 1, 1 });

    try std.testing.expectEqual(Err.invalid_arg, keycache.put(&fx.state, &fx.cells));
}

test "nested gets of one key stack pins that put unwinds one at a time" {
    var fx: Fixture(1) = .{};
    try fx.open(.lru, 0);
    const a = fx.get(.{ 1, 1 });
    const b = fx.get(.{ 1, 1 });

    try std.testing.expectEqual(@as(u16, 2), fx.meta[0].pin_count);
    try std.testing.expectEqual(Err.ok, keycache.put(&fx.state, a.view.data.?));
    try std.testing.expectEqual(Err.no_mem, fx.get(.{ 2, 2 }).err);
    try std.testing.expectEqual(Err.ok, keycache.put(&fx.state, b.view.data.?));
    try std.testing.expectEqual(Err.ok, fx.get(.{ 2, 2 }).err);
}

test "the pin count refuses to wrap at the ceiling" {
    var fx: Fixture(1) = .{};
    try fx.open(.lru, 0);
    try fx.touch(.{ 1, 1 });
    fx.meta[0].pin_count = std.math.maxInt(u16);

    // C incremented unchecked here: the count wrapped to zero and the caller
    // got a view of a cell that was immediately evictable.
    try std.testing.expectEqual(Err.no_mem, fx.get(.{ 1, 1 }).err);
    try std.testing.expectEqual(std.math.maxInt(u16), fx.meta[0].pin_count);
}

test "a refused pin does not count as a hit" {
    var fx: Fixture(1) = .{};
    try fx.open(.lru, 0);
    try fx.touch(.{ 1, 1 });
    fx.meta[0].pin_count = std.math.maxInt(u16);

    _ = fx.get(.{ 1, 1 });

    // The only prior get was the miss that filled the cell, so a refused pin
    // must leave the hit counter where it was.
    var hits: u32 = 0;
    try std.testing.expectEqual(Err.ok, keycache.stats(&fx.state, &hits, null, null));
    try std.testing.expectEqual(@as(u32, 0), hits);
}

test "prefetch leaves the entry resident and unpinned" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);

    try std.testing.expectEqual(Err.ok, keycache.prefetch(&fx.state, &[_]u8{ 7, 7 }));

    try std.testing.expectEqual(@as(u16, 0), fx.meta[0].pin_count);
    try std.testing.expectEqual(@as(u8, 1), fx.meta[0].valid);
    // Resident: the following get is a hit rather than a second render.
    const before = Render.calls;
    try fx.touch(.{ 7, 7 });
    try std.testing.expectEqual(before, Render.calls);
}

test "prefetching a resident key is a no-op refresh" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    try fx.touch(.{ 7, 7 });

    try std.testing.expectEqual(Err.ok, keycache.prefetch(&fx.state, &[_]u8{ 7, 7 }));

    try std.testing.expectEqual(@as(u32, 1), Render.calls);
    try std.testing.expectEqual(@as(u16, 0), fx.meta[0].pin_count);
}

test "prefetch reports the render failure and warms nothing" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    Render.fail_with = Err.out_of_range.code();

    try std.testing.expectEqual(Err.out_of_range, keycache.prefetch(&fx.state, &[_]u8{ 7, 7 }));
    try std.testing.expectEqual(@as(u8, 0), fx.meta[0].valid);
}

test "prefetch cannot warm into a fully pinned cache" {
    var fx: Fixture(1) = .{};
    try fx.open(.lru, 0);
    _ = fx.get(.{ 1, 1 });

    try std.testing.expectEqual(Err.no_mem, keycache.prefetch(&fx.state, &[_]u8{ 2, 2 }));
}

test "stats reports every counter and mutates nothing" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    try fx.touch(.{ 1, 1 });
    try fx.touch(.{ 1, 1 });

    var hits: u32 = 0;
    var misses: u32 = 0;
    var evictions: u32 = 0;
    try std.testing.expectEqual(Err.ok, keycache.stats(&fx.state, &hits, &misses, &evictions));
    try std.testing.expectEqual(@as(u32, 1), hits);
    try std.testing.expectEqual(@as(u32, 1), misses);
    try std.testing.expectEqual(@as(u32, 0), evictions);
}

test "stats accepts a caller that wants only one counter" {
    var fx: Fixture(2) = .{};
    try fx.open(.lru, 0);
    try fx.touch(.{ 1, 1 });

    var misses: u32 = 0;
    try std.testing.expectEqual(Err.ok, keycache.stats(&fx.state, null, &misses, null));
    try std.testing.expectEqual(@as(u32, 1), misses);
}

test "an SLRU scan does not evict the protected working set" {
    var fx: Fixture(4) = .{};
    try fx.open(.slru, 0);
    // Two keys earn protection by being touched twice.
    try fx.touch(.{ 1, 1 });
    try fx.touch(.{ 1, 1 });
    try fx.touch(.{ 2, 2 });
    try fx.touch(.{ 2, 2 });

    // A linear flood of one-shot keys.
    for (3..12) |i| try fx.touch(.{ @intCast(i), 0 });

    // The hot pair is still resident: fetching them renders nothing new.
    const before = Render.calls;
    try fx.touch(.{ 1, 1 });
    try fx.touch(.{ 2, 2 });
    try std.testing.expectEqual(before, Render.calls);
}

test "the same flood under LRU does displace the working set" {
    var fx: Fixture(4) = .{};
    try fx.open(.lru, 0);
    try fx.touch(.{ 1, 1 });
    try fx.touch(.{ 1, 1 });
    try fx.touch(.{ 2, 2 });
    try fx.touch(.{ 2, 2 });

    for (3..12) |i| try fx.touch(.{ @intCast(i), 0 });

    const before = Render.calls;
    try fx.touch(.{ 1, 1 });
    try std.testing.expectEqual(before + 1, Render.calls);
}

test "an injected hash places keys and lookups agree with it" {
    var fx: Fixture(4) = .{};
    Render.reset();
    var c = fx.cfg(.lru, 0);
    c.hash = struct {
        fn always(_: ?*const anyopaque, _: u32, _: ?*anyopaque) callconv(.c) u32 {
            return 0;
        }
    }.always;
    try std.testing.expectEqual(Err.ok, keycache.init(&fx.state, &c));

    // Every key collides in bucket 0, so the chain carries all of them.
    try fx.touch(.{ 1, 1 });
    try fx.touch(.{ 2, 2 });
    try fx.touch(.{ 3, 3 });

    const before = Render.calls;
    try fx.touch(.{ 1, 1 });
    try fx.touch(.{ 2, 2 });
    try fx.touch(.{ 3, 3 });
    try std.testing.expectEqual(before, Render.calls);
}

test "keys are compared over every byte, not just the first" {
    var fx: Fixture(4) = .{};
    try fx.open(.lru, 0);
    try fx.touch(.{ 1, 1 });

    try fx.touch(.{ 1, 2 });

    try std.testing.expectEqual(@as(u32, 2), Render.calls);
}

test "a one-cell cache recycles the same cell every miss" {
    var fx: Fixture(1) = .{};
    try fx.open(.lru, 0);

    for (0..4) |i| try fx.touch(.{ @intCast(i), 0 });

    var evictions: u32 = 0;
    try std.testing.expectEqual(Err.ok, keycache.stats(&fx.state, null, null, &evictions));
    try std.testing.expectEqual(@as(u32, 3), evictions);
}

test "the state is the layout a facade embeds" {
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(State, "cfg"));
    try std.testing.expectEqual(@sizeOf(Cfg), @offsetOf(State, "sets"));
    try std.testing.expectEqual(@sizeOf(Cfg) + 10 * @sizeOf(u32), @sizeOf(State));
}
