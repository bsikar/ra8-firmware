//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ABI tests for `ra8_usb_device_open`: verifies C struct layouts, validation
//! codes, optional compose execution, HS+FS forwarding, error callback wiring,
//! and seam invocation ordering without referencing any stack middleware symbol.

const std = @import("std");
const open_abi = @import("open_abi");
const compose_abi = open_abi.compose;

const err_ok: u16 = 0;
const err_no_mem: u16 = 0x102;
const err_invalid_arg: u16 = 0x103;
const err_invalid_size: u16 = 0x105;
const err_hw_init_failed: u16 = 0x201;

extern fn ra8_usb_device_open(cfg: ?*const open_abi.COpenConfig) callconv(.c) u16;

var pool_buf: [4096]u8 = undefined;
var fs_buf: [256]u8 = undefined;
var hs_buf: [256]u8 = undefined;
var strings_buf: [256]u8 = undefined;
var langid_buf: [8]u8 = undefined;
var class_param_dummy: u32 = 0x12345678;

const dev_desc = compose_abi.desc.CDevice{
    .vid = 0x1209,
    .pid = 0x000A,
    .bcd_device = 0x0100,
    .manufacturer = "Brighton",
    .product = "RA8 Open Test",
    .serial = "0001",
    .langid = 0x0409,
    .max_power_ma = 100,
    .self_powered = 0,
    .remote_wakeup = 0,
};

const cdc_cfg = compose_abi.desc.CCdcAcm{
    .notify_ep = 0x83,
    .notify_bytes = 8,
    .notify_interval_ms = 255,
    .out_ep = 0x02,
    .in_ep = 0x81,
    .data_bytes = 64,
    .high_speed = 0,
};

const classes = [_]compose_abi.CClass{
    .{ .kind = 1, .body = .{ .cdc_acm = cdc_cfg } },
};

const compose_cfg = compose_abi.CConfig{
    .desc = &dev_desc,
    .classes = &classes,
    .class_count = 1,
};

const Spy = struct {
    var seq_len: usize = 0;
    var seq: [8]u8 = @splat(0);
    var system_err: u16 = err_ok;
    var error_reg_err: u16 = err_ok;
    var stack_err: u16 = err_ok;
    var class_err: u16 = err_ok;
    var dcd_err: u16 = err_ok;
    var last_port: u8 = 255;
    var last_hs_len: u32 = 0;
    var last_fs_len: u32 = 0;
    var last_cfg: u8 = 0;
    var last_iface: u8 = 255;
    var last_param: ?*anyopaque = null;
    var last_cb: ?open_abi.ErrorCb = null;

    fn reset() void {
        seq_len = 0;
        seq = @splat(0);
        system_err = err_ok;
        error_reg_err = err_ok;
        stack_err = err_ok;
        class_err = err_ok;
        dcd_err = err_ok;
        last_port = 255;
        last_hs_len = 0;
        last_fs_len = 0;
        last_cfg = 0;
        last_iface = 255;
        last_param = null;
        last_cb = null;
    }

    fn push(tag: u8) void {
        seq[seq_len] = tag;
        seq_len += 1;
    }

    fn systemInit(pool: ?*anyopaque, pool_bytes: u32) callconv(.c) u16 {
        _ = pool;
        _ = pool_bytes;
        push('S');
        return system_err;
    }

    fn errorCbReg(cb: ?open_abi.ErrorCb) callconv(.c) u16 {
        push('E');
        last_cb = cb;
        return error_reg_err;
    }

    fn stackInit(
        hs_fw: ?[*]u8,
        hs_len: u32,
        fs_fw: ?[*]u8,
        fs_len: u32,
        strings_fw: ?[*]u8,
        strings_len: u32,
        langid_fw: ?[*]u8,
        langid_len: u32,
    ) callconv(.c) u16 {
        _ = hs_fw;
        _ = fs_fw;
        _ = strings_fw;
        _ = strings_len;
        _ = langid_fw;
        _ = langid_len;
        push('D');
        last_hs_len = hs_len;
        last_fs_len = fs_len;
        return stack_err;
    }

    fn classRegister(
        name: [*:0]const u8,
        entry: open_abi.ClassEntryFn,
        cfg_num: u8,
        if_num: u8,
        param: ?*anyopaque,
    ) callconv(.c) u16 {
        _ = name;
        _ = entry;
        push('R');
        last_cfg = cfg_num;
        last_iface = if_num;
        last_param = param;
        return class_err;
    }

    fn dcdOpen(port: u8) callconv(.c) u16 {
        push('O');
        last_port = port;
        return dcd_err;
    }

    fn trace() []const u8 {
        return seq[0..seq_len];
    }
};

fn fakeClassEntry(command: ?*anyopaque) callconv(.c) u32 {
    _ = command;
    return 0;
}

