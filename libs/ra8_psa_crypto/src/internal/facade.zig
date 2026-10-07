//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The facade proper: module state, the order in which checks run, and the
//! hand-off to whichever backend the build selected. Everything here works
//! in slices; the membrane above converts raw pointers, and the backend
//! below owns the actual crypto.

const build_config = @import("build_config");
const guard = @import("guard.zig");
const pool = @import("pool.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Limits = vocab.Limits;

/// The C chose its crypto with `#ifdef RA8_OFF_TARGET`; this is the same
/// switch. Only the taken arm is analysed, so a host build never reaches the
/// translate-c module of the vendored PSA headers.
const backend = if (build_config.off_target)
    @import("backend_fake.zig")
else
    @import("backend_psa.zig");

pub const Slot = pool.Slot;

var state: pool.Pool = .{};

fn view(slot: ?*const Slot) ?guard.SlotView {
    const live = slot orelse return null;
    if (!state.owns(live)) return null;
    return .{ .usage = live.attr.usage };
}

fn releaseOne(_: void, slot: *Slot) void {
    backend.destroyKey(slot);
}

pub fn init() u16 {
    if (state.initialized) return Err.exists;
    const status = backend.init();
    if (status != Err.ok) return status;
    state.reset();
    state.initialized = true;
    return Err.ok;
}

pub fn deinit() u16 {
    if (!state.initialized) return Err.not_initialized;
    state.releaseAll({}, releaseOne);
    backend.deinit();
    state.initialized = false;
    return Err.ok;
}

pub fn keyImport(attr: *const vocab.KeyAttr, data: []const u8, out: **Slot) u16 {
    const check = guard.keyImport(state.initialized, attr, data.len, true);
    if (check != Err.ok) return check;

    const slot = state.alloc() orelse return Err.no_mem;
    const imported = backend.importKey(slot, attr, data);
    if (imported != Err.ok) return imported;

    slot.in_use = true;
    slot.attr = attr.*;
    @memcpy(slot.key[0..data.len], data);
    slot.key_len = data.len;
    out.* = slot;
    return Err.ok;
}

pub fn keyDestroy(handle: ?*Slot) u16 {
    if (!state.initialized) return Err.not_initialized;
    if (!state.owns(handle)) return Err.invalid_arg;
    const slot = handle.?;
    backend.destroyKey(slot);
    slot.clear();
    return Err.ok;
}

pub fn hashCompute(alg: vocab.Alg, input: []const u8, input_present: bool, out: []u8, out_len: *usize) u16 {
    const check = guard.hashCompute(state.initialized, alg, input_present, input.len, out.len);
    if (check != Err.ok) return check;
    const status = backend.hash(input, out);
    if (status != Err.ok) return status;
    out_len.* = Limits.sha256_len;
    return Err.ok;
}

pub fn signHash(
    handle: ?*const Slot,
    alg: vocab.Alg,
    digest: []const u8,
    signature: []u8,
    out_len: *usize,
) u16 {
    const check = guard.hashOperation(state.initialized, view(handle), alg, digest.len, vocab.Usage.sign);
    if (check != Err.ok) return check;
    if (signature.len < Limits.sha256_len) return Err.invalid_size;
    return backend.signHash(handle.?, digest, signature, out_len);
}

pub fn verifyHash(
    handle: ?*const Slot,
    alg: vocab.Alg,
    digest: []const u8,
    signature: []const u8,
) u16 {
    const check = guard.hashOperation(state.initialized, view(handle), alg, digest.len, vocab.Usage.verify);
    if (check != Err.ok) return check;
    return backend.verifyHash(handle.?, digest, signature);
}

pub fn aeadEncrypt(
    handle: ?*const Slot,
    alg: vocab.Alg,
    nonce: []const u8,
    aad: []const u8,
    aad_present: bool,
    plain: []const u8,
    plain_present: bool,
    out: []u8,
    out_len: *usize,
) u16 {
    const check = guard.aeadEncrypt(
        state.initialized,
        view(handle),
        alg,
        nonce.len,
        aad_present,
        aad.len,
        plain_present,
        plain.len,
        out.len,
    );
    if (check != Err.ok) return check;
    return backend.aeadEncrypt(handle.?, nonce, aad, plain, out, out_len);
}

pub fn aeadDecrypt(
    handle: ?*const Slot,
    alg: vocab.Alg,
    nonce: []const u8,
    aad: []const u8,
    aad_present: bool,
    cipher: []const u8,
    out: []u8,
    out_present: bool,
    out_len: *usize,
) u16 {
    const plan = guard.aeadDecrypt(
        state.initialized,
        view(handle),
        alg,
        nonce.len,
        aad_present,
        aad.len,
        cipher.len,
        out_present,
        out.len,
    );
    const plain_len = switch (plan) {
        .reject => |status| return status,
        .accept => |len| len,
    };
    return backend.aeadDecrypt(handle.?, nonce, aad, cipher, out, plain_len, out_len);
}

pub fn random(out: []u8) u16 {
    const check = guard.random(state.initialized, out.len);
    if (check != Err.ok) return check;
    return backend.random(out);
}

/// Test-only: drop all state so each host case starts from a cold module.
pub fn resetForTest() void {
    state = .{};
}
