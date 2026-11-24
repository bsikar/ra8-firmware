//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the ESP32-C6 backend's transport-free half: the arena
//! floor, the association flags, the precedence between a connect and a later
//! disconnect, and the two record copies `get_mac` and `get_ap` perform.

const std = @import("std");
const c6link = @import("c6link");
const core = c6link.core;

comptime {
    _ = @import("log_fixture.zig");
}

test "the mirrored widths match the facade's" {
    try std.testing.expectEqual(core.mac_bytes, c6link.c6.mac_bytes);
    try std.testing.expectEqual(core.ssid_max, c6link.c6.ssid_max);
}

test "an arena under the link's floor is refused" {
    try std.testing.expect(c6link.arenaTooSmall(0));
    try std.testing.expect(c6link.arenaTooSmall(c6link.c6.arena_min - 1));
}

test "an arena at or above the floor is accepted" {
    try std.testing.expect(!c6link.arenaTooSmall(c6link.c6.arena_min));
    try std.testing.expect(!c6link.arenaTooSmall(64 * 1024));
}

test "setup copies the board's config into the handle" {
    var arena: [c6link.c6.arena_min]u8 = undefined;
    var link: u32 = 0;
    var handle: c6link.Handle = .{};
    const cfg: c6link.Cfg = .{
        .link = @ptrCast(&link),
        .arena = &arena,
        .arena_bytes = arena.len,
    };

    c6link.applyCfg(&handle, &cfg);

    try std.testing.expect(handle.link == @as(?*anyopaque, @ptrCast(&link)));
    try std.testing.expectEqual(@as(u32, arena.len), handle.arena_bytes);
    try std.testing.expect(handle.arena != null);
}

test "setup leaves a fresh handle down" {
    var handle: c6link.Handle = .{};
    const cfg: c6link.Cfg = .{ .arena_bytes = c6link.c6.arena_min };

    c6link.applyCfg(&handle, &cfg);

    try std.testing.expectEqual(core.Link.down, c6link.linkState(&handle));
}

test "setup clears the latches a previous link left behind" {
    var handle: c6link.Handle = .{};
    const up: c6link.Event = .{ .kind = .sta_connected };
    const down: c6link.Event = .{ .kind = .sta_disconnected, .reason = 0x0F03 };
    c6link.noteEvent(&handle, &up);
    c6link.noteEvent(&handle, &down);
    const cfg: c6link.Cfg = .{ .arena_bytes = c6link.c6.arena_min };

    c6link.applyCfg(&handle, &cfg);

    try std.testing.expect(!handle.connected);
    try std.testing.expect(!handle.disconnected);
    try std.testing.expectEqual(@as(u16, 0), handle.reason);

    // A connect heard on the new link must now bring it up; a disconnect
    // latched from the old one would otherwise pin it down.
    c6link.noteEvent(&handle, &up);
    try std.testing.expectEqual(core.Link.up, c6link.linkState(&handle));
}

test "a connect event brings the link up" {
    var handle: c6link.Handle = .{};
    const ev: c6link.Event = .{ .kind = .sta_connected };

    c6link.noteEvent(&handle, &ev);

    try std.testing.expect(handle.connected);
    try std.testing.expectEqual(core.Link.up, c6link.linkState(&handle));
}

test "a disconnect event records its reason and wins over a connect" {
    var handle: c6link.Handle = .{};
    const up: c6link.Event = .{ .kind = .sta_connected };
    const down: c6link.Event = .{ .kind = .sta_disconnected, .reason = 0x0F03 };

    c6link.noteEvent(&handle, &up);
    c6link.noteEvent(&handle, &down);

    try std.testing.expectEqual(@as(u16, 0x0F03), handle.reason);
    try std.testing.expectEqual(core.Link.down, c6link.linkState(&handle));
}

