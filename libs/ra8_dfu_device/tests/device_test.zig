//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The DFU device state against a scripted programmer: block staging and
//! padding, the latched error, upload clamping and the one-shot commit.

const std = @import("std");
const device = @import("device");
const usbx = @import("usbx");

const Call = struct { target: device.Slot, offset: u32, bytes: [device.block_bytes]u8, len: u32 };

const Fake = struct {
    calls: [8]Call = undefined,
    count: usize = 0,
    fail_at: ?usize = null,
    commits: u32 = 0,
    committed_len: u32 = 0,
    committed_seq: u32 = 0,
    other_seq: u32 = 6,

    pub fn image(self: *Fake, target: device.Slot, offset: u32, bytes: []const u8) u16 {
        var call = Call{ .target = target, .offset = offset, .bytes = undefined, .len = @intCast(bytes.len) };
        @memcpy(call.bytes[0..bytes.len], bytes);
        self.calls[self.count] = call;
        self.count += 1;
        if (self.fail_at) |at| if (at + 1 == self.count) return 0x405;
        return device.ok;
    }

    pub fn commit(self: *Fake, _: device.Slot, img_len: u32, seq: u32) u16 {
        self.commits += 1;
        self.committed_len = img_len;
        self.committed_seq = seq;
        return device.ok;
    }

    pub fn otherSeq(self: *Fake, _: device.Slot) u32 {
        return self.other_seq;
    }
};

test "a full block is programmed as is at block * 64" {
    var state = device.State{};
    var fake = Fake{};
    const block: [device.block_bytes]u8 = @splat(0xA5);
    state.write(&fake, 3, &block);
    try std.testing.expectEqual(@as(u32, 192), fake.calls[0].offset);
    try std.testing.expectEqual(device.block_bytes, fake.calls[0].len);
    try std.testing.expectEqual(device.Slot.b, fake.calls[0].target);
    try std.testing.expectEqual(@as(u32, 256), state.img_len);
    try std.testing.expectEqual(@as(u32, 1), state.writes);
}

test "a short final block is padded to a 32-byte page with 0xFF" {
    var state = device.State{};
    var fake = Fake{};
    state.write(&fake, 0, &[_]u8{ 1, 2, 3 });
    const call = fake.calls[0];
    try std.testing.expectEqual(@as(u32, 32), call.len);
    try std.testing.expectEqualSlices(u8, &.{ 1, 2, 3 }, call.bytes[0..3]);
    for (call.bytes[3..32]) |byte| try std.testing.expectEqual(@as(u8, 0xFF), byte);
    try std.testing.expectEqual(@as(u32, 32), state.img_len);
}

test "the first program error latches and stops counting" {
    var state = device.State{};
    var fake = Fake{ .fail_at = 1 };
    const block: [device.block_bytes]u8 = @splat(0);
    state.write(&fake, 0, &block);
    state.write(&fake, 1, &block);
    try std.testing.expectEqual(@as(u16, 0x405), state.prog_err);
    try std.testing.expect(!state.mediaOk());
    try std.testing.expectEqual(@as(u32, 1), state.writes);
    try std.testing.expectEqual(@as(u32, 64), state.img_len);
}

test "an empty block is end-of-download" {
    var state = device.State{};
    var fake = Fake{};
    state.write(&fake, 9, &.{});
    try std.testing.expect(state.manifest);
    try std.testing.expectEqual(@as(usize, 0), fake.count);
}

test "an upload is clamped to the bytes accepted" {
    const state = device.State{ .img_len = 100 };
    try std.testing.expectEqual(device.Span{ .offset = 64, .len = 36 }, state.readSpan(1, 64).?);
    try std.testing.expectEqual(device.Span{ .offset = 0, .len = 16 }, state.readSpan(0, 16).?);
    try std.testing.expectEqual(@as(?device.Span, null), state.readSpan(2, 64));
}

test "the worker commits once, after manifest, one past the other slot" {
    var state = device.State{ .prepared = true, .img_len = 128 };
    var fake = Fake{};
    try std.testing.expectEqual(device.ok, state.workerStep(&fake));
    try std.testing.expectEqual(@as(u32, 0), fake.commits);
    state.manifest = true;
    try std.testing.expectEqual(device.ok, state.workerStep(&fake));
    try std.testing.expectEqual(device.ok, state.workerStep(&fake));
    try std.testing.expectEqual(@as(u32, 1), fake.commits);
    try std.testing.expectEqual(@as(u32, 128), fake.committed_len);
    try std.testing.expectEqual(@as(u32, 7), fake.committed_seq);
    try std.testing.expect(state.committed);
}

test "the worker never commits an unprepared slot or a failed download" {
    var fake = Fake{};
    var unprepared = device.State{ .manifest = true };
    _ = unprepared.workerStep(&fake);
    var failed = device.State{ .manifest = true, .prepared = true, .prog_err = 0x405 };
    try std.testing.expectEqual(@as(u16, 0x405), failed.workerStep(&fake));
    try std.testing.expectEqual(@as(u32, 0), fake.commits);
}

test "set_target accepts only slot A or B" {
    var state = device.State{};
    state.setTarget(0);
    try std.testing.expectEqual(device.Slot.a, state.target);
    state.setTarget(2);
    try std.testing.expectEqual(device.Slot.a, state.target);
}

test "the USBX parameter block is ten words" {
    try std.testing.expectEqual(10 * @sizeOf(usize), @sizeOf(usbx.DfuParameter));
    try std.testing.expectEqual(8 * @sizeOf(usize), @offsetOf(usbx.DfuParameter, "framework"));
}
