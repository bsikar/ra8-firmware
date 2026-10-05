//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the callback-driven PHY driver in ra8_rmac_phy.h (RA8FW-765),
//! which replaces ra8_rmac_phy.c. Logic is in internal/rmac_phy_drv.zig.

const common = @import("abi_common.zig");
const drv = @import("internal/rmac_phy_drv.zig");

const tag = "RMPHY";

const ReadFn = *const fn (?*anyopaque, u8, u8, ?*u16) callconv(.c) u16;
const WriteFn = *const fn (?*anyopaque, u8, u8, u16) callconv(.c) u16;

/// Mirror of ra8_rmac_phy_io_t.
const IoCfg = extern struct { read: ?ReadFn, write: ?WriteFn, ctx: ?*anyopaque };

/// Mirror of ra8_rmac_phy_cfg_t.
const Cfg = extern struct {
    io: IoCfg,
    lsi_type: u8,
    phy_address: u8,
    reset_poll_max: u16,
    local_advertise: u16,
    gbit_advertise: u16,
};

const State = struct {
    opened: bool = false,
    phy_address: u8 = 0,
    lsi_type: u8 = 0,
    local_advertise: u16 = 0,
    gbit_advertise: u16 = 0,
    io: IoCfg = .{ .read = null, .write = null, .ctx = null },
    last_bmsr: u16 = 0,
};

var s_state: State = .{};

/// Binds the stored callbacks to the stored PHY address.
const Io = struct {
    st: *const State,
    pub fn read(self: Io, reg: u8, out: *u16) u16 {
        return self.st.io.read.?(self.st.io.ctx, self.st.phy_address, reg, out);
    }
    pub fn write(self: Io, reg: u8, value: u16) u16 {
        return self.st.io.write.?(self.st.io.ctx, self.st.phy_address, reg, value);
    }
};

fn io() Io {
    return .{ .st = &s_state };
}

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

fn validate(cfg: ?*const Cfg) u16 {
    const c = cfg orelse return nullPtr("cfg must not be nullptr");
    if (c.io.read == null) return nullPtr("io.read required");
    if (c.io.write == null) return nullPtr("io.write required");
    if (c.phy_address > drv.addr_max) return common.k_ra8_err_invalid_arg;
    if (c.lsi_type >= drv.lsi_count) return common.k_ra8_err_invalid_arg;
    return common.k_ra8_ok;
}

export fn priv_ra8_rmac_phy_internal_speed_ok(err: u16, reg_value: u16, mask: u16) bool {
    return drv.speedOk(err, reg_value, mask);
}

export fn ra8_rmac_phy_open(cfg: ?*const Cfg) u16 {
    const varg = validate(cfg);
    if (varg != common.k_ra8_ok) return varg;
    if (s_state.opened) return common.k_ra8_err_exists;
    const c = cfg.?;
    s_state = .{
        .opened = true,
        .phy_address = c.phy_address,
        .lsi_type = c.lsi_type,
        .local_advertise = c.local_advertise,
        .gbit_advertise = c.gbit_advertise,
        .io = c.io,
        .last_bmsr = 0,
    };
    const poll_max = if (c.reset_poll_max == 0) drv.reset_poll_default else c.reset_poll_max;
    var err = drv.resetAndWait(io(), poll_max);
    if (err == common.k_ra8_ok) err = drv.programAdvertise(io(), s_state.local_advertise, s_state.gbit_advertise);
    if (err != common.k_ra8_ok) {
        s_state.opened = false;
        return err;
    }
    common.ra8_log_emit_info_val(tag, "rmac phy lsi", c.lsi_type);
    return common.k_ra8_ok;
}

export fn ra8_rmac_phy_close() u16 {
    if (!s_state.opened) return common.k_ra8_err_invalid_state;
    s_state.opened = false;
    return common.k_ra8_ok;
}

export fn ra8_rmac_phy_mdio_read(reg_addr: u8, out_data: ?*u16) u16 {
    const out = out_data orelse return nullPtr("out_data must not be nullptr");
    if (!s_state.opened) return common.k_ra8_err_not_initialized;
    if (reg_addr > drv.reg_max) return common.k_ra8_err_invalid_arg;
    return io().read(reg_addr, out);
}

export fn ra8_rmac_phy_mdio_write(reg_addr: u8, data: u16) u16 {
    if (!s_state.opened) return common.k_ra8_err_not_initialized;
    if (reg_addr > drv.reg_max) return common.k_ra8_err_invalid_arg;
    return io().write(reg_addr, data);
}

export fn ra8_rmac_phy_auto_negotiate_start() u16 {
    if (!s_state.opened) return common.k_ra8_err_not_initialized;
    return io().write(drv.reg_control, drv.bmcr_an_enable | drv.bmcr_an_restart);
}

export fn ra8_rmac_phy_link_status_get(out_link: ?*drv.Link) u16 {
    const out = out_link orelse return nullPtr("out must not be nullptr");
    if (!s_state.opened) return common.k_ra8_err_not_initialized;
    var bmsr: u16 = 0;
    const err = io().read(drv.reg_status, &bmsr);
    if (err != common.k_ra8_ok) return err;
    s_state.last_bmsr = bmsr;
    if (drv.fromBmsr(bmsr, out)) drv.resolve(io(), s_state.gbit_advertise, out);
    return common.k_ra8_ok;
}

export fn ra8_rmac_phy_lsi_get(out_lsi: ?*u8) u16 {
    const out = out_lsi orelse return nullPtr("out must not be nullptr");
    if (!s_state.opened) return common.k_ra8_err_not_initialized;
    out.* = s_state.lsi_type;
    return common.k_ra8_ok;
}
