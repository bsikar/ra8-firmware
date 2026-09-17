//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Placement rules for the `check_header_file_placement` gate (#858, #1219).
//!
//! A header under a `src/` directory is module-private by construction and
//! must announce that with an `_internal` stem suffix. This module holds the
//! whole decision: pathlib-compatible suffix/stem splitting, the nearest
//! `inc`/`src` ancestor walk, the vendored and generated exclusions, the
//! shared build-output rule and the renderers. It touches no file system and
//! reads no argv, so every rule below is testable in isolation.

const std = @import("std");

/// Suffixes the predecessor's HEADER_SUFFIXES accepted, case-sensitively.
pub const header_suffixes = [_][]const u8{ ".h", ".hpp", ".hh", ".hxx" };

/// Top-level directories the whole-tree sweep walks (SCAN_ROOTS).
pub const scan_roots = [_][]const u8{ "libs", "port", "examples", "tools", "apps", "tests" };

/// Substrings that take a path out of scope wherever they appear (EXCLUDE_FRAGMENTS).
pub const exclude_fragments = [_][]const u8{
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "libs/ra8_fonts/",
};

/// The stem suffix marking a src/ header as intentionally module-private.
pub const internal_stem_suffix = "_internal";

/// A whole-tree pass below this measured population has lost scope.
pub const min_private_headers: usize = 100;

/// Roots beneath which a per-target build tree legitimately appears, at any
/// depth (lint_targets.BUILD_TREE_ROOTS).
pub const build_tree_roots = [_][]const u8{
    "docs", "examples", "local-poc", "port", "tests", "tools", "apps",
};

/// Directory names owned by a tool, matched at ANY depth
/// (lint_targets.TOOL_OUTPUT_DIR_NAMES).
pub const tool_output_dir_names = [_][]const u8{
    ".zig-cache", "CMakeFiles", "_deps", "__pycache__", "node_modules",
};

fn inList(haystack: []const []const u8, needle: []const u8) bool {
    for (haystack) |item| {
        if (std.mem.eql(u8, item, needle)) return true;
    }
    return false;
}

/// True when one path COMPONENT names a build tree: exact `build`, or a
/// `build-` / `build_` / `cmake-build-` prefix. The separator is required, so
/// `builders` is NOT a build directory.
pub fn isBuildDirName(name: []const u8) bool {
    if (std.mem.eql(u8, name, "build")) return true;
    return std.mem.startsWith(u8, name, "build-") or
        std.mem.startsWith(u8, name, "build_") or
        std.mem.startsWith(u8, name, "cmake-build-");
}

/// True when repo-relative `rel` lives inside a build tree. DIRECTORY
/// components only: a FILE called `build` is not a build tree, which is why
/// the last component is never examined.
pub fn isBuildOutput(rel: []const u8) bool {
    var iterator = std.mem.splitScalar(u8, rel, '/');
    // Collect the leading component once; the scan needs it for every part.
    const first = iterator.first();
    var index: usize = 0;
    var current: []const u8 = first;
    while (iterator.next()) |next_part| {
        // `current` is a directory component precisely because another
        // component follows it.
        if (inList(&tool_output_dir_names, current)) return true;
        if (isBuildDirName(current) and (index == 0 or inList(&build_tree_roots, first))) return true;
        current = next_part;
        index += 1;
    }
    return false;
}

/// Python's `str.strip("/")`, which strips BOTH ends.
pub fn stripSlashes(text: []const u8) []const u8 {
    var start: usize = 0;
    var end: usize = text.len;
    while (start < end and text[start] == '/') start += 1;
    while (end > start and text[end - 1] == '/') end -= 1;
    return text[start..end];
}

/// `lint_targets.is_build_output_path` for a path that may be absolute.
/// Backslashes fold to slashes, both ends lose their slashes, then a leading
/// repo root or `./` is removed before the component-wise rule runs.
pub fn isBuildOutputPath(allocator: std.mem.Allocator, path: []const u8, repo_root: []const u8) !bool {
    const folded = try allocator.alloc(u8, path.len);
    defer allocator.free(folded);
    for (path, 0..) |byte, i| folded[i] = if (byte == '\\') '/' else byte;

    var text = stripSlashes(folded);

    const folded_root = try allocator.alloc(u8, repo_root.len);
    defer allocator.free(folded_root);
    for (repo_root, 0..) |byte, i| folded_root[i] = if (byte == '\\') '/' else byte;
    const root = stripSlashes(folded_root);

    if (root.len != 0 and text.len > root.len + 1 and
        std.mem.startsWith(u8, text, root) and text[root.len] == '/')
    {
        text = text[root.len + 1 ..];
    } else if (std.mem.startsWith(u8, text, "./")) {
        text = text[2..];
    }
    return isBuildOutput(text);
}

