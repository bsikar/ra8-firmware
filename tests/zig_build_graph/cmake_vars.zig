//! SPDX-License-Identifier: MIT
//!
//! Read `set(NAME value)` out of a board memory-map .cmake at configure time.
//!
//! The CPU1 image window is defined once, in
//! libs/ra8_board_ek_ra8d2/ld/cpu1_memory_map.cmake, because #742 pulled those
//! two addresses out of nine hand-maintained linker scripts. ra8_add_app()
//! reaches them with include(); this reads the same file rather than copying
//! the numbers into Zig, so the board layer stays the one definition and a
//! silicon-driven change cannot leave the two graphs disagreeing.
//!
//! Deliberately not a CMake parser. It understands `set(NAME value)` on one
//! line, with or without quotes, and `${NAME}` expansion against what it has
//! already read. Anything else in the file is skipped, so a module that grows
//! a function or an if() still yields its variables.

const std = @import("std");

pub const Vars = struct {
    map: std.StringHashMap([]const u8),

    /// The value of `name`, or a build-stopping error naming the file: a
    /// missing variable means the board module was renamed or restructured,
    /// and silently substituting an empty string would place .cpu1_image at
    /// address 0.
    pub fn get(self: Vars, name: []const u8, source: []const u8) []const u8 {
        return self.map.get(name) orelse std.debug.panic(
            "{s} does not define {s}; the CPU1 image window moved",
            .{ source, name },
        );
    }
};

/// Parse every one-line `set()` in `text`, expanding `${...}` against the
/// variables already seen above it, which is the order CMake itself resolves.
pub fn parse(allocator: std.mem.Allocator, text: []const u8) Vars {
    var vars = Vars{ .map = std.StringHashMap([]const u8).init(allocator) };

    var lines = std.mem.splitScalar(u8, text, '\n');
    while (lines.next()) |raw| {
        const line = std.mem.trim(u8, raw, " \t\r");
        if (!std.mem.startsWith(u8, line, "set(")) continue;
        if (!std.mem.endsWith(u8, line, ")")) continue;

        const body = line["set(".len .. line.len - 1];
        const split = std.mem.indexOfScalar(u8, body, ' ') orelse continue;
        const name = body[0..split];
        const value = std.mem.trim(u8, body[split + 1 ..], " \t\"");

        vars.map.put(name, expand(allocator, vars, value)) catch @panic("OOM");
    }
    return vars;
}

/// `${NAME}` -> its value. An unknown name is left as written rather than
/// blanked, so it reaches the linker as a visible error instead of as a hole.
fn expand(allocator: std.mem.Allocator, vars: Vars, value: []const u8) []const u8 {
    var out = std.ArrayList(u8).init(allocator);
    var rest = value;
    while (std.mem.indexOf(u8, rest, "${")) |open| {
        const close = std.mem.indexOfScalarPos(u8, rest, open, '}') orelse break;
        out.appendSlice(rest[0..open]) catch @panic("OOM");
        const name = rest[open + 2 .. close];
        out.appendSlice(vars.map.get(name) orelse rest[open .. close + 1]) catch @panic("OOM");
        rest = rest[close + 1 ..];
    }
    out.appendSlice(rest) catch @panic("OOM");
    return out.items;
}
