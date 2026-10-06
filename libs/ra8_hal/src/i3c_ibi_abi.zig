//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for I3C HDR mode, IBI and target open (RA8FW-826). The register
//! logic is in internal/i3c_ibi.zig.

const common = @import("abi_common.zig");
const ibi = @import("internal/i3c_ibi.zig");
const ccc = @import("internal/i3c_ccc.zig");
const ctl = @import("internal/i3c_ctl.zig");

const tag = "I3C";

comptime {
    if (@sizeOf(ibi.Ibi) != 12) @compileError("ra8_i3c_ibi_t is 12 bytes");
}

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

export fn priv_ra8_i3c_internal_hdr_mode_invalid(sdr: u32, ddr: u32, ts: u32, mode: u32) bool {
    return ibi.hdrModeInvalid(sdr, ddr, ts, mode);
}

export fn ra8_i3c_set_hdr_mode(target_addr: u8, mode: u8) u16 {
    if (target_addr > ccc.addr_mask) return common.k_ra8_err_invalid_arg;
    if (ibi.hdrModeInvalid(ibi.hdr_sdr, ibi.hdr_ddr, ibi.hdr_ts, mode)) return common.k_ra8_err_invalid_arg;
    ibi.setHdr(Mmio{}, target_addr, mode);
    return common.k_ra8_ok;
}

export fn ra8_i3c_ibi_enable(target_addr: u8) u16 {
    if (target_addr > ccc.addr_mask) return common.k_ra8_err_invalid_arg;
    ibi.ibiEnable(Mmio{});
    return common.k_ra8_ok;
}

export fn ra8_i3c_target_open(static_addr: u8) u16 {
    if (static_addr > ccc.addr_mask) return common.k_ra8_err_invalid_arg;
    ibi.targetOpen(Mmio{}, static_addr);
    return common.k_ra8_ok;
}

export fn ra8_i3c_ibi_read(out_ibi: ?*ibi.Ibi) u16 {
    const out = out_ibi orelse {
        common.ra8_log_emit_error(tag, "out_ibi must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (!ibi.ibiRead(Mmio{}, out)) return common.k_ra8_err_no_data;
    return common.k_ra8_ok;
}
