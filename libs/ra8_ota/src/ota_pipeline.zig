//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Adapter bridging `libs/ra8_ota` to the shared `libs/ra8_update` pipeline
//! session engine and `"RA8U"` N-image signed bundle verifier.
//!
//! Zero `export fn` symbols live here so any target linking both `libra8_ota.a`
//! and `libra8_update.a` never sees duplicate C symbols.

const std = @import("std");
const Err = @import("err").Err;
const pl = @import("pipeline");
const sess = @import("session");
const bn = @import("bundle");
const impl = @import("internal/root.zig");

pub const max_meta_bytes: usize =
    bn.header_bytes + @as(usize, bn.max_images) * bn.desc_bytes + bn.sig_slot_bytes;

pub const State = struct {
    session: sess.Session = undefined,
    cfg_ptr: ?*const impl.Cfg = null,
    floor: u32 = 0,
    bundle_mode: bool = false,
    descs_ready: bool = false,
    probe: [4]u8 = @splat(0),
    probe_len: u8 = 0,
    meta_buf: [max_meta_bytes]u8 = @splat(0),
    meta_pay_off: u32 = max_meta_bytes,
    bundle_total: u32 = 0,
    bundle_hdr: bn.Header = std.mem.zeroes(bn.Header),
    bundle_descs: [bn.max_images]bn.ImageDesc = undefined,
    ra8_off: u32 = 0,
    ra8_size: u32 = 0,
    bundle_version: u32 = 0,

    pub fn reset(self: *State) void {
        self.* = .{};
    }

    pub fn initSession(self: *State, cfg: *const impl.Cfg) u16 {
        self.reset();
        self.cfg_ptr = cfg;
        var decl = pl.canonicalDecl(.ota);
        decl.payload = @backingInt(pl.PayloadKind.manifest_raw);
        const pl_cfg = sess.Cfg{
            .decl = decl,
            .pubkey_handle = cfg.pubkey_handle,
            .align_bytes = 1,
            .max_bytes = cfg.flash.bank_size_bytes,
            .reserved0 = 0,
            .storage = .{
                .prepare = otaPrepare,
                .write = otaWrite,
                .readback = otaReadback,
                .ctx = self,
            },
            .crypto = .{
                .sha256_init = cfg.crypto.sha256_init,
                .sha256_update = cfg.crypto.sha256_update,
                .sha256_final = cfg.crypto.sha256_final,
                .ecdsa_verify = cfg.crypto.ecdsa_verify,
                .ctx = cfg.crypto.ctx,
            },
            .rollback = .{
                .read_floor = otaReadFloor,
                .commit_floor = otaCommitFloor,
                .ctx = self,
            },
            .commit = .{
                .commit = otaCommit,
                .confirm = null,
                .rollback = null,
                .ctx = self,
            },
        };
        return sess.init(&self.session, &pl_cfg);
    }

    fn activeCfg(self: *const State) ?*const impl.Cfg {
        return self.cfg_ptr;
    }

    fn resetDownloadScratch(self: *State) void {
        self.bundle_mode = false;
        self.descs_ready = false;
        self.probe_len = 0;
        self.probe = @splat(0);
        self.meta_pay_off = max_meta_bytes;
        self.bundle_total = 0;
        self.bundle_hdr = std.mem.zeroes(bn.Header);
        self.ra8_off = 0;
        self.ra8_size = 0;
        self.bundle_version = 0;
    }

    fn flashProgram(self: *const State, bank_off: u32, src: [*]const u8, len: u32) u16 {
        if (len == 0) return Err.ok.raw();
        const cfg = self.activeCfg() orelse return impl.err.not_initialized;
        return cfg.flash.program.?(
            cfg.flash.ctx,
            cfg.flash.inactive_bank_addr + bank_off,
            src,
            len,
        );
    }

    fn flashReadback(self: *const State, bank_off: u32, dst: [*]u8, len: u32) u16 {
        if (len == 0) return Err.ok.raw();
        const cfg = self.activeCfg() orelse return impl.err.not_initialized;
        return cfg.flash.readback.?(
            cfg.flash.ctx,
            cfg.flash.inactive_bank_addr + bank_off,
            dst,
            len,
        );
    }

    fn mapBundlePayloadOff(self: *const State, container_off: u32) u32 {
        if (container_off >= self.ra8_off) {
            return container_off - self.ra8_off;
        }
        return (self.bundle_total - self.ra8_off) + (container_off - self.meta_pay_off);
    }

    fn writeBundleSpan(self: *State, start_off: u32, src: []const u8) u16 {
        var cursor: usize = 0;
        var pos: u32 = start_off;

        while (cursor < src.len) {
            if (pos < bn.header_bytes) {
                const take: usize = @min(src.len - cursor, @as(usize, bn.header_bytes - pos));
                @memcpy(self.meta_buf[pos .. pos + take], src[cursor .. cursor + take]);
                pos += @intCast(take);
                cursor += take;
                if (pos == bn.header_bytes) {
                    const hdr = bn.parseHeader(
                        self.meta_buf[0..bn.header_bytes],
                        self.session.cfg.max_bytes,
                    ) catch |e| return bn.mapParseErr(e);
                    if (hdr.total_bytes != self.session.expected_bytes) {
                        return Err.invalid_size.raw();
                    }
                    self.bundle_hdr = hdr;
                    self.bundle_total = hdr.total_bytes;
                    self.meta_pay_off = hdr.payloads_offset;
                    self.bundle_version = hdr.bundle_version;
                }
            } else if (!self.descs_ready and pos < self.meta_pay_off) {
                const take: usize = @min(src.len - cursor, @as(usize, self.meta_pay_off - pos));
                @memcpy(self.meta_buf[pos .. pos + take], src[cursor .. cursor + take]);
                pos += @intCast(take);
                cursor += take;
                if (pos == self.meta_pay_off) {
                    const sig_off: usize = @intCast(bn.sigOffset(&self.bundle_hdr));
                    bn.parseAndValidateDescs(
                        &self.bundle_hdr,
                        self.meta_buf[bn.header_bytes..sig_off],
                        &self.bundle_descs,
                    ) catch |e| return bn.mapParseErr(e);
                    const hit = bn.findTarget(
                        self.bundle_descs[0..self.bundle_hdr.image_count],
                        .ra8_app,
                    ) orelse return Err.not_found.raw();
                    if (hit.format != @backingInt(bn.PayloadFmt.raw) and
                        hit.format != @backingInt(bn.PayloadFmt.rot1_img))
                    {
                        return Err.invalid_arg.raw();
                    }
                    self.ra8_off = hit.offset;
                    self.ra8_size = hit.size;
                    self.descs_ready = true;
                }
            } else {
                if (!self.descs_ready) return Err.invalid_state.raw();
                const rem = src.len - cursor;
                const chunk_len: usize = if (pos < self.ra8_off)
                    @min(rem, @as(usize, self.ra8_off - pos))
                else
                    rem;
                const mapped = self.mapBundlePayloadOff(pos);
                const e = self.flashProgram(mapped, src[cursor..].ptr, @intCast(chunk_len));
                if (e != Err.ok.raw()) return e;
                pos += @intCast(chunk_len);
                cursor += chunk_len;
            }
        }
        return Err.ok.raw();
    }
};

