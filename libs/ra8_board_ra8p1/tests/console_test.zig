//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Console policy: the PCLKA floor, the init ordering, the initialized gate and
//! the bounded drain, all against the scripted SCI double.

const std = @import("std");
const console_mod = @import("console");
const fake = @import("sci_fake.zig");

const vocab = console_mod.vocab;

const Err = vocab.Err;
const Wiring = console_mod.Wiring;
const Console = console_mod.Console(fake);

fn freshConsole() Console {
    fake.state.clear();
    return .{};
}

test "init rejects a zero baud before touching the clock" {
    var console = freshConsole();
    try std.testing.expectEqual(Err.invalid_arg, console.init(0));
    try std.testing.expectEqual(@as(usize, 0), fake.state.init_count);
    try std.testing.expect(!console.initialized);
}

test "init refuses a pre-pll pclka" {
    var console = freshConsole();
    fake.state.pclka_hz = Wiring.min_pclka_hz - 1;
    try std.testing.expectEqual(Err.not_initialized, console.init(115200));
    try std.testing.expectEqual(@as(usize, 0), fake.state.routed_count);
    try std.testing.expect(!console.initialized);
}

test "init accepts exactly the floor" {
    var console = freshConsole();
    fake.state.pclka_hz = Wiring.min_pclka_hz;
    try std.testing.expectEqual(Err.ok, console.init(115200));
    try std.testing.expectEqual(Wiring.min_pclka_hz, fake.state.init_pclk_hz);
}

test "init routes txd then rxd and hands the live pclka to the channel" {
    var console = freshConsole();
    try std.testing.expectEqual(Err.ok, console.init(115200));
    try std.testing.expectEqual(@as(usize, 2), fake.state.routed_count);
    try std.testing.expectEqual(Wiring.pin_txd, fake.state.routed[0]);
    try std.testing.expectEqual(Wiring.pin_rxd, fake.state.routed[1]);
    try std.testing.expectEqual(@as(u32, 115200), fake.state.init_baud);
    try std.testing.expectEqual(@as(u32, 100_000_000), fake.state.init_pclk_hz);
    try std.testing.expect(console.initialized);
}

test "a failed route leaves the gate shut and never configures the channel" {
    var console = freshConsole();
    fake.state.route_err = Err.invalid_arg;
    try std.testing.expectEqual(Err.invalid_arg, console.init(115200));
    try std.testing.expectEqual(@as(usize, 0), fake.state.init_count);
    try std.testing.expect(!console.initialized);
}

test "a failed channel init leaves the gate shut" {
    var console = freshConsole();
    fake.state.init_err = Err.invalid_arg;
    try std.testing.expectEqual(Err.invalid_arg, console.init(115200));
    try std.testing.expect(!console.initialized);
}

test "a clock read failure is returned as-is" {
    var console = freshConsole();
    fake.state.pclka_err = Err.not_initialized;
    try std.testing.expectEqual(Err.not_initialized, console.init(115200));
}

test "an empty write is a no-op even with no buffer and no init" {
    var console = freshConsole();
    try std.testing.expectEqual(Err.ok, console.write(null, 0));
    try std.testing.expectEqual(Err.invalid_arg, console.write(null, 1));
    try std.testing.expectEqual(@as(usize, 0), fake.state.written);
}

test "write and flush refuse to drive an unconfigured channel" {
    var console = freshConsole();
    const byte: u8 = 'x';
    try std.testing.expectEqual(Err.not_initialized, console.write(@ptrCast(&byte), 1));
    try std.testing.expectEqual(Err.not_initialized, console.flush());
    try std.testing.expectEqual(@as(usize, 0), fake.state.flushes);
}

test "write forwards the whole slice once up" {
    var console = freshConsole();
    try std.testing.expectEqual(Err.ok, console.init(115200));
    const msg = "hello";
    try std.testing.expectEqual(Err.ok, console.write(msg.ptr, msg.len));
    try std.testing.expectEqual(@as(usize, 5), fake.state.written);
    try std.testing.expectEqual(Err.ok, console.flush());
    try std.testing.expectEqual(@as(usize, 1), fake.state.flushes);
}

test "read validates its out_len before anything else" {
    var console = freshConsole();
    var buf: [4]u8 = undefined;
    try std.testing.expectEqual(Err.invalid_arg, console.read(&buf, buf.len, null));
}

test "read zeroes out_len and accepts a zero cap with no buffer" {
    var console = freshConsole();
    var len: usize = 7;
    try std.testing.expectEqual(Err.ok, console.read(null, 0, &len));
    try std.testing.expectEqual(@as(usize, 0), len);
    try std.testing.expectEqual(Err.invalid_arg, console.read(null, 1, &len));
}

test "read refuses an unconfigured channel" {
    var console = freshConsole();
    var buf: [4]u8 = undefined;
    var len: usize = 0;
    try std.testing.expectEqual(Err.not_initialized, console.read(&buf, buf.len, &len));
}

test "read drains what is available and stops" {
    var console = freshConsole();
    try std.testing.expectEqual(Err.ok, console.init(115200));
    fake.state.rx = "ab";
    var buf: [4]u8 = undefined;
    var len: usize = 0;
    try std.testing.expectEqual(Err.ok, console.read(&buf, buf.len, &len));
    try std.testing.expectEqual(@as(usize, 2), len);
    try std.testing.expectEqualSlices(u8, "ab", buf[0..len]);
}

test "read is bounded by cap even with more bytes waiting" {
    var console = freshConsole();
    try std.testing.expectEqual(Err.ok, console.init(115200));
    fake.state.rx = "abcdef";
    var buf: [3]u8 = undefined;
    var len: usize = 0;
    try std.testing.expectEqual(Err.ok, console.read(&buf, buf.len, &len));
    try std.testing.expectEqual(@as(usize, 3), len);
    try std.testing.expectEqualSlices(u8, "abc", buf[0..len]);
}

test "console wiring pins the provisional SCI8 channel and pins" {
    const Pin = vocab.Pin;
    try std.testing.expectEqual(@as(u8, 8), Wiring.sci_channel);
    try std.testing.expectEqual(Pin.pack(13, 2), Wiring.pin_txd);
    try std.testing.expectEqual(Pin.pack(13, 3), Wiring.pin_rxd);
    try std.testing.expectEqual(Pin.pack(13, 4), Wiring.pin_rts);
    try std.testing.expectEqual(Pin.pack(13, 5), Wiring.pin_cts);
    try std.testing.expectEqual(@as(u32, 16_000_000), Wiring.min_pclka_hz);
    try std.testing.expectEqual(@as(u8, 0x04), Wiring.psel_sci_async);
    try std.testing.expectEqual(@as(u8, 3), Wiring.clock_id_pclka);
}
