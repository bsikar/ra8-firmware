//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8app_pack: a ThreadX module binary in, a signed `.ra8app` out.
//!
//!   ra8app_pack [--icon ICON_FILE] SEED_FILE APP_ID DISPLAY_NAME CAPABILITIES MODULE.bin OUT.ra8app
//!
//! SEED_FILE holds the raw 32-byte Ed25519 seed. CAPABILITIES is a number in
//! any base Zig parses (`0x1` for display). Optional --icon specifies a 64-byte
//! 2-bit grayscale packed 16x16 icon to include in format v2 metadata.

const std = @import("std");
const module_pack = @import("module_pack");

const Ed25519 = std.crypto.sign.Ed25519;
const usage = "usage: ra8app_pack [--icon ICON_FILE] SEED_FILE APP_ID DISPLAY_NAME CAPABILITIES MODULE.bin OUT.ra8app\n";
const file_max: usize = 16 * 1024 * 1024;

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const allocator = init.arena.allocator();
    const stderr = std.Io.File.stderr();

    const args = try init.minimal.args.toSlice(allocator);

    var icon_path: ?[]const u8 = null;
    var pos_args: [6][]const u8 = undefined;
    var pos_count: usize = 0;

    var i: usize = 1;
    while (i < args.len) : (i += 1) {
        if (std.mem.eql(u8, args[i], "--icon")) {
            i += 1;
            if (i >= args.len) {
                try stderr.writeStreamingAll(io, "ra8app_pack: --icon requires a file path\n");
                std.process.exit(2);
            }
            icon_path = args[i];
        } else {
            if (pos_count >= 6) {
                try stderr.writeStreamingAll(io, usage);
                std.process.exit(2);
            }
            pos_args[pos_count] = args[i];
            pos_count += 1;
        }
    }

    if (pos_count != 6) {
        try stderr.writeStreamingAll(io, usage);
        std.process.exit(2);
    }

    const cwd = std.Io.Dir.cwd();
    const seed = try cwd.readFileAlloc(io, pos_args[0], allocator, .limited(file_max));
    if (seed.len != Ed25519.KeyPair.seed_length) {
        try stderr.writeStreamingAll(io, "ra8app_pack: the seed file must be exactly 32 bytes\n");
        std.process.exit(1);
    }
    const key_pair = try Ed25519.KeyPair.generateDeterministic(seed[0..Ed25519.KeyPair.seed_length].*);

    var icon_bytes: ?[]const u8 = null;
    if (icon_path) |path| {
        const raw_icon = try cwd.readFileAlloc(io, path, allocator, .limited(file_max));
        if (raw_icon.len != module_pack.packer.gate.image.Format.icon_bytes) {
            try stderr.writeStreamingAll(io, "ra8app_pack: the icon file must be exactly 64 bytes\n");
            std.process.exit(1);
        }
        icon_bytes = raw_icon;
    }

    const identity: module_pack.Identity = .{
        .app_id = pos_args[1],
        .display_name = pos_args[2],
        .capabilities = try std.fmt.parseInt(u32, pos_args[3], 0),
        .icon = icon_bytes,
    };
    const module = try cwd.readFileAlloc(io, pos_args[4], allocator, .limited(file_max));
    const image = try module_pack.packModule(allocator, module, identity, key_pair);
    try cwd.writeFile(io, .{ .sub_path = pos_args[5], .data = image });
}
