//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Scope and detector algebra for the trailing-newline gate (#858).
//!
//! Everything here is pure: it takes a census of repository-relative paths
//! and byte slices and answers questions about them. No file system, no
//! process, no argv. The `git ls-files` census, the directory walk and the
//! reads live in `cli.zig`, so every rule below is provable in a test with no
//! repository on disk.
//!
//! The derived-scope rules reproduce `scripts/checks/lint_targets.py` -- the
//! shared primitive the Python gate called through `first_party_paths` -- and
//! the gate's own extra subtractions on top. A hardcoded root list does not
//! fail when it goes stale, it reports success over a shrinking slice, which
//! is the defect the derived scope exists to prevent.

const std = @import("std");

/// Suffixes the gate treats as first-party source. `.m`, `.mm` and `.inl`
/// carry no entry in the language map on purpose: they are scanned, but no
/// per-language vendored exclusion applies to them.
pub const source_suffixes = [_][]const u8{
    ".c",    ".h",   ".cpp",  ".hpp",   ".cc",
    ".cxx",  ".hh",  ".hxx",  ".m",     ".mm",
    ".inl",  ".py",  ".sh",   ".cmake", ".mk",
    ".just", ".yml", ".yaml", ".ld",
};

/// Extensionless-by-convention listfiles. A suffix set alone cannot see them.
pub const source_names = [_][]const u8{ "CMakeLists.txt", "justfile", "Justfile" };

/// The gate's own subtraction, matched as a substring of the ABSOLUTE path,
/// exactly as the Python matched `frag in str(path)`.
pub const exclude_fragments = [_][]const u8{
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "libs/ra8_fonts/",
    "port/threadx/",
    "_unsupported/",
};

/// Vendored SOUP and generated tables: `lint_targets.EXCLUDED_PREFIXES`.
pub const excluded_prefixes = [_][]const u8{
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "libs/ra8_fonts/",
    "tools/vela/generated/",
};

/// Excluded for C only. A vendored tree is SOUP for the language whose
/// sources it carries, while the build glue that compiles it is ours.
pub const c_excluded_prefixes = [_][]const u8{"port/threadx/"};

/// Top-level roots beneath which a build tree legitimately appears at any
/// depth. Deliberately not "any directory anywhere": `scripts/build/` is
/// source and has to stay visible.
pub const build_tree_roots = [_][]const u8{
    "docs", "examples", "local-poc", "port", "tests", "tools", "apps",
};

/// Directory names a tool reserves, matched at ANY depth because nobody can
/// legitimately author a source directory with one of these names.
pub const tool_output_dir_names = [_][]const u8{
    ".zig-cache", "CMakeFiles", "_deps", "__pycache__", "node_modules",
};

/// Smallest whole-tree sweep the gate will trust. Below it, "all end in a
/// newline" is a lie told by a sweep that scanned nothing.
pub const file_floor: usize = 2200;

/// Smallest census the derived scope will trust, `lint_targets.TRACKED_FLOOR`.
pub const tracked_floor: usize = 1000;

/// True when one path COMPONENT names a build tree. The separator is
/// required, so `builders` is not a build directory.
pub fn isBuildDirName(name: []const u8) bool {
    if (std.mem.eql(u8, name, "build")) return true;
    return std.mem.startsWith(u8, name, "build-") or
        std.mem.startsWith(u8, name, "build_") or
        std.mem.startsWith(u8, name, "cmake-build-");
}

fn containsText(haystack: []const []const u8, needle: []const u8) bool {
    for (haystack) |item| if (std.mem.eql(u8, item, needle)) return true;
    return false;
}

/// True when repo-relative `rel` lives inside a build tree. DIRECTORY
/// components only: a file called `build` is not a build tree.
pub fn isBuildOutput(rel: []const u8) bool {
    const first_cut = std.mem.indexOfScalar(u8, rel, '/') orelse return false;
    const first = rel[0..first_cut];
    const rooted = containsText(&build_tree_roots, first);

    var index: usize = 0;
    var parts = std.mem.splitScalar(u8, rel, '/');
    var current = parts.next();
    while (current) |part| {
        const next = parts.next();
        if (next == null) break; // the last component is the file name
        if (containsText(&tool_output_dir_names, part)) return true;
        if (isBuildDirName(part) and (index == 0 or rooted)) return true;
        index += 1;
        current = next;
    }
    return false;
}

