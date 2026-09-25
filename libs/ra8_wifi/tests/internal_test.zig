//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the pure lifecycle core: the shared value layouts, the state
//! arithmetic, the lease rule, the spent-budget verdict and the backend
//! table's validation order.

const std = @import("std");
const core = @import("implementation");

test "budget constants match the header" {
    try std.testing.expectEqual(@as(u16, 200), core.join_polls);
    try std.testing.expectEqual(@as(u16, 50), core.poll_gap_ms);
    try std.testing.expect(core.join_polls != 0);
    try std.testing.expect(core.poll_gap_ms != 0);
}

test "limits match the header" {
    try std.testing.expectEqual(@as(usize, 6), core.mac_bytes);
    try std.testing.expectEqual(@as(usize, 32), core.ssid_max);
}

test "state enumerators are the C values" {
    try std.testing.expectEqual(@as(u8, 0), @intFromEnum(core.State.down));
    try std.testing.expectEqual(@as(u8, 1), @intFromEnum(core.State.associating));
    try std.testing.expectEqual(@as(u8, 2), @intFromEnum(core.State.associated));
    try std.testing.expectEqual(@as(u8, 3), @intFromEnum(core.State.ip_bound));
}

test "link enumerators are the C values" {
    try std.testing.expectEqual(@as(u8, 0), @intFromEnum(core.Link.down));
    try std.testing.expectEqual(@as(u8, 1), @intFromEnum(core.Link.up));
}

test "shared records keep their C layout" {
    try std.testing.expectEqual(@as(usize, 6), @sizeOf(core.Mac));
    try std.testing.expectEqual(@as(usize, 20), @sizeOf(core.Lease));
    try std.testing.expectEqual(@as(usize, 48), @sizeOf(core.Ap));
    try std.testing.expectEqual(@as(usize, 24), @sizeOf(core.Status));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(core.Lease, "bound"));
    try std.testing.expectEqual(@as(usize, 44), @offsetOf(core.Ap, "authmode"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(core.Status, "ip"));
}

test "zero mac and empty lease are all zeroes" {
    for (core.Mac.zero.octet) |octet| try std.testing.expectEqual(@as(u8, 0), octet);
    try std.testing.expectEqual(@as(u32, 0), core.Lease.empty.ip);
    try std.testing.expect(!core.Lease.empty.bound);
    try std.testing.expectEqual(@as(u8, 0), core.Ap.empty.rssi);
}

test "a zero address is not a bound lease" {
    try std.testing.expect(!core.leaseBound(0));
    try std.testing.expect(core.leaseBound(1));
    try std.testing.expect(core.leaseBound(0xC0A80164));
    try std.testing.expect(core.leaseBound(0xFFFFFFFF));
}

test "associated covers associated and ip_bound only" {
    try std.testing.expect(!core.isAssociated(@intFromEnum(core.State.down)));
    try std.testing.expect(!core.isAssociated(@intFromEnum(core.State.associating)));
    try std.testing.expect(core.isAssociated(@intFromEnum(core.State.associated)));
    try std.testing.expect(core.isAssociated(@intFromEnum(core.State.ip_bound)));
}

test "ip_bound is the topmost state alone" {
    try std.testing.expect(!core.isIpBound(@intFromEnum(core.State.associated)));
    try std.testing.expect(core.isIpBound(@intFromEnum(core.State.ip_bound)));
    try std.testing.expect(!core.isIpBound(@intFromEnum(core.State.down)));
}

test "a link reading maps onto associated or down" {
    try std.testing.expectEqual(
        @intFromEnum(core.State.associated),
        core.stateForLink(@intFromEnum(core.Link.up)),
    );
    try std.testing.expectEqual(
        @intFromEnum(core.State.down),
        core.stateForLink(@intFromEnum(core.Link.down)),
    );
}

test "any byte other than up polls down" {
    var raw: u8 = 2;
    while (raw < 255) : (raw += 1) {
        try std.testing.expectEqual(@intFromEnum(core.State.down), core.stateForLink(raw));
    }
}

test "a radio that never answered reports its own fault" {
    try std.testing.expectEqual(@as(u16, 0x0402), core.waitVerdict(false, 0x0402));
    try std.testing.expectEqual(@as(u16, 0x0203), core.waitVerdict(false, 0x0203));
}

test "a radio that answered but never associated times out" {
    try std.testing.expectEqual(core.err_timeout, core.waitVerdict(true, 0x0402));
    try std.testing.expectEqual(core.err_timeout, core.waitVerdict(true, core.err_ok));
}

