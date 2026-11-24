//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure validation and step sequencing for `ra8_usb_device_open` (RA8FW-317).
//! Contains no middleware externs or C ABI pointers so every rule can be
//! proved off target against minimal fakes.

const std = @import("std");

/// Maximum NUL-terminated class name length copied for stack registration.
pub const class_name_max_len: usize = 63;

/// `ra8_usb_device_port_t` controller selection values.
pub const Port = enum(u8) {
    fs = 0,
    hs = 1,
};

pub const Error = error{
    InvalidArg,
    InvalidState,
};

/// Plain validation snapshot extracted from `ra8_usb_device_open_cfg_t`.
pub const OpenSpec = struct {
    port: u8,
    configuration: u8,
    interface: u8,
    has_pool: bool,
    pool_bytes: u32,
    has_device_cfg: bool,
    has_frameworks: bool,
    has_fs_device: bool,
    fs_device_len: u32,
    has_strings: bool,
    strings_len: u32,
    has_langid: bool,
    langid_len: u32,
    has_hs_device: bool,
    hs_device_len: u32,
    has_ops: bool,
    has_system_init: bool,
    has_device_stack_init: bool,
    has_class_register: bool,
    has_error_cb_register: bool,
    has_dcd_open: bool,
    has_class_driver: bool,
    has_class_name: bool,
    class_name_len: usize,
    has_class_entry: bool,
    has_class_param: bool,
    has_error_cb: bool,
};

/// Validate configuration presence, port rules, ops callbacks, and pairing.
pub fn validateStatic(spec: OpenSpec) Error!Port {
    const port = std.enums.fromInt(Port, spec.port) orelse return Error.InvalidArg;
    if (spec.configuration == 0) return Error.InvalidArg;
    if (!spec.has_pool or spec.pool_bytes == 0) return Error.InvalidArg;
    if (!spec.has_frameworks) return Error.InvalidArg;
    if (spec.has_hs_device != (spec.hs_device_len != 0)) return Error.InvalidArg;
    if (port == .fs and (spec.has_hs_device or spec.hs_device_len != 0)) return Error.InvalidArg;
    if (!spec.has_ops or !spec.has_system_init or !spec.has_device_stack_init or !spec.has_class_register) {
        return Error.InvalidArg;
    }
    if (spec.has_error_cb and !spec.has_error_cb_register) return Error.InvalidArg;
    if (!spec.has_class_driver or !spec.has_class_name or !spec.has_class_entry or !spec.has_class_param) {
        return Error.InvalidArg;
    }
    if (spec.class_name_len == 0 or spec.class_name_len > class_name_max_len) {
        return Error.InvalidArg;
    }
    return port;
}

/// Validate framework pointers and lengths right before `deviceStackInit`.
pub fn validateFrameworks(spec: OpenSpec) Error!void {
    if (!spec.has_fs_device or spec.fs_device_len == 0) return Error.InvalidArg;
    if (!spec.has_strings or spec.strings_len == 0) return Error.InvalidArg;
    if (!spec.has_langid or spec.langid_len == 0) return Error.InvalidArg;
}

/// Drive compose (when requested) and the seam callbacks in stack order.
pub fn execute(spec: OpenSpec, backend: anytype) !void {
    _ = try validateStatic(spec);
    if (spec.has_device_cfg) {
        try backend.compose();
    } else {
        try validateFrameworks(spec);
    }
    if (!backend.systemInit()) return Error.InvalidState;
    if (spec.has_error_cb) {
        if (!backend.registerErrorCb()) return Error.InvalidState;
    }
    if (!backend.deviceStackInit()) return Error.InvalidState;
    if (!backend.classRegister()) return Error.InvalidState;
    if (spec.has_dcd_open) {
        if (!backend.dcdOpen()) return Error.InvalidState;
    }
}
