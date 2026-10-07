//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the USB descriptor builders declared by
//! `inc/ra8_usb_desc.h`. Everything C-shaped stops here: the `const char*`
//! string fields become slices, the caller's pointer and capacity become one
//! slice, and the encoder's errors become `ra8_err_t` values.

const std = @import("std");
const descriptor = @import("internal/descriptor.zig");

// =============================================================================
// ra8_err_t values used on this surface
// =============================================================================

const err_ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_invalid_size: u16 = 0x105;
const err_range_check_failed: u16 = 0x503;
const err_null_ptr: u16 = 0x504;

fn code(err: descriptor.Error) u16 {
    return switch (err) {
        descriptor.Error.InvalidArg => err_invalid_arg,
        descriptor.Error.InvalidSize => err_invalid_size,
        descriptor.Error.RangeCheck => err_range_check_failed,
    };
}

// =============================================================================
// Mirrors of the C structures (ra8_usb_desc.h)
// =============================================================================

/// `ra8_usb_desc_device_t`. The three `bool` fields are `u8` here because the
/// ABI checker keeps `bool` off this boundary; C's `_Bool` is one byte with
/// the same 0/1 values.
pub const CDevice = extern struct {
    vid: u16,
    pid: u16,
    bcd_device: u16,
    manufacturer: ?[*:0]const u8,
    product: ?[*:0]const u8,
    serial: ?[*:0]const u8,
    langid: u16,
    max_power_ma: u16,
    self_powered: u8,
    remote_wakeup: u8,
};

/// `ra8_usb_desc_cdc_acm_t`.
pub const CCdcAcm = extern struct {
    notify_ep: u8,
    notify_bytes: u16,
    notify_interval_ms: u8,
    out_ep: u8,
    in_ep: u8,
    data_bytes: u16,
    high_speed: u8,
};

/// `ra8_usb_desc_msc_t`.
pub const CMsc = extern struct {
    in_ep: u8,
    out_ep: u8,
    data_bytes: u16,
    high_speed: u8,
};

/// `ra8_usb_desc_hid_t`.
pub const CHid = extern struct {
    in_ep: u8,
    data_bytes: u16,
    poll_interval_ms: u8,
    report_bytes: u16,
    boot_interface: u8,
    protocol: u8,
};

/// `ra8_usb_desc_dfu_t`.
pub const CDfu = extern struct {
    can_download: u8,
    can_upload: u8,
    manifestation_tolerant: u8,
    will_detach: u8,
    dfu_mode: u8,
    detach_timeout_ms: u16,
    transfer_bytes: u16,
    bcd_dfu: u16,
};

comptime {
    const pointer_bytes = @sizeOf(usize);
    std.debug.assert(@offsetOf(CDevice, "manufacturer") == 8);
    std.debug.assert(@offsetOf(CDevice, "serial") == 8 + (2 * pointer_bytes));
    std.debug.assert(@offsetOf(CDevice, "langid") == 8 + (3 * pointer_bytes));
    std.debug.assert(@offsetOf(CCdcAcm, "data_bytes") == 8);
    std.debug.assert(@sizeOf(CCdcAcm) == 12);
    std.debug.assert(@sizeOf(CMsc) == 6);
    std.debug.assert(@offsetOf(CHid, "report_bytes") == 6);
    std.debug.assert(@sizeOf(CHid) == 10);
    std.debug.assert(@offsetOf(CDfu, "detach_timeout_ms") == 6);
    std.debug.assert(@sizeOf(CDfu) == 12);
    std.debug.assert(descriptor.Size.device == 18);
    std.debug.assert(descriptor.Limits.strings_bytes_max == 204);
}

// =============================================================================
// C shapes to Zig shapes
// =============================================================================

/// A C string as a slice, refused once it is longer than a descriptor can
/// carry. Scanning stops one byte past the cap, so a run-on string is a
/// refusal rather than an unbounded read.
fn span(text: ?[*:0]const u8) descriptor.Error![]const u8 {
    const ptr = text orelse return "";
    const cap = descriptor.Limits.string_chars_max;
    var len: usize = 0;
    while (ptr[len] != 0) {
        len += 1;
        if (len > cap) return descriptor.Error.RangeCheck;
    }
    return ptr[0..len];
}

/// `ra8_usb_desc_device_t` as the core's `Device`. Shared with the compose
/// membrane so both surfaces read a C device exactly the same way.
pub fn deviceOf(c: *const CDevice) descriptor.Error!descriptor.Device {
    return .{
        .vid = c.vid,
        .pid = c.pid,
        .bcd_device = c.bcd_device,
        .manufacturer = try span(c.manufacturer),
        .product = try span(c.product),
        .serial = try span(c.serial),
        .langid = c.langid,
        .max_power_ma = c.max_power_ma,
        .self_powered = c.self_powered != 0,
        .remote_wakeup = c.remote_wakeup != 0,
    };
}

/// `ra8_usb_desc_cdc_acm_t` as the core's `CdcAcm`.
pub fn cdcAcmOf(c: *const CCdcAcm) descriptor.CdcAcm {
    return .{
        .notify_ep = c.notify_ep,
        .notify_bytes = c.notify_bytes,
        .notify_interval_ms = c.notify_interval_ms,
        .out_ep = c.out_ep,
        .in_ep = c.in_ep,
        .data_bytes = c.data_bytes,
        .high_speed = c.high_speed != 0,
    };
}

