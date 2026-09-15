//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! Dedicated native Zig tests for the reg_gen command-line boundary.

const std = @import("std");
const application = @import("application");
const generator = application.generator;
const parseCliArgs = application.parseCliArgs;
const printUsage = application.printUsage;

test "main module references generator tests" {
    _ = generator;
}

test "printUsage outputs help text" {
    var buffer = std.ArrayList(u8).init(std.testing.allocator);
    defer buffer.deinit();

    try printUsage(buffer.writer());
    try std.testing.expect(std.mem.indexOf(u8, buffer.items, "Usage: reg_gen") != null);
    try std.testing.expect(std.mem.indexOf(u8, buffer.items, "--help") != null);
    try std.testing.expect(std.mem.indexOf(u8, buffer.items, "--output") != null);
}

test "parseCliArgs handles valid arguments and options" {
    var err_buf = std.ArrayList(u8).init(std.testing.allocator);
    defer err_buf.deinit();

    const args1 = [_][]const u8{ "reg_gen", "-h" };
    const opts1 = try parseCliArgs(&args1, err_buf.writer());
    try std.testing.expect(opts1.show_help);

    const args2 = [_][]const u8{ "reg_gen", "-o", "out.h", "regs.json" };
    const opts2 = try parseCliArgs(&args2, err_buf.writer());
    try std.testing.expect(!opts2.show_help);
    try std.testing.expectEqualStrings("out.h", opts2.output_path.?);
    try std.testing.expectEqualStrings("regs.json", opts2.input_path.?);
}

test "parseCliArgs error conditions" {
    var err_buf = std.ArrayList(u8).init(std.testing.allocator);
    defer err_buf.deinit();

    const args_missing_o = [_][]const u8{ "reg_gen", "-o" };
    try std.testing.expectError(error.InvalidArguments, parseCliArgs(&args_missing_o, err_buf.writer()));

    const args_unknown = [_][]const u8{ "reg_gen", "--unknown" };
    try std.testing.expectError(error.InvalidArguments, parseCliArgs(&args_unknown, err_buf.writer()));

    const args_extra = [_][]const u8{ "reg_gen", "file1.json", "file2.json" };
    try std.testing.expectError(error.InvalidArguments, parseCliArgs(&args_extra, err_buf.writer()));
}
