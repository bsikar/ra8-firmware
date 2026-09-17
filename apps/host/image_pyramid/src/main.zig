//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
/// JPEG decode and encode over the C `ra8_jpeg_sw` implementation.
pub const codec = @import("codec.zig");
/// The strip-discarding downsampler and its level planning.
pub const degrade = @import("degrade.zig");

const usage = "usage: image_pyramid <input.jpg> --out-dir <dir> [--levels <1..16>]\n";
const default_levels: u8 = 8;

const Options = struct { input: []const u8, output_dir: []const u8, levels: u8 };

const UsageError = error{
    MissingInput,
    MissingOutputDir,
    DuplicateOption,
    MissingOptionValue,
    InvalidLevels,
    UnknownOption,
    ExtraArgument,
};

fn usageName(err: UsageError) []const u8 {
    return switch (err) {
        error.MissingInput => "missing-input",
        error.MissingOutputDir => "missing-output-dir",
        error.DuplicateOption => "duplicate-option",
        error.MissingOptionValue => "missing-option-value",
        error.InvalidLevels => "invalid-levels",
        error.UnknownOption => "unknown-option",
        error.ExtraArgument => "extra-argument",
    };
}

fn parseLevels(text: []const u8) UsageError!u8 {
    if (text.len == 0 or text.len > 2 or (text.len > 1 and text[0] == '0')) return error.InvalidLevels;
    for (text) |byte| if (byte < '0' or byte > '9') return error.InvalidLevels;
    const value = std.fmt.parseInt(u8, text, 10) catch return error.InvalidLevels;
    if (value < 1 or value > 16) return error.InvalidLevels;
    return value;
}

fn parseArgs(args: []const []const u8) UsageError!Options {
    if (args.len < 2) return error.MissingInput;
    if (args.len > 6) return error.ExtraArgument;
    const input = args[1];
    if (input.len == 0 or input[0] == '-') return error.UnknownOption;
    var output_dir: ?[]const u8 = null;
    var levels = default_levels;
    var saw_levels = false;
    var index: usize = 2;
    while (index < args.len) {
        const option = args[index];
        if (std.mem.eql(u8, option, "--out-dir")) {
            if (output_dir != null) return error.DuplicateOption;
            if (index + 1 >= args.len) return error.MissingOptionValue;
            output_dir = args[index + 1];
            if (output_dir.?.len == 0) return error.MissingOutputDir;
            index += 2;
        } else if (std.mem.eql(u8, option, "--levels")) {
            if (saw_levels) return error.DuplicateOption;
            if (index + 1 >= args.len) return error.MissingOptionValue;
            levels = try parseLevels(args[index + 1]);
            saw_levels = true;
            index += 2;
        } else if (option.len > 0 and option[0] == '-') {
            return error.UnknownOption;
        } else {
            return error.ExtraArgument;
        }
    }
    return .{ .input = input, .output_dir = output_dir orelse return error.MissingOutputDir, .levels = levels };
}

fn printCodecError(writer: anytype, stage: []const u8, failure: codec.CodecFailure) !void {
    try writer.print("error: {s}: 0x{x:0>8}\n", .{ stage, failure.code });
}

fn readInput(allocator: std.mem.Allocator, path: []const u8) ![]u8 {
    const file = try std.fs.cwd().openFile(path, .{});
    defer file.close();
    const stat = try file.stat();
    if (stat.kind != .file) return error.InputNotRegular;
    if (stat.size > 16 * 1024 * 1024) return error.InputTooLarge;
    return try file.readToEndAlloc(allocator, 16 * 1024 * 1024);
}

fn outputNames(allocator: std.mem.Allocator, dims: degrade.Dimensions, level: usize) !struct { final: []u8, temp: []u8 } {
    const final = try std.fmt.allocPrint(allocator, "level-{d:0>2}-{d}x{d}-q25.jpg", .{ level, dims.width, dims.height });
    errdefer allocator.free(final);
    const temp = try std.fmt.allocPrint(allocator, ".{s}.tmp", .{final});
    return .{ .final = final, .temp = temp };
}

fn entryExists(dir: std.fs.Dir, name: []const u8) !bool {
    _ = std.posix.fstatat(dir.fd, name, std.posix.AT.SYMLINK_NOFOLLOW) catch |err| switch (err) {
        error.FileNotFound => return false,
        else => return err,
    };
    return true;
}

fn publishWithoutReplace(dir: std.fs.Dir, temp: []const u8, final: []const u8) !void {
    try std.posix.linkat(dir.fd, temp, dir.fd, final, 0);
}

