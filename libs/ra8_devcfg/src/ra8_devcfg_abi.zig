//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_devcfg/inc/ra8_devcfg.h`. The record codec and
//! the validation rules live in `internal/root.zig`; this file owns the
//! injected store seam, the module cache, the argument guards and the
//! `ra8_err_t` mapping the C implementation exposed.
//!
//! No HAL header is reachable from here. The durable medium is touched only
//! through the two function pointers in `ra8_devcfg_store_t`, which is what
//! keeps the resolver and the commit path host-testable without MMIO. The
//! production extra-MRAM binding stays in its own translation unit.
//!
//! Guard order is part of the contract: the null-argument vectors in
//! `tests/misc/src/test_ra8_devcfg.c` tell `null_ptr` from `not_initialized`
//! by which check fires first, so the checks below run in exactly the order
//! the C wrote them.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Decoded per-unit payload (`ra8_devcfg_body_t`).
pub const Body = implementation.Body;
/// Decoded record: body plus header discriminators (`ra8_devcfg_record_t`).
pub const Record = implementation.Record;
/// Factory authorization token (`ra8_devcfg_factory_token_t`).
pub const FactoryToken = implementation.FactoryToken;

/// Per-record status flags (`ra8_devcfg_flags_t`).
pub const flag_provisioned = implementation.flag_provisioned;
pub const flag_vcom_valid = implementation.flag_vcom_valid;
pub const flag_touch_valid = implementation.flag_touch_valid;

/// Subset of `ra8_err_t` this library returns.
pub const CfgError = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    invalid_state = 0x104,
    not_initialized = 0x10F,
    access_denied = 0x112,
    validation_failed = 0x501,
    null_ptr = 0x504,
};

/// Raw `ra8_err_t` as it crosses the ABI, so a store seam may hand back any
/// code in the repo's error space and have it propagate unchanged.
pub const RawErr = u16;

/// Backing-store read seam (`ra8_devcfg_read_fn_t`).
pub const ReadFn = *const fn (offset: u32, dst: [*]u8, len: u32) callconv(.c) RawErr;
/// Backing-store write seam (`ra8_devcfg_write_fn_t`).
pub const WriteFn = *const fn (offset: u32, src: [*]const u8, len: u32) callconv(.c) RawErr;

