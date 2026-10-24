//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The module's side of `ra8_app_api_t`: a typed surface of thin shims, each
//! one `txm_module_application_request`. Nothing here runs kernel code or
//! holds a kernel pointer; it packs arguments into words and reads back the
//! status.
//!
//! The request entry point is a comptime value, so a module binds
//! `_txm_module_application_request` (see `target`) and the host tests bind a
//! fake that calls `kernel.dispatch` directly.

/// The ABI both sides agree on, exported for the modules that bind the
/// shims: the request numbers, the structs, the status codes (RA8FW-1023).
pub const abi = @import("abi.zig");

const Status = abi.Status;
const Word = abi.Word;

/// `UINT txm_module_application_request(ULONG request, ALIGN_TYPE p1, p2, p3)`.
pub const RequestFn = *const fn (request: u32, p1: Word, p2: Word, p3: Word) callconv(.c) u32;

extern fn _txm_module_application_request(request: u32, p1: Word, p2: Word, p3: Word) callconv(.c) u32;

/// The entry point a module links: ThreadX's module library.
pub const target: RequestFn = &_txm_module_application_request;

/// Errors a call can come back with, one per non-`ok` status.
pub const Error = error{
    UnknownRequest,
    Denied,
    BadPointer,
    BadArgument,
    NotFound,
    IoError,
    NoResource,
    Unexpected,
};

/// Map a status word onto `Error`. `no_event` is not an error and is handled
/// by `touchEvent` before this is reached.
pub fn check(word: u32) Error!void {
    return switch (@as(Status, @fromBackingInt(@intCast(word)))) {
        .ok => {},
        .unknown_request => Error.UnknownRequest,
        .denied => Error.Denied,
        .bad_pointer => Error.BadPointer,
        .bad_argument => Error.BadArgument,
        .not_found => Error.NotFound,
        .io_error => Error.IoError,
        .no_resource => Error.NoResource,
        else => Error.Unexpected,
    };
}

fn addr(pointer: anytype) Word {
    return @intFromPtr(pointer);
}

/// The API over one request entry point.
pub fn Api(comptime request: RequestFn) type {
    return struct {
        fn call(req: abi.Request, p1: Word, p2: Word, p3: Word) u32 {
            return request(@backingInt(req), p1, p2, p3);
        }

        /// The generation the firmware serves.
        pub fn apiVersion() u32 {
            return call(.api_version, 0, 0, 0);
        }

        /// Tell the host the module is done.
        pub fn exit(code: u32) Error!void {
            return check(call(.exit, code, 0, 0));
        }

        /// Milliseconds since the module started, wrapping.
        pub fn ticks() Error!u32 {
            var now: u32 = 0;
            try check(call(.ticks, addr(&now), 0, 0));
            return now;
        }

        pub fn sleep(ms: u32) Error!void {
            return check(call(.sleep, ms, 0, 0));
        }

        /// The module's screen region.
        pub fn screenInfo() Error!abi.ScreenInfo {
            var info: abi.ScreenInfo = .{};
            try check(call(.screen_info, addr(&info), 0, 0));
            return info;
        }

        pub fn drawRect(rect: abi.Rect, argb: u32) Error!void {
            return check(call(.draw_rect, addr(&rect), argb, 0));
        }

        /// ARGB8888 rows, `rect.w * rect.h * 4` bytes.
        pub fn drawImage(rect: abi.Rect, pixels: []const u8) Error!void {
            return check(call(.draw_image, addr(&rect), addr(pixels.ptr), pixels.len));
        }

        /// Open `path`, relative to the module's own directory.
        pub fn fileOpen(path: []const u8, flags: u32) Error!u32 {
            var block: abi.FileOpen = .{ .path = addr(path.ptr), .path_len = @intCast(path.len), .flags = flags };
            try check(call(.file_open, addr(&block), 0, 0));
            return block.handle;
        }

        /// Read into `buffer`; the bytes read.
        pub fn fileRead(handle: u32, buffer: []u8) Error!u32 {
            var block: abi.FileIo = .{ .handle = handle, .buffer = addr(buffer.ptr), .len = @intCast(buffer.len) };
            try check(call(.file_read, addr(&block), 0, 0));
            return block.done;
        }

        /// Write `bytes`; the bytes written.
        pub fn fileWrite(handle: u32, bytes: []const u8) Error!u32 {
            var block: abi.FileIo = .{ .handle = handle, .buffer = addr(bytes.ptr), .len = @intCast(bytes.len) };
            try check(call(.file_write, addr(&block), 0, 0));
            return block.done;
        }

        pub fn fileClose(handle: u32) Error!void {
            return check(call(.file_close, handle, 0, 0));
        }

        /// The next input event, or null when none is queued.
        pub fn touchEvent() Error!?abi.TouchEvent {
            var event: abi.TouchEvent = .{};
            const word = call(.touch_event, addr(&event), 0, 0);
            if (word == @backingInt(Status.no_event)) return null;
            try check(word);
            return event;
        }
    };
}