/// `ra8_usb_desc_msc_t` as the core's `Msc`.
pub fn mscOf(c: *const CMsc) descriptor.Msc {
    return .{
        .in_ep = c.in_ep,
        .out_ep = c.out_ep,
        .data_bytes = c.data_bytes,
        .high_speed = c.high_speed != 0,
    };
}

/// `ra8_usb_desc_hid_t` as the core's `Hid`. A protocol byte outside the
/// three the specification names is refused here rather than encoded.
pub fn hidOf(c: *const CHid) descriptor.Error!descriptor.Hid {
    return .{
        .in_ep = c.in_ep,
        .data_bytes = c.data_bytes,
        .poll_interval_ms = c.poll_interval_ms,
        .report_bytes = c.report_bytes,
        .boot_interface = c.boot_interface != 0,
        .protocol = std.enums.fromInt(descriptor.HidProtocol, c.protocol) orelse
            return descriptor.Error.InvalidArg,
    };
}

/// `ra8_usb_desc_dfu_t` as the core's `Dfu`.
pub fn dfuOf(c: *const CDfu) descriptor.Dfu {
    return .{
        .can_download = c.can_download != 0,
        .can_upload = c.can_upload != 0,
        .manifestation_tolerant = c.manifestation_tolerant != 0,
        .will_detach = c.will_detach != 0,
        .dfu_mode = c.dfu_mode != 0,
        .detach_timeout_ms = c.detach_timeout_ms,
        .transfer_bytes = c.transfer_bytes,
        .bcd_dfu = c.bcd_dfu,
    };
}

pub fn outSlice(out: [*]u8, cap: u32) []u8 {
    return out[0..cap];
}

/// Publish a successful encode: length out, `k_ra8_ok`.
fn wrote(len: usize, out_len: *u32) u16 {
    out_len.* = @truncate(len);
    return err_ok;
}

// =============================================================================
// Exports (inc/ra8_usb_desc.h)
// =============================================================================

export fn ra8_usb_desc_build_langid(
    langid: u16,
    out: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
) callconv(.c) u16 {
    const buf = out orelse return err_null_ptr;
    const len_out = out_len orelse return err_null_ptr;
    const len = descriptor.langid(langid, outSlice(buf, cap)) catch |err| return code(err);
    return wrote(len, len_out);
}

export fn ra8_usb_desc_build_strings(
    dev: ?*const CDevice,
    out: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
) callconv(.c) u16 {
    const c_dev = dev orelse return err_null_ptr;
    const buf = out orelse return err_null_ptr;
    const len_out = out_len orelse return err_null_ptr;
    const model = deviceOf(c_dev) catch |err| return code(err);
    const len = descriptor.strings(model, outSlice(buf, cap)) catch |err| return code(err);
    return wrote(len, len_out);
}

export fn ra8_usb_desc_build_cdc_acm(
    dev: ?*const CDevice,
    cdc: ?*const CCdcAcm,
    out: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
) callconv(.c) u16 {
    const c_dev = dev orelse return err_null_ptr;
    const c_cdc = cdc orelse return err_null_ptr;
    const buf = out orelse return err_null_ptr;
    const len_out = out_len orelse return err_null_ptr;
    const port = cdcAcmOf(c_cdc);
    const model = deviceOf(c_dev) catch |err| return code(err);
    const len = descriptor.cdcAcm(model, port, outSlice(buf, cap)) catch |err| return code(err);
    return wrote(len, len_out);
}

export fn ra8_usb_desc_build_msc(
    dev: ?*const CDevice,
    msc: ?*const CMsc,
    out: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
) callconv(.c) u16 {
    const c_dev = dev orelse return err_null_ptr;
    const c_msc = msc orelse return err_null_ptr;
    const buf = out orelse return err_null_ptr;
    const len_out = out_len orelse return err_null_ptr;
    const storage = mscOf(c_msc);
    const model = deviceOf(c_dev) catch |err| return code(err);
    const len = descriptor.msc(model, storage, outSlice(buf, cap)) catch |err| return code(err);
    return wrote(len, len_out);
}

export fn ra8_usb_desc_build_hid(
    dev: ?*const CDevice,
    hid: ?*const CHid,
    out: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
) callconv(.c) u16 {
    const c_dev = dev orelse return err_null_ptr;
    const c_hid = hid orelse return err_null_ptr;
    const buf = out orelse return err_null_ptr;
    const len_out = out_len orelse return err_null_ptr;
    const human = hidOf(c_hid) catch |err| return code(err);
    const model = deviceOf(c_dev) catch |err| return code(err);
    const len = descriptor.hid(model, human, outSlice(buf, cap)) catch |err| return code(err);
    return wrote(len, len_out);
}

export fn ra8_usb_desc_build_dfu(
    dev: ?*const CDevice,
    dfu: ?*const CDfu,
    out: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
) callconv(.c) u16 {
    const c_dev = dev orelse return err_null_ptr;
    const c_dfu = dfu orelse return err_null_ptr;
    const buf = out orelse return err_null_ptr;
    const len_out = out_len orelse return err_null_ptr;
    const upgrade = dfuOf(c_dfu);
    const model = deviceOf(c_dev) catch |err| return code(err);
    const len = descriptor.dfu(model, upgrade, outSlice(buf, cap)) catch |err| return code(err);
    return wrote(len, len_out);
}
