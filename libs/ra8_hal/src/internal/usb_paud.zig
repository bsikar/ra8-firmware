//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB device-Audio (UAC1) class shim (RA8FW-614). Holds the format,
//! volume and setup-handler shadow; the device core is reached through a
//! `usb` ops value so host tests can stand in for the controller.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;
pub const not_supported: u16 = 0x107;
pub const null_ptr: u16 = 0x504;
pub const hw_init_failed: u16 = 0x201;

pub const speed_fs: u8 = 0;
pub const speed_hs: u8 = 1;
pub const pipe_iso_in: u8 = 1;
pub const pipe_iso_out: u8 = 2;
pub const ep_iso_in_addr: u8 = 1;
pub const ep_iso_out_addr: u8 = 2;
pub const dir_out: u8 = 0;
pub const dir_in: u8 = 1;
pub const type_iso: u8 = 2;
pub const max_packet_fs_default: u16 = 192;
pub const max_packet_hs: u16 = 1024;

const class_envelopes = [_]u8{ 0xA1, 0x21, 0xA2, 0x22 };
const class_requests = [_]u8{ 0x01, 0x81, 0x02, 0x82, 0x03, 0x83, 0x04, 0x84, 0xFF };

pub const Format = extern struct {
    sample_rate_hz: u32,
    channels: u8,
    bytes_per_sample: u8,
};

pub const default_format = Format{ .sample_rate_hz = 48000, .channels = 2, .bytes_per_sample = 2 };

pub const Setup = extern struct {
    bm_request_type: u8,
    b_request: u8,
    w_value: u16,
    w_index: u16,
    w_length: u16,
};

pub const SetupFn = *const fn (ctx: ?*anyopaque, setup: *const Setup) callconv(.c) u16;

pub const State = struct {
    initialized: bool = false,
    speed: u8 = 0,
    iso_max_packet: u16 = 0,
    desc: ?[*]const u8 = null,
    desc_len: u16 = 0,
    format: Format = .{ .sample_rate_hz = 0, .channels = 0, .bytes_per_sample = 0 },
    volume_q8_8: i16 = 0,
    setup_cb: ?SetupFn = null,
    setup_ctx: ?*anyopaque = null,
};

pub fn isoMaxPacket(speed: u8) u16 {
    return if (speed == speed_hs) max_packet_hs else max_packet_fs_default;
}

fn contains(set: []const u8, v: u8) bool {
    for (set) |x| if (x == v) return true;
    return false;
}

pub fn isClassEnvelope(bm: u8) bool {
    return contains(&class_envelopes, bm);
}

pub fn isKnownClassRequest(b_request: u8) bool {
    return contains(&class_requests, b_request);
}

fn resetShadow(s: *State, speed: u8) void {
    s.speed = speed;
    s.iso_max_packet = isoMaxPacket(speed);
    s.desc = null;
    s.desc_len = 0;
    s.format = default_format;
    s.volume_q8_8 = 0;
    s.setup_cb = null;
    s.setup_ctx = null;
}

pub fn init(s: *State, usb: anytype, speed: u8) u16 {
    if (speed != speed_fs and speed != speed_hs) return invalid_arg;
    const usb_err = usb.deviceInit(speed);
    if (usb_err != ok) {
        usb.errVal("ra8_usb_device_init failed", usb_err);
        return hw_init_failed;
    }
    resetShadow(s, speed);
    const mp = isoMaxPacket(speed);
    _ = usb.configureEndpoint(speed, pipe_iso_in, ep_iso_in_addr, dir_in, type_iso, mp);
    _ = usb.configureEndpoint(speed, pipe_iso_out, ep_iso_out_addr, dir_out, type_iso, mp);
    s.initialized = true;
    usb.infoVal("device-Audio ready", speed);
    return ok;
}

pub fn close(s: *State, usb: anytype) u16 {
    if (!s.initialized) return invalid_state;
    _ = usb.deviceAttach(s.speed, false);
    const e = usb.deviceDeinit(s.speed);
    s.initialized = false;
    s.desc = null;
    s.setup_cb = null;
    s.setup_ctx = null;
    return e;
}

pub fn setDescriptors(s: *State, usb: anytype, desc: ?[*]const u8, len: u16) u16 {
    if (!s.initialized) return invalid_state;
    const d = desc orelse {
        usb.err("set_descriptors: desc");
        return null_ptr;
    };
    if (len == 0) return invalid_arg;
    s.desc = d;
    s.desc_len = len;
    return ok;
}

pub fn sendFrame(s: *State, usb: anytype, frame: ?[*]const u8, len: u16) u16 {
    if (!s.initialized) return invalid_state;
    if (frame == null and len != 0) return null_ptr;
    if (len == 0 or len > s.iso_max_packet) return invalid_arg;
    return usb.queueIn(s.speed, pipe_iso_in, frame.?, len);
}

pub fn recvFrame(s: *State, usb: anytype, buf: ?[*]u8, max_len: u16, got_len: ?*u16) u16 {
    const b = buf orelse {
        usb.err("recv_frame: buf");
        return null_ptr;
    };
    const g = got_len orelse {
        usb.err("recv_frame: got_len");
        return null_ptr;
    };
    if (!s.initialized) return invalid_state;
    if (max_len == 0) return invalid_arg;
    var inout: u16 = max_len;
    const e = usb.queueOut(s.speed, pipe_iso_out, b, &inout, true);
    g.* = if (e == ok) inout else 0;
    return e;
}

pub fn setFormat(s: *State, f: Format) u16 {
    if (!s.initialized) return invalid_state;
    if (f.sample_rate_hz == 0) return invalid_arg;
    if (f.channels < 1 or f.channels > 2) return invalid_arg;
    if (f.bytes_per_sample < 1 or f.bytes_per_sample > 4) return invalid_arg;
    s.format = f;
    return ok;
}

pub fn handleSetup(s: *State, usb: anytype, setup: ?*const Setup) u16 {
    const p = setup orelse {
        usb.err("handle_setup: setup");
        return null_ptr;
    };
    if (!s.initialized) return invalid_state;
    if (!isClassEnvelope(p.bm_request_type)) return not_supported;
    if (!isKnownClassRequest(p.b_request)) return not_supported;
    if (s.setup_cb) |cb| {
        if (cb(s.setup_ctx, p) != ok) return usb.controlResponse(s.speed, false);
    }
    return usb.controlResponse(s.speed, true);
}
