//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A backend for the `ra8_app_api_t` tests: a 32x16 region, one file, and a
//! queue of one touch event. It counts every call, so a test can prove a
//! refusal never reached it.

const std = @import("std");
const api = @import("app_api");
const abi = api.abi;
const kernel = api.kernel;

pub const Fake = struct {
    calls: u32 = 0,
    exited: ?u32 = null,
    slept: u32 = 0,
    filled: ?abi.Rect = null,
    colour: u32 = 0,
    blitted: usize = 0,
    opened: [64]u8 = undefined,
    opened_len: usize = 0,
    file: [32]u8 = @splat(0),
    file_len: usize = 0,
    open_handle: ?u32 = null,
    queued: ?abi.TouchEvent = null,

    pub const handle: u32 = 7;
    pub const screen = abi.ScreenInfo{ .width = 32, .height = 16, .bytes_per_pixel = 4 };

    fn of(ctx: *anyopaque) *Fake {
        const self: *Fake = @ptrCast(@alignCast(ctx));
        self.calls += 1;
        return self;
    }

    fn exitFn(ctx: *anyopaque, _: *const kernel.Caller, code: u32) void {
        of(ctx).exited = code;
    }

    fn ticksFn(ctx: *anyopaque, _: *const kernel.Caller) u32 {
        _ = of(ctx);
        return 1234;
    }

    fn sleepFn(ctx: *anyopaque, _: *const kernel.Caller, ms: u32) void {
        of(ctx).slept = ms;
    }

    fn screenFn(ctx: *anyopaque, _: *const kernel.Caller) abi.ScreenInfo {
        // Asked by the kernel to bound a rectangle; not a call of its own.
        _ = ctx;
        return screen;
    }

    fn fillFn(ctx: *anyopaque, _: *const kernel.Caller, rect: abi.Rect, argb: u32) abi.Status {
        const self = of(ctx);
        self.filled = rect;
        self.colour = argb;
        return .ok;
    }

    fn blitFn(ctx: *anyopaque, _: *const kernel.Caller, _: abi.Rect, pixels: []const u8) abi.Status {
        of(ctx).blitted = pixels.len;
        return .ok;
    }

    fn openFn(ctx: *anyopaque, _: *const kernel.Caller, path: []const u8, _: u32, out: *u32) abi.Status {
        const self = of(ctx);
        @memcpy(self.opened[0..path.len], path);
        self.opened_len = path.len;
        self.open_handle = handle;
        out.* = handle;
        return .ok;
    }

    fn readFn(ctx: *anyopaque, _: *const kernel.Caller, h: u32, buffer: []u8, done: *u32) abi.Status {
        const self = of(ctx);
        if (self.open_handle != h) return .not_found;
        const n = @min(buffer.len, self.file_len);
        @memcpy(buffer[0..n], self.file[0..n]);
        done.* = @intCast(n);
        return .ok;
    }

    fn writeFn(ctx: *anyopaque, _: *const kernel.Caller, h: u32, bytes: []const u8, done: *u32) abi.Status {
        const self = of(ctx);
        if (self.open_handle != h) return .not_found;
        const n = @min(bytes.len, self.file.len);
        @memcpy(self.file[0..n], bytes[0..n]);
        self.file_len = n;
        done.* = @intCast(n);
        return .ok;
    }

    fn closeFn(ctx: *anyopaque, _: *const kernel.Caller, h: u32) abi.Status {
        const self = of(ctx);
        if (self.open_handle != h) return .not_found;
        self.open_handle = null;
        return .ok;
    }

    fn touchFn(ctx: *anyopaque, _: *const kernel.Caller, event: *abi.TouchEvent) abi.Status {
        const self = of(ctx);
        event.* = self.queued orelse return .no_event;
        self.queued = null;
        return .ok;
    }

    pub fn backend(self: *Fake) kernel.Backend {
        return .{
            .ctx = self,
            .exit = exitFn,
            .ticks = ticksFn,
            .sleep = sleepFn,
            .screen = screenFn,
            .fill = fillFn,
            .blit = blitFn,
            .open = openFn,
            .read = readFn,
            .write = writeFn,
            .close = closeFn,
            .touch = touchFn,
        };
    }

    pub fn openedPath(self: *const Fake) []const u8 {
        return self.opened[0..self.opened_len];
    }
};

/// Module memory for a test: a buffer the caller's data region covers
/// exactly, so anything outside it is not the module's.
pub const Arena = struct {
    bytes: [512]u8 align(16) = @splat(0),

    pub fn owned(self: *Arena) api.memory.Owned {
        const start = @intFromPtr(&self.bytes);
        return .{ .data = .{ .start = start, .end = start + self.bytes.len } };
    }

    /// A `T` at `offset` into the arena.
    pub fn at(self: *Arena, comptime T: type, offset: usize) *T {
        return @ptrCast(@alignCast(&self.bytes[offset]));
    }

    pub fn word(self: *Arena, offset: usize) abi.Word {
        return @intFromPtr(&self.bytes[offset]);
    }
};

pub const all = api.appimg.Capability.display | api.appimg.Capability.storage;

pub fn caller(arena: *Arena, capabilities: u32) kernel.Caller {
    return .{ .app_id = "com.example.notes", .capabilities = capabilities, .owned = arena.owned() };
}

pub fn status(word: u32) abi.Status {
    return @fromBackingInt(@intCast(word));
}
