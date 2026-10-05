//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the RMAC Clause 22 PHY helpers (RA8FW-744). The MDIO
//! primitives stay in ra8_rmac.c; the register snapshots stay in
//! ra8_rmac_mgmt.c.

const common = @import("abi_common.zig");
const phy = @import("internal/rmac_phy.zig");

const tag = "RMAC";

extern fn ra8_rmac_mdio_c22_read(port: u8, phy_addr: u8, reg: u8, out: *u16) u16;
extern fn ra8_rmac_mdio_c22_write(port: u8, phy_addr: u8, reg: u8, value: u16) u16;

comptime {
    if (@sizeOf(phy.Link) != 2 or @offsetOf(phy.Link, "speed") != 1) @compileError("Link must match ra8_rmac_phy_link_t");
}

const C = struct {
    pub fn read(_: C, port: u8, addr: u8, reg: u8, out: *u16) u16 {
        return ra8_rmac_mdio_c22_read(port, addr, reg, out);
    }
    pub fn write(_: C, port: u8, addr: u8, reg: u8, value: u16) u16 {
        return ra8_rmac_mdio_c22_write(port, addr, reg, value);
    }
    pub fn logError(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

export fn ra8_rmac_phy_reset(port: u8, phy_addr: u8) u16 {
    return phy.reset(C{}, port, phy_addr);
}

export fn ra8_rmac_phy_set_advertise(port: u8, phy_addr: u8, capabilities: u16) u16 {
    return phy.setAdvertise(C{}, port, phy_addr, capabilities);
}

export fn ra8_rmac_phy_auto_neg_start(port: u8, phy_addr: u8) u16 {
    return phy.autoNegStart(C{}, port, phy_addr);
}

export fn ra8_rmac_phy_auto_neg_wait(port: u8, phy_addr: u8, timeout_ms: u32, out: ?*phy.Link) u16 {
    return phy.autoNegWait(C{}, port, phy_addr, timeout_ms, out);
}

export fn ra8_rmac_phy_link_status(port: u8, phy_addr: u8, out: ?*phy.Link) u16 {
    return phy.linkStatus(C{}, port, phy_addr, out);
}