/// The final path component, as `pathlib.PurePath.name` yields it.
pub fn pathName(path: []const u8) []const u8 {
    const trimmed = std.mem.trimRight(u8, path, "/");
    if (std.mem.lastIndexOfScalar(u8, trimmed, '/')) |cut| return trimmed[cut + 1 ..];
    return trimmed;
}

/// `pathlib.PurePath.suffix`. A name that is all dots, or whose only dot is
/// leading, has NO suffix: `Path(".h").suffix` is the empty string.
pub fn pathSuffix(path: []const u8) []const u8 {
    const name = pathName(path);
    if (std.mem.lastIndexOfScalar(u8, name, '.')) |cut| {
        if (cut == 0) return "";
        // A trailing dot is not a suffix either: `Path("a.").suffix` is "".
        if (cut == name.len - 1) return "";
        return name[cut..];
    }
    return "";
}

/// `pathlib.PurePath.stem`: the name with `pathSuffix` removed.
pub fn pathStem(path: []const u8) []const u8 {
    const name = pathName(path);
    const suffix = pathSuffix(path);
    return name[0 .. name.len - suffix.len];
}

/// True when the path's suffix is one the gate scans.
pub fn isHeader(path: []const u8) bool {
    return inList(&header_suffixes, pathSuffix(path));
}

/// True when a NAME matches one of the `rglob("*<suffix>")` patterns the
/// predecessor discovered with. This is glob matching, NOT pathlib suffix
/// semantics, and the two genuinely disagree: `Path(".h").suffix` is empty, so
/// an explicitly listed file called `.h` was dropped by `_is_header`, while
/// `rglob("*.h")` matches the same name (pathlib's globber matches leading
/// dots) and the directory walk never re-filtered its hits. A `.h` under a
/// `src/` directory was therefore reported by a sweep and ignored when named
/// on the command line, and both halves are preserved here.
pub fn matchesHeaderGlob(path: []const u8) bool {
    const name = pathName(path);
    for (header_suffixes) |suffix| {
        if (std.mem.endsWith(u8, name, suffix)) return true;
    }
    return false;
}

/// True when the stem carries the `_internal` marker.
pub fn isInternal(path: []const u8) bool {
    return std.mem.endsWith(u8, pathStem(path), internal_stem_suffix);
}

/// The nearest `inc`/`src` ancestor component of the path's PARENT, or null.
/// The closest one to the file decides, so a module may nest an `inc` inside a
/// `src` tree and the deeper `inc` wins.
pub fn governingDir(path: []const u8) ?[]const u8 {
    const trimmed = std.mem.trimRight(u8, path, "/");
    const cut = std.mem.lastIndexOfScalar(u8, trimmed, '/') orelse return null;
    const parent = trimmed[0..cut];
    var index: usize = parent.len;
    while (index > 0) {
        const slash = std.mem.lastIndexOfScalar(u8, parent[0..index], '/');
        const start = if (slash) |position| position + 1 else 0;
        const part = parent[start..index];
        if (std.mem.eql(u8, part, "inc")) return "inc";
        if (std.mem.eql(u8, part, "src")) return "src";
        if (slash == null) break;
        index = slash.?;
    }
    return null;
}

/// True when the header's nearest inc/src ancestor is a private `src`.
pub fn underSrc(path: []const u8) bool {
    const governing = governingDir(path) orelse return false;
    return std.mem.eql(u8, governing, "src");
}

/// True when any EXCLUDE_FRAGMENTS substring appears anywhere in the path, or
/// the path is build output.
pub fn isExcluded(allocator: std.mem.Allocator, path: []const u8, repo_root: []const u8) !bool {
    if (try isBuildOutputPath(allocator, path, repo_root)) return true;
    for (exclude_fragments) |fragment| {
        if (std.mem.indexOf(u8, path, fragment) != null) return true;
    }
    return false;
}

