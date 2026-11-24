//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure unit tests for `internal/device_open.zig`: validation guards, port/HS
//! pairing, and strict callback bring-up sequencing.

const std = @import("std");
const device_open = @import("device_open");

fn validSpec() device_open.OpenSpec {
    return .{
        .port = @backingInt(device_open.Port.fs),
        .configuration = 1,
        .interface = 0,
        .has_pool = true,
        .pool_bytes = 16384,
        .has_device_cfg = false,
        .has_frameworks = true,
        .has_fs_device = true,
        .fs_device_len = 93,
        .has_strings = true,
        .strings_len = 56,
        .has_langid = true,
        .langid_len = 2,
        .has_hs_device = false,
        .hs_device_len = 0,
        .has_ops = true,
        .has_system_init = true,
        .has_device_stack_init = true,
        .has_class_register = true,
        .has_error_cb_register = true,
        .has_dcd_open = true,
        .has_class_driver = true,
        .has_class_name = true,
        .class_name_len = 22,
        .has_class_entry = true,
        .has_class_param = true,
        .has_error_cb = false,
    };
}

const SpyBackend = struct {
    log: [8]u8 = @splat(0),
    len: usize = 0,
    fail_on: u8 = 0,

    fn record(self: *SpyBackend, step: u8) bool {
        if (self.fail_on == step) return false;
        self.log[self.len] = step;
        self.len += 1;
        return true;
    }

    pub fn compose(self: *SpyBackend) device_open.Error!void {
        if (!self.record('C')) return device_open.Error.InvalidState;
    }

    pub fn systemInit(self: *SpyBackend) bool {
        return self.record('S');
    }

    pub fn registerErrorCb(self: *SpyBackend) bool {
        return self.record('E');
    }

    pub fn deviceStackInit(self: *SpyBackend) bool {
        return self.record('D');
    }

    pub fn classRegister(self: *SpyBackend) bool {
        return self.record('R');
    }

    pub fn dcdOpen(self: *SpyBackend) bool {
        return self.record('O');
    }

    fn trace(self: *const SpyBackend) []const u8 {
        return self.log[0..self.len];
    }
};

test "valid FS and HS specifications pass static validation" {
    try std.testing.expectEqual(device_open.Port.fs, try device_open.validateStatic(validSpec()));

    var hs = validSpec();
    hs.port = @backingInt(device_open.Port.hs);
    hs.has_hs_device = true;
    hs.hs_device_len = 103;
    try std.testing.expectEqual(device_open.Port.hs, try device_open.validateStatic(hs));
}

test "invalid port, zero config, or missing pool fail with InvalidArg" {
    var bad = validSpec();
    bad.port = 2;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));

    bad = validSpec();
    bad.configuration = 0;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));

    bad = validSpec();
    bad.has_pool = false;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));

    bad = validSpec();
    bad.pool_bytes = 0;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));
}

test "FS port with HS framework and mismatched HS pointer/length pair are refused" {
    var bad = validSpec();
    bad.has_hs_device = true;
    bad.hs_device_len = 103;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));

    bad = validSpec();
    bad.port = @backingInt(device_open.Port.hs);
    bad.has_hs_device = true;
    bad.hs_device_len = 0;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));

    bad = validSpec();
    bad.port = @backingInt(device_open.Port.hs);
    bad.has_hs_device = false;
    bad.hs_device_len = 103;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));
}

test "missing ops callbacks, missing error_cb hook, or bad class descriptor refuse" {
    var bad = validSpec();
    bad.has_ops = false;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));

    bad = validSpec();
    bad.has_system_init = false;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));

    bad = validSpec();
    bad.has_error_cb = true;
    bad.has_error_cb_register = false;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));

    bad = validSpec();
    bad.class_name_len = 0;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));

    bad = validSpec();
    bad.class_name_len = device_open.class_name_max_len + 1;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));

    bad = validSpec();
    bad.has_class_param = false;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateStatic(bad));
}

test "pre-built frameworks require non-null non-zero FS, strings, and langid" {
    var bad = validSpec();
    bad.fs_device_len = 0;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateFrameworks(bad));

    bad = validSpec();
    bad.strings_len = 0;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateFrameworks(bad));

    bad = validSpec();
    bad.langid_len = 0;
    try std.testing.expectError(device_open.Error.InvalidArg, device_open.validateFrameworks(bad));
}

test "execute runs compose, systemInit, errorCb, deviceStackInit, classRegister, dcdOpen in order" {
    var spec = validSpec();
    spec.has_device_cfg = true;
    spec.has_error_cb = true;
    var spy = SpyBackend{};
    try device_open.execute(spec, &spy);
    try std.testing.expectEqualStrings("CSEDRO", spy.trace());
}

test "execute omits optional compose, errorCb, and dcdOpen steps when unset" {
    var spec = validSpec();
    spec.has_dcd_open = false;
    var spy = SpyBackend{};
    try device_open.execute(spec, &spy);
    try std.testing.expectEqualStrings("SDR", spy.trace());
}

test "execute halts immediately on failing step without invoking later stages" {
    var spec = validSpec();
    spec.has_device_cfg = true;
    spec.has_error_cb = true;

    inline for ([_]struct { step: u8, want: []const u8 }{
        .{ .step = 'C', .want = "" },
        .{ .step = 'S', .want = "C" },
        .{ .step = 'E', .want = "CS" },
        .{ .step = 'D', .want = "CSE" },
        .{ .step = 'R', .want = "CSED" },
        .{ .step = 'O', .want = "CSEDR" },
    }) |tc| {
        var spy = SpyBackend{ .fail_on = tc.step };
        try std.testing.expectError(device_open.Error.InvalidState, device_open.execute(spec, &spy));
        try std.testing.expectEqualStrings(tc.want, spy.trace());
    }
}
