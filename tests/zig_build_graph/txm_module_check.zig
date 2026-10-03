//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `zig build txm-module-check`: three probes, built as modules for both
//! cores by the recipe in txm_module_object.zig, and each held to
//! tools/check_txm_module_relocs.
//!
//! The check passes a data word that holds an address only when the
//! module's rebase records name it. So the probe with a constant table of
//! function pointers passes, because its two words are in the records; the
//! probe with no such word passes with nothing to cover; and the table
//! probe linked with one record deliberately left out fails, naming the
//! word the records miss. That last one is what shows the check has not
//! simply gone quiet.

const std = @import("std");
const middleware = @import("middleware.zig");
const module_object = @import("txm_module_object.zig");
const module_target = @import("txm_module_target.zig");

pub const step_name = "txm-module-check";
pub const step_description =
    "Build the Zig-to-C module probes for both cores and check their data relocations";

/// One probe: a module whose start thread is the Zig in `source`.
pub const Probe = struct {
    name: []const u8,
    source: []const u8,
    /// A record to leave out of the rebase table, by its index.
    leave_out: ?usize = null,
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
        .clean = true,
        .expect = &.{"word(s) of data hold an address, each one in the rebase records"},
    },
    .{
        .name = "plain",
        .source = probe_dir ++ "plain.zig",
        .clean = true,
        .expect = &.{"OK, no absolute relocation in a data section"},
    },
    .{
        .name = "table_short",
        .source = probe_dir ++ "table.zig",
        .leave_out = 0,
        .clean = false,
        .expect = &.{ ".data at 0x", "R_ARM_ABS32 -> ", "FAIL, 1 word(s)" },
    },
};

pub const cores = [_]module_object.Core{ .cortex_m85, .cortex_m33 };

/// The exit status tools/check_txm_module_relocs gives a module it fails.
const exit_findings = 1;

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
pub fn add(
    b: *std.Build,
    step: *std.Build.Step,
    gnu: module_object.Gnu,
    base: middleware.Toolchain,
) void {
    const tool = module_object.checker(b);
    for (cores) |core| {
        const ctx: module_object.Context = .{ .gnu = gnu, .core = core, .base = base };
        for (probes) |probe| {
            const name = b.fmt("txm_probe_{s}_{s}", .{ probe.name, core.suffix() });
            const root = module_object.rootModule(b, core, probe.source);
            const entry = module_object.object(b, ctx, name, root);
            const elf = module_object.link(b, ctx, name, entry, probe.leave_out);
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
        step_name ++ ": no arm-none-eabi-gcc in {s}; pass -D" ++
            module_target.toolchain_option ++
            "=<dir> for an Arm GNU Toolchain 13.3 bin directory",
        .{dir},
    ));
    step.dependOn(&fail.step);
}
