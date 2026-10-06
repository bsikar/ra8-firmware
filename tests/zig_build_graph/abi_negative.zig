// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Host tool behind the ABI negative fixtures. It runs one compile that must
//! fail and exits 0 only when it failed with the fixture's diagnostic, the
//! way expect_c_failure.cmake matches the message from execute_process.
//!
//! Usage: abi_negative <kind> <zig exe> <library> <output>
//! Run it from the build root: the fixture paths are repo-relative.

const std = @import("std");
const abi = @import("abi_contract.zig");

pub fn main(init: std.process.Init) !void {
    const arena = init.arena.allocator();
    const args = try init.minimal.args.toSlice(arena);
    if (args.len != 5) std.process.fatal("usage: abi_negative <kind> <zig exe> <library> <output>", .{});

    const kind = std.meta.stringToEnum(abi.NegativeKind, args[1]) orelse
        std.process.fatal("abi_negative: unknown fixture kind '{s}'", .{args[1]});
    const fixture = fixtureFor(kind);
    const argv = abi.negativeArguments(arena, args[2], fixture, args[3], args[4]);

    const result = std.process.run(arena, init.io, .{
        .argv = argv,
        .stderr_limit = .limited(4 * 1024 * 1024),
        .stdout_limit = .limited(4 * 1024 * 1024),
    }) catch |err| std.process.fatal("ABI negative {t}: could not run the compiler: {t}", .{ kind, err });

    const compiler_failed = switch (result.term) {
        .exited => |code| code != 0,
        else => true,
    };
    switch (abi.classifyNegative(compiler_failed, result.stderr, fixture.expected_diagnostic)) {
        .expected_failure => {},
        .unexpected_success => std.process.fatal(
            "ABI negative {t}: expected a failure, but {s} compiled cleanly",
            .{ kind, fixture.source },
        ),
        .missing_diagnostic => std.process.fatal(
            "ABI negative {t}: failed without the expected diagnostic '{s}':\n{s}",
            .{ kind, fixture.expected_diagnostic, result.stderr },
        ),
    }
}

fn fixtureFor(kind: abi.NegativeKind) abi.NegativeFixture {
    for (abi.negative_fixtures) |fixture| {
        if (fixture.kind == kind) return fixture;
    }
    unreachable;
}
