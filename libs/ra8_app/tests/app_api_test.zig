//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_app_api_t` end to end (RA8FW-294): the module's typed shims, through a
//! request entry point that calls the kernel's dispatch, to a fake backend.
//! On the target the entry point is ThreadX's module library and the trap;
//! here it is a plain call, which is all that differs.

const std = @import("std");
const api = @import("app_api");
const fake = @import("app_api_fake.zig");
const abi = api.abi;
const kernel = api.kernel;
const Capability = api.appimg.Capability;

/// What the fake entry point dispatches to. The shims put their argument
/// blocks on the stack, so the module's region here is the whole address
/// space; the refusal tests pin the region down.
var live_backend: fake.Fake = .{};
var live_caps: u32 = fake.all;

fn entry(request: u32, p1: abi.Word, p2: abi.Word, p3: abi.Word) callconv(.c) u32 {
    const caller: kernel.Caller = .{
        .app_id = "com.example.notes",
        .capabilities = live_caps,
        .owned = .{ .data = .{ .start = 1, .end = std.math.maxInt(abi.Word) } },
    };
    return kernel.dispatch(live_backend.backend(), &caller, request, p1, p2, p3);
}

const App = api.module.Api(&entry);

fn reset(caps: u32) void {
    live_backend = .{};
    live_caps = caps;
}

test "the API generation is the one the .ra8app header admits against" {
    try std.testing.expectEqual(api.appimg.Format.api_version_current, abi.version);
    reset(Capability.none);
    try std.testing.expectEqual(abi.version, App.apiVersion());
}

test "request numbers are append-only from one and never zero" {
    try std.testing.expectEqual(@as(u32, 1), @backingInt(abi.Request.api_version));
    try std.testing.expectEqual(@as(u32, 12), @backingInt(abi.Request.last));
    try std.testing.expect(!kernel.known(0));
    try std.testing.expect(kernel.known(@backingInt(abi.Request.last)));
}

test "the lifecycle shims reach the backend" {
    reset(Capability.none);
    try std.testing.expectEqual(@as(u32, 1234), try App.ticks());
    try App.sleep(25);
    try std.testing.expectEqual(@as(u32, 25), live_backend.slept);
    try App.exit(0);
    try std.testing.expectEqual(@as(?u32, 0), live_backend.exited);
}

test "a module draws into its region" {
    reset(Capability.display);
    const info = try App.screenInfo();
    try std.testing.expectEqual(fake.Fake.screen, info);

    try App.drawRect(.{ .x = 30, .y = 14, .w = 2, .h = 2 }, 0xff112233);
    try std.testing.expectEqual(abi.Rect{ .x = 30, .y = 14, .w = 2, .h = 2 }, live_backend.filled.?);
    try std.testing.expectEqual(@as(u32, 0xff112233), live_backend.colour);

    var pixels: [3 * 2 * 4]u8 = undefined;
    @memset(&pixels, 0xaa);
    try App.drawImage(.{ .x = 1, .y = 1, .w = 3, .h = 2 }, &pixels);
    try std.testing.expectEqual(pixels.len, live_backend.blitted);

    try std.testing.expectError(api.module.Error.BadArgument, App.drawRect(.{ .x = 31, .w = 2, .h = 1 }, 0));
}

test "a module's files live under its own directory and round-trip" {
    reset(Capability.storage);
    const handle = try App.fileOpen("notes/today.txt", abi.OpenFlag.write | abi.OpenFlag.create);
    try std.testing.expectEqual(fake.Fake.handle, handle);
    try std.testing.expectEqualStrings("/apps/com.example.notes/notes/today.txt", live_backend.openedPath());

    try std.testing.expectEqual(@as(u32, 5), try App.fileWrite(handle, "hello"));
    var back: [16]u8 = undefined;
    const got = try App.fileRead(handle, &back);
    try std.testing.expectEqualStrings("hello", back[0..got]);
    try App.fileClose(handle);
    try std.testing.expectError(api.module.Error.NotFound, App.fileClose(handle));

    try std.testing.expectError(api.module.Error.BadArgument, App.fileOpen("../escape", abi.OpenFlag.read));
}

test "a module without the capability gets Denied from the shim" {
    reset(Capability.display);
    try std.testing.expectError(api.module.Error.Denied, App.fileOpen("a", abi.OpenFlag.read));
    reset(Capability.storage);
    try std.testing.expectError(api.module.Error.Denied, App.screenInfo());
    try std.testing.expectError(api.module.Error.Denied, App.touchEvent());
}

test "touch events come one at a time and an empty queue is not an error" {
    reset(Capability.display);
    try std.testing.expectEqual(@as(?abi.TouchEvent, null), try App.touchEvent());
    live_backend.queued = .{ .kind = .press, .x = 4, .y = 9 };
    const event = (try App.touchEvent()).?;
    try std.testing.expectEqual(abi.TouchEvent.Kind.press, event.kind);
    try std.testing.expectEqual(@as(u16, 9), event.y);
    try std.testing.expectEqual(@as(?abi.TouchEvent, null), try App.touchEvent());
}

test "the sandbox joins only a checked app id and path" {
    var out: [api.sandbox.joined_max]u8 = undefined;
    try std.testing.expectEqualStrings("/apps/notes/a/b.txt", try api.sandbox.join(&out, "notes", "a/b.txt"));
    try std.testing.expectError(api.sandbox.Error.BadPath, api.sandbox.join(&out, "../x", "a"));
    try std.testing.expectError(api.sandbox.Error.BadPath, api.sandbox.join(&out, "a/b", "a"));
    try std.testing.expectError(api.sandbox.Error.BadPath, api.sandbox.join(&out, "", "a"));
    const longest: [api.sandbox.app_id_max]u8 = @splat('x');
    const path: [abi.path_max]u8 = @splat('y');
    try std.testing.expectEqual(api.sandbox.joined_max, (try api.sandbox.join(&out, &longest, &path)).len);
}
