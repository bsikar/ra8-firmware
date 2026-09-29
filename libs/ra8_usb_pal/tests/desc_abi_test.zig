//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ABI-membrane tests for the descriptor builders: every export driven at the
//! C shape, with the C structures laid out as `inc/ra8_usb_desc.h` declares
//! them, so the null and error codes a C caller sees are pinned here.

const std = @import("std");
const abi = @import("desc_abi");

const testing = std.testing;

const err_ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_invalid_size: u16 = 0x105;
const err_range_check_failed: u16 = 0x503;
const err_null_ptr: u16 = 0x504;

extern fn ra8_usb_desc_build_langid(langid: u16, out: ?[*]u8, cap: u32, out_len: ?*u32) u16;
extern fn ra8_usb_desc_build_strings(
    dev: ?*const abi.CDevice,
    out: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
) u16;
extern fn ra8_usb_desc_build_cdc_acm(
    dev: ?*const abi.CDevice,
    cdc: ?*const abi.CCdcAcm,
    out: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
) u16;
extern fn ra8_usb_desc_build_msc(
    dev: ?*const abi.CDevice,
    msc: ?*const abi.CMsc,
    out: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
) u16;
extern fn ra8_usb_desc_build_hid(
    dev: ?*const abi.CDevice,
    hid: ?*const abi.CHid,
    out: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
) u16;
extern fn ra8_usb_desc_build_dfu(
    dev: ?*const abi.CDevice,
    dfu: ?*const abi.CDfu,
    out: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
) u16;

comptime {
    _ = abi;
}

const c_device = abi.CDevice{
    .vid = 0x1234,
    .pid = 0x5678,
    .bcd_device = 0,
    .manufacturer = "Acme",
    .product = "Widget",
    .serial = "0001",
    .langid = 0,
    .max_power_ma = 100,
    .self_powered = 0,
    .remote_wakeup = 0,
};

const c_cdc = abi.CCdcAcm{
    .notify_ep = 0x83,
    .notify_bytes = 8,
    .notify_interval_ms = 255,
    .out_ep = 0x02,
    .in_ep = 0x81,
    .data_bytes = 64,
    .high_speed = 0,
};

const c_msc = abi.CMsc{ .in_ep = 0x81, .out_ep = 0x02, .data_bytes = 64, .high_speed = 0 };

const c_hid = abi.CHid{
    .in_ep = 0x81,
    .data_bytes = 8,
    .poll_interval_ms = 10,
    .report_bytes = 63,
    .boot_interface = 0,
    .protocol = 0,
};

const c_dfu = abi.CDfu{
    .can_download = 1,
    .can_upload = 0,
    .manifestation_tolerant = 0,
    .will_detach = 0,
    .dfu_mode = 0,
    .detach_timeout_ms = 1000,
    .transfer_bytes = 256,
    .bcd_dfu = 0x0110,
};

test "langid export writes the default language" {
    var buf: [2]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(err_ok, ra8_usb_desc_build_langid(0, &buf, buf.len, &len));
    try testing.expectEqual(@as(u32, 2), len);
    try testing.expectEqual(@as(u8, 0x09), buf[0]);
}

test "langid export reports a null buffer and a null length separately" {
    var buf: [2]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(err_null_ptr, ra8_usb_desc_build_langid(0, null, 2, &len));
    try testing.expectEqual(err_null_ptr, ra8_usb_desc_build_langid(0, &buf, buf.len, null));
}

test "langid export refuses a capacity under two bytes" {
    var buf: [2]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(err_invalid_size, ra8_usb_desc_build_langid(0, &buf, 1, &len));
}

test "strings export writes every published slot" {
    var buf: [64]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(
        err_ok,
        ra8_usb_desc_build_strings(&c_device, &buf, buf.len, &len),
    );
    try testing.expectEqual(@as(u32, 26), len);
}

test "strings export treats a null string field as an unpublished slot" {
    var device = c_device;
    device.serial = null;
    var buf: [64]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(err_ok, ra8_usb_desc_build_strings(&device, &buf, buf.len, &len));
    try testing.expectEqual(@as(u32, 18), len);
}

test "strings export refuses a run-on string" {
    var device = c_device;
    device.product = "x" ** 65;
    var buf: [256]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(
        err_range_check_failed,
        ra8_usb_desc_build_strings(&device, &buf, buf.len, &len),
    );
}

test "strings export reports a null device" {
    var buf: [64]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(err_null_ptr, ra8_usb_desc_build_strings(null, &buf, buf.len, &len));
}

