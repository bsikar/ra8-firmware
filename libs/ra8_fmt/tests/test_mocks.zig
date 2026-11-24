//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared in-memory source, sink, spool, transaction, and C dependency mocks
//! for `libs/ra8_fmt` Zig unit tests.

const std = @import("std");
const c_abi = @import("c_abi");

pub const MemSource = struct {
    data: []const u8,
    max_chunk: usize = std.math.maxInt(usize),
    read_rc: c_abi.Ra8Err = c_abi.k_ra8_ok,
    validate_rc: c_abi.Ra8Err = c_abi.k_ra8_ok,
    validate_calls: u32 = 0,
    validate_after_reads: u32 = std.math.maxInt(u32),
    reads_seen: u32 = 0,
    inflate_got: usize = std.math.maxInt(usize),

    pub fn readAt(
        ctx: ?*anyopaque,
        offset: u64,
        buf: ?[*]u8,
        len: usize,
        got: ?*usize,
    ) callconv(.c) c_abi.Ra8Err {
        const self: *MemSource = @ptrCast(@alignCast(ctx.?));
        self.reads_seen += 1;
        if (self.read_rc != c_abi.k_ra8_ok) return self.read_rc;
        if (self.inflate_got != std.math.maxInt(usize)) {
            got.?.* = self.inflate_got;
            return c_abi.k_ra8_ok;
        }
        if (offset >= self.data.len) {
            got.?.* = 0;
            return c_abi.k_ra8_ok;
        }
        const remain = self.data.len - @as(usize, @intCast(offset));
        const take = @min(len, @min(remain, self.max_chunk));
        if (take > 0) {
            @memcpy(buf.?[0..take], self.data[@intCast(offset)..][0..take]);
        }
        got.?.* = take;
        return c_abi.k_ra8_ok;
    }

    pub fn validate(ctx: ?*anyopaque, expected_size: u64) callconv(.c) c_abi.Ra8Err {
        const self: *MemSource = @ptrCast(@alignCast(ctx.?));
        _ = expected_size;
        self.validate_calls += 1;
        if (self.reads_seen >= self.validate_after_reads) {
            return self.validate_rc;
        }
        return if (self.validate_after_reads == std.math.maxInt(u32)) self.validate_rc else c_abi.k_ra8_ok;
    }

    pub fn asSource(self: *MemSource, with_validate: bool) c_abi.Source {
        return .{
            .read_at = readAt,
            .validate = if (with_validate) validate else null,
            .ctx = self,
            .size = self.data.len,
        };
    }
};

pub const MemSink = struct {
    buf: [8192]u8 = undefined,
    len: usize = 0,
    fail_after: usize = std.math.maxInt(usize),
    fail_rc: c_abi.Ra8Err = c_abi.k_ra8_err_invalid_size,

    pub fn write(
        ctx: ?*anyopaque,
        bytes: ?[*]const u8,
        count: usize,
    ) callconv(.c) c_abi.Ra8Err {
        const self: *MemSink = @ptrCast(@alignCast(ctx.?));
        if (self.len + count > self.fail_after or self.len + count > self.buf.len) {
            return self.fail_rc;
        }
        if (count > 0) {
            @memcpy(self.buf[self.len..][0..count], bytes.?[0..count]);
            self.len += count;
        }
        return c_abi.k_ra8_ok;
    }

    pub fn asSink(self: *MemSink) c_abi.Sink {
        return .{ .write = write, .ctx = self };
    }

    pub fn text(self: *const MemSink) []const u8 {
        return self.buf[0..self.len];
    }
};

pub const MemTx = struct {
    stage: [8192]u8 = undefined,
    stage_len: usize = 0,
    published: [8192]u8 = undefined,
    published_len: usize = 0,
    append_rc: c_abi.Ra8Err = c_abi.k_ra8_ok,
    commit_rc: c_abi.Ra8Err = c_abi.k_ra8_ok,
    aborted: bool = false,
    committed: bool = false,
    fail_append_on: u32 = 0,
    append_calls: u32 = 0,

    pub fn append(ctx: ?*anyopaque, bytes: ?[*]const u8, len: usize) callconv(.c) c_abi.Ra8Err {
        const self: *MemTx = @ptrCast(@alignCast(ctx.?));
        self.append_calls += 1;
        if (self.fail_append_on != 0 and self.append_calls >= self.fail_append_on) {
            return c_abi.k_ra8_err_no_mem;
        }
        if (self.append_rc != c_abi.k_ra8_ok) return self.append_rc;
        if (self.stage_len + len > self.stage.len) return c_abi.k_ra8_err_invalid_size;
        if (len > 0) {
            @memcpy(self.stage[self.stage_len..][0..len], bytes.?[0..len]);
            self.stage_len += len;
        }
        return c_abi.k_ra8_ok;
    }

    pub fn commit(ctx: ?*anyopaque) callconv(.c) c_abi.Ra8Err {
        const self: *MemTx = @ptrCast(@alignCast(ctx.?));
        if (self.commit_rc != c_abi.k_ra8_ok) return self.commit_rc;
        @memcpy(self.published[0..self.stage_len], self.stage[0..self.stage_len]);
        self.published_len = self.stage_len;
        self.committed = true;
        return c_abi.k_ra8_ok;
    }

    pub fn abort(ctx: ?*anyopaque) callconv(.c) void {
        const self: *MemTx = @ptrCast(@alignCast(ctx.?));
        self.stage_len = 0;
        self.aborted = true;
    }

    pub const ops_vtable = c_abi.TransactionOps{
        .append = append,
        .commit = commit,
        .abort = abort,
    };

    pub fn asTx(self: *MemTx) c_abi.Transaction {
        return .{ .ops = &ops_vtable, .ctx = self };
    }
};

