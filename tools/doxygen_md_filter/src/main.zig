//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Entry point of the `doxygen_md_filter` build tool (#858). Everything
//! decidable lives in `cli.zig`, so the process boundary here stays a thin
//! shell around it: collect argv and the repository root the docs build
//! exports, hand over the real cwd and streams, exit with the status `run`
//! returned.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main() !u8 {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const argv = try std.process.argsAlloc(allocator);
    const environment_root = std.process.getEnvVarOwned(allocator, "RA8_REPO_ROOT") catch null;

    var stdout_buffer = std.io.bufferedWriter(std.io.getStdOut().writer());
    var stderr_buffer = std.io.bufferedWriter(std.io.getStdErr().writer());

    const status = try cli.run(
        allocator,
        std.fs.cwd(),
        argv,
        environment_root,
        stdout_buffer.writer(),
        stderr_buffer.writer(),
    );

    try stdout_buffer.flush();
    try stderr_buffer.flush();
    return status;
}
