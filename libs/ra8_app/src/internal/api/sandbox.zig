//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The storage sandbox: a module names a file relative to its own directory,
//! and the kernel turns that into `/apps/<app-id>/<path>`. The backend only
//! ever sees the joined path, so a module cannot name anything outside its
//! directory, whatever the filesystem would make of `..` or a leading `/`.

const std = @import("std");
const abi = @import("abi.zig");

/// The directory every module's files live under.
pub const root = "/apps/";

/// Longest app id the joined path has room for: the `.ra8app` header's field,
/// less its NUL.
pub const app_id_max = 31;

/// Room for `/apps/<app-id>/<path>`.
pub const joined_max = root.len + app_id_max + 1 + abi.path_max;

pub const Error = error{BadPath};

/// Whether `name` is one path component a module may use: printable ASCII,
/// none of `/`, `\` or `:`, and neither `.` nor `..`.
fn component(name: []const u8) bool {
    if (name.len == 0) return false;
    if (std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..")) return false;
    for (name) |byte| {
        if (byte < 0x21 or byte > 0x7e) return false;
        if (byte == '/' or byte == '\\' or byte == ':') return false;
    }
    return true;
}

/// Check a module's relative path: 1 to `path_max` bytes, components joined by
/// single `/`, no leading or trailing `/`.
pub fn check(path: []const u8) Error!void {
    if (path.len == 0 or path.len > abi.path_max) return Error.BadPath;
    var parts = std.mem.splitScalar(u8, path, '/');
    while (parts.next()) |part| {
        if (!component(part)) return Error.BadPath;
    }
}

/// Write `/apps/<app_id>/<path>` into `out` and return it. `app_id` comes from
/// the admitted header, not from the module, but it is checked the same way:
/// one component.
pub fn join(out: *[joined_max]u8, app_id: []const u8, path: []const u8) Error![]const u8 {
    try check(path);
    if (app_id.len > app_id_max or !component(app_id)) return Error.BadPath;
    var len: usize = 0;
    for ([_][]const u8{ root, app_id, "/", path }) |piece| {
        @memcpy(out[len..][0..piece.len], piece);
        len += piece.len;
    }
    return out[0..len];
}
