//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure parser, bounds/alignment/overlap validator, and serializer helpers for
//! the signed N-image `"RA8U"` update bundle format (`inc/ra8_bundle.h`).

const std = @import("std");
const Err = @import("err").Err;
const pl = @import("pipeline");
const sess = @import("session");

pub const magic: u32 = 0x5241_3855; // "RA8U"
pub const format_version: u16 = 1;
pub const header_bytes: u16 = 32;
pub const desc_bytes: u32 = 64;
pub const sig_slot_bytes: u32 = 64;
pub const page_align: u32 = 32;
pub const max_images: u16 = 4;

/// `ra8_bundle_target_t`.
pub const Target = enum(u32) {
    ra8_app = 1,
    esp32_c6 = 2,
    display = 3,

    pub fn fromRaw(raw: u32) ?Target {
        return switch (raw) {
            1 => .ra8_app,
            2 => .esp32_c6,
            3 => .display,
            else => null,
        };
    }
};

/// `ra8_bundle_payload_fmt_t`.
pub const PayloadFmt = enum(u16) {
    raw = 0,
    rot1_img = 1,
    c6_img = 2,

    pub fn fromRaw(raw: u16) ?PayloadFmt {
        return switch (raw) {
            0 => .raw,
            1 => .rot1_img,
            2 => .c6_img,
            else => null,
        };
    }
};

/// `ra8_bundle_hdr_t` (32 bytes, little-endian on wire).
pub const Header = extern struct {
    magic: u32,
    format_version: u16,
    header_bytes: u16,
    bundle_version: u32,
    image_count: u16,
    flags: u16,
    total_bytes: u32,
    payloads_offset: u32,
    sig_len: u32,
    reserved: u32,
};

/// `ra8_bundle_image_desc_t` (64 bytes, little-endian on wire).
pub const ImageDesc = extern struct {
    target: u32,
    format: u16,
    flags: u16,
    image_version: u32,
    compat_proto_ver: u32,
    offset: u32,
    size: u32,
    reserved0: u32,
    reserved1: u32,
    sha256: [pl.sha256_bytes]u8,
};

comptime {
    std.debug.assert(@sizeOf(Header) == 32);
    std.debug.assert(@offsetOf(Header, "magic") == 0);
    std.debug.assert(@offsetOf(Header, "format_version") == 4);
    std.debug.assert(@offsetOf(Header, "header_bytes") == 6);
    std.debug.assert(@offsetOf(Header, "bundle_version") == 8);
    std.debug.assert(@offsetOf(Header, "image_count") == 12);
    std.debug.assert(@offsetOf(Header, "flags") == 14);
    std.debug.assert(@offsetOf(Header, "total_bytes") == 16);
    std.debug.assert(@offsetOf(Header, "payloads_offset") == 20);
    std.debug.assert(@offsetOf(Header, "sig_len") == 24);
    std.debug.assert(@offsetOf(Header, "reserved") == 28);

    std.debug.assert(@sizeOf(ImageDesc) == 64);
    std.debug.assert(@offsetOf(ImageDesc, "target") == 0);
    std.debug.assert(@offsetOf(ImageDesc, "format") == 4);
    std.debug.assert(@offsetOf(ImageDesc, "flags") == 6);
    std.debug.assert(@offsetOf(ImageDesc, "image_version") == 8);
    std.debug.assert(@offsetOf(ImageDesc, "compat_proto_ver") == 12);
    std.debug.assert(@offsetOf(ImageDesc, "offset") == 16);
    std.debug.assert(@offsetOf(ImageDesc, "size") == 20);
    std.debug.assert(@offsetOf(ImageDesc, "reserved0") == 24);
    std.debug.assert(@offsetOf(ImageDesc, "reserved1") == 28);
    std.debug.assert(@offsetOf(ImageDesc, "sha256") == 32);
}

fn readU16(bytes: []const u8, off: usize) u16 {
    return std.mem.readInt(u16, bytes[off..][0..2], .little);
}

fn readU32(bytes: []const u8, off: usize) u32 {
    return std.mem.readInt(u32, bytes[off..][0..4], .little);
}

/// Byte offset of the ECDSA-P256 raw signature slot inside the container.
pub fn sigOffset(hdr: *const Header) u32 {
    return @as(u32, header_bytes) + @as(u32, hdr.image_count) * desc_bytes;
}

/// Expected `payloads_offset` given a validated `image_count`.
pub fn expectedPayloadsOffset(count: u16) u32 {
    return @as(u32, header_bytes) + @as(u32, count) * desc_bytes + sig_slot_bytes;
}

