//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the MIPI CSI receive-status and module-info accessors declared
//! in ra8_mipi_csi_api.h (RA8FW-636), replacing that part of ra8_mipi_csi.c.

const common = @import("abi_common.zig");
const info = @import("internal/mipi_csi_info.zig");

const tag = "MIPI_CSI";
const base_addr: usize = 0x40347000;

const Csi = struct {
    pub fn read32(_: Csi, off: u16) u32 {
        return @as(*volatile u32, @ptrFromInt(base_addr + off)).*;
    }
    pub fn write32(_: Csi, off: u16, value: u32) void {
        @as(*volatile u32, @ptrFromInt(base_addr + off)).* = value;
    }
    pub fn err(_: Csi, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

const csi = Csi{};

export fn ra8_mipi_csi_get_status(out_mask: ?*u32) u16 {
    return info.readReg(csi, info.off_rxst, out_mask);
}

/// Only RXSC.RACTDETC (bit 17) is W1C; hardware drops the other bits.
export fn ra8_mipi_csi_clear_status(mask: u32) u16 {
    csi.write32(info.off_rxsc, mask);
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_set_rx_irq_enable(mask: u32) u16 {
    csi.write32(info.off_rxie, mask);
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_get_module_irq_status(out_mask: ?*u32) u16 {
    return info.readReg(csi, info.off_mist, out_mask);
}

export fn ra8_mipi_csi_get_module_info(out: ?*info.ModuleInfo) u16 {
    return info.getModuleInfo(csi, out);
}
