//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB host hub class driver (RA8FW-771).

const std = @import("std");
const h = @import("usb_hhub");

const eq = std.testing.expectEqual;
const expect = std.testing.expect;

const Fake = struct {
    resets: [4]bool = undefined,
    n_reset: usize = 0,
    addr: u8 = 0,
    setups: [8]h.Setup = undefined,
    n_setup: usize = 0,
    rc: u16 = 0,

    pub fn busReset(self: *Fake, _: u8, assert_reset: bool) u16 {
        self.resets[self.n_reset] = assert_reset;
        self.n_reset += 1;
        return self.rc;
    }
    pub fn setAddress(self: *Fake, _: u8, address: u8) u16 {
        self.addr = address;
        return 0;
    }
    pub fn setup(self: *Fake, _: u8, s: h.Setup) u16 {
        self.setups[self.n_setup] = s;
        self.n_setup += 1;
        return self.rc;
    }
};

var seen: ?h.Device = null;
var seen_ctx: ?*anyopaque = null;

fn onAttach(ctx: ?*anyopaque, dev: *const h.Device) callconv(.c) void {
    seen = dev.*;
    seen_ctx = ctx;
}

test "request envelopes" {
    try eq(h.Setup{ .bm_request_type = 0x80, .b_request = 6, .w_value = 0x0100, .w_index = 0, .w_length = 18 }, h.getDescriptor(1, 18));
    try eq(h.Setup{ .bm_request_type = 0, .b_request = 5, .w_value = 1, .w_index = 0, .w_length = 0 }, h.setAddress(1));
    try eq(h.Setup{ .bm_request_type = 0, .b_request = 9, .w_value = 1, .w_index = 0, .w_length = 0 }, h.setConfig(1));
    try eq(h.Setup{ .bm_request_type = 0xA0, .b_request = 6, .w_value = 0x2900, .w_index = 0, .w_length = 9 }, h.getHubDescriptor());
    try eq(h.Setup{ .bm_request_type = 0xA3, .b_request = 0, .w_value = 0, .w_index = 3, .w_length = 4 }, h.portStatus(3));
    try eq(h.Setup{ .bm_request_type = 0x23, .b_request = 3, .w_value = 4, .w_index = 2, .w_length = 0 }, h.portFeature(2, 4, true));
    try eq(@as(u8, 1), h.portFeature(2, 4, false).b_request);
}

test "seven steps walk to attach and fire the callback once" {
    var hub = h.Hub{ .initialized = true, .speed = 1 };
    var token: u8 = 0;
    hub.attach_cb = onAttach;
    hub.attach_ctx = &token;
    seen = null;
    var f = Fake{};
    var i: usize = 0;
    while (i < 7) : (i += 1) try eq(@as(u16, 0), hub.advance(&f));
    try eq(h.Step.done, hub.step);
    try expect(hub.attached);
    try eq(@as(usize, 2), f.n_reset);
    try expect(f.resets[0] and !f.resets[1]);
    try eq(@as(u8, 1), f.addr);
    try eq(@as(usize, 5), f.n_setup);
    try eq(@as(u8, 5), f.setups[0].b_request);
    try eq(@as(u16, 0x0200), f.setups[2].w_value);
    try eq(@as(u8, 0xA0), f.setups[4].bm_request_type);
    try eq(h.Device{ .device_address = 1, .port_count = 4 }, seen.?);
    try eq(@as(?*anyopaque, &token), seen_ctx);
    seen = null;
    try eq(@as(u16, 0), hub.advance(&f));
    try eq(@as(?h.Device, null), seen);
}

test "a failing step still advances and reports the error" {
    var hub = h.Hub{ .initialized = true };
    var f = Fake{ .rc = 0x203 };
    try eq(@as(u16, 0x203), hub.advance(&f));
    try eq(h.Step.bus_reset, hub.step);
}

test "port range is 1 to port_count" {
    var hub = h.Hub{};
    hub.device.port_count = 4;
    try expect(!hub.portOk(0));
    try expect(hub.portOk(1));
    try expect(hub.portOk(4));
    try expect(!hub.portOk(5));
}