/// Parse and validate a 32-byte bundle header without reading descriptors.
pub fn parseHeader(raw: []const u8, max_bytes: u32) error{ InvalidArg, InvalidSize }!Header {
    if (raw.len < header_bytes) return error.InvalidSize;
    const hdr = Header{
        .magic = readU32(raw, 0),
        .format_version = readU16(raw, 4),
        .header_bytes = readU16(raw, 6),
        .bundle_version = readU32(raw, 8),
        .image_count = readU16(raw, 12),
        .flags = readU16(raw, 14),
        .total_bytes = readU32(raw, 16),
        .payloads_offset = readU32(raw, 20),
        .sig_len = readU32(raw, 24),
        .reserved = readU32(raw, 28),
    };

    if (hdr.magic != magic) return error.InvalidArg;
    if (hdr.format_version != format_version) return error.InvalidArg;
    if (hdr.header_bytes != header_bytes) return error.InvalidArg;
    if (hdr.image_count == 0 or hdr.image_count > max_images) return error.InvalidArg;
    if (hdr.flags != 0 or hdr.reserved != 0) return error.InvalidArg;
    if (hdr.sig_len == 0 or hdr.sig_len > sig_slot_bytes) return error.InvalidSize;

    const exp_pay = expectedPayloadsOffset(hdr.image_count);
    if (hdr.payloads_offset != exp_pay) return error.InvalidSize;
    if (hdr.total_bytes < exp_pay or hdr.total_bytes > max_bytes) return error.InvalidSize;
    return hdr;
}

/// Decode one 64-byte image descriptor.
pub fn parseDesc(raw: []const u8) error{InvalidSize}!ImageDesc {
    if (raw.len < desc_bytes) return error.InvalidSize;
    var desc = ImageDesc{
        .target = readU32(raw, 0),
        .format = readU16(raw, 4),
        .flags = readU16(raw, 6),
        .image_version = readU32(raw, 8),
        .compat_proto_ver = readU32(raw, 12),
        .offset = readU32(raw, 16),
        .size = readU32(raw, 20),
        .reserved0 = readU32(raw, 24),
        .reserved1 = readU32(raw, 28),
        .sha256 = undefined,
    };
    @memcpy(&desc.sha256, raw[32..64]);
    return desc;
}

/// Parse and structurally validate all `hdr.image_count` descriptors.
pub fn parseAndValidateDescs(
    hdr: *const Header,
    raw_descs: []const u8,
    out_descs: []ImageDesc,
) error{ InvalidArg, InvalidSize }!void {
    const count: usize = @intCast(hdr.image_count);
    const need_bytes = count * desc_bytes;
    if (raw_descs.len < need_bytes or out_descs.len < count) return error.InvalidSize;

    var prev_aligned_end: u64 = hdr.payloads_offset;
    var seen_targets: u32 = 0;

    for (0..count) |idx| {
        const d = try parseDesc(raw_descs[idx * desc_bytes ..][0..desc_bytes]);
        if (Target.fromRaw(d.target) == null) return error.InvalidArg;
        if (PayloadFmt.fromRaw(d.format) == null) return error.InvalidArg;
        if (d.reserved0 != 0 or d.reserved1 != 0) return error.InvalidArg;
        if (d.size == 0) return error.InvalidSize;

        const target_bit = @as(u32, 1) << @as(u5, @intCast(d.target));
        if ((seen_targets & target_bit) != 0) return error.InvalidArg;
        seen_targets |= target_bit;

        if ((d.offset % page_align) != 0) return error.InvalidArg;
        if (@as(u64, d.offset) < prev_aligned_end) return error.InvalidArg;

        const end: u64 = @as(u64, d.offset) + @as(u64, d.size);
        if (end > @as(u64, hdr.total_bytes)) return error.InvalidSize;

        prev_aligned_end = std.mem.alignForward(u64, end, page_align);
        out_descs[idx] = d;
    }
}

/// Locate a validated descriptor matching `target`, if present.
pub fn findTarget(descs: []const ImageDesc, target: Target) ?ImageDesc {
    for (descs) |d| {
        if (d.target == @backingInt(target)) return d;
    }
    return null;
}

/// Check companion link protocol compatibility (`0` = no version constraint).
pub fn compatProtoAllowed(desc: *const ImageDesc, required_proto: u32) bool {
    if (required_proto == 0 or desc.compat_proto_ver == 0) return true;
    return desc.compat_proto_ver == required_proto;
}

/// Map internal parse error to canonical `ra8_err_t` value.
pub fn mapParseErr(e: error{ InvalidArg, InvalidSize }) u16 {
    return switch (e) {
        error.InvalidArg => Err.invalid_arg.raw(),
        error.InvalidSize => Err.invalid_size.raw(),
    };
}

