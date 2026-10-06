//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A Zig source as a ThreadX module (RA8FW-540, RA8FW-539).
//!
//! Zig cannot address module data the way a module needs: through the GOT
//! at r9. gcc can. So a module's Zig goes through Zig's C backend and the C
//! it emits is compiled by gcc with exactly the module flags the C modules
//! get (`cpu1_txm_lib.pic_flags`), against `zig.h` from the Zig lib
//! directory. The C is a build output and is never checked in.
//!
//! The module is then linked twice. Once with an empty rebase table and its
//! relocations kept, so tools/check_txm_module_relocs can list every data
//! word that holds an address; and again with that table in it. The copy of
//! `gcc_setup.s` it links walks the table at start-up
//! (txm_module_rebase.zig). The table is the last thing in `.rodata`, the
//! last read-only section before the data's load image, so putting it in
//! moves the load image and nothing the table names: not a run address in
//! data, and not a function.
//!
//! The module links no C library, so it also carries the four memory
//! routines gcc expects of a freestanding program (txm_module_rt.zig).

const std = @import("std");
const cpu1_txm_hello = @import("cpu1_txm_hello.zig");
const cpu1_txm_lib = @import("cpu1_txm_lib.zig");
const middleware = @import("middleware.zig");
const pkg_path = @import("pkg_path.zig");
const rebase = @import("txm_module_rebase.zig");
const runtime = @import("txm_module_rt.zig");
const target = @import("txm_module_target.zig");
const table = @import("../../tools/check_txm_module_relocs/src/table.zig");

const tool_dir = "tools/check_txm_module_relocs/src/";

/// The module link, as `cpu1_txm_hello.link_flags` but for either core, and
/// with the relocations kept for the table and the check to read.
pub const link_flags = [_][]const u8{
    "-nostdlib",
    "-nostartfiles",
    "-Wl,-e," ++ cpu1_txm_hello.entry_symbol,
    "-Wl,-z,noexecstack",
    "-Wl,--no-warn-rwx-segments",
    "-Wl,--emit-relocs",
};

pub const Gnu = target.Gnu;
pub const Core = target.Core;

/// Everything one module build needs that is not the module itself.
pub const Context = struct {
    gnu: Gnu,
    core: Core,
    /// The M85 middleware toolchain, with `gnu`'s tools in it.
    base: middleware.Toolchain,
};

/// Emit C for `root` and compile it with gcc and the module flags.
pub fn object(
    b: *std.Build,
    ctx: Context,
    name: []const u8,
    root: *std.Build.Module,
) std.Build.LazyPath {
    return compile(b, ctx, name, root, &.{});
}

fn compile(
    b: *std.Build,
    ctx: Context,
    name: []const u8,
    root: *std.Build.Module,
    extra_flags: []const []const u8,
) std.Build.LazyPath {
    const emitted = b.addObject(.{ .name = name, .root_module = root });
    const gcc = b.addSystemCommand(&.{ctx.gnu.gcc});
    gcc.addArgs(ctx.core.cpuFlags());
    gcc.addArgs(&target.c_flags);
    gcc.addArgs(&cpu1_txm_lib.pic_flags);
    gcc.addArgs(extra_flags);
    gcc.addPrefixedDirectoryArg("-I", std.Build.LazyPath.zig_lib);
    gcc.addArg("-c");
    gcc.addFileArg(emitted.getEmittedBin());
    gcc.addArg("-o");
    return gcc.addOutputFileArg(b.fmt("{s}.o", .{name}));
}

/// One Zig source as a module root: C output for `core`, sized for a module.
fn cModule(b: *std.Build, core: Core, source: []const u8) *std.Build.Module {
    return b.createModule(.{
        .root_source_file = b.path(source),
        .target = b.resolveTargetQuery(core.zigQuery()),
        .optimize = .ReleaseSmall,
        .unwind_tables = .none,
    });
}

/// The root module of a module's Zig, with `ra8_rpc_tx` and `ra8_rpc`
/// importable.
pub fn rootModule(b: *std.Build, core: Core, source: []const u8) *std.Build.Module {
    const rpc = cModule(b, core, "libs/ra8_rpc/src/ra8_rpc.zig");
    const rpc_tx = cModule(b, core, "libs/ra8_rpc_tx/src/ra8_rpc_tx.zig");
    rpc_tx.addImport("ra8_rpc", rpc);
    const root = cModule(b, core, source);
    root.addImport("ra8_rpc", rpc);
    root.addImport("ra8_rpc_tx", rpc_tx);
    return root;
}

/// The memory routines every module links, built the way its code is.
fn memoryRoutines(b: *std.Build, ctx: Context) std.Build.LazyPath {
    const root = cModule(b, ctx.core, "tests/zig_build_graph/txm_module_rt.zig");
    return compile(b, ctx, "txm_module_rt", root, &runtime.gcc_flags);
}

/// The module library for the core: `libtxm_m33.a` as it is, or the same
/// sources under the M85 toolchain.
fn moduleLibrary(b: *std.Build, ctx: Context) std.Build.LazyPath {
    switch (ctx.core) {
        .cortex_m33 => {
            const tc = cpu1_txm_lib.toolchain(b.allocator, ctx.base);
            return middleware.add(b, cpu1_txm_lib.txm_m33, tc);
        },
        .cortex_m85 => {
            var library = cpu1_txm_lib.txm_m33;
            library.name = "txm_m85";
            var tc = ctx.base;
            tc.c_flags = std.mem.concat(b.allocator, []const u8, &.{
                ctx.base.c_flags,
                &cpu1_txm_lib.pic_flags,
            }) catch @panic("OOM");
            return middleware.add(b, library, tc);
        },
    }
}

