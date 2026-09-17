//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The argv membrane, the tree walk and the exit contract of the cross-build
//! shard-union gate (#858, #1159).
//!
//! `run` is parameterised on the directory paths are resolved against, on the
//! scratch directory the selftest materialises its fake trees in, on the
//! default repository root and on both output streams, so every status below
//! is provable in a test with no process, no real repository and no CI.
//!
//! EXIT CONTRACT, inherited from the Python this replaced:
//!
//!   0  the shards covered every discovered configuration exactly once, or
//!      `--selftest` found every case behaving
//!   1  any discrepancy (a missing shard manifest, an unbuilt configuration,
//!      one claimed twice, a disagreeing `all-configs.txt`, an absent shard
//!      directory, an empty discovery), a failing selftest case, or a
//!      manifest that cannot be read or is not ASCII
//!   2  a usage error: `--shards` absent, not an integer, or below 1, an
//!      unknown or ambiguous option, or a stray positional argument
//!
//! An empty discovery is status 1 and never 0: a gate with no truth to
//! compare against must not report a clean union.

const std = @import("std");
const implementation = @import("internal/root.zig");

pub const Shard = implementation.Shard;

/// Largest manifest read. Orders of magnitude above the real files; it exists
/// so a corrupt path cannot ask for an unbounded allocation.
pub const max_manifest_bytes: usize = 16 * 1024 * 1024;

pub const usage =
    "usage: " ++ implementation.tool_name ++
    " [-h] [--shards SHARDS] [--repo-root REPO_ROOT] [--selftest]\n";

const help_text = usage ++
    "\nGate: cross-build shards covered every firmware configuration exactly once.\n" ++
    "\noptions:\n" ++
    "  -h, --help             show this help message and exit\n" ++
    "  --shards SHARDS        how many shards were scheduled (must match the manifest names)\n" ++
    "  --repo-root REPO_ROOT  repository root to check (default: $RA8_REPO_ROOT, else the working directory)\n" ++
    "  --selftest             prove the checker still detects violations\n";

const long_options = [_][]const u8{ "--help", "--shards", "--repo-root", "--selftest" };

/// What the command line asked for.
pub const Options = struct {
    shards: ?i64 = null,
    repo_root: ?[]const u8 = null,
    selftest: bool = false,
};

/// The three shapes a command line can take.
pub const Parsed = union(enum) {
    options: Options,
    help,
    /// A usage error, carrying the message argparse would have printed after
    /// its usage line.
    usage_error: []const u8,
};

/// `int(text)` as Python accepts it: surrounding whitespace, an optional
/// sign, and single underscores between digits.
pub fn pythonInt(text: []const u8) ?i64 {
    var body = text;
    while (body.len > 0 and implementation.isPythonSpace(body[0])) body = body[1..];
    while (body.len > 0 and implementation.isPythonSpace(body[body.len - 1])) body = body[0 .. body.len - 1];
    if (body.len == 0) return null;

    var negative = false;
    if (body[0] == '+' or body[0] == '-') {
        negative = body[0] == '-';
        body = body[1..];
    }
    if (body.len == 0) return null;

    var value: i64 = 0;
    var previous_digit = false;
    for (body, 0..) |byte, index| {
        if (byte == '_') {
            if (!previous_digit or index + 1 == body.len) return null;
            previous_digit = false;
            continue;
        }
        if (byte < '0' or byte > '9') return null;
        value = std.math.mul(i64, value, 10) catch return null;
        value = std.math.add(i64, value, @as(i64, byte - '0')) catch return null;
        previous_digit = true;
    }
    if (!previous_digit) return null;
    return if (negative) -value else value;
}

/// Resolve one `--long` token against the option names, the way argparse
/// accepts any unambiguous prefix.
fn resolveLong(token: []const u8) ?[]const u8 {
    var match: ?[]const u8 = null;
    var hits: usize = 0;
    for (long_options) |name| {
        if (std.mem.startsWith(u8, name, token)) {
            if (std.mem.eql(u8, name, token)) return name;
            match = name;
            hits += 1;
        }
    }
    return if (hits == 1) match else null;
}

