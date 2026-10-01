//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the RAM-backed media-download sink.

const std = @import("std");
const Ram = @import("implementation").storage_ram.Ram;

test "bind leaves an idle adapter over the caller's bytes" {
    var backing: [8]u8 = undefined;
    const ram = Ram.bind(&backing);
    try std.testing.expectEqual(@as(usize, 8), ram.capacity);
    try std.testing.expectEqual(@as(usize, 0), ram.length);
    try std.testing.expect(!ram.active);
    try std.testing.expect(!ram.committed);
}

test "a committed transaction exposes exactly its appended bytes in order" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try ram.begin("source");
    try ram.write(&[_]u8{ 1, 2 });
    try ram.write(&[_]u8{ 3, 4, 5 });
    try ram.commit();
    try std.testing.expectEqualSlices(u8, &[_]u8{ 1, 2, 3, 4, 5 }, try ram.view());
}

test "begin refuses an empty destination label" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try std.testing.expectError(error.InvalidArg, ram.begin(""));
    try std.testing.expect(!ram.active);
}

test "begin refuses to overlap a live transaction" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try ram.begin("source");
    try std.testing.expectError(error.InvalidState, ram.begin("overlap"));
}

test "begin clears a prior committed extent" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try ram.begin("first");
    try ram.write(&[_]u8{0xAA});
    try ram.commit();
    try ram.begin("second");
    try std.testing.expectEqual(@as(usize, 0), ram.length);
    try std.testing.expect(!ram.committed);
}

test "write outside a transaction is refused" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try std.testing.expectError(error.InvalidState, ram.write(&[_]u8{1}));
}

test "an oversized fragment is refused whole and leaves the prefix intact" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try ram.begin("source");
    try ram.write(&[_]u8{ 1, 2, 3, 4, 5, 6 });
    try std.testing.expectError(error.NoMem, ram.write(&[_]u8{ 7, 8, 9 }));
    try std.testing.expectEqual(@as(usize, 6), ram.length);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 1, 2, 3, 4, 5, 6 }, backing[0..6]);
}

test "a fragment filling the remaining capacity exactly is accepted" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try ram.begin("source");
    try ram.write(&[_]u8{ 1, 2, 3, 4, 5, 6 });
    try ram.write(&[_]u8{ 7, 8 });
    try std.testing.expectEqual(@as(usize, 8), ram.length);
}

test "an empty fragment is accepted and moves nothing" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try ram.begin("source");
    try ram.write(&[_]u8{});
    try std.testing.expectEqual(@as(usize, 0), ram.length);
}

test "commit refuses an empty extent" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try ram.begin("empty");
    try std.testing.expectError(error.InvalidState, ram.commit());
    try std.testing.expect(ram.active);
}

test "commit refuses twice over" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try ram.begin("source");
    try ram.write(&[_]u8{1});
    try ram.commit();
    try std.testing.expectError(error.InvalidState, ram.commit());
}

test "abort discards the extent and permits a fresh transaction" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try ram.begin("source");
    try ram.write(&[_]u8{ 1, 2, 3 });
    try ram.abort();
    try std.testing.expectEqual(@as(usize, 0), ram.length);
    try std.testing.expectError(error.InvalidState, ram.view());
    try ram.begin("again");
    try ram.abort();
}

test "abort outside a transaction is refused" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try std.testing.expectError(error.InvalidState, ram.abort());
}

test "abort leaves the written bytes in place, only the extent goes" {
    var backing = [_]u8{0} ** 8;
    var ram = Ram.bind(&backing);
    try ram.begin("source");
    try ram.write(&[_]u8{ 0xDE, 0xAD });
    try ram.abort();
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xDE, 0xAD }, backing[0..2]);
}

test "view is refused while a transaction is live" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try ram.begin("source");
    try ram.write(&[_]u8{1});
    try std.testing.expectError(error.InvalidState, ram.view());
}

test "view is refused before anything is committed" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try std.testing.expectError(error.InvalidState, ram.view());
}

test "view hands back the backing pointer itself, not a copy" {
    var backing: [8]u8 = undefined;
    var ram = Ram.bind(&backing);
    try ram.begin("source");
    try ram.write(&[_]u8{0xA5});
    try ram.commit();
    const bytes = try ram.view();
    try std.testing.expectEqual(@as([*]const u8, &backing), bytes.ptr);
    try std.testing.expectEqual(@as(usize, 1), bytes.len);
}
