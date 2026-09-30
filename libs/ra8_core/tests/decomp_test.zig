//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the decompression-limits policy (#2862). The saturating
//! arithmetic is the security-load-bearing part, so each bound and each
//! wrap guard is exercised on its own.

const std = @import("std");

const policy = @import("decomp_policy");
const ratio = @import("decomp_ratio");
const budget = @import("decomp_budget");
const zip = @import("decomp_zip_eocd");

/// The tight policy the C suite uses, small enough to breach cheaply.
fn tight() policy.Limits {
    return .{
        .max_output_bytes = 4096,
        .max_ratio = 4,
        .ratio_grace_bytes = 16,
        .max_entries = 3,
        .max_iterations = 5,
        .max_depth = 2,
    };
}

test "the default policy is usable and every field is the owner-approved value" {
    const lim = policy.default();
    try std.testing.expectEqual(@as(u64, 64 * 1024 * 1024), lim.max_output_bytes);
    try std.testing.expectEqual(@as(u32, 1024), lim.max_ratio);
    try std.testing.expectEqual(@as(u32, 65536), lim.ratio_grace_bytes);
    try std.testing.expectEqual(@as(u32, 4096), lim.max_entries);
    try std.testing.expectEqual(@as(u32, 1048576), lim.max_iterations);
    try std.testing.expectEqual(@as(u8, 2), lim.max_depth);
    try std.testing.expect(lim.usable());
}

test "a zero in any one field makes a policy unusable" {
    const fields = .{
        "max_output_bytes",
        "max_ratio",
        "ratio_grace_bytes",
        "max_entries",
        "max_iterations",
        "max_depth",
    };
    inline for (fields) |name| {
        var lim = tight();
        @field(lim, name) = 0;
        try std.testing.expect(!lim.usable());
    }
}

test "the ratio bound is input times ratio plus grace" {
    try std.testing.expectEqual(@as(u64, 416), ratio.bound(tight(), 100));
    try std.testing.expectEqual(@as(u64, 16), ratio.bound(tight(), 0));
}

test "a product that would wrap saturates instead" {
    const lim = policy.Limits{
        .max_output_bytes = 1,
        .max_ratio = 4,
        .ratio_grace_bytes = 1,
        .max_entries = 1,
        .max_iterations = 1,
        .max_depth = 1,
    };
    try std.testing.expectEqual(ratio.saturated, ratio.bound(lim, std.math.maxInt(u64) / 2));
}

test "a sum that would wrap saturates instead" {
    const lim = policy.Limits{
        .max_output_bytes = 1,
        .max_ratio = 1,
        .ratio_grace_bytes = 4,
        .max_entries = 1,
        .max_iterations = 1,
        .max_depth = 1,
    };
    try std.testing.expectEqual(ratio.saturated, ratio.bound(lim, std.math.maxInt(u64) - 2));
}

test "output charging fires the cap and the ratio bound exactly at their edges" {
    var b = budget.Budget{ .limits = tight() };
    try b.chargeOutput(100, 416);
    try std.testing.expectError(budget.Breach.Ratio, b.chargeOutput(100, 1));

    var cap = budget.Budget{ .limits = tight() };
    try cap.chargeOutput(4096, 4096);
    try std.testing.expectError(budget.Breach.OutputCap, cap.chargeOutput(4096, 1));
}

test "a delta large enough to wrap the accumulator is reported, not absorbed" {
    var b = budget.Budget{ .limits = tight() };
    try b.chargeOutput(8, 8);
    try std.testing.expectError(
        budget.Breach.OutputCap,
        b.chargeOutput(8, std.math.maxInt(u64) - 2),
    );
    try std.testing.expectEqual(ratio.saturated, b.out_bytes);
}

test "charging records the input consumed even when a bound breaks" {
    var b = budget.Budget{ .limits = tight() };
    try std.testing.expectError(budget.Breach.OutputCap, b.chargeOutput(77, 1 << 20));
    try std.testing.expectEqual(@as(u64, 77), b.in_bytes);
}