fn ambiguous(token: []const u8) bool {
    var hits: usize = 0;
    for (long_options) |name| {
        if (std.mem.startsWith(u8, name, token)) hits += 1;
    }
    return hits > 1;
}

/// Parse the command line. Values may be attached (`--shards=3`) or separate.
pub fn parseArgs(allocator: std.mem.Allocator, argv: []const []const u8) std.mem.Allocator.Error!Parsed {
    var options = Options{};
    var index: usize = 0;
    while (index < argv.len) : (index += 1) {
        const argument = argv[index];
        if (std.mem.eql(u8, argument, "-h")) return .help;
        if (!std.mem.startsWith(u8, argument, "--")) {
            return .{ .usage_error = try std.fmt.allocPrint(
                allocator,
                "unrecognized arguments: {s}",
                .{argument},
            ) };
        }

        var token = argument;
        var attached: ?[]const u8 = null;
        if (std.mem.indexOfScalar(u8, argument, '=')) |at| {
            token = argument[0..at];
            attached = argument[at + 1 ..];
        }

        const name = resolveLong(token) orelse {
            if (ambiguous(token)) {
                return .{ .usage_error = try std.fmt.allocPrint(
                    allocator,
                    "ambiguous option: {s} could match --shards, --selftest",
                    .{token},
                ) };
            }
            return .{ .usage_error = try std.fmt.allocPrint(
                allocator,
                "unrecognized arguments: {s}",
                .{argument},
            ) };
        };

        if (std.mem.eql(u8, name, "--help")) return .help;
        if (std.mem.eql(u8, name, "--selftest")) {
            if (attached != null) {
                return .{ .usage_error = try std.fmt.allocPrint(
                    allocator,
                    "argument --selftest: ignored explicit argument '{s}'",
                    .{attached.?},
                ) };
            }
            options.selftest = true;
            continue;
        }

        const value = attached orelse blk: {
            if (index + 1 >= argv.len) {
                return .{ .usage_error = try std.fmt.allocPrint(
                    allocator,
                    "argument {s}: expected one argument",
                    .{name},
                ) };
            }
            index += 1;
            break :blk argv[index];
        };

        if (std.mem.eql(u8, name, "--shards")) {
            options.shards = pythonInt(value) orelse {
                return .{ .usage_error = try std.fmt.allocPrint(
                    allocator,
                    "argument --shards: invalid int value: '{s}'",
                    .{value},
                ) };
            };
        } else {
            options.repo_root = value;
        }
    }
    return .{ .options = options };
}

/// One newline-delimited manifest, or an empty list when the path is not a
/// regular file (the Python's `is_file()` guard).
fn readManifest(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    path: []const u8,
) !?[][]const u8 {
    const stat = dir.statFile(path) catch return null;
    if (stat.kind != .file) return null;
    const text = try dir.readFileAlloc(allocator, path, max_manifest_bytes);
    return try implementation.parseManifest(allocator, text);
}

fn isDirectory(dir: std.fs.Dir, path: []const u8) bool {
    const stat = dir.statFile(path) catch return false;
    return stat.kind == .directory;
}

fn isRegularFile(dir: std.fs.Dir, path: []const u8) bool {
    const stat = dir.statFile(path) catch return false;
    return stat.kind == .file;
}

