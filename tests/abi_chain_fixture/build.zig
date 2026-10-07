//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ra8_build = @import("ra8_zig_build");
const Translator = @import("translate_c").Translator;

/// The public C23 header as a Zig module. translate-c does not know the
/// C23 keywords static_assert and alignof, so they are spelled as the C11
/// keywords they replaced.
fn translateHeader(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.lang.OptimizeMode) *std.Build.Module {
    const header = b.addWriteFiles().add("abi_chain_c.h", "#include <stdbool.h>\n#include \"ra8_abi_chain.h\"\n");
    const translator: Translator = .init(b.dependency("translate_c", .{}), .{
        .c_source_file = header,
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    translator.defineCMacro("static_assert", "_Static_assert");
    translator.defineCMacro("alignof", "_Alignof");
    translator.addIncludePath(b.path("inc"));
    translator.addIncludePath(b.path("../rust_abi_fixture/inc"));
    translator.addIncludePath(b.path("../../libs/ra8_core/inc"));
    return translator.mod;
}

pub fn build(b: *std.Build) void {
    // Default target comes from the shared host probe so a native arm64 macOS
    // build links Zig's bundled libSystem stub instead of the SDK's (RA8FW-330).
    const target = b.standardTargetOptions(.{ .default_target = ra8_build.hostDefaultTargetQuery(b) });
    const optimize = b.standardOptimizeOption(.{});
    const module = b.createModule(.{
        .root_source_file = b.path("tests/chain_adapter.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    const abi_chain_h = translateHeader(b, target, optimize);
    module.addImport("abi_chain_h", abi_chain_h);
    const library = b.addLibrary(.{ .name = "ra8_abi_chain", .linkage = .static, .root_module = module });
    // The C consumer in tests/cmake/rust_abi_contract.cmake links this archive
    // with the system linker, so compiler_rt has to travel inside it.
    library.bundle_compiler_rt = true;
    b.installArtifact(library);

    const supplied_lib_dir = b.option([]const u8, "rust-lib-dir", "Directory containing the Rust ABI archive");
    const rust_lib_dir = supplied_lib_dir orelse b.root.joinString(b.allocator, "../rust_abi_fixture/target/debug") catch @panic("OOM");
    const rust_archive = b.pathJoin(&.{ rust_lib_dir, "libra8_rust_abi_fixture.a" });
    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/chain_test.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    test_module.addImport("abi_chain_h", abi_chain_h);
    const tests = b.addTest(.{ .root_module = test_module });
    tests.root_module.addObjectFile(.{ .cwd_relative = rust_archive });
    // `cargo` builds for the machine it runs on, so read the archive before
    // the link and refuse a mismatch by name rather than by linker error
    // (RA8FW-330).
    const require_archive = ra8_build.addRequireArchiveForTargetStep(b, tests, rust_archive, "-Drust-lib-dir=");
    tests.step.dependOn(require_archive);
    switch (target.result.os.tag) {
        .linux => {
            tests.root_module.linkSystemLibrary("gcc_s", .{});
            tests.root_module.linkSystemLibrary("pthread", .{});
            tests.root_module.linkSystemLibrary("dl", .{});
            tests.root_module.linkSystemLibrary("m", .{});
        },
        // libSystem carries libc, libm, pthreads and libdl on Darwin.
        .macos => tests.root_module.linkSystemLibrary("System", .{}),
        else => @panic("host Zig build graphs support Linux and macOS hosts"),
    }
    if (supplied_lib_dir == null) {
        const cargo = b.addSystemCommand(&.{ "cargo", "build", "--locked", "--manifest-path", b.root.joinString(b.allocator, "../rust_abi_fixture/Cargo.toml") catch @panic("OOM") });
        cargo.setEnvironmentVariable("CARGO_TARGET_DIR", b.root.joinString(b.allocator, "../rust_abi_fixture/target") catch @panic("OOM"));
        require_archive.dependOn(&cargo.step);
    }
    const test_step = b.step("test", "Run Zig chain adapter tests");
    _ = ra8_build.addHostTestRun(b, test_step, tests);
}
