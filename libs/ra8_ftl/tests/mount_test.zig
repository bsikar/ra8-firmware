//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the mount lifecycle's decisions: the config rules, the split
//! of a device into an FTL span and a reserved tail, the blank-tail scan, and
//! the self-consistency a mounted handle has to keep.

const std = @import("std");
const implementation = @import("implementation");
const core = implementation.core;
const mount = implementation.mount;

fn goodCfg() implementation.CfgView {
    return .{
        .has_raw = true,
        .has_map = true,
        .has_pblocks = true,
        .has_scratch = true,
        .has_checkpoint = true,
        .checkpoint_bytes = 512,
        .logical_blocks = 8,
        .reserved_tail_blocks = 1,
    };
}

fn caps(block_count: u32, block_bytes: u16) core.Caps {
    return .{
        .block_count = block_count,
        .erase_unit_blocks = 1,
        .program_size_bytes = 512,
        .logical_block_bytes = block_bytes,
        .erase_value = 0xFF,
        .must_erase_before_write = true,
        .read_only = false,
    };
}

fn mountedHandle() implementation.HandleView {
    return .{
        .has_raw = true,
        .has_checkpoint = true,
        .ck_bytes = 512,
        .physical_blocks = 15,
        .reserved_lba = 15,
        .reserved_blocks = 1,
    };
}

test "a good cfg is accepted" {
    try std.testing.expectEqual(core.ok, implementation.validateCfg(goodCfg()));
}

test "each null storage pointer is refused on its own" {
    const fields = [_][]const u8{ "has_raw", "has_map", "has_pblocks", "has_scratch", "has_checkpoint" };
    inline for (fields) |field| {
        var cfg = goodCfg();
        @field(cfg, field) = false;
        try std.testing.expectEqual(core.err_null_ptr, implementation.validateCfg(cfg));
    }
}

test "zero logical blocks is a sizing fault, not a null one" {
    var cfg = goodCfg();
    cfg.logical_blocks = 0;
    try std.testing.expectEqual(core.err_invalid_size, implementation.validateCfg(cfg));
}

test "a tail below the floor is refused" {
    var cfg = goodCfg();
    cfg.reserved_tail_blocks = mount.reserved_tail_min - 1;
    try std.testing.expectEqual(core.err_invalid_size, implementation.validateCfg(cfg));
    cfg.reserved_tail_blocks = mount.reserved_tail_min;
    try std.testing.expectEqual(core.ok, implementation.validateCfg(cfg));
}

test "zero staging capacity is refused" {
    var cfg = goodCfg();
    cfg.checkpoint_bytes = 0;
    try std.testing.expectEqual(core.err_invalid_size, implementation.validateCfg(cfg));
}

test "the split puts the tail at the top and the FTL below it" {
    const split = switch (implementation.splitFor(caps(16, 512), goodCfg())) {
        .value => |v| v,
        .fault => return error.TestUnexpectedResult,
    };
    try std.testing.expectEqual(@as(u32, 15), split.physical_blocks);
    try std.testing.expectEqual(@as(u32, 512), split.tail_bytes);
}

test "a device no larger than its tail cannot host the FTL" {
    switch (implementation.splitFor(caps(1, 512), goodCfg())) {
        .fault => |f| try std.testing.expectEqual(core.err_invalid_arg, f),
        .value => return error.TestUnexpectedResult,
    }
}

test "the span must leave at least one spare block above the logical count" {
    var cfg = goodCfg();
    cfg.logical_blocks = 15;
    // 16 blocks less a 1-block tail is exactly 15, so there is no spare.
    switch (implementation.splitFor(caps(16, 512), cfg)) {
        .fault => |f| try std.testing.expectEqual(core.err_invalid_arg, f),
        .value => return error.TestUnexpectedResult,
    }
    cfg.logical_blocks = 15 - core.min_spare;
    try std.testing.expect(implementation.splitFor(caps(16, 512), cfg) == .value);
}

test "a staging buffer smaller than the tail is refused" {
    var cfg = goodCfg();
    cfg.checkpoint_bytes = 511;
    switch (implementation.splitFor(caps(16, 512), cfg)) {
        .fault => |f| try std.testing.expectEqual(core.err_invalid_size, f),
        .value => return error.TestUnexpectedResult,
    }
}

