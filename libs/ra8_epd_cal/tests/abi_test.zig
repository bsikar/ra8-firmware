//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the `ra8_epd_cal` C ABI membrane: the resolution ladder, the
//! apply readback, provisioning, and the refusals the C callers depend on.
//! The seams are fakes, which is the whole point of the injected-seam design.

const std = @import("std");
const abi = @import("abi");

/// Diagnostic sink for the test binary. Production links the real logger.
export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    _ = message;
}

/// Diagnostic sink for the test binary. Production links the real logger.
export fn ra8_log_emit_warn(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    _ = message;
}

const ok: abi.RawErr = 0;
const hw_error: abi.RawErr = 0x204;

const limits: abi.Limits = .{ .min_mv = 200, .max_mv = 4000 };

/// Fake controller: holds a VCOM, can be made to fail either direction.
const FakePanel = struct {
    mv: u16 = 0,
    get_err: abi.RawErr = ok,
    set_err: abi.RawErr = ok,
    /// Report a different value than was written, as a lying panel would.
    readback_override: ?u16 = null,
    set_calls: u32 = 0,

    fn get(ctx: ?*anyopaque, out_mv: *u16) callconv(.c) abi.RawErr {
        const self: *FakePanel = @ptrCast(@alignCast(ctx.?));
        if (self.get_err != ok) return self.get_err;
        out_mv.* = self.readback_override orelse self.mv;
        return ok;
    }

    fn set(ctx: ?*anyopaque, mv: u16) callconv(.c) abi.RawErr {
        const self: *FakePanel = @ptrCast(@alignCast(ctx.?));
        self.set_calls += 1;
        if (self.set_err != ok) return self.set_err;
        self.mv = mv;
        return ok;
    }
};

/// Fake non-volatile store backed by a RAM blob.
const FakeStore = struct {
    blob: [abi.blob_size]u8 = @splat(0xFF),
    read_err: abi.RawErr = ok,
    write_err: abi.RawErr = ok,
    writes: u32 = 0,

    fn read(ctx: ?*anyopaque, dst: [*]u8, len: usize) callconv(.c) abi.RawErr {
        const self: *FakeStore = @ptrCast(@alignCast(ctx.?));
        if (self.read_err != ok) return self.read_err;
        @memcpy(dst[0..len], self.blob[0..len]);
        return ok;
    }

    fn write(ctx: ?*anyopaque, src: [*]const u8, len: usize) callconv(.c) abi.RawErr {
        const self: *FakeStore = @ptrCast(@alignCast(ctx.?));
        self.writes += 1;
        if (self.write_err != ok) return self.write_err;
        @memcpy(self.blob[0..len], src[0..len]);
        return ok;
    }

    /// Seed the blob with a valid record for `mv`.
    fn provisionDirect(self: *FakeStore, mv: u16) void {
        const record: abi.Record = .{ .vcom_mv = mv, .schema_version = abi.schema_version };
        const rc = abi.ra8_epd_cal_serialize(&record, &self.blob, self.blob.len);
        std.debug.assert(rc == ok);
    }
};

fn configWith(panel: *FakePanel, store: *FakeStore) abi.Config {
    return .{
        .limits = limits,
        .panel = .{ .get = FakePanel.get, .set = FakePanel.set, .ctx = panel },
        .store = .{ .read = FakeStore.read, .write = FakeStore.write, .ctx = store },
    };
}

test "vcom_in_range refuses a null window" {
    try std.testing.expect(!abi.ra8_epd_cal_vcom_in_range(1530, null));
    try std.testing.expect(abi.ra8_epd_cal_vcom_in_range(1530, &limits));
    try std.testing.expect(!abi.ra8_epd_cal_vcom_in_range(4001, &limits));
}

test "serialize and deserialize refuse null pointers and short buffers" {
    var blob: [abi.blob_size]u8 = @splat(0);
    var record: abi.Record = .{ .vcom_mv = 1530, .schema_version = 1 };

    try std.testing.expectEqual(@as(abi.RawErr, 0x504), abi.ra8_epd_cal_serialize(null, &blob, blob.len));
    try std.testing.expectEqual(@as(abi.RawErr, 0x504), abi.ra8_epd_cal_serialize(&record, null, blob.len));
    try std.testing.expectEqual(@as(abi.RawErr, 0x105), abi.ra8_epd_cal_serialize(&record, &blob, blob.len - 1));

    try std.testing.expectEqual(ok, abi.ra8_epd_cal_serialize(&record, &blob, blob.len));
    var out: abi.Record = .{};
    try std.testing.expectEqual(@as(abi.RawErr, 0x504), abi.ra8_epd_cal_deserialize(null, blob.len, &out));
    try std.testing.expectEqual(@as(abi.RawErr, 0x504), abi.ra8_epd_cal_deserialize(&blob, blob.len, null));
    try std.testing.expectEqual(@as(abi.RawErr, 0x105), abi.ra8_epd_cal_deserialize(&blob, blob.len - 1, &out));
    try std.testing.expectEqual(ok, abi.ra8_epd_cal_deserialize(&blob, blob.len, &out));
    try std.testing.expectEqual(@as(u16, 1530), out.vcom_mv);
}

