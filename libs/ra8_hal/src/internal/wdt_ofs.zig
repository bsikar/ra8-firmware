//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! WDT OFSm read-only decode (RA8FW-888, was part of ra8_wdt.c). In
//! auto-start mode the seven WDT fields come from OFS0 (WDT0) or from
//! OFS3 / OFS3_SEC picked field by field by OFS3_SEL (WDT1). HUM Ch 7.2.1,
//! 7.2.6, 7.2.7 and Ch 27.3.8 Table 27.5. Exports live in
//! src/wdt_ofs_abi.zig.

pub const ofs0_addr: usize = 0x02C9F040;
pub const ofs3_addr: usize = 0x12C9F4C4;
pub const ofs3_sec_addr: usize = 0x02C9F0C4;
pub const ofs3_sel_addr: usize = 0x02C9F124;

const Field = struct { shift: u5, mask: u32 };
const strt: Field = .{ .shift = 17, .mask = 0x1 };
const tops: Field = .{ .shift = 18, .mask = 0x3 };
const cks: Field = .{ .shift = 20, .mask = 0xF };
const rpes: Field = .{ .shift = 24, .mask = 0x3 };
const rpss: Field = .{ .shift = 26, .mask = 0x3 };
const rstirqs: Field = .{ .shift = 28, .mask = 0x1 };
const stpctl: Field = .{ .shift = 30, .mask = 0x1 };

/// Union of the seven fields (`k_ra8_wdt_ofs_field_mask`).
pub const field_mask: u32 = 0x5FFE0000;

comptime {
    var m: u32 = 0;
    for ([_]Field{ strt, tops, cks, rpes, rpss, rstirqs, stpctl }) |f| m |= f.mask << f.shift;
    if (m != field_mask) @compileError("WDT OFS field table drifted from field_mask");
}

fn get(word: u32, f: Field) u8 {
    return @intCast((word >> f.shift) & f.mask);
}

/// Mirrors `ra8_wdt_ofs_decoded_t` (inc/ra8_wdt.h): the six
/// `ra8_wdt_cfg_t` bytes, then start mode and the auto-start flag.
pub const Decoded = extern struct {
    timeout: u8,
    clock_div: u8,
    window_start: u8,
    window_end: u8,
    on_expiry: u8,
    stop_in_sleep: u8,
    start_mode: u8,
    auto_start: bool,
};

/// A multi-bit OFS3_SEL field must be all secure or all non-secure;
/// mixed encodings are prohibited (HUM 7.2.7, p 289).
pub fn selLegal(sel: u32) bool {
    for ([_]Field{ tops, cks, rpes, rpss }) |f| {
        const v = (sel >> f.shift) & f.mask;
        if (v != 0 and v != f.mask) return false;
    }
    return true;
}

/// SEL bit 1 takes the field from OFS3 (non-secure), 0 from OFS3_SEC.
pub fn mux(sel: u32, sec: u32, nonsec: u32) u32 {
    const pick = sel & field_mask;
    return (nonsec & pick) | (sec & ~pick);
}

pub fn decode(ofsm: u32) Decoded {
    const start = get(ofsm, strt);
    return .{
        .timeout = get(ofsm, tops),
        .clock_div = get(ofsm, cks),
        .window_start = get(ofsm, rpss),
        .window_end = get(ofsm, rpes),
        .on_expiry = get(ofsm, rstirqs),
        .stop_in_sleep = get(ofsm, stpctl),
        .start_mode = start,
        .auto_start = start == 0,
    };
}
