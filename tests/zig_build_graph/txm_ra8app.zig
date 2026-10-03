//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The signed `.ra8app` of the CPU1 hello-world module (RA8FW-479). The build
//! runs ra8_app's `ra8app_pack` on txm_hello_m33.bin with the test-only key in
//! libs/ra8_app/tools/test_key.zig and installs arm/txm_hello_m33.ra8app; the
//! test step admits that file with the in-tree verifier and refuses a tampered
//! and a truncated copy (txm_ra8app_check.zig).

const std = @import("std");
const test_key = @import("../../libs/ra8_app/tools/test_key.zig");

const app_root = "libs/ra8_app/";
const module_pack_root = app_root ++ "src/internal/module_pack.zig";
const file_name = "txm_hello_m33.ra8app";

/// Packs `bin` into the signed file and installs it under `arm/`.
pub fn add(b: *std.Build, step: *std.Build.Step, bin: std.Build.LazyPath) std.Build.LazyPath {
    const run = b.addRunArtifact(packTool(b));
    run.addFileArg(b.addWriteFiles().add("test_seed.bin", &test_key.seed));
    run.addArgs(&.{ test_key.app_id, test_key.display_name, test_key.capabilities });
    run.addFileArg(bin);
    const ra8app = run.addOutputFileArg(file_name);
    step.dependOn(&b.addInstallFileWithDir(ra8app, .{ .custom = "arm" }, file_name).step);
    return ra8app;
}

/// Runs txm_ra8app_check.zig against `ra8app` as part of `test_step`.
pub fn addCheck(b: *std.Build, test_step: *std.Build.Step, ra8app: std.Build.LazyPath) void {
    const options = b.addOptions();
    options.addOptionPath("ra8app", ra8app);
    const root = b.createModule(.{
        .root_source_file = b.path("tests/zig_build_graph/txm_ra8app_check.zig"),
        .target = b.graph.host,
    });
    root.addImport("module_pack", hostModule(b, module_pack_root));
    root.addImport("test_key", hostModule(b, app_root ++ "tools/test_key.zig"));
    root.addOptions("built", options);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = root })).step);
}

fn packTool(b: *std.Build) *std.Build.Step.Compile {
    const tool = b.addExecutable(.{
        .name = "ra8app_pack",
        .root_module = hostModule(b, app_root ++ "tools/ra8app_pack.zig"),
    });
    tool.root_module.addImport("module_pack", hostModule(b, module_pack_root));
    return tool;
}

fn hostModule(b: *std.Build, path: []const u8) *std.Build.Module {
    return b.createModule(.{ .root_source_file = b.path(path), .target = b.graph.host });
}
