//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C ABI `ra8_psa_crypto.h` publishes, over the Zig implementation
//! behind it. This is the only file that speaks in raw pointers: it turns
//! each `(ptr, len)` pair into a slice, keeps the C's "null out the output
//! first" ordering, and hands the rest to `internal/facade.zig`.

const facade = @import("internal/facade.zig");
const fake = @import("internal/fake.zig");
const pool = @import("internal/pool.zig");
const vocab = @import("internal/vocab.zig");

const Err = vocab.Err;
const Limits = vocab.Limits;
const Slot = pool.Slot;

comptime {
    // `ra8_psa_key_attr_t` is `{ uint8_t, uint8_t, uint32_t }` in the header.
    if (@sizeOf(vocab.KeyAttr) != 8) @compileError("ra8_psa_key_attr_t must be 8 bytes");
    if (@alignOf(vocab.KeyAttr) != 4) @compileError("ra8_psa_key_attr_t must be 4-aligned");
    if (@offsetOf(vocab.KeyAttr, "usage") != 4) @compileError("usage must sit at offset 4");
}

/// An empty slice that is still non-null, for the `(NULL, 0)` arguments the
/// C contract accepts: the guards ask about presence separately.
fn span(ptr: ?[*]const u8, len: usize) []const u8 {
    const base = ptr orelse return &[_]u8{};
    return base[0..len];
}

fn spanMut(ptr: ?[*]u8, len: usize) []u8 {
    const base = ptr orelse return &[_]u8{};
    return base[0..len];
}

export fn ra8_psa_crypto_init() u16 {
    return facade.init();
}

export fn ra8_psa_crypto_deinit() u16 {
    return facade.deinit();
}

export fn ra8_psa_key_import(
    out_handle: ?*?*Slot,
    attr: ?*const vocab.KeyAttr,
    data: ?[*]const u8,
    data_len: usize,
) u16 {
    if (out_handle) |slot_out| slot_out.* = null;
    if (out_handle == null) return Err.invalid_arg;
    if (attr == null) return Err.invalid_arg;
    if (data == null) return Err.invalid_arg;

    var slot: *Slot = undefined;
    const status = facade.keyImport(attr.?, span(data, data_len), &slot);
    if (status != Err.ok) return status;
    out_handle.?.* = slot;
    return Err.ok;
}

export fn ra8_psa_key_destroy(handle: ?*Slot) u16 {
    return facade.keyDestroy(handle);
}

export fn ra8_psa_hash_compute(
    alg: vocab.Alg,
    input: ?[*]const u8,
    input_len: usize,
    out: ?[*]u8,
    out_cap: usize,
    out_len: ?*usize,
) u16 {
    if (out_len) |written| written.* = 0;
    if (out == null or out_len == null) return Err.invalid_arg;
    return facade.hashCompute(
        alg,
        span(input, input_len),
        input != null,
        spanMut(out, out_cap),
        out_len.?,
    );
}

export fn ra8_psa_sign_hash(
    handle: ?*Slot,
    alg: vocab.Alg,
    hash: ?[*]const u8,
    hash_len: usize,
    sig: ?[*]u8,
    sig_cap: usize,
    sig_len: ?*usize,
) u16 {
    if (sig_len) |written| written.* = 0;
    if (hash == null or sig == null or sig_len == null) return Err.invalid_arg;
    return facade.signHash(handle, alg, span(hash, hash_len), spanMut(sig, sig_cap), sig_len.?);
}

export fn ra8_psa_verify_hash(
    handle: ?*Slot,
    alg: vocab.Alg,
    hash: ?[*]const u8,
    hash_len: usize,
    sig: ?[*]const u8,
    sig_len: usize,
) u16 {
    if (hash == null or sig == null) return Err.invalid_arg;
    return facade.verifyHash(handle, alg, span(hash, hash_len), span(sig, sig_len));
}

export fn ra8_psa_aead_encrypt(
    handle: ?*Slot,
    alg: vocab.Alg,
    nonce: ?[*]const u8,
    nonce_len: usize,
    aad: ?[*]const u8,
    aad_len: usize,
    plain: ?[*]const u8,
    plain_len: usize,
    out: ?[*]u8,
    out_cap: usize,
    out_len: ?*usize,
) u16 {
    if (out_len) |written| written.* = 0;
    if (nonce == null or out == null or out_len == null) return Err.invalid_arg;
    return facade.aeadEncrypt(
        handle,
        alg,
        span(nonce, nonce_len),
        span(aad, aad_len),
        aad != null,
        span(plain, plain_len),
        plain != null,
        spanMut(out, out_cap),
        out_len.?,
    );
}

export fn ra8_psa_aead_decrypt(
    handle: ?*Slot,
    alg: vocab.Alg,
    nonce: ?[*]const u8,
    nonce_len: usize,
    aad: ?[*]const u8,
    aad_len: usize,
    cipher: ?[*]const u8,
    cipher_len: usize,
    out: ?[*]u8,
    out_cap: usize,
    out_len: ?*usize,
) u16 {
    if (out_len) |written| written.* = 0;
    if (nonce == null or cipher == null or out_len == null) return Err.invalid_arg;
    return facade.aeadDecrypt(
        handle,
        alg,
        span(nonce, nonce_len),
        span(aad, aad_len),
        aad != null,
        span(cipher, cipher_len),
        spanMut(out, out_cap),
        out != null,
        out_len.?,
    );
}

export fn ra8_psa_crypto_random(out: ?[*]u8, out_len: usize) u16 {
    if (out == null) return Err.invalid_arg;
    return facade.random(spanMut(out, out_len));
}

/// Still published because `ra8_psa_crypto_internal.h` declares it for the
/// off-target build and the security suite compiles against that header.
export fn ra8_psa_fake_sha256_oneshot(
    input: ?[*]const u8,
    input_len: usize,
    out: *[Limits.sha256_len]u8,
) void {
    fake.sha256(span(input, input_len), out);
}
