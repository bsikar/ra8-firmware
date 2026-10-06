//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB device composite multiplexer (RA8FW-773), ported from
//! ra8_usb_composite.c: interface ownership, class validation and
//! collision checks, SETUP routing by bmRequestType, and the starter
//! step machine. The C ABI is in usb_composite_abi.zig.

const std = @import("std");

pub const ok: u16 = 0;
pub const err_no_mem: u16 = 0x102;
pub const err_invalid_arg: u16 = 0x103;
pub const err_invalid_state: u16 = 0x104;
pub const err_not_found: u16 = 0x106;
pub const err_exists: u16 = 0x10C;
pub const err_null_ptr: u16 = 0x504;

pub const max_classes: u8 = 4;
pub const max_ifs: u8 = 16;
pub const max_address: u16 = 127;
/// Handler index reported when the composite layer answered itself.
pub const handler_self: u8 = max_classes;

pub const req_type_mask: u8 = 0x60;
pub const req_type_standard: u8 = 0x00;
pub const std_set_address: u8 = 0x05;

pub const InitFn = *const fn (?*anyopaque) callconv(.c) u16;
pub const SetupFn = *const fn (?*anyopaque, *const Setup) callconv(.c) u16;
pub const CloseFn = *const fn (?*anyopaque) callconv(.c) u16;

/// Mirror of ra8_usb_setup_t.
pub const Setup = extern struct {
    bm_request_type: u8,
    b_request: u8,
    w_value: u16,
    w_index: u16,
    w_length: u16,
};

/// Mirror of ra8_usb_composite_class_t.
pub const Class = extern struct {
    first: u8 = 0,
    count: u8 = 0,
    init: ?InitFn = null,
    handle_setup: ?SetupFn = null,
    close: ?CloseFn = null,
    ctx: ?*anyopaque = null,
};

comptime {
    const p = @sizeOf(usize);
    std.debug.assert(@sizeOf(Setup) == 8);
    std.debug.assert(@offsetOf(Class, "init") == p);
    std.debug.assert(@offsetOf(Class, "ctx") == 4 * p);
    std.debug.assert(@sizeOf(Class) == 5 * p);
}

pub const Phase = enum(u8) { idle, setup_rx, std_dispatch, class_dispatch, done };

/// The C s_state.
pub const Mux = struct {
    initialized: bool = false,
    speed: u8 = 0,
    phase: Phase = .idle,
    class_count: u8 = 0,
    classes: [max_classes]Class = @splat(.{}),
    owner_plus_one: [max_ifs]u8 = @splat(0),
    device_desc: ?[*]const u8 = null,
    config_desc: ?[*]const u8 = null,
    last_handler: u8 = handler_self,

    /// State init() leaves behind for `speed`.
    pub fn fresh(speed: u8) Mux {
        return .{ .initialized = true, .speed = speed };
    }

    /// Close's reset: keeps classes[], phase and last_handler like the C.
    pub fn shut(m: *Mux) void {
        m.initialized = false;
        m.class_count = 0;
        m.device_desc = null;
        m.config_desc = null;
        m.owner_plus_one = @splat(0);
    }

    /// First failing check of register_class after the init guard.
    pub fn admit(m: *const Mux, cl: *const Class) ?u16 {
        if (validate(cl)) |e| return e;
        if (m.class_count >= max_classes) return err_no_mem;
        if (m.collides(cl)) return err_exists;
        return null;
    }

    pub fn collides(m: *const Mux, cl: *const Class) bool {
        for (0..cl.count) |i| {
            if (m.owner_plus_one[cl.first + i] != 0) return true;
        }
        return false;
    }

    /// Stores the class and marks its interfaces; returns its slot.
    pub fn claim(m: *Mux, cl: *const Class) u8 {
        const slot = m.class_count;
        m.classes[slot] = cl.*;
        for (0..cl.count) |i| m.owner_plus_one[cl.first + i] = slot + 1;
        m.class_count = slot + 1;
        return slot;
    }

    pub fn owner(m: *const Mux, if_num: u8) ?u8 {
        if (if_num >= max_ifs) return null;
        const slot = m.owner_plus_one[if_num];
        if (slot == 0) return null;
        return slot - 1;
    }

    pub fn step(m: *Mux) void {
        m.phase = switch (m.phase) {
            .idle => .setup_rx,
            .setup_rx => .std_dispatch,
            .std_dispatch => .class_dispatch,
            .class_dispatch => .done,
            .done => .idle,
        };
    }
};

pub fn validate(cl: *const Class) ?u16 {
    if (cl.init == null or cl.handle_setup == null or cl.close == null) return err_null_ptr;
    if (cl.count == 0) return err_invalid_arg;
    if (@as(u16, cl.first) + cl.count > max_ifs) return err_invalid_arg;
    return null;
}

pub fn isStandard(s: *const Setup) bool {
    return s.bm_request_type & req_type_mask == req_type_standard;
}

/// Interface number a class request targets (wIndex low byte).
pub fn interfaceOf(s: *const Setup) u8 {
    return @truncate(s.w_index);
}

pub fn speedOk(speed: u8) bool {
    return speed == 0 or speed == 1;
}
