//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `ra8_usb_device_compose` membrane: the NULL and kind refusals the C
//! made, the codes it returned, and that a composed framework set is the
//! same bytes as the descriptor exports produce on their own.

const std = @import("std");
const abi = @import("compose_abi");

const err_ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_invalid_size: u16 = 0x105;
const err_not_supported: u16 = 0x107;

extern fn ra8_usb_device_compose(cfg: ?*const abi.CConfig, fw: ?*abi.CFrameworks) callconv(.c) u16;
extern fn ra8_usb_desc_build_cdc_acm(
    dev: ?*const abi.desc.CDevice,
    cdc: ?*const abi.desc.CCdcAcm,
    out: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
) callconv(.c) u16;

var device_buf: [256]u8 = undefined;
var strings_buf: [256]u8 = undefined;
var langid_buf: [8]u8 = undefined;

const dev = abi.desc.CDevice{
    .vid = 0x1209,
    .pid = 0x0001,
    .bcd_device = 0x0100,
    .manufacturer = "Brighton",
    .product = "RA8 Board",
    .serial = "0001",
    .langid = 0x0409,
    .max_power_ma = 100,
    .self_powered = 0,
    .remote_wakeup = 0,
};

const cdc = abi.desc.CCdcAcm{
    .notify_ep = 0x81,
    .notify_bytes = 16,
    .notify_interval_ms = 16,
    .out_ep = 0x02,
    .in_ep = 0x83,
    .data_bytes = 64,
    .high_speed = 0,
};

fn frameworks() abi.CFrameworks {
    return .{
        .device = &device_buf,
        .device_cap = device_buf.len,
        .device_len = 0,
        .strings = &strings_buf,
        .strings_cap = strings_buf.len,
        .strings_len = 0,
        .langid = &langid_buf,
        .langid_cap = langid_buf.len,
        .langid_len = 0,
    };
}

fn oneClass(kind: u8) abi.CClass {
    return .{ .kind = kind, .body = .{ .cdc_acm = cdc } };
}

const hid = abi.desc.CHid{
    .in_ep = 0x81,
    .data_bytes = 8,
    .poll_interval_ms = 10,
    .report_bytes = 63,
    .boot_interface = 1,
    .protocol = 1,
};

const msc = abi.desc.CMsc{ .in_ep = 0x81, .out_ep = 0x02, .data_bytes = 64, .high_speed = 0 };

const dfu = abi.desc.CDfu{
    .can_download = 1,
    .can_upload = 0,
    .manifestation_tolerant = 1,
    .will_detach = 0,
    .dfu_mode = 1,
    .detach_timeout_ms = 1000,
    .transfer_bytes = 256,
    .bcd_dfu = 0x0110,
};

/// One entry per kind, each carrying the payload its arm actually reads.
const every_kind = [_]abi.CClass{
    .{ .kind = 1, .body = .{ .cdc_acm = cdc } },
    .{ .kind = 2, .body = .{ .hid = hid } },
    .{ .kind = 3, .body = .{ .msc = msc } },
    .{ .kind = 4, .body = .{ .dfu = dfu } },
};

test "a cdc-acm composition succeeds and fills all three lengths" {
    const entry = oneClass(1);
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
    var fw = frameworks();
    try std.testing.expectEqual(err_ok, ra8_usb_device_compose(&cfg, &fw));
    try std.testing.expectEqual(@as(u32, 93), fw.device_len);
    try std.testing.expect(fw.strings_len > 0);
    try std.testing.expectEqual(@as(u32, 2), fw.langid_len);
}

test "the composed device framework is the encoder's own bytes" {
    const entry = oneClass(1);
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
    var fw = frameworks();
    try std.testing.expectEqual(err_ok, ra8_usb_device_compose(&cfg, &fw));
    const composed = device_buf[0..fw.device_len];

    var want: [256]u8 = undefined;
    var want_len: u32 = 0;
    try std.testing.expectEqual(
        err_ok,
        ra8_usb_desc_build_cdc_acm(&dev, &cdc, &want, want.len, &want_len),
    );
    try std.testing.expectEqualSlices(u8, want[0..want_len], composed);
}

test "a NULL config is an invalid argument, not a null-pointer code" {
    var fw = frameworks();
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_compose(null, &fw));
}

test "a NULL frameworks pointer is an invalid argument" {
    const entry = oneClass(1);
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_compose(&cfg, null));
}

test "a NULL device descriptor is refused" {
    const entry = oneClass(1);
    const cfg = abi.CConfig{ .desc = null, .classes = @ptrCast(&entry), .class_count = 1 };
    var fw = frameworks();
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_compose(&cfg, &fw));
}

test "a NULL class array is refused" {
    const cfg = abi.CConfig{ .desc = &dev, .classes = null, .class_count = 1 };
    var fw = frameworks();
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_compose(&cfg, &fw));
}

