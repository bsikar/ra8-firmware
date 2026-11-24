//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the slot geometry and the erase-then-program page loop. The loop
//! runs against a recording backend, so the page sequencing is checked
//! without an MRAM controller.

const std = @import("std");
const dfu_pipeline = @import("dfu_pipeline");
const image = @import("image");
const program = @import("program");
const slot = @import("slot");
const upd_bn = @import("bundle");
const upd_err = @import("upd_err");
const upd_pl = @import("pipeline");
const upd_sess = @import("upd_session");

/// Records every page the loop drives, in order.
const Recorder = struct {
    const Write = struct { addr: u32, erased: bool, byte: u8, len: usize };

    writes: *std.ArrayList(Write),
    fail_on: ?usize = null,
    seen: *usize,

    pub fn programPage(self: Recorder, addr: u32, erased: []const u8, body: []const u8) error{Flash}!void {
        if (self.fail_on) |at| {
            if (self.seen.* == at) {
                self.seen.* += 1;
                return error.Flash;
            }
        }
        self.seen.* += 1;
        self.writes.append(std.testing.allocator, .{
            .addr = addr,
            .erased = true,
            .byte = erased[0],
            .len = erased.len,
        }) catch unreachable;
        self.writes.append(std.testing.allocator, .{
            .addr = addr,
            .erased = false,
            .byte = body[0],
            .len = body.len,
        }) catch unreachable;
    }
};

fn recorder(writes: *std.ArrayList(Recorder.Write), seen: *usize, fail_on: ?usize) Recorder {
    return .{ .writes = writes, .seen = seen, .fail_on = fail_on };
}

test "slot bases are the documented MRAM addresses" {
    try std.testing.expectEqual(@as(u32, 0x0202_0000), program.base(.a));
    try std.testing.expectEqual(@as(u32, 0x0208_F000), program.base(.b));
    try std.testing.expectEqual(@as(u32, 0), program.base(.none));
}

test "the other slot of anything that is not A is A" {
    try std.testing.expectEqual(slot.Slot.b, program.other(.a));
    try std.testing.expectEqual(slot.Slot.a, program.other(.b));
    try std.testing.expectEqual(slot.Slot.a, program.other(.none));
}

test "a body write must be page aligned at both ends" {
    const page = image.layout.page_size;
    try std.testing.expect(program.bodyWriteValid(0, page));
    try std.testing.expect(program.bodyWriteValid(page, page * 4));
    try std.testing.expect(!program.bodyWriteValid(0, 0));
    try std.testing.expect(!program.bodyWriteValid(0, page - 1));
    try std.testing.expect(!program.bodyWriteValid(1, page));
}

test "a body write may not run past the image area" {
    const page = image.layout.page_size;
    try std.testing.expect(program.bodyWriteValid(image.layout.img_max - page, page));
    try std.testing.expect(!program.bodyWriteValid(image.layout.img_max, page));
    try std.testing.expect(!program.bodyWriteValid(image.layout.img_max - page, page * 2));
}

test "every page is erased to all ones before its body goes down" {
    var writes: std.ArrayList(Recorder.Write) = .empty;
    defer writes.deinit(std.testing.allocator);
    var seen: usize = 0;

    const page = image.layout.page_size;
    const body: [page * 3]u8 = @splat(0xA5);
    try program.writePages(recorder(&writes, &seen, null), 0x0202_0000, &body);

    try std.testing.expectEqual(@as(usize, 6), writes.items.len);
    var i: usize = 0;
    while (i < 3) : (i += 1) {
        const addr = 0x0202_0000 + @as(u32, @intCast(i)) * page;
        const erase = writes.items[i * 2];
        const write = writes.items[i * 2 + 1];
        try std.testing.expect(erase.erased);
        try std.testing.expectEqual(addr, erase.addr);
        try std.testing.expectEqual(program.erased_byte, erase.byte);
        try std.testing.expectEqual(@as(usize, page), erase.len);
        try std.testing.expect(!write.erased);
        try std.testing.expectEqual(addr, write.addr);
        try std.testing.expectEqual(@as(u8, 0xA5), write.byte);
    }
}