test "the entry and iteration budgets stop one past their cap" {
    var b = budget.Budget{ .limits = tight() };
    for (0..3) |_| try b.chargeEntry();
    try std.testing.expectError(budget.Breach.Entries, b.chargeEntry());
    try std.testing.expectEqual(@as(u32, 3), b.entries);

    for (0..5) |_| try b.chargeIter();
    try std.testing.expectError(budget.Breach.Iterations, b.chargeIter());
}

test "depth is balanced by leave, and an unbalanced leave is ignored" {
    var b = budget.Budget{ .limits = tight() };
    try b.enter();
    try b.enter();
    try std.testing.expectError(budget.Breach.Depth, b.enter());
    b.leave();
    try b.enter();
    b.leave();
    b.leave();
    b.leave();
    try std.testing.expectEqual(@as(u8, 0), b.depth);
}

test "a declared size is checked against both bounds before any decode" {
    try budget.checkDeclared(tight(), 100, 416);
    try std.testing.expectError(budget.Breach.Ratio, budget.checkDeclared(tight(), 100, 417));
    try std.testing.expectError(budget.Breach.OutputCap, budget.checkDeclared(tight(), 1 << 20, 4097));
}

/// A ZIP-shaped buffer: `entries` declared in an EOCD at the very end.
fn archiveWith(buf: []u8, entries: u16, comment: u16) []u8 {
    @memset(buf, 0);
    const at = buf.len - zip.geometry.eocd_bytes - comment;
    @memcpy(buf[at..][0..zip.signature.len], &zip.signature);
    std.mem.writeInt(u16, buf[at + zip.geometry.entries_offset ..][0..2], entries, .little);
    std.mem.writeInt(u16, buf[at + zip.geometry.comment_offset ..][0..2], comment, .little);
    return buf;
}

const Fixture = struct {
    bytes: []const u8,

    fn read(ctx: ?*anyopaque, dst: []u8, offset: u64) usize {
        const self: *const Fixture = @ptrCast(@alignCast(ctx.?));
        if (offset >= self.bytes.len) return 0;
        const from: usize = @intCast(offset);
        const take = @min(dst.len, self.bytes.len - from);
        @memcpy(dst[0..take], self.bytes[from..][0..take]);
        return take;
    }

    fn reader(self: *const Fixture) zip.Reader {
        return .{ .ctx = @constCast(@ptrCast(self)), .read = Fixture.read };
    }
};

fn preflight(bytes: []const u8, max_entries: u32) zip.Verdict {
    var scratch: [zip.scratch_bytes]u8 = undefined;
    const fixture = Fixture{ .bytes = bytes };
    return zip.preflight(fixture.reader(), bytes.len, &scratch, max_entries);
}

test "an EOCD declaring more entries than the cap is rejected" {
    var buf: [64]u8 = undefined;
    try std.testing.expectEqual(zip.Verdict.over_entry_cap, preflight(archiveWith(&buf, 9, 0), 4));
}

test "an EOCD within the cap leaves the decoder authoritative" {
    var buf: [64]u8 = undefined;
    try std.testing.expectEqual(zip.Verdict.inconclusive, preflight(archiveWith(&buf, 4, 0), 4));
}

test "a record found behind its declared comment still validates" {
    var buf: [128]u8 = undefined;
    try std.testing.expectEqual(zip.Verdict.over_entry_cap, preflight(archiveWith(&buf, 40, 17), 4));
}

test "a signature whose comment length misses the archive end is skipped" {
    var buf: [64]u8 = undefined;
    const bytes = archiveWith(&buf, 9, 0);
    std.mem.writeInt(u16, bytes[bytes.len - zip.geometry.eocd_bytes + zip.geometry.comment_offset ..][0..2], 5, .little);
    try std.testing.expectEqual(zip.Verdict.inconclusive, preflight(bytes, 4));
}

test "an archive too short to hold an EOCD is inconclusive rather than read" {
    var buf: [8]u8 = undefined;
    @memset(&buf, 0);
    try std.testing.expectEqual(zip.Verdict.inconclusive, preflight(&buf, 4));
}

test "a scan window wider than one chunk still reaches the record" {
    const size = 1600;
    var buf: [size]u8 = undefined;
    try std.testing.expectEqual(zip.Verdict.over_entry_cap, preflight(archiveWith(&buf, 5000, 0), 4096));
}
