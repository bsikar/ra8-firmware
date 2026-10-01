//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which library named in an app's `LIBS` contributes a Zig ARCHIVE, decided
//! by the rule rather than by a hand-kept list.
//!
//! `cmake/ra8_app/sources.cmake:371` is the whole rule, and it runs over every
//! `LIBS` entry:
//!
//!     if(EXISTS "${_ra8_lib_path}/build.zig"
//!        AND NOT EXISTS "${_ra8_lib_path}/src/${_ra8_lib}.c")
//!       list(APPEND _ra8_lib_zig "${_ra8_lib}|${_ra8_lib_path}")
//!
//! with the comment that fixes the meaning of the second clause: "A first-half
//! port retains its primary C implementation for ARM. The later ARM flip
//! removes that file; support C sources may remain. Only then link the Zig
//! archive beside any support C objects." So the presence of `build.zig` alone
//! is NOT the test. A library mid-port still has `src/<lib>.c` and must keep
//! linking C, and the archive joins the link only once that file is gone,
//! sitting beside whatever support `.c` the library still has.
//!
//! The Zig graph had the OUTPUT of this rule hand-written into
//! `app_table.zig`'s `zig_libraries` instead of the rule itself: eleven of the
//! twelve apps carried an empty list and the twelfth carried one name. Applying
//! the rule to the libraries the table already names turns up six that qualify,
//! so five archives were missing from the link and nothing could notice,
//! because a hand-kept copy of a derived set cannot drift loudly. Deriving it
//! here means a library finishing its ARM flip joins the link by deleting its
//! last primary `.c`, exactly as it does under CMake, with no second edit in
//! this graph.
//!
//! `zig_libraries` stays as the explicit escape hatch for a library this rule
//! cannot see; it is unioned with what this returns.

const std = @import("std");

/// Where a `LIBS` name resolves on disk, in the order sources.cmake tries:
/// `libs/<name>` first, then `apps/shared_libs/<name>`. Null when neither
/// exists, which is the `ra8_io_bus` / `threadx` case: a LIBS name with no
/// directory of its own, handled elsewhere and contributing no archive.
pub fn pathFor(b: *std.Build, name: []const u8) ?[]const u8 {
    const candidates = [_][]const u8{
        b.fmt("libs/{s}", .{name}),
        b.fmt("apps/shared_libs/{s}", .{name}),
    };
    for (candidates) |candidate| {
        if (b.build_root.handle.access(candidate, .{})) |_| return candidate else |_| {}
    }
    return null;
}

/// The rule itself: a `build.zig` present and the library's primary
/// implementation `src/<name>.c` absent.
pub fn contributesArchive(b: *std.Build, name: []const u8) bool {
    const path = pathFor(b, name) orelse return false;
    const build_file = b.fmt("{s}/build.zig", .{path});
    const has_build = if (b.build_root.handle.access(build_file, .{})) |_| true else |_| false;
    if (!has_build) return false;
    const primary = b.fmt("{s}/src/{s}.c", .{ path, name });
    const has_primary = if (b.build_root.handle.access(primary, .{})) |_| true else |_| false;
    return !has_primary;
}
