//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Core composition rules: the class count gate, the choice of encoder, the
//! order the three encodes run in, and what a partway refusal leaves behind.

const std = @import("std");
const compose = @import("compose");
const descriptor = compose.desc;

const dev = descriptor.Device{
    .vid = 0x1209,
    .pid = 0x0001,
    .bcd_device = 0x0100,
    .manufacturer = "Brighton",
    .product = "RA8 Board",
    .serial = "0001",
    .langid = 0x0409,
    .max_power_ma = 100,
    .self_powered = false,
    .remote_wakeup = false,
};

const port = descriptor.CdcAcm{
    .notify_ep = 0x81,
    .notify_bytes = 16,
    .notify_interval_ms = 16,
    .out_ep = 0x02,
    .in_ep = 0x83,
    .data_bytes = 64,
    .high_speed = false,
};

const storage = descriptor.Msc{
    .in_ep = 0x81,
    .out_ep = 0x02,
    .data_bytes = 64,
    .high_speed = false,
};

const human = descriptor.Hid{
    .in_ep = 0x81,
    .data_bytes = 8,
    .poll_interval_ms = 10,
    .report_bytes = 63,
    .boot_interface = true,
    .protocol = .keyboard,
};

const upgrade = descriptor.Dfu{
    .can_download = true,
    .can_upload = false,
    .manifestation_tolerant = true,
    .will_detach = false,
    .dfu_mode = true,
    .detach_timeout_ms = 1000,
    .transfer_bytes = 256,
    .bcd_dfu = 0x0110,
};

const Buf = struct {
    device: [256]u8 = undefined,
    strings: [256]u8 = undefined,
    langid: [8]u8 = undefined,

    fn buffers(self: *Buf) compose.Buffers {
        return .{
            .device = &self.device,
            .strings = &self.strings,
            .langid = &self.langid,
        };
    }
};

test "zero class entries is an invalid argument" {
    try std.testing.expectError(compose.Error.InvalidArg, compose.checkCount(0));
}

test "one class entry is the one count the encoders serve" {
    try compose.checkCount(1);
}

test "a second class entry is refused as unsupported, not invalid" {
    try std.testing.expectError(compose.Error.NotSupported, compose.checkCount(2));
    try std.testing.expectError(compose.Error.NotSupported, compose.checkCount(255));
}

test "the class ceiling is one function" {
    try std.testing.expectEqual(@as(u8, 1), compose.Limits.classes_max);
}

test "a cdc-acm composition matches the three encodes it replaces" {
    var buf = Buf{};
    var len = compose.Lengths{};
    try compose.compose(dev, .{ .cdc_acm = port }, buf.buffers(), &len);

    var want_device: [256]u8 = undefined;
    var want_strings: [256]u8 = undefined;
    var want_langid: [8]u8 = undefined;
    const device_len = try descriptor.cdcAcm(dev, port, &want_device);
    const strings_len = try descriptor.strings(dev, &want_strings);
    const langid_len = try descriptor.langid(dev.langid, &want_langid);

    try std.testing.expectEqual(device_len, len.device);
    try std.testing.expectEqual(strings_len, len.strings);
    try std.testing.expectEqual(langid_len, len.langid);
    try std.testing.expectEqualSlices(u8, want_device[0..device_len], buf.device[0..len.device]);
    try std.testing.expectEqualSlices(u8, want_strings[0..strings_len], buf.strings[0..len.strings]);
    try std.testing.expectEqualSlices(u8, want_langid[0..langid_len], buf.langid[0..len.langid]);
}

test "the cdc-acm framework is the documented 93 bytes at full speed" {
    var buf = Buf{};
    var len = compose.Lengths{};
    try compose.compose(dev, .{ .cdc_acm = port }, buf.buffers(), &len);
    try std.testing.expectEqual(@as(usize, 93), len.device);
}

