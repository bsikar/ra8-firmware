//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the pure nag policy: clamping, the two re-arm ORs,
//! the two raise ANDs, the worse-of-two rule on a large drop, and the label
//! map. The C host suite still covers the same ground through the ABI; these
//! cases pin the policy itself, including states the C tests can only reach
//! indirectly through the public update.

const std = @import("std");
const implementation = @import("implementation");

const Monitor = implementation.Monitor;
const Nag = implementation.Nag;

fn freshMonitor() Monitor {
    var mon: Monitor = .{ .low_raised = true, .critical_raised = true };
    mon.reset();
    return mon;
}

test "reset arms both bands" {
    var mon: Monitor = .{ .low_raised = true, .critical_raised = true };
    mon.reset();
    try std.testing.expect(!mon.low_raised);
    try std.testing.expect(!mon.critical_raised);
}

test "thresholds match the published header values" {
    try std.testing.expectEqual(@as(u8, 20), implementation.low_pct);
    try std.testing.expectEqual(@as(u8, 10), implementation.critical_pct);
    try std.testing.expectEqual(@as(u8, 3), implementation.rearm_margin);
    try std.testing.expectEqual(@as(u8, 100), implementation.pct_max);
    try std.testing.expectEqual(@as(u8, 23), implementation.low_rearm_pct);
    try std.testing.expectEqual(@as(u8, 13), implementation.critical_rearm_pct);
}

test "clampSoc caps at the ceiling and passes everything below through" {
    try std.testing.expectEqual(@as(u8, 0), implementation.clampSoc(0));
    try std.testing.expectEqual(@as(u8, 55), implementation.clampSoc(55));
    try std.testing.expectEqual(@as(u8, 100), implementation.clampSoc(100));
    try std.testing.expectEqual(@as(u8, 100), implementation.clampSoc(101));
    try std.testing.expectEqual(@as(u8, 100), implementation.clampSoc(200));
    try std.testing.expectEqual(@as(u8, 100), implementation.clampSoc(255));
}

test "rearms is true on charging regardless of SOC" {
    try std.testing.expect(implementation.rearms(true, 0, implementation.low_rearm_pct));
    try std.testing.expect(implementation.rearms(true, 100, implementation.low_rearm_pct));
}

test "rearms needs SOC strictly above the margin when not charging" {
    try std.testing.expect(!implementation.rearms(false, 23, implementation.low_rearm_pct));
    try std.testing.expect(implementation.rearms(false, 24, implementation.low_rearm_pct));
    try std.testing.expect(!implementation.rearms(false, 13, implementation.critical_rearm_pct));
    try std.testing.expect(implementation.rearms(false, 14, implementation.critical_rearm_pct));
}

test "raises is inclusive at the threshold and false once already raised" {
    try std.testing.expect(implementation.raises(20, implementation.low_pct, false));
    try std.testing.expect(!implementation.raises(21, implementation.low_pct, false));
    try std.testing.expect(!implementation.raises(20, implementation.low_pct, true));
    try std.testing.expect(implementation.raises(10, implementation.critical_pct, false));
    try std.testing.expect(!implementation.raises(11, implementation.critical_pct, false));
}

test "healthy reading is quiet" {
    var mon = freshMonitor();
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 72, false));
    try std.testing.expect(!mon.low_raised);
}

test "low band fires once on the descent and stays quiet below" {
    var mon = freshMonitor();
    try std.testing.expectEqual(Nag.low, implementation.step(&mon, 20, false));
    try std.testing.expect(mon.low_raised);
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 18, false));
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 15, false));
}

test "critical band fires once on the descent and stays quiet below" {
    var mon = freshMonitor();
    _ = implementation.step(&mon, 20, false);
    try std.testing.expectEqual(Nag.critical, implementation.step(&mon, 10, false));
    try std.testing.expect(mon.critical_raised);
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 8, false));
}