test "a zero class count is refused" {
    const entry = oneClass(1);
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 0 };
    var fw = frameworks();
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_compose(&cfg, &fw));
}

test "two class entries are unsupported" {
    const entries = [_]abi.CClass{ oneClass(1), oneClass(1) };
    const cfg = abi.CConfig{ .desc = &dev, .classes = &entries, .class_count = 2 };
    var fw = frameworks();
    try std.testing.expectEqual(err_not_supported, ra8_usb_device_compose(&cfg, &fw));
}

test "an unset class kind is refused" {
    const entry = oneClass(0);
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
    var fw = frameworks();
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_compose(&cfg, &fw));
}

test "a kind above the four the encoders name is refused" {
    const entry = oneClass(5);
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
    var fw = frameworks();
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_compose(&cfg, &fw));
    const wild = oneClass(255);
    const wild_cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&wild), .class_count = 1 };
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_compose(&wild_cfg, &fw));
}

test "each of the four kinds composes" {
    for (every_kind) |entry| {
        const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
        var fw = frameworks();
        try std.testing.expectEqual(err_ok, ra8_usb_device_compose(&cfg, &fw));
        try std.testing.expect(fw.device_len > 0);
        try std.testing.expectEqual(@as(u32, 2), fw.langid_len);
    }
}

test "a hid protocol byte outside the published three is refused" {
    var wild = hid;
    wild.protocol = 7;
    const entry = abi.CClass{ .kind = 2, .body = .{ .hid = wild } };
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
    var fw = frameworks();
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_compose(&cfg, &fw));
}

test "a NULL device buffer is refused" {
    const entry = oneClass(1);
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
    var fw = frameworks();
    fw.device = null;
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_compose(&cfg, &fw));
}

test "a NULL strings buffer is refused before anything is encoded" {
    const entry = oneClass(1);
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
    var fw = frameworks();
    fw.strings = null;
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_compose(&cfg, &fw));
    try std.testing.expectEqual(@as(u32, 0), fw.device_len);
}

test "a NULL langid buffer is refused" {
    const entry = oneClass(1);
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
    var fw = frameworks();
    fw.langid = null;
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_compose(&cfg, &fw));
}

test "a device buffer too small reports the size refusal" {
    const entry = oneClass(1);
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
    var fw = frameworks();
    fw.device_cap = 8;
    try std.testing.expectEqual(err_invalid_size, ra8_usb_device_compose(&cfg, &fw));
    try std.testing.expectEqual(@as(u32, 0), fw.device_len);
}

test "a refusal partway still publishes the device length" {
    const entry = oneClass(1);
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
    var fw = frameworks();
    fw.strings_cap = 2;
    try std.testing.expectEqual(err_invalid_size, ra8_usb_device_compose(&cfg, &fw));
    try std.testing.expectEqual(@as(u32, 93), fw.device_len);
    try std.testing.expectEqual(@as(u32, 0), fw.strings_len);
}

test "a high-speed entry composes the longer framework" {
    var fast = cdc;
    fast.high_speed = 1;
    const entry = abi.CClass{ .kind = 1, .body = .{ .cdc_acm = fast } };
    const cfg = abi.CConfig{ .desc = &dev, .classes = @ptrCast(&entry), .class_count = 1 };
    var fw = frameworks();
    try std.testing.expectEqual(err_ok, ra8_usb_device_compose(&cfg, &fw));
    try std.testing.expectEqual(@as(u32, 103), fw.device_len);
}

test "the class structure keeps the C layout" {
    try std.testing.expectEqual(@as(usize, 14), @sizeOf(abi.CClass));
    try std.testing.expectEqual(@as(usize, 2), @offsetOf(abi.CClass, "body"));
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(abi.CClassBody));
}

test "the frameworks structure keeps the C layout" {
    const pointer_bytes = @sizeOf(usize);
    try std.testing.expectEqual(pointer_bytes, @offsetOf(abi.CFrameworks, "device_cap"));
    try std.testing.expectEqual(pointer_bytes + 8, @offsetOf(abi.CFrameworks, "strings"));
    try std.testing.expectEqual(@as(usize, 2 * pointer_bytes), @offsetOf(abi.CConfig, "class_count"));
}

test "the kind enumeration matches the header's values" {
    try std.testing.expectEqual(@as(u8, 0), @backingInt(abi.CKind.none));
    try std.testing.expectEqual(@as(u8, 1), @backingInt(abi.CKind.cdc_acm));
    try std.testing.expectEqual(@as(u8, 2), @backingInt(abi.CKind.hid));
    try std.testing.expectEqual(@as(u8, 3), @backingInt(abi.CKind.msc));
    try std.testing.expectEqual(@as(u8, 4), @backingInt(abi.CKind.dfu));
}
