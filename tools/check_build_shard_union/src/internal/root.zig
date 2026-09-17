//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The cross-build shard-union gate, as pure computation (#858, #1159).
//!
//! Everything here is a function of text and of already-collected path lists:
//! no file system, no argv, no process state, so every rule below is provable
//! with no repository on disk. The tree walk, the argv membrane and the exit
//! contract live in `../cli.zig`.
//!
//! The rules are inherited from the Python this replaced, not reinvented. The
//! load-bearing one is fail-closed: a union is only vouched for against an
//! INDEPENDENTLY re-derived truth, so an empty discovery is a violation rather
//! than a vacuous pass, a missing shard manifest is a shard that did not run,
//! and a configuration claimed by two shards fails because the stride is then
//! broken and some other configuration is missing too.

const std = @import("std");

/// Where `scripts/builders/all_examples.sh` writes its per-shard manifests,
/// as the Python's `SHARD_SUBDIR` spelled it.
pub const shard_subdir = "build/build_all_examples/.shard";

/// The full execution matrix, written identically by every shard.
pub const all_configs_name = "all-configs.txt";

/// An example path needs at least a tier and an app directory.
pub const min_example_path_parts: usize = 2;

/// Separator between path parts in a configuration identifier.
pub const separator = "::";

/// The board products the gate requires beside the examples.
pub const board_prefix = "board::stand_alone::";
pub const ereader_rel = "ereader";
pub const ereader_name = "ra8d2-ereader";
pub const ns_xip_suffix = "@ns-xip";

/// The tier under examples/ that holds shared code rather than an app.
pub const shared_tier = "shared";

pub const rc_ok: u8 = 0;
pub const rc_violation: u8 = 1;
pub const rc_usage: u8 = 2;

/// Diagnostics name the tool, not a module: the Python spelled itself
/// `check_build_shard_union.py` and the migrated tool drops the suffix.
pub const tool_name = "check_build_shard_union";

/// The one problem an empty discovery reports. Spelled exactly as the Python
/// concatenated it, because a gate with no truth to compare against must say
/// so rather than pass.
pub const empty_discovery_problem =
    "no firmware configurations discovered under examples/ or " ++
    "apps/board/stand_alone/ -- this checker " ++
    "cannot vouch for a union it has no truth to compare against.";

/// Reported when the matrix a shard wrote disagrees with a fresh discovery.
pub const disagreement_problem = all_configs_name ++ " disagrees with fresh discovery";

/// Header printed above the problem list on stderr.
pub const failure_header = tool_name ++ ": the cross-build shards did NOT cover the tree";

/// The advisory the Python printed under the problems. Kept verbatim: it is
/// the instruction not to relax the gate to make a red matrix pass.
pub const failure_advisory =
    "\n  Every app must be built by exactly one shard. Do NOT relax this to\n" ++
    "  make a red matrix pass: an unbuilt app is an unchecked app, and the\n" ++
    "  stack-usage aggregate downstream would still clear its floor on the\n" ++
    "  shards that did run.";

/// A manifest byte outside ASCII. The Python read manifests with
/// `encoding="ascii"`, so one such byte raised instead of being tolerated.
pub const ManifestError = error{NonAsciiManifest};

/// Python's `str.isspace` over the ASCII range, which is what `str.strip`
/// removes: the C0 whitespace block plus the four information separators
/// (0x1c-0x1f) and the space.
pub fn isPythonSpace(byte: u8) bool {
    return switch (byte) {
        0x09...0x0d, 0x1c...0x1f, 0x20 => true,
        else => false,
    };
}

/// Where Python's `str.splitlines` breaks, over the ASCII range. Note 0x1f is
/// whitespace but NOT a line break, while 0x1c-0x1e are both.
pub fn isLineBreak(byte: u8) bool {
    return switch (byte) {
        0x0a, 0x0b, 0x0c, 0x0d, 0x1c, 0x1d, 0x1e => true,
        else => false,
    };
}

