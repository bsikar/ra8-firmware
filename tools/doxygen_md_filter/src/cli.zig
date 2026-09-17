//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Command-line membrane for `doxygen_md_filter` (#858), replacing the Python
//! implementation this change deletes.
//!
//! Doxygen runs the filter once per Markdown input file, hands it the file
//! path and reads the filtered page from stdout, so the exit-status contract
//! pinned by `tests/cli_test.zig` is:
//!
//!   0  the filtered page was written to stdout
//!   1  the page could not be read, or the repository root could not be opened
//!   2  wrong arguments (usage error)
//!
//! The repository root decides which link targets exist. It comes from
//! `--repo-root`, else the `RA8_REPO_ROOT` the docs build exports, else the
//! current directory; the Python tool derived it from its own source location,
//! which a compiled tool in the build tree cannot do. A page outside that root
//! keeps the old behaviour: its links resolve as if it sat at the root, so in
//! practice only badge stripping applies to it.
//!
//! Every page is emitted with LF terminators, which is what the Python
//! tool's universal-newline reads produced; see `normalizeTerminators`.
//!
//! One inherited behaviour is deliberately NOT reproduced: the Python tool
//! decoded its input as UTF-8, so a page holding an invalid byte sequence
//! raised and failed the whole docs build. Pages here are filtered as bytes,
//! so such a page passes through and the site still builds. Every tracked
//! page is valid UTF-8 today, and failing a docs build over one stray byte in
//! a page nobody is editing is the worse of the two outcomes.
//!
//! `run` takes the directory paths resolve against and both output streams, so
//! the contract is provable in a temporary directory without spawning a
//! process. `main` supplies the real cwd, environment, stdout and stderr.

const std = @import("std");
const filter = @import("internal/root.zig");

pub const usage = "usage: doxygen_md_filter [--repo-root <dir>] <file.md>";

pub const exit_ok: u8 = 0;
pub const exit_error: u8 = 1;
pub const exit_usage: u8 = 2;

/// Largest page accepted, 16 MiB: orders above the longest document in the
/// tree, and small enough that a wrong argument cannot exhaust the docs
/// builder's memory.
const max_page_bytes = 16 * 1024 * 1024;

/// Rewrite CRLF and lone CR terminators to LF, or return null when the page
/// already holds none.
///
/// The Python tool read its input through CPython's universal-newline
/// translation, so every page it emitted carried LF terminators whatever the
/// file held. One page in the tree is affected (a vendored ThreadX document
/// written with CRLF), and normalising here keeps this swap byte-identical on
/// it rather than reflowing a page as a side effect of a tooling change. The
/// transforms themselves are terminator-agnostic; this is inherited output
/// behaviour, pinned deliberately.
fn normalizeTerminators(allocator: std.mem.Allocator, text: []const u8) !?[]u8 {
    if (std.mem.indexOfScalar(u8, text, '\r') == null) return null;

    var out = try std.ArrayList(u8).initCapacity(allocator, text.len);
    errdefer out.deinit();
    var index: usize = 0;
    while (index < text.len) : (index += 1) {
        if (text[index] != '\r') {
            out.appendAssumeCapacity(text[index]);
            continue;
        }
        out.appendAssumeCapacity('\n');
        if (index + 1 < text.len and text[index + 1] == '\n') index += 1;
    }
    return try out.toOwnedSlice();
}

/// Existence of link targets, answered against one open directory.
const DirResolver = struct {
    dir: std.fs.Dir,

    fn isFile(context: *const anyopaque, path: []const u8) bool {
        const self: *const DirResolver = @ptrCast(@alignCast(context));
        const stat = self.dir.statFile(path) catch return false;
        return stat.kind == .file;
    }

    fn resolver(self: *const DirResolver) filter.Resolver {
        return .{ .context = self, .isFileFn = DirResolver.isFile };
    }
};

/// The repo-relative directory of `source`, or empty when it is outside
/// `root`. Owned by the caller.
///
/// Both paths are resolved first, so a page reached through a symlink or a
/// relative path is attributed to the same directory either way. A page the
/// root does not contain yields the empty directory, which is the root: its
/// badges are still stripped, and a link of its own resolves only if the same
/// path happens to exist in the repository.
fn sourceDirectory(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    root: []const u8,
    source: []const u8,
) ![]u8 {
    const root_real = dir.realpathAlloc(allocator, root) catch return allocator.dupe(u8, "");
    defer allocator.free(root_real);
    const source_real = dir.realpathAlloc(allocator, source) catch return allocator.dupe(u8, "");
    defer allocator.free(source_real);

    if (!std.mem.startsWith(u8, source_real, root_real)) return allocator.dupe(u8, "");
    if (source_real.len <= root_real.len or source_real[root_real.len] != '/') {
        return allocator.dupe(u8, "");
    }
    const relative = source_real[root_real.len + 1 ..];
    return allocator.dupe(u8, std.fs.path.dirname(relative) orelse "");
}

/// Filter one Markdown page to `stdout`, returning the exit status.
///
/// `environment_root` is the repository root the environment names, or null
/// when it names none; an explicit `--repo-root` outranks it.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    argv: []const []const u8,
    environment_root: ?[]const u8,
    stdout: anytype,
    stderr: anytype,
) !u8 {
    var repo_root: ?[]const u8 = null;
    var source: ?[]const u8 = null;

    var index: usize = 1;
    while (index < argv.len) : (index += 1) {
        const argument = argv[index];
        if (std.mem.eql(u8, argument, "--repo-root")) {
            index += 1;
            if (index >= argv.len) {
                try stderr.print("{s}\n", .{usage});
                return exit_usage;
            }
            repo_root = argv[index];
        } else if (std.mem.startsWith(u8, argument, "--") or source != null) {
            try stderr.print("{s}\n", .{usage});
            return exit_usage;
        } else {
            source = argument;
        }
    }

    const source_name = source orelse {
        try stderr.print("{s}\n", .{usage});
        return exit_usage;
    };
    const root = repo_root orelse environment_root orelse ".";

    const raw = dir.readFileAlloc(allocator, source_name, max_page_bytes) catch |err| {
        try stderr.print("doxygen_md_filter: {s}: {s}\n", .{ source_name, @errorName(err) });
        return exit_error;
    };
    defer allocator.free(raw);

    const normalized = try normalizeTerminators(allocator, raw);
    defer if (normalized) |value| allocator.free(value);
    const text = normalized orelse raw;

    var root_dir = dir.openDir(root, .{}) catch |err| {
        try stderr.print("doxygen_md_filter: {s}: {s}\n", .{ root, @errorName(err) });
        return exit_error;
    };
    defer root_dir.close();

    const source_dir = try sourceDirectory(allocator, dir, root, source_name);
    defer allocator.free(source_dir);
    const dir_resolver = DirResolver{ .dir = root_dir };

    const resolver = dir_resolver.resolver();
    const filtered = try filter.filterMarkdown(allocator, text, source_dir, resolver);
    defer allocator.free(filtered);

    try stdout.writeAll(filtered);
    return exit_ok;
}