test "status derives both flags from the state" {
    const lease: core.Lease = .{ .ip = 0xC0A80164, .bound = true };
    const down = core.statusFrom(@intFromEnum(core.State.down), -40, lease);
    try std.testing.expect(!down.associated);
    try std.testing.expect(!down.ip_bound);
    try std.testing.expectEqual(@as(i8, -40), down.rssi);
    try std.testing.expectEqual(@as(u32, 0xC0A80164), down.ip.ip);

    const assoc = core.statusFrom(@intFromEnum(core.State.associated), -56, lease);
    try std.testing.expect(assoc.associated);
    try std.testing.expect(!assoc.ip_bound);

    const bound = core.statusFrom(@intFromEnum(core.State.ip_bound), -56, lease);
    try std.testing.expect(bound.associated);
    try std.testing.expect(bound.ip_bound);
}

test "a complete table has no missing row" {
    try std.testing.expectEqual(@as(?core.Row, null), core.missingRow(.{}));
}

test "each dented row is named" {
    try std.testing.expectEqual(core.Row.open, core.missingRow(.{ .open = false }).?);
    try std.testing.expectEqual(core.Row.close, core.missingRow(.{ .close = false }).?);
    try std.testing.expectEqual(core.Row.radio_up, core.missingRow(.{ .radio_up = false }).?);
    try std.testing.expectEqual(core.Row.radio_down, core.missingRow(.{ .radio_down = false }).?);
    try std.testing.expectEqual(core.Row.join, core.missingRow(.{ .join = false }).?);
    try std.testing.expectEqual(core.Row.leave, core.missingRow(.{ .leave = false }).?);
    try std.testing.expectEqual(core.Row.service, core.missingRow(.{ .service = false }).?);
    try std.testing.expectEqual(core.Row.get_mac, core.missingRow(.{ .get_mac = false }).?);
    try std.testing.expectEqual(core.Row.get_ap, core.missingRow(.{ .get_ap = false }).?);
    try std.testing.expectEqual(core.Row.idle, core.missingRow(.{ .idle = false }).?);
}

test "validation stops at the first gap in lifecycle, session, query order" {
    // Everything missing: the lifecycle rows are named first.
    const all_gone: core.RowPresence = .{
        .open = false,
        .close = false,
        .radio_up = false,
        .radio_down = false,
        .join = false,
        .leave = false,
        .service = false,
        .get_mac = false,
        .get_ap = false,
        .idle = false,
    };
    try std.testing.expectEqual(core.Row.open, core.missingRow(all_gone).?);
    // A whole lifecycle with a session gap reports the session row, not a query one.
    try std.testing.expectEqual(
        core.Row.leave,
        core.missingRow(.{ .leave = false, .get_ap = false }).?,
    );
    // Session intact: the query rows are the last pass.
    try std.testing.expectEqual(
        core.Row.service,
        core.missingRow(.{ .service = false, .idle = false }).?,
    );
}

test "row messages are the C's check strings" {
    try std.testing.expectEqualStrings("backend.open", std.mem.span(core.Row.open.message()));
    try std.testing.expectEqualStrings("backend.close", std.mem.span(core.Row.close.message()));
    try std.testing.expectEqualStrings("backend.radio_up", std.mem.span(core.Row.radio_up.message()));
    try std.testing.expectEqualStrings(
        "backend.radio_down",
        std.mem.span(core.Row.radio_down.message()),
    );
    try std.testing.expectEqualStrings("backend.join", std.mem.span(core.Row.join.message()));
    try std.testing.expectEqualStrings("backend.leave", std.mem.span(core.Row.leave.message()));
    try std.testing.expectEqualStrings("backend.service", std.mem.span(core.Row.service.message()));
    try std.testing.expectEqualStrings("backend.get_mac", std.mem.span(core.Row.get_mac.message()));
    try std.testing.expectEqualStrings("backend.get_ap", std.mem.span(core.Row.get_ap.message()));
    try std.testing.expectEqualStrings("backend.idle", std.mem.span(core.Row.idle.message()));
}

