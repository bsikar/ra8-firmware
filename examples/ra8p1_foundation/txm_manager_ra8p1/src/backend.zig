//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The resident-image half of txm_manager_ra8p1 (RA8FW-680). The image's
//! `_txm_module_manager_application_request` is the hook the ThreadX Module
//! Manager calls for an application request a loaded module makes: the
//! manager's kernel dispatch adds `TXM_APPLICATION_REQUEST_ID_BASE` and
//! hands the call to the image (RA8FW-294). This file names the calling
//! module from the manifest the app admitted, reads the module's data range
//! off the manager's module instance, and serves every request through
//! `app_api.kernel.dispatch` with this app's backend: a 64x64 ARGB8888
//! region backed by a shadow buffer, files in a small RAM store where the
//! card examples use the FAT volume (ra8_fs), the tick clock, the sleep,
//! and an empty touch queue. Every dispatch logs one line, the shell's
//! evidence.

const std = @import("std");
/// The resident image's whole ra8_app surface, bound once (the two
/// internal roots import each other's files).
const app_api = @import("app_resident").api;

const abi = app_api.abi;
const kernel = app_api.kernel;
const memory = app_api.memory;
const sandbox = app_api.sandbox;
const Status = abi.Status;
const Word = abi.Word;

/// offsetof(TXM_MODULE_INSTANCE, txm_module_instance_data_start) and
/// `..._data_end`, measured with an offset probe compiled from the
/// cortex_m33 module port headers -- the M85 manager builds from that port
/// -- with the build's own defines and `tx_user.h`. Both sit before the
/// port extension, so no build define moves them. The ranges pair with the
/// manager-side constants txm_manager_m85 reads off a run (RA8FW-825).
const data_start_offset = 0x2C;
const data_end_offset = 0x30;

/// The screen fixture: a 64x64 ARGB8888 region, the module's own.
const screen_width = 64;
const screen_height = 64;
var shadow: [screen_width * screen_height * 4]u8 = undefined;

/// The one loaded module, as the app registers it. The manager dispatches
/// only module requests and this example loads one module at a time, so the
/// registry has one slot; a host that loads several keys them off the
/// calling thread instead.
var loaded = false;
var app_id_buf: [32]u8 = @splat(0);
var app_id_len: usize = 0;
var capabilities: u32 = 0;
var instance: ?*anyopaque = null;

/// The exit request recorded, and the code it carried.
var exited = false;
var exit_code: u32 = 0;

/// The tick the module started at, for `ticks`' answer.
var start_tick: u32 = 0;

var ctx_token: u8 = 0;

extern fn ra8_board_uart_console_write(data: [*]const u8, len: usize) c_int;
extern fn ra8_board_uart_console_flush() c_int;
extern fn _tx_thread_sleep(ticks: u32) callconv(.c) u32;
extern fn _tx_time_get() callconv(.c) u32;

/// One console line, the shell's evidence.
pub fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

/// Names the module the app admitted, before the manager loads it.
pub fn setManifest(id: []const u8, caps: u32) void {
    const copied = @min(id.len, app_id_buf.len);
    @memcpy(app_id_buf[0..copied], id[0..copied]);
    app_id_len = copied;
    capabilities = caps;
}

/// Names the module the manager just loaded, so the dispatch knows whose
/// data range to hand the kernel.
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

/// Sets the tick `ticks` counts from; the manager half calls it at start.
pub fn markStart() void {
    start_tick = _tx_time_get();
}

/// Waits for the module's exit request and reports its code. Null when the
/// module never exited within the ticks.
pub fn waitExit(tick_limit: u32) ?u32 {
    var waited: u32 = 0;
    while (waited < tick_limit) : (waited += 1) {
        if (@atomicLoad(bool, &exited, .seq_cst)) return exit_code;
        _ = _tx_thread_sleep(1);
    }
    return null;
}

fn backendExit(ctx: *anyopaque, caller: *const kernel.Caller, code: u32) void {
    _ = ctx;
    _ = caller;
    exit_code = code;
    @atomicStore(bool, &exited, true, .seq_cst);
}

/// Milliseconds since the module started; the tick is one millisecond
/// (port/threadx/inc/tx_user.h), so the ticks are the milliseconds.
fn backendTicks(ctx: *anyopaque, caller: *const kernel.Caller) u32 {
    _ = ctx;
    _ = caller;
    return _tx_time_get() -% start_tick;
}

fn backendSleep(ctx: *anyopaque, caller: *const kernel.Caller, ms: u32) void {
    _ = ctx;
    _ = caller;
    _ = _tx_thread_sleep(ms);
}

fn backendScreen(ctx: *anyopaque, caller: *const kernel.Caller) abi.ScreenInfo {
    _ = ctx;
    _ = caller;
    return .{ .width = screen_width, .height = screen_height, .bytes_per_pixel = 4 };
}

fn fillRect(rect: abi.Rect, argb: u32) void {
    var y: u32 = rect.y;
    while (y < @as(u32, rect.y) + rect.h) : (y += 1) {
        var x: u32 = rect.x;
        while (x < @as(u32, rect.x) + rect.w) : (x += 1) {
            std.mem.writeInt(u32, shadow[(y * screen_width + x) * 4 ..][0..4], argb, .little);
        }
    }
}

fn backendFill(ctx: *anyopaque, caller: *const kernel.Caller, rect: abi.Rect, argb: u32) Status {
    _ = ctx;
    _ = caller;
    fillRect(rect, argb);
    return .ok;
}

