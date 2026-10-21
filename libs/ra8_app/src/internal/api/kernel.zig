//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The resident image's side of `ra8_app_api_t`: one `dispatch` that the
//! Module Manager's `_txm_module_manager_application_request` hands every
//! module request to. It refuses in a fixed order, unknown request, then a
//! capability the manifest does not hold, then a pointer outside the module,
//! then a bad argument, and only then calls the backend. A refusal never
//! reaches the backend.
//!
//! Argument blocks are copied out of module memory before they are checked,
//! so another module thread cannot change one between the check and the use.
//! Out values are written back after the backend answers.

const std = @import("std");
const abi = @import("abi.zig");
const memory = @import("memory.zig");
const sandbox = @import("sandbox.zig");
const Capability = @import("../appimg.zig").Capability;

const Status = abi.Status;
const Word = abi.Word;

/// The module making the call, as the manager knows it. `capabilities` is the
/// admitted header's manifest, already checked against the host's grant.
pub const Caller = struct {
    app_id: []const u8,
    capabilities: u32,
    owned: memory.Owned,
};

/// What serves the calls: the display, the files, the input queue and the
/// module's lifecycle. Every entry gets the caller, so the backend can keep
/// each module's handles and screen region apart.
pub const Backend = struct {
    ctx: *anyopaque,
    exit: *const fn (ctx: *anyopaque, caller: *const Caller, code: u32) void,
    ticks: *const fn (ctx: *anyopaque, caller: *const Caller) u32,
    sleep: *const fn (ctx: *anyopaque, caller: *const Caller, ms: u32) void,
    screen: *const fn (ctx: *anyopaque, caller: *const Caller) abi.ScreenInfo,
    fill: *const fn (ctx: *anyopaque, caller: *const Caller, rect: abi.Rect, argb: u32) Status,
    blit: *const fn (ctx: *anyopaque, caller: *const Caller, rect: abi.Rect, pixels: []const u8) Status,
    open: *const fn (ctx: *anyopaque, caller: *const Caller, path: []const u8, flags: u32, handle: *u32) Status,
    read: *const fn (ctx: *anyopaque, caller: *const Caller, handle: u32, buffer: []u8, done: *u32) Status,
    write: *const fn (ctx: *anyopaque, caller: *const Caller, handle: u32, bytes: []const u8, done: *u32) Status,
    close: *const fn (ctx: *anyopaque, caller: *const Caller, handle: u32) Status,
    touch: *const fn (ctx: *anyopaque, caller: *const Caller, event: *abi.TouchEvent) Status,
};

/// The capability a request needs, or `none` when it needs nothing.
pub fn needs(request: abi.Request) u32 {
    return switch (request) {
        .screen_info, .draw_rect, .draw_image, .touch_event => Capability.display,
        .file_open, .file_read, .file_write, .file_close => Capability.storage,
        else => Capability.none,
    };
}

/// Whether `request` is one this generation serves.
pub fn known(request: u32) bool {
    return request >= 1 and request <= @backingInt(abi.Request.last);
}

/// Serve one module request. The answer is the status word the module's shim
/// returns, except `api_version`, which answers with the generation.
pub fn dispatch(backend: Backend, caller: *const Caller, request: u32, p1: Word, p2: Word, p3: Word) u32 {
    if (!known(request)) return @backingInt(Status.unknown_request);
    const call: abi.Request = @fromBackingInt(@intCast(request));
    const need = needs(call);
    if ((caller.capabilities & need) != need) return @backingInt(Status.denied);
    if (call == .api_version) return abi.version;
    return @backingInt(serve(backend, caller, call, p1, p2, p3));
}

fn serve(backend: Backend, caller: *const Caller, call: abi.Request, p1: Word, p2: Word, p3: Word) Status {
    return switch (call) {
        .api_version => unreachable, // answered in `dispatch`
        .exit => exit(backend, caller, p1),
        .ticks => if (owns(u32, caller, p1)) put(u32, caller, p1, backend.ticks(backend.ctx, caller)) else .bad_pointer,
        .sleep => sleep(backend, caller, p1),
        .screen_info => if (owns(abi.ScreenInfo, caller, p1)) put(abi.ScreenInfo, caller, p1, backend.screen(backend.ctx, caller)) else .bad_pointer,
        .draw_rect => drawRect(backend, caller, p1, p2),
        .draw_image => drawImage(backend, caller, p1, p2, p3),
        .file_open => fileOpen(backend, caller, p1),
        .file_read => fileIo(backend, caller, p1, .read),
        .file_write => fileIo(backend, caller, p1, .write),
        .file_close => backend.close(backend.ctx, caller, narrow(p1) orelse return .bad_argument),
        .touch_event => touchEvent(backend, caller, p1),
        _ => .unknown_request,
    };
}

/// A word as a `u32` argument, or null when it does not fit one.
fn narrow(word: Word) ?u32 {
    return std.math.cast(u32, word);
}

