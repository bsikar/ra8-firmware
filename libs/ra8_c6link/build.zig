//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig half of `ra8_c6link`. CMake consumes the installed
//! static library and keeps compiling the library's remaining C, which calls
//! the ported `priv_c6link_*` symbols through the unchanged
//! `src/ra8_c6link_internal.h` declarations.
//!
//! No build options: the wire layers are pure byte work with no target
//! conditionals in them.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_c6link_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const library = b.addLibrary(.{
        .name = "ra8_c6link",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_c6link_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    // One stub instance per test binary, each pointed at the types module that
    // binary already carries, so `mdl_types.zig` never lands in two modules.
    const rpc_stub_internal = b.createModule(.{
        .root_source_file = b.path("tests/mdl_rpc_stub.zig"),
        .target = target,
        .optimize = optimize,
    });
    rpc_stub_internal.addImport("types", implementation_module);
    const rpc_stub_abi = b.createModule(.{
        .root_source_file = b.path("tests/mdl_rpc_stub.zig"),
        .target = target,
        .optimize = optimize,
    });
    rpc_stub_abi.addImport("types", abi_module);

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
    abi_test_module.addImport("mdl_rpc_stub", rpc_stub_abi);
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });

    const frame_test_module = b.createModule(.{
        .root_source_file = b.path("tests/frame_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    frame_test_module.addImport("implementation", implementation_module);
    const frame_tests = b.addTest(.{ .root_module = frame_test_module });

    const caps_test_module = b.createModule(.{
        .root_source_file = b.path("tests/caps_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    caps_test_module.addImport("implementation", implementation_module);
    const caps_tests = b.addTest(.{ .root_module = caps_test_module });

    const storage_ram_test_module = b.createModule(.{
        .root_source_file = b.path("tests/storage_ram_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    storage_ram_test_module.addImport("implementation", implementation_module);
    const storage_ram_tests = b.addTest(.{ .root_module = storage_ram_test_module });

    const mdl_request_test_module = b.createModule(.{
        .root_source_file = b.path("tests/mdl_request_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    mdl_request_test_module.addImport("implementation", implementation_module);
    const mdl_request_tests = b.addTest(.{ .root_module = mdl_request_test_module });

    const mdl_chunk_test_module = b.createModule(.{
        .root_source_file = b.path("tests/mdl_chunk_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    mdl_chunk_test_module.addImport("implementation", implementation_module);
    const mdl_chunk_tests = b.addTest(.{ .root_module = mdl_chunk_test_module });

    const mdl_transfer_test_module = b.createModule(.{
        .root_source_file = b.path("tests/mdl_transfer_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    mdl_transfer_test_module.addImport("implementation", implementation_module);
    mdl_transfer_test_module.addImport("mdl_rpc_stub", rpc_stub_internal);
    const mdl_transfer_tests = b.addTest(.{ .root_module = mdl_transfer_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_frame_tests = b.addRunArtifact(frame_tests);
    const run_caps_tests = b.addRunArtifact(caps_tests);
    const run_storage_ram_tests = b.addRunArtifact(storage_ram_tests);
    const run_mdl_request_tests = b.addRunArtifact(mdl_request_tests);
    const run_mdl_chunk_tests = b.addRunArtifact(mdl_chunk_tests);
    const run_mdl_transfer_tests = b.addRunArtifact(mdl_transfer_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_c6link tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_frame_tests.step);
    test_step.dependOn(&run_caps_tests.step);
    test_step.dependOn(&run_storage_ram_tests.step);
    test_step.dependOn(&run_mdl_request_tests.step);
    test_step.dependOn(&run_mdl_chunk_tests.step);
    test_step.dependOn(&run_mdl_transfer_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
