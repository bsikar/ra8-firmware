//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the RSIP protected-mode session API (RA8FW-789), which
//! replaces ra8_rsip_protected.c. Same 6 symbols, guard order, error
//! codes, log strings and scrub points. Rules are in
//! internal/rsip_protected.zig; the engine stays in ra8_rsip.c.

const std = @import("std");
const common = @import("abi_common.zig");
const p = @import("internal/rsip_protected.zig");

const tag = "RSIP_P";
const ok = p.ok;

/// Mirror of ra8_rsip_key_handle_t.
const Handle = extern struct {
    alg: u32 = 0,
    body_words: u32 = 0,
    body: [260]u32 = [_]u32{0} ** 260,
};

comptime {
    std.debug.assert(@sizeOf(Handle) == 1048);
}

extern fn ra8_rsip_key_validate(buf: [*]const u8, expected: u32) u16;
extern fn ra8_rsip_aes128_install_plain(key: [*]const u8, out: *Handle) u16;
extern fn ra8_rsip_aes192_install_plain(key: [*]const u8, out: *Handle) u16;
extern fn ra8_rsip_aes256_install_plain(key: [*]const u8, out: *Handle) u16;
extern fn ra8_rsip_aes_cipher(key: *const Handle, mode: u8, dir: u8, iv: ?[*]const u8, in: [*]const u8, out: [*]u8, len: u32) u16;
extern fn ra8_rsip_oem_install(cmd: u32, iv: [*]const u8, blob: [*]const u8, len: u32, out: *Handle) u16;
extern fn ra8_rsip_rsa_sign(key: *const Handle, size: u16, digest: [*]const u8, len: u32, sig: [*]u8) u16;
extern fn ra8_rsip_ecdsa_sign(key: *const Handle, curve: u8, digest: [*]const u8, len: u32, sig: [*]u8) u16;

var aes_handle: Handle = .{};
var aes_iv: [p.iv_bytes]u8 = [_]u8{0} ** p.iv_bytes;
var aes_iv_set = false;
var aes_mode: u8 = 0;
var aes_active = false;

/// RA8_CHECK_NULL_PTR: log the message and return null_ptr.
fn nullCheck(ptr: anytype, message: [*:0]const u8) ?u16 {
    if (ptr != null) return null;
    common.ra8_log_emit_error(tag, message);
    return p.err_null_ptr;
}

fn aesInstall(raw: [*]const u8, bits: u16, out: *Handle) u16 {
    return switch (bits) {
        128 => ra8_rsip_aes128_install_plain(raw, out),
        192 => ra8_rsip_aes192_install_plain(raw, out),
        256 => ra8_rsip_aes256_install_plain(raw, out),
        else => p.err_invalid_arg,
    };
}

fn sessionIv(iv: ?[*]const u8) void {
    aes_iv_set = iv != null;
    if (iv) |src| {
        @memcpy(&aes_iv, src[0..p.iv_bytes]);
    } else {
        p.scrub(&aes_iv);
    }
}

export fn ra8_rsip_protected_aes_init(wrapped_key: ?[*]const u8, key_bits: u16, mode: u8, iv: ?[*]const u8) u16 {
    if (nullCheck(wrapped_key, "p_aes_init: wrapped_key")) |e| return e;
    const wk = wrapped_key.?;
    const rc = ra8_rsip_key_validate(wk, p.type_aes);
    if (rc != ok) return rc;
    var raw = [_]u8{0} ** p.aes_max_bytes;
    const n = p.keyBytes(key_bits) orelse return p.err_invalid_arg;
    @memcpy(raw[0..n], wk[p.off_payload..][0..n]);
    var handle: Handle = .{};
    const irc = aesInstall(&raw, key_bits, &handle);
    p.scrub(raw[0..n]);
    if (irc != ok) return irc;
    aes_handle = handle;
    aes_mode = mode;
    sessionIv(iv);
    aes_active = true;
    return ok;
}

fn cipher(dir: u8, in: [*]const u8, out: [*]u8, len: u32) u16 {
    const iv: ?[*]const u8 = if (aes_iv_set) &aes_iv else null;
    return ra8_rsip_aes_cipher(&aes_handle, aes_mode, dir, iv, in, out, len);
}

