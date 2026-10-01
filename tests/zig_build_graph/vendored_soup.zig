//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ===========================================================================
//! The third slice of RA8FW-339: a vendored third-party C tree compiled by the root
//! build graph, with no CMake in the loop, and held to the SAME per-TU flag
//! discipline CMake applies to it.
//!
//! xz-embedded is the right first one. It is four translation units, it is
//! decode-only, and it is the vendored tree whose CMake treatment is the most
//! precisely specified: tests/cmake/core_hal.cmake gives it exactly
//! `-Wno-conversion -fno-strict-aliasing` and nothing else, with a comment
//! recording that the set was measured one flag at a time on all four TUs
//! (only -Wconversion ever fires, from the size_t -> uint32_t narrowing in the
//! first-party porting header). A blanket `-w` would have made this slice
//! meaningless, which is the whole point: the vendored TUs get the narrow
//! suppression and the first-party wrapper beside them keeps the full bar.
//!
//! The suite is `apps/shared_libs/unarch/tests/src/test_unarch_xz.c`,
//! unmodified, over the committed real .xz fixtures. It is the behavioural
//! contract for the decoder's integration: honest streams decode byte-exactly,
//! and every hostile shape (SHA-256 check, an 8 MiB declared dictionary,
//! corruption, truncation, trailing bytes, a 3690:1 zeros bomb) is rejected
//! fail-closed. A build graph that compiled the SOUP but got the porting header
//! or the mode selection wrong would fail those cases rather than pass quietly.
//!
//! Nothing in CMake is changed or deleted; CMake stays authoritative.
//!
//! Split out of build.zig so the root graph stays under the repo's file-size
//! cap; the root re-exports this module and the slice's behaviour is unchanged.

const std = @import("std");
const host_flags = @import("host_flags.zig");

pub const Slice = struct {
    name: []const u8,
    porting_header: []const u8,
    c_suite_path: []const u8,
};

pub const slice = Slice{
    .name = "xz_embedded",
    .porting_header = "apps/shared_libs/unarch/inc/xz_config.h",
    .c_suite_path = "apps/shared_libs/unarch/tests/src/test_unarch_xz.c",
};

/// The vendored decode-only TUs, exactly the set
/// `RA8_XZ_THIRD_PARTY` in tests/cmake/library_sources.cmake lists. The
/// upstream tree carries more (the BCJ filters, the single-call decoder); this
/// firmware enables neither, so compiling them would be dead weight the CMake
/// build does not carry either.
pub const c_sources = [_][]const u8{
    "apps/shared_libs/third_party/xz_embedded/xz_crc32.c",
    "apps/shared_libs/third_party/xz_embedded/xz_crc64.c",
    "apps/shared_libs/third_party/xz_embedded/xz_dec_lzma2.c",
    "apps/shared_libs/third_party/xz_embedded/xz_dec_stream.c",
};

/// The first-party sources that drive the SOUP: the bounded XZ wrapper, its
/// zero-heap pool arena, and the flat-memory read seam the wrapper decodes
/// through. These are NOT vendored, so they take the full warning bar below.
pub const first_party_sources = [_][]const u8{
    "apps/shared_libs/unarch/src/unarch_xz.c",
    "apps/shared_libs/unarch/src/unarch_xz_pool.c",
    "apps/shared_libs/unarch/src/unarch_io.c",
    // The pool stopped being its own bump arena in RA8FW-308: it draws blocks from
    // the shared decoder scratch now, so the arena under it belongs to this
    // slice rather than to something CMake links from elsewhere. Both the
    // scratch and the arena are Zig now, so they arrive as archives below
    // rather than as TUs here.
};

/// Include path for the slice. `apps/shared_libs/unarch/inc` has to be on it
/// for the VENDORED TUs too: xz_private.h includes "xz_config.h", and that
/// porting header is first-party and lives there. Getting this wrong is not a
/// compile error, it is a different decoder (upstream's kernel-allocator
/// defaults instead of the zero-heap pool), which is why the suite matters.
pub const include_paths = [_][]const u8{
    "apps/shared_libs/third_party/xz_embedded",
    "apps/shared_libs/unarch/inc",
    "apps/shared_libs/unarch/tests/inc",
    "libs/ra8_core/inc",
    // unarch_xz_pool.c includes "ra8_imgdec_scratch.h", which in turn includes
    // "ra8_arena.h" (RA8FW-308), so both headers have to be reachable here.
    "libs/ra8_imgdec/inc",
    "libs/ra8_mem/inc",
    "tests/support/inc",
    "tests/fixtures/inc",
    "tests/mocks/inc",
};

/// First-party bar for this slice: the host set plus `-Wconversion`, which is
/// the one class CMake's measurement found the vendored TUs trip. Without it
/// on the first-party TUs the narrow suppression below would be suppressing
/// nothing, and the parity claim would be empty.
pub const first_party_flags = host_flags.c_flags ++ [_][]const u8{"-Wconversion"};

/// The vendored bar, from tests/cmake/core_hal.cmake: the first-party set with
/// `-Wconversion` suppressed for the porting header's fixed-width narrowing,
/// plus `-fno-strict-aliasing` because the decoder type-puns through byte
/// buffers. -Werror stays in force for every other class, including the
/// memory-safety ones, on an attacker-facing decoder.
pub const soup_flags = first_party_flags ++ [_][]const u8{
    "-Wno-conversion",
    "-fno-strict-aliasing",
};

pub fn addSuite(
    b: *std.Build,
    step: *std.Build.Step,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) void {
    const module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    for (include_paths) |include_path| {
        module.addIncludePath(b.path(include_path));
    }
    module.addCSourceFiles(.{
        .files = &c_sources,
        .flags = &soup_flags,
    });
    module.addCSourceFiles(.{
        .files = &first_party_sources,
        .flags = &first_party_flags,
    });
    module.addCSourceFile(.{
        .file = b.path(slice.c_suite_path),
        .flags = &host_flags.c_flags,
    });

    const suite = b.addExecutable(.{
        .name = "c_suite_unarch_xz",
        .root_module = module,
    });
    // unarch_xz_pool.c calls ra8_imgdec_scratch_*, which is Zig now.
    suite.linkLibrary(b.dependency("ra8_imgdec", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_imgdec"));
    // The suite's bound checks (ra8_decomp_*) and the log backend under them
    // are both Zig now, so they arrive as this archive.
    suite.linkLibrary(b.dependency("ra8_core", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_core_zig"));
    // And the bump arena the pool carves from: ra8_mem's last C went
    // (ra8_arena.c), so the seven ra8_arena_* entry points this suite resolves
    // are ra8_mem_abi.zig's exports now.
    suite.linkLibrary(b.dependency("ra8_mem", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_mem"));

    const run_suite = b.addRunArtifact(suite);
    run_suite.expectExitCode(0);
    step.dependOn(&run_suite.step);
}
