//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for internal/usb_composite.zig (RA8FW-773).

const std = @import("std");
const m = @import("usb_composite");

fn initOk(_: ?*anyopaque) callconv(.c) u16 {
    return 0;
}
fn setupEcho(ctx: ?*anyopaque, s: *const m.Setup) callconv(.c) u16 {
    const seen: *u8 = @ptrCast(ctx.?);
    seen.* = s.b_request;
    return 0;
}

fn class(first: u8, count: u8) m.Class {
    return .{ .first = first, .count = count, .init = initOk, .handle_setup = setupEcho, .close = initOk };
}

test "validate: null callbacks, empty and overflowing ranges" {
    var c = class(0, 2);
    try std.testing.expectEqual(@as(?u16, null), m.validate(&c));
    c.close = null;
    try std.testing.expectEqual(@as(?u16, m.err_null_ptr), m.validate(&c));
    try std.testing.expectEqual(@as(?u16, m.err_invalid_arg), m.validate(&class(3, 0)));
    try std.testing.expectEqual(@as(?u16, m.err_invalid_arg), m.validate(&class(15, 2)));
    try std.testing.expectEqual(@as(?u16, null), m.validate(&class(14, 2)));
}

test "claim marks interfaces and owner maps back to the slot" {
    var mux = m.Mux.fresh(1);
    try std.testing.expectEqual(@as(u8, 0), mux.claim(&class(0, 2)));
    try std.testing.expectEqual(@as(u8, 1), mux.claim(&class(2, 1)));
    try std.testing.expectEqual(@as(?u8, 0), mux.owner(1));
    try std.testing.expectEqual(@as(?u8, 1), mux.owner(2));
    try std.testing.expectEqual(@as(?u8, null), mux.owner(3));
    try std.testing.expectEqual(@as(?u8, null), mux.owner(16));
    try std.testing.expectEqual(@as(u8, 2), mux.class_count);
}

test "admit rejects collisions and a full table" {
    var mux = m.Mux.fresh(0);
    _ = mux.claim(&class(4, 2));
    try std.testing.expectEqual(@as(?u16, m.err_exists), mux.admit(&class(5, 1)));
    try std.testing.expectEqual(@as(?u16, null), mux.admit(&class(6, 1)));
    _ = mux.claim(&class(6, 1));
    _ = mux.claim(&class(7, 1));
    _ = mux.claim(&class(8, 1));
    try std.testing.expectEqual(@as(?u16, m.err_no_mem), mux.admit(&class(9, 1)));
}

test "step cycles through the five phases" {
    var mux = m.Mux.fresh(0);
    const order = [_]m.Phase{ .setup_rx, .std_dispatch, .class_dispatch, .done, .idle };
    for (order) |p| {
        mux.step();
        try std.testing.expectEqual(p, mux.phase);
    }
}

test "routing helpers read bmRequestType and wIndex" {
    const std_req = m.Setup{ .bm_request_type = 0x80, .b_request = 6, .w_value = 0, .w_index = 0, .w_length = 18 };
    const cls_req = m.Setup{ .bm_request_type = 0x21, .b_request = 0x20, .w_value = 0, .w_index = 0x0302, .w_length = 7 };
    try std.testing.expect(m.isStandard(&std_req));
    try std.testing.expect(!m.isStandard(&cls_req));
    try std.testing.expectEqual(@as(u8, 2), m.interfaceOf(&cls_req));
}

test "shut clears ownership but keeps the class array" {
    var mux = m.Mux.fresh(1);
    _ = mux.claim(&class(0, 1));
    mux.shut();
    try std.testing.expect(!mux.initialized);
    try std.testing.expectEqual(@as(u8, 0), mux.class_count);
    try std.testing.expectEqual(@as(?u8, null), mux.owner(0));
    try std.testing.expectEqual(@as(u8, 1), mux.classes[0].count);
}

test "class handle_setup callback receives the setup" {
    var seen: u8 = 0;
    var c = class(0, 1);
    c.ctx = &seen;
    const s = m.Setup{ .bm_request_type = 0x21, .b_request = 0x22, .w_value = 0, .w_index = 0, .w_length = 0 };
    try std.testing.expectEqual(@as(u16, 0), c.handle_setup.?(c.ctx, &s));
    try std.testing.expectEqual(@as(u8, 0x22), seen);
}
