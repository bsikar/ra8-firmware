//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Resolve a C23 front end for the reg_gen generated-header contract test.
//!
//! The contract test compiles every generated header with a real C23 compiler.
//! It used to spawn the bare name `clang-18`, which is the compiler the Linux
//! CI image installs. On any other host -- an arm64 macOS workstation in
//! particular (RA8FW-330) -- that name does not exist, so std.process.Child.spawn
//! failed with FileNotFound and the whole reg_gen `zig build test` step could
//! not run off the CI image.
//!
//! Resolution is ordered, not permissive. `clang-18` stays first, so a host
//! that has it behaves exactly as it did before. The search only widens when
//! that name is absent, every candidate has to prove it accepts `-std=c23`
//! before it is used, and a host with no C23 front end at all is still a hard
//! failure rather than a skipped test.

const std = @import("std");

/// Flags appended to a candidate to decide whether it is usable, and also the
/// flags the contract test itself compiles the generated headers with.
pub const compile_args = [_][]const u8{ "-std=c23", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", "-x", "c-header", "-" };

/// A minimal C23 translation unit: bare `static_assert` with no <assert.h> is
/// a C23 keyword, so a C17 front end rejects this and fails the probe.
pub const probe_source =
    \\#include <stdint.h>
    \\#include <stddef.h>
    \\static_assert(sizeof(uint32_t) == 4U, "c23 keyword static_assert");
    \\
;

/// Candidate names tried in order when nothing is pinned. clang-18 first keeps
/// the pinned Linux CI compiler authoritative wherever it exists.
pub const default_names = [_][]const u8{ "clang-18", "clang-19", "clang-20", "clang", "cc" };

pub const Options = struct {
    /// Raw RA8_C23_CC value. When it is set and non-blank it is the ONLY
    /// candidate: an explicit pin that does not work has to fail loudly
    /// instead of silently falling through to some other compiler.
    override: ?[]const u8 = null,
    /// Absolute path of the zig running this build, passed down as a build
    /// option. `zig cc` is a clang 19 front end that ships its own headers, so
    /// it is the last-resort candidate that exists on every machine able to
    /// build this root at all.
    zig_exe: ?[]const u8 = null,
};

/// Split a pinned command line on ASCII whitespace, so RA8_C23_CC can carry
/// arguments ("xcrun clang", "ccache clang-18").
pub fn splitCommand(allocator: std.mem.Allocator, raw: []const u8) ![]const []const u8 {
    var words: std.ArrayList([]const u8) = .empty;
    errdefer words.deinit(allocator);
    var it = std.mem.tokenizeAny(u8, raw, " \t\r\n");
    while (it.next()) |word| try words.append(allocator, word);
    return words.toOwnedSlice(allocator);
}

/// The ordered argv prefixes to probe. Pure: it spawns nothing, so the
/// ordering is unit-testable without a compiler on the machine.
pub fn candidates(allocator: std.mem.Allocator, options: Options) ![]const []const []const u8 {
    var list: std.ArrayList([]const []const u8) = .empty;
    errdefer list.deinit(allocator);

    if (options.override) |raw| {
        const argv = try splitCommand(allocator, raw);
        if (argv.len != 0) {
            try list.append(allocator, argv);
            return list.toOwnedSlice(allocator);
        }
    }

    for (default_names) |name| {
        const argv = try allocator.alloc([]const u8, 1);
        argv[0] = name;
        try list.append(allocator, argv);
    }

    if (options.zig_exe) |exe| {
        if (exe.len != 0) {
            const argv = try allocator.alloc([]const u8, 2);
            argv[0] = exe;
            argv[1] = "cc";
            try list.append(allocator, argv);
        }
    }

    return list.toOwnedSlice(allocator);
}

/// Feed `source` to `argv_prefix ++ compile_args` on stdin and report whether
/// it exited 0. A candidate that cannot even be spawned is simply not there.
pub fn compiles(allocator: std.mem.Allocator, io: std.Io, argv_prefix: []const []const u8, source: []const u8) !bool {
    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(allocator);
    try argv.appendSlice(allocator, argv_prefix);
    try argv.appendSlice(allocator, &compile_args);

    var child = std.process.spawn(io, .{
        .argv = argv.items,
        .stdin = .pipe,
        .stdout = .ignore,
        .stderr = .inherit,
    }) catch return false;

    child.stdin.?.writeStreamingAll(io, source) catch {
        child.stdin.?.close(io);
        child.stdin = null;
        _ = child.wait(io) catch {};
        return false;
    };
    child.stdin.?.close(io);
    child.stdin = null;

    // On Linux the spawn is deferred, so a candidate that does not exist on
    // this host reports FileNotFound out of wait() rather than out of spawn().
    // Either way it is simply not a usable compiler, never a test failure.
    const term = child.wait(io) catch return false;
    return switch (term) {
        .exited => |code| code == 0,
        else => false,
    };
}

/// First candidate that accepts the C23 probe, or error.NoC23CompilerAvailable
/// with every name tried printed so the failure is actionable.
pub fn resolve(allocator: std.mem.Allocator, io: std.Io, options: Options) ![]const []const u8 {
    const list = try candidates(allocator, options);
    for (list) |argv| {
        if (try compiles(allocator, io, argv, probe_source)) return argv;
    }
    std.debug.print("reg_gen: no C23 front end found. Tried:\n", .{});
    for (list) |argv| {
        std.debug.print("  ", .{});
        for (argv) |word| std.debug.print("{s} ", .{word});
        std.debug.print("\n", .{});
    }
    std.debug.print("Set RA8_C23_CC to pin one explicitly.\n", .{});
    return error.NoC23CompilerAvailable;
}
