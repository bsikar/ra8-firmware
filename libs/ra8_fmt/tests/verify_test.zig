//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for `libs/ra8_fmt/src/internal/verify.zig`.

const std = @import("std");
const c_abi = @import("c_abi");
const verify = @import("verify");
const mocks = @import("mocks");

const MockSpool = struct {
    buf: [2048]u8 = undefined,
    len: usize = 0,
    sealed: bool = false,
    tile_h: u16 = 1,
    bpp: u8 = 3,
    corrupt_byte: bool = false,

    fn readAt(ctx: ?*anyopaque, off: u64, out: ?[*]u8, len: usize, got: ?*usize) callconv(.c) c_abi.Ra8Err {
        const self: *MockSpool = @ptrCast(@alignCast(ctx.?));
        if (off >= self.len) {
            got.?.* = 0;
            return c_abi.k_ra8_ok;
        }
        const avail = @min(len, self.len - @as(usize, @intCast(off)));
        @memcpy(out.?[0..avail], self.buf[@intCast(off)..][0..avail]);
        got.?.* = avail;
        return c_abi.k_ra8_ok;
    }

    fn append(ctx: ?*anyopaque, bytes: ?[*]const u8, len: usize) callconv(.c) c_abi.Ra8Err {
        const self: *MockSpool = @ptrCast(@alignCast(ctx.?));
        if (self.len + len > self.buf.len) return c_abi.k_ra8_err_no_mem;
        if (len > 0) @memcpy(self.buf[self.len..][0..len], bytes.?[0..len]);
        self.len += len;
        return c_abi.k_ra8_ok;
    }

    fn seal(ctx: ?*anyopaque, expected: u64) callconv(.c) c_abi.Ra8Err {
        const self: *MockSpool = @ptrCast(@alignCast(ctx.?));
        if (expected != self.len) return c_abi.k_ra8_err_invalid_size;
        self.sealed = true;
        return c_abi.k_ra8_ok;
    }

    fn asSpool(self: *MockSpool) c_abi.Spool {
        return .{ .read_at = readAt, .append = append, .seal = seal, .ctx = self };
    }
};

fn makeVerifyDeps(comptime mismatch: bool) c_abi.Deps {
    var d = mocks.defaultStubDeps();
    const H = struct {
        fn produce(cfg: *const c_abi.JofProduceCfg, info: *c_abi.JofInfo) callconv(.c) c_abi.Ra8Err {
            const spool: *c_abi.Spool = @ptrCast(@alignCast(cfg.sink_ctx.?));
            const sp: *MockSpool = @ptrCast(@alignCast(spool.ctx.?));
            const hdr = [_]u8{
                @intCast(cfg.max_width & 0xFF),
                @intCast(cfg.max_height & 0xFF),
                @intCast(cfg.tile_h & 0xFF),
                sp.bpp,
            };
            const rc = cfg.sink.?(cfg.sink_ctx, &hdr, hdr.len);
            if (rc != c_abi.k_ra8_ok) return rc;
            sp.tile_h = cfg.tile_h;
            info.* = .{
                .width = cfg.max_width,
                .height = cfg.max_height,
                .tile_w = cfg.tile_w,
                .tile_h = cfg.tile_h,
                .tile_cols = 1,
                .tile_rows = (@as(u16, cfg.max_height) + cfg.tile_h - 1) / cfg.tile_h,
                .bpp = sp.bpp,
                .codec = cfg.codec,
                .tile_count = (@as(u16, cfg.max_height) + cfg.tile_h - 1) / cfg.tile_h,
                .index_off = 32,
                .total_size = 4,
            };
            return c_abi.k_ra8_ok;
        }
        fn parse(pread: ?c_abi.PreadFn, ctx: ?*anyopaque, sz: u64, info: *c_abi.JofInfo) callconv(.c) c_abi.Ra8Err {
            _ = pread;
            _ = sz;
            const sp: *MockSpool = @ptrCast(@alignCast(ctx.?));
            info.tile_h = sp.tile_h;
            return c_abi.k_ra8_ok;
        }
        fn readTile(
            pread: ?c_abi.PreadFn,
            ctx: ?*anyopaque,
            info: *const c_abi.JofInfo,
            tx: u16,
            ty: u16,
            scratch: ?[*]u8,
            sc: u32,
            px: ?[*]u8,
            oc: u32,
            ow: *u16,
            oh: *u16,
        ) callconv(.c) c_abi.Ra8Err {
            _ = pread;
            _ = tx;
            _ = scratch;
            _ = sc;
            _ = oc;
            const sp: *MockSpool = @ptrCast(@alignCast(ctx.?));
            const th: u16 = @intCast(@min(@as(u32, info.tile_h), info.height - @as(u32, ty) * info.tile_h));
            const row_bytes = @as(usize, info.width) * info.bpp;
            for (0..th) |r| {
                const y = @as(usize, ty) * info.tile_h + r;
                const val: u8 = @intCast((y * 17 + 5) & 0xFF);
                @memset(px.?[r * row_bytes ..][0..row_bytes], val);
                if (mismatch and sp.tile_h > 1 and r == 0 and ty == 0) {
                    px.?[0] ^= 0xFF;
                }
            }
            ow.* = info.width;
            oh.* = th;
            return c_abi.k_ra8_ok;
        }
    };
    d.jof_produce = H.produce;
    d.jof_parse = H.parse;
    d.jof_read_tile = H.readTile;
    return d;
}