test "error values are the ra8_err_t codes the facade returns" {
    try std.testing.expectEqual(@as(u16, 0x0000), core.err_ok);
    try std.testing.expectEqual(@as(u16, 0x0104), core.err_invalid_state);
    try std.testing.expectEqual(@as(u16, 0x0108), core.err_timeout);
    try std.testing.expectEqual(@as(u16, 0x010F), core.err_not_initialized);
    try std.testing.expectEqual(@as(u16, 0x0504), core.err_null_ptr);
}
// --- the ESP32-C6 backend's core ------------------------------------------

test "an arena below the link's floor is refused, the floor itself is not" {
    try std.testing.expect(core.arenaTooSmall(0));
    try std.testing.expect(core.arenaTooSmall(core.c6_arena_min - 1));
    try std.testing.expect(!core.arenaTooSmall(core.c6_arena_min));
    try std.testing.expect(!core.arenaTooSmall(core.c6_arena_min + 1));
    try std.testing.expectEqual(@as(u32, 2048), core.c6_arena_min);
}

test "the facade and link widths agree, which is what the straight copies need" {
    try std.testing.expectEqual(core.mac_bytes, core.c6_mac_bytes);
    try std.testing.expectEqual(core.ssid_max, core.c6_ssid_max);
    try std.testing.expectEqual(@as(usize, 64), core.c6_pass_max);
    try std.testing.expectEqual(@as(u16, 8), core.c6_announce_transfers);
}

test "the announcement kinds carry the co-processor's own numbering" {
    try std.testing.expectEqual(@as(u8, 0), @intFromEnum(core.EventKind.boot));
    try std.testing.expectEqual(@as(u8, 1), @intFromEnum(core.EventKind.sta_connected));
    try std.testing.expectEqual(@as(u8, 2), @intFromEnum(core.EventKind.sta_disconnected));
    try std.testing.expectEqual(@as(u8, 3), @intFromEnum(core.EventKind.wifi));
}

test "a connected announcement latches, and keeps the reason it was given" {
    const latched = core.Latches.clear.latch(
        @intFromEnum(core.EventKind.sta_connected),
        7,
    );
    try std.testing.expect(latched.connected);
    try std.testing.expect(!latched.disconnected);
    // A connect carries no reason code, so the field is left where it was.
    try std.testing.expectEqual(@as(u16, 0), latched.reason);
}

test "a disconnected announcement latches and records its reason" {
    const latched = core.Latches.clear.latch(
        @intFromEnum(core.EventKind.sta_disconnected),
        0x0F,
    );
    try std.testing.expect(!latched.connected);
    try std.testing.expect(latched.disconnected);
    try std.testing.expectEqual(@as(u16, 0x0F), latched.reason);
}

test "a boot or bare Wi-Fi announcement leaves every latch alone" {
    const held: core.Latches = .{ .connected = true, .disconnected = false, .reason = 3 };
    for ([_]u8{
        @intFromEnum(core.EventKind.boot),
        @intFromEnum(core.EventKind.wifi),
        9, // a kind this build does not know
        255,
    }) |kind| {
        const after = held.latch(kind, 42);
        try std.testing.expect(after.connected);
        try std.testing.expect(!after.disconnected);
        try std.testing.expectEqual(@as(u16, 3), after.reason);
    }
}

test "a disconnect after a connect keeps both latches set" {
    const after = core.Latches.clear
        .latch(@intFromEnum(core.EventKind.sta_connected), 0)
        .latch(@intFromEnum(core.EventKind.sta_disconnected), 5);
    try std.testing.expect(after.connected);
    try std.testing.expect(after.disconnected);
    try std.testing.expectEqual(@as(u16, 5), after.reason);
}

test "the link reading is down unless connected, and a disconnect wins outright" {
    try std.testing.expectEqual(core.Link.down, core.linkForLatches(false, false));
    try std.testing.expectEqual(core.Link.up, core.linkForLatches(true, false));
    try std.testing.expectEqual(core.Link.down, core.linkForLatches(false, true));
    try std.testing.expectEqual(core.Link.down, core.linkForLatches(true, true));
}

test "the shared vtable layout is the C's ten rows, in the C's order" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(ptr * 10, @sizeOf(core.Backend));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(core.Backend, "open"));
    try std.testing.expectEqual(ptr * 4, @offsetOf(core.Backend, "join"));
    try std.testing.expectEqual(ptr * 9, @offsetOf(core.Backend, "idle"));
    try std.testing.expectEqual(ptr * 4, @sizeOf(core.Config));
}

test "invalid_size is the arena rejection the header documents" {
    try std.testing.expectEqual(@as(u16, 0x0105), core.err_invalid_size);
}
