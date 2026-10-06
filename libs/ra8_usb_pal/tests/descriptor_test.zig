//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the pure descriptor encoders: the bytes each builder lays down,
//! the refusals, and the fields the standard computes rather than copies.

const std = @import("std");
const descriptor = @import("descriptor");

const testing = std.testing;

const base_device = descriptor.Device{
    .vid = 0x1234,
    .pid = 0x5678,
    .manufacturer = "Acme",
    .product = "Widget",
    .serial = "0001",
};

const base_cdc = descriptor.CdcAcm{
    .notify_ep = 0x83,
    .notify_bytes = 8,
    .notify_interval_ms = 255,
    .out_ep = 0x02,
    .in_ep = 0x81,
    .data_bytes = 64,
};

const base_msc = descriptor.Msc{ .in_ep = 0x81, .out_ep = 0x02, .data_bytes = 64 };

const base_hid = descriptor.Hid{
    .in_ep = 0x81,
    .data_bytes = 8,
    .poll_interval_ms = 10,
    .report_bytes = 63,
};

const base_dfu = descriptor.Dfu{
    .can_download = true,
    .detach_timeout_ms = 1000,
    .transfer_bytes = 256,
    .bcd_dfu = 0x0110,
};

fn totalLength(bytes: []const u8, config_at: usize) u16 {
    return @as(u16, bytes[config_at + 2]) | (@as(u16, bytes[config_at + 3]) << 8);
}

test "langid defaults to US English and writes two bytes" {
    var buf: [4]u8 = undefined;
    try testing.expectEqual(@as(usize, 2), try descriptor.langid(0, &buf));
    try testing.expectEqual(@as(u8, 0x09), buf[0]);
    try testing.expectEqual(@as(u8, 0x04), buf[1]);
}

test "langid keeps a caller's language" {
    var buf: [2]u8 = undefined;
    _ = try descriptor.langid(0x0407, &buf);
    try testing.expectEqual(@as(u8, 0x07), buf[0]);
    try testing.expectEqual(@as(u8, 0x04), buf[1]);
}

test "langid refuses a buffer under two bytes" {
    var buf: [1]u8 = undefined;
    try testing.expectError(descriptor.Error.InvalidSize, descriptor.langid(0, &buf));
}

test "strings encodes one entry per non-empty slot" {
    var buf: [64]u8 = undefined;
    const len = try descriptor.strings(base_device, &buf);
    try testing.expectEqual(@as(usize, (4 + 4) + (4 + 6) + (4 + 4)), len);
    try testing.expectEqual(@as(u8, 0x09), buf[0]);
    try testing.expectEqual(@as(u8, 0x04), buf[1]);
    try testing.expectEqual(@as(u8, 1), buf[2]);
    try testing.expectEqual(@as(u8, 4), buf[3]);
    try testing.expectEqualStrings("Acme", buf[4..8]);
}

test "strings skips an empty slot and keeps the indices of the rest" {
    var device = base_device;
    device.product = "";
    var buf: [64]u8 = undefined;
    const len = try descriptor.strings(device, &buf);
    try testing.expectEqual(@as(usize, (4 + 4) + (4 + 4)), len);
    try testing.expectEqual(@as(u8, 3), buf[8 + 2]);
}

test "strings refuses a slot longer than a descriptor can carry" {
    var device = base_device;
    device.product = &@as([65:0]u8, @splat('x'));
    var buf: [256]u8 = undefined;
    try testing.expectError(descriptor.Error.RangeCheck, descriptor.strings(device, &buf));
}

test "strings takes a slot of exactly the cap" {
    var device = base_device;
    device.product = &@as([64:0]u8, @splat('x'));
    var buf: [256]u8 = undefined;
    _ = try descriptor.strings(device, &buf);
}

test "strings refuses a buffer that cannot hold every entry" {
    var buf: [8]u8 = undefined;
    try testing.expectError(descriptor.Error.InvalidSize, descriptor.strings(base_device, &buf));
}

test "cdc acm lays down the composite device descriptor" {
    var buf: [128]u8 = undefined;
    const len = try descriptor.cdcAcm(base_device, base_cdc, &buf);
    try testing.expectEqual(@as(u8, 18), buf[0]);
    try testing.expectEqual(@as(u8, 0x01), buf[1]);
    try testing.expectEqual(@as(u8, 0xEF), buf[4]);
    try testing.expectEqual(@as(u8, 0x02), buf[5]);
    try testing.expectEqual(@as(u8, 0x01), buf[6]);
    try testing.expectEqual(@as(u8, 64), buf[7]);
    try testing.expectEqual(@as(usize, 93), len);
}

