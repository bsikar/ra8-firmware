//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The argv membrane and exit-status contract for the example board-pin gate
//! (#858).  Everything that touches argv, the tree or the two output streams
//! lives here; `internal/root.zig` holds the decisions.  `run` is
//! parameterised on the repository directory, the repository root path and
//! both writers, so the tests drive the whole contract without a process.
//!
//! Exit status, inherited from the Python predecessor unchanged:
//!   0  no example hand-encodes a board pin, or argv filtered to nothing,
//!      or `--selftest` held in both directions
//!   1  at least one hand-encoded pin, or a failing selftest assertion
//!   2  the whole-tree sweep enumerated fewer than `file_floor` files
//!
//! There is no usage status: the predecessor read argv only to look for
//! `--selftest` and treated everything else as a path list, so an unknown flag
//! is a path that does not exist and is scanned as nothing.

const std = @import("std");
const implementation = @import("internal/root.zig");

pub const file_floor = implementation.file_floor;
pub const tool_name = implementation.tool_name;

/// One enumerated scan target.  `display` is what the report prints (the
/// predecessor's `_rel`: repo-relative when the path is inside the tree,
/// otherwise the path as given); `path` is what actually gets opened.
pub const Target = struct {
    display: []const u8,
    path: []const u8,
    absolute: bool,
};

fn isAbsolute(path: []const u8) bool {
    return path.len > 0 and path[0] == '/';
}

/// `pathlib.Path(raw)`: collapse repeated slashes and drop `.` components,
/// keeping `..` exactly as written.
fn pathlibNormalize(allocator: std.mem.Allocator, raw: []const u8) ![]const u8 {
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    if (isAbsolute(raw)) try out.append('/');
    var parts = std.mem.splitScalar(u8, raw, '/');
    var wrote = false;
    while (parts.next()) |part| {
        if (part.len == 0 or std.mem.eql(u8, part, ".")) continue;
        if (wrote) try out.append('/');
        try out.appendSlice(part);
        wrote = true;
    }
    if (!wrote and !isAbsolute(raw)) try out.appendSlice(".");
    return out.toOwnedSlice();
}

fn joinRepo(allocator: std.mem.Allocator, repo_root: []const u8, relative: []const u8) ![]const u8 {
    return std.fmt.allocPrint(allocator, "{s}/{s}", .{ repo_root, relative });
}

fn baseName(path: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, path, '/')) |slash| return path[slash + 1 ..];
    return path;
}

const Collector = struct {
    allocator: std.mem.Allocator,
    buckets: [implementation.source_suffixes.len]std.ArrayList([]const u8),

    fn init(allocator: std.mem.Allocator) Collector {
        var self: Collector = .{ .allocator = allocator, .buckets = undefined };
        for (&self.buckets) |*bucket| bucket.* = std.ArrayList([]const u8).init(allocator);
        return self;
    }

    fn offer(self: *Collector, relative: []const u8) !void {
        for (implementation.source_suffixes, 0..) |suffix, index| {
            if (implementation.globMatchesSuffix(baseName(relative), suffix)) {
                try self.buckets[index].append(relative);
                return;
            }
        }
    }

    /// The four `rglob("*" ++ suffix)` passes concatenated in suffix order,
    /// which is the order the predecessor's loop produced.
    fn flatten(self: *Collector) !std.ArrayList([]const u8) {
        var out = std.ArrayList([]const u8).init(self.allocator);
        for (&self.buckets) |*bucket| {
            try out.appendSlice(bucket.items);
            bucket.deinit();
        }
        return out;
    }
};

/// Depth-first pre-order walk of `relative_root` under `dir`, entries sorted
/// per directory.  The predecessor inherited `scandir` order here, which is
/// filesystem-dependent; sorting is the one deliberate behaviour change and it
/// only makes the report's order reproducible.  Symlinked directories are not
/// descended into, matching `pathlib`'s recursive glob.
fn walkTree(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    relative_root: []const u8,
    collector: *Collector,
) !void {
    var handle = dir.openDir(relative_root, .{ .iterate = true }) catch return;
    defer handle.close();

    var names = std.ArrayList(std.fs.Dir.Entry).init(allocator);
    defer names.deinit();
    var iterator = handle.iterate();
    while (try iterator.next()) |entry| {
        try names.append(.{
            .name = try allocator.dupe(u8, entry.name),
            .kind = entry.kind,
        });
    }
    std.mem.sort(std.fs.Dir.Entry, names.items, {}, struct {
        fn lessThan(_: void, a: std.fs.Dir.Entry, b: std.fs.Dir.Entry) bool {
            return std.mem.order(u8, a.name, b.name) == .lt;
        }
    }.lessThan);

    for (names.items) |entry| {
        const relative = try std.fmt.allocPrint(allocator, "{s}/{s}", .{ relative_root, entry.name });
        try collector.offer(relative);
        if (entry.kind == .directory) try walkTree(allocator, dir, relative, collector);
    }
}

