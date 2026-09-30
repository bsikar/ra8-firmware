//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_core`.
//!
//! Two seams of this library are Zig so far: the freestanding runtime
//! primitives (#2820) and the pin-claim validator (#2825). Everything else in `src/`
//! is still C, which `.github/zig-parallel-tree-allowlist.tsv` records per
//! file.
//!
//! THIS LIBRARY SHIPS TWO ARCHIVES, and the split is the point.
//!
//! `ra8_core` holds the freestanding primitives alone. Its exported names are
//! the bare standard ones an image needs (`memcpy`, `memset`, `strlen`,
//! `abs`), so it cannot be linked into a host test binary, which already has
//! a real libc defining every one of them. `-Dabi-prefix=ra8_` renames that
//! whole surface for the one host suite that does test it,
//! `tests/core/src/test_ra8_freestanding.c`.
//!
//! `ra8_core_zig` holds every other ported TU. Those export ordinary `ra8_*`
//! names that collide with nothing, so `tests/cmake/zig_libraries.cmake`
//! links it into every host test the way it links the other migrated
//! libraries. New ra8_core slices belong here; only a libc-named primitive
//! belongs in the other one.
//!
//! `bundle_compiler_rt` is OFF on both. Zig's compiler_rt carries its own
//! `memcpy` / `memset` / `memmove` / `memcmp`: in the freestanding archive
//! that would double-define the names this archive exports itself, and in the
//! general archive it would drag libc names into every host test link. The
//! firmware links compiler_rt from the other Zig archives.

const std = @import("std");

/// Units under `src/internal/freestanding/`. Each is its own module so the
/// tests can import the same module objects the archive does.
const freestanding_units = [_][]const u8{ "mem", "str", "math" };

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const abi_prefix = b.option(
        []const u8,
        "abi-prefix",
        "Prefix for the freestanding archive's exported C symbols (\"ra8_\" for the host suite, empty for an image)",
    ) orelse "";

    const build_options = b.addOptions();
    build_options.addOption([]const u8, "abi_prefix", abi_prefix);

    const test_step = b.step("test", "Run Zig ra8_core tests");

    // ---- the freestanding archive -------------------------------------
    var freestanding_modules = std.StringHashMap(*std.Build.Module).init(b.allocator);
    inline for (freestanding_units) |unit| {
        const module = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/internal/freestanding/{s}.zig", .{unit})),
            .target = target,
            .optimize = optimize,
        });
        freestanding_modules.put(unit, module) catch @panic("OOM");
    }

    const freestanding_abi = b.createModule(.{
        .root_source_file = b.path("src/freestanding_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    freestanding_abi.addOptions("build_options", build_options);
    inline for (freestanding_units) |unit| {
        freestanding_abi.addImport(b.fmt("freestanding_{s}", .{unit}), freestanding_modules.get(unit).?);
    }

    const freestanding_root = b.createModule(.{
        .root_source_file = b.path("src/freestanding_root.zig"),
        .target = target,
        .optimize = optimize,
    });
    freestanding_root.addImport("freestanding_abi", freestanding_abi);

    const freestanding_library = b.addLibrary(.{
        .name = "ra8_core",
        .linkage = .static,
        .root_module = freestanding_root,
    });
    freestanding_library.bundle_compiler_rt = false;
    b.installArtifact(freestanding_library);

    const freestanding_tests = b.createModule(.{
        .root_source_file = b.path("tests/freestanding_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (freestanding_units) |unit| {
        freestanding_tests.addImport(b.fmt("freestanding_{s}", .{unit}), freestanding_modules.get(unit).?);
    }
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = freestanding_tests })).step);

    // ---- the general archive ------------------------------------------
    const pin_validator_registry = b.createModule(.{
        .root_source_file = b.path("src/internal/pin_validator/registry.zig"),
        .target = target,
        .optimize = optimize,
    });

    const pin_validator_abi = b.createModule(.{
        .root_source_file = b.path("src/pin_validator_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    pin_validator_abi.addImport("pin_validator_registry", pin_validator_registry);

    const root = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root.addImport("pin_validator_abi", pin_validator_abi);

    const library = b.addLibrary(.{
        .name = "ra8_core_zig",
        .linkage = .static,
        .root_module = root,
    });
    library.bundle_compiler_rt = false;
    b.installArtifact(library);

    const pin_validator_tests = b.createModule(.{
        .root_source_file = b.path("tests/pin_validator_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    pin_validator_tests.addImport("pin_validator_registry", pin_validator_registry);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = pin_validator_tests })).step);
}