fn makeReq(bpp: u8) c_abi.JofVerifyRequirements {
    return .{
        .width = 2,
        .height = 3,
        .bpp = bpp,
        .band_height = 2,
        .reference_work_bytes = 64,
        .banded_work_bytes = 64,
        .webp_work_bytes = 0,
        .band_tile_bytes = 2 * 2 * @as(u32, bpp),
        .scratch_bytes = 64,
        .row_bytes = 2 * @as(u32, bpp),
    };
}

test "jofVerifyStream matches exact round-trip and emits RGBA-stripped P6 PPM" {
    const raw: [32]u8 = @splat(1);
    var s_ref = mocks.MemSource{ .data = &raw };
    var s_got = mocks.MemSource{ .data = &raw };
    const src_ref = s_ref.asSource(true);
    const src_got = s_got.asSource(true);
    const need = makeReq(4);
    var work_buf: [256]u8 = undefined;
    var band_buf: [256]u8 = undefined;
    var scratch_buf: [256]u8 = undefined;
    var row_buf: [256]u8 = undefined;
    var ws = c_abi.JofVerifyWorkspace{
        .work = &work_buf,
        .work_cap = work_buf.len,
        .webp_work = null,
        .webp_work_cap = 0,
        .band_tile = &band_buf,
        .band_tile_cap = band_buf.len,
        .scratch = &scratch_buf,
        .scratch_cap = scratch_buf.len,
        .row = &row_buf,
        .row_cap = row_buf.len,
    };
    var sp1 = MockSpool{ .bpp = 4 };
    var sp2 = MockSpool{ .bpp = 4 };
    var spool1 = sp1.asSpool();
    var spool2 = sp2.asSpool();
    var dump_tx = mocks.MemTx{};
    var dump = dump_tx.asTx();
    var sink_state = mocks.MemSink{};
    const sink = sink_state.asSink();

    const rc = verify.jofVerifyStream(
        comptime makeVerifyDeps(false),
        &src_ref,
        &src_got,
        &need,
        &ws,
        &spool1,
        &spool2,
        &dump,
        "out.ppm",
        &sink,
    );
    try std.testing.expectEqual(c_abi.k_ra8_ok, rc);
    try std.testing.expect(dump_tx.committed);
    try std.testing.expect(std.mem.startsWith(u8, dump_tx.published[0..dump_tx.published_len], "P6\n2 3\n255\n"));
    try std.testing.expectEqual("P6\n2 3\n255\n".len + 2 * 3 * 3, dump_tx.published_len);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "ROUND-TRIP EXACT") != null);
}

