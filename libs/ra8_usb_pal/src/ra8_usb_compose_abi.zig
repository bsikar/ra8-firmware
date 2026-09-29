//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `ra8_usb_device_compose`, declared by
//! `inc/ra8_usb_compose.h`. The kind byte beside a bare union becomes a
//! tagged union, the caller's pointer and capacity pairs become slices, and
//! the core's errors become `ra8_err_t` values. Every refusal the C made on
//! this surface is made here, with the same code.

const std = @import("std");
const compose = @import("internal/compose.zig");
const descriptor = @import("internal/descriptor.zig");
const desc_abi = @import("ra8_usb_desc_abi.zig");

/// The descriptor membrane, re-exported so a caller of this surface reaches
/// the same C structure mirrors through one import.
pub const desc = desc_abi;

// =============================================================================
// ra8_err_t values used on this surface
// =============================================================================

const err_ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_invalid_size: u16 = 0x105;
const err_not_supported: u16 = 0x107;
const err_range_check_failed: u16 = 0x503;

fn code(err: compose.Error) u16 {
    return switch (err) {
        compose.Error.InvalidArg => err_invalid_arg,
        compose.Error.NotSupported => err_not_supported,
        compose.Error.InvalidSize => err_invalid_size,
        compose.Error.RangeCheck => err_range_check_failed,
    };
}

// =============================================================================
// Mirrors of the C structures (ra8_usb_compose.h)
// =============================================================================

/// `ra8_usb_class_kind_t`. Zero is the unset value the C refuses.
pub const CKind = enum(u8) {
    none = 0,
    cdc_acm = 1,
    hid = 2,
    msc = 3,
    dfu = 4,
};

/// The payload half of `ra8_usb_class_t`: C's bare union, read through the
/// arm the kind byte names.
pub const CClassBody = extern union {
    cdc_acm: desc_abi.CCdcAcm,
    hid: desc_abi.CHid,
    msc: desc_abi.CMsc,
    dfu: desc_abi.CDfu,
};

/// `ra8_usb_class_t`.
pub const CClass = extern struct {
    kind: u8,
    body: CClassBody,
};

/// `ra8_usb_device_cfg_t`.
pub const CConfig = extern struct {
    desc: ?*const desc_abi.CDevice,
    classes: ?[*]const CClass,
    class_count: u8,
};

/// `ra8_usb_device_frameworks_t`.
pub const CFrameworks = extern struct {
    device: ?[*]u8,
    device_cap: u32,
    device_len: u32,
    strings: ?[*]u8,
    strings_cap: u32,
    strings_len: u32,
    langid: ?[*]u8,
    langid_cap: u32,
    langid_len: u32,
};

comptime {
    const pointer_bytes = @sizeOf(usize);
    std.debug.assert(@offsetOf(CClass, "body") == 2);
    std.debug.assert(@sizeOf(CClass) == 14);
    std.debug.assert(@sizeOf(CClassBody) == 12);
    std.debug.assert(@offsetOf(CConfig, "classes") == pointer_bytes);
    std.debug.assert(@offsetOf(CConfig, "class_count") == 2 * pointer_bytes);
    std.debug.assert(@offsetOf(CFrameworks, "device_cap") == pointer_bytes);
    std.debug.assert(@offsetOf(CFrameworks, "strings") == pointer_bytes + 8);
    std.debug.assert(compose.Limits.classes_max == 1);
}

// =============================================================================
// C shapes to Zig shapes
// =============================================================================

/// One class entry as a tagged union. An unset or unknown kind has no arm,
/// which is the C's `default:` refusal.
fn class(entry: *const CClass) compose.Error!compose.Class {
    const kind = std.meta.intToEnum(CKind, entry.kind) catch return compose.Error.InvalidArg;
    return switch (kind) {
        .cdc_acm => .{ .cdc_acm = desc_abi.cdcAcmOf(&entry.body.cdc_acm) },
        .hid => .{ .hid = try desc_abi.hidOf(&entry.body.hid) },
        .msc => .{ .msc = desc_abi.mscOf(&entry.body.msc) },
        .dfu => .{ .dfu = desc_abi.dfuOf(&entry.body.dfu) },
        .none => compose.Error.InvalidArg,
    };
}

/// Every pointer the C checked for NULL, in the C's own order. A missing one
/// is `k_ra8_err_invalid_arg`, not the null-pointer code.
fn buffers(fw: *const CFrameworks) compose.Error!compose.Buffers {
    const device = fw.device orelse return compose.Error.InvalidArg;
    const strings = fw.strings orelse return compose.Error.InvalidArg;
    const langid = fw.langid orelse return compose.Error.InvalidArg;
    return .{
        .device = device[0..fw.device_cap],
        .strings = strings[0..fw.strings_cap],
        .langid = langid[0..fw.langid_cap],
    };
}

/// Publish whatever the core wrote, truncating to the C's `uint32_t` fields.
fn publish(len: compose.Lengths, fw: *CFrameworks) void {
    fw.device_len = @truncate(len.device);
    fw.strings_len = @truncate(len.strings);
    fw.langid_len = @truncate(len.langid);
}

fn run(cfg: *const CConfig, fw: *CFrameworks, len: *compose.Lengths) compose.Error!void {
    const c_dev = cfg.desc orelse return compose.Error.InvalidArg;
    const entries = cfg.classes orelse return compose.Error.InvalidArg;
    try compose.checkCount(cfg.class_count);
    const out = try buffers(fw);
    const model = try desc_abi.deviceOf(c_dev);
    try compose.compose(model, try class(&entries[0]), out, len);
}

// =============================================================================
// Export (inc/ra8_usb_compose.h)
// =============================================================================

export fn ra8_usb_device_compose(cfg: ?*const CConfig, fw: ?*CFrameworks) callconv(.c) u16 {
    const c_cfg = cfg orelse return err_invalid_arg;
    const c_fw = fw orelse return err_invalid_arg;
    var len = compose.Lengths{};
    run(c_cfg, c_fw, &len) catch |err| {
        publish(len, c_fw);
        return code(err);
    };
    publish(len, c_fw);
    return err_ok;
}
