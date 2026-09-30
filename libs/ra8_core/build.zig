//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_core`.
//!
//! Three seams of this library are Zig so far: the freestanding runtime
//! primitives (#2820), the pin-claim validator (#2825) and the SysTick
//! timebase with its time-interface binding (#2830). Everything else in `src/`
//! is still C, which `.github/zig-parallel-tree-allowlist.tsv` records per
//! file.
//!
//! WHAT THIS LIBRARY SHIPS DEPENDS ON WHO LINKS IT.
//!
//! A HOST build gets TWO archives, and the split is the point.
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
//! A FREESTANDING build gets ONE archive, `ra8_core`, holding both roots
//! (`src/image_root.zig`). An image has no libc, so it needs the bare names
//! and the ports together and nothing it links defines either twice; the host
//! hazard the split exists for is not present there.
//!
//! The one archive is also what makes the ports REACHABLE from an image.
//! `_ra8_zig_build_archive()` in cmake/ra8_app/zig_libs.cmake names a
//! cross-built archive `lib<lib>.a`, so `ra8_link_zig_library_for_cpu(LIB
//! ra8_core)` can only ever fetch `libra8_core.a`. Applying the host split to
//! an image build leaves every cross-target consumer looking at the
//! freestanding half alone, so an image could not take an ra8_core port at
//! all: the three ARM images that still compile `ra8_log.c` by path had no
//! archive to move to until these two were composed.
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

    // A freestanding target takes the one composed archive below instead, so
    // only a host build installs the split halves.
    const image_build = target.result.os.tag == .freestanding;

    if (!image_build) {
        const freestanding_library = b.addLibrary(.{
            .name = "ra8_core",
            .linkage = .static,
            .root_module = freestanding_root,
        });
        freestanding_library.bundle_compiler_rt = false;
        b.installArtifact(freestanding_library);
    }

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

    const systick_reload = b.createModule(.{
        .root_source_file = b.path("src/internal/systick/reload.zig"),
        .target = target,
        .optimize = optimize,
    });

    const systick_regs = b.createModule(.{
        .root_source_file = b.path("src/internal/systick/regs.zig"),
        .target = target,
        .optimize = optimize,
    });

    const systick_abi = b.createModule(.{
        .root_source_file = b.path("src/systick_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    systick_abi.addImport("systick_reload", systick_reload);
    systick_abi.addImport("systick_regs", systick_regs);

    const time_interface_systick_abi = b.createModule(.{
        .root_source_file = b.path("src/time_interface_systick_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const root = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root.addImport("pin_validator_abi", pin_validator_abi);
    root.addImport("systick_abi", systick_abi);
    root.addImport("time_interface_systick_abi", time_interface_systick_abi);

    if (!image_build) {
        const library = b.addLibrary(.{
            .name = "ra8_core_zig",
            .linkage = .static,
            .root_module = root,
        });
        library.bundle_compiler_rt = false;
        b.installArtifact(library);
    } else {
        const image_root = b.createModule(.{
            .root_source_file = b.path("src/image_root.zig"),
            .target = target,
            .optimize = optimize,
        });
        image_root.addImport("freestanding_abi", freestanding_abi);
        image_root.addImport("pin_validator_abi", pin_validator_abi);
        image_root.addImport("systick_abi", systick_abi);
        image_root.addImport("time_interface_systick_abi", time_interface_systick_abi);

        const image_library = b.addLibrary(.{
            .name = "ra8_core",
            .linkage = .static,
            .root_module = image_root,
        });
        image_library.bundle_compiler_rt = false;
        b.installArtifact(image_library);
    }

    const pin_validator_tests = b.createModule(.{
        .root_source_file = b.path("tests/pin_validator_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    pin_validator_tests.addImport("pin_validator_registry", pin_validator_registry);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = pin_validator_tests })).step);

    const systick_tests = b.createModule(.{
        .root_source_file = b.path("tests/systick_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    systick_tests.addImport("systick_reload", systick_reload);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = systick_tests })).step);
}