test "boot and passthrough wifi events say nothing about association" {
    var handle: c6link.Handle = .{};
    const boot: c6link.Event = .{ .kind = .boot, .reset_reason = 3 };
    const wifi: c6link.Event = .{ .kind = .wifi, .wifi_event_id = 42, .reason = 7 };

    c6link.noteEvent(&handle, &boot);
    c6link.noteEvent(&handle, &wifi);

    try std.testing.expect(!handle.connected);
    try std.testing.expect(!handle.disconnected);
    try std.testing.expectEqual(@as(u16, 0), handle.reason);
    try std.testing.expectEqual(core.Link.down, c6link.linkState(&handle));
}

test "arming a join clears a previous attempt's flags" {
    var handle: c6link.Handle = .{ .connected = true, .disconnected = true, .reason = 9 };

    c6link.armJoin(&handle);

    try std.testing.expect(!handle.connected);
    try std.testing.expect(!handle.disconnected);
    try std.testing.expectEqual(@as(u16, 0), handle.reason);
}

test "a stale disconnect cannot decide a re-armed join" {
    var handle: c6link.Handle = .{ .disconnected = true, .reason = 4 };
    const up: c6link.Event = .{ .kind = .sta_connected };

    c6link.armJoin(&handle);
    c6link.noteEvent(&handle, &up);

    try std.testing.expectEqual(core.Link.up, c6link.linkState(&handle));
}

test "get_mac copies every octet in transmission order" {
    const mac: c6link.Mac = .{ .octet = .{ 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 } };
    var out: core.Mac = core.Mac.zero;

    c6link.copyMac(&out, &mac);

    try std.testing.expectEqualSlices(u8, &mac.octet, &out.octet);
}

test "get_ap copies the whole record including the ssid's trailing nul" {
    var info: c6link.ApInfo = .{
        .bssid = .{ .octet = .{ 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF } },
        .ssid_len = 5,
        .channel = 11,
        .rssi = -57,
        .authmode = 4,
    };
    @memcpy(info.ssid[0..5], "ra8ap");
    var out: core.Ap = .{};

    c6link.copyAp(&out, &info);

    try std.testing.expectEqualSlices(u8, &info.bssid.octet, &out.bssid.octet);
    try std.testing.expectEqualStrings("ra8ap", out.ssid[0..out.ssid_len]);
    try std.testing.expectEqual(@as(u8, 0), out.ssid[5]);
    try std.testing.expectEqual(@as(u8, 11), out.channel);
    try std.testing.expectEqual(@as(i8, -57), out.rssi);
    try std.testing.expectEqual(@as(i32, 4), out.authmode);
}

test "an ap record at the ssid capacity copies without truncation" {
    var info: c6link.ApInfo = .{ .ssid_len = @intCast(c6link.c6.ssid_max) };
    @memset(info.ssid[0..c6link.c6.ssid_max], 'z');
    var out: core.Ap = .{};

    c6link.copyAp(&out, &info);

    try std.testing.expectEqual(@as(usize, c6link.c6.ssid_max), out.ssid_len);
    try std.testing.expectEqual(@as(u8, 'z'), out.ssid[c6link.c6.ssid_max - 1]);
    try std.testing.expectEqual(@as(u8, 0), out.ssid[c6link.c6.ssid_max]);
}

test "the ap record and the facade's agree on shape, which is what the copy assumes" {
    try std.testing.expectEqual(@sizeOf(core.Ap), @sizeOf(c6link.ApInfo));
    try std.testing.expectEqual(@alignOf(core.Ap), @alignOf(c6link.ApInfo));
    try std.testing.expectEqual(@offsetOf(core.Ap, "bssid"), @offsetOf(c6link.ApInfo, "bssid"));
    try std.testing.expectEqual(@offsetOf(core.Ap, "ssid"), @offsetOf(c6link.ApInfo, "ssid"));
    try std.testing.expectEqual(@offsetOf(core.Ap, "ssid_len"), @offsetOf(c6link.ApInfo, "ssid_len"));
    try std.testing.expectEqual(@offsetOf(core.Ap, "channel"), @offsetOf(c6link.ApInfo, "channel"));
    try std.testing.expectEqual(@offsetOf(core.Ap, "rssi"), @offsetOf(c6link.ApInfo, "rssi"));
    try std.testing.expectEqual(
        @offsetOf(core.Ap, "authmode"),
        @offsetOf(c6link.ApInfo, "authmode"),
    );
}