/// `isBuildOutput` for a path that may be absolute, `./`-prefixed or
/// slash-wrapped. Normalising here keeps every call site one predicate.
pub fn isBuildOutputPath(allocator: std.mem.Allocator, path: []const u8, repo_root: []const u8) !bool {
    const text_owned = try std.mem.replaceOwned(u8, allocator, path, "\\", "/");
    defer allocator.free(text_owned);
    const root_owned = try std.mem.replaceOwned(u8, allocator, repo_root, "\\", "/");
    defer allocator.free(root_owned);

    var text = std.mem.trim(u8, text_owned, "/");
    const root = std.mem.trim(u8, root_owned, "/");

    if (root.len != 0 and text.len > root.len and
        std.mem.startsWith(u8, text, root) and text[root.len] == '/')
    {
        text = text[root.len + 1 ..];
    } else if (std.mem.startsWith(u8, text, "./")) {
        text = text[2..];
    }
    return isBuildOutput(text);
}

/// The final path component.
pub fn pathName(rel: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, rel, '/')) |cut| return rel[cut + 1 ..];
    return rel;
}

/// `pathlib.PurePath.suffix`: empty unless a dot sits strictly inside the
/// name, so `.bashrc` and `trailing.` both have no suffix.
pub fn pathSuffix(name: []const u8) []const u8 {
    const cut = std.mem.lastIndexOfScalar(u8, name, '.') orelse return "";
    if (cut == 0 or cut + 1 >= name.len) return "";
    return name[cut..];
}

fn suffixLanguage(suffix: []const u8) ?[]const u8 {
    const pairs = [_]struct { []const u8, []const u8 }{
        .{ ".c", "c" },       .{ ".h", "c" },       .{ ".cpp", "c" },      .{ ".hpp", "c" },
        .{ ".cc", "c" },      .{ ".cxx", "c" },     .{ ".hh", "c" },       .{ ".hxx", "c" },
        .{ ".py", "python" }, .{ ".sh", "shell" },  .{ ".bash", "shell" }, .{ ".cmake", "cmake" },
        .{ ".yml", "yaml" },  .{ ".yaml", "yaml" }, .{ ".mk", "make" },    .{ ".just", "just" },
        .{ ".ld", "ld" },     .{ ".zig", "zig" },
    };
    for (pairs) |pair| if (std.mem.eql(u8, pair[0], suffix)) return pair[1];
    return null;
}

/// The language a path's name implies, before any exclusion is applied.
///
/// An extensionless name answers null rather than consulting a shebang. The
/// Python did read shebangs, but no shebang language is `c` and `c` is the
/// only language carrying an exclusion, so the two agree on every path this
/// gate can reach.
pub fn rawLanguage(rel: []const u8) ?[]const u8 {
    const name = pathName(rel);
    if (std.mem.eql(u8, name, "CMakeLists.txt")) return "cmake";
    if (std.mem.eql(u8, name, "justfile") or std.mem.eql(u8, name, "Justfile")) return "just";
    return suffixLanguage(pathSuffix(name));
}

fn startsWithAny(rel: []const u8, prefixes: []const []const u8) bool {
    for (prefixes) |prefix| if (std.mem.startsWith(u8, rel, prefix)) return true;
    return false;
}

/// `lint_targets._excluded`: SOUP, generated tables and build output always,
/// plus the per-language vendored trees when a language is supplied.
pub fn isExcludedRel(rel: []const u8, language: ?[]const u8) bool {
    if (startsWithAny(rel, &excluded_prefixes) or isBuildOutput(rel)) return true;
    const lang = language orelse return false;
    if (!std.mem.eql(u8, lang, "c")) return false;
    return startsWithAny(rel, &c_excluded_prefixes);
}

