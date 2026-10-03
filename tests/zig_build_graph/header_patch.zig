//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Rewrite whole lines of a vendored header into a generated copy, so a
//! value upstream hard-codes without an `#ifndef` (RA8FW-484: ThreadX's
//! `TXM_MODULE_MPU_TOTAL_ENTRIES`) can change without forking the vendored
//! file or committing a first-party C header.
//!
//! Every rewrite must match exactly one line, byte for byte. A vendored bump
//! that moves or reformats the line fails the build instead of quietly
//! shipping the upstream value.
//!
//!   header_patch IN OUT OLD NEW [OLD NEW]...

const std = @import("std");

pub const Rewrite = struct {
    old: []const u8,
    new: []const u8,
};

pub const Error = error{ RewriteNotFound, RewriteNotUnique } || std.mem.Allocator.Error;

/// `text` with each rewrite's `old` line replaced by its `new` one. A line
/// is what sits between newlines; `old` and `new` carry no newline.
pub fn apply(allocator: std.mem.Allocator, text: []const u8, rewrites: []const Rewrite) Error![]u8 {
    for (rewrites) |rewrite| {
        if (count(text, rewrite.old) == 0) return error.RewriteNotFound;
        if (count(text, rewrite.old) > 1) return error.RewriteNotUnique;
    }
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    var lines = std.mem.splitScalar(u8, text, '\n');
    var first = true;
    while (lines.next()) |line| {
        if (!first) try out.append('\n');
        first = false;
        try out.appendSlice(replacement(line, rewrites));
    }
    return out.toOwnedSlice();
}

fn replacement(line: []const u8, rewrites: []const Rewrite) []const u8 {
    for (rewrites) |rewrite| {
        if (std.mem.eql(u8, line, rewrite.old)) return rewrite.new;
    }
    return line;
}

/// How many whole lines of `text` equal `line`.
pub fn count(text: []const u8, line: []const u8) usize {
    var n: usize = 0;
    var lines = std.mem.splitScalar(u8, text, '\n');
    while (lines.next()) |candidate| {
        if (std.mem.eql(u8, candidate, line)) n += 1;
    }
    return n;
}

pub fn main() !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const args = try std.process.argsAlloc(allocator);
    if (args.len < 5 or (args.len - 3) % 2 != 0) {
        std.debug.print("usage: header_patch IN OUT OLD NEW [OLD NEW]...\n", .{});
        std.process.exit(2);
    }
    var rewrites = std.ArrayList(Rewrite).init(allocator);
    var i: usize = 3;
    while (i < args.len) : (i += 2) try rewrites.append(.{ .old = args[i], .new = args[i + 1] });
    const text = try std.fs.cwd().readFileAlloc(allocator, args[1], 1 << 20);
    const patched = apply(allocator, text, rewrites.items) catch |err| {
        std.debug.print("header_patch: {s}: {s}\n", .{ args[1], @errorName(err) });
        std.process.exit(1);
    };
    try std.fs.cwd().writeFile(.{ .sub_path = args[2], .data = patched });
}
