//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `inc/ra8_update.h`: exposes the shared declarative
//! stage -> verify -> commit -> confirm/rollback session engine over caller-
//! injected storage, crypto, rollback, and commit interfaces.

const std = @import("std");
const Err = @import("err").Err;
const pl = @import("pipeline");

const ptr_bytes = @sizeOf(*anyopaque);
const ptr_align = @alignOf(*anyopaque);

pub const StorageIface = extern struct {
    prepare: ?*const fn (?*anyopaque, u32) callconv(.c) u16,
    write: ?*const fn (?*anyopaque, u32, [*]const u8, u32) callconv(.c) u16,
    readback: ?*const fn (?*anyopaque, u32, [*]u8, u32) callconv(.c) u16,
    ctx: ?*anyopaque,
};

pub const CryptoIface = extern struct {
    sha256_init: ?*const fn (?*anyopaque) callconv(.c) u16,
    sha256_update: ?*const fn (?*anyopaque, [*]const u8, u32) callconv(.c) u16,
    sha256_final: ?*const fn (?*anyopaque, [*]u8) callconv(.c) u16,
    ecdsa_verify: ?*const fn (?*anyopaque, u32, [*]const u8, [*]const u8, u32) callconv(.c) u16,
    ctx: ?*anyopaque,
};

pub const RollbackIface = extern struct {
    read_floor: ?*const fn (?*anyopaque, *u32) callconv(.c) u16,
    commit_floor: ?*const fn (?*anyopaque, u32) callconv(.c) u16,
    ctx: ?*anyopaque,
};

pub const CommitIface = extern struct {
    commit: ?*const fn (?*anyopaque, u32, u32, bool, u8) callconv(.c) u16,
    confirm: ?*const fn (?*anyopaque) callconv(.c) u16,
    rollback: ?*const fn (?*anyopaque, u16) callconv(.c) u16,
    ctx: ?*anyopaque,
};

pub const Cfg = extern struct {
    decl: pl.PipelineDecl,
    pubkey_handle: u32,
    align_bytes: u32,
    max_bytes: u32,
    reserved0: u32,
    storage: StorageIface,
    crypto: CryptoIface,
    rollback: RollbackIface,
    commit: CommitIface,
};

pub const VerifyReq = extern struct {
    expected_version: u32,
    sig_len: u32,
    meta_prefix_len: u32,
    reserved0: u32,
    meta_prefix: ?[*]const u8,
    expected_sha256: ?[*]const u8,
    sig: ?[*]const u8,
};

pub const Session = extern struct {
    cfg: Cfg,
    stage: u8,
    verified: u8,
    last_err: u16,
    expected_bytes: u32,
    bytes_written: u32,
    verified_version: u32,
    staged_digest: [pl.sha256_bytes]u8,
};

comptime {
    std.debug.assert(@sizeOf(StorageIface) == ptr_bytes * 4);
    std.debug.assert(@sizeOf(CryptoIface) == ptr_bytes * 5);
    std.debug.assert(@sizeOf(RollbackIface) == ptr_bytes * 3);
    std.debug.assert(@sizeOf(CommitIface) == ptr_bytes * 4);

    std.debug.assert(@offsetOf(Cfg, "decl") == 0);
    std.debug.assert(@offsetOf(Cfg, "pubkey_handle") == 8);
    std.debug.assert(@offsetOf(Cfg, "align_bytes") == 12);
    std.debug.assert(@offsetOf(Cfg, "max_bytes") == 16);
    std.debug.assert(@offsetOf(Cfg, "reserved0") == 20);
    std.debug.assert(@offsetOf(Cfg, "storage") == std.mem.alignForward(usize, 24, ptr_align));
    std.debug.assert(@offsetOf(Cfg, "crypto") == @offsetOf(Cfg, "storage") + @sizeOf(StorageIface));
    std.debug.assert(@offsetOf(Cfg, "rollback") == @offsetOf(Cfg, "crypto") + @sizeOf(CryptoIface));
    std.debug.assert(@offsetOf(Cfg, "commit") == @offsetOf(Cfg, "rollback") + @sizeOf(RollbackIface));
    std.debug.assert(@sizeOf(Cfg) == @offsetOf(Cfg, "commit") + @sizeOf(CommitIface));

    std.debug.assert(@offsetOf(VerifyReq, "expected_version") == 0);
    std.debug.assert(@offsetOf(VerifyReq, "sig_len") == 4);
    std.debug.assert(@offsetOf(VerifyReq, "meta_prefix_len") == 8);
    std.debug.assert(@offsetOf(VerifyReq, "reserved0") == 12);
    std.debug.assert(@offsetOf(VerifyReq, "meta_prefix") == std.mem.alignForward(usize, 16, ptr_align));
    std.debug.assert(@offsetOf(VerifyReq, "expected_sha256") == @offsetOf(VerifyReq, "meta_prefix") + ptr_bytes);
    std.debug.assert(@offsetOf(VerifyReq, "sig") == @offsetOf(VerifyReq, "expected_sha256") + ptr_bytes);

    std.debug.assert(@offsetOf(Session, "cfg") == 0);
    std.debug.assert(@offsetOf(Session, "stage") == @sizeOf(Cfg));
    std.debug.assert(@offsetOf(Session, "verified") == @sizeOf(Cfg) + 1);
    std.debug.assert(@offsetOf(Session, "last_err") == @sizeOf(Cfg) + 2);
    std.debug.assert(@offsetOf(Session, "expected_bytes") == @sizeOf(Cfg) + 4);
    std.debug.assert(@offsetOf(Session, "bytes_written") == @sizeOf(Cfg) + 8);
    std.debug.assert(@offsetOf(Session, "verified_version") == @sizeOf(Cfg) + 12);
    std.debug.assert(@offsetOf(Session, "staged_digest") == @sizeOf(Cfg) + 16);
}

