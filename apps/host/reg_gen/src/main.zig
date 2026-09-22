//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! reg_gen: Generates strict C23 register headers from JSON register definitions.

const std = @import("std");
pub const generator = @import("generator.zig");

pub fn printUsage(writer: anytype) !void {
    try writer.print(
        \\reg_gen: C23 Register Header Generator
        \\Usage: reg_gen [options] [path/to/registers.json]
        \\
        \\Options:
        \\  -h, --help                Print this help and exit
        \\  -o, --output <file>       Write output to <file> instead of stdout
        \\
        \\Arguments:
        \\  [path/to/registers.json]  Path to input JSON (defaults to registers.json if found)
        \\
    , .{});
}

fn findJsonFile(buf: *[std.fs.max_path_bytes]u8) ![]const u8 {
    const candidates = [_][]const u8{
        "registers.json",
        "apps/host/reg_gen/registers.json",
    };

    for (candidates) |path| {
        const file = std.fs.cwd().openFile(path, .{}) catch continue;
        file.close();
        return path;
    }

    var bin_dir_buf: [std.fs.max_path_bytes]u8 = undefined;
    const exe_dir = std.fs.selfExeDirPath(&bin_dir_buf) catch null;
    if (exe_dir) |dir| {
        const joined = std.fmt.bufPrint(buf, "{s}{c}registers.json", .{ dir, std.fs.path.sep }) catch return error.PathTooLong;
        const file = std.fs.cwd().openFile(joined, .{}) catch null;
        if (file) |f| {
            f.close();
            return joined;
        }
    }

    return error.FileNotFound;
}

pub const CliOptions = struct {
    input_path: ?[]const u8 = null,
    output_path: ?[]const u8 = null,
    show_help: bool = false,
};

pub fn parseCliArgs(args: []const []const u8, err_writer: anytype) !CliOptions {
    var opts = CliOptions{};
    var i: usize = 1;
    while (i < args.len) : (i += 1) {
        const arg = args[i];
        if (std.mem.eql(u8, arg, "-h") or std.mem.eql(u8, arg, "--help")) {
            opts.show_help = true;
            return opts;
        } else if (std.mem.eql(u8, arg, "-o") or std.mem.eql(u8, arg, "--output")) {
            i += 1;
            if (i >= args.len) {
                try err_writer.print("Error: --output requires a file argument\n", .{});
                return error.InvalidArguments;
            }
            opts.output_path = args[i];
        } else if (std.mem.startsWith(u8, arg, "-")) {
            try err_writer.print("Error: unrecognized option '{s}'\n", .{arg});
            return error.InvalidArguments;
        } else if (opts.input_path == null) {
            opts.input_path = arg;
        } else {
            try err_writer.print("Error: unexpected extra argument '{s}'\n", .{arg});
            return error.InvalidArguments;
        }
    }
    return opts;
}

fn run(allocator: std.mem.Allocator) !void {
    const stderr = std.io.getStdErr().writer();

    const args = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, args);

    const opts = try parseCliArgs(args, stderr);
    if (opts.show_help) {
        try printUsage(std.io.getStdOut().writer());
        return;
    }

    var path_buf: [std.fs.max_path_bytes]u8 = undefined;
    const resolved_input = if (opts.input_path) |p|
        p
    else
        findJsonFile(&path_buf) catch |err| {
            try stderr.print("Error: could not locate default registers.json: {s}\n", .{@errorName(err)});
            return err;
        };

    const file = std.fs.cwd().openFile(resolved_input, .{}) catch |err| {
        try stderr.print("Error opening input file '{s}': {s}\n", .{ resolved_input, @errorName(err) });
        return err;
    };
    defer file.close();

    const json_bytes = file.readToEndAlloc(allocator, 10 * 1024 * 1024) catch |err| {
        try stderr.print("Error reading file '{s}': {s}\n", .{ resolved_input, @errorName(err) });
        return err;
    };
    defer allocator.free(json_bytes);

    const parsed = generator.parseDefinition(allocator, json_bytes) catch |err| {
        try stderr.print("Error parsing register definition JSON in '{s}': {s}\n", .{ resolved_input, @errorName(err) });
        return err;
    };
    defer parsed.deinit();

    if (opts.output_path) |out_p| {
        const out_file = std.fs.cwd().createFile(out_p, .{}) catch |err| {
            try stderr.print("Error creating output file '{s}': {s}\n", .{ out_p, @errorName(err) });
            return err;
        };
        defer out_file.close();
        try generator.generateC23Header(parsed.value, allocator, out_file.writer());
    } else {
        const stdout = std.io.getStdOut().writer();
        try generator.generateC23Header(parsed.value, allocator, stdout);
    }
}

pub fn main() u8 {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer {
        const check = gpa.deinit();
        if (check == .leak) {
            std.log.err("Memory leak detected in reg_gen GeneralPurposeAllocator", .{});
        }
    }
    const allocator = gpa.allocator();

    run(allocator) catch return 1;
    return 0;
}
