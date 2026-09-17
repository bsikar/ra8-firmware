//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! argv membrane, repository discovery and exit contract for the
//! host-build-entrypoint gate (#858), replacing the argparse `main` of
//! scripts/checks/check_host_build_entrypoints.py.

const std = @import("std");
const impl = @import("internal/root.zig");

pub const tool = impl.tool;

const max_file_bytes = 16 * 1024 * 1024;

pub const usage_line = "usage: " ++ tool ++ " [-h] [--selftest]";

const Action = union(enum) {
    help,
    selftest,
    audit,
    usage_error: []const u8,
};

fn lessThan(_: void, a: []const u8, b: []const u8) bool {
    return std.mem.lessThan(u8, a, b);
}

/// argparse's option handling, including its prefix abbreviation: `--self`
/// resolves to `--selftest`, an explicit value for a store_true flag is an
/// error, and any positional argument is an error.
pub fn parseArgs(argv: []const []const u8) Action {
    var want_selftest = false;
    var positional_only = false;
    for (argv) |arg| {
        if (!positional_only and std.mem.eql(u8, arg, "--")) {
            positional_only = true;
            continue;
        }
        if (positional_only or arg.len < 2 or arg[0] != '-') return .{ .usage_error = arg };
        if (std.mem.startsWith(u8, arg, "--")) {
            const eq = std.mem.indexOfScalar(u8, arg, '=');
            const name = if (eq) |i| arg[0..i] else arg;
            const body = name[2..];
            if (body.len == 0) return .{ .usage_error = arg };
            var matches: usize = 0;
            var is_help = false;
            if (std.mem.startsWith(u8, "help", body)) {
                matches += 1;
                is_help = true;
            }
            if (std.mem.startsWith(u8, "selftest", body)) matches += 1;
            if (matches != 1 or eq != null) return .{ .usage_error = arg };
            if (is_help) return .help;
            want_selftest = true;
            continue;
        }
        if (std.mem.eql(u8, arg, "-h")) return .help;
        return .{ .usage_error = arg };
    }
    return if (want_selftest) .selftest else .audit;
}

fn printHelp(out: anytype) !void {
    try out.print("{s}\n\n", .{usage_line});
    try out.print("Keep native Just builds on the C23 compiler and complete tool registry.\n\n", .{});
    try out.print("options:\n", .{});
    try out.print("  -h, --help  show this help message and exit\n", .{});
    try out.print("  --selftest  prove the detectors fire and stay quiet, then exit\n", .{});
}

// -- discovery ---------------------------------------------------------------

fn readFileAt(allocator: std.mem.Allocator, repo_root: []const u8, rel: []const u8) !?[]const u8 {
    const path = try std.fs.path.join(allocator, &.{ repo_root, rel });
    defer allocator.free(path);
    return std.fs.cwd().readFileAlloc(allocator, path, max_file_bytes) catch |e| switch (e) {
        error.FileNotFound, error.IsDir, error.AccessDenied, error.NotDir => null,
        else => e,
    };
}

/// `_just_files`: the root entry point, then every module, sorted.
pub fn justFiles(allocator: std.mem.Allocator, repo_root: []const u8) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit();
    }
    try out.append(try allocator.dupe(u8, "justfile"));

    const just_dir = try std.fs.path.join(allocator, &.{ repo_root, "just" });
    defer allocator.free(just_dir);
    var dir = std.fs.cwd().openDir(just_dir, .{ .iterate = true }) catch |e| switch (e) {
        error.FileNotFound, error.NotDir, error.AccessDenied => return out.toOwnedSlice(),
        else => return e,
    };
    defer dir.close();

    var names = std.ArrayList([]const u8).init(allocator);
    defer {
        for (names.items) |item| allocator.free(item);
        names.deinit();
    }
    var it = dir.iterate();
    while (try it.next()) |entry| {
        if (entry.kind == .directory) continue;
        if (!std.mem.endsWith(u8, entry.name, ".just")) continue;
        try names.append(try allocator.dupe(u8, entry.name));
    }
    std.mem.sort([]const u8, names.items, {}, lessThan);
    for (names.items) |name| {
        try out.append(try std.fmt.allocPrint(allocator, "just/{s}", .{name}));
    }
    return out.toOwnedSlice();
}

