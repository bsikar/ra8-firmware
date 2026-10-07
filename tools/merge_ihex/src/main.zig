//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Entry point of the `merge_ihex` build tool (RA8FW-335). Everything decidable
//! lives in `cli.zig`, so the process boundary here stays a thin shell around
//! it: collect argv, hand over the real cwd and streams, exit with the status
//! `run` returned.

const std = @import("std");
const cli = @import("cli.zig");

pub fn main(init: std.process.Init) !u8 {
    const io = init.io;
    const allocator = init.arena.allocator();
    const argv = try init.minimal.args.toSlice(allocator);

    var stdout_buffer: [1024]u8 = undefined;
    var stderr_buffer: [1024]u8 = undefined;
    var stdout = std.Io.File.stdout().writer(io, &stdout_buffer);
    var stderr = std.Io.File.stderr().writer(io, &stderr_buffer);

    const status = try cli.run(
        allocator,
        io,
        std.Io.Dir.cwd(),
        argv,
        &stdout.interface,
        &stderr.interface,
    );

    try stdout.interface.flush();
    try stderr.interface.flush();
    return status;
}
