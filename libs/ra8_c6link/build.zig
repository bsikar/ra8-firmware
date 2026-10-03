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
        .root_source_file = b.path("src/ra8_c6link_lib.zig"),
        .target = target,
        .optimize = optimize,
    });
    addHeaders(library_module, b);

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

    const rpc_wait_test_module = b.createModule(.{
        .root_source_file = b.path("tests/rpc_wait_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    rpc_wait_test_module.addImport("implementation", implementation_module);
    const rpc_wait_tests = b.addTest(.{ .root_module = rpc_wait_test_module });

    const sta_cfg_test_module = b.createModule(.{
        .root_source_file = b.path("tests/sta_cfg_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    sta_cfg_test_module.addImport("implementation", implementation_module);
    const sta_cfg_tests = b.addTest(.{ .root_module = sta_cfg_test_module });

    const rx_route_test_module = b.createModule(.{
        .root_source_file = b.path("tests/rx_route_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    rx_route_test_module.addImport("implementation", implementation_module);
    const rx_route_tests = b.addTest(.{ .root_module = rx_route_test_module });

    const field_copy_test_module = b.createModule(.{
        .root_source_file = b.path("tests/field_copy_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    field_copy_test_module.addImport("implementation", implementation_module);
    const field_copy_tests = b.addTest(.{ .root_module = field_copy_test_module });

    const tx_admit_test_module = b.createModule(.{
        .root_source_file = b.path("tests/tx_admit_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    tx_admit_test_module.addImport("implementation", implementation_module);
    const tx_admit_tests = b.addTest(.{ .root_module = tx_admit_test_module });

    const wifi_init_test_module = b.createModule(.{
        .root_source_file = b.path("tests/wifi_init_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    wifi_init_test_module.addImport("implementation", implementation_module);
    const wifi_init_tests = b.addTest(.{ .root_module = wifi_init_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_frame_tests = b.addRunArtifact(frame_tests);
    const run_caps_tests = b.addRunArtifact(caps_tests);
    const run_rpc_wait_tests = b.addRunArtifact(rpc_wait_tests);
    const run_sta_cfg_tests = b.addRunArtifact(sta_cfg_tests);
    const run_rx_route_tests = b.addRunArtifact(rx_route_tests);
    const run_field_copy_tests = b.addRunArtifact(field_copy_tests);
    const bare_rpc_test_module = b.createModule(.{
        .root_source_file = b.path("tests/bare_rpc_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    bare_rpc_test_module.addImport("implementation", implementation_module);
    const bare_rpc_tests = b.addTest(.{ .root_module = bare_rpc_test_module });

    const sta_policy_test_module = b.createModule(.{
        .root_source_file = b.path("tests/sta_policy_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    sta_policy_test_module.addImport("implementation", implementation_module);
    const sta_policy_tests = b.addTest(.{ .root_module = sta_policy_test_module });

    const arena_test_module = b.createModule(.{
        .root_source_file = b.path("tests/arena_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    arena_test_module.addImport("implementation", implementation_module);
    const arena_tests = b.addTest(.{ .root_module = arena_test_module });

    const run_tx_admit_tests = b.addRunArtifact(tx_admit_tests);
    const run_wifi_init_tests = b.addRunArtifact(wifi_init_tests);
    const run_bare_rpc_tests = b.addRunArtifact(bare_rpc_tests);
    const run_sta_policy_tests = b.addRunArtifact(sta_policy_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_c6link tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_frame_tests.step);
    test_step.dependOn(&run_caps_tests.step);
    test_step.dependOn(&run_rpc_wait_tests.step);
    test_step.dependOn(&run_sta_cfg_tests.step);
    test_step.dependOn(&run_rx_route_tests.step);
    test_step.dependOn(&run_field_copy_tests.step);
    test_step.dependOn(&run_tx_admit_tests.step);
    test_step.dependOn(&run_wifi_init_tests.step);
    test_step.dependOn(&run_bare_rpc_tests.step);
    test_step.dependOn(&run_sta_policy_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&b.addRunArtifact(arena_tests).step);
    addPumpTests(b, test_step, target, optimize, implementation_module);
    addHeaderAbiTest(b, test_step, target, optimize, "src/ra8_c6link_field_abi.zig", "tests/field_abi_test.zig", "field_abi");
    addHeaderAbiTest(b, test_step, target, optimize, "src/ra8_c6link_emit_abi.zig", "tests/emit_abi_test.zig", "emit_abi");
    addHeaderAbiTest(b, test_step, target, optimize, "src/ra8_c6link_lifecycle_abi.zig", "tests/lifecycle_abi_test.zig", "lifecycle_abi");
    addHeaderAbiTest(b, test_step, target, optimize, "src/ra8_c6link_ready_abi.zig", "tests/ready_abi_test.zig", "ready_abi");
}

/// A C ABI file that reads the public header, tested on its own.
fn addHeaderAbiTest(
    b: *std.Build,
    test_step: *std.Build.Step,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    source: []const u8,
    test_source: []const u8,
    import_name: []const u8,
) void {
    const abi = b.createModule(.{ .root_source_file = b.path(source), .target = target, .optimize = optimize });
    addHeaders(abi, b);
    const tests = b.createModule(.{ .root_source_file = b.path(test_source), .target = target, .optimize = optimize });
    tests.addImport(import_name, abi);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = tests })).step);
}

/// The public headers the pump's ABI reads `ra8_c6link_t` from.
fn addHeaders(module: *std.Build.Module, b: *std.Build) void {
    module.addIncludePath(b.path("inc"));
    module.addIncludePath(b.path("../ra8_core/inc"));
}

/// The poll pump and the frame dispatcher: the loop on a scripted port, and
/// both C ABI files on a real handle.
fn addPumpTests(
    b: *std.Build,
    test_step: *std.Build.Step,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    implementation_module: *std.Build.Module,
) void {
    const pump_test_module = b.createModule(.{
        .root_source_file = b.path("tests/pump_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    pump_test_module.addImport("implementation", implementation_module);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = pump_test_module })).step);

    const pump_abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_c6link_pump_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    addHeaders(pump_abi_module, b);
    const pump_abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/pump_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    pump_abi_test_module.addImport("pump_abi", pump_abi_module);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = pump_abi_test_module })).step);

    const dispatch_abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_c6link_dispatch_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    addHeaders(dispatch_abi_module, b);
    const dispatch_abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/dispatch_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    dispatch_abi_test_module.addImport("dispatch_abi", dispatch_abi_module);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = dispatch_abi_test_module })).step);
}
