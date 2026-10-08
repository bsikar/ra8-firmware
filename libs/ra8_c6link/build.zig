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
//!
//! The C headers reach Zig through translate-c. `Headers` translates each of
//! the three views once, and every module that reads one imports that shared
//! translation, so all the ABI files see the same `ra8_c6link_t`.

const std = @import("std");
const Translator = @import("translate_c").Translator;

/// The translated header views. `rpc` is null until the lazy vendor packages
/// are fetched; Zig fetches them and re-runs the configure.
const Headers = struct {
    public: *std.Build.Module,
    capture: *std.Build.Module,
    rpc: ?*std.Build.Module,
};

const public_header =
    \\#include <stdbool.h>
    \\#include "ra8_c6link.h"
    \\
;
const capture_header =
    \\#include <stdbool.h>
    \\#include "ra8_c6link_capture.h"
    \\
;
const rpc_header =
    \\#include <stdbool.h>
    \\#include "ra8_c6link_internal.h"
    \\#include "ra8_c6link_wifi.h"
    \\
;

/// One translate-c run over a generated include list (no committed C), with
/// the public include roots every view needs.
fn translate(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.lang.OptimizeMode,
    name: []const u8,
    source: []const u8,
) Translator {
    const translator: Translator = .init(b.dependency("translate_c", .{}), .{
        .c_source_file = b.addWriteFiles().add(name, source),
        .target = target,
        .optimize = optimize,
        .link_libc = false,
        // The ABI tests build C records field by field and leave the rest
        // zeroed, as the @cImport translation allowed.
        .default_init = true,
    });
    translator.defineCMacro("static_assert", "_Static_assert");
    translator.addIncludePath(b.path("inc"));
    translator.addIncludePath(b.path("../ra8_core/inc"));
    return translator;
}

fn translateHeaders(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.lang.OptimizeMode) Headers {
    const public = translate(b, target, optimize, "c6link_c.h", public_header);
    public.defineCMacro("alignas", "_Alignas");
    const capture = translate(b, target, optimize, "c6link_capture_c.h", capture_header);
    return .{ .public = public.mod, .capture = capture.mod, .rpc = translateRpc(b, target, optimize) };
}

/// The private header and the vendored esp-hosted and protobuf-c headers it
/// pulls in. `RA8_FREESTANDING` routes the protobuf-c fork's `assert` through
/// `ra8_check.h`, so this translates without libc on host and on Arm alike.
fn translateRpc(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.lang.OptimizeMode) ?*std.Build.Module {
    const esp = b.lazyDependency("esp_hosted", .{}) orelse return null;
    const protobuf = b.lazyDependency("protobuf_c", .{}) orelse return null;
    const rpc = translate(b, target, optimize, "c6link_rpc_c.h", rpc_header);
    rpc.defineCMacro("RA8_FREESTANDING", "1");
    rpc.defineCMacro("alignas", "_Alignas");
    rpc.addIncludePath(b.path("src"));
    rpc.addIncludePath(esp.path("common"));
    rpc.addIncludePath(esp.path("common/transport"));
    rpc.addIncludePath(esp.path("common/proto"));
    rpc.addIncludePath(protobuf.path("."));
    return rpc.mod;
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const headers = translateHeaders(b, target, optimize);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_c6link_lib.zig"),
        .target = target,
        .optimize = optimize,
    });
    addHeaders(library_module, headers);
    addVendorHeaders(library_module, headers);

    const library = b.addLibrary(.{
        .name = "ra8_c6link",
        .linkage = .static,
        .root_module = library_module,
    });
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
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
    addPumpTests(b, test_step, target, optimize, implementation_module, headers);
    addHeaderAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_field_abi.zig", "tests/field_abi_test.zig", "field_abi");
    addHeaderAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_emit_abi.zig", "tests/emit_abi_test.zig", "emit_abi");
    addHeaderAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_lifecycle_abi.zig", "tests/lifecycle_abi_test.zig", "lifecycle_abi");
    addHeaderAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_ready_abi.zig", "tests/ready_abi_test.zig", "ready_abi");
    addHeaderAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_eth_abi.zig", "tests/eth_abi_test.zig", "eth_abi");
    addHeaderAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_capture_abi.zig", "tests/capture_abi_test.zig", "capture_abi");
    addCodecAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_fw_abi.zig", "tests/fw_abi_test.zig", "fw_abi");
    addCodecAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_wifi_abi.zig", "tests/wifi_abi_test.zig", "wifi_abi");
    addCodecAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_bare_abi.zig", "tests/bare_abi_test.zig", "bare_abi");
    addCodecAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_take_abi.zig", "tests/take_abi_test.zig", "take_abi");
    addCodecAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_resp_abi.zig", "tests/resp_abi_test.zig", "resp_abi");
    addCodecAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_event_abi.zig", "tests/event_abi_test.zig", "event_abi");
    addCodecAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_consume_abi.zig", "tests/consume_abi_test.zig", "consume_abi");
    addCodecAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_call_abi.zig", "tests/call_abi_test.zig", "call_abi");
    addCodecAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_ap_info_abi.zig", "tests/ap_info_abi_test.zig", "ap_info_abi");
    addCodecAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_scan_abi.zig", "tests/scan_abi_test.zig", "scan_abi");
    addCodecAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_mac_abi.zig", "tests/mac_abi_test.zig", "mac_abi");
    addCodecAbiTest(b, test_step, target, optimize, headers, "src/ra8_c6link_sta_abi.zig", "tests/sta_abi_test.zig", "sta_abi");
}