/// `str.splitlines` over ASCII text: CRLF counts once, a trailing break does
/// not yield a final empty line.
pub const LineIterator = struct {
    text: []const u8,
    index: usize = 0,

    pub fn next(self: *LineIterator) ?[]const u8 {
        if (self.index >= self.text.len) return null;
        const start = self.index;
        var cursor = start;
        while (cursor < self.text.len and !isLineBreak(self.text[cursor])) cursor += 1;
        const line = self.text[start..cursor];
        if (cursor < self.text.len) {
            const crlf = self.text[cursor] == '\r' and
                cursor + 1 < self.text.len and
                self.text[cursor + 1] == '\n';
            cursor += if (crlf) 2 else 1;
        }
        self.index = cursor;
        return line;
    }
};

/// Python's `str.strip()` with no argument, over ASCII text.
pub fn strip(line: []const u8) []const u8 {
    var start: usize = 0;
    var end: usize = line.len;
    while (start < end and isPythonSpace(line[start])) start += 1;
    while (end > start and isPythonSpace(line[end - 1])) end -= 1;
    return line[start..end];
}

/// True when every byte is ASCII, i.e. when `bytes.decode("ascii")` succeeds.
pub fn isAscii(text: []const u8) bool {
    for (text) |byte| {
        if (byte >= 0x80) return false;
    }
    return true;
}

/// One newline-delimited manifest's entries, blank lines dropped, in file
/// order. The returned slices borrow `text`.
///
/// A non-ASCII byte is an error rather than a tolerated entry: the Python
/// read these files as ASCII and raised on anything else, and a gate that
/// silently accepted a mojibake configuration name would compare the wrong
/// set.
pub fn parseManifest(
    allocator: std.mem.Allocator,
    text: []const u8,
) (std.mem.Allocator.Error || ManifestError)![][]const u8 {
    if (!isAscii(text)) return error.NonAsciiManifest;
    var entries = std.ArrayList([]const u8).init(allocator);
    errdefer entries.deinit();
    var lines = LineIterator{ .text = text };
    while (lines.next()) |line| {
        const trimmed = strip(line);
        if (trimmed.len == 0) continue;
        try entries.append(trimmed);
    }
    return entries.toOwnedSlice();
}

/// A code point of a filesystem name as Python sees it: valid UTF-8 decodes,
/// and any other byte becomes U+DC00+byte, which is the surrogateescape
/// handler `os.scandir` decodes names with. Sorting has to agree with that,
/// because `sorted()` compares code points, not bytes.
const CodePoints = struct {
    text: []const u8,
    index: usize = 0,

    fn next(self: *CodePoints) ?u21 {
        if (self.index >= self.text.len) return null;
        const first = self.text[self.index];
        if (first < 0x80) {
            self.index += 1;
            return first;
        }
        const length = std.unicode.utf8ByteSequenceLength(first) catch {
            self.index += 1;
            return @as(u21, 0xDC00) + first;
        };
        if (self.index + length > self.text.len) {
            self.index += 1;
            return @as(u21, 0xDC00) + first;
        }
        const point = std.unicode.utf8Decode(self.text[self.index..][0..length]) catch {
            self.index += 1;
            return @as(u21, 0xDC00) + first;
        };
        self.index += length;
        return point;
    }
};

/// Python's `str` ordering of two names.
pub fn pythonOrder(left: []const u8, right: []const u8) std.math.Order {
    var a = CodePoints{ .text = left };
    var b = CodePoints{ .text = right };
    while (true) {
        const next_a = a.next();
        const next_b = b.next();
        if (next_a == null and next_b == null) return .eq;
        if (next_a == null) return .lt;
        if (next_b == null) return .gt;
        if (next_a.? != next_b.?) return if (next_a.? < next_b.?) .lt else .gt;
    }
}

/// `sorted()` over configuration names.
pub fn pythonLessThan(_: void, left: []const u8, right: []const u8) bool {
    return pythonOrder(left, right) == .lt;
}

/// Sort names the way `sorted()` would.
pub fn sortNames(names: [][]const u8) void {
    std.mem.sort([]const u8, names, {}, pythonLessThan);
}

