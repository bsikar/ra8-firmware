//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/ssie_stream.zig (RA8FW-605).

const std = @import("std");
const s = @import("ssie_stream");

const base: usize = 0x4000_0000;

const Fake = struct {
    rt: [2]s.Runtime = .{ .{}, .{} },
    /// Successive SSIFSR reads; the last value repeats.
    fsr: []const u32 = &.{0},
    fsr_i: usize = 0,
    rdr: u32 = 0x100,
    tdr: [8]u32 = undefined,
    tdr_n: usize = 0,
    fsr_writes: [4]u32 = undefined,
    fsr_n: usize = 0,
    starts: [4]struct { ch: u8, cfg: s.DmacConfig } = undefined,
    start_n: usize = 0,
    start_rc: [2]u16 = .{ 0, 0 },
    stops: [4]u8 = undefined,
    stop_n: usize = 0,
    errs: usize = 0,
    last_val: ?u32 = null,
    pub fn regs(_: *Fake, ch: u8) ?usize {
        return if (ch < 2) base + @as(usize, ch) * 0x100 else null;
    }
    pub fn read32(self: *Fake, addr: usize) u32 {
        if (addr % 0x100 == s.off_ssifrdr) {
            self.rdr += 1;
            return self.rdr;
        }
        const v = self.fsr[@min(self.fsr_i, self.fsr.len - 1)];
        self.fsr_i += 1;
        return v;
    }
    pub fn write32(self: *Fake, addr: usize, value: u32) void {
        if (addr % 0x100 == s.off_ssiftdr) {
            self.tdr[self.tdr_n] = value;
            self.tdr_n += 1;
        } else {
            self.fsr_writes[self.fsr_n] = value;
            self.fsr_n += 1;
        }
    }
    pub fn runtime(self: *Fake, ch: u8) *s.Runtime {
        return &self.rt[ch];
    }
    pub fn dmacStart(self: *Fake, ch: u8, cfg: *const s.DmacConfig) u16 {
        const rc = self.start_rc[self.start_n];
        self.starts[self.start_n] = .{ .ch = ch, .cfg = cfg.* };
        self.start_n += 1;
        return rc;
    }
    pub fn dmacStop(self: *Fake, ch: u8) u16 {
        self.stops[self.stop_n] = ch;
        self.stop_n += 1;
        return 0;
    }
    pub fn err(self: *Fake, _: [*:0]const u8) void {
        self.errs += 1;
    }
    pub fn errVal(self: *Fake, _: [*:0]const u8, value: u32) void {
        self.last_val = value;
    }
};

var tx_buf = [_]u32{ 1, 2, 3 };
var rx_buf = [_]u32{0} ** 4;

test "layouts match the C structs" {
    try std.testing.expectEqual(@as(usize, 4), @sizeOf(s.Runtime));
    try std.testing.expectEqual(@as(usize, 3), @offsetOf(s.Runtime, "dma_attached"));
    try std.testing.expectEqual(@as(usize, 20), @sizeOf(s.DmacConfig));
    try std.testing.expectEqual(@as(usize, 10), @offsetOf(s.DmacConfig, "width"));
    try std.testing.expectEqual(@as(usize, 12), @offsetOf(s.DmacConfig, "dst_inc"));
    try std.testing.expectEqual(@as(usize, 1), @offsetOf(s.DmaCfg, "rx_dma_channel"));
    try std.testing.expectEqual(@sizeOf(usize), @offsetOf(s.DmaCfg, "tx_buffer"));
    try std.testing.expectEqual(3 * @sizeOf(usize), @offsetOf(s.DmaCfg, "tx_samples"));
}

