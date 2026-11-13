//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The libwebp-shaped face of the shared decoder scratch contract, ported
//! from src/ra8_webp_arena.c (RA8FW-1209, slice 1). RA8FW-308 keeps one copy
//! of the bump policy, in the ra8_imgdec scratch module these hooks forward
//! to; what lives here is only the part libwebp forces: the bound-arena slot,
//! because WebPSafeMalloc and friends take no context argument.

pub const Scratch = @import("scratch_mirror.zig").Scratch;

const scratch_ok: u16 = 0;

extern fn ra8_imgdec_scratch_init(scratch: ?*Scratch, store: ?*anyopaque, cap: usize) callconv(.c) u16;
extern fn ra8_imgdec_scratch_reset(scratch: ?*Scratch) callconv(.c) void;
extern fn ra8_imgdec_scratch_alloc(scratch: ?*Scratch, bytes: usize) callconv(.c) ?*anyopaque;
extern fn ra8_imgdec_scratch_calloc(scratch: ?*Scratch, count: usize, size: usize) callconv(.c) ?*anyopaque;
extern fn ra8_imgdec_scratch_free(scratch: ?*Scratch, ptr: ?*anyopaque) callconv(.c) void;

/// Currently-bound arena, or null when no decode is in flight.
var s_arena: ?*Scratch = null;

/// Bind `arena` as the active scratch and reset it to empty (a bind is a
/// re-init). A null pointer unbinds.
pub export fn ra8_webp_arena_bind(arena: ?*Scratch) callconv(.c) void {
    s_arena = arena;
    const slot = arena orelse return;
    // An unusable descriptor (no store, or no room in it) is still emptied, so
    // the hooks refuse it later the same way they always did rather than
    // drawing from whatever the last decode left behind.
    if (ra8_imgdec_scratch_init(slot, @ptrCast(slot.base), slot.cap) != scratch_ok) {
        ra8_imgdec_scratch_reset(slot);
    }
}

/// Clear the active-arena slot; subsequent allocations fail with null.
pub export fn ra8_webp_arena_unbind() callconv(.c) void {
    s_arena = null;
}

/// libwebp WebPSafeMalloc hook: bump `n` bytes from the bound arena.
pub export fn ra8_webp_arena_malloc(n: usize) callconv(.c) ?*anyopaque {
    return ra8_imgdec_scratch_alloc(s_arena, n);
}

/// libwebp WebPSafeCalloc hook: zeroed `nmemb * size` bytes from the arena.
pub export fn ra8_webp_arena_calloc(nmemb: usize, size: usize) callconv(.c) ?*anyopaque {
    return ra8_imgdec_scratch_calloc(s_arena, nmemb, size);
}

/// libwebp WebPSafeFree hook; null is ignored by the scratch contract.
pub export fn ra8_webp_arena_free(p: ?*anyopaque) callconv(.c) void {
    ra8_imgdec_scratch_free(s_arena, p);
}