/// Runs the tool over `args` and returns its process exit status.
///
/// Takes the writers and the allocator as parameters so tests can drive the whole
/// tool in-process. Returns 0 on success, 2 for a usage error, and 1 for a read,
/// codec or filesystem failure, printing the reason to `stderr` in each case.
/// Levels are published atomically: each is written to a temporary name and linked
/// into place, so a partial run never leaves a half-written level behind.
pub fn execute(allocator: std.mem.Allocator, args: []const []const u8, stdout: anytype, stderr: anytype) !u8 {
    if (args.len == 2 and std.mem.eql(u8, args[1], "--help")) {
        try stdout.writeAll(usage);
        return 0;
    }
    const options = parseArgs(args) catch |err| {
        try stderr.print("error: {s}\n{s}", .{ usageName(err), usage });
        return 2;
    };

    const input = readInput(allocator, options.input) catch |err| {
        try stderr.print("error: read-input: {s}\n", .{@errorName(err)});
        return 1;
    };
    defer allocator.free(input);

    const source_dims = switch (try codec.dimensions(input)) {
        .value => |value| value,
        .failure => |failure| {
            try printCodecError(stderr, "dimensions", failure);
            return 1;
        },
    };
    if (source_dims.width > 2048 or source_dims.height > 2048) {
        try stderr.writeAll("error: dimensions: ImageTooLarge\n");
        return 1;
    }
    const level_plan = degrade.plan(source_dims.width, source_dims.height, options.levels) catch {
        try stderr.print("error: too-many-levels\n{s}", .{usage});
        return 2;
    };
    for (level_plan[0..options.levels]) |dims| {
        if (dims.width > 1024) {
            try stderr.writeAll("error: dimensions: ImageTooLarge\n");
            return 1;
        }
    }

    var output_dir = std.fs.cwd().openDir(options.output_dir, .{ .no_follow = true }) catch |err| {
        try stderr.print("error: output-directory: {s}\n", .{@errorName(err)});
        return 1;
    };
    defer output_dir.close();

    var published_names: [16][64]u8 = undefined;
    var published_lengths: [16]usize = undefined;
    var published_count: usize = 0;
    var complete = false;
    defer if (!complete) {
        var rollback_failed = false;
        for (0..published_count) |index| {
            output_dir.deleteFile(published_names[index][0..published_lengths[index]]) catch {
                rollback_failed = true;
            };
        }
        if (rollback_failed) stderr.writeAll("error: rollback-incomplete\n") catch {};
    };

    for (level_plan[0..options.levels], 1..) |dims, level| {
        const names = try outputNames(allocator, dims, level);
        defer allocator.free(names.final);
        defer allocator.free(names.temp);
        const final_exists = entryExists(output_dir, names.final) catch |err| {
            try stderr.print("error: output-directory: {s}\n", .{@errorName(err)});
            return 1;
        };
        const temp_exists = entryExists(output_dir, names.temp) catch |err| {
            try stderr.print("error: output-directory: {s}\n", .{@errorName(err)});
            return 1;
        };
        if (final_exists or temp_exists) {
            try stderr.writeAll("error: output-collision: OutputCollision\n");
            return 1;
        }
    }

    var current = switch (try codec.decode(allocator, input)) {
        .value => |value| value,
        .failure => |failure| {
            try printCodecError(stderr, "decode", failure);
            return 1;
        },
    };
    defer current.deinit();

    for (level_plan[0..options.levels], 1..) |dims, level| {
        var reduced = try degrade.discardStrips(allocator, current, degrade.axisForLevel(level));
        defer reduced.deinit();
        const encoded = switch (try codec.encode(allocator, reduced)) {
            .value => |value| value,
            .failure => |failure| {
                try printCodecError(stderr, "encode", failure);
                return 1;
            },
        };
        defer allocator.free(encoded);

        const names = try outputNames(allocator, dims, level);
        defer allocator.free(names.final);
        defer allocator.free(names.temp);
        var temp_created = false;
        defer if (temp_created) output_dir.deleteFile(names.temp) catch {};
        var file = output_dir.createFile(names.temp, .{ .exclusive = true }) catch |err| {
            try stderr.print("error: write-output: {s}\n", .{@errorName(err)});
            return 1;
        };
        temp_created = true;
        file.writeAll(encoded) catch |err| {
            file.close();
            try stderr.print("error: write-output: {s}\n", .{@errorName(err)});
            return 1;
        };
        file.close();
        if (published_count >= published_names.len or
            names.final.len > published_names[published_count].len)
        {
            return error.NameTooLong;
        }
        publishWithoutReplace(output_dir, names.temp, names.final) catch |err| {
            if (err == error.PathAlreadyExists) {
                try stderr.writeAll("error: output-collision: OutputCollision\n");
                return 1;
            }
            try stderr.print("error: write-output: {s}\n", .{@errorName(err)});
            return 1;
        };
        @memcpy(published_names[published_count][0..names.final.len], names.final);
        published_lengths[published_count] = names.final.len;
        published_count += 1;
        output_dir.deleteFile(names.temp) catch |err| {
            try stderr.print("error: write-output: {s}\n", .{@errorName(err)});
            return 1;
        };
        temp_created = false;
        const path = try std.fs.path.join(allocator, &.{ options.output_dir, names.final });
        defer allocator.free(path);
        try stdout.print("level={d:0>2} width={d} height={d} path={s}\n", .{ level, dims.width, dims.height, path });

        if (level < options.levels) {
            const next = switch (try codec.decode(allocator, encoded)) {
                .value => |value| value,
                .failure => |failure| {
                    try printCodecError(stderr, "decode", failure);
                    return 1;
                },
            };
            current.deinit();
            current = next;
        }
    }
    complete = true;
    return 0;
}

/// Process entry point: parses argv and exits with `execute`'s status.
pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();
    const args = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, args);
    const status = try execute(allocator, args, std.io.getStdOut().writer(), std.io.getStdErr().writer());
    if (status != 0) std.process.exit(status);
}
