//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const abi = @import("adapter.zig");

comptime {
    _ = @import("adapter.zig");
}

test "Zig consumes Rust fixed-layout operation through C ABI" {
    var config = abi.Config{
        .value = 7,
        .factor = 3,
        .enabled = 1,
        .reserved0 = 0,
    };
    var result: u32 = 0xA5A5A5A5;
    try std.testing.expectEqual(abi.ok, abi.apply(&config, &result));
    try std.testing.expectEqual(@as(u32, 21), result);

    config.enabled = 0;
    try std.testing.expectEqual(abi.ok, abi.apply(&config, &result));
    try std.testing.expectEqual(@as(u32, 7), result);

    result = 0xA5A5A5A5;
    try std.testing.expectEqual(abi.null_pointer, abi.apply(null, &result));
    try std.testing.expectEqual(@as(u32, 0xA5A5A5A5), result);
    try std.testing.expectEqual(abi.null_pointer, abi.apply(&config, null));

    config.enabled = 2;
    try std.testing.expectEqual(abi.invalid_argument, abi.apply(&config, &result));
    try std.testing.expectEqual(@as(u32, 0xA5A5A5A5), result);
    config.enabled = 1;
    config.reserved0 = 1;
    try std.testing.expectEqual(abi.invalid_argument, abi.apply(&config, &result));
    try std.testing.expectEqual(@as(u32, 0xA5A5A5A5), result);
    config.reserved0 = 0;
    config.value = std.math.maxInt(u32);
    config.factor = 2;
    try std.testing.expectEqual(abi.invalid_size, abi.apply(&config, &result));
    try std.testing.expectEqual(@as(u32, 0xA5A5A5A5), result);
}

test "Zig preserves Rust handle ownership contract" {
    try std.testing.expectEqual(abi.null_pointer, abi.create(null));
    var handle: ?*abi.Handle = @ptrFromInt(1);
    abi.failNextAllocation();
    try std.testing.expectEqual(abi.no_memory, abi.create(&handle));
    try std.testing.expectEqual(@as(?*abi.Handle, @ptrFromInt(1)), handle);

    try std.testing.expectEqual(abi.ok, abi.create(&handle));
    try std.testing.expect(handle != null);
    var second: ?*abi.Handle = @ptrFromInt(2);
    try std.testing.expectEqual(abi.no_memory, abi.create(&second));
    try std.testing.expectEqual(@as(?*abi.Handle, @ptrFromInt(2)), second);
    try std.testing.expectEqual(abi.invalid_argument, abi.destroy(&second));
    try std.testing.expectEqual(@as(?*abi.Handle, @ptrFromInt(2)), second);
    try std.testing.expectEqual(abi.null_pointer, abi.destroy(null));

    try std.testing.expectEqual(abi.ok, abi.destroy(&handle));
    try std.testing.expectEqual(@as(?*abi.Handle, null), handle);
    try std.testing.expectEqual(abi.ok, abi.destroy(&handle));
}
