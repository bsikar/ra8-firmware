//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared `ra8_update` pipeline engine adapter for `ra8_dfu`: drives the
//! canonical DFU declaration (`ab_slots`, `trial_budget = 3`,
//! `defer_floor_bump = true`), auto-detects `"RA8U"` N-image signed bundles
//! on incoming slot downloads, unwraps `ra8_app` cleanly to slot offset 0 while
//! preserving 32-byte MRAM page alignment, and enforces fail-closed ECDSA +
//! SHA-256 verification before the slot `"RA8D"` header is committed.

const std = @import("std");
const dfu_err = @import("err");
const image = @import("image");
const program = @import("program");
const slot = @import("slot");
const upd_bn = @import("bundle");
const upd_err = @import("upd_err");
const upd_pl = @import("pipeline");
const upd_sess = @import("upd_session");

pub const max_meta_bytes: usize =
    upd_bn.header_bytes + @as(usize, upd_bn.max_images) * upd_bn.desc_bytes + upd_bn.sig_slot_bytes;

pub const Backend = struct {
    prepare: *const fn (ctx: ?*anyopaque, which: slot.Slot) u16,
    write: *const fn (ctx: ?*anyopaque, which: slot.Slot, offset: u32, data: []const u8) u16,
    read: *const fn (ctx: ?*anyopaque, which: slot.Slot, offset: u32, dst: []u8) u16,
    crc32: *const fn (ctx: ?*anyopaque, which: slot.Slot, len: u32) u32,
    write_header: *const fn (ctx: ?*anyopaque, which: slot.Slot, hdr: *const image.Header) u16,
    ctx: ?*anyopaque = null,
};

