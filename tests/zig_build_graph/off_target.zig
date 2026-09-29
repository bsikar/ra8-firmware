//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_add_app(OFF_TARGET_LIBS <lib>...)`: the one source-set rule that
//! compiles a single executable at TWO preprocessor views (#1133).
//!
//! Extracted into its own module because cross_sources.zig sits at the
//! 1000-line ceiling scripts/checks/check_file_size.py holds every Zig source
//! to, and this is the coherent piece: what the keyword does to the units it
//! names, as opposed to the source-set rules every unit in an app shares.
//!
//! cmake/ra8_app/sources.cmake collects these libraries into a list of their
//! own (`_ra8_lib_extra_off_target`) and cmake/ra8_add_app.cmake then hangs
//! `COMPILE_DEFINITIONS "RA8_OFF_TARGET"` on exactly those source files with
//! set_source_files_properties(), so the define is SOURCE-scope. The include
//! directory is not: it is appended to the same `_ra8_lib_inc` the LIBS loop
//! feeds, and therefore lands on every translation unit in the app.

const std = @import("std");

/// The define `OFF_TARGET_LIBS` puts on the named libraries' own translation
/// units and on nothing else.
///
/// It is the whole point of the keyword. `libs/ra8_psa_crypto` is
/// `#ifndef RA8_OFF_TARGET` / `#ifdef RA8_OFF_TARGET` from top to bottom: with
/// the define its bodies are the deterministic host-side ones, without it the
/// hardware-backed ones that call into TF-PSA-Crypto.
///
/// MEASURED, both arms, on crypto_aes_demo under arm-none-eabi-gcc 13.3.1 at
/// this app's own flags and include path: with the define both units compile;
/// WITHOUT it both fail outright, `ra8_psa_crypto_internal.h:44: fatal error:
/// psa/crypto.h: No such file or directory`, because the on-target arm reaches
/// for the vendored TF-PSA-Crypto headers and THIS app's include path has no
/// tf-psa-crypto in it. So for an app whose only crypto is the off-target
/// library, a graph that drops the define fails closed rather than silently.
///
/// The silent case is the app that carries those headers anyway (the shape
/// secure_boot_hil's own vendored tfpsa_sb library sets up, see app_local.zig):
/// there both arms compile, the image links either way, and it simply stops
/// being the image CMake builds. That is why the set is declared data here
/// rather than inferred from a directory listing, and why both arms are
/// asserted in tests/zig_build_graph/build_graph_test.zig.
pub const off_target_define = "-DRA8_OFF_TARGET";

/// True when `source` belongs to one of the declared `OFF_TARGET_LIBS`, i.e.
/// when it is one of the translation units that takes the define above. Takes
/// the declared set rather than a CrossApp so this module stays a leaf that
/// cross_sources.zig can import.
pub fn isOffTargetSource(libraries: []const []const u8, source: []const u8) bool {
    for (libraries) |library| {
        var buffer: [256]u8 = undefined;
        const prefix = std.fmt.bufPrint(&buffer, "libs/{s}/src/", .{library}) catch continue;
        if (std.mem.startsWith(u8, source, prefix)) return true;
    }
    return false;
}

/// Every translation unit the declared libraries contribute, appended in the
/// order the app names them. LAST of the whole source list, because
/// cmake/ra8_add_app.cmake spells `${_ra8_lib_extra_off_target}` after
/// `${_ra8_lib_extra}` in the add_executable() call, which is also the order
/// the objects reach the linker.
pub fn appendSources(b: *std.Build, libraries: []const []const u8, out: *std.ArrayList([]const u8), collect: fn (*std.Build, []const u8, *std.ArrayList([]const u8)) void) void {
    for (libraries) |library| {
        const dir = b.fmt("libs/{s}/src", .{library});
        const exists = if (b.build_root.handle.access(dir, .{})) |_| true else |_| false;
        if (exists) collect(b, dir, out);
    }
}

/// Every `inc` those libraries put on the app's include path.
pub fn appendIncludeDirs(b: *std.Build, libraries: []const []const u8, out: *std.ArrayList([]const u8)) void {
    for (libraries) |library| {
        const dir = b.fmt("libs/{s}/inc", .{library});
        const exists = if (b.build_root.handle.access(dir, .{})) |_| true else |_| false;
        if (exists) out.append(dir) catch @panic("OOM");
    }
}

test "the define is the one CMake hangs on those sources, and it is scoped to them" {
    try std.testing.expectEqualStrings("-DRA8_OFF_TARGET", off_target_define);
    const declared: []const []const u8 = &.{"ra8_psa_crypto"};
    try std.testing.expect(isOffTargetSource(declared, "libs/ra8_psa_crypto/src/ra8_psa_crypto.c"));
    try std.testing.expect(!isOffTargetSource(declared, "libs/ra8_psa_crypto/inc/ra8_psa_crypto.h"));
    try std.testing.expect(!isOffTargetSource(declared, "libs/ra8_core/src/ra8_log.c"));
    try std.testing.expect(!isOffTargetSource(&.{}, "libs/ra8_psa_crypto/src/ra8_psa_crypto.c"));
}
