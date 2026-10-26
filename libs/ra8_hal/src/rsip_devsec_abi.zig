//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the RSIP device-security surface (RA8FW-1064); replaces
//! ra8_rsip_devsec.c. Logic lives in internal/rsip_devsec.zig. The guarded
//! command path round-trips the host register fake and exists only on hosted
//! builds (the C keyed the same split on RA8_INSECURE_STUB_CRYPTO ||
//! RA8_OFF_TARGET); freestanding image builds are fail-closed: every entry
//! point returns k_ra8_err_not_supported and writes no fabricated state. The
//! RSIP window sits at 0x403B0000.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const devsec = @import("internal/rsip_devsec.zig");

const tag: [*:0]const u8 = "RSIP";
/// Hosted builds keep the guarded command path; freestanding is fail-closed.
const guarded = builtin.os.tag != .freestanding;

fn regPtr(off: usize) *volatile u32 {
    return @ptrFromInt(devsec.base + off);
}

const Hw = struct {
    pub fn read32(_: Hw, off: usize) u32 {
        return regPtr(off).*;
    }
    pub fn write32(_: Hw, off: usize, value: u32) void {
        regPtr(off).* = value;
    }
};

fn nullErr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

export fn ra8_rsip_life_get(out: ?*u32) u16 {
    if (out == null) return nullErr("life_get: out must not be nullptr");
    return devsec.lifeGet(Hw{}, guarded, out.?);
}

export fn ra8_rsip_life_advance(state: u32) u16 {
    return devsec.lifeAdvance(Hw{}, guarded, state);
}

export fn ra8_rsip_debug_level_get(out: ?*u8) u16 {
    if (out == null) return nullErr("debug_level_get: out must not be nullptr");
    return devsec.debugLevelGet(Hw{}, guarded, out.?);
}

export fn ra8_rsip_debug_level_set(level: u8) u16 {
    return devsec.debugLevelSet(Hw{}, guarded, level);
}

export fn ra8_rsip_tamper_enable(sources: u32) u16 {
    return devsec.tamperEnable(Hw{}, guarded, sources);
}

export fn ra8_rsip_tamper_status(out: ?*u32) u16 {
    if (out == null) return nullErr("tamper_status: out must not be nullptr");
    return devsec.tamperStatus(Hw{}, guarded, out.?);
}

export fn ra8_rsip_tamper_ack(mask: u32) u16 {
    return devsec.tamperAck(Hw{}, guarded, mask);
}

export fn ra8_rsip_dpa_arm(enable: bool) u16 {
    return devsec.dpaArm(Hw{}, guarded, enable);
}
