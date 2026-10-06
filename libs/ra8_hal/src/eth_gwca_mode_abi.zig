//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_eth_gwca_set_operation_mode and ra8_eth_gwca_axi_init
//! (RA8FW-848). Logic lives in internal/eth_gwca_mode.zig.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const m = @import("internal/eth_gwca_mode.zig");

const tag = "ETHGWC";
/// `k_ra8_gwca0_base_addr` and the GWCA offsets (ra8_ether_regs.h).
const gwca0_base: usize = 0x403CE000;
const off_gwmc: usize = 0x0000;
const off_gwms: usize = 0x0004;
const off_gwarirm: usize = 0x0380;

/// Host builds go through the C fake-MMIO wait seam (ra8_hw_err.h) so the C
/// suites can hold GWMS or GWARIRM stuck. Freestanding builds never see it.
const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

const Hw = struct {
    pub fn eval(_: Hw, reg: *volatile u32, iter: u32, cond: bool) bool {
        return if (hosted) seam.ra8_fake_mmio_wait_eval(reg, iter, cond) else cond;
    }
    pub fn logError(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

fn gwcaReg(off: usize) *volatile u32 {
    return @ptrFromInt(gwca0_base + off);
}

export fn ra8_eth_gwca_set_operation_mode(mode: u32) u16 {
    return m.setMode(Hw{}, gwcaReg(off_gwmc), gwcaReg(off_gwms), mode);
}

export fn ra8_eth_gwca_axi_init() u16 {
    return m.axiInit(Hw{}, gwcaReg(off_gwarirm));
}
