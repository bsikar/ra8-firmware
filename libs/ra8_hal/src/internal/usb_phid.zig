//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB device HID class (RA8FW-743), ported from ra8_usb_phid.c. The state
//! and every rule live here; the ra8_usb_* primitives and logging come in
//! through an `ops` value so the host tests can record them.
//! USB HID 1.11 sec 7.2 (class requests) and sec 8 (report protocol).

const pvnd = @import("usb_pvnd.zig");

pub const codes = pvnd.codes;
const err = codes;
pub const Setup = pvnd.Setup;
pub const SetupFn = pvnd.SetupFn;
pub const speed_fs = pvnd.speed_fs;
pub const speed_hs = pvnd.speed_hs;

pub const pipe_intr_in: u8 = 6;
pub const pipe_intr_out: u8 = 7;
pub const ep_intr_in: u8 = 1;
pub const ep_intr_out: u8 = 2;
pub const type_intr: u8 = 1;
pub const max_packet_default: u16 = 8;
pub const max_packet_hs: u16 = 1024;

pub const bm_class_iface_in: u8 = 0xA1;
pub const bm_class_iface_out: u8 = 0x21;
pub const req_get_report: u8 = 0x01;
pub const req_get_idle: u8 = 0x02;
pub const req_get_protocol: u8 = 0x03;
pub const req_set_report: u8 = 0x09;
pub const req_set_idle: u8 = 0x0A;
pub const req_set_protocol: u8 = 0x0B;
pub const proto_boot: u8 = 0;
pub const proto_report: u8 = 1;

pub fn intrMaxPacket(speed: u8) u16 {
    return if (speed == speed_hs) max_packet_hs else max_packet_default;
}

pub fn isClassEnvelope(bm: u8) bool {
    return bm == bm_class_iface_in or bm == bm_class_iface_out;
}

pub fn isKnownRequest(b: u8) bool {
    return switch (b) {
        req_get_report, req_set_report, req_get_idle, req_set_idle, req_get_protocol, req_set_protocol => true,
        else => false,
    };
}

