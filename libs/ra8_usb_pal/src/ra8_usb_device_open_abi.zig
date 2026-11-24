//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `ra8_usb_device_open`, declared by
//! `inc/ra8_usb_compose.h`. Validates configuration, port rules, and framework
//! buffers, runs `ra8_usb_device_compose` when `device_cfg` is supplied, and
//! dispatches stack initialisation through the stack-agnostic `ra8_usb_stack_ops_t`
//! seam without referencing any middleware symbol in this archive.

const std = @import("std");
const compose_abi = @import("ra8_usb_compose_abi.zig");
const device_open = @import("internal/device_open.zig");

pub const compose = compose_abi;

const err_ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_invalid_state: u16 = 0x104;

pub const ErrorCb = *const fn (system_level: u32, system_context: u32, error_code: u32) callconv(.c) void;
pub const ClassEntryFn = *const fn (command: ?*anyopaque) callconv(.c) u32;

/// `ra8_usb_device_class_driver_t`.
pub const CClassDriver = extern struct {
    name: ?[*:0]const u8,
    entry: ?ClassEntryFn,
};

/// `ra8_usb_stack_ops_t`.
pub const CStackOps = extern struct {
    system_init: ?*const fn (pool: ?*anyopaque, pool_bytes: u32) callconv(.c) u16,
    error_cb_register: ?*const fn (error_cb: ?ErrorCb) callconv(.c) u16,
    device_stack_init: ?*const fn (
        hs_device_fw: ?[*]u8,
        hs_device_fw_len: u32,
        fs_device_fw: ?[*]u8,
        fs_device_fw_len: u32,
        strings_fw: ?[*]u8,
        strings_fw_len: u32,
        langid_fw: ?[*]u8,
        langid_fw_len: u32,
    ) callconv(.c) u16,
    class_register: ?*const fn (
        class_name: [*:0]const u8,
        class_entry: ClassEntryFn,
        configuration: u8,
        interface: u8,
        class_param: ?*anyopaque,
    ) callconv(.c) u16,
    dcd_open: ?*const fn (port: u8) callconv(.c) u16,
};

/// `ra8_usb_device_open_cfg_t`.
pub const COpenConfig = extern struct {
    port: u8,
    configuration: u8,
    interface: u8,
    pool_bytes: u32,
    pool: ?*anyopaque,
    device_cfg: ?*const compose_abi.CConfig,
    frameworks: ?*compose_abi.CFrameworks,
    hs_device_framework: ?[*]u8,
    ops: ?*const CStackOps,
    class_driver: ?*const CClassDriver,
    class_param: ?*anyopaque,
    error_cb: ?ErrorCb,
    hs_device_framework_len: u32,
};

comptime {
    const pointer_bytes = @sizeOf(usize);
    std.debug.assert(@sizeOf(CClassDriver) == 2 * pointer_bytes);
    std.debug.assert(@offsetOf(CClassDriver, "entry") == pointer_bytes);
    std.debug.assert(@sizeOf(CStackOps) == 5 * pointer_bytes);
    std.debug.assert(@offsetOf(CStackOps, "dcd_open") == 4 * pointer_bytes);
    std.debug.assert(@offsetOf(COpenConfig, "pool_bytes") == 4);
    std.debug.assert(@offsetOf(COpenConfig, "pool") == 8);
    std.debug.assert(@offsetOf(COpenConfig, "device_cfg") == 8 + pointer_bytes);
    std.debug.assert(@offsetOf(COpenConfig, "frameworks") == 8 + 2 * pointer_bytes);
    std.debug.assert(@offsetOf(COpenConfig, "ops") == 8 + 4 * pointer_bytes);
    std.debug.assert(@offsetOf(COpenConfig, "error_cb") == 8 + 7 * pointer_bytes);
    std.debug.assert(@offsetOf(COpenConfig, "hs_device_framework_len") == 8 + 8 * pointer_bytes);
    std.debug.assert(device_open.class_name_max_len == 63);
}

fn boundedClassNameLen(maybe_name: ?[*:0]const u8) usize {
    const name_z = maybe_name orelse return 0;
    var len: usize = 0;
    while (len <= device_open.class_name_max_len and name_z[len] != 0) : (len += 1) {}
    return len;
}

