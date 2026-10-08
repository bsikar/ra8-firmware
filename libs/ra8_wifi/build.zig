//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `ra8_wifi` facade. CMake
//! consumes the installed static library through the unchanged
//! `inc/ra8_wifi.h` C ABI; the `test` step covers the lifecycle core and the
//! ABI membrane.
//!
//! No build options: the radio is a caller-supplied vtable on both the host
//! and the target, so nothing about this library is configured at compile
//! time.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_wifi_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const library = b.addLibrary(.{
        .name = "ra8_wifi",
        .linkage = .static,
        .root_module = library_module,
    });
    // Host tests link this archive with the system toolchain, and Rust
    // consumers link it into PIE executables.
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;

    // The ESP32-C6 backend is a separate member of the same archive, so a link
    // pulls it in only when it names the backend. Folded into the facade's
    // object, it made every facade-only link resolve the ra8_c6link transport.
    const c6link_backend = b.addObject(.{
        .name = "ra8_wifi_c6link",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/ra8_wifi_c6link_abi.zig"),
            .target = target,
            .optimize = optimize,
            .pic = true,
        }),
    });
    library.root_module.addObject(c6link_backend);
    b.installArtifact(library);

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_wifi_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const c6link_module = b.createModule(.{
        .root_source_file = b.path("src/internal/c6link.zig"),
        .target = target,
        .optimize = optimize,
    });

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

    const c6link_test_module = b.createModule(.{
        .root_source_file = b.path("tests/c6link_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    c6link_test_module.addImport("c6link", c6link_module);
    const c6link_tests = b.addTest(.{ .root_module = c6link_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_c6link_tests = b.addRunArtifact(c6link_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_wifi tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_c6link_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
