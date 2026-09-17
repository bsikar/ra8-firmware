//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The argv membrane and the exit contract for `gen_jlink_w4` (#858). One
//! function, `run`, is parameterised on a directory handle and both output
//! streams, so every branch of the contract is reachable from a test without
//! touching the real process.
//!
//! Exit contract, inherited unchanged from scripts/gen/gen_jlink_w4.py:
//!
//!   0  the script was emitted in full
//!   1  a usage error, an unrecognised argument, a base address CPython's
//!      `int(text, 16)` would have rejected, an image that cannot be read, or
//!      an image with no readable vector table
//!
//! There is no exit 2: the predecessor had none. The only 2 in this tool's
//! world belongs to scripts/builders/gen_jlink_w4.sh, for an absent zig.
//!
//! Order matters and is inherited: the base address is parsed BEFORE the
//! option scan, so a bad base outranks an unknown argument, and the image is
//! read AFTER it, so an unreadable image outranks nothing. Nothing reaches
//! stdout until the image has been read and its vector table accepted, so a
//! failing run emits no partial script.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Refuse to buffer an image larger than this. The predecessor had no limit
/// beyond memory; a flat MRAM image for this part is at most a few megabytes,
/// and the ceiling keeps a mistyped path from exhausting the host.
pub const max_image_bytes: usize = 64 * 1024 * 1024;

/// Where diagnostics and the script go.
pub const Streams = struct {
    out: std.io.AnyWriter,
    err: std.io.AnyWriter,
};

/// What the process shell supplies: the directory relative paths resolve
/// against and the program name the usage line names.
pub const Context = struct {
    dir: std.fs.Dir,
    program_name: []const u8,
};

fn usage(streams: Streams, program_name: []const u8) !u8 {
    try streams.err.print(
        "Usage: {s} <binary> <base_addr_hex> [--device DEV]\n",
        .{program_name},
    );
    return 1;
}

/// Run the generator over `arguments`, which excludes the program name.
pub fn run(
    allocator: std.mem.Allocator,
    arguments: []const []const u8,
    context: Context,
    streams: Streams,
) !u8 {
    var arena_state = std.heap.ArenaAllocator.init(allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    if (arguments.len < 2) return usage(streams, context.program_name);

    const image_path = arguments[0];
    const base = implementation.pythonHexInt(arena, arguments[1]) catch {
        try streams.err.print(
            "gen_jlink_w4: invalid literal for int() with base 16: '{s}'\n",
            .{arguments[1]},
        );
        return 1;
    };

    var device: []const u8 = implementation.default_device;
    var index: usize = 2;
    while (index < arguments.len) {
        if (std.mem.eql(u8, arguments[index], "--device") and index + 1 < arguments.len) {
            device = arguments[index + 1];
            index += 2;
            continue;
        }
        // A trailing --device with no value lands here too, exactly as the
        // predecessor's else branch did.
        try streams.err.print("Unknown arg: {s}\n", .{arguments[index]});
        return 1;
    }

    const image = context.dir.readFileAlloc(arena, image_path, max_image_bytes) catch |err| {
        try streams.err.print(
            "gen_jlink_w4: cannot read '{s}': {s}\n",
            .{ image_path, @errorName(err) },
        );
        return 1;
    };
    const padded = try implementation.padToWord(arena, image);

    const entry = implementation.entryOf(padded) orelse {
        // Where struct.unpack_from raised. Both offsets are named because the
        // predecessor reached the second one only for a four-byte image.
        const offset: usize = if (padded.len < 4) 0 else 4;
        try streams.err.print(
            "gen_jlink_w4: unpack requires a buffer of at least {d} bytes for unpacking 4 bytes at offset {d} (actual buffer size is {d})\n",
            .{ offset + 4, offset, padded.len },
        );
        return 1;
    };

    try implementation.writePreamble(streams.out, device);
    var offset: usize = 0;
    while (offset < padded.len) : (offset += 4) {
        const address = try implementation.formatAddress(arena, base, offset);
        defer arena.free(address);
        try implementation.writeWordWrite(streams.out, address, implementation.wordAt(padded, offset).?);
    }
    try implementation.writeEntryInjection(streams.out, entry);
    return 0;
}
