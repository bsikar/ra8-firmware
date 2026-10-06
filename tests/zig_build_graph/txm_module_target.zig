//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What a ThreadX module is built with and for: the Arm GNU toolchain, by
//! path, and the two cores.
//!
//! The toolchain is found by path, not on PATH: the cortex-m85 half needs
//! Arm GNU 13.3, and an older `arm-none-eabi-gcc` earlier on PATH rejects
//! `-mcpu=cortex-m85`.

const std = @import("std");
const arm_flags = @import("arm_flags.zig");
const cpu1_image = @import("cpu1_image.zig");

/// Where the Arm GNU Toolchain 13.3 lives unless `-Darm-gnu-toolchain` says
/// otherwise.
pub const default_toolchain_dir = "/opt/arm-gnu-toolchain-13.3/bin";
pub const toolchain_option = "arm-gnu-toolchain";

/// The tools this recipe drives, each by its full path.
pub const Gnu = struct {
    gcc: []const u8,
    ar: []const u8,
    objcopy: []const u8,
};

/// The toolchain in `dir`, or null when its gcc is not there.
pub fn findGnu(b: *std.Build, dir: []const u8) ?Gnu {
    const gcc = b.pathJoin(&.{ dir, "arm-none-eabi-gcc" });
    b.dependOnFileMetadata(b.graph.cwdRelativePath(gcc));
    std.Io.Dir.cwd().access(b.graph.io, gcc, .{}) catch return null;
    return .{
        .gcc = gcc,
        .ar = b.pathJoin(&.{ dir, "arm-none-eabi-ar" }),
        .objcopy = b.pathJoin(&.{ dir, "arm-none-eabi-objcopy" }),
    };
}

/// The two cores a module can be built for.
pub const Core = enum {
    cortex_m85,
    cortex_m33,

    pub fn suffix(core: Core) []const u8 {
        return switch (core) {
            .cortex_m85 => "m85",
            .cortex_m33 => "m33",
        };
    }

    /// What gcc is told to select the core. The M33 set leaves out the
    /// options of `cpu1_image.target_flags` that are not about the CPU.
    pub fn cpuFlags(core: Core) []const []const u8 {
        return switch (core) {
            .cortex_m85 => &arm_flags.cpu_select_flags,
            .cortex_m33 => cpu1_image.target_flags[0..m33_cpu_flag_count],
        };
    }

    /// The target Zig emits C for.
    pub fn zigQuery(core: Core) std.Target.Query {
        return .{
            .cpu_arch = .thumb,
            .os_tag = .freestanding,
            .abi = .eabihf,
            .cpu_model = .{ .explicit = switch (core) {
                .cortex_m85 => &std.Target.arm.cpu.cortex_m85,
                .cortex_m33 => &std.Target.arm.cpu.cortex_m33,
            } },
            .ofmt = .c,
        };
    }
};

/// `-mcpu`, `-mthumb`, `-mfloat-abi` and `-mfpu`, the first four of
/// `cpu1_image.target_flags`.
const m33_cpu_flag_count = 4;

/// What every generated C unit is compiled with after the CPU selection and
/// before the module flags: optimised for size, no hosted library assumed.
pub const c_flags = [_][]const u8{ "-Os", "-ffreestanding", "-fno-builtin" };
