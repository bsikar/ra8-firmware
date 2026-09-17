//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Policy-core tests: the config gate, the wrapping deadline arithmetic, the
//! bounded name copy and the registry verdict.

const std = @import("std");
const core = @import("implementation");

fn goodCfg() core.CfgView {
    return .{
        .has_stack = true,
        .stack_size_bytes = 1024,
        .priority = 4,
        .refresh_period_ms = 50,
    };
}

test "validateCfg accepts a populated block" {
    try std.testing.expect(core.validateCfg(goodCfg()) == null);
}

test "validateCfg reports null_ptr for a missing block" {
    try std.testing.expectEqual(core.CfgFault.null_ptr, core.validateCfg(null).?);
}

test "validateCfg reports null_ptr for a missing stack" {
    var cfg = goodCfg();
    cfg.has_stack = false;
    try std.testing.expectEqual(core.CfgFault.null_ptr, core.validateCfg(cfg).?);
}

test "validateCfg reports invalid_arg below the stack floor" {
    var cfg = goodCfg();
    cfg.stack_size_bytes = core.min_stack_bytes - 1;
    try std.testing.expectEqual(core.CfgFault.invalid_arg, core.validateCfg(cfg).?);
}

test "validateCfg accepts exactly the stack floor" {
    var cfg = goodCfg();
    cfg.stack_size_bytes = core.min_stack_bytes;
    try std.testing.expect(core.validateCfg(cfg) == null);
}

test "validateCfg reports invalid_arg for a zero period" {
    var cfg = goodCfg();
    cfg.refresh_period_ms = 0;
    try std.testing.expectEqual(core.CfgFault.invalid_arg, core.validateCfg(cfg).?);
}

test "validateCfg reports invalid_arg above the priority ceiling" {
    var cfg = goodCfg();
    cfg.priority = core.max_priority + 1;
    try std.testing.expectEqual(core.CfgFault.invalid_arg, core.validateCfg(cfg).?);
}

test "validateCfg accepts exactly the priority ceiling" {
    var cfg = goodCfg();
    cfg.priority = core.max_priority;
    try std.testing.expect(core.validateCfg(cfg) == null);
}

test "validateCfg checks the stack before the size" {
    var cfg = goodCfg();
    cfg.has_stack = false;
    cfg.stack_size_bytes = 0;
    try std.testing.expectEqual(core.CfgFault.null_ptr, core.validateCfg(cfg).?);
}

test "isOverdue is false inside the deadline" {
    try std.testing.expect(!core.isOverdue(100, 60, 50));
}

test "isOverdue is false exactly on the deadline" {
    try std.testing.expect(!core.isOverdue(110, 60, 50));
}

test "isOverdue is true one millisecond past the deadline" {
    try std.testing.expect(core.isOverdue(111, 60, 50));
}

test "isOverdue handles a zero deadline" {
    try std.testing.expect(!core.isOverdue(60, 60, 0));
    try std.testing.expect(core.isOverdue(61, 60, 0));
}

test "isOverdue survives a 32-bit wrap" {
    const last: u32 = 0xFFFF_FFF0;
    try std.testing.expect(!core.isOverdue(last +% 10, last, 50));
    try std.testing.expect(core.isOverdue(last +% 60, last, 50));
}

test "copyName copies a short name and terminates it" {
    var dst: [core.name_max]u8 = undefined;
    core.copyName(&dst, "worker");
    try std.testing.expectEqualStrings("worker", std.mem.sliceTo(&dst, 0));
    try std.testing.expectEqual(@as(u8, 0), dst[core.name_max - 1]);
}

test "copyName truncates at name_max - 1 bytes" {
    var dst: [core.name_max]u8 = undefined;
    core.copyName(&dst, "0123456789abcdefghij");
    try std.testing.expectEqualStrings("0123456789abcde", std.mem.sliceTo(&dst, 0));
    try std.testing.expectEqual(@as(u8, 0), dst[core.name_max - 1]);
}

test "copyName accepts an empty name" {
    var dst: [core.name_max]u8 = undefined;
    core.copyName(&dst, "");
    try std.testing.expectEqual(@as(usize, 0), std.mem.sliceTo(&dst, 0).len);
}

