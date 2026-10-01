//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the bare Wi-Fi request/answer pairing.

const std = @import("std");
const bare_rpc = @import("implementation").bare_rpc;

const Req = bare_rpc.Req;
const Resp = bare_rpc.Resp;

const bare = [_]struct { req: u32, resp: u32 }{
    .{ .req = Req.wifi_deinit, .resp = Resp.wifi_deinit },
    .{ .req = Req.wifi_start, .resp = Resp.wifi_start },
    .{ .req = Req.wifi_stop, .resp = Resp.wifi_stop },
    .{ .req = Req.wifi_connect, .resp = Resp.wifi_connect },
    .{ .req = Req.wifi_disconnect, .resp = Resp.wifi_disconnect },
};

test "each bare request pairs with its own answer" {
    for (bare) |pair| {
        try std.testing.expectEqual(pair.resp, bare_rpc.respFor(pair.req).?);
    }
}

test "the answers are the ids the co-processor sends" {
    try std.testing.expectEqual(@as(u32, 535), Resp.wifi_deinit);
    try std.testing.expectEqual(@as(u32, 536), Resp.wifi_start);
    try std.testing.expectEqual(@as(u32, 537), Resp.wifi_stop);
    try std.testing.expectEqual(@as(u32, 538), Resp.wifi_connect);
    try std.testing.expectEqual(@as(u32, 539), Resp.wifi_disconnect);
}

test "the requests are the ids this host sends" {
    try std.testing.expectEqual(@as(u32, 279), Req.wifi_deinit);
    try std.testing.expectEqual(@as(u32, 280), Req.wifi_start);
    try std.testing.expectEqual(@as(u32, 281), Req.wifi_stop);
    try std.testing.expectEqual(@as(u32, 282), Req.wifi_connect);
    try std.testing.expectEqual(@as(u32, 283), Req.wifi_disconnect);
}

test "no two bare requests share an answer" {
    for (bare, 0..) |a, i| {
        for (bare[i + 1 ..]) |b| {
            try std.testing.expect(a.req != b.req);
            try std.testing.expect(a.resp != b.resp);
        }
    }
}

test "the Wi-Fi requests that carry a body are not bare" {
    try std.testing.expectEqual(@as(?u32, null), bare_rpc.respFor(260)); // Req_SetWifiMode
    try std.testing.expectEqual(@as(?u32, null), bare_rpc.respFor(278)); // Req_WifiInit
    try std.testing.expectEqual(@as(?u32, null), bare_rpc.respFor(284)); // Req_WifiSetConfig
}

test "an answer id is never mistaken for a request" {
    for (bare) |pair| {
        try std.testing.expectEqual(@as(?u32, null), bare_rpc.respFor(pair.resp));
    }
}

test "zero is not a bare request" {
    try std.testing.expectEqual(@as(?u32, null), bare_rpc.respFor(0));
}

test "exactly five ids in the whole message space are bare" {
    var found: usize = 0;
    var id: u32 = 0;
    while (id < 2048) : (id += 1) {
        if (bare_rpc.respFor(id) != null) found += 1;
    }
    try std.testing.expectEqual(@as(usize, 5), found);
}

test "isBare agrees with respFor everywhere" {
    var id: u32 = 0;
    while (id < 2048) : (id += 1) {
        try std.testing.expectEqual(bare_rpc.respFor(id) != null, bare_rpc.isBare(id));
    }
}

test "isBare accepts the five and nothing beside them" {
    for (bare) |pair| {
        try std.testing.expect(bare_rpc.isBare(pair.req));
    }
    try std.testing.expect(!bare_rpc.isBare(Req.wifi_disconnect + 1));
    try std.testing.expect(!bare_rpc.isBare(Req.wifi_deinit - 1));
}

test "the bare requests are one contiguous run" {
    var id: u32 = Req.wifi_deinit;
    while (id <= Req.wifi_disconnect) : (id += 1) {
        try std.testing.expect(bare_rpc.isBare(id));
    }
}

test "the fixed distance between the numberings is a coincidence, not the rule" {
    // Every pair happens to sit 256 apart today. The map states each answer
    // outright so that renumbering one message breaks a test here rather than
    // silently sending the facade to wait on the wrong answer.
    for (bare) |pair| {
        try std.testing.expectEqual(pair.req + 256, pair.resp);
    }
}

test "the answer never collides with a request id" {
    for (bare) |a| {
        for (bare) |b| {
            try std.testing.expect(a.resp != b.req);
        }
    }
}