fn assemble(
    b: *std.Build,
    ctx: Context,
    source: std.Build.LazyPath,
    name: []const u8,
) std.Build.LazyPath {
    const run = b.addSystemCommand(&.{ctx.gnu.gcc});
    run.addArgs(ctx.core.cpuFlags());
    run.addArgs(&cpu1_txm_lib.pic_flags);
    run.addArg("-c");
    run.addFileArg(source);
    run.addArg("-o");
    return run.addOutputFileArg(name);
}

/// The copy of the vendored `gcc_setup.s` with the rebase pass in it.
fn gccSetup(b: *std.Build) std.Build.LazyPath {
    const tool = b.addExecutable(.{
        .name = "header_patch",
        .root_module = b.createModule(.{
            .root_source_file = b.path("tests/zig_build_graph/header_patch.zig"),
            .target = b.graph.host,
        }),
    });
    const run = b.addRunArtifact(tool);
    run.addFileArg(pkg_path.lazy(b, rebase.source));
    const out = run.addOutputFileArg("gcc_setup.s");
    for (rebase.rewrites) |rewrite| run.addArgs(&.{ rewrite.old, rewrite.new });
    return out;
}

/// A host tool of tools/check_txm_module_relocs, built from `main`.
fn hostTool(b: *std.Build, name: []const u8, main: []const u8) *std.Build.Step.Compile {
    return b.addExecutable(.{
        .name = name,
        .root_module = b.createModule(.{
            .root_source_file = b.path(b.fmt("{s}{s}", .{ tool_dir, main })),
            .target = b.graph.host,
        }),
    });
}

/// The tool that checks a linked module's data relocations against its
/// rebase records.
pub fn checker(b: *std.Build) *std.Build.Step.Compile {
    return hostTool(b, "check_txm_module_relocs", "main.zig");
}

/// A rebase table with nothing in it, for the first of the two links.
fn emptyTable(b: *std.Build) std.Build.LazyPath {
    var text: std.Io.Writer.Allocating = .init(b.allocator);
    table.write(&text.writer, &.{}, null) catch @panic("OOM");
    return b.addWriteFiles().add("txm_rebase_empty.s", text.written());
}

/// The rebase table of `first`, a module already linked with an empty one.
/// `leave_out` drops one entry, for a probe that must fail the check.
fn tableFor(b: *std.Build, first: std.Build.LazyPath, leave_out: ?usize) std.Build.LazyPath {
    const run = b.addRunArtifact(hostTool(b, "gen_txm_rebase_table", "gen_main.zig"));
    run.addFileArg(first);
    const out = run.addOutputFileArg("txm_rebase.s");
    if (leave_out) |index| run.addArg(b.fmt("--leave-out={d}", .{index}));
    return out;
}

/// What goes into every link of one module, before the table.
const Parts = struct {
    preamble: std.Build.LazyPath,
    setup: std.Build.LazyPath,
    entry: std.Build.LazyPath,
    memory: std.Build.LazyPath,
    library: std.Build.LazyPath,
};

fn linkOnce(
    b: *std.Build,
    ctx: Context,
    name: []const u8,
    parts: Parts,
    rebase_table: std.Build.LazyPath,
) std.Build.LazyPath {
    const run = b.addSystemCommand(&.{ctx.gnu.gcc});
    run.addArgs(ctx.core.cpuFlags());
    run.addArgs(&link_flags);
    run.addPrefixedFileArg("-T", pkg_path.lazy(b, cpu1_txm_hello.linker_script));
    run.addArg("-o");
    const elf = run.addOutputFileArg(b.fmt("{s}.elf", .{name}));
    run.addFileArg(parts.preamble);
    run.addFileArg(parts.setup);
    run.addFileArg(parts.entry);
    run.addFileArg(parts.memory);
    run.addFileArg(parts.library);
    // Last, so the table is the last thing in `.rodata`.
    run.addFileArg(assemble(b, ctx, rebase_table, "txm_rebase.o"));
    return elf;
}

/// Link `entry` as a module: the preamble, the GOT and data set-up with the
/// rebase pass, the entry object, the module library and the rebase table,
/// with the module linker script.
pub fn link(
    b: *std.Build,
    ctx: Context,
    name: []const u8,
    entry: std.Build.LazyPath,
    leave_out: ?usize,
) std.Build.LazyPath {
    const preamble = pkg_path.lazy(b, cpu1_txm_hello.preamble);
    const parts: Parts = .{
        .preamble = assemble(b, ctx, preamble, "txm_module_preamble.o"),
        .setup = assemble(b, ctx, gccSetup(b), "gcc_setup.o"),
        .entry = entry,
        .memory = memoryRoutines(b, ctx),
        .library = moduleLibrary(b, ctx),
    };
    const first = linkOnce(b, ctx, b.fmt("{s}_first", .{name}), parts, emptyTable(b));
    return linkOnce(b, ctx, name, parts, tableFor(b, first, leave_out));
}