test "cdc acm publishes the string indices it was given" {
    var buf: [128]u8 = undefined;
    _ = try descriptor.cdcAcm(base_device, base_cdc, &buf);
    try testing.expectEqual(@as(u8, 1), buf[14]);
    try testing.expectEqual(@as(u8, 2), buf[15]);
    try testing.expectEqual(@as(u8, 3), buf[16]);
}

test "cdc acm zeroes the index of a slot with no string" {
    var device = base_device;
    device.serial = "";
    var buf: [128]u8 = undefined;
    _ = try descriptor.cdcAcm(device, base_cdc, &buf);
    try testing.expectEqual(@as(u8, 0), buf[16]);
}

test "cdc acm defaults bcdDevice to 1.00" {
    var buf: [128]u8 = undefined;
    _ = try descriptor.cdcAcm(base_device, base_cdc, &buf);
    try testing.expectEqual(@as(u8, 0x00), buf[12]);
    try testing.expectEqual(@as(u8, 0x01), buf[13]);
}

test "cdc acm inserts a device qualifier only at high speed" {
    var buf: [128]u8 = undefined;
    const full = try descriptor.cdcAcm(base_device, base_cdc, &buf);

    var high = base_cdc;
    high.high_speed = true;
    var hs_buf: [128]u8 = undefined;
    const hs = try descriptor.cdcAcm(base_device, high, &hs_buf);

    try testing.expectEqual(full + 10, hs);
    try testing.expectEqual(@as(u8, 10), hs_buf[18]);
    try testing.expectEqual(@as(u8, 0x06), hs_buf[19]);
}

test "cdc acm patches wTotalLength over the configuration only" {
    var buf: [128]u8 = undefined;
    const len = try descriptor.cdcAcm(base_device, base_cdc, &buf);
    const config_at: usize = 18;
    try testing.expectEqual(@as(u16, @truncate(len - config_at)), totalLength(&buf, config_at));
}

test "cdc acm rounds bMaxPower up to 2 mA units" {
    var device = base_device;
    device.max_power_ma = 101;
    var buf: [128]u8 = undefined;
    _ = try descriptor.cdcAcm(device, base_cdc, &buf);
    try testing.expectEqual(@as(u8, 51), buf[18 + 8]);
}

test "cdc acm folds the power flags into bmAttributes" {
    var device = base_device;
    device.self_powered = true;
    device.remote_wakeup = true;
    var buf: [128]u8 = undefined;
    _ = try descriptor.cdcAcm(device, base_cdc, &buf);
    try testing.expectEqual(@as(u8, 0xE0), buf[18 + 7]);
}

test "cdc acm refuses endpoints pointing the wrong way" {
    var buf: [128]u8 = undefined;
    var wrong = base_cdc;
    wrong.out_ep = 0x82;
    try testing.expectError(
        descriptor.Error.InvalidArg,
        descriptor.cdcAcm(base_device, wrong, &buf),
    );

    wrong = base_cdc;
    wrong.notify_ep = 0x03;
    try testing.expectError(
        descriptor.Error.InvalidArg,
        descriptor.cdcAcm(base_device, wrong, &buf),
    );
}

test "cdc acm refuses a zero packet size" {
    var buf: [128]u8 = undefined;
    var wrong = base_cdc;
    wrong.data_bytes = 0;
    try testing.expectError(
        descriptor.Error.InvalidArg,
        descriptor.cdcAcm(base_device, wrong, &buf),
    );
}

test "cdc acm refuses more power than a configuration may request" {
    var device = base_device;
    device.max_power_ma = 501;
    var buf: [128]u8 = undefined;
    try testing.expectError(
        descriptor.Error.RangeCheck,
        descriptor.cdcAcm(device, base_cdc, &buf),
    );
}

test "cdc acm refuses a buffer it would overrun" {
    var buf: [40]u8 = undefined;
    try testing.expectError(
        descriptor.Error.InvalidSize,
        descriptor.cdcAcm(base_device, base_cdc, &buf),
    );
}

test "msc publishes a per-interface device and two bulk endpoints" {
    var buf: [128]u8 = undefined;
    const len = try descriptor.msc(base_device, base_msc, &buf);
    try testing.expectEqual(@as(u8, 0x00), buf[4]);
    try testing.expectEqual(@as(usize, 18 + 9 + 9 + 7 + 7), len);
    try testing.expectEqual(@as(u8, 0x08), buf[18 + 9 + 5]);
    try testing.expectEqual(@as(u8, 0x06), buf[18 + 9 + 6]);
    try testing.expectEqual(@as(u8, 0x50), buf[18 + 9 + 7]);
}

test "msc refuses an IN address on the OUT endpoint" {
    var wrong = base_msc;
    wrong.out_ep = 0x82;
    var buf: [128]u8 = undefined;
    try testing.expectError(descriptor.Error.InvalidArg, descriptor.msc(base_device, wrong, &buf));
}

