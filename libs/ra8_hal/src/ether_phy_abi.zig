//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the Clause-22 Ethernet PHY (internal/ether_phy.zig,
//! RA8FW-558). Built as its own object in libra8_hal.a (RA8FW-542). Owns
//! the single module state the deleted ra8_ether_phy.c kept in s_state.

const common = @import("abi_common.zig");
const phy = @import("internal/ether_phy.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;

const tag = "EPHY";

var state: phy.State = .{};

/// `RA8_CHECK_NULL_PTR`'s failure path.
fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return k_ra8_err_null_ptr;
}

/// `ra8_err_t ra8_ether_phy_open(const ra8_ether_phy_cfg_t* cfg)`.
export fn ra8_ether_phy_open(cfg: ?*const phy.Cfg) u16 {
    const c = cfg orelse return nullPtr("cfg must not be nullptr");
    if (c.io.read == null) return nullPtr("io.read required");
    if (c.io.write == null) return nullPtr("io.write required");
    const err = state.open(c.*);
    if (err == k_ra8_ok) common.ra8_log_emit_info_val(tag, "phy ready addr", c.phy_address);
    return err;
}

/// `ra8_err_t ra8_ether_phy_close(void)`.
export fn ra8_ether_phy_close() u16 {
    return state.close();
}

/// `ra8_err_t ra8_ether_phy_mdio_read(uint8_t reg_addr, uint16_t* out_data)`.
export fn ra8_ether_phy_mdio_read(reg_addr: u8, out_data: ?*u16) u16 {
    const out = out_data orelse return nullPtr("out_data must not be nullptr");
    return state.mdioRead(reg_addr, out);
}

/// `ra8_err_t ra8_ether_phy_mdio_write(uint8_t reg_addr, uint16_t data)`.
export fn ra8_ether_phy_mdio_write(reg_addr: u8, data: u16) u16 {
    return state.mdioWrite(reg_addr, data);
}

/// `ra8_err_t ra8_ether_phy_auto_negotiate_start(void)`.
export fn ra8_ether_phy_auto_negotiate_start() u16 {
    return state.autoNegotiateStart();
}

/// `ra8_err_t ra8_ether_phy_link_status_get(ra8_ether_phy_link_t* out)`.
export fn ra8_ether_phy_link_status_get(out: ?*phy.Link) u16 {
    const o = out orelse return nullPtr("out must not be nullptr");
    return state.linkStatus(o);
}
