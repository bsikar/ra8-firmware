//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RSIP protected-mode session rules (RA8FW-789), ported from
//! ra8_rsip_protected.c: key-length, RSA size, OEM command and curve
//! tables plus the scrub helper. The C ABI is in rsip_protected_abi.zig.

pub const ok: u16 = 0;
pub const err_invalid_arg: u16 = 0x103;
pub const err_invalid_state: u16 = 0x104;
pub const err_null_ptr: u16 = 0x504;

/// Payload offset inside a wrapped blob.
pub const off_payload: usize = 20;
pub const aes_max_bytes: usize = 32;
pub const iv_bytes: usize = 16;
pub const wrapped_max_payload: usize = 600;

pub const type_aes: u32 = 0xD1D2D3D4;
pub const type_rsa_pub: u32 = 0xE1E2E3E4;
pub const type_rsa_priv: u32 = 0xE5E6E7E8;
pub const type_ecc_priv: u32 = 0xF5F6F7F8;

pub const oem_invalid: u32 = 0;
pub const dir_encrypt: u8 = 0;
pub const dir_decrypt: u8 = 1;

/// Raw AES key length for a key-bits value; over 32 bytes is invalid_arg.
pub fn keyBytes(bits: u16) ?usize {
    const n: usize = bits / 8;
    if (n > aes_max_bytes) return null;
    return n;
}

pub fn modBytes(size: u16) ?u32 {
    return switch (size) {
        1024 => 128,
        2048 => 256,
        3072 => 384,
        4096 => 512,
        else => null,
    };
}

/// OEM install command for an RSA private key. 1024 has none and maps
/// to the invalid sentinel, as in the C.
pub fn installCmd(size: u16) u32 {
    return switch (size) {
        2048 => 13,
        3072 => 15,
        4096 => 17,
        else => oem_invalid,
    };
}

pub const EccParams = struct { alg: u32, priv_bytes: u32 };

pub fn eccParams(curve: u8) ?EccParams {
    return switch (curve) {
        2 => .{ .alg = 23, .priv_bytes = 32 },
        3 => .{ .alg = 25, .priv_bytes = 48 },
        4 => .{ .alg = 37, .priv_bytes = 66 },
        9 => .{ .alg = 35, .priv_bytes = 32 },
        else => null,
    };
}

/// Zero a buffer through a volatile pointer so it is never elided.
pub fn scrub(buf: []u8) void {
    const p: [*]volatile u8 = buf.ptr;
    for (0..buf.len) |i| p[i] = 0;
}
