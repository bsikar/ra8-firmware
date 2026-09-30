//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The shape of `ra8_keycache` as `inc/ra8_keycache.h` publishes it, and
//! nothing else. The engine itself is still C (`src/ra8_keycache.c`): the typed
//! facades over it move to Zig first, and a facade embeds `ra8_keycache_t` by
//! value inside its own caller-allocated state, so the layout has to be right
//! before any of them can be ported.
//!
//! Types only, deliberately: no `extern fn` lives here, so a facade that
//! imports this file is still a valid host test root.

const std = @import("std");

/// `ra8_keycache_evict_t`, an `enum : uint8_t`.
pub const Evict = enum(u8) {
    lru = 0,
    slru = 1,
};

/// `ra8_keycache_hash_fn`.
pub const HashFn = *const fn (key: ?*const anyopaque, key_bytes: u32, ctx: ?*anyopaque) callconv(.c) u32;

/// `ra8_keycache_render_fn`, the render-on-miss seam.
pub const RenderFn = *const fn (
    ctx: ?*anyopaque,
    key: ?*const anyopaque,
    cell: ?[*]u8,
    cell_bytes: u32,
    user: ?*anyopaque,
) callconv(.c) u16;

/// `ra8_keycache_cell_t`: one cell's link metadata.
pub const Cell = extern struct {
    prev: i32,
    next: i32,
    hash_next: i32,
    pin_count: u16,
    seg: u8,
    valid: u8,
};

/// `ra8_keycache_cfg_t`.
pub const Cfg = extern struct {
    cell_mem: ?[*]u8,
    cell_bytes: u32,
    cell_count: u32,
    key_mem: ?[*]u8,
    key_bytes: u32,
    user_mem: ?[*]u8,
    user_bytes: u32,
    meta: ?[*]Cell,
    buckets: ?[*]i32,
    bucket_count: u32,
    render: ?RenderFn,
    render_ctx: ?*anyopaque,
    evict: Evict,
    protected_pct: u8,
    hash: ?HashFn,
    hash_ctx: ?*anyopaque,
};

/// `ra8_keycache_t`. A facade embeds this as its first member, so its size is
/// what decides where the facade's own fields land.
pub const State = extern struct {
    cfg: Cfg,
    pb_head: i32,
    pb_tail: i32,
    pt_head: i32,
    pt_tail: i32,
    protected_count: u32,
    protected_cap: u32,
    hits: u32,
    misses: u32,
    evictions: u32,
};

/// `ra8_keycache_view_t`: what a get hands back.
pub const View = extern struct {
    data: ?[*]u8,
    user: ?*anyopaque,
};

comptime {
    const ptr = @sizeOf(usize);

    // Field order, checked against the header rather than assumed. Written
    // pointer-width aware so the same asserts hold for the host archive and
    // for the 32-bit ARM one.
    std.debug.assert(@offsetOf(Cell, "prev") == 0);
    std.debug.assert(@offsetOf(Cell, "pin_count") == 12);
    std.debug.assert(@sizeOf(Cell) == 16);

    // `ra8_keycache_cfg_t` is 9 pointers, 5 uint32_t and 2 uint8_t in the
    // header's order. The offsets are spelled out per pointer width rather than
    // derived, because a derivation would happily agree with a reordered
    // header; these numbers only hold for the order C declares.
    std.debug.assert(ptr == 8 or ptr == 4);
    std.debug.assert(@offsetOf(Cfg, "cell_mem") == 0);
    std.debug.assert(@offsetOf(Cfg, "cell_bytes") == ptr);
    std.debug.assert(@offsetOf(Cfg, "cell_count") == ptr + @sizeOf(u32));
    std.debug.assert(@offsetOf(Cfg, "render") == if (ptr == 8) 72 else 40);
    std.debug.assert(@offsetOf(Cfg, "render_ctx") == if (ptr == 8) 80 else 44);
    std.debug.assert(@offsetOf(Cfg, "hash") == if (ptr == 8) 96 else 52);
    std.debug.assert(@sizeOf(Cfg) == if (ptr == 8) 112 else 60);

    // The facade's own fields sit after this, so a wrong size here silently
    // moves them.
    std.debug.assert(@offsetOf(State, "cfg") == 0);
    std.debug.assert(@offsetOf(State, "pb_head") == @sizeOf(Cfg));
    std.debug.assert(@sizeOf(State) == @sizeOf(Cfg) + 10 * @sizeOf(u32));

    std.debug.assert(@sizeOf(View) == 2 * ptr);
}