fn buildSpec(cfg: *const COpenConfig) device_open.OpenSpec {
    const fw = cfg.frameworks;
    const ops = cfg.ops;
    const driver = cfg.class_driver;
    const class_name = if (driver) |d| d.name else null;
    return .{
        .port = cfg.port,
        .configuration = cfg.configuration,
        .interface = cfg.interface,
        .has_pool = cfg.pool != null,
        .pool_bytes = cfg.pool_bytes,
        .has_device_cfg = cfg.device_cfg != null,
        .has_frameworks = fw != null,
        .has_fs_device = if (fw) |f| f.device != null else false,
        .fs_device_len = if (fw) |f| f.device_len else 0,
        .has_strings = if (fw) |f| f.strings != null else false,
        .strings_len = if (fw) |f| f.strings_len else 0,
        .has_langid = if (fw) |f| f.langid != null else false,
        .langid_len = if (fw) |f| f.langid_len else 0,
        .has_hs_device = cfg.hs_device_framework != null,
        .hs_device_len = cfg.hs_device_framework_len,
        .has_ops = ops != null,
        .has_system_init = if (ops) |o| o.system_init != null else false,
        .has_device_stack_init = if (ops) |o| o.device_stack_init != null else false,
        .has_class_register = if (ops) |o| o.class_register != null else false,
        .has_error_cb_register = if (ops) |o| o.error_cb_register != null else false,
        .has_dcd_open = if (ops) |o| o.dcd_open != null else false,
        .has_class_driver = driver != null,
        .has_class_name = class_name != null,
        .class_name_len = boundedClassNameLen(class_name),
        .has_class_entry = if (driver) |d| d.entry != null else false,
        .has_class_param = cfg.class_param != null,
        .has_error_cb = cfg.error_cb != null,
    };
}

const CBackend = struct {
    cfg: *const COpenConfig,
    last_err: u16 = err_ok,

    fn failWith(self: *CBackend, rc: u16) bool {
        if (rc == err_ok) return true;
        self.last_err = rc;
        return false;
    }

    pub fn compose(self: *CBackend) device_open.Error!void {
        const rc = compose_abi.composeNullable(self.cfg.device_cfg, self.cfg.frameworks);
        if (rc != err_ok) {
            self.last_err = rc;
            return device_open.Error.InvalidState;
        }
        const fw = self.cfg.frameworks orelse return device_open.Error.InvalidArg;
        if (fw.device == null or fw.device_len == 0 or
            fw.strings == null or fw.strings_len == 0 or
            fw.langid == null or fw.langid_len == 0)
        {
            return device_open.Error.InvalidArg;
        }
    }

    pub fn systemInit(self: *CBackend) bool {
        const ops = self.cfg.ops orelse return self.failWith(err_invalid_arg);
        const f = ops.system_init orelse return self.failWith(err_invalid_arg);
        return self.failWith(f(self.cfg.pool, self.cfg.pool_bytes));
    }

    pub fn registerErrorCb(self: *CBackend) bool {
        const ops = self.cfg.ops orelse return self.failWith(err_invalid_arg);
        const f = ops.error_cb_register orelse return self.failWith(err_invalid_arg);
        return self.failWith(f(self.cfg.error_cb));
    }

    pub fn deviceStackInit(self: *CBackend) bool {
        const ops = self.cfg.ops orelse return self.failWith(err_invalid_arg);
        const f = ops.device_stack_init orelse return self.failWith(err_invalid_arg);
        const fw = self.cfg.frameworks orelse return self.failWith(err_invalid_arg);
        return self.failWith(f(
            self.cfg.hs_device_framework,
            self.cfg.hs_device_framework_len,
            fw.device,
            fw.device_len,
            fw.strings,
            fw.strings_len,
            fw.langid,
            fw.langid_len,
        ));
    }

    pub fn classRegister(self: *CBackend) bool {
        const ops = self.cfg.ops orelse return self.failWith(err_invalid_arg);
        const f = ops.class_register orelse return self.failWith(err_invalid_arg);
        const driver = self.cfg.class_driver orelse return self.failWith(err_invalid_arg);
        const name = driver.name orelse return self.failWith(err_invalid_arg);
        const entry = driver.entry orelse return self.failWith(err_invalid_arg);
        return self.failWith(f(
            name,
            entry,
            self.cfg.configuration,
            self.cfg.interface,
            self.cfg.class_param,
        ));
    }

    pub fn dcdOpen(self: *CBackend) bool {
        const ops = self.cfg.ops orelse return self.failWith(err_invalid_arg);
        const f = ops.dcd_open orelse return true;
        return self.failWith(f(self.cfg.port));
    }
};

// =============================================================================
// Export (inc/ra8_usb_compose.h)
// =============================================================================

export fn ra8_usb_device_open(cfg: ?*const COpenConfig) callconv(.c) u16 {
    const c_cfg = cfg orelse return err_invalid_arg;
    const spec = buildSpec(c_cfg);
    var backend = CBackend{ .cfg = c_cfg };
    device_open.execute(spec, &backend) catch |err| {
        if (backend.last_err != err_ok) return backend.last_err;
        return switch (err) {
            device_open.Error.InvalidArg => err_invalid_arg,
            device_open.Error.InvalidState => err_invalid_state,
        };
    };
    return err_ok;
}
