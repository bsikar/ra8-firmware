// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! The font store's decisions, run against a fake volume: which name is
//! opened, when a missing file is provisioned, and what counts as a font.

const std = @import("std");
const policy = @import("policy");

const err = policy.err;

/// A volume that answers from a script, and records what it was asked to do.
const FakeVolume = struct {
    open_results: []const u16 = &.{},
    opens: u32 = 0,
    write_result: u16 = err.ok,
    writes: u32 = 0,
    last_write_len: u32 = 0,
    read_result: u16 = err.ok,
    read_bytes: u32 = 0,
    closes: u32 = 0,
    opened_names: [4][:0]const u8 = @splat(""),

    /// A non-null placeholder for `ra8_fs_file_t*`: the policy only ever
    /// passes it back down, never dereferences it.
    var file_token: u8 = 0;

    fn seam(self: *FakeVolume) policy.Fs {
        return .{
            .ctx = self,
            .open_read = openRead,
            .write_file = writeFile,
            .read = read,
            .close = close,
        };
    }

    fn openRead(ctx: ?*anyopaque, name: [*:0]const u8, out_file: *?policy.File) u16 {
        const self: *FakeVolume = @ptrCast(@alignCast(ctx.?));
        if (self.opens < self.opened_names.len) {
            self.opened_names[self.opens] = std.mem.span(name);
        }
        const status = self.open_results[self.opens];
        self.opens += 1;
        if (status == err.ok) {
            out_file.* = @ptrCast(&file_token);
        }
        return status;
    }

    fn writeFile(ctx: ?*anyopaque, name: [*:0]const u8, data: [*]const u8, len: u32) u16 {
        _ = name;
        _ = data;
        const self: *FakeVolume = @ptrCast(@alignCast(ctx.?));
        self.writes += 1;
        self.last_write_len = len;
        return self.write_result;
    }

    fn read(ctx: ?*anyopaque, file: policy.File, buf: [*]u8, cap: u32, out_got: *u32) u16 {
        _ = file;
        const self: *FakeVolume = @ptrCast(@alignCast(ctx.?));
        const handed = @min(self.read_bytes, cap);
        @memset(buf[0..handed], 0xA5);
        out_got.* = handed;
        return self.read_result;
    }

    fn close(ctx: ?*anyopaque, file: policy.File) u16 {
        _ = file;
        const self: *FakeVolume = @ptrCast(@alignCast(ctx.?));
        self.closes += 1;
        return err.ok;
    }
};

test "a NULL filename selects the default" {
    try std.testing.expectEqualStrings("FONT.OTF", std.mem.span(policy.resolveName(null)));
}

test "a given filename is used as-is" {
    const name: [*:0]const u8 = "LITERATA.OTF";
    try std.testing.expectEqualStrings("LITERATA.OTF", std.mem.span(policy.resolveName(name)));
}

test "zero capacity is rejected before anything touches the bus" {
    try std.testing.expectEqual(err.invalid_arg, policy.validateCapacity(0));
    try std.testing.expectEqual(err.ok, policy.validateCapacity(1));
}

test "a font already on the card is read without provisioning" {
    var fake: FakeVolume = .{ .open_results = &.{err.ok} };
    var file: ?policy.File = null;
    var source: policy.Source = .provisioned;

    const status = policy.openOrProvision(fake.seam(), "FONT.OTF", .{}, &file, &source);

    try std.testing.expectEqual(err.ok, status);
    try std.testing.expectEqual(policy.Source.card, source);
    try std.testing.expectEqual(@as(u32, 0), fake.writes);
    try std.testing.expect(file != null);
}

test "an absent font is written from the blob, then read back off the card" {
    var blob: [32]u8 = @splat(0x4F);
    var fake: FakeVolume = .{ .open_results = &.{ err.not_found, err.ok } };
    var file: ?policy.File = null;
    var source: policy.Source = .card;

    const status = policy.openOrProvision(
        fake.seam(),
        "FONT.OTF",
        .{ .data = &blob, .len = blob.len },
        &file,
        &source,
    );

    try std.testing.expectEqual(err.ok, status);
    try std.testing.expectEqual(policy.Source.provisioned, source);
    try std.testing.expectEqual(@as(u32, 1), fake.writes);
    try std.testing.expectEqual(@as(u32, 32), fake.last_write_len);
    // The bytes handed back are always the on-card copy, so the re-open is
    // what makes this correct rather than the write.
    try std.testing.expectEqual(@as(u32, 2), fake.opens);
}

