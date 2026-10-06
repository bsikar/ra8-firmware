//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! WDT timing helpers (RA8FW-889, was part of ra8_wdt.c): WDTCR.TOPS to
//! counter cycles and WDTCR.CKS to the PCLKB divisor. HUM Ch 27.2.2
//! "WDTCR : WDT Control Register", pp 1258-1259. Exports live in
//! src/wdt_timing_abi.zig.

/// TOPS[1:0]: 0 -> 1024, 1 -> 4096, 2 -> 8192, 3 -> 16384 cycles.
pub fn cycles(tops: u8) ?u16 {
    return switch (tops) {
        0 => 1024,
        1 => 4096,
        2 => 8192,
        3 => 16384,
        else => null,
    };
}

/// CKS[3:0]: only 0x1, 0x4, 0xF, 0x6, 0x7 and 0x8 are legal.
pub fn divisor(cks: u8) ?u16 {
    return switch (cks) {
        0x1 => 4,
        0x4 => 64,
        0xF => 128,
        0x6 => 512,
        0x7 => 2048,
        0x8 => 8192,
        else => null,
    };
}

/// PCLKB cycles to underflow; at most 16384 * 8192, so it fits u32.
pub fn total(tops: u8, cks: u8) ?u32 {
    const c = cycles(tops) orelse return null;
    const d = divisor(cks) orelse return null;
    return @as(u32, c) * d;
}