fn stateFromCtx(ctx: ?*anyopaque) ?*State {
    const raw = ctx orelse return null;
    return @ptrCast(@alignCast(raw));
}

fn otaPrepare(ctx: ?*anyopaque, total_bytes: u32) callconv(.c) u16 {
    const st = stateFromCtx(ctx) orelse return Err.null_ptr.raw();
    const cfg = st.activeCfg() orelse return impl.err.not_initialized;
    st.resetDownloadScratch();
    return cfg.flash.erase.?(cfg.flash.ctx, cfg.flash.inactive_bank_addr, total_bytes);
}

fn otaWrite(ctx: ?*anyopaque, offset: u32, data: [*]const u8, len: u32) callconv(.c) u16 {
    const st = stateFromCtx(ctx) orelse return Err.null_ptr.raw();
    const slice = data[0..len];

    if (st.bundle_mode) {
        return st.writeBundleSpan(offset, slice);
    }

    if (offset < 4 and st.session.expected_bytes >= bn.header_bytes) {
        var idx: usize = 0;
        while (idx < slice.len and st.probe_len < 4) : (idx += 1) {
            st.probe[st.probe_len] = slice[idx];
            st.probe_len += 1;
        }
        if (st.probe_len < 4) return Err.ok.raw();
        const magic = std.mem.readInt(u32, &st.probe, .little);
        if (magic == bn.magic) {
            st.bundle_mode = true;
            st.session.cfg.decl.payload = @backingInt(pl.PayloadKind.bundle);
            const p_err = st.writeBundleSpan(0, st.probe[0..4]);
            if (p_err != Err.ok.raw()) return p_err;
            return st.writeBundleSpan(4, slice[idx..]);
        }
        const f_err = st.flashProgram(0, &st.probe, 4);
        if (f_err != Err.ok.raw()) return f_err;
        return st.flashProgram(4, slice[idx..].ptr, @intCast(slice.len - idx));
    }

    return st.flashProgram(offset, data, len);
}

fn otaReadback(ctx: ?*anyopaque, offset: u32, dst: [*]u8, len: u32) callconv(.c) u16 {
    const st = stateFromCtx(ctx) orelse return Err.null_ptr.raw();
    if (!st.bundle_mode) {
        return st.flashReadback(offset, dst, len);
    }

    var cursor: usize = 0;
    var pos: u32 = offset;
    const target_len: usize = @intCast(len);
    while (cursor < target_len) {
        const rem = target_len - cursor;
        if (pos < st.meta_pay_off) {
            const take: usize = @min(rem, @as(usize, st.meta_pay_off - pos));
            @memcpy(dst[cursor .. cursor + take], st.meta_buf[pos .. pos + take]);
            pos += @intCast(take);
            cursor += take;
        } else {
            const take: usize = if (pos < st.ra8_off)
                @min(rem, @as(usize, st.ra8_off - pos))
            else
                rem;
            const mapped = st.mapBundlePayloadOff(pos);
            const e = st.flashReadback(mapped, dst + cursor, @intCast(take));
            if (e != Err.ok.raw()) return e;
            pos += @intCast(take);
            cursor += take;
        }
    }
    return Err.ok.raw();
}

fn otaReadFloor(ctx: ?*anyopaque, out_floor: *u32) callconv(.c) u16 {
    const st = stateFromCtx(ctx) orelse return Err.null_ptr.raw();
    out_floor.* = st.floor;
    return Err.ok.raw();
}

fn otaCommitFloor(ctx: ?*anyopaque, new_floor: u32) callconv(.c) u16 {
    const st = stateFromCtx(ctx) orelse return Err.null_ptr.raw();
    if (new_floor > st.floor) st.floor = new_floor;
    return Err.ok.raw();
}

fn otaCommit(ctx: ?*anyopaque, _: u32, bank_idx: u32, _: bool, _: u8) callconv(.c) u16 {
    const st = stateFromCtx(ctx) orelse return Err.null_ptr.raw();
    const cfg = st.activeCfg() orelse return impl.err.not_initialized;
    return cfg.flash.set_startup.?(cfg.flash.ctx, @truncate(bank_idx), true);
}
