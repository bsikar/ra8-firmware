//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host-test fake for the symbols the net PAL ABI leaves extern: the
//! `ra8_eth` seam and three log entry points. Tests stage results and read
//! call counts through the plain Zig functions below. Ported from
//! abi_fixture.c (RA8FW-637).

pub const EthHandler = ?*const fn (ctx: ?*anyopaque, status_mask: u32) callconv(.c) void;

/// Mirrors ra8_eth_link_t from libs/ra8_hal/inc/ra8_eth.h.
const EthLink = extern struct {
    link_up: u8,
    speed_mbps: u16,
    full_duplex: u8,
    bmsr: u16,
};

const link_not_ready: u16 = 0x010F;

var eth_init_result: u16 = 0;
var eth_deinit_result: u16 = 0;
var eth_init_calls: u32 = 0;
var eth_deinit_calls: u32 = 0;
var eth_attach_calls: u32 = 0;
var attached_handler: EthHandler = null;
var eth_link_result: u16 = 0;
var eth_link_up: u8 = 0;
var eth_link_calls: u32 = 0;
var log_error_calls: u32 = 0;
var log_info_calls: u32 = 0;
var log_error_val_calls: u32 = 0;
var log_last_error_message: [*:0]const u8 = "";
var log_last_error_value: u32 = 0;

fn ethInit() callconv(.c) u16 {
    eth_init_calls += 1;
    return eth_init_result;
}

fn ethDeinit() callconv(.c) u16 {
    eth_deinit_calls += 1;
    return eth_deinit_result;
}

fn ethAttachHandler(handler: EthHandler, ctx: ?*anyopaque) callconv(.c) void {
    _ = ctx;
    eth_attach_calls += 1;
    attached_handler = handler;
}

fn ethLinkStatus(out_status: *EthLink) callconv(.c) u16 {
    eth_link_calls += 1;
    if (eth_link_result != 0) return eth_link_result;
    out_status.* = .{ .link_up = eth_link_up, .speed_mbps = 0, .full_duplex = 0, .bmsr = 0 };
    return 0;
}

fn logEmitError(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void {
    _ = tag;
    log_error_calls += 1;
    log_last_error_message = message;
}

fn logEmitInfo(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void {
    _ = tag;
    _ = message;
    log_info_calls += 1;
}

fn logEmitErrorVal(tag: [*:0]const u8, message: [*:0]const u8, value: u32) callconv(.c) void {
    _ = tag;
    log_error_val_calls += 1;
    log_last_error_message = message;
    log_last_error_value = value;
}

comptime {
    @export(&ethInit, .{ .name = "ra8_eth_init" });
    @export(&ethDeinit, .{ .name = "ra8_eth_deinit" });
    @export(&ethAttachHandler, .{ .name = "ra8_eth_attach_handler" });
    @export(&ethLinkStatus, .{ .name = "ra8_eth_link_status" });
    @export(&logEmitError, .{ .name = "ra8_log_emit_error" });
    @export(&logEmitInfo, .{ .name = "ra8_log_emit_info" });
    @export(&logEmitErrorVal, .{ .name = "ra8_log_emit_error_val" });
}

pub fn reset() void {
    eth_init_result = 0;
    eth_deinit_result = 0;
    eth_init_calls = 0;
    eth_deinit_calls = 0;
    eth_attach_calls = 0;
    attached_handler = null;
    eth_link_result = link_not_ready;
    eth_link_up = 0;
    eth_link_calls = 0;
    log_error_calls = 0;
    log_info_calls = 0;
    log_error_val_calls = 0;
    log_last_error_message = "";
    log_last_error_value = 0;
}

pub fn setEthInitResult(result: u16) void {
    eth_init_result = result;
}

pub fn setEthDeinitResult(result: u16) void {
    eth_deinit_result = result;
}

pub fn setEthLinkResult(result: u16) void {
    eth_link_result = result;
}

pub fn setEthLinkUp(link_up: u8) void {
    eth_link_up = link_up;
}

pub fn ethInitCalls() u32 {
    return eth_init_calls;
}

pub fn ethDeinitCalls() u32 {
    return eth_deinit_calls;
}

pub fn ethAttachCalls() u32 {
    return eth_attach_calls;
}

pub fn ethLinkCalls() u32 {
    return eth_link_calls;
}

pub fn attachedHandler() EthHandler {
    return attached_handler;
}

pub fn logErrorCalls() u32 {
    return log_error_calls;
}

pub fn logInfoCalls() u32 {
    return log_info_calls;
}

pub fn logErrorValCalls() u32 {
    return log_error_val_calls;
}

pub fn logLastErrorMessage() [*:0]const u8 {
    return log_last_error_message;
}

pub fn logLastErrorValue() u32 {
    return log_last_error_value;
}