fn collectFrom(
    allocator: std.mem.Allocator,
    root: std.fs.Dir,
    subdirectory: []const u8,
    board: bool,
    set: *std.StringHashMap(void),
) !void {
    var tree = root.openDir(subdirectory, .{ .iterate = true }) catch |err| switch (err) {
        error.FileNotFound, error.NotDir => return,
        else => return err,
    };
    defer tree.close();

    var walker = try tree.walk(allocator);
    defer walker.deinit();
    while (try walker.next()) |entry| {
        if (!std.mem.eql(u8, entry.basename, "main.c")) continue;
        const parent = std.fs.path.dirname(entry.path) orelse continue;
        if (!std.mem.eql(u8, std.fs.path.basename(parent), "src")) continue;
        const app_relative = std.fs.path.dirname(parent) orelse ".";

        const manifest = try std.fs.path.join(allocator, &.{ app_relative, "CMakeLists.txt" });
        defer allocator.free(manifest);
        if (!isRegularFile(tree, manifest)) continue;

        if (board) {
            const relative: []const u8 = if (std.mem.eql(u8, app_relative, ".")) "" else app_relative;
            const identifier = try implementation.boardConfig(allocator, relative);
            if (set.contains(identifier)) {
                allocator.free(identifier);
            } else {
                try set.put(identifier, {});
            }
            if (implementation.requiresNsXip(relative)) {
                const variant = try std.fmt.allocPrint(allocator, "{s}{s}{s}", .{
                    implementation.board_prefix,
                    implementation.ereader_name,
                    implementation.ns_xip_suffix,
                });
                if (set.contains(variant)) {
                    allocator.free(variant);
                } else {
                    try set.put(variant, {});
                }
            }
            continue;
        }

        const parts = try implementation.splitParts(allocator, app_relative);
        defer allocator.free(parts);
        if (!implementation.isExampleSelected(parts)) continue;
        const identifier = try implementation.exampleConfig(allocator, parts);
        if (set.contains(identifier)) {
            allocator.free(identifier);
        } else {
            try set.put(identifier, {});
        }
    }
}

/// Independently re-derive every required firmware build configuration, the
/// way `discover_apps` did: a structural walk of examples/ and of the
/// stand-alone board products, never the execution enumerator and never the
/// `all-configs.txt` a shard wrote.
pub fn discoverApps(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
) ![][]const u8 {
    var root = dir.openDir(repo_root, .{}) catch |err| switch (err) {
        error.FileNotFound, error.NotDir => return allocator.alloc([]const u8, 0),
        else => return err,
    };
    defer root.close();

    var set = std.StringHashMap(void).init(allocator);
    defer set.deinit();
    try collectFrom(allocator, root, "examples", false, &set);
    try collectFrom(allocator, root, "apps/board/stand_alone", true, &set);

    var names = std.ArrayList([]const u8).init(allocator);
    errdefer names.deinit();
    var it = set.keyIterator();
    while (it.next()) |key| try names.append(key.*);
    const owned = try names.toOwnedSlice();
    implementation.sortNames(owned);
    return owned;
}

/// The verdict of one union check.
pub const Verdict = struct {
    status: u8,
    problems: [][]const u8,
};

/// `check_union`: compare the union of the shard manifests against a fresh
/// discovery. Prints the clean line itself, exactly where the Python printed
/// it, so a passing selftest case reports in the same order.
pub fn checkUnion(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    shards: usize,
    out: anytype,
) !Verdict {
    var problems = std.ArrayList([]const u8).init(allocator);
    errdefer problems.deinit();

    const expected = try discoverApps(allocator, dir, repo_root);
    if (expected.len == 0) {
        try problems.append(try allocator.dupe(u8, implementation.empty_discovery_problem));
        return .{ .status = implementation.rc_violation, .problems = try problems.toOwnedSlice() };
    }

    const shard_dir = try implementation.joinPath(allocator, repo_root, implementation.shard_subdir);
    if (!isDirectory(dir, shard_dir)) {
        try problems.append(try implementation.renderMissingShardDir(allocator, shard_dir));
        return .{ .status = implementation.rc_violation, .problems = try problems.toOwnedSlice() };
    }

    const all_configs = try implementation.joinPath(allocator, shard_dir, implementation.all_configs_name);
    if (!isRegularFile(dir, all_configs)) {
        try problems.append(try implementation.renderMissingAllConfigs(allocator, all_configs));
    } else {
        // A read that finds no regular file is an EMPTY matrix, never a
        // skipped comparison: `read_manifest` returned `[]` there and `[] !=
        // expected` reported the disagreement. The file is stat-ed and then
        // read, so it can stop being a regular file in between, and silently
        // dropping the comparison on that path would let the one artefact
        // this gate cross-checks vanish and still report a clean union.
        const claimed = (try readManifest(allocator, dir, all_configs)) orelse &[_][]const u8{};
        if (!implementation.sameList(claimed, expected)) {
            try problems.append(try allocator.dupe(u8, implementation.disagreement_problem));
        }
    }

    var shard_paths = try allocator.alloc([]const u8, shards);
    for (0..shards) |offset| {
        const name = try implementation.shardFileName(allocator, offset + 1, shards);
        shard_paths[offset] = try implementation.joinPath(allocator, shard_dir, name);
        if (!isRegularFile(dir, shard_paths[offset])) {
            try problems.append(try implementation.renderMissingShard(allocator, name));
        }
    }

    if (problems.items.len > 0) {
        return .{ .status = implementation.rc_violation, .problems = try problems.toOwnedSlice() };
    }

    var collected = try allocator.alloc(Shard, shards);
    for (shard_paths, 0..) |path, offset| {
        const apps = (try readManifest(allocator, dir, path)) orelse &[_][]const u8{};
        collected[offset] = .{ .index = offset + 1, .apps = apps };
    }
    try implementation.auditShardContents(allocator, collected, expected, &problems);
    if (problems.items.len > 0) {
        return .{ .status = implementation.rc_violation, .problems = try problems.toOwnedSlice() };
    }

    const line = try implementation.renderCleanLine(allocator, shards, expected.len);
    try out.print("{s}\n", .{line});
    return .{ .status = implementation.rc_ok, .problems = try problems.toOwnedSlice() };
}