test "copyName zero-fills stale bytes" {
    var dst: [core.name_max]u8 = [_]u8{'x'} ** core.name_max;
    core.copyName(&dst, "ab");
    try std.testing.expectEqualStrings("ab", std.mem.sliceTo(&dst, 0));
    try std.testing.expectEqual(@as(u8, 0), dst[2]);
    try std.testing.expectEqual(@as(u8, 0), dst[core.name_max - 1]);
}

test "a fresh registry is empty" {
    var reg = core.Registry{};
    try std.testing.expectEqual(@as(u8, 0), reg.used());
    try std.testing.expectEqual(@as(u8, 0), reg.findFree().?);
}

test "fill marks a slot used and stamps the check-in" {
    var reg = core.Registry{};
    reg.fill(0, "io", 25, 1000);
    try std.testing.expectEqual(@as(u8, 1), reg.used());
    try std.testing.expect(reg.isRegistered(0));
    try std.testing.expectEqual(@as(u32, 25), reg.slots[0].deadline_ms);
    try std.testing.expectEqual(@as(u32, 1000), reg.slots[0].last_checkin_ms);
    try std.testing.expectEqualStrings("io", std.mem.sliceTo(&reg.slots[0].name, 0));
}

test "findFree walks past used slots" {
    var reg = core.Registry{};
    reg.fill(0, "a", 10, 0);
    reg.fill(1, "b", 10, 0);
    try std.testing.expectEqual(@as(u8, 2), reg.findFree().?);
}

test "findFree returns null when the registry is full" {
    var reg = core.Registry{};
    var i: u8 = 0;
    while (i < core.max_threads) : (i += 1) reg.fill(i, "w", 10, 0);
    try std.testing.expect(reg.findFree() == null);
    try std.testing.expectEqual(core.max_threads, reg.used());
}

test "isRegistered rejects an out-of-range handle" {
    var reg = core.Registry{};
    reg.fill(0, "a", 10, 0);
    try std.testing.expect(!reg.isRegistered(core.max_threads));
    try std.testing.expect(!reg.isRegistered(1));
}

test "clear returns every slot to free" {
    var reg = core.Registry{};
    reg.fill(3, "c", 10, 7);
    reg.clear();
    try std.testing.expectEqual(@as(u8, 0), reg.used());
    try std.testing.expectEqual(@as(u32, 0), reg.slots[3].last_checkin_ms);
}

test "an empty registry does not refresh" {
    const reg = core.Registry{};
    const verdict = reg.verdict(500);
    try std.testing.expect(!verdict.any_present);
    try std.testing.expect(verdict.all_alive);
    try std.testing.expect(!verdict.willRefresh());
}

test "one live thread refreshes" {
    var reg = core.Registry{};
    reg.fill(0, "a", 50, 100);
    const verdict = reg.verdict(140);
    try std.testing.expect(verdict.any_present);
    try std.testing.expect(verdict.all_alive);
    try std.testing.expect(verdict.willRefresh());
}

test "one overdue thread stops the refresh" {
    var reg = core.Registry{};
    reg.fill(0, "a", 50, 100);
    try std.testing.expect(!reg.verdict(151).willRefresh());
}

test "one overdue thread among live ones stops the refresh" {
    var reg = core.Registry{};
    reg.fill(0, "a", 50, 100);
    reg.fill(1, "b", 10, 100);
    reg.fill(2, "c", 50, 100);
    const verdict = reg.verdict(140);
    try std.testing.expect(verdict.any_present);
    try std.testing.expect(!verdict.all_alive);
    try std.testing.expect(!verdict.willRefresh());
}

test "a gap in the registry does not break the walk" {
    var reg = core.Registry{};
    reg.fill(5, "late", 50, 100);
    try std.testing.expect(reg.verdict(120).willRefresh());
}

test "every registered thread is checked against its own deadline" {
    var reg = core.Registry{};
    reg.fill(0, "fast", 10, 100);
    reg.fill(1, "slow", 1000, 100);
    try std.testing.expect(reg.verdict(109).willRefresh());
    try std.testing.expect(!reg.verdict(111).willRefresh());
}

test "the verdict survives a wrapping clock" {
    var reg = core.Registry{};
    const last: u32 = 0xFFFF_FF00;
    reg.fill(0, "a", 500, last);
    try std.testing.expect(reg.verdict(last +% 400).willRefresh());
    try std.testing.expect(!reg.verdict(last +% 600).willRefresh());
}
