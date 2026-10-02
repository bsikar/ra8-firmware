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

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
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
    library.bundle_compiler_rt = true;
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
}