fn toolRootNames(allocator: std.mem.Allocator, repo_root: []const u8) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit();
    }
    const tools_dir = try std.fs.path.join(allocator, &.{ repo_root, "tools" });
    defer allocator.free(tools_dir);
    var dir = std.fs.cwd().openDir(tools_dir, .{ .iterate = true }) catch |e| switch (e) {
        error.FileNotFound, error.NotDir, error.AccessDenied => return out.toOwnedSlice(),
        else => return e,
    };
    defer dir.close();
    var it = dir.iterate();
    while (try it.next()) |entry| {
        if (entry.kind != .directory) continue;
        try out.append(try allocator.dupe(u8, entry.name));
    }
    std.mem.sort([]const u8, out.items, {}, lessThan);
    return out.toOwnedSlice();
}

/// `_compiled_tools`: tool roots holding authored compiled implementation
/// anywhere under `src/`. pathlib's glob sees dotted names, so a hidden root
/// still counts.
pub fn compiledTools(allocator: std.mem.Allocator, repo_root: []const u8) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit();
    }
    const roots = try toolRootNames(allocator, repo_root);
    defer impl.freeStrings(allocator, roots);

    for (roots) |name| {
        const src = try std.fs.path.join(allocator, &.{ repo_root, "tools", name, "src" });
        defer allocator.free(src);
        var dir = std.fs.cwd().openDir(src, .{ .iterate = true }) catch continue;
        defer dir.close();
        var walker = try dir.walk(allocator);
        defer walker.deinit();
        while (try walker.next()) |entry| {
            if (entry.kind != .file) continue;
            if (!impl.isCompiledSuffix(entry.basename)) continue;
            try out.append(try allocator.dupe(u8, name));
            break;
        }
    }
    return out.toOwnedSlice();
}

/// `_cmake_tools`: tool roots the CMake dispatcher manages.
pub fn cmakeTools(allocator: std.mem.Allocator, repo_root: []const u8) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit();
    }
    const roots = try toolRootNames(allocator, repo_root);
    defer impl.freeStrings(allocator, roots);
    for (roots) |name| {
        const listfile = try std.fs.path.join(
            allocator,
            &.{ repo_root, "tools", name, "CMakeLists.txt" },
        );
        defer allocator.free(listfile);
        const stat = std.fs.cwd().statFile(listfile) catch continue;
        if (stat.kind != .file) continue;
        try out.append(try allocator.dupe(u8, name));
    }
    return out.toOwnedSlice();
}

/// `_inventory_errors`.
pub fn inventoryErrors(allocator: std.mem.Allocator, repo_root: []const u8) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit();
    }
    const compiled = try compiledTools(allocator, repo_root);
    defer impl.freeStrings(allocator, compiled);
    const managed = try cmakeTools(allocator, repo_root);
    defer impl.freeStrings(allocator, managed);
    for (compiled) |name| {
        if (impl.containsString(managed, name)) continue;
        try out.append(try std.fmt.allocPrint(
            allocator,
            "tools/{s}: compiled tool has no CMakeLists.txt",
            .{name},
        ));
    }
    return out.toOwnedSlice();
}

fn appendMissingContract(
    allocator: std.mem.Allocator,
    errors: *std.ArrayList([]const u8),
    text: []const u8,
    required: []const []const u8,
    comptime template: []const u8,
) !void {
    for (required) |needle| {
        if (std.mem.indexOf(u8, text, needle) != null) continue;
        try errors.append(try std.fmt.allocPrint(allocator, template, .{needle}));
    }
}