test "no blob means a missing font stays missing" {
    var fake: FakeVolume = .{ .open_results = &.{err.not_found} };
    var file: ?policy.File = null;
    var source: policy.Source = .provisioned;

    const status = policy.openOrProvision(fake.seam(), "FONT.OTF", .{}, &file, &source);

    try std.testing.expectEqual(err.not_found, status);
    try std.testing.expectEqual(policy.Source.card, source);
    try std.testing.expectEqual(@as(u32, 0), fake.writes);
}

test "an empty blob means a missing font stays missing" {
    var blob: [4]u8 = @splat(0x4F);
    var fake: FakeVolume = .{ .open_results = &.{err.not_found} };
    var file: ?policy.File = null;
    var source: policy.Source = .card;

    const status = policy.openOrProvision(
        fake.seam(),
        "FONT.OTF",
        .{ .data = &blob, .len = 0 },
        &file,
        &source,
    );

    try std.testing.expectEqual(err.not_found, status);
    try std.testing.expectEqual(@as(u32, 0), fake.writes);
}

test "an open failure other than not-found is forwarded untouched" {
    var fake: FakeVolume = .{ .open_results = &.{err.invalid_arg} };
    var file: ?policy.File = null;
    var source: policy.Source = .card;

    const status = policy.openOrProvision(fake.seam(), "FONT.OTF", .{}, &file, &source);

    try std.testing.expectEqual(err.invalid_arg, status);
    try std.testing.expectEqual(@as(u32, 0), fake.writes);
}

test "a failed provisioning write is reported rather than retried" {
    var blob: [8]u8 = @splat(0x4F);
    var fake: FakeVolume = .{ .open_results = &.{err.not_found}, .write_result = err.no_data };
    var file: ?policy.File = null;
    var source: policy.Source = .card;

    const status = policy.openOrProvision(
        fake.seam(),
        "FONT.OTF",
        .{ .data = &blob, .len = blob.len },
        &file,
        &source,
    );

    try std.testing.expectEqual(err.no_data, status);
    try std.testing.expectEqual(@as(u32, 1), fake.opens);
}

test "the name the caller asked for is the name that gets opened" {
    var fake: FakeVolume = .{ .open_results = &.{err.ok} };
    var file: ?policy.File = null;
    var source: policy.Source = .card;

    _ = policy.openOrProvision(fake.seam(), "LITERATA.OTF", .{}, &file, &source);

    try std.testing.expectEqualStrings("LITERATA.OTF", fake.opened_names[0]);
}

test "a full-length read hands back the byte count" {
    var fake: FakeVolume = .{ .read_bytes = 4096 };
    var storage: [4096]u8 = @splat(0);
    var length: u32 = 0;

    const status = policy.readFont(fake.seam(), @ptrCast(&FakeVolume.file_token), &storage, storage.len, &length);

    try std.testing.expectEqual(err.ok, status);
    try std.testing.expectEqual(@as(u32, 4096), length);
    try std.testing.expectEqual(@as(u32, 1), fake.closes);
}

test "a read shorter than a font header is no-data" {
    var fake: FakeVolume = .{ .read_bytes = policy.limits.min_font_bytes - 1 };
    var storage: [64]u8 = @splat(0);
    var length: u32 = 0xDEAD;

    const status = policy.readFont(fake.seam(), @ptrCast(&FakeVolume.file_token), &storage, storage.len, &length);

    try std.testing.expectEqual(err.no_data, status);
    try std.testing.expectEqual(@as(u32, 0xDEAD), length);
    // Still closed: a rejected font must not leak the handle.
    try std.testing.expectEqual(@as(u32, 1), fake.closes);
}

test "exactly the minimum length is accepted" {
    var fake: FakeVolume = .{ .read_bytes = policy.limits.min_font_bytes };
    var storage: [64]u8 = @splat(0);
    var length: u32 = 0;

    const status = policy.readFont(fake.seam(), @ptrCast(&FakeVolume.file_token), &storage, storage.len, &length);

    try std.testing.expectEqual(err.ok, status);
    try std.testing.expectEqual(policy.limits.min_font_bytes, length);
}

test "a failed read closes the handle and forwards the failure" {
    var fake: FakeVolume = .{ .read_result = err.invalid_arg, .read_bytes = 4096 };
    var storage: [64]u8 = @splat(0);
    var length: u32 = 0;

    const status = policy.readFont(fake.seam(), @ptrCast(&FakeVolume.file_token), &storage, storage.len, &length);

    try std.testing.expectEqual(err.invalid_arg, status);
    try std.testing.expectEqual(@as(u32, 1), fake.closes);
}
