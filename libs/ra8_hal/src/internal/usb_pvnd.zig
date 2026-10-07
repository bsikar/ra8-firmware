//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB device vendor class (RA8FW-582), ported from ra8_usb_pvnd.c. The
//! state and every rule live here; the ra8_usb_* primitives and logging
//! come in through an `ops` value so the host tests can record them.

const err = struct {
    pub const ok: u16 = 0x000;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const not_supported: u16 = 0x107;
    pub const hw_init_failed: u16 = 0x201;
    pub const null_ptr: u16 = 0x504;
};
pub const codes = err;

pub const speed_fs: u8 = 0;
pub const speed_hs: u8 = 1;
pub const pipe_bulk_in: u8 = 5;
pub const pipe_bulk_out: u8 = 1;
pub const ep_bulk_in: u8 = 1;
pub const ep_bulk_out: u8 = 2;
pub const dir_out: u8 = 0;
pub const dir_in: u8 = 1;
pub const type_bulk: u8 = 0;
pub const max_packet_fs: u16 = 64;
pub const max_packet_hs: u16 = 512;

/// Mirrors ra8_usb_setup_t (8 bytes).
pub const Setup = extern struct {
    bm_request_type: u8,
    b_request: u8,
    w_value: u16,
    w_index: u16,
    w_length: u16,
};

pub const SetupFn = *const fn (?*anyopaque, *const Setup) callconv(.c) u16;

const vendor_envelopes = [_]u8{ 0xC0, 0x40, 0xC1, 0x41, 0xC2, 0x42 };

pub fn isVendorEnvelope(bm: u8) bool {
    for (vendor_envelopes) |v| {
        if (v == bm) return true;
    }
    return false;
}

pub fn bulkMaxPacket(speed: u8) u16 {
    return if (speed == speed_hs) max_packet_hs else max_packet_fs;
}

pub const State = struct {
    initialized: bool = false,
    speed: u8 = speed_fs,
    bulk_max_packet: u16 = 0,
    desc: ?[*]const u8 = null,
    desc_len: u16 = 0,
    setup_cb: ?SetupFn = null,
    setup_ctx: ?*anyopaque = null,

    pub fn init(s: *State, ops: anytype, speed: u8) u16 {
        if (speed != speed_fs and speed != speed_hs) return err.invalid_arg;
        const usb_err = ops.deviceInit(speed);
        if (usb_err != err.ok) {
            ops.logErrorVal("ra8_usb_device_init failed", usb_err);
            return err.hw_init_failed;
        }
        s.speed = speed;
        s.bulk_max_packet = bulkMaxPacket(speed);
        s.desc = null;
        s.desc_len = 0;
        s.setup_cb = null;
        s.setup_ctx = null;
        const mp = s.bulk_max_packet;
        // Validated speed and fixed pipe tuples: the C ignored these results.
        _ = ops.configureEndpoint(speed, pipe_bulk_in, ep_bulk_in, dir_in, type_bulk, mp);
        _ = ops.configureEndpoint(speed, pipe_bulk_out, ep_bulk_out, dir_out, type_bulk, mp);
        s.initialized = true;
        ops.logInfoVal("device-Vendor ready", speed);
        return err.ok;
    }

    pub fn close(s: *State, ops: anytype) u16 {
        if (!s.initialized) return err.invalid_state;
        _ = ops.deviceAttach(s.speed, false);
        const e = ops.deviceDeinit(s.speed);
        s.initialized = false;
        s.desc = null;
        s.setup_cb = null;
        s.setup_ctx = null;
        return e;
    }

    pub fn setDescriptors(s: *State, ops: anytype, desc: ?[*]const u8, len: u16) u16 {
        if (!s.initialized) return err.invalid_state;
        const d = desc orelse {
            ops.logError("set_descriptors: desc");
            return err.null_ptr;
        };
        if (len == 0) return err.invalid_arg;
        s.desc = d;
        s.desc_len = len;
        return err.ok;
    }

    pub fn send(s: *const State, ops: anytype, data: ?[*]const u8, len: u16) u16 {
        if (!s.initialized) return err.invalid_state;
        if (data == null and len != 0) return err.null_ptr;
        if (len == 0 or len > s.bulk_max_packet) return err.invalid_arg;
        return ops.queueIn(s.speed, pipe_bulk_in, data.?, len);
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

    pub fn attachSetupHandler(s: *State, cb: ?SetupFn, ctx: ?*anyopaque) u16 {
        if (!s.initialized) return err.invalid_state;
        s.setup_cb = cb;
        s.setup_ctx = ctx;
        return err.ok;
    }

    pub fn handleSetup(s: *const State, ops: anytype, setup: ?*const Setup) u16 {
        const p = setup orelse {
            ops.logError("handle_setup: setup");
            return err.null_ptr;
        };
        if (!s.initialized) return err.invalid_state;
        if (!isVendorEnvelope(p.bm_request_type)) return err.not_supported;
        // No handler, or a handler error, stalls the vendor request.
        const cb = s.setup_cb orelse return ops.controlResponse(s.speed, false);
        return ops.controlResponse(s.speed, cb(s.setup_ctx, p) == err.ok);
    }
};
