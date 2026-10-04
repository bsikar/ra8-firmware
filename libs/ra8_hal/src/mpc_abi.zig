//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the PFS pin-mux helpers (internal/mpc.zig,
//! RA8FW-560). Built as its own object in libra8_hal.a (RA8FW-542).

const common = @import("abi_common.zig");
const mpc = @import("internal/mpc.zig");

const tag = "MPC";
const dir_output: u8 = 1; // k_ra8_mpc_dir_output
const pull_up: u8 = 1; // k_ra8_mpc_pull_up
const block = mpc.Block{};

/// `ra8_err_t ra8_mpc_route_peripheral(ra8_port_t, ra8_pin_t, ra8_mpc_psel_t)`.
export fn ra8_mpc_route_peripheral(port: u8, pin: u8, psel: u8) u16 {
    const err = block.routePeripheral(port, pin, psel);
    if (err == common.k_ra8_ok) common.ra8_log_emit_info_val(tag, "route", psel);
    return err;
}

/// `ra8_err_t ra8_mpc_set_gpio(ra8_port_t, ra8_pin_t, ra8_mpc_dir_t)`.
export fn ra8_mpc_set_gpio(port: u8, pin: u8, dir: u8) u16 {
    return block.setGpio(port, pin, dir == dir_output);
}

/// `ra8_err_t ra8_mpc_set_analog(ra8_port_t, ra8_pin_t)`.
export fn ra8_mpc_set_analog(port: u8, pin: u8) u16 {
    return block.setAnalog(port, pin);
}

/// `ra8_err_t ra8_mpc_set_irq(ra8_port_t, ra8_pin_t)`.
export fn ra8_mpc_set_irq(port: u8, pin: u8) u16 {
    return block.setIrq(port, pin);
}

/// `ra8_err_t ra8_mpc_set_pull(ra8_port_t, ra8_pin_t, ra8_mpc_pull_t)`.
export fn ra8_mpc_set_pull(port: u8, pin: u8, pull: u8) u16 {
    return block.setPull(port, pin, pull == pull_up);
}

/// `ra8_err_t ra8_mpc_set_open_drain(ra8_port_t, ra8_pin_t, bool)`.
export fn ra8_mpc_set_open_drain(port: u8, pin: u8, enable: bool) u16 {
    return block.setOpenDrain(port, pin, enable);
}

/// `ra8_err_t ra8_mpc_read_pfs(ra8_port_t, ra8_pin_t, uint32_t* out_val)`.
export fn ra8_mpc_read_pfs(port: u8, pin: u8, out_val: ?*u32) u16 {
    const out = out_val orelse {
        common.ra8_log_emit_error(tag, "out_val must not be NULL");
        return common.k_ra8_err_null_ptr;
    };
    return block.readPfs(port, pin, out);
}