test "a drop straight into both bands reports the worse one" {
    var mon = freshMonitor();
    try std.testing.expectEqual(Nag.critical, implementation.step(&mon, 4, false));
    try std.testing.expect(mon.low_raised);
    try std.testing.expect(mon.critical_raised);
}

test "empty battery from fresh raises critical" {
    var mon = freshMonitor();
    try std.testing.expectEqual(Nag.critical, implementation.step(&mon, 0, false));
}

test "an over-range reading clamps to full and is quiet" {
    var mon = freshMonitor();
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 200, false));
    try std.testing.expect(!mon.low_raised);
    try std.testing.expect(!mon.critical_raised);
}

test "low re-arms only past its margin" {
    var mon = freshMonitor();
    try std.testing.expectEqual(Nag.low, implementation.step(&mon, 20, false));
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 22, false));
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 20, false));
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 24, false));
    try std.testing.expectEqual(Nag.low, implementation.step(&mon, 20, false));
}

test "critical re-arms only past its margin" {
    var mon = freshMonitor();
    try std.testing.expectEqual(Nag.critical, implementation.step(&mon, 10, false));
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 12, false));
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 10, false));
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 14, false));
    try std.testing.expectEqual(Nag.critical, implementation.step(&mon, 10, false));
}

test "charging is silent and re-arms both bands" {
    var mon = freshMonitor();
    _ = implementation.step(&mon, 4, false);
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 5, true));
    try std.testing.expect(!mon.low_raised);
    try std.testing.expect(!mon.critical_raised);
    try std.testing.expectEqual(Nag.critical, implementation.step(&mon, 5, false));
}

test "charging at a healthy SOC is also silent" {
    var mon = freshMonitor();
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 90, true));
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 2, true));
}

test "a full recovery then a fresh descent nags both bands again" {
    var mon = freshMonitor();
    try std.testing.expectEqual(Nag.critical, implementation.step(&mon, 6, false));
    try std.testing.expectEqual(Nag.none, implementation.step(&mon, 100, false));
    try std.testing.expectEqual(Nag.low, implementation.step(&mon, 20, false));
    try std.testing.expectEqual(Nag.critical, implementation.step(&mon, 10, false));
}

test "the low band holds while SOC sits inside the hysteresis band" {
    var mon = freshMonitor();
    _ = implementation.step(&mon, 20, false);
    var soc: u8 = 21;
    while (soc <= 23) : (soc += 1) {
        try std.testing.expectEqual(Nag.none, implementation.step(&mon, soc, false));
        try std.testing.expect(mon.low_raised);
    }
}

test "label covers every enumerator and the out-of-range fallback" {
    try std.testing.expectEqualStrings("OK", std.mem.span(implementation.label(0)));
    try std.testing.expectEqualStrings("LOW", std.mem.span(implementation.label(1)));
    try std.testing.expectEqualStrings("CRITICAL", std.mem.span(implementation.label(2)));
    try std.testing.expectEqualStrings("?", std.mem.span(implementation.label(3)));
    try std.testing.expectEqualStrings("?", std.mem.span(implementation.label(200)));
    try std.testing.expectEqualStrings("?", std.mem.span(implementation.label(255)));
}

test "monitor is two bytes with the C field order" {
    try std.testing.expectEqual(@as(usize, 2), @sizeOf(Monitor));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(Monitor, "low_raised"));
    try std.testing.expectEqual(@as(usize, 1), @offsetOf(Monitor, "critical_raised"));
}

test "every SOC from 0 to 255 is decided without a panic" {
    var soc: u9 = 0;
    while (soc <= 255) : (soc += 1) {
        var mon = freshMonitor();
        const nag = implementation.step(&mon, @intCast(soc), false);
        const expected: Nag = if (soc <= implementation.critical_pct)
            .critical
        else if (soc <= implementation.low_pct)
            .low
        else
            .none;
        try std.testing.expectEqual(expected, nag);
    }
}