/// `_shared_dispatch_errors`.
pub fn sharedDispatchErrors(allocator: std.mem.Allocator, repo_root: []const u8) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit();
    }
    const just_text = try readFileAt(allocator, repo_root, "just/shared.just");
    defer if (just_text) |t| allocator.free(t);
    if (just_text) |text| {
        if (std.mem.indexOf(u8, text, impl.shared_just_delegation) == null) {
            try out.append(try allocator.dupe(
                u8,
                "just/shared.just does not delegate to the shared-library dispatcher",
            ));
        }
    } else {
        try out.append(try allocator.dupe(u8, "just/shared.just is unreadable"));
    }

    const dispatcher = try readFileAt(allocator, repo_root, "scripts/builders/build_shared_libs.sh");
    defer if (dispatcher) |t| allocator.free(t);
    try appendMissingContract(
        allocator,
        &out,
        dispatcher orelse "",
        &impl.shared_dispatcher_contract,
        "shared-library dispatcher lacks contract: {s}",
    );
    return out.toOwnedSlice();
}

pub const Dispatch = struct {
    listed: [][]const u8,
    errors: [][]const u8,
};

/// `_live_dispatch`: the read-only `build_host_tools.sh list` mode.
pub fn liveDispatch(allocator: std.mem.Allocator, repo_root: []const u8) !Dispatch {
    var listed = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (listed.items) |item| allocator.free(item);
        listed.deinit();
    }
    var errors = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (errors.items) |item| allocator.free(item);
        errors.deinit();
    }

    const script = try std.fs.path.join(
        allocator,
        &.{ repo_root, "scripts", "builders", "build_host_tools.sh" },
    );
    defer allocator.free(script);

    const result = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &.{ script, "list" },
        .cwd = repo_root,
        .max_output_bytes = max_file_bytes,
    }) catch |e| {
        try errors.append(try std.fmt.allocPrint(
            allocator,
            "tool dispatcher list failed: {s}",
            .{@errorName(e)},
        ));
        return .{ .listed = try listed.toOwnedSlice(), .errors = try errors.toOwnedSlice() };
    };
    defer allocator.free(result.stdout);
    defer allocator.free(result.stderr);

    const failed = switch (result.term) {
        .Exited => |code| code != 0,
        else => true,
    };
    if (failed) {
        try errors.append(try std.fmt.allocPrint(
            allocator,
            "tool dispatcher list failed: {s}",
            .{impl.strip(result.stderr)},
        ));
        return .{ .listed = try listed.toOwnedSlice(), .errors = try errors.toOwnedSlice() };
    }

    var it = impl.LineIterator{ .text = result.stdout };
    while (it.next()) |line| {
        if (impl.containsString(listed.items, line)) continue;
        try listed.append(try allocator.dupe(u8, line));
    }
    std.mem.sort([]const u8, listed.items, {}, lessThan);
    return .{ .listed = try listed.toOwnedSlice(), .errors = try errors.toOwnedSlice() };
}

/// `_dispatcher_errors`.
pub fn dispatcherErrors(
    allocator: std.mem.Allocator,
    repo_root: []const u8,
    listed: []const []const u8,
) ![][]const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    errdefer {
        for (out.items) |item| allocator.free(item);
        out.deinit();
    }
    const expected = try cmakeTools(allocator, repo_root);
    defer impl.freeStrings(allocator, expected);
    for (expected) |name| {
        if (impl.containsString(listed, name)) continue;
        try out.append(try std.fmt.allocPrint(allocator, "tool dispatcher omits {s}", .{name}));
    }
    for (listed) |name| {
        if (impl.containsString(expected, name)) continue;
        try out.append(try std.fmt.allocPrint(allocator, "tool dispatcher invents {s}", .{name}));
    }

    const tools_just = try readFileAt(allocator, repo_root, "just/tools.just");
    defer if (tools_just) |t| allocator.free(t);
    try appendMissingContract(
        allocator,
        &out,
        tools_just orelse "",
        &impl.tools_just_contract,
        "just/tools.just lacks discovery contract: {s}",
    );

    const wrapper = try readFileAt(allocator, repo_root, "scripts/builders/host_cmake.sh");
    defer if (wrapper) |t| allocator.free(t);
    try appendMissingContract(
        allocator,
        &out,
        wrapper orelse "",
        &impl.host_cmake_contract,
        "host_cmake.sh lacks compiler/cache contract: {s}",
    );

    const dispatcher = try readFileAt(allocator, repo_root, "scripts/builders/build_host_tools.sh");
    defer if (dispatcher) |t| allocator.free(t);
    try appendMissingContract(
        allocator,
        &out,
        dispatcher orelse "",
        &impl.dispatcher_clean_contract,
        "tool dispatcher lacks legacy-clean contract: {s}",
    );
    return out.toOwnedSlice();
}

