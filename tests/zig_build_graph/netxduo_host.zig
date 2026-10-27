//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ra8_ipif host harness over the vendored ThreadX Linux port and NetX Duo
//! (RA8FW-1065, under RA8FW-360's host path).
//!
//! RA8FW-360's proof ran this stack by hand: the vendored ThreadX and NetX
//! trees, the ra8_ipif port glue and a fourteen-check harness, exit 0. The
//! re-derivation after the slice-5 landing was where RA8FW-1065 surfaced: the
//! vendored ThreadX Linux port hard-codes a 32-bit ULONG under `__x86_64__`
//! (tx_port.h), truncating every kernel-object pointer NetX receives on an
//! x86_64 host build. The visible shape is NX_PTR_ERROR from nx_ip_create on
//! a reused control block and NX_NOT_INITIALIZED from everything after it,
//! which reads as an ra8_ipif defect and is not one.
//!
//! The fix is the RA8FW-484 pattern: the build.zig.zon pins stay, and a
//! generated copy of tx_port.h with the one line rewritten shadows the
//! vendored header on this slice's include path. The rewrite is whole-line
//! and unique in the file, so a vendored bump that moves or reformats the
//! line fails the build here instead of quietly truncating pointers again.
//!
//! `zig build netxduo-host` compiles the whole host stack through zig's own C
//! driver and runs the harness: fourteen PASS lines and exit 0 are the proof.

const std = @import("std");
const middleware = @import("middleware.zig");
const pkg_path = @import("pkg_path.zig");

pub const step_description = "Run the ra8_ipif host harness over the vendored ThreadX Linux port and NetX Duo";

/// The vendored ThreadX Linux port's `__x86_64__` branch (tx_port.h), the one
/// line the defect turns on. The else branch beside it already declares
/// `unsigned long`, so the generated copy only widens the `__x86_64__` one.
pub const tx_port_header = middleware.HeaderPatch{
    .header = "pkg:threadx/ports/linux/gnu/inc/tx_port.h",
    .rewrites = &.{.{
        .old = "typedef unsigned int                            ULONG;",
        .new = "typedef unsigned long                           ULONG;",
    }},
};

/// The vendored sources, at the flag set the RA8FW-360 harness pass measured:
/// the gnu11 dialect and `_GNU_SOURCE` the Linux port asks for. No warning
/// flags, and deliberately so: the vendored ThreadX and NetX headers sit in
/// every consumer TU's include set here, so any warning bar would govern
/// header text this graph does not own.
pub const vendored_dirs = [_][]const u8{
    "pkg:threadx/common/src",
    "pkg:threadx/ports/linux/gnu/src",
    "pkg:netxduo/common/src",
    "pkg:netxduo/addons/dhcp",
};
pub const vendored_flags = [_][]const u8{ "-std=gnu11", "-D_GNU_SOURCE", "-fno-sanitize=undefined" };

/// The first-party TUs: the ra8_ipif port glue and the harness that drives
/// it. No warning bar, for the same reason; c23 is the repo's host dialect,
/// and its `bool` keyword is why ra8_ipif needs no stdbool.h shim here.
pub const first_party_sources = [_][]const u8{
    "port/netxduo/src/ra8_ipif.c",
    "port/netxduo/tests/host/netx_ipif_harness.c",
};
pub const first_party_flags = [_][]const u8{ "-std=c23", "-fno-sanitize=undefined" };

/// The include path, in the order the header search consumes it: the
/// generated tx_port.h copy first so it shadows the vendored header, then the
/// two stacks' public headers, then the port glue and what ra8_ipif.h needs.
pub const include_paths = [_][]const u8{
    "pkg:threadx/common/inc",
    "pkg:threadx/ports/linux/gnu/inc",
    "pkg:netxduo/common/inc",
    "pkg:netxduo/addons/dhcp",
    "pkg:netxduo/ports/linux/gnu/inc",
    "port/netxduo/inc",
    "libs/ra8_core/inc",
    "libs/ra8_wifi/inc",
};

/// Every `.c` file under one vendored directory, as a basename list for
/// `addCSourceFiles`' `.root`. Same enumeration rule the middleware table
/// uses: sorted, so the compile set stays stable across runs.
fn cBasenames(b: *std.Build, dir_path: []const u8) []const []const u8 {
    var dir = pkg_path.openDir(b, dir_path) orelse return &.{};
    defer dir.close(b.graph.io);

    var names: std.ArrayList([]const u8) = .empty;
    var it = dir.iterate();
    while (it.next(b.graph.io) catch |err| {
        std.debug.panic("ra8: cannot walk '{s}': {s}", .{ dir_path, @errorName(err) });
    }) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ".c")) continue;
        names.append(b.allocator, b.dupe(entry.name)) catch @panic("OOM");
    }
    std.mem.sort([]const u8, names.items, {}, struct {
        fn lessThan(_: void, a: []const u8, c: []const u8) bool {
            return std.mem.lessThan(u8, a, c);
        }
    }.lessThan);
    return names.items;
}

/// The generated tx_port.h copy, through the header_patch tool: one directory
/// holding the rewritten header under its vendored basename. The rewrite runs
/// on the host and fails the build when the line is missing or repeated.
fn patchedTxPortDir(b: *std.Build) std.Build.LazyPath {
    const tool = b.addExecutable(.{
        .name = "header_patch",
        .root_module = b.createModule(.{
            .root_source_file = b.path("tests/zig_build_graph/header_patch.zig"),
            .target = b.graph.host,
        }),
    });
    const run = b.addRunArtifact(tool);
    run.addFileArg(pkg_path.lazy(b, tx_port_header.header));
    const out = run.addOutputFileArg(std.fs.path.basename(tx_port_header.header));
    for (tx_port_header.rewrites) |rewrite| run.addArgs(&.{ rewrite.old, rewrite.new });
    return out.dirname();
}

pub fn add(
    b: *std.Build,
    step: *std.Build.Step,
    target: std.Build.ResolvedTarget,
    optimize: std.lang.OptimizeMode,
) void {
    const module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    module.addIncludePath(patchedTxPortDir(b));
    for (include_paths) |include_path| {
        module.addIncludePath(pkg_path.lazy(b, include_path));
    }
    for (vendored_dirs) |dir| {
        module.addCSourceFiles(.{
            .root = pkg_path.lazy(b, dir),
            .files = cBasenames(b, dir),
            .flags = &vendored_flags,
        });
    }
    module.addCSourceFiles(.{
        .files = &first_party_sources,
        .flags = &first_party_flags,
    });
    // The ThreadX Linux port runs the kernel on pthreads; NetX needs libm.
    module.linkSystemLibrary("pthread", .{});
    module.linkSystemLibrary("m", .{});

    const suite = b.addExecutable(.{
        .name = "c_suite_netx_ipif_host",
        .root_module = module,
    });
    const run_suite = b.addRunArtifact(suite);
    run_suite.expectExitCode(0);
    step.dependOn(&run_suite.step);
}
