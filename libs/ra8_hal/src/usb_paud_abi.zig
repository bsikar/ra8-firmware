//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_usb_paud.h (RA8FW-614). The ra8_usb_* device core stays
//! in C; this file binds it as the ops value and keeps the one state.

const common = @import("abi_common.zig");
const pa = @import("internal/usb_paud.zig");

const tag = "USBPAUD";

extern fn ra8_usb_device_init(speed: u8) u16;
extern fn ra8_usb_device_deinit(speed: u8) u16;
extern fn ra8_usb_device_attach(speed: u8, attached: bool) u16;
extern fn ra8_usb_configure_endpoint(speed: u8, pipe: u8, ep: u8, dir: u8, ty: u8, mp: u16) u16;
extern fn ra8_usb_queue_in(speed: u8, pipe: u8, data: [*]const u8, len: u16) u16;
extern fn ra8_usb_queue_out(speed: u8, pipe: u8, buf: [*]u8, inout_len: *u16, rearm: bool) u16;
extern fn ra8_usb_control_response(speed: u8, accept: bool) u16;

const Usb = struct {
    pub fn deviceInit(_: Usb, speed: u8) u16 {
        return ra8_usb_device_init(speed);
    }
    pub fn deviceDeinit(_: Usb, speed: u8) u16 {
        return ra8_usb_device_deinit(speed);
    }
    pub fn deviceAttach(_: Usb, speed: u8, attached: bool) u16 {
        return ra8_usb_device_attach(speed, attached);
    }
    pub fn configureEndpoint(_: Usb, speed: u8, pipe: u8, ep: u8, dir: u8, ty: u8, mp: u16) u16 {
        return ra8_usb_configure_endpoint(speed, pipe, ep, dir, ty, mp);
    }
    pub fn queueIn(_: Usb, speed: u8, pipe: u8, data: [*]const u8, len: u16) u16 {
        return ra8_usb_queue_in(speed, pipe, data, len);
    }
    pub fn queueOut(_: Usb, speed: u8, pipe: u8, buf: [*]u8, inout: *u16, rearm: bool) u16 {
        return ra8_usb_queue_out(speed, pipe, buf, inout, rearm);
    }
    pub fn controlResponse(_: Usb, speed: u8, accept: bool) u16 {
        return ra8_usb_control_response(speed, accept);
    }
    pub fn err(_: Usb, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn errVal(_: Usb, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_error_val(tag, msg, value);
    }
    pub fn infoVal(_: Usb, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_info_val(tag, msg, value);
    }
};

var state: pa.State = .{};
const usb = Usb{};

export fn ra8_usb_paud_init(speed: u8) u16 {
    return pa.init(&state, usb, speed);
}

export fn ra8_usb_paud_close() u16 {
    return pa.close(&state, usb);
}

export fn ra8_usb_paud_set_descriptors(desc: ?[*]const u8, desc_len: u16) u16 {
    return pa.setDescriptors(&state, usb, desc, desc_len);
}

export fn ra8_usb_paud_send_frame(frame: ?[*]const u8, len: u16) u16 {
    return pa.sendFrame(&state, usb, frame, len);
}

export fn ra8_usb_paud_recv_frame(buf: ?[*]u8, max_len: u16, got_len: ?*u16) u16 {
    return pa.recvFrame(&state, usb, buf, max_len, got_len);
}

export fn ra8_usb_paud_set_format(format: pa.Format) u16 {
    return pa.setFormat(&state, format);
}

export fn ra8_usb_paud_get_format(out_format: ?*pa.Format) u16 {
    const out = out_format orelse {
        usb.err("get_format: out_format");
        return pa.null_ptr;
    };
    if (!state.initialized) return pa.invalid_state;
    out.* = state.format;
    return pa.ok;
}

export fn ra8_usb_paud_set_volume(volume_q8_8: i16) u16 {
    if (!state.initialized) return pa.invalid_state;
    state.volume_q8_8 = volume_q8_8;
    return pa.ok;
}

export fn ra8_usb_paud_get_volume(out_volume: ?*i16) u16 {
    const out = out_volume orelse {
        usb.err("get_volume: out_volume");
        return pa.null_ptr;
    };
    if (!state.initialized) return pa.invalid_state;
    out.* = state.volume_q8_8;
    return pa.ok;
}

export fn ra8_usb_paud_attach_setup_handler(setup_fn: ?pa.SetupFn, ctx: ?*anyopaque) u16 {
    if (!state.initialized) return pa.invalid_state;
    state.setup_cb = setup_fn;
    state.setup_ctx = ctx;
    return pa.ok;
}

export fn ra8_usb_paud_handle_setup(setup: ?*const pa.Setup) u16 {
    return pa.handleSetup(&state, usb, setup);
}