test "msc adds a qualifier at high speed" {
    var high = base_msc;
    high.high_speed = true;
    var buf: [128]u8 = undefined;
    const len = try descriptor.msc(base_device, high, &buf);
    try testing.expectEqual(@as(usize, 18 + 10 + 9 + 9 + 7 + 7), len);
    try testing.expectEqual(@as(u8, 0x00), buf[18 + 4]);
}

test "hid carries the class descriptor and one interrupt endpoint" {
    var buf: [128]u8 = undefined;
    const len = try descriptor.hid(base_device, base_hid, &buf);
    try testing.expectEqual(@as(usize, 18 + 9 + 9 + 9 + 7), len);
    const class_at: usize = 18 + 9 + 9;
    try testing.expectEqual(@as(u8, 9), buf[class_at]);
    try testing.expectEqual(@as(u8, 0x21), buf[class_at + 1]);
    try testing.expectEqual(@as(u8, 0x22), buf[class_at + 6]);
    try testing.expectEqual(@as(u8, 63), buf[class_at + 7]);
    try testing.expectEqual(@as(u8, 0x03), buf[class_at + 9 + 3]);
}

test "hid tags a boot interface with its subclass and protocol" {
    var boot = base_hid;
    boot.boot_interface = true;
    boot.protocol = .keyboard;
    var buf: [128]u8 = undefined;
    _ = try descriptor.hid(base_device, boot, &buf);
    const iface_at: usize = 18 + 9;
    try testing.expectEqual(@as(u8, 0x01), buf[iface_at + 6]);
    try testing.expectEqual(@as(u8, 0x01), buf[iface_at + 7]);
}

test "hid refuses a boot protocol on a non-boot interface" {
    var wrong = base_hid;
    wrong.protocol = .mouse;
    var buf: [128]u8 = undefined;
    try testing.expectError(descriptor.Error.InvalidArg, descriptor.hid(base_device, wrong, &buf));
}

test "hid refuses a zero poll interval" {
    var wrong = base_hid;
    wrong.poll_interval_ms = 0;
    var buf: [128]u8 = undefined;
    try testing.expectError(descriptor.Error.InvalidArg, descriptor.hid(base_device, wrong, &buf));
}

test "dfu publishes an endpoint-less interface in runtime protocol" {
    var buf: [128]u8 = undefined;
    const len = try descriptor.dfu(base_device, base_dfu, &buf);
    try testing.expectEqual(@as(usize, 18 + 9 + 9 + 9), len);
    const iface_at: usize = 18 + 9;
    try testing.expectEqual(@as(u8, 0), buf[iface_at + 4]);
    try testing.expectEqual(@as(u8, 0xFE), buf[iface_at + 5]);
    try testing.expectEqual(@as(u8, 0x01), buf[iface_at + 7]);
}

test "dfu switches protocol in dfu mode" {
    var mode = base_dfu;
    mode.dfu_mode = true;
    var buf: [128]u8 = undefined;
    _ = try descriptor.dfu(base_device, mode, &buf);
    try testing.expectEqual(@as(u8, 0x02), buf[18 + 9 + 7]);
}

test "dfu folds its capabilities into bmAttributes" {
    var all = base_dfu;
    all.can_upload = true;
    all.manifestation_tolerant = true;
    all.will_detach = true;
    try testing.expectEqual(@as(u8, 0x0F), all.attributes());
    var buf: [128]u8 = undefined;
    _ = try descriptor.dfu(base_device, all, &buf);
    try testing.expectEqual(@as(u8, 0x0F), buf[18 + 9 + 9 + 2]);
}

test "dfu refuses a device that can neither download nor upload" {
    var wrong = base_dfu;
    wrong.can_download = false;
    var buf: [128]u8 = undefined;
    try testing.expectError(descriptor.Error.InvalidArg, descriptor.dfu(base_device, wrong, &buf));
}

test "dfu refuses a zero transfer size or version" {
    var buf: [128]u8 = undefined;
    var wrong = base_dfu;
    wrong.transfer_bytes = 0;
    try testing.expectError(descriptor.Error.InvalidArg, descriptor.dfu(base_device, wrong, &buf));

    wrong = base_dfu;
    wrong.bcd_dfu = 0;
    try testing.expectError(descriptor.Error.InvalidArg, descriptor.dfu(base_device, wrong, &buf));
}

test "every framework fits the published maximum" {
    var buf: [descriptor.Limits.framework_bytes_max]u8 = undefined;
    _ = try descriptor.cdcAcm(base_device, base_cdc, &buf);
    _ = try descriptor.msc(base_device, base_msc, &buf);
    _ = try descriptor.hid(base_device, base_hid, &buf);
    _ = try descriptor.dfu(base_device, base_dfu, &buf);
}