export fn ra8_rsip_protected_aes_encrypt(plaintext: ?[*]const u8, ciphertext: ?[*]u8, len: u32) u16 {
    if (!aes_active) return p.err_invalid_state;
    if (nullCheck(plaintext, "p_aes_encrypt: plaintext")) |e| return e;
    if (nullCheck(ciphertext, "p_aes_encrypt: ciphertext")) |e| return e;
    return cipher(p.dir_encrypt, plaintext.?, ciphertext.?, len);
}

export fn ra8_rsip_protected_aes_decrypt(ciphertext: ?[*]const u8, plaintext: ?[*]u8, len: u32) u16 {
    if (!aes_active) return p.err_invalid_state;
    if (nullCheck(ciphertext, "p_aes_decrypt: ciphertext")) |e| return e;
    if (nullCheck(plaintext, "p_aes_decrypt: plaintext")) |e| return e;
    return cipher(p.dir_decrypt, ciphertext.?, plaintext.?, len);
}

export fn ra8_rsip_protected_aes_finish() u16 {
    if (!aes_active) return p.err_invalid_state;
    p.scrub(std.mem.asBytes(&aes_handle));
    p.scrub(&aes_iv);
    aes_iv_set = false;
    aes_active = false;
    return ok;
}

fn rsaValidate(wrapped: [*]const u8) u16 {
    if (ra8_rsip_key_validate(wrapped, p.type_rsa_pub) == ok) return ok;
    return ra8_rsip_key_validate(wrapped, p.type_rsa_priv);
}

fn rsaInstall(wrapped: [*]const u8, size: u16, mod_bytes: u32, out: *Handle) u16 {
    var modulus = [_]u8{0} ** p.wrapped_max_payload;
    @memcpy(modulus[0..mod_bytes], wrapped[p.off_payload..][0..mod_bytes]);
    const iv = [_]u8{0} ** p.iv_bytes;
    const rc = ra8_rsip_oem_install(p.installCmd(size), &iv, &modulus, mod_bytes, out);
    p.scrub(modulus[0..mod_bytes]);
    return rc;
}

export fn ra8_rsip_protected_rsa_decrypt(wrapped_priv: ?[*]const u8, size: u16, ciphertext: ?[*]const u8, ciphertext_len: u32, plaintext_out: ?[*]u8, plaintext_cap: u32) u16 {
    if (nullCheck(wrapped_priv, "p_rsa_decrypt: wrapped_priv")) |e| return e;
    if (nullCheck(ciphertext, "p_rsa_decrypt: ciphertext")) |e| return e;
    if (nullCheck(plaintext_out, "p_rsa_decrypt: plaintext_out")) |e| return e;
    const rc = rsaValidate(wrapped_priv.?);
    if (rc != ok) return rc;
    const mod_bytes = p.modBytes(size) orelse return p.err_invalid_arg;
    if (plaintext_cap < mod_bytes or ciphertext_len > mod_bytes) return p.err_invalid_arg;
    var handle: Handle = .{};
    const irc = rsaInstall(wrapped_priv.?, size, mod_bytes, &handle);
    if (irc != ok) return irc;
    return ra8_rsip_rsa_sign(&handle, size, ciphertext.?, ciphertext_len, plaintext_out.?);
}

export fn ra8_rsip_protected_ecdsa_sign(wrapped_priv: ?[*]const u8, curve: u8, hash: ?[*]const u8, hash_len: u32, sig_out: ?[*]u8) u16 {
    if (nullCheck(wrapped_priv, "p_ecdsa_sign: wrapped_priv")) |e| return e;
    if (nullCheck(hash, "p_ecdsa_sign: hash")) |e| return e;
    if (nullCheck(sig_out, "p_ecdsa_sign: sig_out")) |e| return e;
    const wp = wrapped_priv.?;
    const rc = ra8_rsip_key_validate(wp, p.type_ecc_priv);
    if (rc != ok) return rc;
    const params = p.eccParams(curve) orelse return p.err_invalid_arg;
    var handle: Handle = .{ .alg = params.alg, .body_words = params.priv_bytes / 4 };
    const body = std.mem.asBytes(&handle.body)[0..params.priv_bytes];
    @memcpy(body, wp[p.off_payload..][0..params.priv_bytes]);
    const src = ra8_rsip_ecdsa_sign(&handle, curve, hash.?, hash_len, sig_out.?);
    p.scrub(body);
    return src;
}
