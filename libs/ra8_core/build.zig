//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_core`.
//!
//! One seam of this library is Zig so far: the freestanding runtime
//! primitives (#2820), the libc subset the firmware provides for itself
//! because it links no libc. Everything else in `src/` is still C, which
//! `.github/zig-parallel-tree-allowlist.tsv` records per file.
//!
//! Two settings here are worth reading twice.
//!
//! `bundle_compiler_rt` is deliberately OFF. Zig's compiler_rt carries its
//! own `memcpy` / `memset` / `memmove` / `memcmp`, and this archive exports
//! those names itself, so bundling both would put two definitions of each in
//! one archive. The firmware links compiler_rt from the other Zig archives.
//!
//! `-Dabi-prefix` renames the whole exported surface. It is empty for an
//! image, which needs the bare standard names the compiler emits calls to,
//! and `ra8_` for the host suite in `tests/core/src/test_ra8_freestanding.c`,
//! which has a real libc underneath it and cannot have these names collide.

const std = @import("std");

/// Units under `src/internal/freestanding/`. Each is its own module so the
/// tests can import the same module objects the archive does.
const units = [_][]const u8{ "mem", "str", "math" };

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const abi_prefix = b.option(
        []const u8,
        "abi-prefix",
        "Prefix for the exported C symbols (\"ra8_\" for the host suite, empty for an image)",
    ) orelse "";

    const build_options = b.addOptions();
    build_options.addOption([]const u8, "abi_prefix", abi_prefix);

    var modules = std.StringHashMap(*std.Build.Module).init(b.allocator);
    inline for (units) |unit| {
        const module = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/internal/freestanding/{s}.zig", .{unit})),
            .target = target,
            .optimize = optimize,
        });
        modules.put(unit, module) catch @panic("OOM");
    }

    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/freestanding_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_module.addOptions("build_options", build_options);
    inline for (units) |unit| {
        abi_module.addImport(b.fmt("freestanding_{s}", .{unit}), modules.get(unit).?);
    }

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root_module.addImport("freestanding_abi", abi_module);

    const library = b.addLibrary(.{
        .name = "ra8_core",
        .linkage = .static,
        .root_module = root_module,
    });
    library.bundle_compiler_rt = false;
    b.installArtifact(library);

    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/freestanding_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (units) |unit| {
        test_module.addImport(b.fmt("freestanding_{s}", .{unit}), modules.get(unit).?);
    }

    const test_step = b.step("test", "Run Zig ra8_core tests");
    const tests = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(tests).step);
}