pub const Engine = struct {
    session: upd_sess.Session = undefined,
    crypto: ?upd_sess.CryptoIface = null,
    pubkey_handle: u32 = 0,
    rollback: ?upd_sess.RollbackIface = null,
    backend: ?Backend = null,
    target_slot: slot.Slot = .none,
    floor: u32 = 0,

    bundle_mode: bool = false,
    descs_ready: bool = false,
    meta_buf: [max_meta_bytes]u8 = @splat(0),
    meta_pay_off: u32 = max_meta_bytes,
    bundle_total: u32 = 0,
    bundle_hdr: upd_bn.Header = std.mem.zeroes(upd_bn.Header),
    bundle_descs: [upd_bn.max_images]upd_bn.ImageDesc = undefined,
    ra8_off: u32 = 0,
    ra8_size: u32 = 0,
    aligned_ra8_size: u32 = 0,
    aligned_bundle_tail: u32 = 0,
    scratch: [256]u8 = @splat(0),

    pub fn setBundleHooks(
        self: *Engine,
        crypto_opt: ?*const upd_sess.CryptoIface,
        pubkey_handle: u32,
        rollback_opt: ?*const upd_sess.RollbackIface,
    ) u16 {
        if (crypto_opt) |c| {
            if (c.sha256_init == null or c.sha256_update == null or
                c.sha256_final == null or c.ecdsa_verify == null)
            {
                return dfu_err.Err.null_ptr.raw();
            }
        }
        if (rollback_opt) |rb| {
            if (rb.read_floor == null or rb.commit_floor == null) {
                return dfu_err.Err.invalid_arg.raw();
            }
        }
        self.crypto = if (crypto_opt) |c| c.* else null;
        self.pubkey_handle = if (crypto_opt != null) pubkey_handle else 0;
        self.rollback = if (rollback_opt) |rb| rb.* else null;
        return dfu_err.Err.ok.raw();
    }

    fn resetTransfer(self: *Engine) void {
        self.bundle_mode = false;
        self.descs_ready = false;
        self.meta_pay_off = max_meta_bytes;
        self.bundle_total = 0;
        self.bundle_hdr = std.mem.zeroes(upd_bn.Header);
        self.ra8_off = 0;
        self.ra8_size = 0;
        self.aligned_ra8_size = 0;
        self.aligned_bundle_tail = 0;
    }

    pub fn prepare(self: *Engine, which: slot.Slot, backend: Backend) u16 {
        if (program.base(which) == 0) return dfu_err.Err.invalid_arg.raw();
        self.backend = backend;
        self.target_slot = which;
        self.resetTransfer();

        var decl = upd_pl.canonicalDecl(.dfu);
        decl.payload = @backingInt(upd_pl.PayloadKind.rot1_slot);
        decl.enforce_rot = false;

        const cfg = upd_sess.Cfg{
            .decl = decl,
            .pubkey_handle = self.pubkey_handle,
            .align_bytes = 1,
            .max_bytes = image.layout.img_max,
            .reserved0 = 0,
            .storage = .{
                .prepare = cbPrepare,
                .write = cbWrite,
                .readback = cbReadback,
                .ctx = self,
            },
            .crypto = .{
                .sha256_init = cbShaInit,
                .sha256_update = cbShaUpdate,
                .sha256_final = cbShaFinal,
                .ecdsa_verify = cbEcdsaVerify,
                .ctx = self,
            },
            .rollback = .{
                .read_floor = cbReadFloor,
                .commit_floor = cbCommitFloor,
                .ctx = self,
            },
            .commit = .{
                .commit = cbCommit,
                .confirm = cbConfirm,
                .rollback = null,
                .ctx = self,
            },
        };
        const init_err = upd_sess.init(&self.session, &cfg);
        if (init_err != upd_err.Err.ok.raw()) return init_err;
        return upd_sess.stageBegin(&self.session, image.layout.img_max);
    }

    pub fn writeImage(
        self: *Engine,
        which: slot.Slot,
        offset: u32,
        data: []const u8,
        backend: Backend,
    ) u16 {
        self.backend = backend;
        const staging = self.session.stage == @backingInt(upd_pl.Stage.staging) and
            which == self.target_slot;

        if (!self.bundle_mode and offset == 0 and data.len >= 4 and
            std.mem.readInt(u32, data[0..4], .little) == upd_bn.magic)
        {
            if (self.crypto == null) return dfu_err.Err.null_ptr.raw();
            if (!staging) return dfu_err.Err.invalid_state.raw();
            if (data.len < upd_bn.header_bytes) return dfu_err.Err.invalid_size.raw();
            const hdr = upd_bn.parseHeader(
                data[0..upd_bn.header_bytes],
                image.layout.img_max,
            ) catch |e| return upd_bn.mapParseErr(e);
            self.bundle_mode = true;
            self.bundle_hdr = hdr;
            self.bundle_total = hdr.total_bytes;
            self.meta_pay_off = hdr.payloads_offset;
            self.session.cfg.decl.payload = @backingInt(upd_pl.PayloadKind.bundle);
            self.session.cfg.decl.enforce_rot = true;
            self.session.cfg.pubkey_handle = self.pubkey_handle;
            self.session.expected_bytes = hdr.total_bytes;
        }

        if (self.bundle_mode) {
            if (!staging or offset >= self.bundle_total or offset != self.session.bytes_written) {
                return dfu_err.Err.invalid_arg.raw();
            }
            const logical_len: u32 = @min(@as(u32, @intCast(data.len)), self.bundle_total - offset);
            return upd_sess.stageWrite(&self.session, offset, data.ptr, logical_len);
        }

        if (staging and offset == self.session.bytes_written) {
            return upd_sess.stageWrite(&self.session, offset, data.ptr, @intCast(data.len));
        }
        return backend.write(backend.ctx, which, offset, data);
    }

    pub fn commit(
        self: *Engine,
        which: slot.Slot,
        img_len: u32,
        seq: u32,
        backend: Backend,
    ) u16 {
        if (program.base(which) == 0 or !image.lengthValid(img_len)) {
            return dfu_err.Err.invalid_arg.raw();
        }
        self.backend = backend;

        if (self.bundle_mode and which == self.target_slot) {
            if (!self.descs_ready or img_len < self.bundle_total or
                self.session.bytes_written != self.bundle_total)
            {
                return upd_sess.transitionRollback(&self.session, dfu_err.Err.invalid_size.raw());
            }
            if (self.crypto == null) {
                return upd_sess.transitionRollback(&self.session, dfu_err.Err.null_ptr.raw());
            }
            const fin_err = upd_sess.stageFinish(&self.session);
            if (fin_err != upd_err.Err.ok.raw()) return fin_err;

            var out_hdr: upd_bn.Header = undefined;
            var out_descs: [upd_bn.max_images]upd_bn.ImageDesc = undefined;
            const ver_err = upd_bn.verifyBundleSession(
                &self.session,
                0,
                &self.scratch,
                &out_hdr,
                &out_descs,
            );
            if (ver_err != upd_err.Err.ok.raw()) return ver_err;

            const final_len = self.aligned_ra8_size;
            if (!image.lengthValid(final_len)) {
                return upd_sess.transitionRollback(&self.session, dfu_err.Err.invalid_size.raw());
            }
            const hdr = image.Header{
                .magic = image.layout.hdr_magic,
                .seq = seq,
                .img_len = final_len,
                .img_crc32 = backend.crc32(backend.ctx, which, final_len),
                .entry = image.layout.run_base,
                .rsv0 = 0,
                .rsv1 = 0,
                .rsv2 = 0,
            };
            const wr_err = backend.write_header(backend.ctx, which, &hdr);
            if (wr_err != dfu_err.Err.ok.raw()) {
                return upd_sess.transitionRollback(&self.session, wr_err);
            }
            return upd_sess.commit(&self.session, seq);
        }

        const hdr = image.Header{
            .magic = image.layout.hdr_magic,
            .seq = seq,
            .img_len = img_len,
            .img_crc32 = backend.crc32(backend.ctx, which, img_len),
            .entry = image.layout.run_base,
            .rsv0 = 0,
            .rsv1 = 0,
            .rsv2 = 0,
        };
        const wr_err = backend.write_header(backend.ctx, which, &hdr);
        if (wr_err != dfu_err.Err.ok.raw()) return wr_err;

        if (self.session.stage == @backingInt(upd_pl.Stage.staging) and which == self.target_slot) {
            self.session.expected_bytes = img_len;
            self.session.bytes_written = img_len;
            if (upd_sess.stageFinish(&self.session) == upd_err.Err.ok.raw()) {
                self.session.verified_version = seq;
                self.session.verified = 1;
                self.session.stage = @backingInt(upd_pl.Stage.verified);
                _ = upd_sess.commit(&self.session, seq);
            }
        }
        return dfu_err.Err.ok.raw();
    }

    pub fn confirmPending(self: *Engine) u16 {
        if (self.session.stage == @backingInt(upd_pl.Stage.trial_pending) and
            self.session.verified != 0)
        {
            return upd_sess.confirm(&self.session);
        }
        return dfu_err.Err.ok.raw();
    }

    fn mapBundlePayloadOff(self: *const Engine, container_off: u32) u32 {
        if (container_off >= self.ra8_off) {
            return container_off - self.ra8_off;
        }
        return self.aligned_bundle_tail + (container_off - self.meta_pay_off);
    }

    fn writeAlignedSlot(self: *const Engine, slot_off: u32, bytes: []const u8) u16 {
        const be = self.backend orelse return dfu_err.Err.invalid_state.raw();
        if (bytes.len == 0) return dfu_err.Err.ok.raw();
        const rem_tail = bytes.len % image.layout.page_size;
        const full_len = bytes.len - rem_tail;
        if (full_len > 0) {
            const w_err = be.write(be.ctx, self.target_slot, slot_off, bytes[0..full_len]);
            if (w_err != dfu_err.Err.ok.raw()) return w_err;
        }
        if (rem_tail > 0) {
            var pad_page: [image.layout.page_size]u8 = @splat(program.erased_byte);
            @memcpy(pad_page[0..rem_tail], bytes[full_len..]);
            return be.write(be.ctx, self.target_slot, slot_off + @as(u32, @intCast(full_len)), &pad_page);
        }
        return dfu_err.Err.ok.raw();
    }

    fn writeBundleSpan(self: *Engine, start_off: u32, src: []const u8) u16 {
        var cursor: usize = 0;
        var pos: u32 = start_off;

        while (cursor < src.len) {
            if (!self.descs_ready and pos < self.meta_pay_off) {
                const take: usize = @min(src.len - cursor, @as(usize, self.meta_pay_off - pos));
                @memcpy(self.meta_buf[pos .. pos + take], src[cursor .. cursor + take]);
                pos += @intCast(take);
                cursor += take;
                if (pos == self.meta_pay_off) {
                    const sig_off: usize = @intCast(upd_bn.sigOffset(&self.bundle_hdr));
                    upd_bn.parseAndValidateDescs(
                        &self.bundle_hdr,
                        self.meta_buf[upd_bn.header_bytes..sig_off],
                        &self.bundle_descs,
                    ) catch |e| return upd_bn.mapParseErr(e);

                    const hit = upd_bn.findTarget(
                        self.bundle_descs[0..self.bundle_hdr.image_count],
                        .ra8_app,
                    ) orelse return dfu_err.Err.not_found.raw();
                    if (hit.format != @backingInt(upd_bn.PayloadFmt.raw) and
                        hit.format != @backingInt(upd_bn.PayloadFmt.rot1_img))
                    {
                        return dfu_err.Err.invalid_arg.raw();
                    }

                    self.ra8_off = hit.offset;
                    self.ra8_size = hit.size;
                    self.aligned_ra8_size = @intCast(std.mem.alignForward(
                        u32,
                        hit.size,
                        image.layout.page_size,
                    ));
                    self.aligned_bundle_tail = @intCast(std.mem.alignForward(
                        u32,
                        self.bundle_total - hit.offset,
                        image.layout.page_size,
                    ));
                    self.descs_ready = true;
                }
            } else {
                if (!self.descs_ready) return dfu_err.Err.invalid_state.raw();
                const rem = src.len - cursor;
                const chunk_len: usize = if (pos < self.ra8_off)
                    @min(rem, @as(usize, self.ra8_off - pos))
                else
                    rem;
                const mapped = self.mapBundlePayloadOff(pos);
                const w_err = self.writeAlignedSlot(mapped, src[cursor .. cursor + chunk_len]);
                if (w_err != dfu_err.Err.ok.raw()) return w_err;
                pos += @intCast(chunk_len);
                cursor += chunk_len;
            }
        }
        return dfu_err.Err.ok.raw();
    }
};

