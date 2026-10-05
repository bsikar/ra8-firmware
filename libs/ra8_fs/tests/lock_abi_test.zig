//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_fs_set_lock and the priv_lock_acquire/release forwarders (RA8FW-724).

const std = @import("std");
const fs = @import("ra8_fs");
const lock = fs.lock;

var acquires: u32 = 0;
var releases: u32 = 0;
var last_ctx: ?*anyopaque = null;
var cookie: u32 = 0;

fn fakeAcquire(ctx: ?*anyopaque) callconv(.C) void {
    acquires += 1;
    last_ctx = ctx;
}

fn fakeRelease(ctx: ?*anyopaque) callconv(.C) void {
    releases += 1;
    last_ctx = ctx;
}

fn reset() !void {
    try std.testing.expectEqual(lock.ok, lock.ra8_fs_set_lock(null));
    acquires = 0;
    releases = 0;
    last_ctx = null;
}

fn binding() lock.Lock {
    return .{ .acquire = &fakeAcquire, .release = &fakeRelease, .ctx = &cookie };
}

test "acquire and release are no-ops with no lock installed" {
    try reset();
    lock.priv_lock_acquire();
    lock.priv_lock_release();
    try std.testing.expectEqual(@as(u32, 0), acquires + releases);
}

test "an installed lock forwards both calls with its ctx" {
    try reset();
    const b = binding();
    try std.testing.expectEqual(lock.ok, lock.ra8_fs_set_lock(&b));
    lock.priv_lock_acquire();
    try std.testing.expectEqual(@as(u32, 1), acquires);
    try std.testing.expectEqual(@as(?*anyopaque, &cookie), last_ctx);
    lock.priv_lock_release();
    try std.testing.expectEqual(@as(u32, 1), releases);
}

test "the binding is copied, so the caller's struct need not outlive the call" {
    try reset();
    var b = binding();
    try std.testing.expectEqual(lock.ok, lock.ra8_fs_set_lock(&b));
    b.acquire = null;
    lock.priv_lock_acquire();
    try std.testing.expectEqual(@as(u32, 1), acquires);
}

test "a null acquire or release is rejected and keeps the previous lock" {
    try reset();
    const good = binding();
    try std.testing.expectEqual(lock.ok, lock.ra8_fs_set_lock(&good));
    var bad = binding();
    bad.acquire = null;
    try std.testing.expectEqual(lock.err_invalid_arg, lock.ra8_fs_set_lock(&bad));
    bad = binding();
    bad.release = null;
    try std.testing.expectEqual(lock.err_invalid_arg, lock.ra8_fs_set_lock(&bad));
    lock.priv_lock_acquire();
    try std.testing.expectEqual(@as(u32, 1), acquires);
}

test "a null binding uninstalls the lock" {
    try reset();
    const b = binding();
    try std.testing.expectEqual(lock.ok, lock.ra8_fs_set_lock(&b));
    try std.testing.expectEqual(lock.ok, lock.ra8_fs_set_lock(null));
    lock.priv_lock_acquire();
    lock.priv_lock_release();
    try std.testing.expectEqual(@as(u32, 0), acquires + releases);
}