/// `_enumerate_targets`: the argv list when there is one, the `examples/`
/// sweep when there is not, with build output filtered out of both.
pub fn enumerateTargets(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    paths: []const []const u8,
) !std.ArrayList(Target) {
    var targets = std.ArrayList(Target).init(allocator);
    errdefer targets.deinit();

    if (paths.len == 0) {
        var collector = Collector.init(allocator);
        try walkTree(allocator, dir, implementation.scan_root, &collector);
        var flat = try collector.flatten();
        defer flat.deinit();
        for (flat.items) |relative| {
            const absolute = try joinRepo(allocator, repo_root, relative);
            if (implementation.isBuildOutputPath(absolute, repo_root)) continue;
            try targets.append(.{ .display = relative, .path = relative, .absolute = false });
        }
        return targets;
    }

    for (paths) |raw| {
        const normalized = try pathlibNormalize(allocator, raw);
        const absolute_path = isAbsolute(normalized);
        // A relative argument is resolved against the repository root, never
        // against the caller's working directory.
        const open_path = normalized;
        const probe = if (absolute_path) normalized else normalized;

        var is_directory = false;
        if (absolute_path) {
            if (std.fs.cwd().statFile(probe)) |stat| {
                is_directory = stat.kind == .directory;
            } else |_| {}
        } else {
            if (dir.statFile(probe)) |stat| {
                is_directory = stat.kind == .directory;
            } else |_| {}
        }

        if (is_directory) {
            var collector = Collector.init(allocator);
            if (absolute_path) {
                var opened = std.fs.cwd().openDir(probe, .{}) catch continue;
                opened.close();
                try walkTree(allocator, std.fs.cwd(), probe, &collector);
            } else {
                try walkTree(allocator, dir, probe, &collector);
            }
            var flat = try collector.flatten();
            defer flat.deinit();
            for (flat.items) |relative| {
                const full = if (absolute_path)
                    relative
                else
                    try joinRepo(allocator, repo_root, relative);
                if (implementation.isBuildOutputPath(full, repo_root)) continue;
                try targets.append(.{
                    .display = if (absolute_path) relative else relative,
                    .path = relative,
                    .absolute = absolute_path,
                });
            }
            continue;
        }

        if (!implementation.isSourceName(baseName(normalized))) continue;
        const full = if (absolute_path)
            normalized
        else
            try joinRepo(allocator, repo_root, normalized);
        if (implementation.isBuildOutputPath(full, repo_root)) continue;
        var display = normalized;
        if (absolute_path and std.mem.startsWith(u8, normalized, repo_root) and
            normalized.len > repo_root.len and normalized[repo_root.len] == '/')
        {
            display = normalized[repo_root.len + 1 ..];
        }
        try targets.append(.{ .display = display, .path = open_path, .absolute = absolute_path });
    }
    return targets;
}

/// `path.read_text(...)` with the predecessor's `except OSError: continue`.
/// Deliberately uncapped: the predecessor's `read()` had no ceiling, and a
/// ceiling here would be a fail-open, since a source above it would be
/// skipped silently while still counting toward the scanned total, i.e. a
/// file nobody read reported as carrying no hand-encoded pin.  The read is
/// bounded by the file itself.
fn readTarget(allocator: std.mem.Allocator, dir: std.fs.Dir, target: Target) ?[]u8 {
    var file = if (target.absolute)
        std.fs.cwd().openFile(target.path, .{}) catch return null
    else
        dir.openFile(target.path, .{}) catch return null;
    defer file.close();
    const raw = file.readToEndAlloc(allocator, std.math.maxInt(usize)) catch return null;
    return implementation.decodeLossy(allocator, raw) catch null;
}