/// True when `rel` survives `first_party_paths`, language excludes included.
pub fn isFirstParty(rel: []const u8) bool {
    if (isExcludedRel(rel, null)) return false;
    const lang = rawLanguage(rel);
    if (lang != null and isExcludedRel(rel, lang)) return false;
    return true;
}

/// True when `rel` ends in a source suffix.
pub fn hasSourceSuffix(rel: []const u8) bool {
    for (source_suffixes) |suffix| if (std.mem.endsWith(u8, rel, suffix)) return true;
    return false;
}

/// True when the path's own NAME is one of the extensionless listfiles. The
/// Python matched `endswith(name)` first and then filtered on the basename,
/// so `my-justfile` is not a listfile.
pub fn isSourceName(rel: []const u8) bool {
    return containsText(&source_names, pathName(rel));
}

/// `check_final_newline._is_source`, used for argv-supplied paths.
pub fn isSource(path: []const u8) bool {
    const name = pathName(path);
    const suffix = pathSuffix(name);
    if (suffix.len != 0) {
        for (source_suffixes) |candidate| if (std.mem.eql(u8, candidate, suffix)) return true;
    }
    return containsText(&source_names, name);
}

fn lessThanString(_: void, left: []const u8, right: []const u8) bool {
    return std.mem.order(u8, left, right) == .lt;
}

/// The whole-tree scope: every first-party source path in `census`, sorted.
///
/// The union of the suffix sweep and the listfile sweep, exactly as the
/// Python built it. Deduplicated, so a path cannot be scanned twice.
pub fn derivedScope(allocator: std.mem.Allocator, census: []const []const u8) ![][]const u8 {
    var seen = std.StringHashMap(void).init(allocator);
    defer seen.deinit();
    var kept = std.ArrayList([]const u8).init(allocator);
    errdefer kept.deinit();

    for (census) |rel| {
        const matches = hasSourceSuffix(rel) or isSourceName(rel);
        if (!matches or !isFirstParty(rel)) continue;
        const gop = try seen.getOrPut(rel);
        if (gop.found_existing) continue;
        try kept.append(rel);
    }

    const out = try kept.toOwnedSlice();
    std.mem.sort([]const u8, out, {}, lessThanString);
    return out;
}

/// True when the gate's own fragment subtraction drops this ABSOLUTE path.
pub fn hasExcludedFragment(absolute: []const u8) bool {
    for (exclude_fragments) |fragment| {
        if (std.mem.indexOf(u8, absolute, fragment) != null) return true;
    }
    return false;
}

/// The detector: a file is fine when it is empty or ends in a newline byte.
/// An unreadable file is not this gate's problem and answers true.
pub fn endsInNewline(data: []const u8) bool {
    return data.len == 0 or data[data.len - 1] == '\n';
}

/// `check_final_newline._rel`: repo-relative when the path is under the root,
/// otherwise the path unchanged.
pub fn displayPath(absolute: []const u8, repo_root: []const u8) []const u8 {
    const root = std.mem.trimRight(u8, repo_root, "/");
    if (root.len == 0) return absolute;
    if (absolute.len > root.len and
        std.mem.startsWith(u8, absolute, root) and absolute[root.len] == '/')
    {
        return absolute[root.len + 1 ..];
    }
    return absolute;
}

/// Sort a path list in place, byte order, as `sorted()` did.
pub fn sortPaths(paths: [][]const u8) void {
    std.mem.sort([]const u8, paths, {}, lessThanString);
}

/// True when any path in `scope` starts with `root_name + "/"`. The selftest's
/// scope probe: a clean run over a scope that never reaches `just/` or
/// `infra/` proves nothing.
pub fn scopeReaches(scope: []const []const u8, root_name: []const u8) bool {
    for (scope) |rel| {
        if (std.mem.startsWith(u8, rel, root_name) and
            rel.len > root_name.len and rel[root_name.len] == '/') return true;
    }
    return false;
}
