//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the USB device printer class (RA8FW-586). The single class
//! state lives here; the ra8_usb_* primitives stay in ra8_usb_device.c.

const common = @import("abi_common.zig");
const pprn = @import("internal/usb_pprn.zig");

const tag = "USBPPRN";

extern fn ra8_usb_device_init(speed: u8) u16;
extern fn ra8_usb_device_deinit(speed: u8) u16;
extern fn ra8_usb_device_attach(speed: u8, attached: bool) u16;
extern fn ra8_usb_configure_endpoint(speed: u8, pipe: u8, ep: u8, dir: u8, kind: u8, mp: u16) u16;
extern fn ra8_usb_queue_in(speed: u8, pipe: u8, data: [*]const u8, len: u16) u16;
extern fn ra8_usb_queue_out(speed: u8, pipe: u8, buf: [*]u8, inout: *u16, rearm: bool) u16;
extern fn ra8_usb_control_response(speed: u8, accept: bool) u16;

var state: pprn.State = .{};

/// Binds internal/usb_pprn.zig to the C primitives and the log sink.
const C = struct {
    pub fn deviceInit(_: C, speed: u8) u16 {
        return ra8_usb_device_init(speed);
    }
    pub fn deviceDeinit(_: C, speed: u8) u16 {
        return ra8_usb_device_deinit(speed);
    }
    pub fn deviceAttach(_: C, speed: u8, attached: bool) u16 {
        return ra8_usb_device_attach(speed, attached);
    }
    pub fn configureEndpoint(_: C, speed: u8, pipe: u8, ep: u8, dir: u8, kind: u8, mp: u16) u16 {
        return ra8_usb_configure_endpoint(speed, pipe, ep, dir, kind, mp);
    }
    pub fn queueIn(_: C, speed: u8, pipe: u8, data: [*]const u8, len: u16) u16 {
        return ra8_usb_queue_in(speed, pipe, data, len);
    }
    pub fn queueOut(_: C, speed: u8, pipe: u8, buf: [*]u8, inout: *u16, rearm: bool) u16 {
        return ra8_usb_queue_out(speed, pipe, buf, inout, rearm);
    }
    pub fn controlResponse(_: C, speed: u8, accept: bool) u16 {
        return ra8_usb_control_response(speed, accept);
    }
    pub fn logError(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn logErrorVal(_: C, msg: [*:0]const u8, v: u16) void {
        common.ra8_log_emit_error_val(tag, msg, v);
    }
    pub fn logInfoVal(_: C, msg: [*:0]const u8, v: u8) void {
        common.ra8_log_emit_info_val(tag, msg, v);
    }
};

export fn ra8_usb_pprn_init(speed: u8) u16 {
    return state.init(C{}, speed);
}

export fn ra8_usb_pprn_close() u16 {
    return state.close(C{});
}

export fn ra8_usb_pprn_set_descriptors(desc: ?[*]const u8, len: u16, id: ?[*]const u8, id_len: u16) u16 {
    return state.setDescriptors(C{}, desc, len, id, id_len);
}

export fn ra8_usb_pprn_recv(buf: ?[*]u8, max_len: u16, got_len: ?*u16) u16 {
    return state.recv(C{}, buf, max_len, got_len);
}

export fn ra8_usb_pprn_send(data: ?[*]const u8, len: u16) u16 {
    return state.send(C{}, data, len);
}

export fn ra8_usb_pprn_set_port_status(status: u8) u16 {
    return state.setPortStatus(status);
}

export fn ra8_usb_pprn_get_port_status(out: ?*u8) u16 {
    return state.getPortStatus(C{}, out);
}

export fn ra8_usb_pprn_attach_setup_handler(cb: ?pprn.SetupFn, ctx: ?*anyopaque) u16 {
    return state.attachSetupHandler(cb, ctx);
}

export fn ra8_usb_pprn_handle_setup(setup: ?*const pprn.Setup) u16 {
    return state.handleSetup(C{}, setup);
}
