//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `gen_txm_rebase_table <module.elf> <table.s> [--leave-out=<index>]`
//!
//! Reads a module linked with `--emit-relocs` and writes its rebase table as
//! assembly. The module is then linked again with that table in it.
//!
//! Exit status: 0 when the table was written, 1 when the module has a site
//! or a value its start-up could not put right, 2 when the image could not
//! be read or the command line is wrong.

const std = @import("std");
const report = @import("report.zig");
const table = @import("table.zig");

const max_image_bytes = 64 * 1024 * 1024;
const leave_out_flag = "--leave-out=";

const Exit = struct {
    const written = 0;
    const refused = 1;
    const unusable = 2;
};

pub fn main() u8 {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const err = std.io.getStdErr().writer();

    const args = std.process.argsAlloc(allocator) catch return Exit.unusable;
    if (args.len < 3 or args.len > 4) return usage(err);
    var leave_out: ?usize = null;
    if (args.len == 4) {
        if (!std.mem.startsWith(u8, args[3], leave_out_flag)) return usage(err);
        const digits = args[3][leave_out_flag.len..];
        leave_out = std.fmt.parseInt(usize, digits, 10) catch return usage(err);
    }

    const image = std.fs.cwd().readFileAlloc(allocator, args[1], max_image_bytes) catch |e| {
        err.print("{s}: cannot read: {s}\n", .{ args[1], @errorName(e) }) catch {};
        return Exit.unusable;
    };
    var storage: [table.max_sites]u32 = undefined;
    var problem: ?table.Problem = null;
    const addresses = table.sites(image, &storage, &problem) catch |e| {
        return refuse(err, args[1], e, problem);
    };
    if (leave_out) |index| if (index >= addresses.len) {
        const count = addresses.len;
        err.print("{s}: no site {d} to leave out of {d}\n", .{ args[1], index, count }) catch {};
        return Exit.unusable;
    };

    var text = std.ArrayList(u8).init(allocator);
    table.write(text.writer(), addresses, leave_out) catch return Exit.unusable;
    std.fs.cwd().writeFile(.{ .sub_path = args[2], .data = text.items }) catch |e| {
        err.print("{s}: cannot write: {s}\n", .{ args[2], @errorName(e) }) catch {};
        return Exit.unusable;
    };
    return Exit.written;
}

fn usage(err: anytype) u8 {
    err.writeAll(
        "usage: gen_txm_rebase_table <module.elf> <table.s> [--leave-out=<index>]\n",
    ) catch {};
    return Exit.unusable;
}

fn refuse(err: anytype, path: []const u8, cause: table.Error, problem: ?table.Problem) u8 {
    const why: []const u8 = switch (cause) {
        error.SiteInCode => "is not in the data range, and code is never written",
        error.ValueOutsideModule => "holds a value in neither link range",
        error.NotAWholeWord => "is not a whole word, so no delta can be added to it",
        error.DynamicRelocation => "the module has dynamic relocations, which nothing applies",
        error.NotAModule => "the image does not name its link ranges",
        error.TooManySites => "more sites than one table holds",
        else => {
            err.print("{s}: cannot read: {s}\n", .{ path, @errorName(cause) }) catch {};
            return Exit.unusable;
        },
    };
    if (problem) |found| {
        report.siteLine(err, path, found.site) catch {};
        err.print("{s}: the word at 0x{x:0>8} (stored 0x{x:0>8}) {s}\n", .{
            path, found.site.address, found.stored, why,
        }) catch {};
    } else {
        err.print("{s}: {s}\n", .{ path, why }) catch {};
    }
    return Exit.refused;
}