fn fakeErrorCb(_: u32, _: u32, _: u32) callconv(.c) void {}

const driver = open_abi.CClassDriver{
    .name = "test_class_cdc",
    .entry = fakeClassEntry,
};

const full_ops = open_abi.CStackOps{
    .system_init = Spy.systemInit,
    .error_cb_register = Spy.errorCbReg,
    .device_stack_init = Spy.stackInit,
    .class_register = Spy.classRegister,
    .dcd_open = Spy.dcdOpen,
};

fn frameworksTemplate() compose_abi.CFrameworks {
    return .{
        .device = &fs_buf,
        .device_cap = fs_buf.len,
        .device_len = 0,
        .strings = &strings_buf,
        .strings_cap = strings_buf.len,
        .strings_len = 0,
        .langid = &langid_buf,
        .langid_cap = langid_buf.len,
        .langid_len = 0,
    };
}

fn validCfg(fw: *compose_abi.CFrameworks) open_abi.COpenConfig {
    return .{
        .port = 0,
        .configuration = 1,
        .interface = 0,
        .pool_bytes = pool_buf.len,
        .pool = &pool_buf,
        .device_cfg = &compose_cfg,
        .frameworks = fw,
        .hs_device_framework = null,
        .ops = &full_ops,
        .class_driver = &driver,
        .class_param = &class_param_dummy,
        .error_cb = null,
        .hs_device_framework_len = 0,
    };
}

test "open composes descriptors and runs stack ops in exact order" {
    Spy.reset();
    var fw = frameworksTemplate();
    var cfg = validCfg(&fw);
    cfg.error_cb = fakeErrorCb;

    try std.testing.expectEqual(err_ok, ra8_usb_device_open(&cfg));
    try std.testing.expectEqualStrings("SEDRO", Spy.trace());
    try std.testing.expectEqual(@as(u32, 93), fw.device_len);
    try std.testing.expectEqual(@as(u32, 93), Spy.last_fs_len);
    try std.testing.expectEqual(@as(u32, 0), Spy.last_hs_len);
    try std.testing.expectEqual(@as(u8, 0), Spy.last_port);
    try std.testing.expectEqual(@as(u8, 1), Spy.last_cfg);
    try std.testing.expectEqual(@as(u8, 0), Spy.last_iface);
    try std.testing.expectEqual(@as(?*anyopaque, &class_param_dummy), Spy.last_param);
    try std.testing.expect(Spy.last_cb == fakeErrorCb);
}

test "open accepts pre-built HS and FS frameworks on HS port" {
    Spy.reset();
    var fw = frameworksTemplate();
    fw.device_len = 93;
    fw.strings_len = 32;
    fw.langid_len = 2;

    var cfg = validCfg(&fw);
    cfg.port = 1;
    cfg.device_cfg = null;
    cfg.hs_device_framework = &hs_buf;
    cfg.hs_device_framework_len = 103;

    try std.testing.expectEqual(err_ok, ra8_usb_device_open(&cfg));
    try std.testing.expectEqualStrings("SDRO", Spy.trace());
    try std.testing.expectEqual(@as(u8, 1), Spy.last_port);
    try std.testing.expectEqual(@as(u32, 103), Spy.last_hs_len);
    try std.testing.expectEqual(@as(u32, 93), Spy.last_fs_len);
}

test "compose error stops before any ops callback executes" {
    Spy.reset();
    var fw = frameworksTemplate();
    fw.device_cap = 8;
    const cfg = validCfg(&fw);

    try std.testing.expectEqual(err_invalid_size, ra8_usb_device_open(&cfg));
    try std.testing.expectEqual(@as(usize, 0), Spy.seq_len);
}

test "failing ops callback returns exact caller error code and halts sequence" {
    Spy.reset();
    var fw = frameworksTemplate();
    const cfg = validCfg(&fw);

    Spy.system_err = err_no_mem;
    try std.testing.expectEqual(err_no_mem, ra8_usb_device_open(&cfg));
    try std.testing.expectEqualStrings("S", Spy.trace());

    Spy.reset();
    Spy.dcd_err = err_hw_init_failed;
    try std.testing.expectEqual(err_hw_init_failed, ra8_usb_device_open(&cfg));
    try std.testing.expectEqualStrings("SDRO", Spy.trace());
}

test "null cfg and bad arguments return err_invalid_arg without calling ops" {
    Spy.reset();
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_open(null));

    var fw = frameworksTemplate();
    var bad = validCfg(&fw);
    bad.pool = null;
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_open(&bad));

    bad = validCfg(&fw);
    bad.ops = null;
    try std.testing.expectEqual(err_invalid_arg, ra8_usb_device_open(&bad));
    try std.testing.expectEqual(@as(usize, 0), Spy.seq_len);
}