test "a short tail is written as one partial page" {
    var writes: std.ArrayList(Recorder.Write) = .empty;
    defer writes.deinit(std.testing.allocator);
    var seen: usize = 0;

    const page = image.layout.page_size;
    const body: [page + 8]u8 = @splat(0x11);
    try program.writePages(recorder(&writes, &seen, null), 0, &body);

    try std.testing.expectEqual(@as(usize, 4), writes.items.len);
    try std.testing.expectEqual(@as(usize, page), writes.items[1].len);
    try std.testing.expectEqual(@as(u32, page), writes.items[2].addr);
    try std.testing.expectEqual(@as(usize, 8), writes.items[3].len);
}

test "the loop stops at the first failing page" {
    var writes: std.ArrayList(Recorder.Write) = .empty;
    defer writes.deinit(std.testing.allocator);
    var seen: usize = 0;

    const page = image.layout.page_size;
    const body: [page * 4]u8 = @splat(0x22);
    try std.testing.expectError(
        error.Flash,
        program.writePages(recorder(&writes, &seen, 2), 0, &body),
    );
    // Two pages committed, the third refused, the fourth never attempted.
    try std.testing.expectEqual(@as(usize, 4), writes.items.len);
    try std.testing.expectEqual(@as(usize, 3), seen);
}

// =============================================================================
// Shared ra8_update pipeline + signed "RA8U" DFU bundle tests
// =============================================================================

const SlotSim = struct {
    slot_a: [4096]u8 = @splat(program.erased_byte),
    slot_b: [4096]u8 = @splat(program.erased_byte),
    hdr_a: ?image.Header = null,
    hdr_b: ?image.Header = null,
    sha_ctx: std.crypto.hash.sha2.Sha256 = undefined,
    expected_digest: [32]u8 = @splat(0),
    reject_ecdsa: bool = false,
    ecdsa_calls: u32 = 0,
    floor: u32 = 0,
    floor_commits: u32 = 0,

    fn slotBuf(self: *SlotSim, which: slot.Slot) []u8 {
        return switch (which) {
            .a => &self.slot_a,
            .b => &self.slot_b,
            .none => unreachable,
        };
    }

    fn backend(self: *SlotSim) dfu_pipeline.Backend {
        return .{
            .prepare = simPrepare,
            .write = simWrite,
            .read = simRead,
            .crc32 = simCrc,
            .write_header = simWriteHdr,
            .ctx = self,
        };
    }
};

fn simFrom(ctx: ?*anyopaque) *SlotSim {
    return @ptrCast(@alignCast(ctx.?));
}

fn simPrepare(ctx: ?*anyopaque, which: slot.Slot) u16 {
    const sim = simFrom(ctx);
    @memset(sim.slotBuf(which), program.erased_byte);
    switch (which) {
        .a => sim.hdr_a = null,
        .b => sim.hdr_b = null,
        .none => return 0x103,
    }
    return 0;
}

fn simWrite(ctx: ?*anyopaque, which: slot.Slot, offset: u32, data: []const u8) u16 {
    const sim = simFrom(ctx);
    @memcpy(sim.slotBuf(which)[offset .. offset + data.len], data);
    return 0;
}

fn simRead(ctx: ?*anyopaque, which: slot.Slot, offset: u32, dst: []u8) u16 {
    const sim = simFrom(ctx);
    @memcpy(dst, sim.slotBuf(which)[offset .. offset + dst.len]);
    return 0;
}

fn simCrc(ctx: ?*anyopaque, which: slot.Slot, len: u32) u32 {
    const sim = simFrom(ctx);
    return std.hash.Crc32.hash(sim.slotBuf(which)[0..len]);
}

fn simWriteHdr(ctx: ?*anyopaque, which: slot.Slot, hdr: *const image.Header) u16 {
    const sim = simFrom(ctx);
    switch (which) {
        .a => sim.hdr_a = hdr.*,
        .b => sim.hdr_b = hdr.*,
        .none => return 0x103,
    }
    return 0;
}

fn simShaInit(ctx: ?*anyopaque) callconv(.c) u16 {
    const sim = simFrom(ctx);
    sim.sha_ctx = std.crypto.hash.sha2.Sha256.init(.{});
    return 0;
}

fn simShaUpdate(ctx: ?*anyopaque, data: [*]const u8, len: u32) callconv(.c) u16 {
    const sim = simFrom(ctx);
    sim.sha_ctx.update(data[0..len]);
    return 0;
}

fn simShaFinal(ctx: ?*anyopaque, out32: [*]u8) callconv(.c) u16 {
    const sim = simFrom(ctx);
    var digest: [32]u8 = undefined;
    sim.sha_ctx.final(&digest);
    @memcpy(out32[0..32], &digest);
    return 0;
}