pub fn defaultStubDeps() c_abi.Deps {
    const H = struct {
        fn workBytes(w: u16, h: u16, tw: u16, th: u16) callconv(.c) u32 {
            _ = tw;
            return @as(u32, w) * @as(u32, h) + @as(u32, th) * 4;
        }
        fn webpWorkBytes(w: u16, h: u16, src_len: u32) callconv(.c) u32 {
            return @as(u32, w) * @as(u32, h) * 4 + src_len;
        }
        fn produce(cfg: *const c_abi.JofProduceCfg, info: *c_abi.JofInfo) callconv(.c) c_abi.Ra8Err {
            _ = cfg;
            info.* = std.mem.zeroes(c_abi.JofInfo);
            return c_abi.k_ra8_ok;
        }
        fn storedBound(raw: u32) callconv(.c) u32 {
            return raw + raw / 8 + 256;
        }
        fn parse(pread: ?c_abi.PreadFn, ctx: ?*anyopaque, sz: u64, info: *c_abi.JofInfo) callconv(.c) c_abi.Ra8Err {
            _ = pread;
            _ = ctx;
            _ = sz;
            _ = info;
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
            _ = ctx;
            _ = info;
            _ = tx;
            _ = ty;
            _ = scratch;
            _ = sc;
            _ = px;
            _ = oc;
            ow.* = 0;
            oh.* = 0;
            return c_abi.k_ra8_ok;
        }
        fn audit(
            pread: ?c_abi.PreadFn,
            ctx: ?*anyopaque,
            sz: u64,
            ws: *c_abi.JofAuditWorkspace,
            out: *c_abi.JofAuditResult,
        ) callconv(.c) c_abi.Ra8Err {
            _ = pread;
            _ = ctx;
            _ = sz;
            _ = ws;
            out.* = std.mem.zeroes(c_abi.JofAuditResult);
            return c_abi.k_ra8_ok;
        }
        fn bookOpen(
            rd: *c_abi.BookChunked,
            fr: ?c_abi.VsourceReadFn,
            fc: ?*anyopaque,
            fl: u64,
            inf: ?c_abi.BookInflateFn,
            tbl: ?[*]u64,
            tc: u32,
            st: ?[*]u8,
            sc: u32,
        ) callconv(.c) c_abi.Ra8Err {
            _ = fr;
            _ = fc;
            _ = fl;
            _ = inf;
            _ = tc;
            _ = st;
            _ = sc;
            rd.* = std.mem.zeroes(c_abi.BookChunked);
            rd.table = tbl;
            return c_abi.k_ra8_ok;
        }
        fn bookValidate(
            rd: *c_abi.BookChunked,
            c: ?[*]u8,
            cc: u32,
            s: ?[*]u8,
            sc: u32,
            hdr: *c_abi.BookHeader,
        ) callconv(.c) c_abi.Ra8Err {
            _ = rd;
            _ = c;
            _ = cc;
            _ = s;
            _ = sc;
            hdr.* = std.mem.zeroes(c_abi.BookHeader);
            return c_abi.k_ra8_ok;
        }
        fn sniff(bytes: [*]const u8, count: u32, out: *u32) callconv(.c) c_abi.Ra8Err {
            if (count >= 2 and bytes[0] == 0xFF and bytes[1] == 0xD8) {
                out.* = c_abi.k_imgdec_format_jpeg;
                return c_abi.k_ra8_ok;
            }
            if (count >= 8 and std.mem.eql(u8, bytes[0..8], "\x89PNG\r\n\x1a\n")) {
                out.* = c_abi.k_imgdec_format_png;
                return c_abi.k_ra8_ok;
            }
            if (count >= 12 and std.mem.eql(u8, bytes[0..4], "RIFF") and std.mem.eql(u8, bytes[8..12], "WEBP")) {
                out.* = c_abi.k_imgdec_format_webp;
                return c_abi.k_ra8_ok;
            }
            out.* = c_abi.k_imgdec_format_none;
            return c_abi.k_ra8_err_not_supported;
        }
        fn webpInfo(data: [*]const u8, size: usize, ow: *u32, oh: *u32) callconv(.c) c_abi.Ra8Err {
            _ = data;
            _ = size;
            ow.* = 64;
            oh.* = 32;
            return c_abi.k_ra8_ok;
        }
        fn tinfl(
            out_buf: ?*anyopaque,
            out_len: usize,
            src: ?*const anyopaque,
            src_len: usize,
            flags: c_int,
        ) callconv(.c) usize {
            _ = out_buf;
            _ = out_len;
            _ = src;
            _ = src_len;
            _ = flags;
            return 0;
        }
    };
    return .{
        .jof_work_bytes = H.workBytes,
        .jof_webp_work_bytes = H.webpWorkBytes,
        .jof_produce = H.produce,
        .jof_stored_bound = H.storedBound,
        .jof_parse = H.parse,
        .jof_read_tile = H.readTile,
        .jof_audit = H.audit,
        .book_chunked_open = H.bookOpen,
        .book_chunked_validate_strict = H.bookValidate,
        .ra8_imgdec_sniff = H.sniff,
        .ra8_webp_get_info = H.webpInfo,
        .tinfl_decompress_mem_to_mem = H.tinfl,
    };
}
