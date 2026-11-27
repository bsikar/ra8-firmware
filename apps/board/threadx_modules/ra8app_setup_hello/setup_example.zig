//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `.ra8app` setup example (RA8FW-1224): the whole pack, sign, verify and
//! gate path in one file, so a new module author can read the steps in order
//! before wiring their own module into the build graph.
//!
//! 1. A module binary carries a preamble the packer reads: the shell entry
//!    (word 6, Thumb bit set, distance from the word itself) and the start
//!    stack (word 10); the code size (word 14) must be the whole file, and
//!    the data size (word 15) is RAM the manager allocates at load.
//! 2. The caller supplies the identity the binary cannot say about itself:
//!    `app_id`, `display_name` and the capability set. Capabilities are the
//!    permissions the host grants or withholds at load.
//! 3. `module_pack.packModule` writes the fixed header, appends the module
//!    image and signs the whole thing with Ed25519. The test key is public
//!    on purpose (see libs/ra8_app/tools/test_key.zig): it proves the path,
//!    never provenance.
//! 4. The gate (`gate.verify`) parses the header, checks the signature over
//!    the signed span and applies the host's grant: a capability the app
//!    declared but the host withheld is a refusal, never a silent downgrade.
//!
//! The build graph's version of these steps is tests/zig_build_graph/
//! txm_ra8app.zig (add, addCheck) and its verify test is txm_ra8app_check.zig,
//! which runs against the real arm/txm_hello_m33.ra8app the build installs.
//! Unit tests verifying this walkthrough live in tests/zig_build_graph/ra8app_setup_test.zig.

const std = @import("std");
const module_pack = @import("module_pack");
const test_key = @import("test_key");

const pack = module_pack.packer;
const gate = pack.gate;
const appimg = gate.image;
const preamble = module_pack.preamble;
const Ed25519 = std.crypto.sign.Ed25519;

/// The example module image: filler code over the 16-word preamble. The
/// shell entry sits 0x228 bytes in and the start stack is 0x400, the same
/// shape module_pack_test.zig signs with.
pub const image_len: usize = 0x2D4;

/// The example identity: display and storage, no network, the grant an
/// offline e-reader app asks for. `ra8app_pack` takes the same three fields
/// on its command line.
pub const identity: module_pack.Identity = .{
    .app_id = test_key.app_id,
    .display_name = test_key.display_name,
    .capabilities = appimg.Capability.display | appimg.Capability.storage,
};

pub fn module() [image_len]u8 {
    var bytes: [image_len]u8 = @splat(0x5A);
    const words = [_]u32{
        preamble.id, 6, 1,     32,    0x12345678, 0x02000007, 0x211,     0x175,
        0,           1, 0x400, 0x16D, 1,          0x400,      image_len, 0x9C,
    };
    for (words, 0..) |value, index| {
        std.mem.writeInt(u32, bytes[index * 4 ..][0..4], value, .little);
    }
    return bytes;
}

pub fn keyPair() !Ed25519.KeyPair {
    return Ed25519.KeyPair.generateDeterministic(test_key.seed);
}

/// The load-time step a Module Manager runs: verify against the host's
/// grant. `grant` is the capability mask this host is willing to hand out.
pub fn admit(bytes: []const u8, key_pair: Ed25519.KeyPair, grant: u32) gate.Error!appimg.Header {
    const public_key = key_pair.public_key.toBytes();
    return gate.verify(pack.hostBackend(&public_key, grant), bytes);
}

pub fn expectRefused(bytes: []const u8, key_pair: Ed25519.KeyPair, grant: u32) !void {
    if (admit(bytes, key_pair, grant)) |_| return error.TestUnexpectedResult else |_| {}
}
