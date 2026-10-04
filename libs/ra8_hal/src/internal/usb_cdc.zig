//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB CDC-ACM device class (RA8FW-623). The USB device stack is reached
//! through a `usb` ops value so host tests can stand in for ra8_usb_*.

pub const Setup = @import("usb_paud.zig").Setup;

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;
pub const not_supported: u16 = 0x107;
pub const no_data: u16 = 0x10A;
pub const hw_init_failed: u16 = 0x201;
pub const null_ptr: u16 = 0x504;

pub const speed_fs: u8 = 0;
pub const speed_hs: u8 = 1;
pub const dir_out: u8 = 0;
pub const dir_in: u8 = 1;
pub const type_bulk: u8 = 0;
pub const type_intr: u8 = 1;

pub const pipe_bulk_in: u8 = 1;
pub const pipe_bulk_out: u8 = 2;
pub const pipe_intr_in: u8 = 6;
pub const ep_bulk_in: u8 = 1;
pub const ep_bulk_out: u8 = 2;
pub const ep_intr_in: u8 = 3;
pub const bulk_mp_fs: u16 = 64;
pub const bulk_mp_hs: u16 = 512;
pub const intr_mp: u16 = 8;

pub const req_set_line_coding: u8 = 0x20;
pub const req_get_line_coding: u8 = 0x21;
pub const req_set_control_line_state: u8 = 0x22;
pub const line_state_dtr: u16 = 0x0001;
pub const line_state_rts: u16 = 0x0002;
pub const bm_class_iface: u8 = 0x21;
pub const bm_class_in: u8 = 0xA1;

pub const line_coding_len = 7;
pub const data_stage_polls: u16 = 1000;

/// ra8_usb_cdc_line_coding_t.
pub const LineCoding = extern struct {
    dte_rate: u32,
    char_format: u8,
    parity_type: u8,
    data_bits: u8,
};

pub const default_coding = LineCoding{ .dte_rate = 9600, .char_format = 0, .parity_type = 0, .data_bits = 8 };

pub const State = struct {
    initialized: bool = false,
    speed: u8 = 0,
    coding: LineCoding = default_coding,
    dtr: bool = false,
    rts: bool = false,
};

/// RA8_RETURN_ON_ERROR: log the message and the code, hand the code back.
fn logged(usb: anytype, rc: u16, msg: [*:0]const u8) u16 {
    if (rc != ok) {
        usb.err(msg);
        usb.errVal("Error", rc);
    }
    return rc;
}

fn configurePipes(usb: anytype, speed: u8) u16 {
    const mp = if (speed == speed_hs) bulk_mp_hs else bulk_mp_fs;
    var rc = usb.configureEndpoint(speed, pipe_bulk_in, ep_bulk_in, dir_in, type_bulk, mp);
    if (rc != ok) return logged(usb, rc, "cdc: bulk-in cfg");
    rc = usb.configureEndpoint(speed, pipe_bulk_out, ep_bulk_out, dir_out, type_bulk, mp);
    if (rc != ok) return logged(usb, rc, "cdc: bulk-out cfg");
    return usb.configureEndpoint(speed, pipe_intr_in, ep_intr_in, dir_in, type_intr, intr_mp);
}

/// Decode a SET_LINE_CODING payload; short or null payloads are ignored.
pub fn applyLineCoding(s: *State, data: ?[*]const u8, len: u16) void {
    const d = data orelse return;
    if (len < line_coding_len) return;
    s.coding.dte_rate = @as(u32, d[0]) | (@as(u32, d[1]) << 8) | (@as(u32, d[2]) << 16) | (@as(u32, d[3]) << 24);
    s.coding.char_format = d[4];
    s.coding.parity_type = d[5];
    s.coding.data_bits = d[6];
}

pub fn init(s: *State, usb: anytype, speed: u8) u16 {
    if (speed != speed_fs and speed != speed_hs) return invalid_arg;
    const usb_err = usb.deviceInit(speed);
    if (usb_err != ok) {
        usb.errVal("ra8_usb_device_init failed", usb_err);
        return hw_init_failed;
    }
    s.speed = speed;
    s.dtr = false;
    s.rts = false;
    s.coding = default_coding;
    const pipes = configurePipes(usb, speed);
    if (pipes != ok) {
        _ = usb.deviceDeinit(speed);
        return pipes;
    }
    s.initialized = true;
    usb.info("CDC ready");
    return ok;
}

pub fn deinit(s: *State, usb: anytype) u16 {
    if (!s.initialized) return invalid_state;
    _ = usb.attach(s.speed, false);
    const rc = usb.deviceDeinit(s.speed);
    s.initialized = false;
    s.dtr = false;
    s.rts = false;
    return rc;
}

pub fn attach(s: *State, usb: anytype, attached: bool) u16 {
    if (!s.initialized) return invalid_state;
    return usb.attach(s.speed, attached);
}

pub fn send(s: *State, usb: anytype, data: ?[*]const u8, len: u16) u16 {
    if (!s.initialized) return invalid_state;
    if (data == null and len != 0) return invalid_arg;
    return usb.queueIn(s.speed, pipe_bulk_in, data, len);
}

/// Null checks are the ABI's; this is the part after them.
pub fn recv(s: *State, usb: anytype, buf: [*]u8, inout_len: *u16) u16 {
    if (!s.initialized) return invalid_state;
    if (inout_len.* == 0) return invalid_arg;
    return usb.queueOut(s.speed, pipe_bulk_out, buf, inout_len, true);
}

fn pullDataStage(s: *State, usb: anytype, buf: []u8, out_len: *u16) u16 {
    out_len.* = 0;
    const armed = usb.dcpOutArm(s.speed);
    if (armed != ok) return armed;
    var rc = no_data;
    for (0..data_stage_polls) |_| {
        rc = usb.dcpOutRead(s.speed, buf.ptr, @intCast(buf.len), out_len);
        if (rc != no_data) return rc;
    }
    return rc;
}

fn dispatch(s: *State, usb: anytype, setup: *const Setup) u16 {
    switch (setup.b_request) {
        req_set_line_coding => {
            var buf = [_]u8{0} ** (line_coding_len + 1);
            var plen: u16 = 0;
            if (pullDataStage(s, usb, &buf, &plen) == ok) applyLineCoding(s, &buf, plen);
            return usb.controlResponse(s.speed, true);
        },
        req_get_line_coding => return usb.controlResponse(s.speed, true),
        req_set_control_line_state => {
            s.dtr = (setup.w_value & line_state_dtr) != 0;
            s.rts = (setup.w_value & line_state_rts) != 0;
            return usb.controlResponse(s.speed, true);
        },
        else => return not_supported,
    }
}

pub fn handleSetup(s: *State, usb: anytype, setup: *const Setup) u16 {
    if (!s.initialized) return invalid_state;
    if (setup.bm_request_type != bm_class_iface and setup.bm_request_type != bm_class_in) return not_supported;
    return dispatch(s, usb, setup);
}
