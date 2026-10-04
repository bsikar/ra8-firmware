//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_usb_cdc.h (RA8FW-623), replacing ra8_usb_cdc.c. The USB
//! device stack stays C and is bound here as the `usb` ops value.

const common = @import("abi_common.zig");
const cdc = @import("internal/usb_cdc.zig");

const tag = "USBCDC";

extern fn ra8_usb_device_init(speed: u8) u16;
extern fn ra8_usb_device_deinit(speed: u8) u16;
extern fn ra8_usb_device_attach(speed: u8, attached: bool) u16;
extern fn ra8_usb_configure_endpoint(speed: u8, pipe: u8, ep: u8, dir: u8, ty: u8, mp: u16) u16;
extern fn ra8_usb_queue_in(speed: u8, pipe: u8, data: ?[*]const u8, len: u16) u16;
extern fn ra8_usb_queue_out(speed: u8, pipe: u8, buf: [*]u8, inout_len: *u16, rearm: bool) u16;
extern fn ra8_usb_dcp_out_arm(speed: u8) u16;
extern fn ra8_usb_dcp_out_read(speed: u8, buf: [*]u8, cap: u16, out_rx: *u16) u16;
extern fn ra8_usb_control_response(speed: u8, accept: bool) u16;

const Usb = struct {
    pub fn deviceInit(_: Usb, speed: u8) u16 {
        return ra8_usb_device_init(speed);
    }
    pub fn deviceDeinit(_: Usb, speed: u8) u16 {
        return ra8_usb_device_deinit(speed);
    }
    pub fn attach(_: Usb, speed: u8, attached: bool) u16 {
        return ra8_usb_device_attach(speed, attached);
    }
    pub fn configureEndpoint(_: Usb, speed: u8, pipe: u8, ep: u8, dir: u8, ty: u8, mp: u16) u16 {
        return ra8_usb_configure_endpoint(speed, pipe, ep, dir, ty, mp);
    }
    pub fn queueIn(_: Usb, speed: u8, pipe: u8, data: ?[*]const u8, len: u16) u16 {
        return ra8_usb_queue_in(speed, pipe, data, len);
    }
    pub fn queueOut(_: Usb, speed: u8, pipe: u8, buf: [*]u8, inout_len: *u16, rearm: bool) u16 {
        return ra8_usb_queue_out(speed, pipe, buf, inout_len, rearm);
    }
    pub fn dcpOutArm(_: Usb, speed: u8) u16 {
        return ra8_usb_dcp_out_arm(speed);
    }
    pub fn dcpOutRead(_: Usb, speed: u8, buf: [*]u8, cap: u16, out_rx: *u16) u16 {
        return ra8_usb_dcp_out_read(speed, buf, cap, out_rx);
    }
    pub fn controlResponse(_: Usb, speed: u8, accept: bool) u16 {
        return ra8_usb_control_response(speed, accept);
    }
    pub fn info(_: Usb, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn err(_: Usb, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn errVal(_: Usb, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_error_val(tag, msg, value);
    }
};

var state: cdc.State = .{};

fn fail(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

/// RA8_TEST_HELPER in the C: exported for the host C tests.
export fn ra8_usb_cdc_test_apply_line_coding(data: ?[*]const u8, len: u16) void {
    cdc.applyLineCoding(&state, data, len);
}

export fn ra8_usb_cdc_init(speed: u8) u16 {
    return cdc.init(&state, Usb{}, speed);
}

export fn ra8_usb_cdc_deinit() u16 {
    return cdc.deinit(&state, Usb{});
}

export fn ra8_usb_cdc_attach(attached: bool) u16 {
    return cdc.attach(&state, Usb{}, attached);
}

export fn ra8_usb_cdc_send(data: ?[*]const u8, len: u16) u16 {
    return cdc.send(&state, Usb{}, data, len);
}

export fn ra8_usb_cdc_recv(out_buf: ?[*]u8, inout_len: ?*u16) u16 {
    const buf = out_buf orelse return fail("cdc_recv: out_buf");
    const len = inout_len orelse return fail("cdc_recv: inout_len");
    return cdc.recv(&state, Usb{}, buf, len);
}

export fn ra8_usb_cdc_handle_setup(setup: ?*const cdc.Setup) u16 {
    const s = setup orelse return fail("handle_setup: setup");
    return cdc.handleSetup(&state, Usb{}, s);
}

export fn ra8_usb_cdc_get_line_coding(out: ?*cdc.LineCoding) u16 {
    const o = out orelse return fail("get_line_coding: out");
    if (!state.initialized) return common.k_ra8_err_invalid_state;
    o.* = state.coding;
    return common.k_ra8_ok;
}

export fn ra8_usb_cdc_get_line_state(out_dtr: ?*bool, out_rts: ?*bool) u16 {
    const d = out_dtr orelse return fail("get_line_state: out_dtr");
    const r = out_rts orelse return fail("get_line_state: out_rts");
    if (!state.initialized) return common.k_ra8_err_invalid_state;
    d.* = state.dtr;
    r.* = state.rts;
    return common.k_ra8_ok;
}