test "cdc acm export writes the framework and its length" {
    var buf: [128]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(
        err_ok,
        ra8_usb_desc_build_cdc_acm(&c_device, &c_cdc, &buf, buf.len, &len),
    );
    try testing.expectEqual(@as(u32, 93), len);
    try testing.expectEqual(@as(u8, 18), buf[0]);
    try testing.expectEqual(@as(u8, 50), buf[18 + 8]);
}

test "cdc acm export refuses a misdirected endpoint" {
    var wrong = c_cdc;
    wrong.out_ep = 0x82;
    var buf: [128]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(
        err_invalid_arg,
        ra8_usb_desc_build_cdc_acm(&c_device, &wrong, &buf, buf.len, &len),
    );
}

test "cdc acm export refuses a capacity it would overrun" {
    var buf: [128]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(
        err_invalid_size,
        ra8_usb_desc_build_cdc_acm(&c_device, &c_cdc, &buf, 40, &len),
    );
}

test "cdc acm export reports each null argument" {
    var buf: [128]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(
        err_null_ptr,
        ra8_usb_desc_build_cdc_acm(null, &c_cdc, &buf, buf.len, &len),
    );
    try testing.expectEqual(
        err_null_ptr,
        ra8_usb_desc_build_cdc_acm(&c_device, null, &buf, buf.len, &len),
    );
    try testing.expectEqual(
        err_null_ptr,
        ra8_usb_desc_build_cdc_acm(&c_device, &c_cdc, null, 128, &len),
    );
}

test "cdc acm export reads high_speed as a C bool byte" {
    var high = c_cdc;
    high.high_speed = 1;
    var buf: [128]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(
        err_ok,
        ra8_usb_desc_build_cdc_acm(&c_device, &high, &buf, buf.len, &len),
    );
    try testing.expectEqual(@as(u32, 103), len);
}

test "msc export writes a per-interface framework" {
    var buf: [128]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(err_ok, ra8_usb_desc_build_msc(&c_device, &c_msc, &buf, buf.len, &len));
    try testing.expectEqual(@as(u32, 50), len);
    try testing.expectEqual(@as(u8, 0x00), buf[4]);
}

test "msc export refuses more power than a configuration may request" {
    var device = c_device;
    device.max_power_ma = 501;
    var buf: [128]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(
        err_range_check_failed,
        ra8_usb_desc_build_msc(&device, &c_msc, &buf, buf.len, &len),
    );
}

test "hid export writes the class descriptor" {
    var buf: [128]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(err_ok, ra8_usb_desc_build_hid(&c_device, &c_hid, &buf, buf.len, &len));
    try testing.expectEqual(@as(u32, 52), len);
    try testing.expectEqual(@as(u8, 0x21), buf[18 + 9 + 9 + 1]);
}

test "hid export refuses a protocol outside the published set" {
    var wrong = c_hid;
    wrong.protocol = 9;
    var buf: [128]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(
        err_invalid_arg,
        ra8_usb_desc_build_hid(&c_device, &wrong, &buf, buf.len, &len),
    );
}

test "dfu export writes the functional descriptor" {
    var buf: [128]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(err_ok, ra8_usb_desc_build_dfu(&c_device, &c_dfu, &buf, buf.len, &len));
    try testing.expectEqual(@as(u32, 45), len);
    try testing.expectEqual(@as(u8, 0x01), buf[18 + 9 + 9 + 2]);
}

test "dfu export refuses a device that can neither download nor upload" {
    var wrong = c_dfu;
    wrong.can_download = 0;
    var buf: [128]u8 = undefined;
    var len: u32 = 0;
    try testing.expectEqual(
        err_invalid_arg,
        ra8_usb_desc_build_dfu(&c_device, &wrong, &buf, buf.len, &len),
    );
}

test "the C structures keep the layouts the header declares" {
    const pointer_bytes = @sizeOf(usize);
    try testing.expectEqual(@as(usize, 8), @offsetOf(abi.CDevice, "manufacturer"));
    try testing.expectEqual(
        @as(usize, 8 + (3 * pointer_bytes)),
        @offsetOf(abi.CDevice, "langid"),
    );
    try testing.expectEqual(@as(usize, 12), @sizeOf(abi.CCdcAcm));
    try testing.expectEqual(@as(usize, 6), @sizeOf(abi.CMsc));
    try testing.expectEqual(@as(usize, 10), @sizeOf(abi.CHid));
    try testing.expectEqual(@as(usize, 12), @sizeOf(abi.CDfu));
}
