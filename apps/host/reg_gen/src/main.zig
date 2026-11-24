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

fn findJsonFile(io: std.Io, buf: *[std.Io.Dir.max_path_bytes]u8) ![]const u8 {
    const candidates = [_][]const u8{
        "registers.json",
        "apps/host/reg_gen/registers.json",
    };

    const cwd = std.Io.Dir.cwd();
    for (candidates) |path| {
        const file = cwd.openFile(io, path, .{}) catch continue;
        file.close(io);
        return path;
    }

    var bin_dir_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
    const dir_len = std.process.executableDirPath(io, &bin_dir_buf) catch return error.FileNotFound;
    const joined = std.fmt.bufPrint(buf, "{s}{c}registers.json", .{ bin_dir_buf[0..dir_len], std.fs.path.sep }) catch return error.PathTooLong;
    const file = cwd.openFile(io, joined, .{}) catch return error.FileNotFound;
    file.close(io);
    return joined;
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

fn run(init: std.process.Init) !void {
    const io = init.io;
    const allocator = init.arena.allocator();
    var stderr_buf: [256]u8 = undefined;
    var stderr_writer = std.Io.File.stderr().writer(io, &stderr_buf);
    const stderr = &stderr_writer.interface;

    const args = try init.minimal.args.toSlice(allocator);
    const opts = parseCliArgs(args, stderr) catch |err| {
        stderr.flush() catch {};
        return err;
    };
    if (opts.show_help) {
        var stdout_buf: [256]u8 = undefined;
        var stdout_writer = std.Io.File.stdout().writer(io, &stdout_buf);
        try printUsage(&stdout_writer.interface);
        try stdout_writer.interface.flush();
        return;
    }

    var path_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
    const resolved_input = if (opts.input_path) |p|
        p
    else
        findJsonFile(io, &path_buf) catch |err| {
            stderr.print("Error: could not locate default registers.json: {s}\n", .{@errorName(err)}) catch {};
            stderr.flush() catch {};
            return err;
        };

    const cwd = std.Io.Dir.cwd();
    const json_bytes = cwd.readFileAlloc(io, resolved_input, allocator, .limited(10 * 1024 * 1024)) catch |err| {
        stderr.print("Error reading file '{s}': {s}\n", .{ resolved_input, @errorName(err) }) catch {};
        stderr.flush() catch {};
        return err;
    };

    const parsed = generator.parseDefinition(allocator, json_bytes) catch |err| {
        stderr.print("Error parsing register definition JSON in '{s}': {s}\n", .{ resolved_input, @errorName(err) }) catch {};
        stderr.flush() catch {};
        return err;
    };
    defer parsed.deinit();

    var io_buf: [4096]u8 = undefined;
    if (opts.output_path) |out_p| {
        const out_file = cwd.createFile(io, out_p, .{}) catch |err| {
            stderr.print("Error creating output file '{s}': {s}\n", .{ out_p, @errorName(err) }) catch {};
            stderr.flush() catch {};
            return err;
        };
        defer out_file.close(io);
        var file_writer = out_file.writer(io, &io_buf);
        try generator.generateC23Header(parsed.value, allocator, &file_writer.interface);
        try file_writer.interface.flush();
    } else {
        var stdout_writer = std.Io.File.stdout().writer(io, &io_buf);
        try generator.generateC23Header(parsed.value, allocator, &stdout_writer.interface);
        try stdout_writer.interface.flush();
    }
}

pub fn main(init: std.process.Init) u8 {
    run(init) catch return 1;
    return 0;
}
