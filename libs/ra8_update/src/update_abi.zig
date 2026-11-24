//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `inc/ra8_update.h`: exposes the shared declarative
//! stage -> verify -> commit -> confirm/rollback session engine over caller-
//! injected storage, crypto, rollback, and commit interfaces.

const Err = @import("err").Err;
const pl = @import("pipeline");
const sess = @import("session");

pub const StorageIface = sess.StorageIface;
pub const CryptoIface = sess.CryptoIface;
pub const RollbackIface = sess.RollbackIface;
pub const CommitIface = sess.CommitIface;
pub const Cfg = sess.Cfg;
pub const VerifyReq = sess.VerifyReq;
pub const Session = sess.Session;

pub const transitionRollback = sess.transitionRollback;
pub const rehashSpan = sess.rehashSpan;

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
    return sess.init(session, cfg);
}

export fn ra8_update_stage_begin(session_opt: ?*Session, total_bytes: u32) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    return sess.stageBegin(session, total_bytes);
}

export fn ra8_update_stage_write(
    session_opt: ?*Session,
    offset: u32,
    data_opt: ?[*]const u8,
    len: u32,
) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    const data = data_opt orelse return Err.null_ptr.raw();
    return sess.stageWrite(session, offset, data, len);
}

export fn ra8_update_stage_finish(session_opt: ?*Session) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    return sess.stageFinish(session);
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
    return sess.verify(session, req, scratch_ptr[0..scratch_cap]);
}

export fn ra8_update_commit(session_opt: ?*Session, seq_or_bank: u32) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    return sess.commit(session, seq_or_bank);
}

export fn ra8_update_confirm(session_opt: ?*Session) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    return sess.confirm(session);
}

export fn ra8_update_rollback(session_opt: ?*Session, reason: u16) u16 {
    const session = session_opt orelse return Err.null_ptr.raw();
    return sess.rollback(session, reason);
}
