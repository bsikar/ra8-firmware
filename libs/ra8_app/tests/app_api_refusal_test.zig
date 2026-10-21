//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_app_api_t` refusals (RA8FW-294): every one answers its status and
//! never reaches the backend.

const std = @import("std");
const api = @import("app_api");
const fake = @import("app_api_fake.zig");
const abi = api.abi;
const kernel = api.kernel;
const Capability = api.appimg.Capability;

fn req(r: abi.Request) u32 {
    return @backingInt(r);
}

fn refused(backend: *fake.Fake, want: abi.Status, word: u32) !void {
    try std.testing.expectEqual(want, fake.status(word));
    try std.testing.expectEqual(@as(u32, 0), backend.calls);
}

test "a request number this generation does not serve is refused" {
    var arena: fake.Arena = .{};
    var backend: fake.Fake = .{};
    const caller = fake.caller(&arena, fake.all);
    for ([_]u32{ 0, req(abi.Request.last) + 1, 0xffff, 0xffff_ffff }) |number| {
        try refused(&backend, .unknown_request, kernel.dispatch(backend.backend(), &caller, number, 0, 0, 0));
    }
}

test "a call whose capability the manifest does not hold is denied before its pointers are read" {
    var arena: fake.Arena = .{};
    var backend: fake.Fake = .{};
    const display_only = fake.caller(&arena, Capability.display);
    const storage_only = fake.caller(&arena, Capability.storage);
    const nothing = fake.caller(&arena, Capability.none);
    // Pointer words of zero: a capability refusal must come first.
    for ([_]abi.Request{ .file_open, .file_read, .file_write, .file_close }) |r| {
        try refused(&backend, .denied, kernel.dispatch(backend.backend(), &display_only, req(r), 0, 0, 0));
        try refused(&backend, .denied, kernel.dispatch(backend.backend(), &nothing, req(r), 0, 0, 0));
    }
    for ([_]abi.Request{ .screen_info, .draw_rect, .draw_image, .touch_event }) |r| {
        try refused(&backend, .denied, kernel.dispatch(backend.backend(), &storage_only, req(r), 0, 0, 0));
        try refused(&backend, .denied, kernel.dispatch(backend.backend(), &nothing, req(r), 0, 0, 0));
    }
}

test "the lifecycle calls need no capability" {
    var arena: fake.Arena = .{};
    var backend: fake.Fake = .{};
    const nothing = fake.caller(&arena, Capability.none);
    try std.testing.expectEqual(abi.version, kernel.dispatch(backend.backend(), &nothing, req(.api_version), 0, 0, 0));
    try std.testing.expectEqual(abi.Status.ok, fake.status(kernel.dispatch(backend.backend(), &nothing, req(.sleep), 5, 0, 0)));
    try std.testing.expectEqual(abi.Status.ok, fake.status(kernel.dispatch(backend.backend(), &nothing, req(.ticks), arena.word(0), 0, 0)));
    try std.testing.expectEqual(abi.Status.ok, fake.status(kernel.dispatch(backend.backend(), &nothing, req(.exit), 3, 0, 0)));
    try std.testing.expectEqual(@as(?u32, 3), backend.exited);
}

test "a pointer outside the module, straddling its end, null, wrapping or misaligned is refused" {
    var arena: fake.Arena = .{};
    var outside: fake.Arena = .{};
    var backend: fake.Fake = .{};
    const caller = fake.caller(&arena, fake.all);
    const end = arena.word(0) + arena.bytes.len;
    const words = [_]abi.Word{
        0, // null
        outside.word(0), // another buffer entirely
        end - 4, // the last 4 bytes: an 8-byte Rect runs past the end
        end, // one past the end
        std.math.maxInt(abi.Word) - 3, // the range wraps
        arena.word(1), // not aligned for the struct
    };
    for (words) |w| {
        try refused(&backend, .bad_pointer, kernel.dispatch(backend.backend(), &caller, req(.draw_rect), w, 0, 0));
        try refused(&backend, .bad_pointer, kernel.dispatch(backend.backend(), &caller, req(.screen_info), w, 0, 0));
        try refused(&backend, .bad_pointer, kernel.dispatch(backend.backend(), &caller, req(.file_open), w, 0, 0));
    }
    // A 4-byte out word: its last legal home is `end - 4`, so straddle at `end - 2`.
    for ([_]abi.Word{ 0, outside.word(0), end - 2, end, arena.word(2) }) |w| {
        try refused(&backend, .bad_pointer, kernel.dispatch(backend.backend(), &caller, req(.ticks), w, 0, 0));
    }
}

