//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The selected board's Zig archive, which every image for that board links.
//!
//! `cmake/ra8_app/sources.cmake:276` registers it the moment the board layer
//! has a `build.zig`, with no opt-in and no dependence on `LIBS`:
//!
//!     list(APPEND _ra8_lib_zig "ra8_board_${_RA8_APP_BOARD}|${_ra8_board_dir}")
//!
//! and both boards in the tree have one. The surrounding comment spells out
//! why the registration is unconditional rather than gated on the board being
//! fully ported: a PARTIALLY ported board links the archive BESIDE its
//! remaining C objects, and gating on the absence of board `.c` "silently
//! dropped the ported half of such a board out of the link" (#2998).
//!
//! That is exactly the state the Zig cross graph was in. It globs the board's
//! `src/*.c` (cross_sources.zig, the board layer line) but linked no board
//! archive at all, so every board symbol that has already moved to Zig was
//! undefined at link: `ra8_board_uart_console_write`, `ra8_board_led_on` /
//! `_off` / `_toggle` / `_pin`, `ra8_board_clock` and the rest of
//! `ra8_board_ek_ra8d2_abi.zig`, none of which has a `.c` definition left
//! anywhere in the tree.
//!
//! The archive is built for the CONSUMING image's cpu, not the app's, for the
//! reason core_archive.zig records: `_ra8_zig_build_archive()` keys its output
//! on `(library, cpu)` precisely because a dual-core app's M33 image must not
//! be handed the M85 archive.

const std = @import("std");

/// `libs/ra8_board_ek_ra8d2` -> `ra8_board_ek_ra8d2`. The directory basename
/// IS the dependency name, the artifact name and the archive basename, the
/// same identity core_archive.zig relies on; CMake builds the same string the
/// same way, out of `_RA8_APP_BOARD`.
pub fn nameFor(board_dir: []const u8) []const u8 {
    return std.fs.path.basename(board_dir);
}

/// Whether this board layer ships a Zig half at all. Mirrors the
/// `if(EXISTS "${_ra8_board_dir}/build.zig")` that guards the registration:
/// a board with no `build.zig` is pure C and contributes no archive.
pub fn has(b: *std.Build, board_dir: []const u8) bool {
    const build_file = b.fmt("{s}/build.zig", .{board_dir});
    return if (b.build_root.handle.access(build_file, .{})) |_| true else |_| false;
}

/// The board archive for an image whose target the caller already resolved.
pub fn forTarget(
    b: *std.Build,
    board_dir: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) std.Build.LazyPath {
    const name = nameFor(board_dir);
    const dependency = b.dependency(name, .{
        .target = target,
        .optimize = optimize,
    });
    return dependency.artifact(name).getEmittedBin();
}