fn backendBlit(ctx: *anyopaque, caller: *const kernel.Caller, rect: abi.Rect, pixels: []const u8) Status {
    _ = ctx;
    _ = caller;
    // The kernel checked the length and the bounds; copy the rows in.
    var y: u32 = 0;
    while (y < rect.h) : (y += 1) {
        const row_bytes = @as(usize, rect.w) * 4;
        const src = pixels[y * row_bytes ..][0..row_bytes];
        const dst = (@as(usize, rect.y) + y) * screen_width + rect.x;
        @memcpy(shadow[dst * 4 ..][0..row_bytes], src);
    }
    return .ok;
}

/// The RAM file store: one entry per joined path, surviving close, so the
/// shell's write-then-read-back sidecar sees its own bytes. The bytes cap
/// covers the sidecar's payloads; a full store or a full entry is a real
/// refusal, not a silent drop.
const path_max = 96;
const file_bytes_max = 1024;
const store_max = 4;
const Entry = struct {
    used: bool = false,
    path_len: usize = 0,
    path: [path_max]u8 = @splat(0),
    bytes: [file_bytes_max]u8 = undefined,
    len: usize = 0,
};
var store: [store_max]Entry = @splat(.{});

/// The open files, one slot each; the handle is the slot plus one, so zero
/// is never a handle.
const handle_max = 4;
const Open = struct { entry: ?usize = null, cursor: usize = 0 };
var files: [handle_max]Open = @splat(.{});

fn slotOf(handle: u32) ?usize {
    if (handle == 0 or handle > handle_max) return null;
    const slot = handle - 1;
    return if (files[slot].entry != null) slot else null;
}

/// Finds the entry for `path` or claims an empty one. The kernel
/// sandbox-joins the module's relative path before it reaches the backend
/// (sandbox.zig: the backend only ever sees the joined path), so this keys
/// on the joined path verbatim.
fn entryOf(path: []const u8) ?usize {
    if (path.len > path_max) return null;
    for (0..store_max) |i| {
        const entry = &store[i];
        if (entry.used and entry.path_len == path.len and std.mem.eql(u8, entry.path[0..entry.path_len], path)) return i;
    }
    for (0..store_max) |i| {
        const entry = &store[i];
        if (!entry.used) {
            entry.used = true;
            entry.path_len = path.len;
            @memcpy(entry.path[0..path.len], path);
            entry.len = 0;
            return i;
        }
    }
    return null;
}

fn backendOpen(ctx: *anyopaque, caller: *const kernel.Caller, path: []const u8, flags: u32, out: *u32) Status {
    _ = ctx;
    _ = caller;
    for (&files, 0..) |*file, slot| {
        if (file.entry != null) continue;
        const at = entryOf(path) orelse return .no_resource;
        // The kernel checked the flags; write is create-or-truncate
        // (libs/ra8_fs/inc/ra8_fs_types.h's mode_write), read never
        // truncates.
        if (flags & abi.OpenFlag.write != 0) store[at].len = 0;
        file.* = .{ .entry = at, .cursor = 0 };
        out.* = @intCast(slot + 1);
        return .ok;
    }
    return .no_resource;
}

fn backendRead(ctx: *anyopaque, caller: *const kernel.Caller, handle: u32, buffer: []u8, done: *u32) Status {
    _ = ctx;
    _ = caller;
    const slot = slotOf(handle) orelse return .bad_argument;
    const entry = &store[files[slot].entry.?];
    const f = &files[slot];
    const remaining = entry.len - f.cursor;
    const got = @min(remaining, buffer.len);
    @memcpy(buffer[0..got], entry.bytes[f.cursor..][0..got]);
    f.cursor += got;
    done.* = @intCast(got);
    return .ok;
}

fn backendWrite(ctx: *anyopaque, caller: *const kernel.Caller, handle: u32, bytes: []const u8, done: *u32) Status {
    _ = ctx;
    _ = caller;
    const slot = slotOf(handle) orelse return .bad_argument;
    const entry = &store[files[slot].entry.?];
    if (entry.len + bytes.len > file_bytes_max) return .io_error;
    @memcpy(entry.bytes[entry.len..][0..bytes.len], bytes);
    entry.len += bytes.len;
    done.* = @intCast(bytes.len);
    return .ok;
}

fn backendClose(ctx: *anyopaque, caller: *const kernel.Caller, handle: u32) Status {
    _ = ctx;
    _ = caller;
    const slot = slotOf(handle) orelse return .bad_argument;
    files[slot] = .{};
    return .ok;
}

/// No input is wired in this example: the queue stays empty.
fn backendTouch(ctx: *anyopaque, caller: *const kernel.Caller, event: *abi.TouchEvent) Status {
    _ = ctx;
    _ = caller;
    _ = event;
    return .no_event;
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
};

/// The module's data range, straight off the manager's module instance:
/// `[txm_module_instance_data_start, txm_module_instance_data_end + 1)`,
/// which is what the module's data covers (memory.zig pairs the two).
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

/// Serves one request for the one loaded module. With no module loaded
/// there is no caller, and no request is one this image serves.
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
    var line: [64]u8 = undefined;
    const text = std.fmt.bufPrint(&line, "txm_manager_ra8p1: request {d} -> {d}\r\n", .{ request, served }) catch return served;
    say(text);
    return served;
}