fn validateCfg(cfg: *const Cfg) u16 {
    const decl_err = pl.validateDecl(&cfg.decl);
    if (decl_err != Err.ok.raw()) return decl_err;
    if (cfg.reserved0 != 0) return Err.invalid_arg.raw();
    if (cfg.max_bytes == 0 or cfg.max_bytes > pl.default_max_bytes) return Err.invalid_size.raw();
    if (cfg.align_bytes != 0 and cfg.align_bytes != 1 and cfg.align_bytes != pl.page_bytes) {
        return Err.invalid_arg.raw();
    }
    if (cfg.storage.prepare == null or cfg.storage.write == null or cfg.storage.readback == null) {
        return Err.null_ptr.raw();
    }
    if (cfg.crypto.sha256_init == null or cfg.crypto.sha256_update == null or cfg.crypto.sha256_final == null) {
        return Err.null_ptr.raw();
    }
    if (cfg.decl.enforce_rot and cfg.crypto.ecdsa_verify == null) return Err.null_ptr.raw();
    if (cfg.decl.enforce_rollback) {
        if (cfg.rollback.read_floor == null or cfg.rollback.commit_floor == null) {
            return Err.null_ptr.raw();
        }
    }
    if (cfg.commit.commit == null) return Err.null_ptr.raw();
    if (cfg.decl.trial_budget > 0 and cfg.commit.confirm == null) return Err.null_ptr.raw();
    return Err.ok.raw();
}

pub fn transitionRollback(session: *Session, reason: u16) u16 {
    if (session.cfg.commit.rollback) |rb| {
        _ = rb(session.cfg.commit.ctx, reason);
    }
    session.verified = 0;
    session.last_err = reason;
    session.stage = @backingInt(pl.Stage.rolled_back);
    return reason;
}

/// Read back `[start_offset .. start_offset + span_bytes)` via `storage.readback`
/// in bounded chunks and compute its SHA-256 digest.
pub fn rehashSpan(
    session: *const Session,
    start_offset: u32,
    span_bytes: u32,
    scratch: []u8,
    out_digest: *[pl.sha256_bytes]u8,
) u16 {
    var code = session.cfg.crypto.sha256_init.?(session.cfg.crypto.ctx);
    if (code != Err.ok.raw()) return code;

    const max_chunks: u32 = (span_bytes / @as(u32, @intCast(scratch.len))) + 2;
    var done: u32 = 0;
    var iter: u32 = 0;
    while (iter < max_chunks and done < span_bytes) : (iter += 1) {
        const want: u32 = @min(span_bytes - done, @as(u32, @intCast(scratch.len)));
        code = session.cfg.storage.readback.?(
            session.cfg.storage.ctx,
            start_offset + done,
            scratch.ptr,
            want,
        );
        if (code != Err.ok.raw()) return code;
        code = session.cfg.crypto.sha256_update.?(session.cfg.crypto.ctx, scratch.ptr, want);
        if (code != Err.ok.raw()) return code;
        done += want;
    }
    if (done != span_bytes) return Err.fail.raw();
    return session.cfg.crypto.sha256_final.?(session.cfg.crypto.ctx, out_digest);
}

fn rehashStaged(session: *const Session, scratch: []u8, out_digest: *[pl.sha256_bytes]u8) u16 {
    return rehashSpan(session, 0, session.expected_bytes, scratch, out_digest);
}

