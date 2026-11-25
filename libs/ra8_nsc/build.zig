//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig NSC veneer package (`ra8_nsc_appimg`). The
//! archive carries the secure-gateway veneer for the Ed25519 verify
//! backend; the two-image TrustZone build links it beside libra8_app.a
//! and places `.gnu.sgstubs` with the Secure app's linker script. The
//! `test` step runs the host-testable range-check decision logic.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    _ = b.addModule("ra8_nsc_appimg_pubkey", .{
        .root_source_file = b.path("src/ra8_nsc_appimg_pubkey.zig"),
    });

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_nsc_appimg_verify.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });

    const library = b.addLibrary(.{
        .name = "ra8_nsc_appimg",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.installHeadersDirectory(b.path("inc"), "", .{});
    b.installArtifact(library);

    const pubkey_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_nsc_appimg_pubkey.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });

    const pubkey_library = b.addLibrary(.{
        .name = "ra8_nsc_appimg_pubkey",
        .linkage = .static,
        .root_module = pubkey_module,
    });
    pubkey_library.bundle_compiler_rt = pubkey_library.root_module.resolved_target.?.result.os.tag != .freestanding;
    pubkey_library.installHeadersDirectory(b.path("inc"), "", .{});
    b.installArtifact(pubkey_library);

    const verify_subject = b.createModule(.{
        .root_source_file = b.path("src/ra8_nsc_appimg_verify.zig"),
        .target = target,
        .optimize = optimize,
    });
    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/appimg_verify_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.addImport("verify", verify_subject);
    const tests = b.addTest(.{ .root_module = test_module });
    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run the package tests");
    test_step.dependOn(&run_tests.step);

    const pubkey_subject = b.createModule(.{
        .root_source_file = b.path("src/ra8_nsc_appimg_pubkey.zig"),
        .target = target,
        .optimize = optimize,
    });
    const pubkey_test_module = b.createModule(.{
        .root_source_file = b.path("tests/appimg_pubkey_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    pubkey_test_module.addImport("pubkey", pubkey_subject);
    const pubkey_tests = b.addTest(.{ .root_module = pubkey_test_module });
    const run_pubkey_tests = b.addRunArtifact(pubkey_tests);
    test_step.dependOn(&run_pubkey_tests.step);
}