test "each class arm reaches its own encoder" {
    const cases = [_]compose.Class{
        .{ .cdc_acm = port },
        .{ .hid = human },
        .{ .msc = storage },
        .{ .dfu = upgrade },
    };
    var seen: [cases.len]usize = undefined;
    for (cases, 0..) |class, i| {
        var buf = Buf{};
        var len = compose.Lengths{};
        try compose.compose(dev, class, buf.buffers(), &len);
        try std.testing.expect(len.device > 0);
        // byte 19 is the configuration descriptor's bDescriptorType.
        try std.testing.expectEqual(@as(u8, 0x02), buf.device[19]);
        seen[i] = len.device;
    }
    for (seen[1..]) |other| try std.testing.expect(other != seen[0]);
}

test "every arm writes the same string table and langid" {
    const cases = [_]compose.Class{
        .{ .cdc_acm = port },
        .{ .hid = human },
        .{ .msc = storage },
        .{ .dfu = upgrade },
    };
    var first = Buf{};
    var first_len = compose.Lengths{};
    try compose.compose(dev, cases[0], first.buffers(), &first_len);
    for (cases[1..]) |class| {
        var buf = Buf{};
        var len = compose.Lengths{};
        try compose.compose(dev, class, buf.buffers(), &len);
        try std.testing.expectEqualSlices(
            u8,
            first.strings[0..first_len.strings],
            buf.strings[0..len.strings],
        );
        try std.testing.expectEqualSlices(
            u8,
            first.langid[0..first_len.langid],
            buf.langid[0..len.langid],
        );
    }
}

test "a device buffer too small refuses before anything is written" {
    var small: [8]u8 = undefined;
    var buf = Buf{};
    var len = compose.Lengths{};
    const out = compose.Buffers{
        .device = &small,
        .strings = &buf.strings,
        .langid = &buf.langid,
    };
    try std.testing.expectError(
        descriptor.Error.InvalidSize,
        compose.compose(dev, .{ .cdc_acm = port }, out, &len),
    );
    try std.testing.expectEqual(@as(usize, 0), len.device);
    try std.testing.expectEqual(@as(usize, 0), len.strings);
}

test "a refusal partway leaves the earlier length published" {
    var small: [2]u8 = undefined;
    var buf = Buf{};
    var len = compose.Lengths{};
    const out = compose.Buffers{
        .device = &buf.device,
        .strings = &small,
        .langid = &buf.langid,
    };
    try std.testing.expectError(
        descriptor.Error.InvalidSize,
        compose.compose(dev, .{ .cdc_acm = port }, out, &len),
    );
    try std.testing.expectEqual(@as(usize, 93), len.device);
    try std.testing.expectEqual(@as(usize, 0), len.strings);
    try std.testing.expectEqual(@as(usize, 0), len.langid);
}

test "a langid buffer too small still leaves device and strings published" {
    var small: [1]u8 = undefined;
    var buf = Buf{};
    var len = compose.Lengths{};
    const out = compose.Buffers{
        .device = &buf.device,
        .strings = &buf.strings,
        .langid = &small,
    };
    try std.testing.expectError(
        descriptor.Error.InvalidSize,
        compose.compose(dev, .{ .msc = storage }, out, &len),
    );
    try std.testing.expect(len.device > 0);
    try std.testing.expect(len.strings > 0);
    try std.testing.expectEqual(@as(usize, 0), len.langid);
}

test "lengths start at zero" {
    const len = compose.Lengths{};
    try std.testing.expectEqual(@as(usize, 0), len.device);
    try std.testing.expectEqual(@as(usize, 0), len.strings);
    try std.testing.expectEqual(@as(usize, 0), len.langid);
}

test "the error set carries the encoder's own refusals" {
    const E = compose.Error;
    try std.testing.expectError(E.InvalidArg, compose.checkCount(0));
    var buf = Buf{};
    var len = compose.Lengths{};
    var small: [1]u8 = undefined;
    const out = compose.Buffers{
        .device = &small,
        .strings = &buf.strings,
        .langid = &buf.langid,
    };
    const got = compose.compose(dev, .{ .hid = human }, out, &len);
    try std.testing.expectError(E.InvalidSize, got);
}

test "a high-speed cdc-acm composition is the longer framework" {
    var fast = port;
    fast.high_speed = true;
    var buf = Buf{};
    var len = compose.Lengths{};
    try compose.compose(dev, .{ .cdc_acm = fast }, buf.buffers(), &len);
    try std.testing.expectEqual(@as(usize, 103), len.device);
}
