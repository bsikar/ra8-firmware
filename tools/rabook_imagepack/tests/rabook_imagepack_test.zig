//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig host-side tests for the thin `tools/rabook_imagepack` CLI wrapper and
//! the build-graph/ABI contract tying it to `libs/ra8_fmt`.

const std = @import("std");

fn readPackageOrRepoFile(
    allocator: std.mem.Allocator,
    pkg_rel: []const u8,
    repo_rel: []const u8,
) ![]u8 {
    const dir = std.Io.Dir.cwd();
    return dir.readFileAlloc(std.testing.io, pkg_rel, allocator, .limited(1024 * 1024)) catch
        try dir.readFileAlloc(std.testing.io, repo_rel, allocator, .limited(1024 * 1024));
}

test "stream headers declare portable IO and streaming engine signatures" {
    const allocator = std.testing.allocator;
    const fmt_io_h = try readPackageOrRepoFile(allocator, "../../libs/ra8_fmt/inc/ra8_fmt_io.h", "libs/ra8_fmt/inc/ra8_fmt_io.h");
    defer allocator.free(fmt_io_h);
    const fmt_stream_h = try readPackageOrRepoFile(allocator, "../../libs/ra8_fmt/inc/ra8_fmt_stream.h", "libs/ra8_fmt/inc/ra8_fmt_stream.h");
    defer allocator.free(fmt_stream_h);

    try std.testing.expect(std.mem.indexOf(u8, fmt_io_h, "ra8_fmt_source_t") != null);
    try std.testing.expect(std.mem.indexOf(u8, fmt_io_h, "ra8_fmt_sink_t") != null);
    try std.testing.expect(std.mem.indexOf(u8, fmt_io_h, "ra8_fmt_spool_t") != null);
    try std.testing.expect(std.mem.indexOf(u8, fmt_io_h, "ra8_fmt_transaction_t") != null);
    try std.testing.expect(std.mem.indexOf(u8, fmt_stream_h, "ra8_fmt_jof_inspect_stream") != null);
    try std.testing.expect(std.mem.indexOf(u8, fmt_stream_h, "ra8_fmt_rabook_inspect_stream") != null);
    try std.testing.expect(std.mem.indexOf(u8, fmt_stream_h, "ra8_fmt_jof_convert_requirements") != null);
    try std.testing.expect(std.mem.indexOf(u8, fmt_stream_h, "ra8_fmt_jof_convert_stream") != null);
    try std.testing.expect(std.mem.indexOf(u8, fmt_stream_h, "ra8_fmt_jof_verify_requirements") != null);
    try std.testing.expect(std.mem.indexOf(u8, fmt_stream_h, "ra8_fmt_jof_verify_stream") != null);
}

test "tool sources remain a thin POSIX CLI adapter without engine duplication" {
    const allocator = std.testing.allocator;
    const cli_main_c = try readPackageOrRepoFile(allocator, "src/main.c", "tools/rabook_imagepack/src/main.c");
    defer allocator.free(cli_main_c);
    const portable_main_c = try readPackageOrRepoFile(allocator, "src/ra8_fmt_portable_main.c", "tools/rabook_imagepack/src/ra8_fmt_portable_main.c");
    defer allocator.free(portable_main_c);

    try std.testing.expect(std.mem.indexOf(u8, cli_main_c, "ra8_fmt_portable_main") != null);
    try std.testing.expect(std.mem.indexOf(u8, portable_main_c, "ra8_fmt_stream.h") != null);
    try std.testing.expect(std.mem.indexOf(u8, cli_main_c, "jof_produce") == null);
    try std.testing.expect(std.mem.indexOf(u8, portable_main_c, "jof_produce") == null);
}