pub const State = struct {
    initialized: bool = false,
    speed: u8 = speed_fs,
    intr_max_packet: u16 = 0,
    report_desc: ?[*]const u8 = null,
    report_desc_len: u16 = 0,
    hid_desc: ?[*]const u8 = null,
    hid_desc_len: u16 = 0,
    idle_rate: u8 = 0,
    protocol: u8 = proto_report,
    setup_cb: ?SetupFn = null,
    setup_ctx: ?*anyopaque = null,

    fn resetShadow(s: *State, speed: u8) void {
        s.* = .{ .initialized = s.initialized, .speed = speed, .intr_max_packet = intrMaxPacket(speed) };
    }

    pub fn init(s: *State, ops: anytype, speed: u8) u16 {
        if (speed != speed_fs and speed != speed_hs) return err.invalid_arg;
        const usb_err = ops.deviceInit(speed);
        if (usb_err != err.ok) {
            ops.logErrorVal("ra8_usb_device_init failed", usb_err);
            return err.hw_init_failed;
        }
        s.resetShadow(speed);
        const mp = s.intr_max_packet;
        // Validated speed and fixed pipe tuples: the C ignored these results.
        _ = ops.configureEndpoint(speed, pipe_intr_in, ep_intr_in, pvnd.dir_in, type_intr, mp);
        _ = ops.configureEndpoint(speed, pipe_intr_out, ep_intr_out, pvnd.dir_out, type_intr, mp);
        s.initialized = true;
        ops.logInfoVal("device-HID ready", speed);
        return err.ok;
    }

    pub fn close(s: *State, ops: anytype) u16 {
        if (!s.initialized) return err.invalid_state;
        _ = ops.deviceAttach(s.speed, false);
        const e = ops.deviceDeinit(s.speed);
        s.initialized = false;
        s.report_desc = null;
        s.hid_desc = null;
        s.setup_cb = null;
        s.setup_ctx = null;
        return e;
    }

    pub fn setDescriptors(s: *State, ops: anytype, report: ?[*]const u8, report_len: u16, hid: ?[*]const u8, hid_len: u16) u16 {
        if (!s.initialized) return err.invalid_state;
        const r = report orelse {
            ops.logError("set_descriptors: report_desc");
            return err.null_ptr;
        };
        const h = hid orelse {
            ops.logError("set_descriptors: hid_desc");
            return err.null_ptr;
        };
        if (report_len == 0 or hid_len == 0) return err.invalid_arg;
        s.report_desc = r;
        s.report_desc_len = report_len;
        s.hid_desc = h;
        s.hid_desc_len = hid_len;
        return err.ok;
    }

    /// A non-zero report ID is queued as its own byte ahead of the payload;
    /// the framed length must still fit the pipe max packet.
    pub fn sendReport(s: *const State, ops: anytype, report_id: u8, payload: ?[*]const u8, len: u16) u16 {
        if (!s.initialized) return err.invalid_state;
        if (payload == null and len != 0) return err.null_ptr;
        if (report_id == 0 and len == 0) return err.invalid_arg;
        const framed: u32 = @as(u32, len) + @intFromBool(report_id != 0);
        if (framed > s.intr_max_packet) return err.invalid_arg;
        if (report_id != 0) {
            const rid = [1]u8{report_id};
            const e = ops.queueIn(s.speed, pipe_intr_in, &rid, 1);
            if (e != err.ok) {
                ops.logError("send_report: rid byte");
                ops.logErrorVal("Error", e);
                return e;
            }
        }
        const empty = [0]u8{};
        return ops.queueIn(s.speed, pipe_intr_in, payload orelse &empty, len);
    }

    pub fn recvReport(s: *const State, ops: anytype, buf: ?[*]u8, max_len: u16, got: ?*u16) u16 {
        const b = buf orelse {
            ops.logError("recv_report: buf");
            return err.null_ptr;
        };
        const g = got orelse {
            ops.logError("recv_report: got_len");
            return err.null_ptr;
        };
        if (!s.initialized) return err.invalid_state;
        if (max_len == 0) return err.invalid_arg;
        var inout: u16 = max_len;
        const e = ops.queueOut(s.speed, pipe_intr_out, b, &inout, true);
        g.* = if (e == err.ok) inout else 0;
        return e;
    }

    pub fn attachSetupHandler(s: *State, cb: ?SetupFn, ctx: ?*anyopaque) u16 {
        if (!s.initialized) return err.invalid_state;
        s.setup_cb = cb;
        s.setup_ctx = ctx;
        return err.ok;
    }

    /// SET_IDLE keeps wValue's high byte; SET_PROTOCOL takes only boot or
    /// report. The other requests carry payloads the application owns.
    fn applyClassSetup(s: *State, p: *const Setup) void {
        switch (p.b_request) {
            req_set_idle => s.idle_rate = @truncate(p.w_value >> 8),
            req_set_protocol => {
                if (p.w_value == proto_boot or p.w_value == proto_report) s.protocol = @truncate(p.w_value);
            },
            else => {},
        }
    }

    pub fn handleSetup(s: *State, ops: anytype, setup: ?*const Setup) u16 {
        const p = setup orelse {
            ops.logError("handle_setup: setup");
            return err.null_ptr;
        };
        if (!s.initialized) return err.invalid_state;
        if (!isClassEnvelope(p.bm_request_type)) return err.not_supported;
        if (!isKnownRequest(p.b_request)) return err.not_supported;
        s.applyClassSetup(p);
        const cb = s.setup_cb orelse return ops.controlResponse(s.speed, true);
        return ops.controlResponse(s.speed, cb(s.setup_ctx, p) == err.ok);
    }

    pub fn getIdle(s: *const State, ops: anytype, out: ?*u8) u16 {
        const o = out orelse {
            ops.logError("get_idle: out_idle_rate");
            return err.null_ptr;
        };
        if (!s.initialized) return err.invalid_state;
        o.* = s.idle_rate;
        return err.ok;
    }

    pub fn getProtocol(s: *const State, ops: anytype, out: ?*u8) u16 {
        const o = out orelse {
            ops.logError("get_protocol: out_protocol");
            return err.null_ptr;
        };
        if (!s.initialized) return err.invalid_state;
        o.* = s.protocol;
        return err.ok;
    }
};
