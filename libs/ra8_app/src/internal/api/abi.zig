//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_app_api_t`: the calls a loaded module can make into the resident
//! image (RA8FW-294). This file is the ABI both sides agree on, the request
//! numbers, the structs a module passes by pointer, and the status codes. It
//! holds no behaviour.
//!
//! A call is one ThreadX application request. The module's shim calls
//! `txm_module_application_request(request, p1, p2, p3)`, ThreadX adds
//! `TXM_APPLICATION_REQUEST_ID_BASE` and traps into the Module Manager, and
//! the manager hands `request` back to the resident image's
//! `_txm_module_manager_application_request`, where `kernel.dispatch` checks
//! and serves it. No kernel function pointer ever crosses into the module.
//!
//! Requests are append-only. A number is never reused or renumbered, and a
//! call whose meaning changes gets a new number. `version` counts the
//! generations; a `.ra8app` whose `min_api_version` is above it is refused at
//! load (`appimg.parse`), so a module never calls a number this firmware does
//! not serve.

const std = @import("std");

/// API generation this firmware serves. Equal to the `.ra8app` format's
/// `api_version_current`; a test holds the two together.
pub const version: u32 = 1;

/// One argument word: `ALIGN_TYPE`, 32 bits on the Cortex-M, the pointer
/// width on the host the tests run on.
pub const Word = usize;

/// The request numbers, below `TXM_APPLICATION_REQUEST_ID_BASE`.
///
/// Append-only. Zero is never a request, so a zeroed word cannot be taken for
/// one.
pub const Request = enum(u32) {
    /// `() -> version`. Answers with the generation in the status word.
    api_version = 1,
    /// `(code)`. The module is done; the host stops and unloads it.
    exit = 2,
    /// `(*u32 out)`. Milliseconds since the module started, wrapping.
    ticks = 3,
    /// `(milliseconds)`. Suspends the calling thread.
    sleep = 4,
    /// `(*ScreenInfo out)`. The module's screen region. Needs `display`.
    screen_info = 5,
    /// `(*const Rect, argb)`. Fills a rectangle of the region. Needs `display`.
    draw_rect = 6,
    /// `(*const Rect, pixels, bytes)`. ARGB8888 rows, `w * h * 4` bytes.
    /// Needs `display`.
    draw_image = 7,
    /// `(*FileOpen)`. Opens a file under the module's own directory.
    /// Needs `storage`.
    file_open = 8,
    /// `(*FileIo)`. Reads up to `len` bytes. Needs `storage`.
    file_read = 9,
    /// `(*FileIo)`. Writes `len` bytes. Needs `storage`.
    file_write = 10,
    /// `(handle)`. Closes a handle `file_open` gave. Needs `storage`.
    file_close = 11,
    /// `(*TouchEvent out)`. The next touch or button event, if any.
    /// Needs `display`: input belongs to the screen region it lands on.
    touch_event = 12,
    _,

    /// The last request this generation defines.
    pub const last: Request = .touch_event;
};

/// What a call answers in its status word. `ok` is zero, so a module can test
/// for success the way it tests a ThreadX service.
pub const Status = enum(u32) {
    ok = 0,
    /// The request number is not one this firmware serves.
    unknown_request = 1,
    /// The module's manifest does not hold the capability the call needs.
    denied = 2,
    /// A pointer, or the bytes it covers, is outside the module's memory.
    bad_pointer = 3,
    /// An argument is out of range: a rectangle off the region, a bad path,
    /// a length that does not match.
    bad_argument = 4,
    /// The file or handle does not exist.
    not_found = 5,
    /// The host's backend failed.
    io_error = 6,
    /// No handle or buffer is free.
    no_resource = 7,
    /// `touch_event` had nothing queued.
    no_event = 8,
    _,
};

/// A rectangle in the module's region, origin top left.
pub const Rect = extern struct {
    x: u16 = 0,
    y: u16 = 0,
    w: u16 = 0,
    h: u16 = 0,
};

/// The module's screen region. Drawing is in region coordinates.
pub const ScreenInfo = extern struct {
    width: u16 = 0,
    height: u16 = 0,
    /// Bytes per pixel `draw_image` takes. 4 (ARGB8888) in generation 1.
    bytes_per_pixel: u16 = 0,
    reserved: u16 = 0,
};

/// Open flags, one bit each.
pub const OpenFlag = struct {
    pub const read: u32 = 0x1;
    pub const write: u32 = 0x2;
    /// With `write`: create the file if it is missing, truncate it if not.
    pub const create: u32 = 0x4;
    /// Every bit generation 1 defines.
    pub const known: u32 = 0x7;
};

/// `file_open`'s argument block, in module memory.
pub const FileOpen = extern struct {
    /// Path relative to the module's directory, not NUL terminated.
    path: Word = 0,
    path_len: u32 = 0,
    flags: u32 = 0,
    /// Out: the handle, valid until `file_close`.
    handle: u32 = 0,
};

/// `file_read` and `file_write`'s argument block, in module memory.
pub const FileIo = extern struct {
    handle: u32 = 0,
    /// The buffer, in module memory.
    buffer: Word = 0,
    len: u32 = 0,
    /// Out: the bytes moved.
    done: u32 = 0,
};

/// One input event.
pub const TouchEvent = extern struct {
    kind: Kind = .none,
    x: u16 = 0,
    y: u16 = 0,
    /// Button number for `.button_*`, zero otherwise.
    button: u16 = 0,

    pub const Kind = enum(u16) { none = 0, press = 1, move = 2, release = 3, button_down = 4, button_up = 5, _ };
};

/// Longest path, relative to the module's directory, `file_open` takes.
pub const path_max: u32 = 96;

comptime {
    std.debug.assert(@sizeOf(Rect) == 8);
    std.debug.assert(@sizeOf(ScreenInfo) == 8);
    std.debug.assert(@sizeOf(TouchEvent) == 8);
    std.debug.assert(@offsetOf(FileOpen, "path_len") == @sizeOf(Word));
    std.debug.assert(@offsetOf(FileIo, "buffer") == @max(4, @alignOf(Word)));
}
