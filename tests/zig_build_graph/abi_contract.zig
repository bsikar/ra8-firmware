//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ABI-contract slice of the root build graph (#857), and the first slice
//! of it that carries negative controls.
//!
//! Every step the graph had before this one can only fail by something that
//! should build failing to build. None of them can fail by something that
//! should NOT build building anyway, which is the failure mode an ABI boundary
//! actually has: a C consumer whose struct layout has drifted from the Zig
//! definition still compiles, and a call into an export that no longer exists
//! still compiles -- both only fall over at the assertion or the link step
//! that nobody ran. CMake already knows this. tests/cmake/zig_abi_contract.cmake
//! registers the C consumer of the Zig fixture archive AND two deliberate
//! failures through tests/cmake/expect_c_failure.cmake, so the contract is
//! proven in both directions. Parity means reproducing both directions.
//!
//! This module is the Zig half of that:
//!
//!   positive   tests/zig_abi_fixture/src/test_abi_fixture.c compiled at the
//!              consumer bar, linked against the fixture archive the fixture's
//!              own build.zig produces, and run; exit code 0 required
//!   negative   src/negative_layout.c must FAIL to compile, naming the layout
//!              assertion, and src/negative_missing_symbol.c must FAIL to link,
//!              naming ra8_abi_fixture_missing
//!
//! A negative control that stops failing is a silent hole, so each one asserts
//! the diagnostic text too: a compile that fails for an unrelated reason (a
//! missing header, a typo in a flag) does not count as the failure we wanted
//! and fails the step with the compiler's own output attached.
//!
//! It lives beside the graph's unit tests rather than in build.zig because
//! build.zig is one line under the repository's 1000-line file cap, and
//! because the parts worth testing (the argument vectors, the pass/fail
//! classification) are ordinary functions once they are in a module.

const std = @import("std");

/// The fixture package, as declared in the root build.zig.zon.
pub const dependency_name = "ra8_abi_fixture";
/// The static archive that package installs.
pub const artifact_name = "ra8_abi_fixture";

/// The fixture's public C header directory and the shared error vocabulary,
/// the same two directories zig_abi_contract.cmake puts on the consumer's
/// include path, in the same order.
pub const include_paths = [_][]const u8{
    "tests/zig_abi_fixture/inc",
    "libs/ra8_core/inc",
};

/// The C consumer CMake registers as the `ra8_abi_fixture_c_consumer` test.
pub const c_consumer_path = "tests/zig_abi_fixture/src/test_abi_fixture.c";

/// The consumer's warning bar. `-Wall -Wextra -Werror` is what
/// zig_abi_contract.cmake puts on the target; the dialect is C23 because the
/// fixture's public header uses `nullptr`, `static_assert` and an enum with a
/// fixed underlying type, and because a consumer proving a C23 ABI in a looser
/// dialect than the repository compiles in would prove the wrong thing.
pub const consumer_flags = [_][]const u8{
    "-std=c23",
    "-Wall",
    "-Wextra",
    "-Werror",
};

/// The dialect expect_c_failure.cmake compiles the negative fixtures at. It is
/// deliberately the CMake value rather than the consumer's: the negatives must
/// fail for the reason they name, not because a stricter dialect rejected them
/// first.
pub const negative_dialect_flag = "-std=gnu2x";

/// Which half of the boundary a negative fixture attacks.
pub const NegativeKind = enum {
    /// Layout drift: caught by the header's own assertions, at compile time.
    layout,
    /// A vanished export: caught only by the linker, against the real archive.
    missing_symbol,
};

/// One deliberate failure, and the diagnostic that proves it failed for the
/// intended reason.
pub const NegativeFixture = struct {
    kind: NegativeKind,
    source: []const u8,
    expected_diagnostic: []const u8,
};

pub const negative_fixtures = [_]NegativeFixture{
    .{
        .kind = .layout,
        .source = "tests/zig_abi_fixture/src/negative_layout.c",
        .expected_diagnostic = "ABI contract fixture deliberately requires an incompatible layout",
    },
    .{
        .kind = .missing_symbol,
        .source = "tests/zig_abi_fixture/src/negative_missing_symbol.c",
        .expected_diagnostic = "ra8_abi_fixture_missing",
    },
};

/// What a negative fixture's compile actually did.
pub const NegativeOutcome = enum {
    /// It failed, and the failure named what it was supposed to name.
    expected_failure,
    /// It built. The control is dead and the boundary is unguarded.
    unexpected_success,
    /// It failed for some other reason, so it proves nothing.
    missing_diagnostic,
};

/// Classify one negative fixture's compile. Split out from the step so the
/// rule is testable without running a compiler.
pub fn classifyNegative(
    compiler_failed: bool,
    stderr: []const u8,
    expected_diagnostic: []const u8,
) NegativeOutcome {
    if (!compiler_failed) return .unexpected_success;
    if (std.mem.indexOf(u8, stderr, expected_diagnostic) == null) return .missing_diagnostic;
    return .expected_failure;
}

