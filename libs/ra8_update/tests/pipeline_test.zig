//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the pure `ra8_update` pipeline decisions and error mapping.

const std = @import("std");
const Err = @import("err").Err;
const pl = @import("pipeline");

test "unknown external error codes map safely without panic" {
    try std.testing.expectEqual(Err.ok, Err.from(0));
    try std.testing.expectEqual(Err.crc_mismatch, Err.from(0x0405));
    try std.testing.expectEqual(Err.fail, Err.from(0xFFFF));
    try std.testing.expectEqual(Err.fail, Err.from(0xBEEF));
}

test "canonical declarations pick distinct transport/trial policies" {
    const ota = pl.canonicalDecl(.ota);
    const dfu = pl.canonicalDecl(.dfu);
    const mem = pl.canonicalDecl(.mem);

    try std.testing.expectEqual(@as(u16, Err.ok.raw()), pl.validateDecl(&ota));
    try std.testing.expectEqual(@as(u16, Err.ok.raw()), pl.validateDecl(&dfu));
    try std.testing.expectEqual(@as(u16, Err.ok.raw()), pl.validateDecl(&mem));

    try std.testing.expectEqual(@as(u8, 0), ota.trial_budget);
    try std.testing.expect(!ota.defer_floor_bump);
    try std.testing.expect(pl.shouldCommitFloorAtCommit(&ota));

    try std.testing.expectEqual(@as(u8, 3), dfu.trial_budget);
    try std.testing.expect(dfu.defer_floor_bump);
    try std.testing.expect(!pl.shouldCommitFloorAtCommit(&dfu));
}

test "validateDecl rejects inconsistent rollback or unknown enum tags" {
    var bad = pl.canonicalDecl(.dfu);
    bad.enforce_rollback = false;
    try std.testing.expectEqual(Err.invalid_arg.raw(), pl.validateDecl(&bad));

    bad = pl.canonicalDecl(.dfu);
    bad.defer_floor_bump = false; // trial_budget > 0 must defer floor bump
    try std.testing.expectEqual(Err.invalid_arg.raw(), pl.validateDecl(&bad));

    bad = pl.canonicalDecl(.ota);
    bad.transport = 99;
    try std.testing.expectEqual(Err.invalid_arg.raw(), pl.validateDecl(&bad));

    bad = pl.canonicalDecl(.ota);
    bad.reserved0 = 1;
    try std.testing.expectEqual(Err.invalid_arg.raw(), pl.validateDecl(&bad));
}

test "staging bounds enforce sequential offsets and 32B MRAM page alignment" {
    try std.testing.expectEqual(Err.invalid_size.raw(), pl.beginSizeValid(0, 4096, 32));
    try std.testing.expectEqual(Err.invalid_size.raw(), pl.beginSizeValid(4097, 4096, 32));
    try std.testing.expectEqual(Err.invalid_size.raw(), pl.beginSizeValid(33, 4096, 32));
    try std.testing.expectEqual(Err.ok.raw(), pl.beginSizeValid(128, 4096, 32));

    // Sequential 64-byte chunks succeed; skipped offset or unaligned mid chunk fail.
    try std.testing.expectEqual(Err.ok.raw(), pl.stageWriteValid(0, 64, 0, 128, 32));
    try std.testing.expectEqual(Err.invalid_arg.raw(), pl.stageWriteValid(64, 64, 0, 128, 32));
    try std.testing.expectEqual(Err.invalid_size.raw(), pl.stageWriteValid(0, 31, 0, 128, 32));
    try std.testing.expectEqual(Err.invalid_size.raw(), pl.stageWriteValid(64, 65, 64, 128, 32));
}

test "constant-time compare and terminal stage classification" {
    const a = [_]u8{ 1, 2, 3, 4 };
    var b = [_]u8{ 1, 2, 3, 4 };
    try std.testing.expect(pl.equalConstantTime(&a, &b));
    b[3] ^= 1;
    try std.testing.expect(!pl.equalConstantTime(&a, &b));
    try std.testing.expect(!pl.equalConstantTime(&a, a[0..3]));
    try std.testing.expect(!pl.equalConstantTime(&.{}, &.{}));

    try std.testing.expect(!pl.stageIsTerminal(.staging));
    try std.testing.expect(!pl.stageIsTerminal(.trial_pending));
    try std.testing.expect(pl.stageIsTerminal(.confirmed));
    try std.testing.expect(pl.stageIsTerminal(.rolled_back));
    try std.testing.expect(pl.stageIsTerminal(.failed));
}
