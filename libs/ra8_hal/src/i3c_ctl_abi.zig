//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the I3C address, bus-enable, status and stop entry points
//! (RA8FW-818). The register logic is in internal/i3c_ctl.zig.

const common = @import("abi_common.zig");
const ctl = @import("internal/i3c_ctl.zig");

const tag = "I3C";
/// `ra8_mstp_t` k_ra8_mstp_i3c: (k_ra8_mstp_reg_b << 8) | 4 (inc/ra8_mstp_regs.h).
const mstp_i3c: u16 = (1 << 8) | 4;

extern fn ra8_mstp_disable(id: u16) u16;

const Mmio = struct {
    pub fn read32(_: Mmio, off: usize) u32 {
        const p: *volatile u32 = @ptrFromInt(ctl.base + off);
        return p.*;
    }
    pub fn write32(_: Mmio, off: usize, v: u32) void {
        const p: *volatile u32 = @ptrFromInt(ctl.base + off);
        p.* = v;
    }
};

export fn ra8_i3c_set_address(addr: u32) u16 {
    const word = ctl.msdvadWord(addr) orelse return common.k_ra8_err_invalid_arg;
    (Mmio{}).write32(ctl.off_msdvad, word);
    return common.k_ra8_ok;
}

export fn ra8_i3c_bus_enable(enable: bool) u16 {
    ctl.busEnable(Mmio{}, enable);
    return common.k_ra8_ok;
}

export fn ra8_i3c_get_status(out_mask: ?*u32) u16 {
    const out = out_mask orelse {
        common.ra8_log_emit_error(tag, "out_mask must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    out.* = (Mmio{}).read32(ctl.off_inst);
    return common.k_ra8_ok;
}

export fn ra8_i3c_clear_status(mask: u32) u16 {
    ctl.clearStatus(Mmio{}, mask);
    return common.k_ra8_ok;
}

export fn ra8_i3c_enter_stop() u16 {
    ctl.stop(Mmio{});
    return ra8_mstp_disable(mstp_i3c);
}
