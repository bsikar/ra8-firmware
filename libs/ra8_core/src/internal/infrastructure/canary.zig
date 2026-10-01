//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The stack-overflow sentinel.
//!
//! The linker reserves 32 bytes just below the stack top and names both ends
//! (`.stack_canary` in each board's `ld/linker_script.ld`). Boot fills that
//! region with a known pattern; anything that later finds a word changed has
//! proof the stack ran past its own floor.
//!
//! The whole target/host split lives in `region()`. On a host binary there
//! are no linker symbols and no stack to overflow, so the region is empty,
//! and seeding an empty region and finding an empty region intact both fall
//! out on their own. The C carried the same `#ifndef RA8_OFF_TARGET` twice,
//! once around each loop.

const builtin = @import("builtin");

/// Whether this build is an image rather than a host test binary.
const on_target = builtin.target.os.tag == .freestanding;

/// The word written into every slot of the region.
pub const sentinel = struct {
    pub const pattern: u32 = 0xDEAD_BEEF;
};

extern var g_ra8_ls_stack_canary_start: u32;
extern var g_ra8_ls_stack_canary_end: u32;

/// The canary region as a slice; empty off target, where it does not exist.
pub fn region() []u32 {
    if (comptime !on_target) return &.{};
    const start: [*]u32 = @ptrCast(&g_ra8_ls_stack_canary_start);
    const end: [*]u32 = @ptrCast(&g_ra8_ls_stack_canary_end);
    const words = (@intFromPtr(end) - @intFromPtr(start)) / @sizeOf(u32);
    return start[0..words];
}

/// Write the pattern into every word. One-shot init, not thread-safe.
pub fn seed() void {
    @memset(region(), sentinel.pattern);
}

/// Whether every word still reads back as the pattern. Read-only.
pub fn intact() bool {
    for (region()) |word| {
        if (word != sentinel.pattern) return false;
    }
    return true;
}
