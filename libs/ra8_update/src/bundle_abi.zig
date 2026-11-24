//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `inc/ra8_bundle.h`: parses, validates, and verifies
//! `"RA8U"` N-image signed firmware containers over injected crypto hooks.

const Err = @import("err").Err;
const pl = @import("pipeline");
const bundle = @import("bundle");
const update_abi = @import("update_abi");

/// Verify `SHA-256(header[0..32] || descs[0..N*64])` against `sig[0..sig_len]`.
pub fn verifyBundleSignature(
    crypto: *const update_abi.CryptoIface,
    pubkey_handle: u32,
    hdr_and_descs: []const u8,
    sig: []const u8,
) u16 {
    const init_fn = crypto.sha256_init orelse return Err.null_ptr.raw();
    const upd_fn = crypto.sha256_update orelse return Err.null_ptr.raw();
    const fin_fn = crypto.sha256_final orelse return Err.null_ptr.raw();
    const ver_fn = crypto.ecdsa_verify orelse return Err.null_ptr.raw();

    var code = init_fn(crypto.ctx);
    if (code != Err.ok.raw()) return code;
    code = upd_fn(crypto.ctx, hdr_and_descs.ptr, @intCast(hdr_and_descs.len));
    if (code != Err.ok.raw()) return code;
    var digest: [pl.sha256_bytes]u8 = @splat(0);
    code = fin_fn(crypto.ctx, &digest);
    if (code != Err.ok.raw()) return code;

    const ver_err = ver_fn(crypto.ctx, pubkey_handle, &digest, sig.ptr, @intCast(sig.len));
    return if (ver_err == Err.ok.raw()) Err.ok.raw() else Err.validation_failed.raw();
}

export fn ra8_bundle_parse_header(
    raw_opt: ?[*]const u8,
    raw_len: u32,
    max_bytes: u32,
    out_hdr_opt: ?*bundle.Header,
) u16 {
    const out_hdr = out_hdr_opt orelse return Err.null_ptr.raw();
    const raw = raw_opt orelse return Err.null_ptr.raw();
    const hdr = bundle.parseHeader(raw[0..raw_len], max_bytes) catch |e| {
        return bundle.mapParseErr(e);
    };
    out_hdr.* = hdr;
    return Err.ok.raw();
}

export fn ra8_bundle_parse_and_validate(
    container_opt: ?[*]const u8,
    container_len: u32,
    max_bytes: u32,
    out_hdr_opt: ?*bundle.Header,
    out_descs_opt: ?[*]bundle.ImageDesc,
    descs_cap: u32,
) u16 {
    const out_hdr = out_hdr_opt orelse return Err.null_ptr.raw();
    const out_descs = out_descs_opt orelse return Err.null_ptr.raw();
    const container = container_opt orelse return Err.null_ptr.raw();
    if (descs_cap == 0) return Err.invalid_size.raw();

    const raw = container[0..container_len];
    const hdr = bundle.parseHeader(raw, max_bytes) catch |e| {
        return bundle.mapParseErr(e);
    };
    if (hdr.total_bytes != container_len) return Err.invalid_size.raw();
    if (@as(u32, hdr.image_count) > descs_cap) return Err.invalid_size.raw();

    var local_descs: [bundle.max_images]bundle.ImageDesc = undefined;
    const desc_start: usize = bundle.header_bytes;
    const desc_bytes: usize = @as(usize, hdr.image_count) * bundle.desc_bytes;
    bundle.parseAndValidateDescs(&hdr, raw[desc_start .. desc_start + desc_bytes], &local_descs) catch |e| {
        return bundle.mapParseErr(e);
    };

    out_hdr.* = hdr;
    for (0..@as(usize, hdr.image_count)) |i| {
        out_descs[i] = local_descs[i];
    }
    return Err.ok.raw();
}

export fn ra8_bundle_find_target(
    descs_opt: ?[*]const bundle.ImageDesc,
    image_count: u16,
    target_raw: u32,
    out_desc_opt: ?*bundle.ImageDesc,
) u16 {
    const out_desc = out_desc_opt orelse return Err.null_ptr.raw();
    const descs = descs_opt orelse return Err.null_ptr.raw();
    if (image_count == 0 or image_count > bundle.max_images) return Err.invalid_arg.raw();
    const target = bundle.Target.fromRaw(target_raw) orelse return Err.invalid_arg.raw();

    const hit = bundle.findTarget(descs[0..image_count], target) orelse return Err.not_found.raw();
    out_desc.* = hit;
    return Err.ok.raw();
}

