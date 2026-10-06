//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_wdt_ofs_get / ra8_wdt_ofs_reader_set (RA8FW-888). Owns
//! the OFSm reader hook, file-local as the C static was. The driver
//! never writes OFSm; reads go through the hook so tests and non-secure
//! callers can supply their own transport. Logic lives in
//! internal/wdt_ofs.zig.

const common = @import("abi_common.zig");
const ofs = @import("internal/wdt_ofs.zig");

const tag = "WDT";
const instance_count: u8 = 2;

/// `ra8_wdt_ofs_reader_fn_t` (inc/ra8_wdt.h).
const Reader = *const fn (ofs_addr: usize, out_word: ?*u32) callconv(.c) u16;

/// Option-setting words are ordinary readable memory in the extra-MRAM
/// configuration area (HUM Ch 7.1 Figure 7.1, p 279).
fn defaultReader(ofs_addr: usize, out_word: ?*u32) callconv(.c) u16 {
    const out = out_word orelse return common.k_ra8_err_null_ptr;
    const src: *const volatile u32 = @ptrFromInt(ofs_addr);
    out.* = src.*;
    return common.k_ra8_ok;
}

var reader: Reader = defaultReader;

export fn ra8_wdt_ofs_reader_set(hook: ?Reader) u16 {
    reader = hook orelse defaultReader;
    return common.k_ra8_ok;
}

/// OFS3_SEL, then OFS3_SEC, then OFS3; the first failure wins.
fn readWdt1(out: *u32) u16 {
    var sel: u32 = 0;
    var sec: u32 = 0;
    var nonsec: u32 = 0;
    var e = reader(ofs.ofs3_sel_addr, &sel);
    if (e == common.k_ra8_ok) e = reader(ofs.ofs3_sec_addr, &sec);
    if (e == common.k_ra8_ok) e = reader(ofs.ofs3_addr, &nonsec);
    if (e != common.k_ra8_ok) return e;
    if (!ofs.selLegal(sel)) {
        common.ra8_log_emit_error(tag, "OFS3_SEL holds a prohibited mixed encoding");
        return common.k_ra8_err_invalid_state;
    }
    out.* = ofs.mux(sel, sec, nonsec);
    return common.k_ra8_ok;
}

export fn ra8_wdt_ofs_get(which: u8, out: ?*ofs.Decoded) u16 {
    const dst = out orelse {
        common.ra8_log_emit_error(tag, "out must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (which >= instance_count) return common.k_ra8_err_invalid_arg;
    var word: u32 = 0;
    const e = if (which == 0) reader(ofs.ofs0_addr, &word) else readWdt1(&word);
    if (e != common.k_ra8_ok) return e;
    dst.* = ofs.decode(word);
    return common.k_ra8_ok;
}
