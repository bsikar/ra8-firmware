//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the USB host hub class driver (RA8FW-771), which replaces
//! ra8_usb_hhub.c. Same symbols, guard order, error codes and log strings.
//! Logic is in internal/usb_hhub.zig.

const common = @import("abi_common.zig");
const h = @import("internal/usb_hhub.zig");

const tag = "USBHHUB";
const ok = common.k_ra8_ok;
const invalid_state = common.k_ra8_err_invalid_state;
const hw_init_failed: u16 = 0x201;

extern fn ra8_usb_host_init(speed: u8) u16;
extern fn ra8_usb_host_deinit(speed: u8) u16;
extern fn ra8_usb_host_set_uact(speed: u8, enable: bool) u16;
extern fn ra8_usb_host_bus_reset(speed: u8, assert_reset: bool) u16;
extern fn ra8_usb_host_setup_request(speed: u8, setup: *const h.Setup) u16;
extern fn ra8_usb_set_address(speed: u8, address: u8) u16;

const Host = struct {
    pub fn busReset(_: Host, speed: u8, assert_reset: bool) u16 {
        return ra8_usb_host_bus_reset(speed, assert_reset);
    }
    pub fn setAddress(_: Host, speed: u8, address: u8) u16 {
        return ra8_usb_set_address(speed, address);
    }
    pub fn setup(_: Host, speed: u8, s: h.Setup) u16 {
        return ra8_usb_host_setup_request(speed, &s);
    }
};

var state: h.Hub = .{};

/// Initialised and attached, else invalid_state.
fn ready() ?u16 {
    if (!state.initialized or !state.attached) return invalid_state;
    return null;
}

fn portGuard(port: u8) ?u16 {
    if (ready()) |e| return e;
    if (!state.portOk(port)) return common.k_ra8_err_invalid_arg;
    return null;
}

export fn ra8_usb_hhub_init(speed: u8) u16 {
    if (speed != 0 and speed != 1) return common.k_ra8_err_invalid_arg;
    const usb_err = ra8_usb_host_init(speed);
    if (usb_err != ok) {
        common.ra8_log_emit_error_val(tag, "ra8_usb_host_init failed", usb_err);
        return hw_init_failed;
    }
    state = .{ .initialized = true, .speed = speed };
    common.ra8_log_emit_info_val(tag, "host-HUB ready", speed);
    return ok;
}

export fn ra8_usb_hhub_close() u16 {
    if (!state.initialized) return invalid_state;
    _ = ra8_usb_host_set_uact(state.speed, false);
    const err = ra8_usb_host_deinit(state.speed);
    state.initialized = false;
    state.attached = false;
    state.attach_cb = null;
    state.attach_ctx = null;
    state.step = .idle;
    return err;
}

export fn ra8_usb_hhub_attach_callback(on_attach: ?h.AttachFn, ctx: ?*anyopaque) u16 {
    if (!state.initialized) return invalid_state;
    state.attach_cb = on_attach;
    state.attach_ctx = ctx;
    return ok;
}

export fn ra8_usb_hhub_get_port_count(count: ?*u8) u16 {
    const out = count orelse {
        common.ra8_log_emit_error(tag, "get_port_count: count");
        return common.k_ra8_err_null_ptr;
    };
    if (ready()) |e| return e;
    out.* = state.device.port_count;
    return ok;
}

export fn ra8_usb_hhub_get_port_status(port: u8, status: ?*u32) u16 {
    const out = status orelse {
        common.ra8_log_emit_error(tag, "get_port_status: status");
        return common.k_ra8_err_null_ptr;
    };
    if (portGuard(port)) |e| return e;
    out.* = 0;
    return (Host{}).setup(state.speed, h.portStatus(port));
}

export fn ra8_usb_hhub_set_port_feature(port: u8, feature: u16) u16 {
    if (portGuard(port)) |e| return e;
    return (Host{}).setup(state.speed, h.portFeature(port, feature, true));
}

export fn ra8_usb_hhub_clear_port_feature(port: u8, feature: u16) u16 {
    if (portGuard(port)) |e| return e;
    return (Host{}).setup(state.speed, h.portFeature(port, feature, false));
}

export fn ra8_usb_hhub_step() u16 {
    if (!state.initialized) return invalid_state;
    return state.advance(Host{});
}