/// The argument vector for one negative fixture, in compiler order.
///
/// The layout fixture stops at `-c`: it never reaches a link, and asking it to
/// would hide the assertion behind an unrelated undefined `main`. The
/// missing-symbol fixture has to link, and has to link against the REAL
/// archive, because an archive is exactly what could have stopped exporting
/// the symbol; a stub would assert nothing.
pub fn negativeArguments(
    allocator: std.mem.Allocator,
    zig_exe: []const u8,
    fixture: NegativeFixture,
    library_path: []const u8,
    output_path: []const u8,
) []const []const u8 {
    var arguments = std.ArrayList([]const u8).init(allocator);
    arguments.appendSlice(&.{ zig_exe, "cc", negative_dialect_flag }) catch @panic("OOM");
    for (include_paths) |include_path| {
        arguments.appendSlice(&.{ "-I", include_path }) catch @panic("OOM");
    }
    arguments.append(fixture.source) catch @panic("OOM");
    switch (fixture.kind) {
        .layout => arguments.append("-c") catch @panic("OOM"),
        .missing_symbol => arguments.append(library_path) catch @panic("OOM"),
    }
    arguments.appendSlice(&.{ "-o", output_path }) catch @panic("OOM");
    return arguments.toOwnedSlice() catch @panic("OOM");
}

/// A build step that requires its compile to fail, and to fail for the stated
/// reason. There is no `addExecutable` shape for this: the graph has to run the
/// compiler itself and inspect the result, exactly as expect_c_failure.cmake
/// runs it through `execute_process` and matches the message.
const NegativeStep = struct {
    step: std.Build.Step,
    fixture: NegativeFixture,
    library: std.Build.LazyPath,

    fn make(step: *std.Build.Step, options: std.Build.Step.MakeOptions) anyerror!void {
        _ = options;
        const self: *NegativeStep = @fieldParentPtr("step", step);
        const b = step.owner;
        const kind = @tagName(self.fixture.kind);

        const output_path = b.pathJoin(&.{ b.makeTempPath(), "negative.out" });
        const arguments = negativeArguments(
            b.allocator,
            b.graph.zig_exe,
            self.fixture,
            self.library.getPath2(b, step),
            output_path,
        );

        const result = std.process.Child.run(.{
            .allocator = b.allocator,
            .argv = arguments,
            .max_output_bytes = 4 * 1024 * 1024,
        }) catch |err| return step.fail(
            "ABI negative {s}: could not run the compiler: {s}",
            .{ kind, @errorName(err) },
        );

        const compiler_failed = switch (result.term) {
            .Exited => |code| code != 0,
            else => true,
        };
        return switch (classifyNegative(
            compiler_failed,
            result.stderr,
            self.fixture.expected_diagnostic,
        )) {
            .expected_failure => {},
            .unexpected_success => step.fail(
                "ABI negative {s}: expected a failure, but {s} compiled cleanly",
                .{ kind, self.fixture.source },
            ),
            .missing_diagnostic => step.fail(
                "ABI negative {s}: failed without the expected diagnostic '{s}':\n{s}",
                .{ kind, self.fixture.expected_diagnostic, result.stderr },
            ),
        };
    }
};

/// Wire the whole slice onto `step`, and add its row to the parity manifest.
pub fn add(
    b: *std.Build,
    step: *std.Build.Step,
    parity_step: *std.Build.Step,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) void {
    const dependency = b.dependency(dependency_name, .{
        .target = target,
        .optimize = optimize,
    });
    const archive = dependency.artifact(artifact_name);

    const module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    for (include_paths) |include_path| {
        module.addIncludePath(b.path(include_path));
    }
    module.addCSourceFile(.{
        .file = b.path(c_consumer_path),
        .flags = &consumer_flags,
    });

    const consumer = b.addExecutable(.{
        .name = "c_abi_consumer_ra8_abi_fixture",
        .root_module = module,
    });
    consumer.linkLibrary(archive);
    const run_consumer = b.addRunArtifact(consumer);
    run_consumer.expectExitCode(0);
    step.dependOn(&run_consumer.step);

    for (negative_fixtures) |fixture| {
        const negative = b.allocator.create(NegativeStep) catch @panic("OOM");
        negative.* = .{
            .step = std.Build.Step.init(.{
                .id = .custom,
                .name = b.fmt("abi negative {s}", .{@tagName(fixture.kind)}),
                .owner = b,
                .makeFn = NegativeStep.make,
            }),
            .fixture = fixture,
            .library = archive.getEmittedBin(),
        };
        negative.library.addStepDependencies(&negative.step);
        step.dependOn(&negative.step);
    }

    const print = b.addSystemCommand(&.{ "printf", "%s\t%s\t%s\n" });
    print.addArg(artifact_name);
    print.addArg(include_paths[0]);
    print.addArg(c_consumer_path);
    parity_step.dependOn(&print.step);
}

/// Append this slice's compile-database rows to `candidates` (#959).
///
/// Only the positive consumer is described. The two negative fixtures are
/// deliberately absent: a compilation database is a list of commands that are
/// expected to succeed, and clang-tidy walking into a TU whose whole purpose is
/// to fail would report the deliberate failure as a finding. The archive they
/// link against has no C command of its own either; it is Zig.
pub fn appendCompileDbEntries(
    b: *std.Build,
    comptime Entry: type,
    candidates: *std.ArrayList(Entry),
    driver: []const u8,
) void {
    candidates.append(.{
        .file = c_consumer_path,
        .driver = driver,
        .flags = &consumer_flags,
        .include_dirs = &include_paths,
        .object = b.fmt("abi/{s}.o", .{std.fs.path.basename(c_consumer_path)}),
    }) catch @panic("OOM");
}