/// Dependency-injection vtable for the record backing store
/// (`ra8_devcfg_store_t`).
pub const Store = extern struct {
    read: ?ReadFn = null,
    write: ?WriteFn = null,
};

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_warn(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;

const tag: [*:0]const u8 = "DEVCFG";

comptime {
    if (@sizeOf(CfgError) != 2) @compileError("ra8_err_t width");
    if (@backingInt(CfgError.ok) != 0) @compileError("k_ra8_ok value");
    if (@backingInt(CfgError.invalid_arg) != 0x103) @compileError("k_ra8_err_invalid_arg");
    if (@backingInt(CfgError.invalid_state) != 0x104) @compileError("k_ra8_err_invalid_state");
    if (@backingInt(CfgError.not_initialized) != 0x10F) @compileError("k_ra8_err_not_initialized");
    if (@backingInt(CfgError.access_denied) != 0x112) @compileError("k_ra8_err_access_denied");
    if (@backingInt(CfgError.validation_failed) != 0x501) @compileError("k_ra8_err_validation");
    if (@backingInt(CfgError.null_ptr) != 0x504) @compileError("k_ra8_err_null_ptr");

    // Pointer-width aware, so the same asserts hold for the 64-bit host build
    // and the 32-bit Arm cross build.
    const word = @sizeOf(usize);
    if (@offsetOf(Store, "write") != word) @compileError("store write offset");
    if (@sizeOf(Store) != word * 2) @compileError("store size");
}

/// Module cache state established by `ra8_devcfg_load` (`ra8_devcfg_state_t`).
const State = enum(u8) {
    unloaded = 0,
    loaded = 1,
    unprovisioned = 2,
};

var s_state: State = .unloaded;
var s_record: Record = .{};
var s_challenge_nonce: [implementation.token_nonce_len]u8 = @splat(0);
var s_challenge_active: bool = false;
var s_test_pubkey: ?[implementation.token_pubkey_len]u8 = null;
var s_challenge_counter: u32 = 0x5A000001;

fn raw(err: CfgError) RawErr {
    return @backingInt(err);
}

/// Read one copy through the store and validate it. A read fault or blank
/// window is "copy absent", so the resolver falls through rather than failing
/// the whole load; it never fabricates a record.
fn probe(read: ReadFn, offset: u32, out: *Record) bool {
    var buf: [implementation.record_len]u8 = @splat(0);
    if (read(offset, &buf, implementation.record_len) != raw(.ok)) {
        return false;
    }
    if (!implementation.copyValid(&buf)) {
        return false;
    }
    implementation.deserialize(&buf, out);
    return true;
}

/// Load and resolve the device configuration record from both copies.
pub export fn ra8_devcfg_load(store: ?*const Store) callconv(.c) RawErr {
    const s = store orelse {
        ra8_log_emit_error(tag, "load: store null");
        return raw(.null_ptr);
    };
    const read = s.read orelse {
        ra8_log_emit_error(tag, "load: store->read null");
        return raw(.null_ptr);
    };

    var rec0: Record = .{};
    var rec1: Record = .{};
    const valid0 = probe(read, implementation.copy0_off, &rec0);
    const valid1 = probe(read, implementation.copy1_off, &rec1);

    if (valid0 and valid1) {
        // Both survive -- the higher sequence is the newer record; a tie takes 0.
        s_record = if (rec0.seq >= rec1.seq) rec0 else rec1;
    } else if (valid0) {
        s_record = rec0;
    } else if (valid1) {
        s_record = rec1;
    } else {
        s_state = .unprovisioned;
        ra8_log_emit_warn(tag, "no valid device config -- UNPROVISIONED");
        return raw(.validation_failed);
    }
    s_state = .loaded;
    return raw(.ok);
}

/// Fetch the validated panel VCOM magnitude in millivolts. Any error means
/// INV-VCOM-1 forbids driving the panel.
pub export fn ra8_devcfg_get_vcom_mv(out_mv: ?*u16) callconv(.c) RawErr {
    const out = out_mv orelse {
        ra8_log_emit_error(tag, "get_vcom: out_mv null");
        return raw(.null_ptr);
    };
    if (s_state != .loaded) {
        return raw(.not_initialized);
    }
    if ((s_record.flags & implementation.flag_vcom_valid) == 0) {
        ra8_log_emit_error(tag, "VCOM not marked valid -- refuse the panel");
        return raw(.validation_failed);
    }
    const mv = s_record.body.panel_vcom_mv;
    if (!implementation.vcomInRange(mv)) {
        ra8_log_emit_error(tag, "VCOM out of plausible range -- refuse the panel");
        return raw(.validation_failed);
    }
    out.* = mv;
    return raw(.ok);
}

/// Expose the decoded body of the loaded record. The pointer aliases the
/// module cache and stays valid until the next load or reset.
pub export fn ra8_devcfg_get_body(out_body: ?*?*const Body) callconv(.c) RawErr {
    const out = out_body orelse {
        ra8_log_emit_error(tag, "get_body: out_body null");
        return raw(.null_ptr);
    };
    if (s_state != .loaded) {
        return raw(.not_initialized);
    }
    out.* = &s_record.body;
    return raw(.ok);
}

/// Report whether neither record copy is valid: the provisioning gate. True
/// before any load has run, too.
pub export fn ra8_devcfg_is_blank() callconv(.c) bool {
    return s_state != .loaded;
}

/// Commit a new record to the stale copy slot with header-last ordering, so a
/// power cut before the header lands leaves that slot header-invalid and the
/// other copy the sole survivor.
pub export fn ra8_devcfg_commit(store: ?*const Store, rec: ?*const Record) callconv(.c) RawErr {
    const s = store orelse {
        ra8_log_emit_error(tag, "commit: store null");
        return raw(.null_ptr);
    };
    const read = s.read orelse {
        ra8_log_emit_error(tag, "commit: store->read null");
        return raw(.null_ptr);
    };
    const write = s.write orelse {
        ra8_log_emit_error(tag, "commit: store->write null");
        return raw(.null_ptr);
    };
    const record = rec orelse {
        ra8_log_emit_error(tag, "commit: rec null");
        return raw(.null_ptr);
    };

    if (s_state == .loaded and (s_record.flags & implementation.flag_provisioned) != 0) {
        ra8_log_emit_error(tag, "commit: unit already provisioned -- re-entry refused");
        return raw(.access_denied);
    }

    var cur0: Record = .{};
    var cur1: Record = .{};
    const valid0 = probe(read, implementation.copy0_off, &cur0);
    const valid1 = probe(read, implementation.copy1_off, &cur1);
    const seq0: u32 = if (valid0) cur0.seq else 0;
    const seq1: u32 = if (valid1) cur1.seq else 0;
    const new_seq = implementation.nextSeq(valid0, seq0, valid1, seq1);
    const target = implementation.targetOffset(valid0, seq0, valid1, seq1);

    var buf: [implementation.record_len]u8 = @splat(0);
    implementation.serialize(record, new_seq, &buf);

    const body_err = write(
        target + implementation.hdr_bytes,
        buf[implementation.hdr_bytes..].ptr,
        implementation.body_bytes,
    );
    if (body_err != raw(.ok)) {
        ra8_log_emit_error(tag, "commit: body write failed");
        ra8_log_emit_error_val(tag, "Error", body_err);
        return body_err;
    }
    const hdr_err = write(target, &buf, implementation.hdr_bytes);
    if (hdr_err != raw(.ok)) {
        ra8_log_emit_error(tag, "commit: header write failed");
        ra8_log_emit_error_val(tag, "Error", hdr_err);
        return hdr_err;
    }
    return raw(.ok);
}

/// Drop the cached record so a later load re-resolves from scratch. Touches no
/// backing store.
pub export fn ra8_devcfg_reset() callconv(.c) void {
    s_state = .unloaded;
    s_record = .{};
    s_challenge_active = false;
    @memset(&s_challenge_nonce, 0);
    s_test_pubkey = null;
}

/// Request an anti-replay challenge nonce for virgin factory provisioning.
/// Permanently refuses if the unit is already provisioned (gate closed).
pub export fn ra8_devcfg_get_provision_challenge(out_nonce: ?[*]u8) callconv(.c) RawErr {
    const out = out_nonce orelse {
        ra8_log_emit_error(tag, "get_challenge: out_nonce null");
        return raw(.null_ptr);
    };
    if (s_state == .loaded and (s_record.flags & implementation.flag_provisioned) != 0) {
        ra8_log_emit_error(tag, "get_challenge: unit already provisioned -- permanently refused");
        return raw(.access_denied);
    }
    if (!s_challenge_active) {
        s_challenge_counter +%= 1;
        var seed: [16]u8 = undefined;
        std.mem.writeInt(u32, seed[0..4], s_challenge_counter, .little);
        std.mem.writeInt(u32, seed[4..8], s_challenge_counter ^ 0x55AA55AA, .little);
        std.mem.writeInt(u32, seed[8..12], ~s_challenge_counter, .little);
        std.mem.writeInt(u32, seed[12..16], s_challenge_counter +% 0x12345678, .little);
        @memcpy(&s_challenge_nonce, &seed);
        s_challenge_active = true;
    }
    @memcpy(out[0..implementation.token_nonce_len], &s_challenge_nonce);
    return raw(.ok);
}

/// Execute factory provisioning on a virgin unit using an authenticated token.
/// Permanently refuses re-entry if the unit is already provisioned.
pub export fn ra8_devcfg_provision(
    store: ?*const Store,
    rec: ?*const Record,
    token: ?*const implementation.FactoryToken,
) callconv(.c) RawErr {
    const s = store orelse {
        ra8_log_emit_error(tag, "provision: store null");
        return raw(.null_ptr);
    };
    const read = s.read orelse {
        ra8_log_emit_error(tag, "provision: store->read null");
        return raw(.null_ptr);
    };
    const write = s.write orelse {
        ra8_log_emit_error(tag, "provision: store->write null");
        return raw(.null_ptr);
    };
    const record = rec orelse {
        ra8_log_emit_error(tag, "provision: rec null");
        return raw(.null_ptr);
    };
    const tok = token orelse {
        ra8_log_emit_error(tag, "provision: token null");
        return raw(.null_ptr);
    };

    // Gate 1: Permanently refuse re-entry if already provisioned in memory cache.
    if (s_state == .loaded and (s_record.flags & implementation.flag_provisioned) != 0) {
        ra8_log_emit_error(tag, "provision: permanently locked -- unit already provisioned");
        return raw(.access_denied);
    }

    // Check existing store copies: if either copy is valid and marked provisioned, refuse!
    var cur0: Record = .{};
    var cur1: Record = .{};
    const valid0 = probe(read, implementation.copy0_off, &cur0);
    const valid1 = probe(read, implementation.copy1_off, &cur1);
    if ((valid0 and (cur0.flags & implementation.flag_provisioned) != 0) or
        (valid1 and (cur1.flags & implementation.flag_provisioned) != 0))
    {
        ra8_log_emit_error(tag, "provision: store holds provisioned record -- permanently refused");
        return raw(.access_denied);
    }

    // Gate 2: Challenge must be active
    if (!s_challenge_active) {
        ra8_log_emit_error(tag, "provision: no active challenge -- request challenge first");
        return raw(.access_denied);
    }

    // Gate 3: Cryptographic & parameter token verification
    const pubkey = s_test_pubkey orelse implementation.default_factory_pubkey;
    const verified = implementation.verifyToken(tok, record, &s_challenge_nonce, &pubkey);

    // Active challenge nonce is consumed on every attempt (anti-replay)
    s_challenge_active = false;
    @memset(&s_challenge_nonce, 0);

    if (!verified) {
        ra8_log_emit_error(tag, "provision: token verification failed (forged, tampered, or replayed)");
        return raw(.validation_failed);
    }

    // Mark record as provisioned
    var rec_to_commit = record.*;
    rec_to_commit.flags |= implementation.flag_provisioned;

    const seq0: u32 = if (valid0) cur0.seq else 0;
    const seq1: u32 = if (valid1) cur1.seq else 0;
    const new_seq = implementation.nextSeq(valid0, seq0, valid1, seq1);
    const target = implementation.targetOffset(valid0, seq0, valid1, seq1);

    var buf: [implementation.record_len]u8 = @splat(0);
    implementation.serialize(&rec_to_commit, new_seq, &buf);

    // 1. Write signature headroom (64 bytes) to target + record_len
    const sig_err = write(
        target + implementation.record_len,
        &tok.signature,
        implementation.sig_headroom,
    );
    if (sig_err != raw(.ok)) {
        ra8_log_emit_error(tag, "provision: signature write failed");
        ra8_log_emit_error_val(tag, "Error", sig_err);
        return sig_err;
    }

    // 2. Write body bytes (96 bytes) to target + hdr_bytes
    const body_err = write(
        target + implementation.hdr_bytes,
        buf[implementation.hdr_bytes..].ptr,
        implementation.body_bytes,
    );
    if (body_err != raw(.ok)) {
        ra8_log_emit_error(tag, "provision: body write failed");
        ra8_log_emit_error_val(tag, "Error", body_err);
        return body_err;
    }

    // 3. Write header page (32 bytes) LAST to target
    const hdr_err = write(target, &buf, implementation.hdr_bytes);
    if (hdr_err != raw(.ok)) {
        ra8_log_emit_error(tag, "provision: header write failed");
        ra8_log_emit_error_val(tag, "Error", hdr_err);
        return hdr_err;
    }

    // Adopt provisioned record into module cache
    s_record = rec_to_commit;
    s_record.seq = new_seq;
    s_record.schema_version = implementation.schema_ver;
    s_state = .loaded;

    ra8_log_emit_warn(tag, "factory provisioning SUCCESS");
    return raw(.ok);
}

/// Helper for factory fixtures and tests to build and sign a factory token.
pub export fn ra8_devcfg_build_token(
    rec: ?*const Record,
    nonce: ?[*]const u8,
    secret_key: ?[*]const u8,
    out_token: ?*implementation.FactoryToken,
) callconv(.c) RawErr {
    const record = rec orelse return raw(.null_ptr);
    const non = nonce orelse return raw(.null_ptr);
    const key = secret_key orelse return raw(.null_ptr);
    const tok = out_token orelse return raw(.null_ptr);

    const ok = implementation.signToken(
        record,
        non[0..implementation.token_nonce_len],
        key[0..64],
        tok,
    );
    if (!ok) return raw(.invalid_arg);
    return raw(.ok);
}

/// Test-only hook to override the factory verification public key.
pub export fn ra8_devcfg_set_token_pubkey_for_test(pubkey_32: ?[*]const u8) callconv(.c) void {
    if (pubkey_32) |p| {
        var k: [implementation.token_pubkey_len]u8 = undefined;
        @memcpy(&k, p[0..implementation.token_pubkey_len]);
        s_test_pubkey = k;
    } else {
        s_test_pubkey = null;
    }
}

/// Test-only hook to inject a deterministic challenge nonce.
pub export fn ra8_devcfg_set_challenge_for_test(nonce: ?[*]const u8) callconv(.c) void {
    if (nonce) |n| {
        @memcpy(&s_challenge_nonce, n[0..implementation.token_nonce_len]);
        s_challenge_active = true;
    } else {
        s_challenge_active = false;
        @memset(&s_challenge_nonce, 0);
    }
}

comptime {
    // The production extra-MRAM binding keeps its own file, as its own
    // translation unit did. Reference it here so the archive still exports
    // `ra8_devcfg_default_store` alongside the membrane above.
    _ = @import("store_extra_mram.zig");
}
