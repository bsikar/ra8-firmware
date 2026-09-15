//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! Dedicated native Zig tests for the fixture's private implementation.

const std = @import("std");
const implementation = @import("implementation");
const apply = implementation.apply;
const bytesCreate = implementation.bytesCreate;
const bytesRelease = implementation.bytesRelease;
const create = implementation.create;
const destroy = implementation.destroy;
const failNextAllocation = implementation.failNextAllocation;

test "private implementation maps expected failures" {
    try std.testing.expectError(
        error.InvalidBoolean,
        apply(.{ .value = 1, .factor = 1, .enabled = 2, .reserved0 = 0 }),
    );
    try std.testing.expectError(
        error.Overflow,
        apply(.{
            .value = std.math.maxInt(u32),
            .factor = 2,
            .enabled = 1,
            .reserved0 = 0,
        }),
    );
}

test "bounded handle allocation fails once and recovers" {
    failNextAllocation();
    try std.testing.expectError(error.NoMemory, create());
    const handle = try create();
    try std.testing.expectError(error.NoMemory, create());
    try std.testing.expect(destroy(handle));
    const recovered = try create();
    try std.testing.expect(destroy(recovered));
}

test "owned bytes preserve lifecycle after injected failure" {
    const handle = try create();
    failNextAllocation();
    try std.testing.expectError(error.NoMemory, bytesCreate(handle, "abc"));
    const bytes = try bytesCreate(handle, "abc");
    var wrong = [_]u8{0};
    try std.testing.expectError(error.InvalidPointer, bytesRelease(&wrong));
    try bytesRelease(bytes.ptr);
    try std.testing.expectError(error.InvalidState, bytesRelease(bytes.ptr));
    try std.testing.expect(destroy(handle));
}
