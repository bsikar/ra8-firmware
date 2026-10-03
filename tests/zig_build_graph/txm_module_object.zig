//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A Zig source as a ThreadX module object (RA8FW-540, after RA8FW-534).
//!
//! Zig cannot address module data the way a module needs: through the GOT
//! at r9. gcc can. So a module's Zig goes through Zig's C backend and the C
//! it emits is compiled by gcc with exactly the module flags the C modules
//! get (`cpu1_txm_lib.pic_flags`), against `zig.h` from the Zig lib
//! directory. The C is a build output and is never checked in.
//!
//! The toolchain is found by path, not on PATH: the cortex-m85 half needs
//! Arm GNU 13.3, and an older `arm-none-eabi-gcc` earlier on PATH rejects
//! `-mcpu=cortex-m85`.
//!
//! `zig build txm-module-check` builds two probes for both cores, links each
//! as a module with its relocations kept, and holds the result to
//! tools/check_txm_module_relocs: no data section may keep an absolute
//! relocation, because the module's start-up rebases the GOT and nothing
//! else. The probe with a constant table of function pointers must fail
//! that check and the probe without one must pass. Until the start-up
//! rebases data too (RA8FW-539), that failure is the expected result.

const std = @import("std");
const arm_flags = @import("arm_flags.zig");
const cpu1_image = @import("cpu1_image.zig");
const cpu1_threadx = @import("cpu1_threadx.zig");
const cpu1_txm_hello = @import("cpu1_txm_hello.zig");
const cpu1_txm_lib = @import("cpu1_txm_lib.zig");
const middleware = @import("middleware.zig");
const pkg_path = @import("pkg_path.zig");

pub const step_name = "txm-module-check";
pub const step_description =
    "Build the Zig-to-C module probes for both cores and check their data relocations";

/// Where the Arm GNU Toolchain 13.3 lives unless `-Darm-gnu-toolchain` says
/// otherwise.
pub const default_toolchain_dir = "/opt/arm-gnu-toolchain-13.3/bin";
pub const toolchain_option = "arm-gnu-toolchain";

/// The tools this step drives, each by its full path.
pub const Gnu = struct {
    gcc: []const u8,
    ar: []const u8,
    objcopy: []const u8,
};