/// `Path.relative_to(REPO_ROOT)` when the path is under the root, else the
/// path unchanged.
pub fn relativeTo(path: []const u8, repo_root: []const u8) []const u8 {
    if (repo_root.len == 0) return path;
    const root = std.mem.trimRight(u8, repo_root, "/");
    if (std.mem.eql(u8, path, root)) return ".";
    if (path.len > root.len + 1 and std.mem.startsWith(u8, path, root) and path[root.len] == '/') {
        return path[root.len + 1 ..];
    }
    return path;
}

/// Python's `sorted()` over str: unsigned code-point order, shorter first on a
/// shared prefix. Over UTF-8 bytes this is the same ordering.
pub fn pythonLessThan(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.order(u8, left, right) == .lt;
}

/// The result of auditing a target list: how many headers were actually under
/// a src/ directory, and the sorted repo-relative paths of the misplaced ones.
pub const Audit = struct {
    scanned: usize,
    offenders: [][]const u8,
};

/// Count the private headers and collect the misplaced ones. Targets that are
/// not under a src/ directory are skipped before the count, so the scanned
/// total is legitimately far smaller than the list handed in.
pub fn auditTargets(
    allocator: std.mem.Allocator,
    targets: []const []const u8,
    repo_root: []const u8,
) !Audit {
    var scanned: usize = 0;
    var offenders = std.ArrayList([]const u8).init(allocator);
    errdefer offenders.deinit();
    for (targets) |path| {
        if (!underSrc(path)) continue;
        scanned += 1;
        if (!isInternal(path)) {
            try offenders.append(relativeTo(path, repo_root));
        }
    }
    const owned = try offenders.toOwnedSlice();
    std.mem.sort([]const u8, owned, {}, pythonLessThan);
    return .{ .scanned = scanned, .offenders = owned };
}

/// Whether the private-header census is non-vacuous for this mode. An explicit
/// path list is exempt; a whole-tree sweep must clear the floor.
pub fn censusOk(scanned: usize, explicit_paths: bool) bool {
    return explicit_paths or scanned >= min_private_headers;
}

pub fn renderNoHeaders(writer: anytype) !void {
    try writer.writeAll("check_header_file_placement.py: no headers to scan\n");
}

pub fn renderCollapsed(writer: anytype, scanned: usize) !void {
    try writer.print(
        "check_header_file_placement.py: whole-tree scan reached only {d} private header(s), below floor {d}\n",
        .{ scanned, min_private_headers },
    );
}

pub fn renderClean(writer: anytype, scanned: usize) !void {
    try writer.print(
        "check_header_file_placement.py: {d} src/ header(s) scanned, all module-private (*_internal.h).\n",
        .{scanned},
    );
}

pub const guidance =
    "\nA header under a src/ directory is module-private and must say so.\n" ++
    "For each offender, decide which it is and fix at the root:\n" ++
    "  - public interface (consumed outside the module) -> move it to the\n" ++
    "    module's inc/ directory;\n" ++
    "  - genuinely module-private -> rename it '*_internal.h'.\n" ++
    "Update every #include of the header in the same change.  There is no\n" ++
    "waiver marker -- placement is the contract.\n";

/// The predecessor's failure report: a header line, a blank line from the
/// trailing `\n` in its format string, one indented path per offender, then
/// the guidance block whose own leading newline separates it.
pub fn renderOffenders(writer: anytype, offenders: []const []const u8) !void {
    try writer.print(
        "check_header_file_placement.py: {d} src/ header(s) are not *_internal.h:\n\n",
        .{offenders.len},
    );
    for (offenders) |path| try writer.print("  {s}\n", .{path});
    try writer.writeAll(guidance);
}

pub fn renderSelftestPass(writer: anytype) !void {
    try writer.writeAll(
        "check_header_file_placement.py --selftest: PASS (fire, quiet, tests, exclusions)\n",
    );
}

pub fn renderSelftestFailure(writer: anytype, detail: []const u8) !void {
    try writer.print("  [FAIL] {s}\n", .{detail});
}

pub fn renderSelftestWithPaths(writer: anytype) !void {
    try writer.writeAll("--selftest does not accept paths\n");
}