test "serialize refuses a zero VCOM" {
    var blob: [abi.blob_size]u8 = @splat(0);
    const record: abi.Record = .{ .vcom_mv = 0, .schema_version = 1 };
    try std.testing.expectEqual(@as(abi.RawErr, 0x103), abi.ra8_epd_cal_serialize(&record, &blob, blob.len));
}

test "resolve refuses null arguments and unusable limits" {
    var panel: FakePanel = .{ .mv = 1530 };
    var store: FakeStore = .{};
    var cfg = configWith(&panel, &store);
    var result: abi.Result = .{};

    try std.testing.expectEqual(@as(abi.RawErr, 0x504), abi.ra8_epd_cal_resolve(null, &result));
    try std.testing.expectEqual(@as(abi.RawErr, 0x504), abi.ra8_epd_cal_resolve(&cfg, null));

    cfg.limits = .{ .min_mv = 0, .max_mv = 4000 };
    try std.testing.expectEqual(@as(abi.RawErr, 0x103), abi.ra8_epd_cal_resolve(&cfg, &result));
    try std.testing.expectEqual(abi.Source.none, result.source);

    cfg.limits = .{ .min_mv = 4000, .max_mv = 200 };
    try std.testing.expectEqual(@as(abi.RawErr, 0x103), abi.ra8_epd_cal_resolve(&cfg, &result));
}

test "resolve prefers the controller over a provisioned record" {
    var panel: FakePanel = .{ .mv = 1530 };
    var store: FakeStore = .{};
    store.provisionDirect(2000);
    const cfg = configWith(&panel, &store);

    var result: abi.Result = .{};
    try std.testing.expectEqual(ok, abi.ra8_epd_cal_resolve(&cfg, &result));
    try std.testing.expectEqual(abi.Source.panel, result.source);
    try std.testing.expectEqual(@as(u16, 1530), result.vcom_mv);
}

test "resolve falls through to the record when the controller declines" {
    var panel: FakePanel = .{ .mv = 0 };
    var store: FakeStore = .{};
    store.provisionDirect(2000);
    var cfg = configWith(&panel, &store);

    // Controller reports an out-of-range 0: a board that has not finished
    // booting. The record wins.
    var result: abi.Result = .{};
    try std.testing.expectEqual(ok, abi.ra8_epd_cal_resolve(&cfg, &result));
    try std.testing.expectEqual(abi.Source.record, result.source);
    try std.testing.expectEqual(@as(u16, 2000), result.vcom_mv);

    // Same when the controller read faults outright.
    panel.mv = 1530;
    panel.get_err = hw_error;
    try std.testing.expectEqual(ok, abi.ra8_epd_cal_resolve(&cfg, &result));
    try std.testing.expectEqual(abi.Source.record, result.source);

    // Same when there is no read seam at all.
    cfg.panel.get = null;
    try std.testing.expectEqual(ok, abi.ra8_epd_cal_resolve(&cfg, &result));
    try std.testing.expectEqual(abi.Source.record, result.source);
}

test "resolve falls through to the operator value, then refuses" {
    var panel: FakePanel = .{ .mv = 0 };
    var store: FakeStore = .{};
    var cfg = configWith(&panel, &store);

    // Blank storage, no operator value: nothing trusted.
    var result: abi.Result = .{};
    try std.testing.expectEqual(@as(abi.RawErr, 0x106), abi.ra8_epd_cal_resolve(&cfg, &result));
    try std.testing.expectEqual(abi.Source.none, result.source);
    try std.testing.expectEqual(@as(u16, 0), result.vcom_mv);

    // An out-of-range operator value is not a value.
    cfg.has_provisioned = true;
    cfg.provisioned_mv = 4001;
    try std.testing.expectEqual(@as(abi.RawErr, 0x106), abi.ra8_epd_cal_resolve(&cfg, &result));

    cfg.provisioned_mv = 1800;
    try std.testing.expectEqual(ok, abi.ra8_epd_cal_resolve(&cfg, &result));
    try std.testing.expectEqual(abi.Source.provisioned, result.source);
    try std.testing.expectEqual(@as(u16, 1800), result.vcom_mv);
}

test "resolve ignores a corrupt or out-of-range record" {
    var panel: FakePanel = .{ .mv = 0 };
    var store: FakeStore = .{};
    var cfg = configWith(&panel, &store);
    cfg.has_provisioned = true;
    cfg.provisioned_mv = 1800;

    // CRC damage: the record is present but cannot be trusted.
    store.provisionDirect(2000);
    store.blob[8] ^= 0x01;
    var result: abi.Result = .{};
    try std.testing.expectEqual(ok, abi.ra8_epd_cal_resolve(&cfg, &result));
    try std.testing.expectEqual(abi.Source.provisioned, result.source);

    // A read fault falls through the same way.
    store.provisionDirect(2000);
    store.read_err = hw_error;
    try std.testing.expectEqual(ok, abi.ra8_epd_cal_resolve(&cfg, &result));
    try std.testing.expectEqual(abi.Source.provisioned, result.source);

    // So does a record whose VCOM sits outside this panel's window.
    store.read_err = ok;
    store.provisionDirect(4000);
    cfg.limits = .{ .min_mv = 200, .max_mv = 2500 };
    try std.testing.expectEqual(ok, abi.ra8_epd_cal_resolve(&cfg, &result));
    try std.testing.expectEqual(abi.Source.provisioned, result.source);
}