fn simEcdsa(ctx: ?*anyopaque, pubkey: u32, digest: [*]const u8, _: [*]const u8, _: u32) callconv(.c) u16 {
    const sim = simFrom(ctx);
    sim.ecdsa_calls += 1;
    if (sim.reject_ecdsa or pubkey != 0xA8D2) return upd_err.Err.validation_failed.raw();
    if (!std.mem.eql(u8, digest[0..32], &sim.expected_digest)) return upd_err.Err.validation_failed.raw();
    return 0;
}

fn simReadFloor(ctx: ?*anyopaque, out_floor: *u32) callconv(.c) u16 {
    out_floor.* = simFrom(ctx).floor;
    return 0;
}

fn simCommitFloor(ctx: ?*anyopaque, new_floor: u32) callconv(.c) u16 {
    const sim = simFrom(ctx);
    if (new_floor > sim.floor) sim.floor = new_floor;
    sim.floor_commits += 1;
    return 0;
}

fn writeHdrLe(dst: []u8, hdr: upd_bn.Header) void {
    std.mem.writeInt(u32, dst[0..4], hdr.magic, .little);
    std.mem.writeInt(u16, dst[4..6], hdr.format_version, .little);
    std.mem.writeInt(u16, dst[6..8], hdr.header_bytes, .little);
    std.mem.writeInt(u32, dst[8..12], hdr.bundle_version, .little);
    std.mem.writeInt(u16, dst[12..14], hdr.image_count, .little);
    std.mem.writeInt(u16, dst[14..16], hdr.flags, .little);
    std.mem.writeInt(u32, dst[16..20], hdr.total_bytes, .little);
    std.mem.writeInt(u32, dst[20..24], hdr.payloads_offset, .little);
    std.mem.writeInt(u32, dst[24..28], hdr.sig_len, .little);
    std.mem.writeInt(u32, dst[28..32], hdr.reserved, .little);
}

fn writeDescLe(dst: []u8, d: upd_bn.ImageDesc) void {
    std.mem.writeInt(u32, dst[0..4], d.target, .little);
    std.mem.writeInt(u16, dst[4..6], d.format, .little);
    std.mem.writeInt(u16, dst[6..8], d.flags, .little);
    std.mem.writeInt(u32, dst[8..12], d.image_version, .little);
    std.mem.writeInt(u32, dst[12..16], d.compat_proto_ver, .little);
    std.mem.writeInt(u32, dst[16..20], d.offset, .little);
    std.mem.writeInt(u32, dst[20..24], d.size, .little);
    std.mem.writeInt(u32, dst[24..28], d.reserved0, .little);
    std.mem.writeInt(u32, dst[28..32], d.reserved1, .little);
    @memcpy(dst[32..64], &d.sha256);
}

fn streamDfuBlocks(eng: *dfu_pipeline.Engine, sim: *SlotSim, bytes: []const u8) u16 {
    var off: u32 = 0;
    while (off < bytes.len) {
        const rem = bytes.len - off;
        if (rem >= 64) {
            const e = eng.writeImage(.b, off, bytes[off .. off + 64], sim.backend());
            if (e != 0) return e;
            off += 64;
        } else {
            var tail: [64]u8 = @splat(program.erased_byte);
            @memcpy(tail[0..rem], bytes[off..]);
            const padded = std.mem.alignForward(usize, rem, image.layout.page_size);
            const e = eng.writeImage(.b, off, tail[0..padded], sim.backend());
            if (e != 0) return e;
            off += @intCast(padded);
        }
    }
    return 0;
}

