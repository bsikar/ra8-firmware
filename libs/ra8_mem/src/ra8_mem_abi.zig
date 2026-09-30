//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C membrane for the `ra8_mem` slab: every symbol `inc/ra8_slab.h`
//! declares, and nothing else. That header is unchanged, so the host suite,
//! `mem_subsystem` and the rest of `libs/ra8_mem` link against this archive
//! without knowing the bodies moved.
//!
//! Raw pointers stop here. Everything past this file works in slices, typed
//! enums and non-optional references.

const std = @import("std");

const slab = @import("internal/slab.zig");
const vocab = @import("internal/vocab.zig");

const Err = vocab.Err;

comptime {
    // `ra8_err.h` spells `ra8_err_t` as `enum : uint16_t`, so the return width
    // has to match on every target the archive is built for.
    std.debug.assert(@sizeOf(Err) == 2);
}

// ---------------------------------------------------------------------------
// ra8_slab.h
// ---------------------------------------------------------------------------

export fn ra8_slab_init(
    handle: ?*slab.Slab,
    buffer: ?*anyopaque,
    buffer_bytes: u32,
    cell_bytes: u32,
) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const pool = buffer orelse return Err.null_ptr.code();
    return slab.init(self, @ptrCast(pool), buffer_bytes, cell_bytes).code();
}

export fn ra8_slab_alloc(handle: ?*slab.Slab, out_cell: ?*?*anyopaque) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const dst = out_cell orelse return Err.null_ptr.code();
    return slab.alloc(self, dst).code();
}

export fn ra8_slab_free(handle: ?*slab.Slab, cell: ?*anyopaque) u16 {
    const self = handle orelse return Err.null_ptr.code();
    const target = cell orelse return Err.null_ptr.code();
    return slab.free(self, target).code();
}

export fn ra8_slab_stats(handle: ?*const slab.Slab, out_free: ?*u32, out_total: ?*u32) u16 {
    const self = handle orelse return Err.null_ptr.code();
    return slab.stats(self, out_free, out_total).code();
}