test "apply drives the panel and confirms the readback" {
    var panel: FakePanel = .{ .mv = 0 };
    var store: FakeStore = .{};
    const cfg = configWith(&panel, &store);

    const result: abi.Result = .{ .vcom_mv = 1530, .source = .provisioned };
    try std.testing.expectEqual(ok, abi.ra8_epd_cal_apply(&cfg, &result));
    try std.testing.expectEqual(@as(u16, 1530), panel.mv);
    try std.testing.expectEqual(@as(u32, 1), panel.set_calls);
}

test "apply refuses an unresolved result, null arguments and an out-of-range value" {
    var panel: FakePanel = .{};
    var store: FakeStore = .{};
    const cfg = configWith(&panel, &store);

    const unresolved: abi.Result = .{ .vcom_mv = 0, .source = .none };
    try std.testing.expectEqual(@as(abi.RawErr, 0x104), abi.ra8_epd_cal_apply(&cfg, &unresolved));

    const good: abi.Result = .{ .vcom_mv = 1530, .source = .record };
    try std.testing.expectEqual(@as(abi.RawErr, 0x504), abi.ra8_epd_cal_apply(null, &good));
    try std.testing.expectEqual(@as(abi.RawErr, 0x504), abi.ra8_epd_cal_apply(&cfg, null));

    const too_high: abi.Result = .{ .vcom_mv = 4001, .source = .record };
    try std.testing.expectEqual(@as(abi.RawErr, 0x503), abi.ra8_epd_cal_apply(&cfg, &too_high));
    try std.testing.expectEqual(@as(u32, 0), panel.set_calls);
}

test "apply propagates a write failure and refuses an unconfirmable panel" {
    var panel: FakePanel = .{ .set_err = hw_error };
    var store: FakeStore = .{};
    var cfg = configWith(&panel, &store);
    const result: abi.Result = .{ .vcom_mv = 1530, .source = .record };

    try std.testing.expectEqual(hw_error, abi.ra8_epd_cal_apply(&cfg, &result));

    // No write seam at all.
    cfg.panel.set = null;
    try std.testing.expectEqual(@as(abi.RawErr, 0x107), abi.ra8_epd_cal_apply(&cfg, &result));

    // A panel that cannot be read back is a panel whose bias cannot be
    // confirmed, so the value is not left on it.
    panel.set_err = ok;
    cfg.panel.set = FakePanel.set;
    cfg.panel.get = null;
    try std.testing.expectEqual(@as(abi.RawErr, 0x107), abi.ra8_epd_cal_apply(&cfg, &result));

    cfg.panel.get = FakePanel.get;
    panel.get_err = hw_error;
    try std.testing.expectEqual(@as(abi.RawErr, 0x204), abi.ra8_epd_cal_apply(&cfg, &result));
}

test "apply refuses a panel that reports back a different bias" {
    var panel: FakePanel = .{ .readback_override = 1000 };
    var store: FakeStore = .{};
    const cfg = configWith(&panel, &store);
    const result: abi.Result = .{ .vcom_mv = 1530, .source = .record };

    try std.testing.expectEqual(@as(abi.RawErr, 0x501), abi.ra8_epd_cal_apply(&cfg, &result));
}

test "provision writes a record the resolver then accepts" {
    var panel: FakePanel = .{ .mv = 0 };
    var store: FakeStore = .{};
    var cfg = configWith(&panel, &store);

    try std.testing.expectEqual(ok, abi.ra8_epd_cal_provision(&cfg, 1530));
    try std.testing.expectEqual(@as(u32, 1), store.writes);

    var result: abi.Result = .{};
    try std.testing.expectEqual(ok, abi.ra8_epd_cal_resolve(&cfg, &result));
    try std.testing.expectEqual(abi.Source.record, result.source);
    try std.testing.expectEqual(@as(u16, 1530), result.vcom_mv);

    // Null config, no write seam, out-of-range value: all refused, nothing written.
    try std.testing.expectEqual(@as(abi.RawErr, 0x504), abi.ra8_epd_cal_provision(null, 1530));
    try std.testing.expectEqual(@as(abi.RawErr, 0x503), abi.ra8_epd_cal_provision(&cfg, 4001));
    cfg.store.write = null;
    try std.testing.expectEqual(@as(abi.RawErr, 0x107), abi.ra8_epd_cal_provision(&cfg, 1530));
    try std.testing.expectEqual(@as(u32, 1), store.writes);
}

test "provision propagates a store write failure" {
    var panel: FakePanel = .{};
    var store: FakeStore = .{ .write_err = hw_error };
    const cfg = configWith(&panel, &store);

    try std.testing.expectEqual(hw_error, abi.ra8_epd_cal_provision(&cfg, 1530));
}