// -- the audit ---------------------------------------------------------------

fn extend(
    allocator: std.mem.Allocator,
    errors: *std.ArrayList([]const u8),
    items: [][]const u8,
) !void {
    defer allocator.free(items);
    for (items) |item| try errors.append(item);
}

pub fn audit(
    allocator: std.mem.Allocator,
    repo_root: []const u8,
    out: anytype,
    err: anytype,
) !u8 {
    var errors = std.ArrayList([]const u8).init(allocator);
    defer {
        for (errors.items) |item| allocator.free(item);
        errors.deinit();
    }

    const just_paths = try justFiles(allocator, repo_root);
    defer impl.freeStrings(allocator, just_paths);
    for (just_paths) |rel| {
        const text = try readFileAt(allocator, repo_root, rel);
        defer if (text) |t| allocator.free(t);
        if (text) |t| {
            try extend(allocator, &errors, try impl.recipeErrors(allocator, rel, t));
        } else {
            try errors.append(try std.fmt.allocPrint(allocator, "{s} is unreadable", .{rel}));
        }
    }

    try extend(allocator, &errors, try inventoryErrors(allocator, repo_root));
    try extend(allocator, &errors, try sharedDispatchErrors(allocator, repo_root));

    const dispatch = try liveDispatch(allocator, repo_root);
    defer impl.freeStrings(allocator, dispatch.listed);
    const listed_count = dispatch.listed.len;
    try extend(allocator, &errors, dispatch.errors);
    try extend(allocator, &errors, try dispatcherErrors(allocator, repo_root, dispatch.listed));

    if (errors.items.len > 0) {
        try err.print("{s}: host build contract violations:\n", .{tool});
        for (errors.items) |item| try err.print("  {s}\n", .{item});
        return 1;
    }
    try out.print(
        "{s}: clean ({d} Just files, {d} compiled tools)\n",
        .{ tool, just_paths.len, listed_count },
    );
    return 0;
}

// -- the selftest ------------------------------------------------------------

pub const good_fixture = "build:\n    bash scripts/builders/host_cmake.sh tools/x tools/x/build\n";
pub const cross_fixture = "build:\n    cmake -S x -B b -DCMAKE_TOOLCHAIN_FILE=cmake/arm.cmake\n    cmake --build b\n";
pub const bad_cmake_fixture = "build:\n    cmake -S tools/x -B tools/x/build\n";
pub const mixed_cmake_fixture = "build:\n    cmake -S arm -B arm/build -DCMAKE_TOOLCHAIN_FILE=cmake/arm.cmake\n    cmake -S tools/x -B tools/x/build\n";
pub const bad_cc_fixture = "build:\n    cc -std=gnu23 src/main.c -o tool\n";

fn recipeErrorCount(allocator: std.mem.Allocator, label: []const u8, text: []const u8) !usize {
    const errors = try impl.recipeErrors(allocator, label, text);
    defer impl.freeStrings(allocator, errors);
    return errors.len;
}

fn fixtureErrorCount(
    allocator: std.mem.Allocator,
    expected: []const []const u8,
    listed: []const []const u8,
) !usize {
    const errors = try impl.dispatcherFixtureErrors(allocator, expected, listed);
    defer impl.freeStrings(allocator, errors);
    return errors.len;
}

fn writeFixture(
    allocator: std.mem.Allocator,
    root: []const u8,
    rel: []const u8,
    contents: []const u8,
) !void {
    const path = try std.fs.path.join(allocator, &.{ root, rel });
    defer allocator.free(path);
    if (std.fs.path.dirname(path)) |parent| try std.fs.cwd().makePath(parent);
    try std.fs.cwd().writeFile(.{ .sub_path = path, .data = contents });
}