test "tail bytes are computed wide, so a huge tail cannot wrap into range" {
    // 0x800000 blocks of 512 B is 4 GiB exactly, which no u32 byte count can
    // cover; in 32-bit arithmetic it would wrap to 0 and look satisfiable.
    try std.testing.expectEqual(@as(u64, 0x1_0000_0000), implementation.tailBytes(0x80_0000, 512));
    var cfg = goodCfg();
    cfg.reserved_tail_blocks = 0x80_0000;
    cfg.checkpoint_bytes = 0xFFFF_FFFF;
    switch (implementation.splitFor(caps(0x90_0000, 512), cfg)) {
        .fault => |f| try std.testing.expectEqual(core.err_invalid_size, f),
        .value => return error.TestUnexpectedResult,
    }
}

test "a blank tail reads as erased" {
    const blank = [_]u8{0xFF} ** 8;
    try std.testing.expect(implementation.allErased(&blank, 0xFF));
    try std.testing.expect(!implementation.allErased(&blank, 0x00));
}

test "one programmed byte anywhere means the tail is not blank" {
    var buf = [_]u8{0xFF} ** 8;
    buf[7] = 0xFE;
    try std.testing.expect(!implementation.allErased(&buf, 0xFF));
    try std.testing.expect(implementation.allErased(buf[0..7], 0xFF));
}

test "an empty span is vacuously erased" {
    const empty: []const u8 = &.{};
    try std.testing.expect(implementation.allErased(empty, 0xFF));
}

test "a mounted handle passes its own shape check" {
    try std.testing.expectEqual(core.ok, implementation.mountedShape(mountedHandle()));
}

test "an unbound device is not initialized, an unmounted one is invalid_state" {
    var h = mountedHandle();
    h.has_raw = false;
    try std.testing.expectEqual(core.err_not_initialized, implementation.mountedShape(h));

    var never = mountedHandle();
    never.has_checkpoint = false;
    try std.testing.expectEqual(core.err_invalid_state, implementation.mountedShape(never));

    var zero = mountedHandle();
    zero.reserved_blocks = 0;
    try std.testing.expectEqual(core.err_invalid_state, implementation.mountedShape(zero));
}

test "a handle re-bound with a different geometry is caught" {
    // ra8_ftl_init over a different span leaves reserved_lba pointing into it.
    var rebound = mountedHandle();
    rebound.physical_blocks = 20;
    try std.testing.expectEqual(core.err_invalid_state, implementation.mountedShape(rebound));
}

test "the recorded tail must still lie inside the device" {
    const h = mountedHandle();
    switch (implementation.mountedAgainstDevice(h, caps(16, 512))) {
        .value => |tail| try std.testing.expectEqual(@as(u32, 512), tail),
        .fault => return error.TestUnexpectedResult,
    }
    switch (implementation.mountedAgainstDevice(h, caps(15, 512))) {
        .fault => |f| try std.testing.expectEqual(core.err_invalid_state, f),
        .value => return error.TestUnexpectedResult,
    }
}

test "a staging buffer that no longer covers the tail is refused" {
    var h = mountedHandle();
    h.ck_bytes = 511;
    switch (implementation.mountedAgainstDevice(h, caps(16, 512))) {
        .fault => |f| try std.testing.expectEqual(core.err_invalid_size, f),
        .value => return error.TestUnexpectedResult,
    }
}

test "a checkpoint longer than its tail is refused" {
    try std.testing.expectEqual(core.ok, implementation.checkpointFits(512, 512));
    try std.testing.expectEqual(core.err_invalid_size, implementation.checkpointFits(513, 512));
}

test "the mount states keep the C's numbering" {
    // ra8_ftl_mount_state_t is `enum : uint8_t`: ra8_ftl_mount stores through
    // the caller's one-byte state, so the type must stay one byte wide.
    try std.testing.expectEqual(u8, mount.State);
    try std.testing.expectEqual(@as(mount.State, 0), mount.state_cold);
    try std.testing.expectEqual(@as(mount.State, 1), mount.state_resumed);
    try std.testing.expectEqual(@as(u32, 1), mount.reserved_tail_min);
}