test "production surface gate enforces non-vacuity and forbids heap/stdio APIs" {
    const allocator = std.testing.allocator;
    const production_surface_sh = try readPackageOrRepoFile(allocator, "tests/check_production_surface.sh", "tools/rabook_imagepack/tests/check_production_surface.sh");
    defer allocator.free(production_surface_sh);

    try std.testing.expect(std.mem.indexOf(u8, production_surface_sh, "scanned=$(find") != null);
    try std.testing.expect(std.mem.indexOf(u8, production_surface_sh, "malloc|calloc|realloc|free") != null);
    try std.testing.expect(std.mem.indexOf(u8, production_surface_sh, "fopen|fdopen|freopen|fclose") != null);
    try std.testing.expect(std.mem.indexOf(u8, production_surface_sh, "nm -u") != null);
}

test "CLI command subcommands cover inspect, convert, and verify paths" {
    const allocator = std.testing.allocator;
    const portable_main_c = try readPackageOrRepoFile(allocator, "src/ra8_fmt_portable_main.c", "tools/rabook_imagepack/src/ra8_fmt_portable_main.c");
    defer allocator.free(portable_main_c);
    const portable_convert_c = try readPackageOrRepoFile(allocator, "src/ra8_fmt_portable_convert.c", "tools/rabook_imagepack/src/ra8_fmt_portable_convert.c");
    defer allocator.free(portable_convert_c);
    const portable_verify_c = try readPackageOrRepoFile(allocator, "src/ra8_fmt_portable_verify.c", "tools/rabook_imagepack/src/ra8_fmt_portable_verify.c");
    defer allocator.free(portable_verify_c);

    try std.testing.expect(std.mem.indexOf(u8, portable_main_c, "\"inspect\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, portable_convert_c, "\"convert\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, portable_verify_c, "\"verify\"") != null);
}

test "CLI adapter avoids forbidden allocator and FILE tokens" {
    const allocator = std.testing.allocator;
    const cli_main_c = try readPackageOrRepoFile(allocator, "src/main.c", "tools/rabook_imagepack/src/main.c");
    defer allocator.free(cli_main_c);
    const portable_main_c = try readPackageOrRepoFile(allocator, "src/ra8_fmt_portable_main.c", "tools/rabook_imagepack/src/ra8_fmt_portable_main.c");
    defer allocator.free(portable_main_c);
    const portable_convert_c = try readPackageOrRepoFile(allocator, "src/ra8_fmt_portable_convert.c", "tools/rabook_imagepack/src/ra8_fmt_portable_convert.c");
    defer allocator.free(portable_convert_c);
    const portable_verify_c = try readPackageOrRepoFile(allocator, "src/ra8_fmt_portable_verify.c", "tools/rabook_imagepack/src/ra8_fmt_portable_verify.c");
    defer allocator.free(portable_verify_c);

    const sources = [_][]const u8{ cli_main_c, portable_main_c, portable_convert_c, portable_verify_c };
    const forbidden = [_][]const u8{ "malloc(", "calloc(", "realloc(", "fopen(", "fprintf(" };
    for (sources) |src| {
        for (forbidden) |token| {
            try std.testing.expect(std.mem.indexOf(u8, src, token) == null);
        }
    }
}

test "stream headers preserve C++ extern guard and pragma once" {
    const allocator = std.testing.allocator;
    const fmt_io_h = try readPackageOrRepoFile(allocator, "../../libs/ra8_fmt/inc/ra8_fmt_io.h", "libs/ra8_fmt/inc/ra8_fmt_io.h");
    defer allocator.free(fmt_io_h);
    const fmt_stream_h = try readPackageOrRepoFile(allocator, "../../libs/ra8_fmt/inc/ra8_fmt_stream.h", "libs/ra8_fmt/inc/ra8_fmt_stream.h");
    defer allocator.free(fmt_stream_h);

    try std.testing.expect(std.mem.indexOf(u8, fmt_io_h, "#pragma once") != null);
    try std.testing.expect(std.mem.indexOf(u8, fmt_stream_h, "#pragma once") != null);
    try std.testing.expect(std.mem.indexOf(u8, fmt_stream_h, "extern \"C\"") != null);
}
