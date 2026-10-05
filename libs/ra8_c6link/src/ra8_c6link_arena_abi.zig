//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the decode arena: the four `priv_c6link_arena_*` symbols that
//! `src/ra8_c6link_internal.h` declares and the Zig RPC layer calls.
//!
//! Only the arena's four fields of `ra8_c6link_t` are touched. They sit after
//! seven pointer-sized fields (the four-pointer transport, the event and rx
//! callbacks and their context), which the comptime check below pins.

const arena = @import("internal/arena.zig");

/// The leading part of `ra8_c6link_t`, up to and including the arena fields.
const LinkHead = extern struct {
    head: [7]?*anyopaque,
    arena: arena.Arena,
};

comptime {
    const ptr = @sizeOf(usize);
    if (@offsetOf(LinkHead, "arena") != 7 * ptr) @compileError("ra8_c6link_t arena offset drifted");
    if (@offsetOf(arena.Arena, "bytes") != ptr) @compileError("arena_bytes offset drifted");
    if (@offsetOf(arena.Arena, "last") != ptr + 8) @compileError("arena_last offset drifted");
}

const AllocFn = *const fn (?*anyopaque, usize) callconv(.c) ?*anyopaque;
const FreeFn = *const fn (?*anyopaque, ?*anyopaque) callconv(.c) void;

/// protobuf-c's `ProtobufCAllocator`.
const Allocator = extern struct {
    alloc: ?AllocFn,
    free: ?FreeFn,
    allocator_data: ?*anyopaque,
};

fn state(ctx: ?*anyopaque) ?*arena.Arena {
    const link: *LinkHead = @ptrCast(@alignCast(ctx orelse return null));
    return &link.arena;
}

/// `priv_c6link_arena_alloc`: the allocator's `alloc` row.
pub export fn priv_c6link_arena_alloc(ctx: ?*anyopaque, size: usize) callconv(.c) ?*anyopaque {
    const a = state(ctx) orelse return null;
    return @ptrCast(a.alloc(size));
}

/// `priv_c6link_arena_free`: the allocator's `free` row.
pub export fn priv_c6link_arena_free(ctx: ?*anyopaque, pointer: ?*anyopaque) callconv(.c) void {
    const a = state(ctx) orelse return;
    a.free(pointer);
}

/// `priv_c6link_arena_reset`: empty the link's arena; null is ignored.
pub export fn priv_c6link_arena_reset(link: ?*anyopaque) callconv(.c) void {
    const a = state(link) orelse return;
    a.reset();
}

/// `priv_c6link_arena_bind`: point an allocator descriptor at the link's arena.
pub export fn priv_c6link_arena_bind(out: ?*Allocator, link: ?*anyopaque) callconv(.c) void {
    const o = out orelse return;
    if (link == null) return;
    o.* = .{
        .alloc = priv_c6link_arena_alloc,
        .free = priv_c6link_arena_free,
        .allocator_data = link,
    };
}
