//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the USB device composite multiplexer (RA8FW-773), which
//! replaces ra8_usb_composite.c. Same symbols, guard order, error codes
//! and log strings. Logic is in internal/usb_composite.zig.

const common = @import("abi_common.zig");
const m = @import("internal/usb_composite.zig");

const tag = "USBCOMP";
const ok = m.ok;
const hw_init_failed: u16 = 0x201;

extern fn ra8_usb_device_init(speed: u8) u16;
extern fn ra8_usb_device_deinit(speed: u8) u16;
extern fn ra8_usb_set_address(speed: u8, address: u8) u16;

var mux: m.Mux = .{};

fn needInit() ?u16 {
    if (!mux.initialized) return m.err_invalid_state;
    return null;
}

/// RA8_CHECK_NULL_PTR: log the message and return null_ptr.
fn nullCheck(p: anytype, message: [*:0]const u8) ?u16 {
    if (p != null) return null;
    common.ra8_log_emit_error(tag, message);
    return m.err_null_ptr;
}

export fn ra8_usb_composite_init(speed: u8) u16 {
    if (!m.speedOk(speed)) return m.err_invalid_arg;
    const usb_err = ra8_usb_device_init(speed);
    if (usb_err != ok) {
        common.ra8_log_emit_error_val(tag, "ra8_usb_device_init failed", usb_err);
        return hw_init_failed;
    }
    mux = m.Mux.fresh(speed);
    common.ra8_log_emit_info_val(tag, "composite ready", speed);
    return ok;
}

export fn ra8_usb_composite_close() u16 {
    if (needInit()) |e| return e;
    var i = mux.class_count;
    while (i > 0) : (i -= 1) {
        const cl = &mux.classes[i - 1];
        if (cl.close) |close| _ = close(cl.ctx);
    }
    const deinit_err = ra8_usb_device_deinit(mux.speed);
    mux.shut();
    if (deinit_err != ok) {
        common.ra8_log_emit_error_val(tag, "ra8_usb_device_deinit failed", deinit_err);
        return deinit_err;
    }
    return ok;
}

export fn ra8_usb_composite_register_class(class_layer: ?*const m.Class) u16 {
    if (needInit()) |e| return e;
    const cl = class_layer orelse return m.err_null_ptr;
    if (mux.admit(cl)) |e| return e;
    const slot = mux.claim(cl);
    // Init runs last; a failure leaves the class registered, as in C.
    const init_err = cl.init.?(cl.ctx);
    if (init_err != ok) {
        common.ra8_log_emit_error_val(tag, "class init failed", init_err);
        return init_err;
    }
    common.ra8_log_emit_info_val(tag, "class registered", slot);
    return ok;
}

export fn ra8_usb_composite_set_descriptors(device_desc: ?[*]const u8, config_desc: ?[*]const u8) u16 {
    if (needInit()) |e| return e;
    if (nullCheck(device_desc, "set_descriptors: device")) |e| return e;
    if (nullCheck(config_desc, "set_descriptors: config")) |e| return e;
    mux.device_desc = device_desc;
    mux.config_desc = config_desc;
    common.ra8_log_emit_info(tag, "descriptors cached");
    return ok;
}

export fn ra8_usb_composite_step() u16 {
    if (needInit()) |e| return e;
    mux.step();
    return ok;
}

fn standard(setup: *const m.Setup) u16 {
    if (setup.b_request != m.std_set_address) return ok;
    if (setup.w_value > m.max_address) return m.err_invalid_arg;
    return ra8_usb_set_address(mux.speed, @intCast(setup.w_value));
}

export fn ra8_usb_composite_dispatch_setup(setup: ?*const m.Setup, out_handler_class: ?*u8) u16 {
    if (needInit()) |e| return e;
    if (nullCheck(setup, "dispatch_setup: setup")) |e| return e;
    if (nullCheck(out_handler_class, "dispatch_setup: out_handler")) |e| return e;
    const s = setup.?;
    const out = out_handler_class.?;
    if (m.isStandard(s)) {
        mux.last_handler = m.handler_self;
        out.* = m.handler_self;
        return standard(s);
    }
    const idx = mux.owner(m.interfaceOf(s)) orelse return m.err_not_found;
    const cl = &mux.classes[idx];
    const route_err = cl.handle_setup.?(cl.ctx, s);
    mux.last_handler = idx;
    out.* = idx;
    return route_err;
}

export fn ra8_usb_composite_get_class_count(out_count: ?*u8) u16 {
    if (needInit()) |e| return e;
    if (nullCheck(out_count, "get_class_count: out")) |e| return e;
    out_count.?.* = mux.class_count;
    return ok;
}

export fn ra8_usb_composite_get_device_descriptor(out_desc: ?*?[*]const u8) u16 {
    if (needInit()) |e| return e;
    if (nullCheck(out_desc, "get_device_descriptor: out")) |e| return e;
    out_desc.?.* = mux.device_desc;
    return ok;
}

export fn ra8_usb_composite_get_config_descriptor(out_desc: ?*?[*]const u8) u16 {
    if (needInit()) |e| return e;
    if (nullCheck(out_desc, "get_config_descriptor: out")) |e| return e;
    out_desc.?.* = mux.config_desc;
    return ok;
}
