//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const chain = @import("chain_adapter.zig");

comptime {
    _ = @import("chain_adapter.zig");
}

test "chain rejects null apply outputs before Rust" {
    const result: u16 = @intCast(chain.ra8_abi_chain_apply(null, null));
    try std.testing.expectEqual(@as(u16, 0x504), result);
}

test "chain rejects null create outputs before Rust" {
    const result: u16 = @intCast(chain.ra8_abi_chain_create(0x5A494701, null));
    try std.testing.expectEqual(@as(u16, 0x504), result);
}