/// Verify `SHA-256(header[0..32] || descs[0..N*64])` against `sig[0..sig_len]`.
pub fn verifyBundleSignature(
    crypto: *const sess.CryptoIface,
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

/// Verify a staged `"RA8U"` bundle via `session.cfg.storage.readback` and
/// `session.cfg.crypto`, enforcing signature, rollback floor, proto compat,
/// and every descriptor's SHA-256 payload digest.
pub fn verifyBundleSession(
    session: *sess.Session,
    req_compat_proto: u32,
    scratch: []u8,
    out_hdr: *Header,
    out_descs: []ImageDesc,
) u16 {
    if (out_descs.len == 0 or scratch.len == 0) return Err.invalid_size.raw();
    if (session.stage != @backingInt(pl.Stage.staged)) return Err.invalid_state.raw();

    session.stage = @backingInt(pl.Stage.verifying);
    var hdr_buf: [header_bytes]u8 = undefined;
    const rd_hdr = session.cfg.storage.readback.?(
        session.cfg.storage.ctx,
        0,
        &hdr_buf,
        header_bytes,
    );
    if (rd_hdr != Err.ok.raw()) return sess.transitionRollback(session, rd_hdr);

    const hdr = parseHeader(&hdr_buf, session.cfg.max_bytes) catch |e| {
        return sess.transitionRollback(session, mapParseErr(e));
    };
    if (hdr.total_bytes != session.expected_bytes) {
        return sess.transitionRollback(session, Err.invalid_size.raw());
    }
    if (@as(usize, hdr.image_count) > out_descs.len) {
        return sess.transitionRollback(session, Err.invalid_size.raw());
    }

    const meta_span: usize = @intCast(expectedPayloadsOffset(hdr.image_count));
    var meta_buf: [header_bytes + max_images * desc_bytes + sig_slot_bytes]u8 = undefined;
    const rd_meta = session.cfg.storage.readback.?(
        session.cfg.storage.ctx,
        0,
        &meta_buf,
        @intCast(meta_span),
    );
    if (rd_meta != Err.ok.raw()) return sess.transitionRollback(session, rd_meta);

    var local_descs: [max_images]ImageDesc = undefined;
    const sig_off: usize = @intCast(sigOffset(&hdr));
    parseAndValidateDescs(&hdr, meta_buf[header_bytes..sig_off], &local_descs) catch |e| {
        return sess.transitionRollback(session, mapParseErr(e));
    };

    if (session.cfg.decl.enforce_rot) {
        const sig_err = verifyBundleSignature(
            &session.cfg.crypto,
            session.cfg.pubkey_handle,
            meta_buf[0..sig_off],
            meta_buf[sig_off .. sig_off + hdr.sig_len],
        );
        if (sig_err != Err.ok.raw()) return sess.transitionRollback(session, sig_err);
    }

    if (session.cfg.decl.enforce_rollback) {
        var floor: u32 = 0;
        const read_err = session.cfg.rollback.read_floor.?(session.cfg.rollback.ctx, &floor);
        if (read_err != Err.ok.raw()) return sess.transitionRollback(session, read_err);
        if (hdr.bundle_version < floor) {
            return sess.transitionRollback(session, Err.validation_failed.raw());
        }
    }

    for (0..@as(usize, hdr.image_count)) |i| {
        if (!compatProtoAllowed(&local_descs[i], req_compat_proto)) {
            return sess.transitionRollback(session, Err.validation_failed.raw());
        }
        var digest: [pl.sha256_bytes]u8 = @splat(0);
        const h_err = sess.rehashSpan(
            session,
            local_descs[i].offset,
            local_descs[i].size,
            scratch,
            &digest,
        );
        if (h_err != Err.ok.raw()) return sess.transitionRollback(session, h_err);
        if (!pl.equalConstantTime(&digest, &local_descs[i].sha256)) {
            return sess.transitionRollback(session, Err.crc_mismatch.raw());
        }
    }

    var root_digest: [pl.sha256_bytes]u8 = @splat(0);
    const meta_hash_err = session.cfg.crypto.sha256_init.?(session.cfg.crypto.ctx);
    if (meta_hash_err != Err.ok.raw()) return sess.transitionRollback(session, meta_hash_err);
    const meta_upd_err = session.cfg.crypto.sha256_update.?(session.cfg.crypto.ctx, &meta_buf, @intCast(sig_off));
    if (meta_upd_err != Err.ok.raw()) return sess.transitionRollback(session, meta_upd_err);
    const meta_fin_err = session.cfg.crypto.sha256_final.?(session.cfg.crypto.ctx, &root_digest);
    if (meta_fin_err != Err.ok.raw()) return sess.transitionRollback(session, meta_fin_err);

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