test "jofVerifyStream reports pixel diffs and commits diagnostic dump even on mismatch" {
    const raw: [32]u8 = @splat(1);
    var s_ref = mocks.MemSource{ .data = &raw };
    var s_got = mocks.MemSource{ .data = &raw };
    const src_ref = s_ref.asSource(false);
    const src_got = s_got.asSource(false);
    const need = makeReq(1);
    var work_buf: [256]u8 = undefined;
    var band_buf: [256]u8 = undefined;
    var scratch_buf: [256]u8 = undefined;
    var row_buf: [256]u8 = undefined;
    var ws = c_abi.JofVerifyWorkspace{
        .work = &work_buf,
        .work_cap = work_buf.len,
        .webp_work = null,
        .webp_work_cap = 0,
        .band_tile = &band_buf,
        .band_tile_cap = band_buf.len,
        .scratch = &scratch_buf,
        .scratch_cap = scratch_buf.len,
        .row = &row_buf,
        .row_cap = row_buf.len,
    };
    var sp1 = MockSpool{ .bpp = 1 };
    var sp2 = MockSpool{ .bpp = 1 };
    var spool1 = sp1.asSpool();
    var spool2 = sp2.asSpool();
    var dump_tx = mocks.MemTx{};
    var dump = dump_tx.asTx();
    var sink_state = mocks.MemSink{};
    const sink = sink_state.asSink();

    const rc = verify.jofVerifyStream(
        comptime makeVerifyDeps(true),
        &src_ref,
        &src_got,
        &need,
        &ws,
        &spool1,
        &spool2,
        &dump,
        "diff.ppm",
        &sink,
    );
    try std.testing.expectEqual(c_abi.k_ra8_err_validation_failed, rc);
    try std.testing.expect(dump_tx.committed);
    try std.testing.expect(std.mem.startsWith(u8, dump_tx.published[0..dump_tx.published_len], "P5\n2 3\n255\n"));
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "  DIFF at pixel (0, 0):") != null);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "MISMATCH (1 differing bytes)") != null);
}

test "jofVerifyStream rejects aliased contexts, undersized buffers, and keeps dump non-fatal on append fail" {
    const raw: [32]u8 = @splat(1);
    var shared_src = mocks.MemSource{ .data = &raw };
    const src_a = shared_src.asSource(false);
    const src_b = shared_src.asSource(false);
    const need = makeReq(3);
    var work_buf: [256]u8 = undefined;
    var band_buf: [256]u8 = undefined;
    var scratch_buf: [256]u8 = undefined;
    var row_buf: [256]u8 = undefined;
    var ws = c_abi.JofVerifyWorkspace{
        .work = &work_buf,
        .work_cap = work_buf.len,
        .webp_work = null,
        .webp_work_cap = 0,
        .band_tile = &band_buf,
        .band_tile_cap = band_buf.len,
        .scratch = &scratch_buf,
        .scratch_cap = scratch_buf.len,
        .row = &row_buf,
        .row_cap = row_buf.len,
    };
    var sp1 = MockSpool{ .bpp = 3 };
    var sp2 = MockSpool{ .bpp = 3 };
    var spool1 = sp1.asSpool();
    var spool2 = sp2.asSpool();
    var sink_state = mocks.MemSink{};
    const sink = sink_state.asSink();

    try std.testing.expectEqual(
        c_abi.k_ra8_err_null_ptr,
        verify.jofVerifyStream(comptime makeVerifyDeps(false), &src_a, &src_b, &need, &ws, &spool1, &spool2, null, null, &sink),
    );

    var s_ref = mocks.MemSource{ .data = &raw };
    var s_got = mocks.MemSource{ .data = &raw };
    const r = s_ref.asSource(false);
    const g = s_got.asSource(false);
    var bad_ws = ws;
    bad_ws.row_cap = 1;
    try std.testing.expectEqual(
        c_abi.k_ra8_err_invalid_size,
        verify.jofVerifyStream(comptime makeVerifyDeps(false), &r, &g, &need, &bad_ws, &spool1, &spool2, null, null, &sink),
    );

    var dump_tx = mocks.MemTx{ .fail_append_on = 2 };
    var dump = dump_tx.asTx();
    const rc_dump = verify.jofVerifyStream(comptime makeVerifyDeps(false), &r, &g, &need, &ws, &spool1, &spool2, &dump, "fail.ppm", &sink);
    try std.testing.expectEqual(c_abi.k_ra8_ok, rc_dump);
    try std.testing.expect(dump_tx.aborted);
    try std.testing.expect(!dump_tx.committed);
    try std.testing.expect(std.mem.indexOf(u8, sink_state.text(), "wrote reassembled raster to fail.ppm (FAILED)") != null);
}
