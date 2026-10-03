//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_wifi_stop` and `ra8_c6link_wifi_leave` against a scripted
//! bare-request layer: the ids they send, the order, which error wins, and
//! the guards in front of them.

const std = @import("std");
const wifi = @import("wifi_abi");

const c = wifi.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const not_initialized: u16 = 0x10F;
    pub const null_ptr: u16 = 0x504;
    pub const hw_timeout: u16 = 0x203;
    pub const protocol_error: u16 = 0x406;
};

const Script = struct {
    sent: [4]u32 = .{ 0, 0, 0, 0 },
    count: usize = 0,
    replies: [4]u16 = .{ 0, 0, 0, 0 },
};

var script: Script = .{};

export fn priv_c6link_bare_req(link: ?*c.ra8_c6link_t, req_id: u32) callconv(.c) c.ra8_err_t {
    _ = link;
    const at = script.count;
    script.sent[at] = req_id;
    script.count += 1;
    return script.replies[at];
}

fn openLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.open = true;
    return link;
}

test "stop sends WifiStop then WifiDeinit and returns ok" {
    script = .{};
    var link = openLink();
    try std.testing.expectEqual(Code.ok, wifi.ra8_c6link_wifi_stop(&link));
    try std.testing.expectEqual(@as(usize, 2), script.count);
    try std.testing.expectEqual(wifi.Id.stop, script.sent[0]);
    try std.testing.expectEqual(wifi.Id.deinit, script.sent[1]);
}

test "stop still deinits when stop fails, and stop's error wins" {
    script = .{ .replies = .{ Code.hw_timeout, Code.protocol_error, 0, 0 } };
    var link = openLink();
    try std.testing.expectEqual(Code.hw_timeout, wifi.ra8_c6link_wifi_stop(&link));
    try std.testing.expectEqual(@as(usize, 2), script.count);
}

test "stop reports deinit's error when stop succeeded" {
    script = .{ .replies = .{ Code.ok, Code.protocol_error, 0, 0 } };
    var link = openLink();
    try std.testing.expectEqual(Code.protocol_error, wifi.ra8_c6link_wifi_stop(&link));
}

test "leave sends WifiDisconnect and passes its result on" {
    script = .{ .replies = .{ Code.hw_timeout, 0, 0, 0 } };
    var link = openLink();
    try std.testing.expectEqual(Code.hw_timeout, wifi.ra8_c6link_wifi_leave(&link));
    try std.testing.expectEqual(@as(usize, 1), script.count);
    try std.testing.expectEqual(wifi.Id.disconnect, script.sent[0]);
}

test "a null or closed link sends nothing" {
    script = .{};
    var closed = std.mem.zeroes(c.ra8_c6link_t);
    try std.testing.expectEqual(Code.null_ptr, wifi.ra8_c6link_wifi_stop(null));
    try std.testing.expectEqual(Code.null_ptr, wifi.ra8_c6link_wifi_leave(null));
    try std.testing.expectEqual(Code.not_initialized, wifi.ra8_c6link_wifi_stop(&closed));
    try std.testing.expectEqual(Code.not_initialized, wifi.ra8_c6link_wifi_leave(&closed));
    try std.testing.expectEqual(@as(usize, 0), script.count);
}