pub fn selftest(allocator: std.mem.Allocator, out: anytype, err: anytype) !u8 {
    var failures = std.ArrayList([]const u8).init(allocator);
    defer failures.deinit();

    if (try recipeErrorCount(allocator, "good", good_fixture) > 0 or
        try recipeErrorCount(allocator, "cross", cross_fixture) > 0)
    {
        try failures.append("wrapper or ARM-toolchain fixture was rejected");
    }
    if (try recipeErrorCount(allocator, "bad-cmake", bad_cmake_fixture) == 0) {
        try failures.append("raw native CMake fixture was accepted");
    }
    if (try recipeErrorCount(allocator, "mixed-cmake", mixed_cmake_fixture) == 0) {
        try failures.append("raw native CMake hidden beside an ARM configure was accepted");
    }
    if (try recipeErrorCount(allocator, "bad-cc", bad_cc_fixture) == 0) {
        try failures.append("raw compiler fixture was accepted");
    }
    if (impl.standaloneCmake("target_sources(app PRIVATE src/x.c)\n")) {
        try failures.append("consumer CMake fragment was classified as standalone");
    }
    if (!impl.standaloneCmake("project(shared LANGUAGES C)\n")) {
        try failures.append("standalone shared CMake project was classified as a fragment");
    }

    var name_buf: [96]u8 = undefined;
    const leaf = try std.fmt.bufPrint(
        &name_buf,
        "ra8-{s}-{x}",
        .{ tool, std.crypto.random.int(u32) },
    );
    const tmp = try std.fs.path.join(allocator, &.{ "/tmp", leaf });
    defer allocator.free(tmp);
    try std.fs.cwd().makePath(tmp);
    defer std.fs.cwd().deleteTree(tmp) catch {};

    try writeFixture(allocator, tmp, "justfile", "default:\n    true\n");
    try writeFixture(allocator, tmp, "just/future.just", bad_cmake_fixture);
    {
        const discovered = try justFiles(allocator, tmp);
        defer impl.freeStrings(allocator, discovered);
        const ok = discovered.len == 2 and
            std.mem.eql(u8, discovered[0], "justfile") and
            std.mem.eql(u8, discovered[1], "just/future.just");
        if (!ok) try failures.append("new Just module was omitted from discovery");
    }

    try writeFixture(allocator, tmp, "tools/native/src/main.c", "int main(void){}\n");
    {
        const errors = try inventoryErrors(allocator, tmp);
        defer impl.freeStrings(allocator, errors);
        if (errors.len == 0) try failures.append("compiled tool without CMake was accepted");
    }
    try writeFixture(allocator, tmp, "tools/native/CMakeLists.txt", "project(native C)\n");
    {
        const errors = try inventoryErrors(allocator, tmp);
        defer impl.freeStrings(allocator, errors);
        if (errors.len > 0) try failures.append("compiled tool with CMake was rejected");
    }

    if (try fixtureErrorCount(allocator, &.{"native"}, &.{}) == 0) {
        try failures.append("dispatcher omission was accepted");
    }
    if (try fixtureErrorCount(allocator, &.{"native"}, &.{"native"}) > 0) {
        try failures.append("complete dispatcher fixture was rejected");
    }

    if (failures.items.len > 0) {
        try err.print("{s} --selftest FAILED:\n", .{tool});
        for (failures.items) |failure| try err.print("  {s}\n", .{failure});
        return 1;
    }
    try out.print("{s} --selftest: PASS (12 both-direction cases)\n", .{tool});
    return 0;
}

// -- entry point -------------------------------------------------------------

pub fn run(
    allocator: std.mem.Allocator,
    argv: []const []const u8,
    repo_root: []const u8,
    out: anytype,
    err: anytype,
) !u8 {
    switch (parseArgs(argv)) {
        .help => {
            try printHelp(out);
            return 0;
        },
        .usage_error => |arg| {
            try err.print("{s}\n", .{usage_line});
            try err.print("{s}: error: unrecognized arguments: {s}\n", .{ tool, arg });
            return 2;
        },
        .selftest => return selftest(allocator, out, err),
        .audit => return audit(allocator, repo_root, out, err),
    }
}
