//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_mstp.h (RA8FW-770); replaces ra8_mstp.c. Logic lives in
//! internal/mstp.zig over the Hw below. The MSTP block sits at 0x40203000
//! (Secure alias) or its IDAU bit[28]=1 Non-secure alias when the archive's
//! `periph_ns_alias` option is set; the PSAR reads stay Secure-only. Hosted
//! builds poll through the fake-MMIO wait seam keyed on the polled MSTPCR
//! register, so the C suites' fault injection is unchanged.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const options = @import("ra8_hal_options");
const alias = @import("internal/periph_alias.zig");
const mstp = @import("internal/mstp.zig");

const tag: [*:0]const u8 = "MSTP";
const hosted = builtin.os.tag != .freestanding;

/// The r_mstp_regs_t block: the Secure alias, or the NS alias under the
/// option (the C picked the same base with RA8_PERIPH_NS_ALIAS).
const base: usize = alias.of(0x4020_3000, options.periph_ns_alias);

const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

const Hw = struct {
    pub fn regPtr(reg: u8) *volatile u32 {
        return @ptrFromInt(base + 4 * @as(usize, reg));
    }
    pub fn read(_: Hw, reg: u8) u32 {
        return regPtr(reg).*;
    }
    pub fn write(_: Hw, reg: u8, value: u32) void {
        regPtr(reg).* = value;
    }
    pub fn psar(_: Hw, reg: u8) u32 {
        const addr = mstp.psar_addrs[reg];
        if (addr == 0) return 0;
        return @as(*volatile u32, @ptrFromInt(addr)).*;
    }
    pub fn poll(_: Hw, reg: u8, iter: u32, cond: bool) bool {
        if (!hosted) return cond;
        return seam.ra8_fake_mmio_wait_eval(regPtr(reg), iter, cond);
    }
    pub fn info(_: Hw, message: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, message);
    }
    pub fn err(_: Hw, message: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, message);
    }
    pub fn errVal(_: Hw, message: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_error_val(tag, message, value);
    }
};

/// The one private gate, as the C kept `s_refcount` file-static.
var s_gate: mstp.Gate = .{};

export fn ra8_mstp_init() u16 {
    return mstp.init(Hw{}, &s_gate);
}

export fn ra8_mstp_enable(id: u16) u16 {
    return mstp.enable(Hw{}, &s_gate, id);
}

export fn ra8_mstp_disable(id: u16) u16 {
    return mstp.disable(Hw{}, &s_gate, id);
}

export fn ra8_mstp_get_refcount(id: u16, out_ref: ?*u8) u16 {
    return mstp.refcount(Hw{}, &s_gate, id, out_ref);
}

export fn ra8_mstp_is_stopped(id: u16, out_stopped: ?*bool) u16 {
    return mstp.stopped(Hw{}, &s_gate, id, out_stopped);
}

export fn priv_ra8_mstp_wait_reg_settle_internal(reg: u8, expect: u32, care_mask: u32) u16 {
    return mstp.waitRegSettle(Hw{}, reg, expect, care_mask);
}

export fn priv_ra8_mstp_ns_mask_internal(reg: u8) u32 {
    return mstp.nsMask(Hw{}, reg);
}