fn engineFromCtx(ctx: ?*anyopaque) ?*Engine {
    const raw = ctx orelse return null;
    return @ptrCast(@alignCast(raw));
}

fn cbPrepare(ctx: ?*anyopaque, _: u32) callconv(.c) u16 {
    const eng = engineFromCtx(ctx) orelse return dfu_err.Err.null_ptr.raw();
    const be = eng.backend orelse return dfu_err.Err.invalid_state.raw();
    return be.prepare(be.ctx, eng.target_slot);
}

fn cbWrite(ctx: ?*anyopaque, offset: u32, data: [*]const u8, len: u32) callconv(.c) u16 {
    const eng = engineFromCtx(ctx) orelse return dfu_err.Err.null_ptr.raw();
    if (eng.bundle_mode) return eng.writeBundleSpan(offset, data[0..len]);
    const be = eng.backend orelse return dfu_err.Err.invalid_state.raw();
    return be.write(be.ctx, eng.target_slot, offset, data[0..len]);
}

fn cbReadback(ctx: ?*anyopaque, offset: u32, dst: [*]u8, len: u32) callconv(.c) u16 {
    const eng = engineFromCtx(ctx) orelse return dfu_err.Err.null_ptr.raw();
    const be = eng.backend orelse return dfu_err.Err.invalid_state.raw();
    if (!eng.bundle_mode) return be.read(be.ctx, eng.target_slot, offset, dst[0..len]);

    var cursor: usize = 0;
    var pos: u32 = offset;
    const total: usize = @intCast(len);
    while (cursor < total) {
        const rem = total - cursor;
        if (pos < eng.meta_pay_off) {
            const take: usize = @min(rem, @as(usize, eng.meta_pay_off - pos));
            @memcpy(dst[cursor .. cursor + take], eng.meta_buf[pos .. pos + take]);
            pos += @intCast(take);
            cursor += take;
        } else {
            const take: usize = if (pos < eng.ra8_off)
                @min(rem, @as(usize, eng.ra8_off - pos))
            else
                rem;
            const mapped = eng.mapBundlePayloadOff(pos);
            const r_err = be.read(be.ctx, eng.target_slot, mapped, dst[cursor .. cursor + take]);
            if (r_err != dfu_err.Err.ok.raw()) return r_err;
            pos += @intCast(take);
            cursor += take;
        }
    }
    return dfu_err.Err.ok.raw();
}

