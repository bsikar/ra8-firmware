//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What the checker says: one line for each thing wrong, then a verdict.

const std = @import("std");
const arm = @import("arm.zig");
const check = @import("check.zig");
const coverage = @import("coverage.zig");
const elf32 = @import("elf32.zig");
const records = @import("records.zig");

/// Check `image` and write the report for the file called `path`. Returns
/// how many things were wrong; zero is a pass.
pub fn write(writer: anytype, path: []const u8, image: []const u8) !usize {
    const table = try records.Records.read(try elf32.File.init(image));
    var sites = try check.Iterator.init(image);
    var covered: usize = 0;
    var missed: usize = 0;
    while (try sites.next()) |site| {
        if (coverage.covers(table, site)) {
            covered += 1;
            continue;
        }
        missed += 1;
        try siteLine(writer, path, site);
    }

    var stray: usize = 0;
    for (0..table.count()) |index| {
        const fault = try coverage.fault(image, table, index) orelse continue;
        stray += 1;
        try writer.print("{s}: rebase record 0x{x:0>8}: {s}\n", .{
            path,
            table.at(index),
            switch (fault) {
                .names_no_site => "no data word at that address holds an address",
                .listed_twice => "listed more than once",
            },
        });
    }
    try verdict(writer, path, covered, missed, stray);
    return missed + stray;
}

fn verdict(writer: anytype, path: []const u8, covered: usize, missed: usize, stray: usize) !void {
    if (missed != 0) try writer.print(
        "{s}: FAIL, {d} word(s) of data hold an address nothing rebases at load\n",
        .{ path, missed },
    );
    if (stray != 0) try writer.print(
        "{s}: FAIL, {d} rebase record(s) would change a word they should not\n",
        .{ path, stray },
    );
    if (missed != 0 or stray != 0) return;
    if (covered == 0) {
        try writer.print("{s}: OK, no absolute relocation in a data section\n", .{path});
    } else {
        try writer.print(
            "{s}: OK, {d} word(s) of data hold an address, each one in the rebase records\n",
            .{ path, covered },
        );
    }
}

/// One site, as `section at address: type -> symbol (value)`.
pub fn siteLine(writer: anytype, path: []const u8, site: check.Finding) !void {
    try writer.print("{s}: {s} at 0x{x:0>8}: ", .{ path, site.section, site.address });
    if (arm.name(site.kind)) |name| {
        try writer.writeAll(name);
    } else {
        try writer.print("relocation type {d}", .{site.kind});
    }
    try writer.print(" -> {s} (0x{x:0>8})\n", .{ site.symbol, site.value });
}
