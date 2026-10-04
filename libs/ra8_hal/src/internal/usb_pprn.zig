//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB device printer class (RA8FW-586), ported from ra8_usb_pprn.c. The
//! state and every rule live here; the ra8_usb_* primitives and logging
//! come in through an `ops` value so the host tests can record them.
//! USB Printer Class 1.1 sec 4.2 (GET_DEVICE_ID, GET_PORT_STATUS, SOFT_RESET).

const pvnd = @import("usb_pvnd.zig");

pub const codes = pvnd.codes;
const err = codes;
pub const Setup = pvnd.Setup;
pub const SetupFn = pvnd.SetupFn;
pub const speed_fs = pvnd.speed_fs;
pub const speed_hs = pvnd.speed_hs;

pub const pipe_bulk_out: u8 = 3;
pub const pipe_bulk_in: u8 = 4;
pub const ep_bulk_out: u8 = 1;
pub const ep_bulk_in: u8 = 2;
pub const max_packet_fs: u16 = 64;
pub const max_packet_hs: u16 = 512;

pub const bm_class_iface_in: u8 = 0xA1;
pub const bm_class_iface_out: u8 = 0x21;
pub const req_get_device_id: u8 = 0x00;
pub const req_get_port_status: u8 = 0x01;
pub const req_soft_reset: u8 = 0x02;
/// SELECT (bit 4) and NOT_ERROR (bit 3): online with no error.
pub const default_port_status: u8 = (1 << 4) | (1 << 3);

pub fn bulkMaxPacket(speed: u8) u16 {
    return if (speed == speed_hs) max_packet_hs else max_packet_fs;
}

pub fn isClassEnvelope(bm: u8) bool {
    return bm == bm_class_iface_in or bm == bm_class_iface_out;
}

pub fn isKnownRequest(b_request: u8) bool {
    return b_request == req_get_device_id or b_request == req_get_port_status or
        b_request == req_soft_reset;
}

pub const State = struct {
    initialized: bool = false,
    speed: u8 = speed_fs,
    bulk_max_packet: u16 = 0,
    desc: ?[*]const u8 = null,
    desc_len: u16 = 0,
    device_id: ?[*]const u8 = null,
    device_id_len: u16 = 0,
    port_status: u8 = 0,
    setup_cb: ?SetupFn = null,
    setup_ctx: ?*anyopaque = null,

    fn resetShadow(s: *State, speed: u8) void {
        s.speed = speed;
        s.bulk_max_packet = bulkMaxPacket(speed);
        s.desc = null;
        s.desc_len = 0;
        s.device_id = null;
        s.device_id_len = 0;
        s.port_status = default_port_status;
        s.setup_cb = null;
        s.setup_ctx = null;
    }

    pub fn init(s: *State, ops: anytype, speed: u8) u16 {
        if (speed != speed_fs and speed != speed_hs) return err.invalid_arg;
        const usb_err = ops.deviceInit(speed);
        if (usb_err != err.ok) {
            ops.logErrorVal("ra8_usb_device_init failed", usb_err);
            return err.hw_init_failed;
        }
        s.resetShadow(speed);
        const mp = s.bulk_max_packet;
        // Validated speed and fixed pipe tuples: the C ignored these results.
        _ = ops.configureEndpoint(speed, pipe_bulk_out, ep_bulk_out, pvnd.dir_out, pvnd.type_bulk, mp);
        _ = ops.configureEndpoint(speed, pipe_bulk_in, ep_bulk_in, pvnd.dir_in, pvnd.type_bulk, mp);
        s.initialized = true;
        ops.logInfoVal("device-Printer ready", speed);
        return err.ok;
    }

    pub fn close(s: *State, ops: anytype) u16 {
        if (!s.initialized) return err.invalid_state;
        _ = ops.deviceAttach(s.speed, false);
        const e = ops.deviceDeinit(s.speed);
        s.initialized = false;
        s.desc = null;
        s.device_id = null;
        s.setup_cb = null;
        s.setup_ctx = null;
        return e;
    }

    /// `device_id` is optional, but its pointer and length must agree.
    pub fn setDescriptors(s: *State, ops: anytype, desc: ?[*]const u8, len: u16, id: ?[*]const u8, id_len: u16) u16 {
        if (!s.initialized) return err.invalid_state;
        const d = desc orelse {
            ops.logError("set_descriptors: desc");
            return err.null_ptr;
        };
        if (len == 0) return err.invalid_arg;
        if ((id != null) != (id_len != 0)) return err.invalid_arg;
        s.desc = d;
        s.desc_len = len;
        s.device_id = id;
        s.device_id_len = id_len;
        return err.ok;
    }

    pub fn recv(s: *const State, ops: anytype, buf: ?[*]u8, max_len: u16, got: ?*u16) u16 {
        const b = buf orelse {
            ops.logError("recv: buf");
            return err.null_ptr;
        };
        const g = got orelse {
            ops.logError("recv: got_len");
            return err.null_ptr;
        };
        if (!s.initialized) return err.invalid_state;
        if (max_len == 0) return err.invalid_arg;
        var inout: u16 = max_len;
        const e = ops.queueOut(s.speed, pipe_bulk_out, b, &inout, true);
        g.* = if (e == err.ok) inout else 0;
        return e;
    }

    pub fn send(s: *const State, ops: anytype, data: ?[*]const u8, len: u16) u16 {
        if (!s.initialized) return err.invalid_state;
        if (data == null and len != 0) return err.null_ptr;
        if (len == 0 or len > s.bulk_max_packet) return err.invalid_arg;
        return ops.queueIn(s.speed, pipe_bulk_in, data.?, len);
    }

    pub fn setPortStatus(s: *State, status: u8) u16 {
        if (!s.initialized) return err.invalid_state;
        s.port_status = status;
        return err.ok;
    }

    pub fn getPortStatus(s: *const State, ops: anytype, out: ?*u8) u16 {
        const o = out orelse {
            ops.logError("get_port_status: out_status");
            return err.null_ptr;
        };
        if (!s.initialized) return err.invalid_state;
        o.* = s.port_status;
        return err.ok;
    }

    pub fn attachSetupHandler(s: *State, cb: ?SetupFn, ctx: ?*anyopaque) u16 {
        if (!s.initialized) return err.invalid_state;
        s.setup_cb = cb;
        s.setup_ctx = ctx;
        return err.ok;
    }

    /// A known class request is accepted unless the handler returns an error.
    /// SOFT_RESET flushes nothing here; the handler may re-arm the pipes.
    pub fn handleSetup(s: *const State, ops: anytype, setup: ?*const Setup) u16 {
        const p = setup orelse {
            ops.logError("handle_setup: setup");
            return err.null_ptr;
        };
        if (!s.initialized) return err.invalid_state;
        if (!isClassEnvelope(p.bm_request_type)) return err.not_supported;
        if (!isKnownRequest(p.b_request)) return err.not_supported;
        const cb = s.setup_cb orelse return ops.controlResponse(s.speed, true);
        return ops.controlResponse(s.speed, cb(s.setup_ctx, p) == err.ok);
    }
};
