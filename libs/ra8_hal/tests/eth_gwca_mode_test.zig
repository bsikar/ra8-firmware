//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/eth_gwca_mode.zig (RA8FW-848).

const std = @import("std");
const m = @import("eth_gwca_mode");

/// Flips `target` to `value` on read `at`, so a poll converges late.
const Fake = struct {
    target: *volatile u32,
    value: u32,
    at: u32,
    errors: *u32,
    pub fn eval(self: Fake, reg: *volatile u32, iter: u32, cond: bool) bool {
        _ = reg;
        if (iter == self.at) {
            self.target.* = self.value;
            return true;
        }
        return cond;
    }
    pub fn logError(self: Fake, msg: [*:0]const u8) void {
        _ = msg;
        self.errors.* += 1;
    }
};

const Never = struct {
    errors: *u32,
    pub fn eval(_: Never, _: *volatile u32, _: u32, _: bool) bool {
        return false;
    }
    pub fn logError(self: Never, _: [*:0]const u8) void {
        self.errors.* += 1;
    }
};

test "setMode rejects modes above OPC" {
    var gwmc: u32 = 0;
    var gwms: u32 = 0;
    var errs: u32 = 0;
    try std.testing.expectEqual(m.invalid_arg, m.setMode(Never{ .errors = &errs }, &gwmc, &gwms, 4));
    try std.testing.expectEqual(@as(u32, 0), gwmc);
}

test "setMode keeps the upper GWMC bits and converges" {
    var gwmc: u32 = 0xF0;
    var gwms: u32 = 0;
    var errs: u32 = 0;
    const fake = Fake{ .target = &gwms, .value = 2, .at = 5, .errors = &errs };
    try std.testing.expectEqual(m.ok, m.setMode(fake, &gwmc, &gwms, 2));
    try std.testing.expectEqual(@as(u32, 0xF2), gwmc);
    try std.testing.expectEqual(@as(u32, 0), errs);
}

test "setMode times out and logs when GWMS never matches" {
    var gwmc: u32 = 0;
    var gwms: u32 = 0;
    var errs: u32 = 0;
    try std.testing.expectEqual(m.hw_timeout, m.setMode(Never{ .errors = &errs }, &gwmc, &gwms, 3));
    try std.testing.expectEqual(@as(u32, 1), errs);
}

test "axiInit requests ARIOG and returns once ARR is set" {
    var r: u32 = 0;
    var errs: u32 = 0;
    try std.testing.expectEqual(m.ok, m.axiInit(Fake{ .target = &r, .value = 3, .at = 0, .errors = &errs }, &r));
    var stuck: u32 = 0;
    try std.testing.expectEqual(m.hw_timeout, m.axiInit(Never{ .errors = &errs }, &stuck));
    try std.testing.expectEqual(m.gwarirm_ariog, stuck);
    try std.testing.expectEqual(@as(u32, 1), errs);
}