const SelftestCase = struct {
    label: []const u8,
    examples: []const []const u8,
    ereader: bool,
    shards: usize,
    slices: []const []const []const u8,
    expect_pass: bool,
};

/// The seven fixtures `_selftest_cases` ran, in order and with its labels.
/// The third and fourth are deliberately the same shape under two names:
/// that is how the Python read, and a selftest is not the place to quietly
/// change what is asserted.
const selftest_cases = [_]SelftestCase{
    .{
        .label = "complete examples plus board variants",
        .examples = &.{ "a", "b" },
        .ereader = true,
        .shards = 2,
        .slices = &.{
            &.{ "board::stand_alone::ra8d2-ereader", "tier::a" },
            &.{ "board::stand_alone::ra8d2-ereader@ns-xip", "tier::b" },
        },
        .expect_pass = true,
    },
    .{
        .label = "complete 1-way",
        .examples = &.{ "a", "b" },
        .ereader = false,
        .shards = 1,
        .slices = &.{&.{ "tier::a", "tier::b" }},
        .expect_pass = true,
    },
    .{
        .label = "a shard built nothing",
        .examples = &.{ "a", "b" },
        .ereader = false,
        .shards = 2,
        .slices = &.{ &.{"tier::a"}, &.{} },
        .expect_pass = false,
    },
    .{
        .label = "an app fell through",
        .examples = &.{ "a", "b" },
        .ereader = false,
        .shards = 2,
        .slices = &.{ &.{"tier::a"}, &.{} },
        .expect_pass = false,
    },
    .{
        .label = "an app built twice",
        .examples = &.{ "a", "b" },
        .ereader = false,
        .shards = 2,
        .slices = &.{ &.{"tier::a"}, &.{"tier::a"} },
        .expect_pass = false,
    },
    .{
        .label = "an unknown app appeared",
        .examples = &.{ "a", "b" },
        .ereader = false,
        .shards = 2,
        .slices = &.{ &.{"tier::a"}, &.{ "tier::b", "ghost" } },
        .expect_pass = false,
    },
    .{
        .label = "e-reader XIP configuration omitted",
        .examples = &.{},
        .ereader = true,
        .shards = 1,
        .slices = &.{&.{"board::stand_alone::ra8d2-ereader"}},
        .expect_pass = false,
    },
};

