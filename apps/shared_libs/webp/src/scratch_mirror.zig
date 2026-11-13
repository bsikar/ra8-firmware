//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Layout mirror of `ra8_imgdec_scratch_t` (libs/ra8_imgdec/inc/
//! ra8_imgdec_scratch.h): the shared decoder scratch record, kept as a
//! comptime-checked mirror so the webp seam passes the real arena record
//! across the C ABI without importing across package roots.

const std = @import("std");

pub const Scratch = extern struct {
    base: ?[*]u8 = null,
    cap: usize = 0,
    offset: usize = 0,
    live: usize = 0,
    high_water: usize = 0,
};

comptime {
    if (@sizeOf(Scratch) != 5 * @sizeOf(usize)) {
        @compileError("ra8_webp: Scratch mirror layout mismatch vs ra8_imgdec_scratch_t");
    }
    if (@offsetOf(Scratch, "high_water") != 4 * @sizeOf(usize)) {
        @compileError("ra8_webp: Scratch mirror high_water offset mismatch");
    }
}
