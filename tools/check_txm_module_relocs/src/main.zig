//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `check_txm_module_relocs <module.elf>...`
//!
//! Checks each linked ThreadX module image and prints a report for it. The
//! images must have been linked with `--emit-relocs`.
//!
//! Exit status: 0 when every image passes, 1 when any has a finding, 2 when
//! one could not be read or the command line is wrong. An image this tool
//! cannot read is never a pass.

const std = @import("std");
const report = @import("report.zig");

const max_image_bytes = 64 * 1024 * 1024;

const Exit = struct {
    const pass = 0;
    const findings = 1;
    const unusable = 2;
};

pub fn main() u8 {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const out = std.io.getStdOut().writer();
    const args = std.process.argsAlloc(allocator) catch return Exit.unusable;
    if (args.len < 2) {
        std.io.getStdErr().writer().writeAll(
            "usage: check_txm_module_relocs <module.elf>...\n",
        ) catch {};
        return Exit.unusable;
    }

    var worst: u8 = Exit.pass;
    for (args[1..]) |path| {
        const status = checkOne(allocator, out, path);
        worst = @max(worst, status);
    }
    return worst;
}

fn checkOne(allocator: std.mem.Allocator, out: anytype, path: []const u8) u8 {
    const image = std.fs.cwd().readFileAlloc(allocator, path, max_image_bytes) catch |err| {
        out.print("{s}: cannot read: {s}\n", .{ path, @errorName(err) }) catch {};
        return Exit.unusable;
    };
    const count = report.write(out, path, image) catch |err| {
        out.print("{s}: cannot check: {s}\n", .{ path, @errorName(err) }) catch {};
        return Exit.unusable;
    };
    return if (count == 0) Exit.pass else Exit.findings;
}