fn writeTree(
    allocator: std.mem.Allocator,
    scratch: std.fs.Dir,
    root: []const u8,
    examples: []const []const u8,
    ereader: bool,
) !void {
    for (examples) |app| {
        const source_dir = try std.fmt.allocPrint(allocator, "{s}/examples/tier/{s}/src", .{ root, app });
        defer allocator.free(source_dir);
        try scratch.makePath(source_dir);
        const main_c = try std.fmt.allocPrint(allocator, "{s}/main.c", .{source_dir});
        defer allocator.free(main_c);
        try scratch.writeFile(.{ .sub_path = main_c, .data = "int main(void){return 0;}\n" });
        const lists = try std.fmt.allocPrint(
            allocator,
            "{s}/examples/tier/{s}/CMakeLists.txt",
            .{ root, app },
        );
        defer allocator.free(lists);
        try scratch.writeFile(.{ .sub_path = lists, .data = "add_executable(test src/main.c)\n" });
    }
    if (!ereader) return;

    const board = try std.fmt.allocPrint(
        allocator,
        "{s}/apps/board/stand_alone/ereader",
        .{root},
    );
    defer allocator.free(board);
    const board_src = try std.fmt.allocPrint(allocator, "{s}/src", .{board});
    defer allocator.free(board_src);
    try scratch.makePath(board_src);
    const board_main = try std.fmt.allocPrint(allocator, "{s}/main.c", .{board_src});
    defer allocator.free(board_main);
    try scratch.writeFile(.{ .sub_path = board_main, .data = "void main(void) {}\n" });
    const board_lists = try std.fmt.allocPrint(allocator, "{s}/CMakeLists.txt", .{board});
    defer allocator.free(board_lists);
    try scratch.writeFile(.{ .sub_path = board_lists, .data = "add_executable(ereader src/main.c)\n" });
}

fn writeShardManifests(
    allocator: std.mem.Allocator,
    scratch: std.fs.Dir,
    root: []const u8,
    shards: usize,
    slices: []const []const []const u8,
) !void {
    const shard_dir = try std.fmt.allocPrint(
        allocator,
        "{s}/{s}",
        .{ root, implementation.shard_subdir },
    );
    defer allocator.free(shard_dir);
    try scratch.makePath(shard_dir);

    var every = std.ArrayList([]const u8).init(allocator);
    defer every.deinit();
    for (slices) |slice| {
        for (slice) |app| {
            if (!implementation.containsName(every.items, app)) try every.append(app);
        }
    }
    implementation.sortNames(every.items);

    var body = std.ArrayList(u8).init(allocator);
    defer body.deinit();
    for (every.items) |app| {
        try body.appendSlice(app);
        try body.append('\n');
    }
    const all_configs = try std.fmt.allocPrint(
        allocator,
        "{s}/{s}",
        .{ shard_dir, implementation.all_configs_name },
    );
    defer allocator.free(all_configs);
    try scratch.writeFile(.{ .sub_path = all_configs, .data = body.items });

    for (slices, 0..) |slice, offset| {
        var shard_body = std.ArrayList(u8).init(allocator);
        defer shard_body.deinit();
        for (slice) |app| {
            try shard_body.appendSlice(app);
            try shard_body.append('\n');
        }
        const name = try implementation.shardFileName(allocator, offset + 1, shards);
        defer allocator.free(name);
        const path = try std.fmt.allocPrint(allocator, "{s}/{s}", .{ shard_dir, name });
        defer allocator.free(path);
        try scratch.writeFile(.{ .sub_path = path, .data = shard_body.items });
    }
}

