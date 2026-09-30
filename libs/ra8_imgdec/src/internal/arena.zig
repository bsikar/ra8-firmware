//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The seam `ra8_imgdec` reaches `ra8_arena` through.
//!
//! The arena is still C, one ring down. Naming it behind two function pointers
//! keeps the fabric and the mux free of an unresolved extern, so the whole of
//! the decision logic runs in the host suite against a fake.

const vocab = @import("vocab.zig");

pub const RemainingFn = *const fn (arena: ?*anyopaque, out: *u32) u16;
pub const CarveFn = *const fn (
    arena: ?*anyopaque,
    bytes: u32,
    alignment: u32,
    out: *?*anyopaque,
) u16;

/// What the ring below offers. The membrane binds the real `ra8_arena_*`; a
/// test binds whatever it wants to observe.
pub const Ops = struct {
    remaining: RemainingFn,
    carve: CarveFn,
};

/// An `Ops` that refuses everything, for a caller that has no arena at all.
pub const none = Ops{ .remaining = refuseRemaining, .carve = refuseCarve };

fn refuseRemaining(arena: ?*anyopaque, out: *u32) u16 {
    _ = arena;
    out.* = 0;
    return vocab.Err.invalid_state;
}

fn refuseCarve(arena: ?*anyopaque, bytes: u32, alignment: u32, out: *?*anyopaque) u16 {
    _ = arena;
    _ = bytes;
    _ = alignment;
    out.* = null;
    return vocab.Err.invalid_state;
}