fn verifySignature(
    session: *const Session,
    req: *const VerifyReq,
    digest: *const [pl.sha256_bytes]u8,
) u16 {
    if (req.sig_len == 0 or req.sig_len > pl.sig_max_bytes) return Err.invalid_size.raw();
    const sig_ptr = req.sig orelse return Err.null_ptr.raw();
    var bound: [pl.sha256_bytes]u8 = @splat(0);
    if (req.meta_prefix_len > 0) {
        const meta_ptr = req.meta_prefix orelse return Err.null_ptr.raw();
        var code = session.cfg.crypto.sha256_init.?(session.cfg.crypto.ctx);
        if (code != Err.ok.raw()) return code;
        code = session.cfg.crypto.sha256_update.?(session.cfg.crypto.ctx, meta_ptr, req.meta_prefix_len);
        if (code != Err.ok.raw()) return code;
        code = session.cfg.crypto.sha256_update.?(session.cfg.crypto.ctx, digest, pl.sha256_bytes);
        if (code != Err.ok.raw()) return code;
        code = session.cfg.crypto.sha256_final.?(session.cfg.crypto.ctx, &bound);
        if (code != Err.ok.raw()) return code;
    } else {
        bound = digest.*;
    }
    const ver_err = session.cfg.crypto.ecdsa_verify.?(
        session.cfg.crypto.ctx,
        session.cfg.pubkey_handle,
        &bound,
        sig_ptr,
        req.sig_len,
    );
    return if (ver_err == Err.ok.raw()) Err.ok.raw() else Err.validation_failed.raw();
}

export fn ra8_update_decl_canonical(transport_raw: u8, out_decl_opt: ?*pl.PipelineDecl) u16 {
    const out = out_decl_opt orelse return Err.null_ptr.raw();
    const t = pl.Transport.fromRaw(transport_raw) orelse return Err.invalid_arg.raw();
    out.* = pl.canonicalDecl(t);
    return Err.ok.raw();
}

export fn ra8_update_stage_is_terminal(stage_raw: u8) bool {
    const st = pl.Stage.fromRaw(stage_raw) orelse return false;
    return pl.stageIsTerminal(st);
}

export fn ra8_update_init(session_opt: ?*Session, cfg_opt: ?*const Cfg) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    const cfg = cfg_opt orelse return Err.null_ptr.raw();
    const code = validateCfg(cfg);
    if (code != Err.ok.raw()) return code;

    session.* = .{
        .cfg = cfg.*,
        .stage = @backingInt(pl.Stage.idle),
        .verified = 0,
        .last_err = Err.ok.raw(),
        .expected_bytes = 0,
        .bytes_written = 0,
        .verified_version = 0,
        .staged_digest = @splat(0),
    };
    return Err.ok.raw();
}

export fn ra8_update_stage_begin(session_opt: ?*Session, total_bytes: u32) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    const st = pl.Stage.fromRaw(session.stage) orelse return Err.invalid_state.raw();
    if (st != .idle and st != .rolled_back and st != .confirmed and st != .failed) {
        return Err.invalid_state.raw();
    }
    const sz_err = pl.beginSizeValid(total_bytes, session.cfg.max_bytes, session.cfg.align_bytes);
    if (sz_err != Err.ok.raw()) return sz_err;

    session.stage = @backingInt(pl.Stage.preflight);
    session.verified = 0;
    session.expected_bytes = total_bytes;
    session.bytes_written = 0;
    session.verified_version = 0;
    session.last_err = Err.ok.raw();
    @memset(&session.staged_digest, 0);

    const prep_err = session.cfg.storage.prepare.?(session.cfg.storage.ctx, total_bytes);
    if (prep_err != Err.ok.raw()) {
        return transitionRollback(session, prep_err);
    }
    session.stage = @backingInt(pl.Stage.staging);
    return Err.ok.raw();
}

export fn ra8_update_stage_write(
    session_opt: ?*Session,
    offset: u32,
    data_opt: ?[*]const u8,
    len: u32,
) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    const data = data_opt orelse return Err.null_ptr.raw();
    if (session.stage != @backingInt(pl.Stage.staging)) return Err.invalid_state.raw();

    const span_err = pl.stageWriteValid(
        offset,
        len,
        session.bytes_written,
        session.expected_bytes,
        session.cfg.align_bytes,
    );
    if (span_err != Err.ok.raw()) return span_err;

    const wr_err = session.cfg.storage.write.?(session.cfg.storage.ctx, offset, data, len);
    if (wr_err != Err.ok.raw()) {
        return transitionRollback(session, wr_err);
    }
    session.bytes_written += len;
    return Err.ok.raw();
}

