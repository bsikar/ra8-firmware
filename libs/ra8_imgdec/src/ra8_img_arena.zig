//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The stb_image allocator hooks (RA8FW-1238): the module-global bound slot
//! the single-TU stb decoder reaches its bump arena through, ported from
//! apps/shared_libs/reflow/src/ra8_img_arena.c.
//!
//! stb_image_impl.c builds as one TU with no context to carry a user pointer
//! through, so its STBI_MALLOC / STBI_FREE / STBI_REALLOC_SIZED macros call
//! these five entry points and the app binds its arena before decoding. The
//! hooks are pure forwarding: the bump logic lives one door down in
//! internal/scratch.zig (RA8FW-308), and a zero-size request reserves one
//! byte so stb never sees a null from a "successful" allocation.

const scratch_mod = @import("internal/scratch.zig");

pub const Scratch = @import("internal/abi.zig").Scratch;
const Err = @import("internal/vocab.zig").Err;

/// The reserve `ra8_imgdec_scratch_alloc` refuses (zero bytes) becomes a
/// one-byte block, so a zero-size stb request still returns a non-null pointer.
pub const zero_request_bytes: usize = 1;

/// The arena stb allocates from, bound with `ra8_img_arena_bind`.
var s_arena: ?*Scratch = null;

pub export fn ra8_img_arena_bind(arena: ?*Scratch) callconv(.c) void {
    s_arena = arena;
    if (arena) |a| {
        if (a.base) |base| {
            if (scratch_mod.init(a, base[0..a.cap]) != Err.ok) scratch_mod.reset(a);
        } else {
            scratch_mod.reset(a);
        }
    }
}

pub export fn ra8_img_arena_unbind() callconv(.c) void {
    s_arena = null;
}

pub export fn ra8_img_arena_malloc(bytes: usize) callconv(.c) ?*anyopaque {
    const want = if (bytes == 0) zero_request_bytes else bytes;
    const arena = s_arena orelse return null;
    return @ptrCast(scratch_mod.alloc(arena, want));
}

pub export fn ra8_img_arena_free(ptr: ?*anyopaque) callconv(.c) void {
    const arena = s_arena orelse return;
    scratch_mod.free(arena, @ptrCast(ptr));
}

pub export fn ra8_img_arena_realloc_sized(
    ptr: ?*anyopaque,
    old_bytes: usize,
    new_bytes: usize,
) callconv(.c) ?*anyopaque {
    if (new_bytes == 0) return ra8_img_arena_malloc(0);
    const arena = s_arena orelse return null;
    return @ptrCast(scratch_mod.realloc(arena, @ptrCast(ptr), old_bytes, new_bytes));
}
