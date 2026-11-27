//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The stb_truetype allocator hooks (RA8FW-1239): the static glyph-scratch
//! arena the STBTT_malloc / STBTT_free macros reach through, ported from
//! apps/shared_libs/reflow/src/ra8_stbtt_alloc.c.
//!
//! The macros carry no context parameter, so the hooks keep the file-global
//! backing store the C shim had: a static arena composed statically over the
//! shared scratch contract (RA8FW-308), with no bind point and no lazy init,
//! because SOUP invokes these hooks directly through the macros.

const abi = @import("internal/abi.zig");
const scratch_mod = @import("internal/scratch.zig");

const Scratch = abi.Scratch;

/// Allocation alignment, bytes: the shared scratch contract's
/// `k_ra8_imgdec_scratch_align`. The C shim pinned this equality with a
/// static_assert; the comptime check below keeps the two honest here.
pub const align_bytes: usize = scratch_mod.Limits.alignment;

/// 3x the 32 KiB worst case: the densest printable-ASCII glyph ('@') of the
/// bundled Literata face at the library's maximum font size accumulates
/// ~32 KiB of stb scratch within a single rasterisation. The margin covers a
/// heavier face or a larger glyph.
pub const arena_bytes: usize = 96 * 1024;

/// What a zero-byte request reserves, so two zero-byte blocks never alias the
/// next real allocation.
pub const zero_request_bytes: usize = 1;

comptime {
    if (align_bytes != 16) @compileError("stb_truetype scratch alignment must match the shared contract (16)");
}

/// Backing store for all stb_truetype scratch. Aligned so every
/// 16-byte-rounded offset yields a 16-byte-aligned pointer.
var s_arena_buf: [arena_bytes]u8 align(align_bytes) linksection(".sdram_data") = undefined;

/// The arena, composed statically rather than through scratch init: the hooks
/// are macros invoked by SOUP and have no bind point to run an init from, and
/// these are exactly the field values init writes for a fresh store.
var s_scratch: Scratch = .{ .base = &s_arena_buf, .cap = arena_bytes };

/// Allocate `bytes` of glyph scratch from the static arena. A zero-byte
/// request reserves one aligned slot rather than being refused, so a
/// zero-byte block is still an address of its own. Returns null when the
/// request does not fit the remaining capacity; stb_truetype tolerates that
/// and skips the offending glyph.
pub export fn ra8_stbtt_malloc(bytes: usize) callconv(.c) ?*anyopaque {
    const want = if (bytes == 0) zero_request_bytes else bytes;
    return @ptrCast(scratch_mod.alloc(&s_scratch, want));
}

/// Release one block; the arena fully drains (the cursor rewinds to the base)
/// when the last live block goes, which is the shared scratch's auto-reset.
pub export fn ra8_stbtt_free(ptr: ?*anyopaque) callconv(.c) void {
    scratch_mod.free(&s_scratch, @ptrCast(ptr));
}

/// Worst-case arena high-water mark observed since boot, in bytes.
pub export fn ra8_stbtt_alloc_high_water() callconv(.c) usize {
    return scratch_mod.highWater(&s_scratch);
}
