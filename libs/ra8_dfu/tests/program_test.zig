//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the slot geometry and the erase-then-program page loop. The loop
//! runs against a recording backend, so the page sequencing is checked
//! without an MRAM controller.

const std = @import("std");
const image = @import("image");
const program = @import("program");
const slot = @import("slot");

/// Records every page the loop drives, in order.
const Recorder = struct {
    const Write = struct { addr: u32, erased: bool, byte: u8, len: usize };

    writes: *std.ArrayList(Write),
    fail_on: ?usize = null,
    seen: *usize,

    pub fn programPage(self: Recorder, addr: u32, erased: []const u8, body: []const u8) error{Flash}!void {
        if (self.fail_on) |at| {
            if (self.seen.* == at) {
                self.seen.* += 1;
                return error.Flash;
            }
        }
        self.seen.* += 1;
        self.writes.append(.{
            .addr = addr,
            .erased = true,
            .byte = erased[0],
            .len = erased.len,
        }) catch unreachable;
        self.writes.append(.{
            .addr = addr,
            .erased = false,
            .byte = body[0],
            .len = body.len,
        }) catch unreachable;
    }
};

fn recorder(writes: *std.ArrayList(Recorder.Write), seen: *usize, fail_on: ?usize) Recorder {
    return .{ .writes = writes, .seen = seen, .fail_on = fail_on };
}

test "slot bases are the documented MRAM addresses" {
    try std.testing.expectEqual(@as(u32, 0x0202_0000), program.base(.a));
    try std.testing.expectEqual(@as(u32, 0x0209_0000), program.base(.b));
    try std.testing.expectEqual(@as(u32, 0), program.base(.none));
}

test "the other slot of anything that is not A is A" {
    try std.testing.expectEqual(slot.Slot.b, program.other(.a));
    try std.testing.expectEqual(slot.Slot.a, program.other(.b));
    try std.testing.expectEqual(slot.Slot.a, program.other(.none));
}

test "a body write must be page aligned at both ends" {
    const page = image.layout.page_size;
    try std.testing.expect(program.bodyWriteValid(0, page));
    try std.testing.expect(program.bodyWriteValid(page, page * 4));
    try std.testing.expect(!program.bodyWriteValid(0, 0));
    try std.testing.expect(!program.bodyWriteValid(0, page - 1));
    try std.testing.expect(!program.bodyWriteValid(1, page));
}

test "a body write may not run past the image area" {
    const page = image.layout.page_size;
    try std.testing.expect(program.bodyWriteValid(image.layout.img_max - page, page));
    try std.testing.expect(!program.bodyWriteValid(image.layout.img_max, page));
    try std.testing.expect(!program.bodyWriteValid(image.layout.img_max - page, page * 2));
}

test "every page is erased to all ones before its body goes down" {
    var writes = std.ArrayList(Recorder.Write).init(std.testing.allocator);
    defer writes.deinit();
    var seen: usize = 0;

    const page = image.layout.page_size;
    const body = [_]u8{0xA5} ** (page * 3);
    try program.writePages(recorder(&writes, &seen, null), 0x0202_0000, &body);

    try std.testing.expectEqual(@as(usize, 6), writes.items.len);
    var i: usize = 0;
    while (i < 3) : (i += 1) {
        const addr = 0x0202_0000 + @as(u32, @intCast(i)) * page;
        const erase = writes.items[i * 2];
        const write = writes.items[i * 2 + 1];
        try std.testing.expect(erase.erased);
        try std.testing.expectEqual(addr, erase.addr);
        try std.testing.expectEqual(program.erased_byte, erase.byte);
        try std.testing.expectEqual(@as(usize, page), erase.len);
        try std.testing.expect(!write.erased);
        try std.testing.expectEqual(addr, write.addr);
        try std.testing.expectEqual(@as(u8, 0xA5), write.byte);
    }
}

test "a short tail is written as one partial page" {
    var writes = std.ArrayList(Recorder.Write).init(std.testing.allocator);
    defer writes.deinit();
    var seen: usize = 0;

    const page = image.layout.page_size;
    const body = [_]u8{0x11} ** (page + 8);
    try program.writePages(recorder(&writes, &seen, null), 0, &body);

    try std.testing.expectEqual(@as(usize, 4), writes.items.len);
    try std.testing.expectEqual(@as(usize, page), writes.items[1].len);
    try std.testing.expectEqual(@as(u32, page), writes.items[2].addr);
    try std.testing.expectEqual(@as(usize, 8), writes.items[3].len);
}

test "the loop stops at the first failing page" {
    var writes = std.ArrayList(Recorder.Write).init(std.testing.allocator);
    defer writes.deinit();
    var seen: usize = 0;

    const page = image.layout.page_size;
    const body = [_]u8{0x22} ** (page * 4);
    try std.testing.expectError(
        error.Flash,
        program.writePages(recorder(&writes, &seen, 2), 0, &body),
    );
    // Two pages committed, the third refused, the fourth never attempted.
    try std.testing.expectEqual(@as(usize, 4), writes.items.len);
    try std.testing.expectEqual(@as(usize, 3), seen);
}