test "attach starts tx and rx DMA at the FIFO registers" {
    var f = Fake{};
    const dma = s.DmaCfg{ .tx_dma_channel = 2, .rx_dma_channel = 3, .tx_buffer = &tx_buf, .rx_buffer = &rx_buf, .tx_samples = 3, .rx_samples = 4 };
    try std.testing.expectEqual(s.ok, s.attachDma(&f, 1, &dma));
    try std.testing.expectEqual(@as(usize, 2), f.start_n);
    const tx = f.starts[0];
    try std.testing.expectEqual(@as(u8, 2), tx.ch);
    try std.testing.expectEqual(@as(u32, @truncate(base + 0x100 + 0x18)), tx.cfg.dst);
    try std.testing.expectEqual(@as(u32, @truncate(@intFromPtr(&tx_buf))), tx.cfg.src);
    try std.testing.expect(tx.cfg.src_inc and !tx.cfg.dst_inc);
    try std.testing.expectEqual(@as(u8, 2), tx.cfg.width);
    const rx = f.starts[1];
    try std.testing.expectEqual(@as(u32, @truncate(base + 0x100 + 0x1C)), rx.cfg.src);
    try std.testing.expect(rx.cfg.dst_inc and !rx.cfg.src_inc);
    try std.testing.expectEqual(@as(u16, 4), rx.cfg.count);
    try std.testing.expectEqual(s.Runtime{ .tx_dma_channel = 2, .rx_dma_channel = 3, .dma_attached = true }, f.rt[1]);
}

test "attach rejects null, bad channel and bad configs" {
    var f = Fake{};
    try std.testing.expectEqual(s.null_ptr, s.attachDma(&f, 0, null));
    try std.testing.expectEqual(s.invalid_arg, s.attachDma(&f, 2, &s.DmaCfg{}));
    try std.testing.expectEqual(@as(usize, 1), f.errs);
    try std.testing.expectEqual(s.invalid_arg, s.attachDma(&f, 0, &s.DmaCfg{}));
    try std.testing.expectEqual(s.invalid_arg, s.attachDma(&f, 0, &s.DmaCfg{ .tx_dma_channel = 0, .tx_samples = 1 }));
    try std.testing.expectEqual(s.invalid_arg, s.attachDma(&f, 0, &s.DmaCfg{ .rx_dma_channel = 0, .rx_buffer = &rx_buf }));
    try std.testing.expectEqual(@as(usize, 4), f.errs);
    try std.testing.expectEqual(@as(?u32, s.invalid_arg), f.last_val);
    try std.testing.expectEqual(@as(usize, 0), f.start_n);
}

test "rx start failure unwinds the started tx channel" {
    var f = Fake{ .start_rc = .{ 0, 0x109 } };
    const dma = s.DmaCfg{ .tx_dma_channel = 4, .rx_dma_channel = 5, .tx_buffer = &tx_buf, .rx_buffer = &rx_buf, .tx_samples = 1, .rx_samples = 1 };
    try std.testing.expectEqual(@as(u16, 0x109), s.attachDma(&f, 0, &dma));
    try std.testing.expectEqual(@as(usize, 1), f.stop_n);
    try std.testing.expectEqual(@as(u8, 4), f.stops[0]);
    try std.testing.expectEqual(s.Runtime{}, f.rt[0]);
    try std.testing.expectEqual(@as(usize, 1), f.errs);
}

test "tx start failure logs both levels and leaves rx untouched" {
    var f = Fake{ .start_rc = .{ 0x203, 0 } };
    const dma = s.DmaCfg{ .tx_dma_channel = 4, .rx_dma_channel = 5, .tx_buffer = &tx_buf, .rx_buffer = &rx_buf, .tx_samples = 1, .rx_samples = 1 };
    try std.testing.expectEqual(@as(u16, 0x203), s.attachDma(&f, 0, &dma));
    try std.testing.expectEqual(@as(usize, 1), f.start_n);
    try std.testing.expectEqual(@as(usize, 2), f.errs);
    try std.testing.expect(!f.rt[0].dma_attached);
}

