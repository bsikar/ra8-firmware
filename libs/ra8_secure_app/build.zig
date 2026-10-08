//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_secure_app`. CMake consumes
//! the installed static library through the unchanged `inc/key_vault.h`,
//! `inc/ota_commit.h`, `src/secure_trng_internal.h`,
//! `src/sec_cmac_internal.h` and `src/key_import_internal.h`.
//!
//! Two build options carry the C's preprocessor switches:
//!
//! * `off-target` is `RA8_OFF_TARGET`. It defaults from the target, so a hosted
//!   build gets the host shadows and a freestanding build does not.
//! * `insecure-stub-crypto` is `RA8_INSECURE_STUB_CRYPTO`, the explicit
//!   dev/eval opt-in from the fail-closed crypto gate. It defaults false, so a production image
//!   that passes nothing fails closed.
//!
//! The vault and the TRNG take the union of the two. `ota_commit` takes
//! `off-target` alone: an insecure dev image still must not arm a real
//! boot-bank swap.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const off_target = b.option(
        bool,
        "off-target",
        "Keep the host shadows and the software entropy stand-in",
    ) orelse (target.result.os.tag != .freestanding);

    const insecure_stub_crypto = b.option(
        bool,
        "insecure-stub-crypto",
        "Declare this an insecure dev/eval image; never for production",
    ) orelse false;

    const build_options = b.addOptions();
    build_options.addOption(bool, "off_target", off_target);
    build_options.addOption(bool, "insecure_stub_crypto", insecure_stub_crypto);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_secure_app_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    library_module.addOptions("build_config", build_options);

    const library = b.addLibrary(.{
        .name = "ra8_secure_app",
        .linkage = .static,
        .root_module = library_module,
    });
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_secure_app tests");

    // Every unit under test carries the backend switch, so each test module
    // gets the same build options the library module does.
    const units = [_]struct { name: []const u8, source: []const u8, root: []const u8 }{
        .{ .name = "vocab", .source = "src/internal/vocab.zig", .root = "tests/vocab_test.zig" },
        .{ .name = "vault", .source = "src/internal/vault.zig", .root = "tests/vault_test.zig" },
        .{ .name = "trng", .source = "src/internal/trng.zig", .root = "tests/trng_test.zig" },
        .{ .name = "ota", .source = "src/internal/ota.zig", .root = "tests/ota_test.zig" },
        .{ .name = "aes", .source = "src/internal/aes.zig", .root = "tests/aes_test.zig" },
        .{ .name = "cmac", .source = "src/internal/cmac.zig", .root = "tests/cmac_test.zig" },
        .{ .name = "key_handle", .source = "src/internal/key_handle.zig", .root = "tests/key_handle_test.zig" },
        .{ .name = "key_import", .source = "src/internal/key_import.zig", .root = "tests/key_import_test.zig" },
    };
    for (units) |unit| {
        const under_test = b.createModule(.{
            .root_source_file = b.path(unit.source),
            .target = target,
            .optimize = optimize,
        });
        under_test.addOptions("build_config", build_options);
        const test_module = b.createModule(.{
            .root_source_file = b.path(unit.root),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport(unit.name, under_test);
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }

    addAbiTests(b, target, optimize, test_step);
}

/// Drive the C membrane through a linked archive on both sides of the
/// fail-closed split, whatever this build's own options say.
///
/// The membrane screens some arguments before the vault sees them, so the order
/// of those screens is tested at the exported symbol. `vault` comes from the
/// same source under the same options, so the test knows which side it is on
/// without restating the switch.
fn addAbiTests(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.lang.OptimizeMode,
    test_step: *std.Build.Step,
) void {
    // `vault.enabled` is `off_target or insecure_stub_crypto`; with the second
    // held false, `off_target` alone picks the side.
    for ([_]bool{ true, false }) |off_target| {
        const abi_options = b.addOptions();
        abi_options.addOption(bool, "off_target", off_target);
        abi_options.addOption(bool, "insecure_stub_crypto", false);

        const abi_library_module = b.createModule(.{
            .root_source_file = b.path("src/ra8_secure_app_abi.zig"),
            .target = target,
            .optimize = optimize,
        });
        abi_library_module.addOptions("build_config", abi_options);
        const abi_library = b.addLibrary(.{
            .name = if (off_target) "ra8_secure_app_enabled" else "ra8_secure_app_fail_closed",
            .linkage = .static,
            .root_module = abi_library_module,
        });

        const vault_module = b.createModule(.{
            .root_source_file = b.path("src/internal/vault.zig"),
            .target = target,
            .optimize = optimize,
        });
        vault_module.addOptions("build_config", abi_options);
        const test_module = b.createModule(.{
            .root_source_file = b.path("tests/abi_test.zig"),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport("vault", vault_module);
        test_module.linkLibrary(abi_library);
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }
}