/// List equality, which is what `read_manifest(...) != expected` tested.
pub fn sameList(left: []const []const u8, right: []const []const u8) bool {
    if (left.len != right.len) return false;
    for (left, right) |a, b| {
        if (!std.mem.eql(u8, a, b)) return false;
    }
    return true;
}

/// True when an examples-relative app directory is a build configuration: at
/// least a tier and an app, and not the shared tier.
pub fn isExampleSelected(parts: []const []const u8) bool {
    if (parts.len < min_example_path_parts) return false;
    return !std.mem.eql(u8, parts[0], shared_tier);
}

/// Split a slash-separated relative path into its parts, dropping empty and
/// `.` segments the way pathlib does.
pub fn splitParts(
    allocator: std.mem.Allocator,
    relative: []const u8,
) std.mem.Allocator.Error![][]const u8 {
    var parts = std.ArrayList([]const u8).init(allocator);
    errdefer parts.deinit();
    var it = std.mem.splitScalar(u8, relative, '/');
    while (it.next()) |part| {
        if (part.len == 0 or std.mem.eql(u8, part, ".")) continue;
        try parts.append(part);
    }
    return parts.toOwnedSlice();
}

/// `"::".join(rel.parts)`.
pub fn exampleConfig(
    allocator: std.mem.Allocator,
    parts: []const []const u8,
) std.mem.Allocator.Error![]const u8 {
    return std.mem.join(allocator, separator, parts);
}

/// The board product name for a stand_alone-relative directory: the e-reader
/// is renamed to its board identifier, everything else keeps its own last
/// path component.
pub fn boardName(relative: []const u8) []const u8 {
    if (std.mem.eql(u8, relative, ereader_rel)) return ereader_name;
    const last = std.mem.lastIndexOfScalar(u8, relative, '/');
    return if (last) |at| relative[at + 1 ..] else relative;
}

/// `f"board::stand_alone::{name}"`.
pub fn boardConfig(
    allocator: std.mem.Allocator,
    relative: []const u8,
) std.mem.Allocator.Error![]const u8 {
    return std.fmt.allocPrint(allocator, "{s}{s}", .{ board_prefix, boardName(relative) });
}

/// True when this board directory also requires its Non-Secure XIP variant.
pub fn requiresNsXip(relative: []const u8) bool {
    return std.mem.eql(u8, relative, ereader_rel);
}

/// `str(Path(raw))`: duplicate separators collapse, `.` segments drop, a
/// trailing separator goes, an empty path is `.`, and POSIX keeps exactly two
/// leading slashes while three or more collapse to one.
pub fn normalizePath(
    allocator: std.mem.Allocator,
    raw: []const u8,
) std.mem.Allocator.Error![]const u8 {
    var leading: usize = 0;
    while (leading < raw.len and raw[leading] == '/') leading += 1;
    const root: []const u8 = switch (leading) {
        0 => "",
        2 => "//",
        else => "/",
    };

    var text = std.ArrayList(u8).init(allocator);
    errdefer text.deinit();
    try text.appendSlice(root);

    var wrote = false;
    var it = std.mem.splitScalar(u8, raw[leading..], '/');
    while (it.next()) |part| {
        if (part.len == 0 or std.mem.eql(u8, part, ".")) continue;
        if (wrote) try text.append('/');
        try text.appendSlice(part);
        wrote = true;
    }
    if (!wrote and root.len == 0) try text.append('.');
    return text.toOwnedSlice();
}

/// `Path(base) / rest`, rendered as `str()` would render it.
pub fn joinPath(
    allocator: std.mem.Allocator,
    base: []const u8,
    rest: []const u8,
) std.mem.Allocator.Error![]const u8 {
    const normalized = try normalizePath(allocator, base);
    defer allocator.free(normalized);
    if (rest.len == 0) return allocator.dupe(u8, normalized);
    if (std.mem.eql(u8, normalized, ".")) return normalizePath(allocator, rest);
    const separator_needed = normalized[normalized.len - 1] != '/';
    return std.fmt.allocPrint(allocator, "{s}{s}{s}", .{
        normalized,
        if (separator_needed) "/" else "",
        rest,
    });
}