export fn ra8_bundle_verify_mem(
    container_opt: ?[*]const u8,
    container_len: u32,
    pubkey_handle: u32,
    min_version: u32,
    crypto_opt: ?*const update_abi.CryptoIface,
    scratch_opt: ?[*]u8,
    scratch_cap: u32,
    out_hdr_opt: ?*bundle.Header,
    out_descs_opt: ?[*]bundle.ImageDesc,
    descs_cap: u32,
) u16 {
    _ = scratch_opt;
    _ = scratch_cap;
    const out_hdr = out_hdr_opt orelse return Err.null_ptr.raw();
    const out_descs = out_descs_opt orelse return Err.null_ptr.raw();
    const container = container_opt orelse return Err.null_ptr.raw();
    const crypto = crypto_opt orelse return Err.null_ptr.raw();

    if (crypto.sha256_init == null or crypto.sha256_update == null or
        crypto.sha256_final == null or crypto.ecdsa_verify == null)
    {
        return Err.null_ptr.raw();
    }
    if (descs_cap == 0) return Err.invalid_size.raw();

    const raw = container[0..container_len];
    const hdr = bundle.parseHeader(raw, pl.default_max_bytes) catch |e| {
        return bundle.mapParseErr(e);
    };
    if (hdr.total_bytes != container_len) return Err.invalid_size.raw();
    if (@as(u32, hdr.image_count) > descs_cap) return Err.invalid_size.raw();

    var local_descs: [bundle.max_images]bundle.ImageDesc = undefined;
    const sig_off: usize = @intCast(bundle.sigOffset(&hdr));
    bundle.parseAndValidateDescs(&hdr, raw[bundle.header_bytes..sig_off], &local_descs) catch |e| {
        return bundle.mapParseErr(e);
    };

    const sig_err = verifyBundleSignature(
        crypto,
        pubkey_handle,
        raw[0..sig_off],
        raw[sig_off .. sig_off + hdr.sig_len],
    );
    if (sig_err != Err.ok.raw()) return sig_err;

    if (hdr.bundle_version < min_version) return Err.validation_failed.raw();

    for (0..@as(usize, hdr.image_count)) |i| {
        const d = local_descs[i];
        var code = crypto.sha256_init.?(crypto.ctx);
        if (code != Err.ok.raw()) return code;
        const p_start: usize = @intCast(d.offset);
        code = crypto.sha256_update.?(crypto.ctx, raw[p_start..].ptr, d.size);
        if (code != Err.ok.raw()) return code;
        var digest: [pl.sha256_bytes]u8 = @splat(0);
        code = crypto.sha256_final.?(crypto.ctx, &digest);
        if (code != Err.ok.raw()) return code;
        if (!pl.equalConstantTime(&digest, &d.sha256)) return Err.crc_mismatch.raw();
    }

    out_hdr.* = hdr;
    for (0..@as(usize, hdr.image_count)) |i| {
        out_descs[i] = local_descs[i];
    }
    return Err.ok.raw();
}