/// A C ABI file that reads the vendored RPC codec types, tested on its own.
fn addCodecAbiTest(
    b: *std.Build,
    test_step: *std.Build.Step,
    target: std.Build.ResolvedTarget,
    optimize: std.lang.OptimizeMode,
    headers: Headers,
    source: []const u8,
    test_source: []const u8,
    import_name: []const u8,
) void {
    const abi = b.createModule(.{ .root_source_file = b.path(source), .target = target, .optimize = optimize });
    addHeaders(abi, headers);
    addVendorHeaders(abi, headers);
    const tests = b.createModule(.{ .root_source_file = b.path(test_source), .target = target, .optimize = optimize });
    tests.addImport(import_name, abi);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = tests })).step);
}

/// A C ABI file that reads the public header, tested on its own.
fn addHeaderAbiTest(
    b: *std.Build,
    test_step: *std.Build.Step,
    target: std.Build.ResolvedTarget,
    optimize: std.lang.OptimizeMode,
    headers: Headers,
    source: []const u8,
    test_source: []const u8,
    import_name: []const u8,
) void {
    const abi = b.createModule(.{ .root_source_file = b.path(source), .target = target, .optimize = optimize });
    addHeaders(abi, headers);
    const tests = b.createModule(.{ .root_source_file = b.path(test_source), .target = target, .optimize = optimize });
    tests.addImport(import_name, abi);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = tests })).step);
}

/// The private view, once the lazy vendor packages are there.
fn addVendorHeaders(module: *std.Build.Module, headers: Headers) void {
    if (headers.rpc) |rpc| module.addImport("c6link_rpc_h", rpc);
}

/// The public and capture views the ABI files read `ra8_c6link_t` from.
fn addHeaders(module: *std.Build.Module, headers: Headers) void {
    module.addImport("c6link_h", headers.public);
    module.addImport("c6link_capture_h", headers.capture);
}

/// The poll pump and the frame dispatcher: the loop on a scripted port, and
/// both C ABI files on a real handle.
fn addPumpTests(
    b: *std.Build,
    test_step: *std.Build.Step,
    target: std.Build.ResolvedTarget,
    optimize: std.lang.OptimizeMode,
    implementation_module: *std.Build.Module,
    headers: Headers,
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
    addHeaders(pump_abi_module, headers);
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
    addHeaders(dispatch_abi_module, headers);
    const dispatch_abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/dispatch_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    dispatch_abi_test_module.addImport("dispatch_abi", dispatch_abi_module);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = dispatch_abi_test_module })).step);
}
