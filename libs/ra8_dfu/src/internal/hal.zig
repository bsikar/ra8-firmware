//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The seam this driver sits on: the polled `ra8_usb_host_*` primitives and
//! the two `ra8_time` calls, plus the two wire records they take.
//!
//! Every step module is generic over a HAL type rather than calling these
//! `extern`s directly, so the sequence can be driven against a scripted
//! device in `zig build test` with no controller present. `Hardware` is the
//! one binding that reaches real registers.

const std = @import("std");
const Err = @import("err").Err;

/// Which controller instance a call targets. Mirrors `ra8_usb_speed_t`.
pub const Speed = enum(u8) {
    fs = 0,
    hs = 1,
};

/// USB 2.0 chapter-9 setup packet. Mirrors `ra8_usb_setup_t` byte for byte.
pub const Setup = extern struct {
    bm_request_type: u8,
    b_request: u8,
    w_value: u16,
    w_index: u16,
    w_length: u16,
};

comptime {
    std.debug.assert(@sizeOf(Setup) == 8);
    std.debug.assert(@offsetOf(Setup, "bm_request_type") == 0);
    std.debug.assert(@offsetOf(Setup, "b_request") == 1);
    std.debug.assert(@offsetOf(Setup, "w_value") == 2);
    std.debug.assert(@offsetOf(Setup, "w_index") == 4);
    std.debug.assert(@offsetOf(Setup, "w_length") == 6);
}

extern fn ra8_usb_host_init(speed: u8) callconv(.c) u16;
extern fn ra8_usb_host_deinit(speed: u8) callconv(.c) u16;
extern fn ra8_usb_host_bus_reset(speed: u8, assert_reset: bool) callconv(.c) u16;
extern fn ra8_usb_host_set_uact(speed: u8, enable: bool) callconv(.c) u16;
extern fn ra8_usb_host_set_target(speed: u8, dev_addr: u8) callconv(.c) u16;
extern fn ra8_usb_host_line_state(speed: u8) callconv(.c) u16;
extern fn ra8_usb_host_control_xfer(
    speed: u8,
    setup: *const Setup,
    data: ?[*]u8,
    data_len: u16,
    out_received: ?*u16,
) callconv(.c) u16;
extern fn ra8_time_ms() callconv(.c) u32;
extern fn ra8_delay_ms(ms: u32) callconv(.c) void;

/// The real controller. Firmware links this; the tests do not.
pub const Hardware = struct {
    pub fn init(speed: Speed) Err {
        return Err.from(ra8_usb_host_init(@backingInt(speed)));
    }

    pub fn deinit(speed: Speed) Err {
        return Err.from(ra8_usb_host_deinit(@backingInt(speed)));
    }

    pub fn busReset(speed: Speed, assert_reset: bool) Err {
        return Err.from(ra8_usb_host_bus_reset(@backingInt(speed), assert_reset));
    }

    pub fn setUact(speed: Speed, enable: bool) Err {
        return Err.from(ra8_usb_host_set_uact(@backingInt(speed), enable));
    }

    pub fn setTarget(speed: Speed, dev_addr: u8) Err {
        return Err.from(ra8_usb_host_set_target(@backingInt(speed), dev_addr));
    }

    pub fn lineState(speed: Speed) u16 {
        return ra8_usb_host_line_state(@backingInt(speed));
    }

    pub fn controlXfer(
        speed: Speed,
        setup: *const Setup,
        data: ?[]u8,
        data_len: u16,
        out_received: ?*u16,
    ) Err {
        const ptr: ?[*]u8 = if (data) |slice| slice.ptr else null;
        return Err.from(ra8_usb_host_control_xfer(
            @backingInt(speed),
            setup,
            ptr,
            data_len,
            out_received,
        ));
    }

    pub fn timeMs() u32 {
        return ra8_time_ms();
    }

    pub fn delayMs(ms: u32) void {
        ra8_delay_ms(ms);
    }
};