/// Whether a `T` at `address` is the module's. Checked before the backend is
/// asked for a value to store there, so a refusal never reaches it.
fn owns(comptime T: type, caller: *const Caller, address: Word) bool {
    return caller.owned.holdsFor(T, address);
}

/// Copy a `T` out of module memory, or null when `address` is not the module's.
fn take(comptime T: type, caller: *const Caller, address: Word) ?T {
    if (!caller.owned.holdsFor(T, address)) return null;
    const source: *const T = @ptrFromInt(address);
    return source.*;
}

/// Copy `value` into module memory at `address`.
fn put(comptime T: type, caller: *const Caller, address: Word, value: T) Status {
    if (!caller.owned.holdsFor(T, address)) return .bad_pointer;
    const sink: *T = @ptrFromInt(address);
    sink.* = value;
    return .ok;
}

/// `len` bytes of module memory at `address`, or null when they are not all
/// the module's.
fn bytes(caller: *const Caller, address: Word, len: u32) ?[]u8 {
    if (!caller.owned.holds(address, len)) return null;
    const base: [*]u8 = @ptrFromInt(address);
    return base[0..len];
}

fn exit(backend: Backend, caller: *const Caller, code: Word) Status {
    backend.exit(backend.ctx, caller, narrow(code) orelse return .bad_argument);
    return .ok;
}

fn sleep(backend: Backend, caller: *const Caller, ms: Word) Status {
    backend.sleep(backend.ctx, caller, narrow(ms) orelse return .bad_argument);
    return .ok;
}

/// Whether `rect` is non-empty and lies inside the module's region.
fn inside(rect: abi.Rect, screen: abi.ScreenInfo) bool {
    if (rect.w == 0 or rect.h == 0) return false;
    return @as(u32, rect.x) + rect.w <= screen.width and @as(u32, rect.y) + rect.h <= screen.height;
}

fn drawRect(backend: Backend, caller: *const Caller, rect_at: Word, argb: Word) Status {
    const rect = take(abi.Rect, caller, rect_at) orelse return .bad_pointer;
    const colour = narrow(argb) orelse return .bad_argument;
    if (!inside(rect, backend.screen(backend.ctx, caller))) return .bad_argument;
    return backend.fill(backend.ctx, caller, rect, colour);
}

fn drawImage(backend: Backend, caller: *const Caller, rect_at: Word, pixels_at: Word, len: Word) Status {
    const rect = take(abi.Rect, caller, rect_at) orelse return .bad_pointer;
    const size = narrow(len) orelse return .bad_argument;
    const pixels = bytes(caller, pixels_at, size) orelse return .bad_pointer;
    const screen = backend.screen(backend.ctx, caller);
    if (!inside(rect, screen)) return .bad_argument;
    const want = @as(u64, rect.w) * rect.h * screen.bytes_per_pixel;
    if (want != size) return .bad_argument;
    return backend.blit(backend.ctx, caller, rect, pixels);
}

fn fileOpen(backend: Backend, caller: *const Caller, block_at: Word) Status {
    var block = take(abi.FileOpen, caller, block_at) orelse return .bad_pointer;
    if ((block.flags & ~abi.OpenFlag.known) != 0) return .bad_argument;
    if ((block.flags & (abi.OpenFlag.read | abi.OpenFlag.write)) == 0) return .bad_argument;
    if (block.path_len == 0 or block.path_len > abi.path_max) return .bad_argument;
    const path = bytes(caller, block.path, block.path_len) orelse return .bad_pointer;

    var local: [abi.path_max]u8 = undefined;
    @memcpy(local[0..path.len], path);
    var joined: [sandbox.joined_max]u8 = undefined;
    const full = sandbox.join(&joined, caller.app_id, local[0..path.len]) catch return .bad_argument;

    const status = backend.open(backend.ctx, caller, full, block.flags, &block.handle);
    if (status != .ok) return status;
    return put(abi.FileOpen, caller, block_at, block);
}

const Direction = enum { read, write };

fn fileIo(backend: Backend, caller: *const Caller, block_at: Word, direction: Direction) Status {
    var block = take(abi.FileIo, caller, block_at) orelse return .bad_pointer;
    const buffer = bytes(caller, block.buffer, block.len) orelse return .bad_pointer;
    block.done = 0;
    const status = switch (direction) {
        .read => backend.read(backend.ctx, caller, block.handle, buffer, &block.done),
        .write => backend.write(backend.ctx, caller, block.handle, buffer, &block.done),
    };
    if (block.done > block.len) return .io_error;
    const stored = put(abi.FileIo, caller, block_at, block);
    return if (status != .ok) status else stored;
}

fn touchEvent(backend: Backend, caller: *const Caller, event_at: Word) Status {
    if (!owns(abi.TouchEvent, caller, event_at)) return .bad_pointer;
    var event: abi.TouchEvent = .{};
    const status = backend.touch(backend.ctx, caller, &event);
    if (status != .ok) return status;
    return put(abi.TouchEvent, caller, event_at, event);
}