/// The toolchain in `dir`, or null when its gcc is not there.
pub fn findGnu(b: *std.Build, dir: []const u8) ?Gnu {
    const gcc = b.pathJoin(&.{ dir, "arm-none-eabi-gcc" });
    std.fs.cwd().access(gcc, .{}) catch return null;
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

/// The module link, as `cpu1_txm_hello.link_flags` but for either core, and
/// with the relocations kept for the check to read.
pub const link_flags = [_][]const u8{
    "-nostdlib",
    "-nostartfiles",
    "-Wl,-e," ++ cpu1_txm_hello.entry_symbol,
    "-Wl,-z,noexecstack",
    "-Wl,--no-warn-rwx-segments",
    "-Wl,--emit-relocs",
};

/// One probe: a module whose start thread is the Zig in `source`.
pub const Probe = struct {
    name: []const u8,
    source: []const u8,
    /// Whether the linked module is expected to pass the check.
    clean: bool,
    /// What the check's report must contain.
    expect: []const []const u8,
};

const probe_dir = "tests/zig_build_graph/txm_module_probes/";

pub const probes = [_]Probe{
    .{
        .name = "table",
        .source = probe_dir ++ "table.zig",
        .clean = false,
        .expect = &.{ ".data at 0x", "R_ARM_ABS32 -> ", "double", "square", "FAIL, 2 word(s)" },
    },
    .{
        .name = "plain",
        .source = probe_dir ++ "plain.zig",
        .clean = true,
        .expect = &.{"OK, no absolute relocation in a data section"},
    },
};

pub const cores = [_]Core{ .cortex_m85, .cortex_m33 };

/// The exit status tools/check_txm_module_relocs gives a module with findings.
const exit_findings = 1;

/// Emit C for `source` and compile it with gcc and the module flags.
/// `imports` are added to the root module before it is compiled.
pub fn object(
    b: *std.Build,
    gnu: Gnu,
    core: Core,
    name: []const u8,
    root: *std.Build.Module,
) std.Build.LazyPath {
    const emitted = b.addObject(.{ .name = name, .root_module = root });
    const gcc = b.addSystemCommand(&.{gnu.gcc});
    gcc.addArgs(core.cpuFlags());
    gcc.addArgs(&c_flags);
    gcc.addArgs(&cpu1_txm_lib.pic_flags);
    gcc.addArg(b.fmt("-I{s}", .{b.graph.zig_lib_directory.path orelse "."}));
    gcc.addArg("-c");
    gcc.addFileArg(emitted.getEmittedBin());
    gcc.addArg("-o");
    return gcc.addOutputFileArg(b.fmt("{s}.o", .{name}));
}

/// The root module of a module's Zig: C output for `core`, sized for a
/// module, with `ra8_rpc_tx` and `ra8_rpc` importable.
pub fn rootModule(b: *std.Build, core: Core, source: []const u8) *std.Build.Module {
    const target = b.resolveTargetQuery(core.zigQuery());
    const rpc = b.createModule(.{
        .root_source_file = b.path("libs/ra8_rpc/src/ra8_rpc.zig"),
        .target = target,
        .optimize = .ReleaseSmall,
    });
    const rpc_tx = b.createModule(.{
        .root_source_file = b.path("libs/ra8_rpc_tx/src/ra8_rpc_tx.zig"),
        .target = target,
        .optimize = .ReleaseSmall,
    });
    rpc_tx.addImport("ra8_rpc", rpc);
    const root = b.createModule(.{
        .root_source_file = b.path(source),
        .target = target,
        .optimize = .ReleaseSmall,
        .unwind_tables = .none,
    });
    root.addImport("ra8_rpc", rpc);
    root.addImport("ra8_rpc_tx", rpc_tx);
    return root;
}

/// The module library for `core`: `libtxm_m33.a` as it is, or the same
/// sources under the M85 toolchain.
fn moduleLibrary(b: *std.Build, core: Core, base: middleware.Toolchain) std.Build.LazyPath {
    switch (core) {
        .cortex_m33 => {
            const tc = cpu1_txm_lib.toolchain(b.allocator, base);
            return middleware.add(b, cpu1_txm_lib.txm_m33, tc);
        },
        .cortex_m85 => {
            var library = cpu1_txm_lib.txm_m33;
            library.name = "txm_m85";
            var tc = base;
            tc.c_flags = std.mem.concat(b.allocator, []const u8, &.{
                base.c_flags,
                &cpu1_txm_lib.pic_flags,
            }) catch @panic("OOM");
            return middleware.add(b, library, tc);
        },
    }
}

fn assemble(
    b: *std.Build,
    gnu: Gnu,
    core: Core,
    source: []const u8,
    name: []const u8,
) std.Build.LazyPath {
    const run = b.addSystemCommand(&.{gnu.gcc});
    run.addArgs(core.cpuFlags());
    run.addArgs(&cpu1_txm_lib.pic_flags);
    run.addArg("-c");
    run.addFileArg(pkg_path.lazy(b, source));
    run.addArg("-o");
    return run.addOutputFileArg(name);
}

/// Link `entry` as a module for `core`: the preamble, the GOT set-up, the
/// entry object and the module library, with the module linker script.
pub fn link(
    b: *std.Build,
    gnu: Gnu,
    core: Core,
    base: middleware.Toolchain,
    name: []const u8,
    entry: std.Build.LazyPath,
) std.Build.LazyPath {
    const run = b.addSystemCommand(&.{gnu.gcc});
    run.addArgs(core.cpuFlags());
    run.addArgs(&link_flags);
    run.addPrefixedFileArg("-T", pkg_path.lazy(b, cpu1_txm_hello.linker_script));
    run.addArg("-o");
    const elf = run.addOutputFileArg(b.fmt("{s}.elf", .{name}));
    run.addFileArg(assemble(b, gnu, core, cpu1_txm_hello.preamble, "txm_module_preamble.o"));
    run.addFileArg(assemble(b, gnu, core, cpu1_txm_hello.gcc_setup, "gcc_setup.o"));
    run.addFileArg(entry);
    run.addFileArg(moduleLibrary(b, core, base));
    return elf;
}

/// The host tool that reads a linked module's relocations.
fn checker(b: *std.Build) *std.Build.Step.Compile {
    return b.addExecutable(.{
        .name = "check_txm_module_relocs",
        .root_module = b.createModule(.{
            .root_source_file = b.path("tools/check_txm_module_relocs/src/main.zig"),
            .target = b.graph.host,
        }),
    });
}

/// Run the check on `elf` and require the verdict `probe` expects.
fn check(
    b: *std.Build,
    tool: *std.Build.Step.Compile,
    probe: Probe,
    elf: std.Build.LazyPath,
) *std.Build.Step {
    const run = b.addRunArtifact(tool);
    run.addFileArg(elf);
    run.expectExitCode(if (probe.clean) 0 else exit_findings);
    for (probe.expect) |text| run.addCheck(.{ .expect_stdout_match = text });
    return &run.step;
}

/// Hang every probe for every core off `step`: built, linked, installed
/// under `arm/` and checked. `base` is the M85 middleware toolchain with
/// `gnu`'s tools in it.
pub fn add(b: *std.Build, step: *std.Build.Step, gnu: Gnu, base: middleware.Toolchain) void {
    const tool = checker(b);
    for (cores) |core| {
        for (probes) |probe| {
            const name = b.fmt("txm_probe_{s}_{s}", .{ probe.name, core.suffix() });
            const entry = object(b, gnu, core, name, rootModule(b, core, probe.source));
            const elf = link(b, gnu, core, base, name, entry);
            const install = b.addInstallFileWithDir(
                elf,
                .{ .custom = "arm" },
                b.fmt("{s}.elf", .{name}),
            );
            step.dependOn(&install.step);
            step.dependOn(check(b, tool, probe, elf));
        }
    }
}

/// What the step does when there is no toolchain at `dir`: fail, and say
/// where it looked. A check that cannot run has not passed.
pub fn addMissing(b: *std.Build, step: *std.Build.Step, dir: []const u8) void {
    const fail = b.addFail(b.fmt(
        step_name ++ ": no arm-none-eabi-gcc in {s}; pass -D" ++ toolchain_option ++
            "=<dir> for an Arm GNU Toolchain 13.3 bin directory",
        .{dir},
    ));
    step.dependOn(&fail.step);
}