test "dfu pipeline drives 64B DFU chunks of single and multi-image RA8U bundles and defers floor bump" {
    var sim = SlotSim{};
    var eng = dfu_pipeline.Engine{};

    const crypto = upd_sess.CryptoIface{
        .sha256_init = simShaInit,
        .sha256_update = simShaUpdate,
        .sha256_final = simShaFinal,
        .ecdsa_verify = simEcdsa,
        .ctx = &sim,
    };
    const rollback = upd_sess.RollbackIface{
        .read_floor = simReadFloor,
        .commit_floor = simCommitFloor,
        .ctx = &sim,
    };

    // Multi-image bundle with esp32_c6 first and unaligned ra8_app size (300 bytes) second.
    var c6: [150]u8 = undefined;
    for (&c6, 0..) |*b, idx| b.* = @truncate((idx * 5) +% 1);
    var ra8_img: [300]u8 = undefined;
    for (&ra8_img, 0..) |*b, idx| b.* = @truncate((idx * 11) +% 7);

    const pay_off = upd_bn.expectedPayloadsOffset(2);
    const ra8_off = std.mem.alignForward(u32, pay_off + @as(u32, c6.len), upd_bn.page_align);
    const total: u32 = ra8_off + @as(u32, ra8_img.len); // deliberately not 32B aligned tail

    var container: [1024]u8 = @splat(0);
    var c6_sha: [32]u8 = undefined;
    var ra8_sha: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(&c6, &c6_sha, .{});
    std.crypto.hash.sha2.Sha256.hash(&ra8_img, &ra8_sha, .{});

    const hdr = upd_bn.Header{
        .magic = upd_bn.magic,
        .format_version = upd_bn.format_version,
        .header_bytes = upd_bn.header_bytes,
        .bundle_version = 8,
        .image_count = 2,
        .flags = 0,
        .total_bytes = @intCast(total),
        .payloads_offset = pay_off,
        .sig_len = upd_bn.sig_slot_bytes,
        .reserved = 0,
    };
    const d0 = upd_bn.ImageDesc{
        .target = @backingInt(upd_bn.Target.esp32_c6),
        .format = @backingInt(upd_bn.PayloadFmt.c6_img),
        .flags = 0,
        .image_version = 3,
        .compat_proto_ver = 1,
        .offset = pay_off,
        .size = c6.len,
        .reserved0 = 0,
        .reserved1 = 0,
        .sha256 = c6_sha,
    };
    const d1 = upd_bn.ImageDesc{
        .target = @backingInt(upd_bn.Target.ra8_app),
        .format = @backingInt(upd_bn.PayloadFmt.rot1_img),
        .flags = 0,
        .image_version = 8,
        .compat_proto_ver = 1,
        .offset = ra8_off,
        .size = ra8_img.len,
        .reserved0 = 0,
        .reserved1 = 0,
        .sha256 = ra8_sha,
    };
    writeHdrLe(container[0..32], hdr);
    writeDescLe(container[32..96], d0);
    writeDescLe(container[96..160], d1);
    @memset(container[160..pay_off], 0x5A);
    @memcpy(container[pay_off .. pay_off + c6.len], &c6);
    @memcpy(container[ra8_off .. ra8_off + ra8_img.len], &ra8_img);
    std.crypto.hash.sha2.Sha256.hash(container[0..160], &sim.expected_digest, .{});

    // Fail closed if crypto hooks are absent.
    try std.testing.expectEqual(@as(u16, 0), eng.prepare(.b, sim.backend()));
    try std.testing.expectEqual(@as(u16, 0x504), eng.writeImage(.b, 0, container[0..64], sim.backend()));
    try std.testing.expect(!eng.bundle_mode);

    // Wire hooks and stream the multi-image bundle in 64-byte DFU blocks.
    try std.testing.expectEqual(@as(u16, 0), eng.setBundleHooks(&crypto, 0xA8D2, &rollback));
    try std.testing.expectEqual(@as(u16, 0), eng.prepare(.b, sim.backend()));
    try std.testing.expectEqual(@as(u16, 0), streamDfuBlocks(&eng, &sim, container[0..total]));
    try std.testing.expect(eng.bundle_mode);
    try std.testing.expectEqual(@backingInt(upd_pl.PayloadKind.bundle), eng.session.cfg.decl.payload);
    try std.testing.expectEqualSlices(u8, &ra8_img, sim.slot_b[0..ra8_img.len]);

    const padded_total: u32 = @intCast(std.mem.alignForward(usize, total, image.layout.page_size));
    try std.testing.expectEqual(@as(u16, 0), eng.commit(.b, padded_total, 12, sim.backend()));
    try std.testing.expectEqual(@as(u32, 1), sim.ecdsa_calls);
    try std.testing.expect(sim.hdr_b != null);
    try std.testing.expectEqual(@as(u32, 320), sim.hdr_b.?.img_len);
    try std.testing.expectEqual(@as(u32, 12), sim.hdr_b.?.seq);
    // Rollback floor must stay 0 during trial commit, advancing only on confirmPending.
    try std.testing.expectEqual(@as(u32, 0), sim.floor_commits);
    try std.testing.expectEqual(@as(u32, 0), sim.floor);
    try std.testing.expectEqual(@backingInt(upd_pl.Stage.trial_pending), eng.session.stage);

    try std.testing.expectEqual(@as(u16, 0), eng.confirmPending());
    try std.testing.expectEqual(@as(u32, 1), sim.floor_commits);
    try std.testing.expectEqual(@as(u32, 8), sim.floor);
    try std.testing.expectEqual(@backingInt(upd_pl.Stage.confirmed), eng.session.stage);
}