test "normalizeScanRecords copies multiple scanned records to facade storage" {
    var src: [2]c6link.ApInfo = .{ .{}, .{} };
    src[0].bssid = .{ .octet = .{ 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 } };
    src[0].ssid_len = 4;
    src[0].channel = 1;
    src[0].rssi = -48;
    src[0].authmode = 3;
    @memcpy(src[0].ssid[0..4], "benc");

    src[1].bssid = .{ .octet = .{ 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF } };
    src[1].ssid_len = @intCast(c6link.c6.ssid_max);
    src[1].channel = 11;
    src[1].rssi = -71;
    src[1].authmode = 0;
    @memset(src[1].ssid[0..c6link.c6.ssid_max], 'k');

    var dst: [2]core.Ap = .{ core.Ap.empty, core.Ap.empty };
    c6link.normalizeScanRecords(&dst, &src, 2);

    try std.testing.expectEqualSlices(u8, &src[0].bssid.octet, &dst[0].bssid.octet);
    try std.testing.expectEqualStrings("benc", dst[0].ssid[0..dst[0].ssid_len]);
    try std.testing.expectEqual(@as(u8, 0), dst[0].ssid[4]);
    try std.testing.expectEqual(@as(u8, 1), dst[0].channel);
    try std.testing.expectEqual(@as(i8, -48), dst[0].rssi);
    try std.testing.expectEqual(@as(i32, 3), dst[0].authmode);

    try std.testing.expectEqualSlices(u8, &src[1].bssid.octet, &dst[1].bssid.octet);
    try std.testing.expectEqual(@as(u8, @intCast(c6link.c6.ssid_max)), dst[1].ssid_len);
    try std.testing.expectEqual(@as(u8, 0), dst[1].ssid[c6link.c6.ssid_max]);
    try std.testing.expectEqual(@as(i32, 0), dst[1].authmode);
}

test "normalizeScanRecords handles same-address in-place conversion cleanly" {
    var buf: [2]c6link.ApInfo = .{ .{}, .{} };
    buf[0].bssid = .{ .octet = .{ 1, 2, 3, 4, 5, 6 } };
    buf[0].ssid_len = 3;
    buf[0].channel = 6;
    buf[0].rssi = -60;
    buf[0].authmode = 4;
    @memcpy(buf[0].ssid[0..3], "ap0");

    buf[1].bssid = .{ .octet = .{ 6, 5, 4, 3, 2, 1 } };
    buf[1].ssid_len = 3;
    buf[1].channel = 13;
    buf[1].rssi = -79;
    buf[1].authmode = 2;
    @memcpy(buf[1].ssid[0..3], "ap1");

    const as_ap: [*]core.Ap = @ptrCast(@alignCast(&buf));
    c6link.normalizeScanRecords(as_ap, &buf, 2);

    try std.testing.expectEqualStrings("ap0", as_ap[0].ssid[0..as_ap[0].ssid_len]);
    try std.testing.expectEqual(@as(i8, -60), as_ap[0].rssi);
    try std.testing.expectEqualStrings("ap1", as_ap[1].ssid[0..as_ap[1].ssid_len]);
    try std.testing.expectEqual(@as(i32, 2), as_ap[1].authmode);
}

test "the sta config the join stack zeroes has the layout the link expects" {
    try std.testing.expectEqual(@as(usize, 108), @sizeOf(c6link.StaCfg));
    try std.testing.expectEqual(@as(usize, 33), @offsetOf(c6link.StaCfg, "pass"));
    try std.testing.expectEqual(@as(usize, 107), @offsetOf(c6link.StaCfg, "bssid_set"));
}

test "the event record the link fills has the layout this file reads" {
    try std.testing.expectEqual(@as(usize, 56), @sizeOf(c6link.Event));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(c6link.Event, "reason"));
    try std.testing.expectEqual(@as(usize, 22), @offsetOf(c6link.Event, "ssid"));
}