test "detach stops live channels and clears the runtime" {
    var f = Fake{};
    f.rt[1] = .{ .tx_dma_channel = 6, .rx_dma_channel = s.dma_ch_unused, .initialized = true, .dma_attached = true };
    try std.testing.expectEqual(s.ok, s.detachDma(&f, 1));
    try std.testing.expectEqual(@as(usize, 1), f.stop_n);
    try std.testing.expectEqual(s.Runtime{ .initialized = true }, f.rt[1]);
    try std.testing.expectEqual(s.invalid_arg, s.detachDma(&f, 2));
}

test "attach pair records channels and maps out-of-range to unused" {
    var f = Fake{};
    try std.testing.expectEqual(s.ok, s.attachDmaPair(&f, 0, 1, 9));
    try std.testing.expectEqual(s.Runtime{ .tx_dma_channel = 1, .dma_attached = true }, f.rt[0]);
    try std.testing.expectEqual(s.invalid_arg, s.attachDmaPair(&f, 0, 8, 8));
    try std.testing.expectEqual(s.invalid_arg, s.attachDmaPair(&f, 2, 0, 0));
    try std.testing.expectEqual(@as(usize, 0), f.start_n);
}

test "send waits while TDC shows a full FIFO, then clears TDE keeping RDF" {
    var f = Fake{ .fsr = &.{ 32 << 24, 32 << 24, 31 << 24, 0 } };
    try std.testing.expectEqual(s.ok, s.sendIso(&f, 0, &tx_buf, 3));
    try std.testing.expectEqualSlices(u32, &tx_buf, f.tdr[0..f.tdr_n]);
    try std.testing.expectEqual(@as(usize, 5), f.fsr_i);
    try std.testing.expectEqualSlices(u32, &.{s.rdf_clear}, f.fsr_writes[0..f.fsr_n]);
}

test "send rejects null and bad channel" {
    var f = Fake{};
    try std.testing.expectEqual(s.null_ptr, s.sendIso(&f, 0, null, 1));
    try std.testing.expectEqual(s.invalid_arg, s.sendIso(&f, 3, &tx_buf, 1));
    try std.testing.expectEqual(@as(usize, 0), f.fsr_n);
}

test "recv drains while RDC is non-zero and clears TDE" {
    var f = Fake{ .fsr = &.{ 2 << 8, 1 << 8, 0 } };
    var got: u16 = 9;
    try std.testing.expectEqual(s.ok, s.recvIso(&f, 1, &rx_buf, 4, &got));
    try std.testing.expectEqual(@as(u16, 2), got);
    try std.testing.expectEqualSlices(u32, &.{ 0x101, 0x102 }, rx_buf[0..2]);
    try std.testing.expectEqualSlices(u32, &.{s.tde_clear}, f.fsr_writes[0..f.fsr_n]);
}

test "recv with an empty FIFO or max of zero writes nothing" {
    var f = Fake{};
    var got: u16 = 9;
    try std.testing.expectEqual(s.ok, s.recvIso(&f, 0, &rx_buf, 4, &got));
    try std.testing.expectEqual(@as(u16, 0), got);
    try std.testing.expectEqual(@as(usize, 0), f.fsr_n);
    var g = Fake{ .fsr = &.{1 << 8} };
    try std.testing.expectEqual(s.ok, s.recvIso(&g, 0, &rx_buf, 0, &got));
    try std.testing.expectEqual(@as(usize, 0), g.fsr_i);
}

test "recv rejects null buffer, null out and bad channel" {
    var f = Fake{};
    var got: u16 = 0;
    try std.testing.expectEqual(s.null_ptr, s.recvIso(&f, 0, null, 1, &got));
    try std.testing.expectEqual(s.null_ptr, s.recvIso(&f, 0, &rx_buf, 1, null));
    try std.testing.expectEqual(s.invalid_arg, s.recvIso(&f, 2, &rx_buf, 1, &got));
    try std.testing.expectEqual(@as(usize, 2), f.errs);
}
