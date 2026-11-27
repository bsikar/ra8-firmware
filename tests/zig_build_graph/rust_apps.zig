//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Rust firmware client cross-build registry for the root Zig build graph
//! (RA8FW-994). Cross-builds paired `no_std` Rust static libraries with Cargo
//! (or links a prebuilt directory passed via `-Drust-lib-dir`) against the exact
//! C board, boot, and Zig library substrate used by their C peers.

const std = @import("std");
const build_type = @import("build_type.zig");
const cross_image = @import("cross_image.zig");
const cross_sources = @import("cross_sources.zig");

pub const step_description =
    "Cross-build paired Rust firmware apps for RA8D2 (Cortex-M85) into zig-out/arm";

/// One paired Rust firmware application linked over the C/Zig RA8D2 substrate.
pub const RustApp = struct {
    cross: cross_sources.CrossApp,
    manifest_path: []const u8,
    archive_name: []const u8,
    rust_target: []const u8 = "thumbv8m.main-none-eabihf",
    watch_inputs: []const []const u8,
};

pub const rust_apps = [_]RustApp{
    .{
        // RA8FW-994: Rust client of the ra8_box Zig C ABI executing the shared
        // vectors matching examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_c.
        .cross = .{
            .name = "ra8_box_pair_rs",
            .dir = "examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_rs",
            .board = "libs/ra8_board_ek_ra8d2",
            .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
            .libraries = &.{"ra8_box"},
            .zig_libraries = &.{"ra8_box"},
            .stack_bytes = 4096,
        },
        .manifest_path = "examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_rs/Cargo.toml",
        .archive_name = "libra8_box_pair_rs.a",
        .watch_inputs = &.{
            "examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_rs/Cargo.lock",
            "examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_rs/build.rs",
            "examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_rs/src/lib.rs",
            "examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_rs/src/ffi.rs",
            "examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_rs/src/vectors.rs",
            "examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_rs/src/firmware.rs",
        },
    },
};

fn resolveRustArchive(
    b: *std.Build,
    app: RustApp,
    supplied_lib_dir: ?[]const u8,
) std.Build.LazyPath {
    if (supplied_lib_dir) |dir| {
        return .{ .cwd_relative = b.pathJoin(&.{ dir, app.archive_name }) };
    }
    const cargo = b.addSystemCommand(&.{
        "cargo",
        "build",
        "--release",
        "--locked",
        "--offline",
        "--target",
        app.rust_target,
        "--manifest-path",
    });
    cargo.addFileArg(b.path(app.manifest_path));
    for (app.watch_inputs) |input| cargo.addFileInput(b.path(input));
    cargo.addArg("--target-dir");
    const target_dir = cargo.addOutputDirectoryArg2("rust-target", .{});
    return target_dir.path(b, b.fmt("{s}/release/{s}", .{ app.rust_target, app.archive_name }));
}

/// Wire the `rust-arm` cross-build step into `rust_arm_step`. Missing cross
/// tools skip cleanly just like `arm` so host-only runs stay green.
pub fn addRustCrossBuild(
    b: *std.Build,
    rust_arm_step: *std.Build.Step,
    globals: build_type.Globals,
    supplied_lib_dir: ?[]const u8,
) void {
    const tools = cross_image.findArmTools(b) orelse {
        const notice = b.addSystemCommand(&.{
            "echo",
            "rust-arm: skipped -- no arm-none-eabi-gcc/objcopy/size on PATH",
        });
        rust_arm_step.dependOn(&notice.step);
        return;
    };
    const budget_checker = cross_image.mramBudgetChecker(b);
    for (rust_apps) |rust_app| {
        const archive = resolveRustArchive(b, rust_app, supplied_lib_dir);
        cross_image.addCrossAppWithExtra(
            b,
            rust_arm_step,
            tools,
            budget_checker,
            rust_app.cross,
            globals,
            archive,
            false,
        );
    }
}
