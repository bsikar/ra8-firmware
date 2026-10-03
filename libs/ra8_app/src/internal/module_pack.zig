//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A ThreadX module binary into a signed `.ra8app`: the preamble supplies the
//! entry and the stack, the caller supplies the identity, and the whole binary
//! is the code segment. The data segment is empty because a module's data is
//! built in RAM at load, not carried in the file. This is the path the
//! `ra8app_pack` tool runs.

const std = @import("std");
const pack = @import("appimg_pack.zig");

/// The module preamble reader, re-exported for the tool and its tests.
pub const preamble = @import("txm_preamble.zig");
/// The packer, re-exported so a caller reaches the gate through one import.
pub const packer = pack;

/// What the caller says about the module that its binary does not.
pub const Identity = struct {
    app_id: []const u8,
    display_name: []const u8,
    capabilities: u32 = pack.gate.image.Capability.none,
};

pub const Error = preamble.Error || pack.Error;

/// Pack and sign `module`. The caller owns the returned bytes.
pub fn packModule(
    allocator: std.mem.Allocator,
    module: []const u8,
    identity: Identity,
    key_pair: std.crypto.sign.Ed25519.KeyPair,
) Error![]u8 {
    const described = try preamble.read(module);
    const manifest: pack.Manifest = .{
        .entry_offset = described.entry_offset,
        .stack_size = described.stack_size,
        .capabilities = identity.capabilities,
        .app_id = identity.app_id,
        .display_name = identity.display_name,
    };
    return pack.pack(allocator, manifest, module, &.{}, key_pair);
}
