//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! Dedicated native Zig tests for the exported C ABI membrane.

const std = @import("std");
const abi = @import("abi");
const AbiError = abi.AbiError;
const AbiHandle = abi.AbiHandle;
const ra8_abi_fixture_copy = abi.ra8_abi_fixture_copy;
const ra8_abi_fixture_create = abi.ra8_abi_fixture_create;
const ra8_abi_fixture_destroy = abi.ra8_abi_fixture_destroy;

test "Zig consumer exercises the exported lifecycle ABI" {
    var handle: ?*AbiHandle = null;
    try std.testing.expectEqual(AbiError.ok, ra8_abi_fixture_create(&handle));

    var output = [_]u8{0} ** 3;
    var output_len: u32 = 99;
    try std.testing.expectEqual(
        AbiError.ok,
        ra8_abi_fixture_copy(handle, "zig", 3, &output, output.len, &output_len),
    );
    try std.testing.expectEqualStrings("zig", &output);
    try std.testing.expectEqual(@as(u32, 3), output_len);
    try std.testing.expectEqual(AbiError.ok, ra8_abi_fixture_destroy(&handle));
    try std.testing.expectEqual(@as(?*AbiHandle, null), handle);
}
