//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The CPU0 <-> CPU1 shared SRAM window, described rather than programmed.
//! The boot MPU is what actually marks the window Normal non-cacheable, and it
//! only does so under `RA8_BOOT_ENABLE_CACHE_MPU`, so the descriptor reports
//! the flag the running build was compiled with instead of a constant `true`.

const build_config = @import("build_config");
const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// Where the shared window sits and how far it runs.
pub const Window = struct {
    pub const base: usize = 0x2210_0000;
    pub const size_bytes: u32 = 0x9_0000;
};

/// `ra8_board_shared_ram_t`.
pub const SharedRam = extern struct {
    base: ?*anyopaque,
    size_bytes: u32,
    non_cacheable: bool,
};

/// Whether this build's boot MPU marks the window Normal non-cacheable.
pub const non_cacheable: bool = build_config.boot_cache_mpu;

/// Fill @p out with the shared-window descriptor.
pub fn describe(out: ?*SharedRam) u32 {
    const dst = out orelse return Err.null_ptr;
    dst.* = .{
        .base = @ptrFromInt(Window.base),
        .size_bytes = Window.size_bytes,
        .non_cacheable = non_cacheable,
    };
    return Err.ok;
}
