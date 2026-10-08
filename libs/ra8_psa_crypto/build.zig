//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_psa_crypto`. CMake consumes
//! the installed static library through the unchanged `inc/ra8_psa_crypto.h`.
//!
//! One build option, `off-target`, carries the C's `RA8_OFF_TARGET` switch and
//! picks which backend module the facade imports: the software stand-ins in
//! `src/internal/backend_fake.zig`, or the real TF-PSA-Crypto binding in
//! `src/internal/backend_psa.zig`. It defaults from the target, so a hosted
//! build gets the stand-ins and a freestanding build gets PSA without CMake
//! passing anything.
//!
//! The on-target backend reaches the vendored headers through translate-c: the
//! build writes a one-line `#include <psa/crypto.h>` and translates it into the
//! `psa_h` module. The include roots and the two config-file defines below
//! mirror what `cmake/mbedtls.cmake` already gives the C compiler.

const std = @import("std");
const Translator = @import("translate_c").Translator;

/// Package (pinned in build.zig.zon) and the include root inside it.
const VendorRoot = struct { package: []const u8, dir: []const u8 };

const vendor_include_roots = [_]VendorRoot{
    .{ .package = "tf_psa_crypto", .dir = "include" },
    .{ .package = "tf_psa_crypto", .dir = "drivers/builtin/include" },
    .{ .package = "mbedtls", .dir = "include" },
};

/// On the configure pass that first asks for a package zig fetches it and
/// runs the configure again, so a missing one is skipped here.
fn addPsaHeaders(
    b: *std.Build,
    module: *std.Build.Module,
    target: std.Build.ResolvedTarget,
    optimize: std.lang.OptimizeMode,
) void {
    const header = b.addWriteFiles().add("psa_c.h", "#include <psa/crypto.h>\n");
    const translator: Translator = .init(b.dependency("translate_c", .{}), .{
        .c_source_file = header,
        .target = target,
        .optimize = optimize,
        .link_libc = false,
    });
    for (vendor_include_roots) |root| {
        const dep = b.lazyDependency(root.package, .{}) orelse continue;
        translator.addIncludePath(dep.path(root.dir));
    }
    translator.addIncludePath(b.path("../../port/mbedtls/inc"));
    translator.defineCMacro("TF_PSA_CRYPTO_CONFIG_FILE", "\"tf_psa_crypto_config.h\"");
    translator.defineCMacro("MBEDTLS_CONFIG_FILE", "\"mbedtls_config.h\"");
    module.addImport("psa_h", translator.mod);
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const off_target = b.option(
        bool,
        "off-target",
        "Use the software crypto stand-ins instead of the vendored TF-PSA-Crypto",
    ) orelse (target.result.os.tag != .freestanding);

    const build_options = b.addOptions();
    build_options.addOption(bool, "off_target", off_target);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_psa_crypto_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    library_module.addOptions("build_config", build_options);
    if (!off_target) addPsaHeaders(b, library_module, target, optimize);

    const library = b.addLibrary(.{
        .name = "ra8_psa_crypto",
        .linkage = .static,
        .root_module = library_module,
    });
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_psa_crypto tests");

    const units = [_]struct { name: []const u8, source: []const u8, root: []const u8 }{
        .{ .name = "fake", .source = "src/internal/fake.zig", .root = "tests/fake_test.zig" },
        .{ .name = "pool", .source = "src/internal/pool.zig", .root = "tests/pool_test.zig" },
        .{ .name = "guard", .source = "src/internal/guard.zig", .root = "tests/guard_test.zig" },
        .{ .name = "psa_map", .source = "src/internal/psa_map.zig", .root = "tests/psa_map_test.zig" },
    };
    for (units) |unit| {
        const under_test = b.createModule(.{
            .root_source_file = b.path(unit.source),
            .target = target,
            .optimize = optimize,
        });
        const test_module = b.createModule(.{
            .root_source_file = b.path(unit.root),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport(unit.name, under_test);
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }

    // The facade carries the backend switch, so it needs the build options
    // rather than riding the loop above.
    const facade_module = b.createModule(.{
        .root_source_file = b.path("src/internal/facade.zig"),
        .target = target,
        .optimize = optimize,
    });
    facade_module.addOptions("build_config", build_options);
    const facade_test_module = b.createModule(.{
        .root_source_file = b.path("tests/facade_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    facade_test_module.addImport("facade", facade_module);
    const facade_tests = b.addTest(.{ .root_module = facade_test_module });
    test_step.dependOn(&b.addRunArtifact(facade_tests).step);
}