test "a buffer an argument block names outside the module is refused" {
    var arena: fake.Arena = .{};
    var outside: fake.Arena = .{};
    var backend: fake.Fake = .{};
    const caller = fake.caller(&arena, fake.all);

    arena.at(abi.FileIo, 0).* = .{ .handle = fake.Fake.handle, .buffer = outside.word(0), .len = 8 };
    try refused(&backend, .bad_pointer, kernel.dispatch(backend.backend(), &caller, req(.file_read), arena.word(0), 0, 0));
    try refused(&backend, .bad_pointer, kernel.dispatch(backend.backend(), &caller, req(.file_write), arena.word(0), 0, 0));

    // In range at the start, past the end by one byte.
    arena.at(abi.FileIo, 0).* = .{ .handle = fake.Fake.handle, .buffer = arena.word(500), .len = 13 };
    try refused(&backend, .bad_pointer, kernel.dispatch(backend.backend(), &caller, req(.file_read), arena.word(0), 0, 0));

    arena.at(abi.FileOpen, 0).* = .{ .path = outside.word(0), .path_len = 4, .flags = abi.OpenFlag.read };
    try refused(&backend, .bad_pointer, kernel.dispatch(backend.backend(), &caller, req(.file_open), arena.word(0), 0, 0));

    arena.at(abi.Rect, 0).* = .{ .w = 1, .h = 1 };
    try refused(&backend, .bad_pointer, kernel.dispatch(backend.backend(), &caller, req(.draw_image), arena.word(0), outside.word(0), 4));
}

test "a rectangle off the region, an empty one, or pixels of the wrong size are refused" {
    var arena: fake.Arena = .{};
    var backend: fake.Fake = .{};
    const caller = fake.caller(&arena, fake.all);
    const rects = [_]abi.Rect{
        .{ .x = 0, .y = 0, .w = 33, .h = 1 },
        .{ .x = 31, .y = 0, .w = 2, .h = 1 },
        .{ .x = 0, .y = 15, .w = 1, .h = 2 },
        .{ .x = 0xffff, .y = 0, .w = 0xffff, .h = 1 },
        .{ .x = 0, .y = 0, .w = 0, .h = 4 },
    };
    for (rects) |rect| {
        arena.at(abi.Rect, 0).* = rect;
        try refused(&backend, .bad_argument, kernel.dispatch(backend.backend(), &caller, req(.draw_rect), arena.word(0), 0xff00ff00, 0));
    }
    arena.at(abi.Rect, 0).* = .{ .w = 2, .h = 2 };
    // 2 x 2 at 4 bytes a pixel is 16 bytes; 12 and 20 are refused.
    for ([_]abi.Word{ 12, 20 }) |len| {
        try refused(&backend, .bad_argument, kernel.dispatch(backend.backend(), &caller, req(.draw_image), arena.word(0), arena.word(64), len));
    }
}

test "open flags, path lengths and paths that leave the module's directory are refused" {
    var arena: fake.Arena = .{};
    var backend: fake.Fake = .{};
    const caller = fake.caller(&arena, fake.all);
    const paths = [_][]const u8{ "../other/x", "/etc/passwd", "a//b", "./a", "a/", "a\\b", "c:x", "a b", "notes/..", "" };
    for (paths) |path| {
        @memcpy(arena.bytes[128..][0..path.len], path);
        arena.at(abi.FileOpen, 0).* = .{ .path = arena.word(128), .path_len = @intCast(path.len), .flags = abi.OpenFlag.read };
        try refused(&backend, .bad_argument, kernel.dispatch(backend.backend(), &caller, req(.file_open), arena.word(0), 0, 0));
    }
    @memcpy(arena.bytes[128..][0..5], "a.txt");
    for ([_]u32{ 0, abi.OpenFlag.create, 0x8 | abi.OpenFlag.read }) |flags| {
        arena.at(abi.FileOpen, 0).* = .{ .path = arena.word(128), .path_len = 5, .flags = flags };
        try refused(&backend, .bad_argument, kernel.dispatch(backend.backend(), &caller, req(.file_open), arena.word(0), 0, 0));
    }
    arena.at(abi.FileOpen, 0).* = .{ .path = arena.word(128), .path_len = abi.path_max + 1, .flags = abi.OpenFlag.read };
    try refused(&backend, .bad_argument, kernel.dispatch(backend.backend(), &caller, req(.file_open), arena.word(0), 0, 0));
}

test "a word too wide for its argument is refused" {
    if (@sizeOf(abi.Word) <= 4) return error.SkipZigTest;
    var arena: fake.Arena = .{};
    var backend: fake.Fake = .{};
    const caller = fake.caller(&arena, fake.all);
    const wide: abi.Word = @as(abi.Word, std.math.maxInt(u32)) + 1;
    try refused(&backend, .bad_argument, kernel.dispatch(backend.backend(), &caller, req(.exit), wide, 0, 0));
    try refused(&backend, .bad_argument, kernel.dispatch(backend.backend(), &caller, req(.sleep), wide, 0, 0));
    try refused(&backend, .bad_argument, kernel.dispatch(backend.backend(), &caller, req(.file_close), wide, 0, 0));
}