/// The per-shard manifest file name for shard `index` of `shards`.
pub fn shardFileName(
    allocator: std.mem.Allocator,
    index: usize,
    shards: usize,
) std.mem.Allocator.Error![]const u8 {
    return std.fmt.allocPrint(allocator, "shard-{d}-of-{d}.txt", .{ index, shards });
}

/// One shard's claimed configurations, in file order.
pub const Shard = struct {
    /// 1-based shard number, as the manifest name spells it.
    index: usize,
    apps: []const []const u8,
};

/// `_audit_shard_contents`: the duplicate claims first, in the order the
/// shards are read, then the two count lines when the union disagrees with
/// the discovered truth.
///
/// A configuration claimed twice is reported against the FIRST shard that
/// claimed it, and the later claim is not recorded, exactly as the Python's
/// `seen` dict kept the first writer.
pub fn auditShardContents(
    allocator: std.mem.Allocator,
    shards: []const Shard,
    expected: []const []const u8,
    problems: *std.ArrayList([]const u8),
) std.mem.Allocator.Error!void {
    var seen = std.StringHashMap(usize).init(allocator);
    defer seen.deinit();
    var keys = std.ArrayList([]const u8).init(allocator);
    defer keys.deinit();

    for (shards) |shard| {
        for (shard.apps) |app| {
            if (seen.get(app)) |first| {
                try problems.append(try std.fmt.allocPrint(
                    allocator,
                    "app '{s}' claimed by both shard {d} and shard {d}",
                    .{ app, first, shard.index },
                ));
            } else {
                try seen.put(app, shard.index);
                try keys.append(app);
            }
        }
    }

    sortNames(keys.items);
    if (sameList(keys.items, expected)) return;

    var missing: usize = 0;
    for (expected) |app| {
        if (!seen.contains(app)) missing += 1;
    }
    var extra: usize = 0;
    for (keys.items) |app| {
        if (!containsName(expected, app)) extra += 1;
    }

    if (missing > 0) {
        try problems.append(try std.fmt.allocPrint(
            allocator,
            "{d} firmware configuration(s) never built by any shard",
            .{missing},
        ));
    }
    if (extra > 0) {
        try problems.append(try std.fmt.allocPrint(
            allocator,
            "{d} configuration(s) claimed in manifests are not structural",
            .{extra},
        ));
    }
}

/// Membership over a name list.
pub fn containsName(names: []const []const u8, needle: []const u8) bool {
    for (names) |name| {
        if (std.mem.eql(u8, name, needle)) return true;
    }
    return false;
}

/// The line a clean run prints on stdout.
pub fn renderCleanLine(
    allocator: std.mem.Allocator,
    shards: usize,
    configs: usize,
) std.mem.Allocator.Error![]const u8 {
    return std.fmt.allocPrint(
        allocator,
        "{s}: {d} shard(s) covered all {d} firmware configuration(s) exactly once.",
        .{ tool_name, shards, configs },
    );
}

/// `f"shard manifest directory {shard_dir} does not exist."`
pub fn renderMissingShardDir(
    allocator: std.mem.Allocator,
    shard_dir: []const u8,
) std.mem.Allocator.Error![]const u8 {
    return std.fmt.allocPrint(
        allocator,
        "shard manifest directory {s} does not exist.",
        .{shard_dir},
    );
}

/// `f"missing {all_configs_file}"`, which names the whole path.
pub fn renderMissingAllConfigs(
    allocator: std.mem.Allocator,
    path: []const u8,
) std.mem.Allocator.Error![]const u8 {
    return std.fmt.allocPrint(allocator, "missing {s}", .{path});
}

/// `f"missing shard manifest {path.name}"`, which names the file only.
pub fn renderMissingShard(
    allocator: std.mem.Allocator,
    name: []const u8,
) std.mem.Allocator.Error![]const u8 {
    return std.fmt.allocPrint(allocator, "missing shard manifest {s}", .{name});
}
