//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Admission tests for the `.ra8app` gate: the refusal order, the unsigned
//! rule, and what the gate does with each backend verdict. The backend here
//! is a spy, so every test can say whether the signature was spent at all.

const std = @import("std");
const gate = @import("appimg_verify");
const appimg = gate.image;

const header_bytes = @sizeOf(appimg.Header);
const key: [gate.pubkey_bytes]u8 = @splat(0x11);

/// Backend stand-in: records whether it was called, answers as told.
const Spy = struct {
    answer: gate.Verdict = .good,
    calls: u32 = 0,
    saw_head_len: u32 = 0,
    saw_tail_len: u32 = 0,

    fn verify(
        ctx: ?*anyopaque,
        msg: gate.Message,
        signature: *const [appimg.Width.signature]u8,
        public_key: *const [gate.pubkey_bytes]u8,
    ) gate.Verdict {
        _ = signature;
        _ = public_key;
        const self: *Spy = @ptrCast(@alignCast(ctx.?));
        self.calls += 1;
        self.saw_head_len = @intCast(msg.head.len);
        self.saw_tail_len = @intCast(msg.tail.len);
        return self.answer;
    }

    fn backend(self: *Spy, granted: u32) gate.Backend {
        return .{
            .verify = Spy.verify,
            .ctx = self,
            .public_key = &key,
            .granted = granted,
        };
    }
};

fn goodHeader() appimg.Header {
    var h = std.mem.zeroes(appimg.Header);
    h.magic = appimg.Format.magic;
    h.format_version = appimg.Format.version;
    h.code_size = 256;
    h.data_size = 64;
    h.stack_size = appimg.Format.stack_min;
    h.min_api_version = appimg.Format.api_version_current;
    h.capabilities = appimg.Capability.display;
    @memcpy(h.app_id[0..11], "com.ex.rdr\x00");
    @memset(&h.signature, 0xAB);
    return h;
}

fn imageOf(allocator: std.mem.Allocator, h: appimg.Header) ![]u8 {
    const buf = try allocator.alloc(u8, header_bytes + h.payloadLen());
    @memset(buf, 0);
    @memcpy(buf[0..header_bytes], std.mem.asBytes(&h));
    return buf;
}

test "the signed message is the header prefix and then the payload" {
    const h = goodHeader();
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);

    const msg = try gate.signedMessage(h, image);
    try std.testing.expectEqual(@as(usize, 96), msg.head.len);
    try std.testing.expectEqual(h.payloadLen(), msg.tail.len);
    try std.testing.expectEqual(image.ptr, msg.head.ptr);
    try std.testing.expectEqual(@intFromPtr(image.ptr) + header_bytes, @intFromPtr(msg.tail.ptr));
}

test "the signed message refuses an image too short for its payload" {
    const h = goodHeader();
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);
    try std.testing.expectError(
        appimg.Error.ShortImage,
        gate.signedMessage(h, image[0 .. image.len - 1]),
    );
}

test "a good signature over a granted image admits it" {
    var spy = Spy{ .answer = .good };
    const h = goodHeader();
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);

    const admitted = try gate.verify(spy.backend(appimg.Capability.display), image);
    try std.testing.expectEqual(h.code_size, admitted.code_size);
    try std.testing.expectEqual(@as(u32, 1), spy.calls);
}

test "the backend sees both signed runs, at their real lengths" {
    var spy = Spy{ .answer = .good };
    const h = goodHeader();
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);

    _ = try gate.verify(spy.backend(appimg.Capability.display), image);
    try std.testing.expectEqual(@as(u32, 96), spy.saw_head_len);
    try std.testing.expectEqual(@as(u32, 320), spy.saw_tail_len);
}

test "a malformed container is refused before the backend is reached" {
    var spy = Spy{ .answer = .good };
    var h = goodHeader();
    h.magic = 0;
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);

    try std.testing.expectError(
        appimg.Error.Validation,
        gate.verify(spy.backend(appimg.Capability.known), image),
    );
    try std.testing.expectEqual(@as(u32, 0), spy.calls);
}

test "a withheld capability is refused before a signature is spent" {
    var spy = Spy{ .answer = .good };
    var h = goodHeader();
    h.capabilities = appimg.Capability.display | appimg.Capability.network;
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);

    try std.testing.expectError(
        gate.Error.AccessDenied,
        gate.verify(spy.backend(appimg.Capability.display), image),
    );
    try std.testing.expectEqual(@as(u32, 0), spy.calls);
}

test "a grant carrying an undefined bit refuses as malformed, not access-denied" {
    var spy = Spy{ .answer = .good };
    const h = goodHeader();
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);

    try std.testing.expectError(
        appimg.Error.Validation,
        gate.verify(spy.backend(appimg.Capability.known | 0x40), image),
    );
    try std.testing.expectEqual(@as(u32, 0), spy.calls);
}

test "an unsigned image is refused without consulting the backend" {
    var spy = Spy{ .answer = .good };
    var h = goodHeader();
    @memset(&h.signature, 0);
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);

    try std.testing.expectError(
        appimg.Error.Validation,
        gate.verify(spy.backend(appimg.Capability.display), image),
    );
    try std.testing.expectEqual(@as(u32, 0), spy.calls);
}

test "a single non-zero signature byte is enough to reach the backend" {
    var spy = Spy{ .answer = .bad };
    var h = goodHeader();
    @memset(&h.signature, 0);
    h.signature[appimg.Width.signature - 1] = 1;
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);

    try std.testing.expectError(
        gate.Error.BadSignature,
        gate.verify(spy.backend(appimg.Capability.display), image),
    );
    try std.testing.expectEqual(@as(u32, 1), spy.calls);
}

test "a bad verdict refuses the image" {
    var spy = Spy{ .answer = .bad };
    const h = goodHeader();
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);

    try std.testing.expectError(
        gate.Error.BadSignature,
        gate.verify(spy.backend(appimg.Capability.display), image),
    );
}

test "an unsupported backend is reported as such, not as a bad signature" {
    var spy = Spy{ .answer = .unsupported };
    const h = goodHeader();
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);

    try std.testing.expectError(
        appimg.Error.Unsupported,
        gate.verify(spy.backend(appimg.Capability.display), image),
    );
}

test "an app declaring nothing is admitted under an empty grant" {
    var spy = Spy{ .answer = .good };
    var h = goodHeader();
    h.capabilities = appimg.Capability.none;
    const image = try imageOf(std.testing.allocator, h);
    defer std.testing.allocator.free(image);

    _ = try gate.verify(spy.backend(appimg.Capability.none), image);
    try std.testing.expectEqual(@as(u32, 1), spy.calls);
}