export fn ra8_update_verify_bundle(
    session_opt: ?*update_abi.Session,
    req_compat_proto: u32,
    scratch_opt: ?[*]u8,
    scratch_cap: u32,
    out_hdr_opt: ?*bundle.Header,
    out_descs_opt: ?[*]bundle.ImageDesc,
    descs_cap: u32,
) u16 {
    const out_hdr = out_hdr_opt orelse return Err.null_ptr.raw();
    const out_descs = out_descs_opt orelse return Err.null_ptr.raw();
    const session = session_opt orelse return Err.null_ptr.raw();
    const scratch_ptr = scratch_opt orelse return Err.null_ptr.raw();
    if (descs_cap == 0 or scratch_cap == 0) return Err.invalid_size.raw();
    if (session.stage != @backingInt(pl.Stage.staged)) return Err.invalid_state.raw();

    session.stage = @backingInt(pl.Stage.verifying);
    var hdr_buf: [bundle.header_bytes]u8 = undefined;
    const rd_hdr = session.cfg.storage.readback.?(
        session.cfg.storage.ctx,
        0,
        &hdr_buf,
        bundle.header_bytes,
    );
    if (rd_hdr != Err.ok.raw()) return update_abi.transitionRollback(session, rd_hdr);

    const hdr = bundle.parseHeader(&hdr_buf, session.cfg.max_bytes) catch |e| {
        return update_abi.transitionRollback(session, bundle.mapParseErr(e));
    };
    if (hdr.total_bytes != session.expected_bytes) {
        return update_abi.transitionRollback(session, Err.invalid_size.raw());
    }
    if (@as(u32, hdr.image_count) > descs_cap) {
        return update_abi.transitionRollback(session, Err.invalid_size.raw());
    }

    const meta_span: usize = @intCast(bundle.expectedPayloadsOffset(hdr.image_count));
    var meta_buf: [bundle.header_bytes + bundle.max_images * bundle.desc_bytes + bundle.sig_slot_bytes]u8 = undefined;
    const rd_meta = session.cfg.storage.readback.?(
        session.cfg.storage.ctx,
        0,
        &meta_buf,
        @intCast(meta_span),
    );
    if (rd_meta != Err.ok.raw()) return update_abi.transitionRollback(session, rd_meta);

    var local_descs: [bundle.max_images]bundle.ImageDesc = undefined;
    const sig_off: usize = @intCast(bundle.sigOffset(&hdr));
    bundle.parseAndValidateDescs(&hdr, meta_buf[bundle.header_bytes..sig_off], &local_descs) catch |e| {
        return update_abi.transitionRollback(session, bundle.mapParseErr(e));
    };

    if (session.cfg.decl.enforce_rot) {
        const sig_err = verifyBundleSignature(
            &session.cfg.crypto,
            session.cfg.pubkey_handle,
            meta_buf[0..sig_off],
            meta_buf[sig_off .. sig_off + hdr.sig_len],
        );
        if (sig_err != Err.ok.raw()) return update_abi.transitionRollback(session, sig_err);
    }

    if (session.cfg.decl.enforce_rollback) {
        var floor: u32 = 0;
        const read_err = session.cfg.rollback.read_floor.?(session.cfg.rollback.ctx, &floor);
        if (read_err != Err.ok.raw()) return update_abi.transitionRollback(session, read_err);
        if (hdr.bundle_version < floor) {
            return update_abi.transitionRollback(session, Err.validation_failed.raw());
        }
    }

    const scratch = scratch_ptr[0..scratch_cap];
    for (0..@as(usize, hdr.image_count)) |i| {
        if (!bundle.compatProtoAllowed(&local_descs[i], req_compat_proto)) {
            return update_abi.transitionRollback(session, Err.validation_failed.raw());
        }
        var digest: [pl.sha256_bytes]u8 = @splat(0);
        const h_err = update_abi.rehashSpan(
            session,
            local_descs[i].offset,
            local_descs[i].size,
            scratch,
            &digest,
        );
        if (h_err != Err.ok.raw()) return update_abi.transitionRollback(session, h_err);
        if (!pl.equalConstantTime(&digest, &local_descs[i].sha256)) {
            return update_abi.transitionRollback(session, Err.crc_mismatch.raw());
        }
    }

    var root_digest: [pl.sha256_bytes]u8 = @splat(0);
    const meta_hash_err = session.cfg.crypto.sha256_init.?(session.cfg.crypto.ctx);
    if (meta_hash_err != Err.ok.raw()) return update_abi.transitionRollback(session, meta_hash_err);
    const meta_upd_err = session.cfg.crypto.sha256_update.?(session.cfg.crypto.ctx, &meta_buf, @intCast(sig_off));
    if (meta_upd_err != Err.ok.raw()) return update_abi.transitionRollback(session, meta_upd_err);
    const meta_fin_err = session.cfg.crypto.sha256_final.?(session.cfg.crypto.ctx, &root_digest);
    if (meta_fin_err != Err.ok.raw()) return update_abi.transitionRollback(session, meta_fin_err);

    session.staged_digest = root_digest;
    session.verified_version = hdr.bundle_version;
    session.verified = 1;
    session.stage = @backingInt(pl.Stage.verified);

    out_hdr.* = hdr;
    for (0..@as(usize, hdr.image_count)) |i| {
        out_descs[i] = local_descs[i];
    }
    return Err.ok.raw();
}