test "dfu pipeline rejects invalid ECDSA and tampered RA8U bytes without writing RA8D header" {
    var sim = SlotSim{};
    var eng = dfu_pipeline.Engine{};
    const crypto = upd_sess.CryptoIface{
        .sha256_init = simShaInit,
        .sha256_update = simShaUpdate,
        .sha256_final = simShaFinal,
        .ecdsa_verify = simEcdsa,
        .ctx = &sim,
    };
    const rollback = upd_sess.RollbackIface{
        .read_floor = simReadFloor,
        .commit_floor = simCommitFloor,
        .ctx = &sim,
    };
    try std.testing.expectEqual(@as(u16, 0), eng.setBundleHooks(&crypto, 0xA8D2, &rollback));

    var ra8_img: [128]u8 = @splat(0x33);
    const pay_off = upd_bn.expectedPayloadsOffset(1);
    const total: u32 = pay_off + @as(u32, ra8_img.len);
    var container: [512]u8 = @splat(0);
    var ra8_sha: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(&ra8_img, &ra8_sha, .{});

    const hdr = upd_bn.Header{
        .magic = upd_bn.magic,
        .format_version = upd_bn.format_version,
        .header_bytes = upd_bn.header_bytes,
        .bundle_version = 4,
        .image_count = 1,
        .flags = 0,
        .total_bytes = total,
        .payloads_offset = pay_off,
        .sig_len = upd_bn.sig_slot_bytes,
        .reserved = 0,
    };
    const desc = upd_bn.ImageDesc{
        .target = @backingInt(upd_bn.Target.ra8_app),
        .format = @backingInt(upd_bn.PayloadFmt.raw),
        .flags = 0,
        .image_version = 4,
        .compat_proto_ver = 0,
        .offset = pay_off,
        .size = ra8_img.len,
        .reserved0 = 0,
        .reserved1 = 0,
        .sha256 = ra8_sha,
    };
    writeHdrLe(container[0..32], hdr);
    writeDescLe(container[32..96], desc);
    @memset(container[96..pay_off], 0x22);
    @memcpy(container[pay_off..total], &ra8_img);
    std.crypto.hash.sha2.Sha256.hash(container[0..96], &sim.expected_digest, .{});

    // 1. Bad signature never commits an RA8D slot header.
    sim.reject_ecdsa = true;
    try std.testing.expectEqual(@as(u16, 0), eng.prepare(.b, sim.backend()));
    try std.testing.expectEqual(@as(u16, 0), streamDfuBlocks(&eng, &sim, container[0..total]));
    try std.testing.expectEqual(upd_err.Err.validation_failed.raw(), eng.commit(.b, total, 2, sim.backend()));
    try std.testing.expect(sim.hdr_b == null);
    try std.testing.expectEqual(@backingInt(upd_pl.Stage.rolled_back), eng.session.stage);

    // 2. Tampered MRAM payload fails SHA verification without writing RA8D header.
    sim.reject_ecdsa = false;
    try std.testing.expectEqual(@as(u16, 0), eng.prepare(.b, sim.backend()));
    try std.testing.expectEqual(@as(u16, 0), streamDfuBlocks(&eng, &sim, container[0..total]));
    sim.slot_b[17] ^= 0xFF;
    try std.testing.expectEqual(upd_err.Err.crc_mismatch.raw(), eng.commit(.b, total, 2, sim.backend()));
    try std.testing.expect(sim.hdr_b == null);

    // 3. Downgrade against floor rejects cleanly.
    sim.floor = 10;
    try std.testing.expectEqual(@as(u16, 0), eng.prepare(.b, sim.backend()));
    try std.testing.expectEqual(@as(u16, 0), streamDfuBlocks(&eng, &sim, container[0..total]));
    try std.testing.expectEqual(upd_err.Err.validation_failed.raw(), eng.commit(.b, total, 2, sim.backend()));
    try std.testing.expect(sim.hdr_b == null);
}
