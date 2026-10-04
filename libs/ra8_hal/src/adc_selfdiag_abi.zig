//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ADC_B self-diagnosis half of ra8_adc.h (RA8FW-610).
//! adc_internal.h stays in C for adc.c.

const common = @import("abi_common.zig");
const ad = @import("internal/adc_selfdiag.zig");

const tag = "ADC";

const Hw = struct {
    fn ptr(off: usize) *volatile u32 {
        return @ptrFromInt(ad.base + off);
    }
    pub fn read32(_: Hw, off: usize) u32 {
        return ptr(off).*;
    }
    pub fn write32(_: Hw, off: usize, value: u32) void {
        ptr(off).* = value;
    }
    pub fn err(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn errVal(_: Hw, code: u16) void {
        common.ra8_log_emit_error_val(tag, "Error", code);
    }
};

export fn ra8_adc_self_diagnose(mode: u8, out_code: ?*u16, out_pass: ?*bool) u16 {
    return ad.selfDiagnose(Hw{}, mode, out_code, out_pass);
}

export fn ra8_adc_read_internal_channel(chan: u8, out_raw: ?*u16) u16 {
    return ad.readInternalChannel(Hw{}, chan, out_raw);
}
