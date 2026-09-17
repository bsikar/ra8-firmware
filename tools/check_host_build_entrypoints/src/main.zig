//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Process shell for the host-build-entrypoint gate (#858). The repository root
//! comes from RA8_REPO_ROOT when the trusted launcher sets it, otherwise the
//! working directory, mirroring the predecessor's parents[2] resolution.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main() !u8 {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    const argv = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, argv);

    const env_root: ?[]u8 = std.process.getEnvVarOwned(allocator, "RA8_REPO_ROOT") catch null;
    defer if (env_root) |root| allocator.free(root);
    const cwd = try std.process.getCwdAlloc(allocator);
    defer allocator.free(cwd);

    var stdout = std.io.bufferedWriter(std.io.getStdOut().writer());
    var stderr = std.io.bufferedWriter(std.io.getStdErr().writer());
    const status = try cli.run(
        allocator,
        argv[1..],
        env_root orelse cwd,
        stdout.writer(),
        stderr.writer(),
    );
    try stdout.flush();
    try stderr.flush();
    return status;
}
