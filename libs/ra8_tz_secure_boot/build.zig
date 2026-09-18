//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_tz_secure_boot`. CMake
//! consumes the installed static library through the unchanged
//! `inc/ra8_tz_secure_boot.h` C ABI; the `test` step covers the partition core
//! and the ABI membrane.
//!
//! Two build options carry the C translation unit's `-D` switches:
//!   * `off-target` is `RA8_OFF_TARGET`. It defaults from the target, so a
//!     hosted build captures the register writes and a freestanding build
//!     performs them, without CMake passing anything.
//!   * `enable-root-of-trust` is `RA8_ENABLE_ROOT_OF_TRUST`, which compiles
//!     the default-deny NS authentication gate in front of the BLXNS. It has
//!     no target-derived default and must be passed explicitly, because a
//!     `target_compile_definitions` on the app no longer reaches a source file
//!     CMake does not compile. `cmake/ra8_app/zig_libs.cmake` forwards it.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const off_target = b.option(
        bool,
        "off-target",
        "Capture the SAU / CPSCU / VTOR writes instead of performing them",
    ) orelse (target.result.os.tag != .freestanding);

    const enable_root_of_trust = b.option(
        bool,
        "enable-root-of-trust",
        "Authenticate the Non-Secure image before BLXNS (default-deny gate)",
    ) orelse false;

    const build_options = b.addOptions();
    build_options.addOption(bool, "off_target", off_target);
    build_options.addOption(bool, "enable_root_of_trust", enable_root_of_trust);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_tz_secure_boot_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    library_module.addOptions("build_config", build_options);

    const library = b.addLibrary(.{
        .name = "ra8_tz_secure_boot",
        .linkage = .static,
        .root_module = library_module,
    });
    b.installArtifact(library);

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_tz_secure_boot_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_module.addOptions("build_config", build_options);

    const internal_test_module = b.createModule(.{
        .root_source_file = b.path("tests/internal_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    internal_test_module.addImport("implementation", implementation_module);
    const internal_tests = b.addTest(.{ .root_module = internal_test_module });

    const abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_test_module.addImport("abi", abi_module);
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_tz_secure_boot tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
