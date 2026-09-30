//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The startup zero-fill for regions the reset handler cannot reach (#2901).
//!
//! `Reset_Handler` copies `.data` out of MRAM and zeroes `.bss`, both of
//! which live in SRAM and answer from the first instruction after reset.
//! `.sdram_data` does not: every linker script places it `> SDRAM` as
//! `NOLOAD`, and the external window stays dark until the SDRAM controller
//! bring-up has run. Objects placed there still have static storage duration,
//! so C requires them to read as all-bits-zero before `main()` sees them.
//! This is the fill that makes that true, run at the first moment the window
//! answers.
//!
//! The whole target/host split lives in `sdramSection()`, the same shape
//! `internal/infrastructure/canary.zig` uses: on target the bounds are linker
//! symbols, off target there is no linker script, so a file-static stand-in
//! stands in for the section and the host suite drives that instead.

const builtin = @import("builtin");

/// Whether this build is an image rather than a host test binary.
const on_target = builtin.target.os.tag == .freestanding;

/// The host stand-in for `.sdram_data`, sized as the C sized it.
pub const stand_in = struct {
    pub const bytes: usize = 64;
};

extern var g_ra8_ls_ssdram: u8;
extern var g_ra8_ls_esdram: u8;

var host_window: [stand_in.bytes]u8 = @splat(0);

/// The `.sdram_data` section as a slice: the linker's bounds on target, the
/// stand-in window off it. An image that places nothing in SDRAM links both
/// symbols to the same address, so this comes back empty and the fill is a
/// no-op with no measurable cost.
pub fn sdramSection() []u8 {
    if (comptime !on_target) return &host_window;
    const start: [*]u8 = @ptrCast(&g_ra8_ls_ssdram);
    const end: [*]u8 = @ptrCast(&g_ra8_ls_esdram);
    return start[0 .. @intFromPtr(end) - @intFromPtr(start)];
}

/// The bytes of a half-open `[start, end)` address span, or null when `end`
/// precedes `start`. Half-open is the contract the header states and the host
/// suite pins: the byte AT `end` survives, and an empty span is a success
/// that writes nothing.
pub fn spanLength(start: usize, end: usize) ?usize {
    if (end < start) return null;
    return end - start;
}

/// Clear every byte. Byte-wise, so an unaligned or odd-length span needs no
/// tail case; startup runs it once and it is on no hot path.
pub fn zero(bytes: []u8) void {
    @memset(bytes, 0);
}
