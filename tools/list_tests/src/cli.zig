//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Command-line membrane for `list_tests` (#858), replacing the Python
//! implementation this change deletes.
//!
//! `just/tests.just` runs this once per category through
//! `scripts/builders/list_tests.sh` and shows whatever it prints, so the
//! exit-status contract pinned by `tests/cli_test.zig` is:
//!
//!   0  the listing was printed
//!   1  no category was given (usage line), or the category resolved to no
//!      tests
//!
//! Both statuses and both messages are inherited rather than invented: the
//! Python printed each with `print`, so they went to STDOUT, and both left
//! through `sys.exit(1)`. There is no third status because the Python never
//! had one -- argv beyond the first positional was simply ignored, and this
//! keeps that.
//!
//! `run` takes the directory the search resolves against, the value of
//! `RA8_REPO_ROOT` and both streams, so the whole contract is provable in a
//! temporary directory without spawning a process or touching the real
//! environment. `main` supplies the real cwd, environment and streams.

const std = @import("std");
const listing = @import("internal/root.zig");

pub const usage = "Usage: list_tests <category>";

pub const exit_ok: u8 = 0;
pub const exit_error: u8 = 1;

/// Largest PREFIX of a source file a description is read out of. Far above
/// anything in this tree, and a prefix rather than a ceiling on purpose: the
/// Python streamed the file line by line, so a file past this size still
/// carried its `@brief`. Reading only the head keeps that, because the tag is
/// in the header block or nowhere. A file past the prefix loses nothing a
/// listing shows.
pub const max_source_bytes: usize = 16 * 1024 * 1024;

/// A source file found by one glob, carrying the bytes its description comes
/// from.
const Found = struct {
    file_name: []const u8,
    bytes: []const u8,
};

/// Print one category's test listing, returning the exit status.
///
/// A compiled tool cannot use the Python's `__file__.parents[2]` trick to find
/// the repository, so the root is resolved `--repo-root` > `RA8_REPO_ROOT` >
/// `dir`. The launcher runs from the repository root, so the last of those is
/// the ordinary path.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    argv: []const []const u8,
    repo_root_env: ?[]const u8,
    stdout: anytype,
    stderr: anytype,
) !u8 {
    var repo_root_flag: ?[]const u8 = null;
    var category_arg: ?[]const u8 = null;

    var index: usize = 1;
    while (index < argv.len) : (index += 1) {
        const arg = argv[index];
        if (std.mem.eql(u8, arg, "--repo-root")) {
            if (index + 1 >= argv.len) {
                try stderr.print("list_tests: --repo-root needs a directory\n{s}\n", .{usage});
                return exit_error;
            }
            index += 1;
            repo_root_flag = argv[index];
            continue;
        }
        if (std.mem.startsWith(u8, arg, "--repo-root=")) {
            repo_root_flag = arg["--repo-root=".len..];
            continue;
        }
        // The Python read `sys.argv[1]` and ignored everything after it.
        if (category_arg == null) category_arg = arg;
    }

    const raw_category = category_arg orelse {
        try stdout.print("{s}\n", .{usage});
        return exit_error;
    };

    const category = try listing.asciiLower(allocator, raw_category);
    defer allocator.free(category);

    const root_path = repo_root_flag orelse repo_root_env orelse ".";
    var root = dir.openDir(root_path, .{ .iterate = true }) catch {
        try stderr.print("list_tests: cannot open repository root '{s}'\n", .{root_path});
        return exit_error;
    };
    defer root.close();

    const patterns = try listing.searchPatterns(allocator, category);

    var entries = std.ArrayList(listing.Entry).init(allocator);
    defer entries.deinit();

    for (patterns) |pattern| {
        for (listing.test_suffixes) |suffix| {
            const glob = try std.fmt.allocPrint(
                allocator,
                "{s}/test_*.{s}",
                .{ pattern, suffix },
            );
            defer allocator.free(glob);
            try collect(allocator, root, glob, &entries);
        }
    }

    if (entries.items.len == 0) {
        try stdout.print(
            "Error: Category '{s}' not found or has no tests.\n",
            .{category},
        );
        return exit_error;
    }

    std.mem.sort(listing.Entry, entries.items, {}, listing.lessThanByName);

    try listing.writeHeader(stdout, allocator, category, entries.items.len);
    for (entries.items) |entry| try listing.writeRow(stdout, entry);
    try stdout.writeAll("\n");
    return exit_ok;
}