/// `--selftest`: prove the gate still fires on every break and stays quiet on
/// a complete matrix. Fixtures are materialised under `scratch`.
pub fn selftest(
    allocator: std.mem.Allocator,
    scratch: std.fs.Dir,
    out: anytype,
    err: anytype,
) !u8 {
    var failures: usize = 0;

    for (selftest_cases, 0..) |case, number| {
        const root = try std.fmt.allocPrint(allocator, "case-{d}", .{number});
        defer allocator.free(root);
        try scratch.makePath(root);
        try writeTree(allocator, scratch, root, case.examples, case.ereader);
        try writeShardManifests(allocator, scratch, root, case.shards, case.slices);

        const verdict = try checkUnion(allocator, scratch, root, case.shards, out);
        const behaved = if (case.expect_pass)
            verdict.status == implementation.rc_ok
        else
            verdict.status == implementation.rc_violation;
        try out.print("  [{s}] {s}: expected to {s}, rc={d}\n", .{
            if (behaved) "ok" else "FAIL",
            case.label,
            if (case.expect_pass) "pass" else "fire",
            verdict.status,
        });
        if (!behaved) {
            failures += 1;
            for (verdict.problems) |problem| try out.print("          {s}\n", .{problem});
        }
    }

    // The two boundaries: a tree with no manifest directory at all, and a
    // manifest directory with nothing to discover. Neither may pass.
    {
        const root = "boundary-absent-manifests";
        try scratch.makePath(root);
        try writeTree(allocator, scratch, root, &.{"a"}, false);
        const verdict = try checkUnion(allocator, scratch, root, 1, out);
        const behaved = verdict.status == implementation.rc_violation;
        try out.print("  [{s}] absent manifest dir: expected to fire, rc={d}\n", .{
            if (behaved) "ok" else "FAIL",
            verdict.status,
        });
        if (!behaved) failures += 1;
    }
    {
        const root = "boundary-empty-tree";
        const shard_dir = try std.fmt.allocPrint(
            allocator,
            "{s}/{s}",
            .{ root, implementation.shard_subdir },
        );
        defer allocator.free(shard_dir);
        try scratch.makePath(shard_dir);
        const verdict = try checkUnion(allocator, scratch, root, 1, out);
        const behaved = verdict.status == implementation.rc_violation;
        try out.print("  [{s}] empty tree: expected to fire, rc={d}\n", .{
            if (behaved) "ok" else "FAIL",
            verdict.status,
        });
        if (!behaved) failures += 1;
    }

    if (failures > 0) {
        try err.print("{s} --selftest: {d} case(s) FAILED\n", .{ implementation.tool_name, failures });
        return implementation.rc_violation;
    }
    try out.print("{s} --selftest: all cases pass.\n", .{implementation.tool_name});
    return implementation.rc_ok;
}

/// Dispatch one invocation. `scratch` is only touched by `--selftest`.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    scratch: ?std.fs.Dir,
    argv: []const []const u8,
    default_root: []const u8,
    out: anytype,
    err: anytype,
) !u8 {
    const parsed = try parseArgs(allocator, argv);
    switch (parsed) {
        .help => {
            try out.writeAll(help_text);
            return implementation.rc_ok;
        },
        .usage_error => |message| {
            try err.writeAll(usage);
            try err.print("{s}: error: {s}\n", .{ implementation.tool_name, message });
            return implementation.rc_usage;
        },
        .options => |options| {
            if (options.selftest) {
                const scratch_dir = scratch orelse {
                    try err.print("{s}: no scratch directory for --selftest\n", .{implementation.tool_name});
                    return implementation.rc_violation;
                };
                return selftest(allocator, scratch_dir, out, err);
            }
            if (options.shards == null or options.shards.? < 1) {
                try err.writeAll(usage);
                try err.print(
                    "{s}: error: --shards must be a positive integer\n",
                    .{implementation.tool_name},
                );
                return implementation.rc_usage;
            }

            const root = options.repo_root orelse default_root;
            const verdict = checkUnion(
                allocator,
                dir,
                root,
                @intCast(options.shards.?),
                out,
            ) catch |check_err| switch (check_err) {
                error.NonAsciiManifest => {
                    try err.print(
                        "{s}: a shard manifest is not ASCII, so its configuration names cannot be trusted\n",
                        .{implementation.tool_name},
                    );
                    return implementation.rc_violation;
                },
                error.OutOfMemory => return check_err,
                else => {
                    try err.print(
                        "{s}: cannot read the tree under {s}: {s}\n",
                        .{ implementation.tool_name, root, @errorName(check_err) },
                    );
                    return implementation.rc_violation;
                },
            };
            if (verdict.problems.len > 0) {
                try err.print("{s}\n", .{implementation.failure_header});
                for (verdict.problems) |problem| try err.print("  {s}\n", .{problem});
                try err.print("{s}\n", .{implementation.failure_advisory});
            }
            return verdict.status;
        },
    }
}
