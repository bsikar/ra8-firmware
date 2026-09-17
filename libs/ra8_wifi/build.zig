//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `ra8_wifi` facade and its
//! ESP32-C6 backend. CMake consumes the installed static library through the
//! unchanged `inc/ra8_wifi.h` and `inc/ra8_wifi_c6link.h` C ABI; the `test`
//! step covers the lifecycle core, the facade membrane and the backend.
//!
//! The backend is built as its own object and archived as its own member on
//! purpose. It is the only part of this library that references `ra8_c6link`,
//! so a target linking the facade against a different backend (the mock-backed
//! facade test, the example core test) must not be made to resolve the radio
//! stack's symbols: `ld` pulls that member only when something actually names
//! `k_ra8_wifi_backend_c6link` or `ra8_wifi_c6link_setup`.
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
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;

    const c6link_object = b.addObject(.{
        .name = "ra8_wifi_c6link",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/ra8_wifi_c6link_abi.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    c6link_object.root_module.pic = true;
    library.addObject(c6link_object);
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
    abi_test_module.addIncludePath(b.path("../ra8_core/inc"));
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });
    abi_tests.addCSourceFile(.{
        .file = b.path("tests/log_fixture.c"),
        .flags = &.{ "-std=c23", "-Wall", "-Wextra", "-Werror" },
    });

    const c6link_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_wifi_c6link_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const c6link_test_module = b.createModule(.{
        .root_source_file = b.path("tests/c6link_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    c6link_test_module.addImport("abi", c6link_module);
    const c6link_tests = b.addTest(.{ .root_module = c6link_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const run_c6link_tests = b.addRunArtifact(c6link_tests);
    const test_step = b.step("test", "Run Zig ra8_wifi tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&run_c6link_tests.step);
}