/// Walk one glob and append an entry per matching source file.
fn collect(
    allocator: std.mem.Allocator,
    root: std.fs.Dir,
    glob: []const u8,
    entries: *std.ArrayList(listing.Entry),
) !void {
    var components = std.ArrayList([]const u8).init(allocator);
    defer components.deinit();
    var parts = std.mem.splitScalar(u8, glob, '/');
    while (parts.next()) |part| {
        if (part.len == 0) continue;
        try components.append(part);
    }
    if (components.items.len == 0) return;

    var found = std.ArrayList(Found).init(allocator);
    defer found.deinit();
    try walk(allocator, root, components.items, &found);

    for (found.items) |file| {
        const name = listing.stem(file.file_name);
        const decoded = try listing.decodeIgnoringInvalid(allocator, file.bytes);
        const description = if (listing.briefIn(decoded)) |brief|
            try allocator.dupe(u8, brief)
        else
            try listing.defaultDescription(allocator, name);
        try entries.append(.{ .name = name, .description = description });
    }
}

fn lessThanName(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.order(u8, left, right) == .lt;
}

/// Read up to `max_source_bytes` from one file, or null when it cannot be
/// opened or read at all.
///
/// A plain `readFileAlloc` fails outright on a file past the cap, which would
/// drop the row; this truncates instead, so only bytes past the prefix are
/// lost and the `@brief` in the header block is still found.
fn readPrefix(allocator: std.mem.Allocator, dir: std.fs.Dir, name: []const u8) ?[]u8 {
    var file = dir.openFile(name, .{}) catch return null;
    defer file.close();
    const size = (file.stat() catch return null).size;
    const wanted: usize = @intCast(@min(size, max_source_bytes));
    const buffer = allocator.alloc(u8, wanted) catch return null;
    const read = file.readAll(buffer) catch return null;
    return buffer[0..read];
}

/// Match `components` against the tree under `dir`, appending every file the
/// last component names.
///
/// Directory listings are sorted before they are descended, so discovery order
/// is a function of the tree rather than of `readdir`. The Python's
/// `Path.glob` returned `os.scandir` order, which is filesystem dependent and
/// therefore only stable by luck; entries are sorted by name afterwards either
/// way, so this changes nothing observable except that two same-named tests now
/// tie deterministically.
fn walk(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    components: []const []const u8,
    found: *std.ArrayList(Found),
) !void {
    const head = components[0];
    const is_last = components.len == 1;

    if (!listing.isWildcard(head)) {
        if (is_last) {
            // No directory listing established this name, so an unopenable
            // file here means the glob matched nothing rather than a source
            // whose description is missing.
            const bytes = readPrefix(allocator, dir, head) orelse return;
            try found.append(.{ .file_name = try allocator.dupe(u8, head), .bytes = bytes });
            return;
        }
        var sub = dir.openDir(head, .{ .iterate = true }) catch return;
        defer sub.close();
        try walk(allocator, sub, components[1..], found);
        return;
    }

    var names = std.ArrayList([]const u8).init(allocator);
    defer names.deinit();
    var iterator = dir.iterate();
    while (try iterator.next()) |entry| {
        if (!listing.componentMatches(head, entry.name)) continue;
        const usable = switch (entry.kind) {
            .directory, .sym_link => true,
            .file => is_last,
            else => false,
        };
        if (!usable) continue;
        try names.append(try allocator.dupe(u8, entry.name));
    }
    std.mem.sort([]const u8, names.items, {}, lessThanName);

    for (names.items) |name| {
        if (is_last) {
            // The row comes from the directory listing, so it is never dropped
            // for a file that cannot be read: an unreadable source loses its
            // description, not its place in the listing. The Python raised on
            // one instead, which failed the whole category.
            const bytes = readPrefix(allocator, dir, name) orelse &[_]u8{};
            try found.append(.{ .file_name = name, .bytes = bytes });
            continue;
        }
        var sub = dir.openDir(name, .{ .iterate = true }) catch continue;
        defer sub.close();
        try walk(allocator, sub, components[1..], found);
    }
}
