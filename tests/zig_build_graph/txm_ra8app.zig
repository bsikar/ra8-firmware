//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The signed `.ra8app` files of the CPU1 modules (RA8FW-479, RA8FW-837). The
//! build runs ra8_app's `ra8app_pack` on a module binary with the test-only
//! key in libs/ra8_app/tools/test_key.zig and installs arm/<module>.ra8app;
//! the test step admits txm_hello_m33.ra8app with the in-tree verifier and
//! refuses a tampered and a truncated copy (txm_ra8app_check.zig).

const std = @import("std");
const test_key = @import("../../libs/ra8_app/tools/test_key.zig");

const app_root = "libs/ra8_app/";
const module_pack_root = app_root ++ "src/internal/module_pack.zig";
/// One module's manifest identity: what `ra8app_pack` writes into the
/// `.ra8app`.
pub const Identity = struct {
    app_id: []const u8,
    display_name: []const u8,
    capabilities: []const u8,
};

/// The hello-world identity, the test key's own.
pub const hello = Identity{
    .app_id = test_key.app_id,
    .display_name = test_key.display_name,
    .capabilities = test_key.capabilities,
};

/// The syscall-tour module's identity (RA8FW-1023): display and storage,
/// the capabilities the tour exercises.
pub const tour = Identity{
    .app_id = test_key.tour_app_id,
    .display_name = test_key.tour_display_name,
    .capabilities = test_key.tour_capabilities,
};

/// Packs `bin`, the binary of module `name`, into a signed `<name>.ra8app`
/// with `identity`'s manifest and installs it under `arm/`.
pub fn add(b: *std.Build, step: *std.Build.Step, name: []const u8, bin: std.Build.LazyPath, identity: Identity) std.Build.LazyPath {
    const file_name = b.fmt("{s}.ra8app", .{name});
    const run = b.addRunArtifact(packTool(b));
    run.addFileArg(b.addWriteFiles().add("test_seed.bin", &test_key.seed));
    run.addArgs(&.{ identity.app_id, identity.display_name, identity.capabilities });
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
