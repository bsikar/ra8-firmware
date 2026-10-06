//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which library named in an app's `LIBS` contributes a Zig ARCHIVE, decided
//! by the rule rather than by a hand-kept list.
//!
//! `cmake/ra8_app/sources.cmake` holds the whole rule, and it runs over every
//! `LIBS` entry:
//!
//!     if(EXISTS "${_ra8_lib_path}/build.zig")
//!       list(APPEND _ra8_lib_zig "${_ra8_lib}|${_ra8_lib_path}")
//!
//! A `build.zig` alone is the test. The rule once also required the primary
//! `src/<lib>.c` to be gone, on the theory that a library mid-port keeps a
//! complete C implementation for ARM. Ports do not work that way: each slice
//! deletes the C it replaces, so the C that remains calls into Zig.
//! `ra8_c6link` kept `src/ra8_c6link.c` while its `priv_c6link_*` helpers and
//! handle lifecycle moved to Zig, and that second clause left every app naming
//! it without them at link.
//!
//! The Zig graph had the OUTPUT of this rule hand-written into
//! `app_table.zig`'s `zig_libraries` instead of the rule itself: eleven of the
//! twelve apps carried an empty list and the twelfth carried one name. Applying
//! the rule to the libraries the table already names turns up six that qualify,
//! so five archives were missing from the link and nothing could notice,
//! because a hand-kept copy of a derived set cannot drift loudly. Deriving it
//! here means a library joins the link by gaining a `build.zig`, exactly as it
//! does under CMake, with no second edit in this graph.
//!
//! `zig_libraries` stays as the explicit escape hatch for a library this rule
//! cannot see; it is unioned with what this returns.

const std = @import("std");
const src_tree = @import("src_tree.zig");

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
        if (src_tree.exists(b, candidate)) return candidate;
    }
    return null;
}

/// The rule itself: a `build.zig` present.
pub fn contributesArchive(b: *std.Build, name: []const u8) bool {
    const path = pathFor(b, name) orelse return false;
    const build_file = b.fmt("{s}/build.zig", .{path});
    return src_tree.exists(b, build_file);
}
