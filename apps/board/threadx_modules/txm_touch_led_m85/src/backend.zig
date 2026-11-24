//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Resident-image backend for txm_touch_led_m85 (RA8FW-1262): bridges
//! `ra8_app_api_t` dispatches to EK-RA8D2 hardware (`ra8_touch_read` for GT911
//! touch events and `ra8_board_led_on` / `ra8_board_led_off` for status LEDs)
//! plus a 64x64 ARGB8888 region shadow buffer.

const std = @import("std");
const app_api = @import("app_resident").api;

const abi = app_api.abi;
const kernel = app_api.kernel;
const memory = app_api.memory;
const Status = abi.Status;
const Word = abi.Word;

/// offsetof(TXM_MODULE_INSTANCE, txm_module_instance_data_start) / `_data_end`
/// on the cortex_m33 module port headers used by the M85 manager.
const data_start_offset = 0x2C;
const data_end_offset = 0x30;

const screen_width: u16 = 64;
const screen_height: u16 = 64;
var shadow: [@as(usize, screen_width) * screen_height * 4]u8 = undefined;

var loaded = false;
var app_id_buf: [32]u8 = @splat(0);
var app_id_len: usize = 0;
var capabilities: u32 = 0;
var instance: ?*anyopaque = null;

var exited = false;
var exit_code: u32 = 0;
var start_tick: u32 = 0;
var ctx_token: u8 = 0;

const ok: u16 = 0;
const led_count: u32 = 3;

const TouchPoint = extern struct {
    x: u16 = 0,
    y: u16 = 0,
    track_id: u8 = 0,
    pressure: u8 = 0,
};

extern fn ra8_board_uart_console_write(data: [*]const u8, len: usize) c_int;
extern fn ra8_board_uart_console_flush() c_int;
extern fn ra8_board_led_on(led: u8) callconv(.c) u16;
extern fn ra8_board_led_off(led: u8) callconv(.c) u16;
extern fn ra8_touch_read(out_points: [*]TouchPoint, max_count: u8, got_count: *u8) callconv(.c) u16;
extern fn _tx_thread_sleep(ticks: u32) callconv(.c) u32;
extern fn _tx_time_get() callconv(.c) u32;

pub fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

pub fn setManifest(id: []const u8, caps: u32) void {
    const copied = @min(id.len, app_id_buf.len);
    @memcpy(app_id_buf[0..copied], id[0..copied]);
    app_id_len = copied;
    capabilities = caps;
}

pub fn registered(at: *anyopaque) void {
    instance = at;
    loaded = true;
    exited = false;
    exit_code = 0;
}

pub fn deregistered() void {
    loaded = false;
    instance = null;
}

pub fn markStart() void {
    start_tick = _tx_time_get();
}

pub fn waitExit() u32 {
    while (true) {
        if (@atomicLoad(bool, &exited, .seq_cst)) return exit_code;
        _ = _tx_thread_sleep(1);
    }
}

fn backendExit(_: *anyopaque, _: *const kernel.Caller, code: u32) void {
    exit_code = code;
    @atomicStore(bool, &exited, true, .seq_cst);
}

fn backendTicks(_: *anyopaque, _: *const kernel.Caller) u32 {
    return _tx_time_get() -% start_tick;
}

fn backendSleep(_: *anyopaque, _: *const kernel.Caller, ms: u32) void {
    _ = _tx_thread_sleep(ms);
}

fn backendScreen(_: *anyopaque, _: *const kernel.Caller) abi.ScreenInfo {
    return .{ .width = screen_width, .height = screen_height, .bytes_per_pixel = 4 };
}

fn backendFill(_: *anyopaque, _: *const kernel.Caller, rect: abi.Rect, argb: u32) Status {
    var y: u32 = rect.y;
    while (y < @as(u32, rect.y) + rect.h) : (y += 1) {
        var x: u32 = rect.x;
        while (x < @as(u32, rect.x) + rect.w) : (x += 1) {
            std.mem.writeInt(u32, shadow[(y * screen_width + x) * 4 ..][0..4], argb, .little);
        }
    }
    return .ok;
}