export fn ra8_update_stage_finish(session_opt: ?*Session) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    if (session.stage != @backingInt(pl.Stage.staging)) return Err.invalid_state.raw();
    if (session.bytes_written != session.expected_bytes or session.expected_bytes == 0) {
        return transitionRollback(session, Err.invalid_size.raw());
    }
    session.stage = @backingInt(pl.Stage.staged);
    return Err.ok.raw();
}

export fn ra8_update_verify(
    session_opt: ?*Session,
    req_opt: ?*const VerifyReq,
    scratch_opt: ?[*]u8,
    scratch_cap: u32,
) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    const req = req_opt orelse return Err.null_ptr.raw();
    const scratch_ptr = scratch_opt orelse return Err.null_ptr.raw();
    if (scratch_cap == 0) return Err.invalid_size.raw();
    if (req.meta_prefix_len > pl.max_meta_bytes) return Err.invalid_size.raw();
    if (req.meta_prefix_len > 0 and req.meta_prefix == null) return Err.null_ptr.raw();
    if (session.stage != @backingInt(pl.Stage.staged)) return Err.invalid_state.raw();

    session.stage = @backingInt(pl.Stage.verifying);
    var digest: [pl.sha256_bytes]u8 = @splat(0);
    const rehash_err = rehashStaged(session, scratch_ptr[0..scratch_cap], &digest);
    if (rehash_err != Err.ok.raw()) {
        return transitionRollback(session, rehash_err);
    }

    if (req.expected_sha256) |exp| {
        if (!pl.equalConstantTime(&digest, exp[0..pl.sha256_bytes])) {
            return transitionRollback(session, Err.crc_mismatch.raw());
        }
    }

    if (session.cfg.decl.enforce_rot) {
        const sig_err = verifySignature(session, req, &digest);
        if (sig_err != Err.ok.raw()) {
            return transitionRollback(session, sig_err);
        }
    }

    if (session.cfg.decl.enforce_rollback) {
        var floor: u32 = 0;
        const read_err = session.cfg.rollback.read_floor.?(session.cfg.rollback.ctx, &floor);
        if (read_err != Err.ok.raw()) {
            return transitionRollback(session, read_err);
        }
        if (req.expected_version < floor) {
            return transitionRollback(session, Err.validation_failed.raw());
        }
    }

    session.staged_digest = digest;
    session.verified_version = req.expected_version;
    session.verified = 1;
    session.stage = @backingInt(pl.Stage.verified);
    return Err.ok.raw();
}

export fn ra8_update_commit(session_opt: ?*Session, seq_or_bank: u32) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    if (session.stage != @backingInt(pl.Stage.verified) or session.verified == 0) {
        return Err.invalid_state.raw();
    }

    session.stage = @backingInt(pl.Stage.committing);
    const is_trial = session.cfg.decl.trial_budget > 0;
    const commit_err = session.cfg.commit.commit.?(
        session.cfg.commit.ctx,
        session.expected_bytes,
        seq_or_bank,
        is_trial,
        session.cfg.decl.trial_budget,
    );
    if (commit_err != Err.ok.raw()) {
        return transitionRollback(session, commit_err);
    }

    if (pl.shouldCommitFloorAtCommit(&session.cfg.decl)) {
        const floor_err = session.cfg.rollback.commit_floor.?(
            session.cfg.rollback.ctx,
            session.verified_version,
        );
        if (floor_err != Err.ok.raw()) {
            return transitionRollback(session, floor_err);
        }
    }

    session.stage = if (is_trial)
        @backingInt(pl.Stage.trial_pending)
    else
        @backingInt(pl.Stage.confirmed);
    return Err.ok.raw();
}

export fn ra8_update_confirm(session_opt: ?*Session) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    const st = pl.Stage.fromRaw(session.stage) orelse return Err.invalid_state.raw();
    if (st == .confirmed) return Err.ok.raw();
    if (st != .trial_pending or session.verified == 0) return Err.invalid_state.raw();

    if (session.cfg.commit.confirm) |confirm_fn| {
        const c_err = confirm_fn(session.cfg.commit.ctx);
        if (c_err != Err.ok.raw()) {
            return transitionRollback(session, c_err);
        }
    }
    if (session.cfg.decl.enforce_rollback) {
        const floor_err = session.cfg.rollback.commit_floor.?(
            session.cfg.rollback.ctx,
            session.verified_version,
        );
        if (floor_err != Err.ok.raw()) {
            return transitionRollback(session, floor_err);
        }
    }
    session.stage = @backingInt(pl.Stage.confirmed);
    return Err.ok.raw();
}

export fn ra8_update_rollback(session_opt: ?*Session, reason: u16) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    const code = if (reason == Err.ok.raw()) Err.fail.raw() else reason;
    return transitionRollback(session, code);
}