fn expect(writer: anytype, condition: bool, label: []const u8, failures: *std.ArrayList([]const u8)) !void {
    try writer.print("  [{s}] {s}\n", .{ if (condition) "ok" else "FAIL", label });
    if (!condition) try failures.append(label);
}

/// `selftest()`: the gate proving itself in both directions before a clean run
/// is allowed to mean anything.  The matcher must FIRE on the idiom and stay
/// QUIET on a board-symbol reference, the enumeration must DROP an in-source
/// build file while KEEPING a real example source, and the live sweep must
/// clear the floor with nothing from a build tree in it.
pub fn selftest(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    stdout: anytype,
    stderr: anytype,
) !u8 {
    var failures = std.ArrayList([]const u8).init(allocator);
    defer failures.deinit();

    try expect(
        stdout,
        implementation.matchEncoding(implementation.selftest_idiom),
        "MUST FIRE: hand-encoded (port << 8) | pin",
        &failures,
    );
    try expect(
        stdout,
        !implementation.matchEncoding(implementation.selftest_board_reference),
        "MUST NOT FIRE: a board-symbol reference",
        &failures,
    );

    var enumerated = try enumerateTargets(allocator, dir, repo_root, &[_][]const u8{
        "examples/x/build/gen.c",
        "examples/x/src/main.c",
    });
    defer enumerated.deinit();
    var saw_source = false;
    var saw_build = false;
    for (enumerated.items) |target| {
        if (std.mem.endsWith(u8, target.display, "examples/x/src/main.c")) saw_source = true;
        if (std.mem.endsWith(u8, target.display, "examples/x/build/gen.c")) saw_build = true;
    }
    try expect(stdout, saw_source, "MUST FIRE: a real example source is enumerated", &failures);
    try expect(
        stdout,
        !saw_build,
        "MUST NOT FIRE: an in-source build file is excluded from the scope",
        &failures,
    );

    var live = try enumerateTargets(allocator, dir, repo_root, &[_][]const u8{});
    defer live.deinit();
    const floor_label = try std.fmt.allocPrint(
        allocator,
        "live sweep sees {d} example file(s) (floor {d})",
        .{ live.items.len, file_floor },
    );
    try expect(stdout, live.items.len >= file_floor, floor_label, &failures);
    var all_source = true;
    for (live.items) |target| {
        if (implementation.isBuildOutput(target.display)) all_source = false;
    }
    try expect(stdout, all_source, "no enumerated file lives in a build tree", &failures);

    if (failures.items.len != 0) {
        try stderr.print("\nSELFTEST FAILED: {d} assertion(s)\n", .{failures.items.len});
        for (failures.items) |item| try stderr.print("  {s}\n", .{item});
        return 1;
    }
    try stdout.writeAll("selftest: all assertions held (both directions).\n");
    return 0;
}

/// The whole gate: argv in, exit status out.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    argv: []const []const u8,
    stdout: anytype,
    stderr: anytype,
) !u8 {
    for (argv) |argument| {
        if (std.mem.eql(u8, argument, "--selftest"))
            return selftest(allocator, dir, repo_root, stdout, stderr);
    }

    var targets = try enumerateTargets(allocator, dir, repo_root, argv);
    defer targets.deinit();

    if (argv.len == 0 and targets.items.len < file_floor) {
        try implementation.renderFatalFloor(stderr, targets.items.len);
        return 2;
    }
    if (targets.items.len == 0) {
        try implementation.renderNoFiles(stderr);
        return 0;
    }

    var findings = std.ArrayList(implementation.Finding).init(allocator);
    defer findings.deinit();
    for (targets.items) |target| {
        const text = readTarget(allocator, dir, target) orelse continue;
        var found = try implementation.scanText(allocator, target.display, text);
        defer found.deinit();
        try findings.appendSlice(found.items);
    }

    if (findings.items.len == 0) {
        try implementation.renderClean(stdout, targets.items.len);
        return 0;
    }
    try implementation.renderFindingHeader(stderr, findings.items.len);
    for (findings.items) |finding| try implementation.renderFinding(stderr, finding);
    try implementation.renderGuidance(stderr);
    return 1;
}