fn backendBlit(_: *anyopaque, _: *const kernel.Caller, rect: abi.Rect, pixels: []const u8) Status {
    var y: u32 = 0;
    while (y < rect.h) : (y += 1) {
        const row_bytes = @as(usize, rect.w) * 4;
        const src = pixels[y * row_bytes ..][0..row_bytes];
        const dst = (@as(usize, rect.y) + y) * screen_width + rect.x;
        @memcpy(shadow[dst * 4 ..][0..row_bytes], src);
    }
    return .ok;
}

fn backendOpen(_: *anyopaque, _: *const kernel.Caller, _: []const u8, _: u32, _: *u32) Status {
    return .not_found;
}

fn backendRead(_: *anyopaque, _: *const kernel.Caller, _: u32, _: []u8, _: *u32) Status {
    return .not_found;
}

fn backendWrite(_: *anyopaque, _: *const kernel.Caller, _: u32, _: []const u8, _: *u32) Status {
    return .not_found;
}

fn backendClose(_: *anyopaque, _: *const kernel.Caller, _: u32) Status {
    return .not_found;
}

fn backendTouch(_: *anyopaque, _: *const kernel.Caller, event: *abi.TouchEvent) Status {
    var pts: [1]TouchPoint = .{.{}};
    var got: u8 = 0;
    if (ra8_touch_read(&pts, 1, &got) != ok) return .io_error;
    if (got == 0) return .no_event;
    event.* = .{
        .kind = .press,
        .x = pts[0].x,
        .y = pts[0].y,
        .button = 0,
    };
    var line: [64]u8 = undefined;
    const text = std.fmt.bufPrint(&line, "txm_touch_led_m85: touch x={d} y={d}\r\n", .{ pts[0].x, pts[0].y }) catch return .ok;
    say(text);
    return .ok;
}

fn backendLed(_: *anyopaque, _: *const kernel.Caller, index: u32, on: bool) Status {
    if (index >= led_count) return .bad_argument;
    const led_id: u8 = @intCast(index);
    const status = if (on) ra8_board_led_on(led_id) else ra8_board_led_off(led_id);
    if (status != ok) return .io_error;
    var line: [48]u8 = undefined;
    const state = if (on) "ON" else "OFF";
    const text = std.fmt.bufPrint(&line, "txm_touch_led_m85: LED{d} {s}\r\n", .{ index + 1, state }) catch return .ok;
    say(text);
    return .ok;
}

const backend_impl = kernel.Backend{
    .ctx = &ctx_token,
    .exit = &backendExit,
    .ticks = &backendTicks,
    .sleep = &backendSleep,
    .screen = &backendScreen,
    .fill = &backendFill,
    .blit = &backendBlit,
    .open = &backendOpen,
    .read = &backendRead,
    .write = &backendWrite,
    .close = &backendClose,
    .touch = &backendTouch,
    .led = &backendLed,
};

fn ownedRanges(at: *anyopaque) memory.Owned {
    return .{ .data = .{
        .start = readWord(at, data_start_offset),
        .end = readWord(at, data_end_offset) + 1,
    } };
}

fn readWord(base: *anyopaque, offset: usize) u32 {
    const bytes: [*]u8 = @ptrCast(base);
    const word: *align(1) u32 = @ptrCast(bytes + offset);
    return word.*;
}

fn serve(request: u32, p1: Word, p2: Word, p3: Word) u32 {
    if (!loaded) return @backingInt(Status.unknown_request);
    const at = instance orelse return @backingInt(Status.unknown_request);
    const caller = kernel.Caller{
        .app_id = app_id_buf[0..app_id_len],
        .capabilities = capabilities,
        .owned = ownedRanges(at),
    };
    return kernel.dispatch(backend_impl, &caller, request, p1, p2, p3);
}

export fn _txm_module_manager_application_request(request: u32, p1: u32, p2: u32, p3: u32) callconv(.c) u32 {
    const served = serve(request, p1, p2, p3);
    const is_empty_poll = (request == @backingInt(abi.Request.touch_event)) and (served == @backingInt(Status.no_event));
    if (!is_empty_poll) {
        var line: [64]u8 = undefined;
        const text = std.fmt.bufPrint(&line, "txm_touch_led_m85: request {d} -> {d}\r\n", .{ request, served }) catch return served;
        say(text);
    }
    return served;
}
