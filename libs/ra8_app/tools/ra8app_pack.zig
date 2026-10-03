//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8app_pack: a ThreadX module binary in, a signed `.ra8app` out.
//!
//!   ra8app_pack SEED_FILE APP_ID DISPLAY_NAME CAPABILITIES MODULE.bin OUT.ra8app
//!
//! SEED_FILE holds the raw 32-byte Ed25519 seed. CAPABILITIES is a number in
//! any base Zig parses (`0x1` for display). The work is `module_pack`.

const std = @import("std");
const module_pack = @import("module_pack");

const Ed25519 = std.crypto.sign.Ed25519;
const usage = "usage: ra8app_pack SEED_FILE APP_ID DISPLAY_NAME CAPABILITIES MODULE.bin OUT.ra8app\n";
const file_max: usize = 16 * 1024 * 1024;

pub fn main() !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const args = try std.process.argsAlloc(allocator);
    if (args.len != 7) {
        try std.io.getStdErr().writeAll(usage);
        std.process.exit(2);
    }

    const cwd = std.fs.cwd();
    const seed = try cwd.readFileAlloc(allocator, args[1], file_max);
    if (seed.len != Ed25519.KeyPair.seed_length) {
        try std.io.getStdErr().writeAll("ra8app_pack: the seed file must be exactly 32 bytes\n");
        std.process.exit(1);
    }
    const key_pair = try Ed25519.KeyPair.generateDeterministic(seed[0..Ed25519.KeyPair.seed_length].*);

    const identity: module_pack.Identity = .{
        .app_id = args[2],
        .display_name = args[3],
        .capabilities = try std.fmt.parseInt(u32, args[4], 0),
    };
    const module = try cwd.readFileAlloc(allocator, args[5], file_max);
    const image = try module_pack.packModule(allocator, module, identity, key_pair);
    try cwd.writeFile(.{ .sub_path = args[6], .data = image });
}
