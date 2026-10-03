//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The messages the tests speak, and the golden frame each one must make.
//!
//! The frames under `fixtures/` were written out by hand from the wire rules,
//! one field per line, not captured from the encoder. A fixture that only
//! records what the code did cannot say the code is wrong.

const std = @import("std");
const rpc = @import("ra8_rpc");

/// The envelope the tests speak: bodies of at most 32 bytes.
pub const Env = rpc.Envelope(32);

pub const Mode = enum(u8) { idle = 0, run = 1, halt = 7 };
pub const Region = enum(u16) { boot = 0x0100, app = 0xA55A };

pub const Empty = struct {};

pub const Ints = struct { a: u8, b: u16, c: u32, d: u64, e: i8, f: i16, g: i32, h: i64 };

pub const Tagged = struct { mode: Mode, region: Region };

pub const Blob = struct {
    addr: u32,
    data: []const u8,

    pub const max_len = .{ .data = 16 };
};

pub const Mixed = struct {
    mode: Mode,
    name: []const u8,
    seq: u16,
    body: []const u8,

    pub const max_len = .{ .name = 8, .body = 32 };
};

/// A tag that is not 0, 1, 2: the wire carries the enum's value, not its index.
pub const Which = enum(u8) { none = 0, code = 1, text = 4 };

pub const Body = union(Which) {
    none: void,
    code: u16,
    text: []const u8,

    pub const max_len = .{ .text = 8 };
};

pub const Reply = struct { seq: u16, body: Body };

/// A value, the kind it is framed under, and the bytes that frame must be.
pub fn Case(comptime T: type) type {
    return struct { kind: u16, value: T, golden: []const u8 };
}

/// `Mixed.body` at its bound: 0x00 through 0x1F.
const ramp: [Mixed.max_len.body]u8 = blk: {
    var bytes: [Mixed.max_len.body]u8 = undefined;
    for (&bytes, 0..) |*byte, i| byte.* = i;
    break :blk bytes;
};

pub const cases = .{
    Case(Empty){ .kind = 0x0001, .value = .{}, .golden = golden("empty") },
    Case(Ints){ .kind = 0x0102, .golden = golden("ints"), .value = .{
        .a = 0x01,
        .b = 0x0203,
        .c = 0x04050607,
        .d = 0x08090A0B0C0D0E0F,
        .e = -2,
        .f = -2,
        .g = -3,
        .h = -4,
    } },
    Case(Tagged){ .kind = 0x0003, .golden = golden("tagged"), .value = .{
        .mode = .halt,
        .region = .app,
    } },
    // A zero byte inside the data: a slice carries its length, not a terminator.
    Case(Blob){ .kind = 0x0004, .golden = golden("blob"), .value = .{
        .addr = 0x22000000,
        .data = "\xDE\xAD\xBE\xEF\x00",
    } },
    Case(Blob){ .kind = 0x0004, .golden = golden("blob_empty"), .value = .{
        .addr = 0,
        .data = "",
    } },
    Case(Mixed){ .kind = 0xBEEF, .golden = golden("mixed"), .value = .{
        .mode = .run,
        .name = "ra8",
        .seq = 0x1234,
        .body = &ramp,
    } },
    Case(Reply){ .kind = 0x0011, .golden = golden("reply_none"), .value = .{
        .seq = 1,
        .body = .none,
    } },
    Case(Reply){ .kind = 0x0011, .golden = golden("reply_code"), .value = .{
        .seq = 2,
        .body = .{ .code = 0xBEEF },
    } },
    Case(Reply){ .kind = 0x0011, .golden = golden("reply_text"), .value = .{
        .seq = 3,
        .body = .{ .text = "hi" },
    } },
    Case(rpc.Hello){ .kind = rpc.Kind.hello, .golden = golden("hello"), .value = .{
        .caps = 0x0000_0005,
    } },
    Case(rpc.Fault){ .kind = rpc.Kind.fault, .golden = golden("fault"), .value = .{
        .code = .version_mismatch,
    } },
    Case(Env.Request){ .kind = rpc.Kind.request, .golden = golden("request"), .value = .{
        .id = 7,
        .method = 0x0102,
        .args = "\xDE\xAD",
    } },
    Case(Env.Response){ .kind = rpc.Kind.response, .golden = golden("response_ok"), .value = .{
        .id = 7,
        .result = .{ .ok = "ok!" },
    } },
    Case(Env.Response){ .kind = rpc.Kind.response, .golden = golden("response_err"), .value = .{
        .id = 8,
        .result = .{ .err = .unknown_method },
    } },
    Case(Env.Event){ .kind = rpc.Kind.event, .golden = golden("event"), .value = .{
        .topic = 9,
        .payload = "UART",
    } },
};

/// The bytes of `fixtures/<name>.txt`: hex pairs separated by whitespace.
fn golden(comptime name: []const u8) []const u8 {
    comptime {
        @setEvalBranchQuota(100_000);
        const text = @embedFile("fixtures/" ++ name ++ ".txt");
        var bytes: [text.len / 2]u8 = undefined;
        var count: usize = 0;
        var pairs = std.mem.tokenizeAny(u8, text, " \n");
        while (pairs.next()) |pair| : (count += 1) {
            bytes[count] = std.fmt.parseInt(u8, pair, 16) catch @compileError(name ++ ": bad hex");
        }
        const frame = bytes[0..count].*;
        return &frame;
    }
}