fn cbShaInit(ctx: ?*anyopaque) callconv(.c) u16 {
    const eng = engineFromCtx(ctx) orelse return dfu_err.Err.null_ptr.raw();
    const c = eng.crypto orelse return dfu_err.Err.null_ptr.raw();
    return c.sha256_init.?(c.ctx);
}

fn cbShaUpdate(ctx: ?*anyopaque, data: [*]const u8, len: u32) callconv(.c) u16 {
    const eng = engineFromCtx(ctx) orelse return dfu_err.Err.null_ptr.raw();
    const c = eng.crypto orelse return dfu_err.Err.null_ptr.raw();
    return c.sha256_update.?(c.ctx, data, len);
}

fn cbShaFinal(ctx: ?*anyopaque, out32: [*]u8) callconv(.c) u16 {
    const eng = engineFromCtx(ctx) orelse return dfu_err.Err.null_ptr.raw();
    const c = eng.crypto orelse return dfu_err.Err.null_ptr.raw();
    return c.sha256_final.?(c.ctx, out32);
}

fn cbEcdsaVerify(
    ctx: ?*anyopaque,
    pubkey: u32,
    digest32: [*]const u8,
    sig: [*]const u8,
    sig_len: u32,
) callconv(.c) u16 {
    const eng = engineFromCtx(ctx) orelse return dfu_err.Err.null_ptr.raw();
    const c = eng.crypto orelse return dfu_err.Err.null_ptr.raw();
    return c.ecdsa_verify.?(c.ctx, pubkey, digest32, sig, sig_len);
}

fn cbReadFloor(ctx: ?*anyopaque, out_floor: *u32) callconv(.c) u16 {
    const eng = engineFromCtx(ctx) orelse return dfu_err.Err.null_ptr.raw();
    if (eng.rollback) |rb| {
        if (rb.read_floor) |rf| return rf(rb.ctx, out_floor);
    }
    out_floor.* = eng.floor;
    return dfu_err.Err.ok.raw();
}

fn cbCommitFloor(ctx: ?*anyopaque, new_floor: u32) callconv(.c) u16 {
    const eng = engineFromCtx(ctx) orelse return dfu_err.Err.null_ptr.raw();
    if (eng.rollback) |rb| {
        if (rb.commit_floor) |cf| return cf(rb.ctx, new_floor);
    }
    if (new_floor > eng.floor) eng.floor = new_floor;
    return dfu_err.Err.ok.raw();
}

fn cbCommit(_: ?*anyopaque, _: u32, _: u32, _: bool, _: u8) callconv(.c) u16 {
    return dfu_err.Err.ok.raw();
}

fn cbConfirm(_: ?*anyopaque) callconv(.c) u16 {
    return dfu_err.Err.ok.raw();
}

/// Singleton shared DFU pipeline engine used across `program_abi` and `boot_state_abi`.
pub var shared: Engine = .{};
