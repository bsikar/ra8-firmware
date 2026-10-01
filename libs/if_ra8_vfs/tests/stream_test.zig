//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the `if_ra8_vfs` stream and transaction halves of the
//! adapter: file open and the read, write, seek, tell, size and close round
//! trip, then the staged-write transaction lifecycle from begin through
//! validate, commit and abort. Driven over the fake medium in `vfs_fake.zig`.

const std = @import("std");
const abi = @import("abi");
const core = abi.core;
const fake = @import("vfs_fake.zig");

const expect = std.testing.expect;
const expectEqual = std.testing.expectEqual;

const Fixture = fake.Fixture;
const addNode = fake.addNode;
const findNode = fake.findNode;
const mountedFat = fake.mountedFat;
const names = fake.names;
const pathEq = fake.pathEq;
const resetMedium = fake.resetMedium;
const streams = fake.streams;
const txns = fake.txns;

// ---------------------------------------------------------------------------
// streams
// ---------------------------------------------------------------------------

test "open: an undersized workspace is no_mem before any mode check" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var file: core.FileState = .{};
    try expectEqual(
        core.err_no_mem,
        streams().open.?(fake.g_bound_ctx, "/a", core.open_create_new, &file, 0),
    );
    try expectEqual(@as(u32, 0), fake.g_open_calls);
}

test "open: create-new has no native equivalent" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var file: core.FileState = .{};
    try expectEqual(core.err_not_supported, streams().open.?(
        fake.g_bound_ctx,
        "/a",
        core.open_create_new,
        &file,
        @sizeOf(core.FileState),
    ));
    try expectEqual(@as(u32, 0), fake.g_open_calls);
}

test "stream: write, seek, tell, size, read and close round trip" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var file: core.FileState = .{};
    const bytes = @sizeOf(core.FileState);
    try expectEqual(core.ok, streams().open.?(
        fake.g_bound_ctx,
        "/a.bin",
        core.open_write_truncate,
        &file,
        bytes,
    ));
    try expectEqual(core.fs_mode_write, fake.g_last_open_mode);
    var written: u32 = 0;
    try expectEqual(core.ok, streams().write.?(fake.g_bound_ctx, &file, "hello", 5, &written));
    try expectEqual(@as(u32, 5), written);
    var offset: u64 = 0;
    try expectEqual(core.ok, streams().tell.?(fake.g_bound_ctx, &file, &offset));
    try expectEqual(@as(u64, 5), offset);
    var size: u64 = 0;
    try expectEqual(core.ok, streams().size.?(fake.g_bound_ctx, &file, &size));
    try expectEqual(@as(u64, 5), size);
    try expectEqual(core.ok, streams().seek.?(fake.g_bound_ctx, &file, 1));
    var buf = [_]u8{0} ** 8;
    var got: u32 = 0;
    try expectEqual(core.ok, streams().read.?(fake.g_bound_ctx, &file, &buf, buf.len, &got));
    try expectEqual(@as(u32, 4), got);
    try expect(std.mem.eql(u8, buf[0..4], "ello"));
    try expectEqual(core.ok, streams().close.?(fake.g_bound_ctx, &file));
    try expectEqual(@as(?*anyopaque, null), file.native);
}

test "write: a failed native write publishes no byte count" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var file: core.FileState = .{};
    try expectEqual(core.ok, streams().open.?(
        fake.g_bound_ctx,
        "/a.bin",
        core.open_write_truncate,
        &file,
        @sizeOf(core.FileState),
    ));
    fake.g_write_rc = core.err_invalid_state;
    var written: u32 = 0xFFFF;
    try expectEqual(
        core.err_invalid_state,
        streams().write.?(fake.g_bound_ctx, &file, "hello", 5, &written),
    );
    try expectEqual(@as(u32, 0xFFFF), written);
}

test "close: the handle is consumed even when the native close fails" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var file: core.FileState = .{};
    try expectEqual(core.ok, streams().open.?(
        fake.g_bound_ctx,
        "/a.bin",
        core.open_write_truncate,
        &file,
        @sizeOf(core.FileState),
    ));
    fake.g_close_rc = core.err_invalid_state;
    try expectEqual(core.err_invalid_state, streams().close.?(fake.g_bound_ctx, &file));
    try expectEqual(@as(?*anyopaque, null), file.native);
}

test "stream: sync is deliberately absent" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    try expectEqual(@as(?*const fn (?*anyopaque, ?*anyopaque) callconv(.c) u16, null), streams().sync);
    try expectEqual(@as(u32, 0), fake.g_bound_caps.flags & core.cap_file_sync);
}

// ---------------------------------------------------------------------------
// transactions
// ---------------------------------------------------------------------------

const Validator = struct {
    calls: u32 = 0,
    rc: u16 = 0,
    read_bytes: u32 = 0,

    fn run(ctx: ?*anyopaque, staged: *abi.File) callconv(.c) u16 {
        const self: *Validator = @ptrCast(@alignCast(ctx.?));
        self.calls += 1;
        var buf = [_]u8{0} ** 32;
        var got: u32 = 0;
        const iface = staged.iface.?;
        const read_rc = iface.read.?(staged.ctx, staged.state, &buf, buf.len, &got);
        if (read_rc != core.ok) return read_rc;
        self.read_bytes = got;
        return self.rc;
    }
};

fn beginTransaction(txn: *core.TransactionState) u16 {
    return txns().begin.?(
        fake.g_bound_ctx,
        txn,
        @sizeOf(core.TransactionState),
        "/books/new.cbz",
        core.txn_create_new,
    );
}

test "begin: an undersized workspace is no_mem before the policy check" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.err_no_mem, txns().begin.?(
        fake.g_bound_ctx,
        &txn,
        8,
        "/a",
        core.txn_replace_atomic,
    ));
}

test "begin: only create-new is supported" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.err_not_supported, txns().begin.?(
        fake.g_bound_ctx,
        &txn,
        @sizeOf(core.TransactionState),
        "/a",
        core.txn_replace_atomic,
    ));
    try expectEqual(@as(u32, 0), fake.g_stat_calls);
}

test "begin: an existing destination is refused and never staged" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/books/new.cbz", false, "old");
    var txn: core.TransactionState = undefined;
    try expectEqual(core.err_exists, beginTransaction(&txn));
    try expectEqual(@as(u32, 0), fake.g_open_calls);
}

test "begin: a private sibling stage is opened beside the destination" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    try expect(txn.writer_open);
    try expect(txn.stage_exists);
    try expect(std.mem.eql(u8, txn.destination[0..14], "/books/new.cbz"));
    try expect(std.mem.eql(u8, txn.stage[0..19], "/books/TX000001.TMP"));
    try expectEqual(core.fs_mode_write, fake.g_last_open_mode);
}

test "begin: a colliding stage name is skipped by the bounded search" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/books/TX000001.TMP", false, "busy");
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    try expect(std.mem.eql(u8, txn.stage[0..19], "/books/TX000002.TMP"));
}

test "write and seek: a closed writer is invalid_state" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = std.mem.zeroes(core.TransactionState);
    var written: u32 = 0;
    try expectEqual(
        core.err_invalid_state,
        txns().write.?(fake.g_bound_ctx, &txn, "x", 1, &written),
    );
    try expectEqual(core.err_invalid_state, txns().seek.?(fake.g_bound_ctx, &txn, 0));
}

test "seek: an offset past the stage length is invalid_size" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    var written: u32 = 0;
    try expectEqual(core.ok, txns().write.?(fake.g_bound_ctx, &txn, "abcd", 4, &written));
    try expectEqual(core.err_invalid_size, txns().seek.?(fake.g_bound_ctx, &txn, 5));
    try expectEqual(core.ok, txns().seek.?(fake.g_bound_ctx, &txn, 4));
    try expectEqual(core.ok, txns().seek.?(fake.g_bound_ctx, &txn, 0));
}

test "validate: the writer is closed, the stage reopened read-only, then closed" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    var written: u32 = 0;
    try expectEqual(core.ok, txns().write.?(fake.g_bound_ctx, &txn, "payload", 7, &written));
    var validator = Validator{};
    try expectEqual(
        core.ok,
        txns().validate.?(fake.g_bound_ctx, &txn, &Validator.run, &validator),
    );
    try expectEqual(@as(u32, 1), validator.calls);
    try expectEqual(@as(u32, 7), validator.read_bytes);
    try expectEqual(core.fs_mode_read, fake.g_last_open_mode);
    try expect(!txn.writer_open);
    try expectEqual(@as(?*anyopaque, null), txn.file_state.native);
}

test "validate: a staged read failure is returned after the reader closes" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    var written: u32 = 0;
    try expectEqual(core.ok, txns().write.?(fake.g_bound_ctx, &txn, "payload", 7, &written));
    var validator = Validator{};
    fake.g_read_rc = core.err_invalid_state;
    try expectEqual(
        core.err_invalid_state,
        txns().validate.?(fake.g_bound_ctx, &txn, &Validator.run, &validator),
    );
    try expectEqual(@as(u32, 1), validator.calls);
    try expectEqual(@as(u32, 0), validator.read_bytes);
    try expectEqual(@as(u32, 2), fake.g_close_calls);
    try expect(!txn.writer_open);
    try expect(txn.stage_exists);
    try expectEqual(@as(?*anyopaque, null), txn.file_state.native);
}

test "validate: a validator refusal outranks the closing result" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    var validator = Validator{ .rc = core.err_invalid_arg };
    try expectEqual(
        core.err_invalid_arg,
        txns().validate.?(fake.g_bound_ctx, &txn, &Validator.run, &validator),
    );
    try expect(!txn.writer_open);
}

test "validate: an unopened writer is invalid_state" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = std.mem.zeroes(core.TransactionState);
    var validator = Validator{};
    try expectEqual(
        core.err_invalid_state,
        txns().validate.?(fake.g_bound_ctx, &txn, &Validator.run, &validator),
    );
    try expectEqual(@as(u32, 0), validator.calls);
}

test "commit: a validated stage is published by a no-replace rename" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    var written: u32 = 0;
    try expectEqual(core.ok, txns().write.?(fake.g_bound_ctx, &txn, "data", 4, &written));
    var validator = Validator{};
    try expectEqual(
        core.ok,
        txns().validate.?(fake.g_bound_ctx, &txn, &Validator.run, &validator),
    );
    var published: bool = false;
    try expectEqual(core.ok, txns().commit.?(fake.g_bound_ctx, &txn, &published));
    try expect(published);
    try expect(!txn.stage_exists);
    try expect(pathEq(&fake.g_last_path_b, "ram:/books/new.cbz"));
    try expect(findNode("ram:/books/new.cbz") != null);
}

test "commit: an open writer blocks publication" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    var published: bool = true;
    try expectEqual(
        core.err_invalid_state,
        txns().commit.?(fake.g_bound_ctx, &txn, &published),
    );
    try expectEqual(@as(u32, 0), fake.g_rename_calls);
}

test "commit: a failed rename leaves the transaction abortable" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    var validator = Validator{};
    try expectEqual(
        core.ok,
        txns().validate.?(fake.g_bound_ctx, &txn, &Validator.run, &validator),
    );
    fake.g_rename_rc = core.err_exists;
    var published: bool = false;
    try expectEqual(core.err_exists, txns().commit.?(fake.g_bound_ctx, &txn, &published));
    try expect(!published);
    try expect(txn.stage_exists);
}

test "abort: the writer is closed and the stage removed" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    try expectEqual(core.ok, txns().abort.?(fake.g_bound_ctx, &txn));
    try expect(!txn.writer_open);
    try expect(!txn.stage_exists);
    try expectEqual(@as(u32, 1), fake.g_unlink_calls);
    try expect(findNode("ram:/books/TX000001.TMP") == null);
}

test "abort: the first failure wins and only released resources are cleared" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    fake.g_close_rc = core.err_invalid_state;
    fake.g_unlink_rc = core.err_not_supported;
    try expectEqual(core.err_invalid_state, txns().abort.?(fake.g_bound_ctx, &txn));
    try expect(!txn.writer_open);
    try expect(txn.stage_exists);
}

test "abort: a transaction with nothing owned is a clean no-op" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = std.mem.zeroes(core.TransactionState);
    try expectEqual(core.ok, txns().abort.?(fake.g_bound_ctx, &txn));
    try expectEqual(@as(u32, 0), fake.g_unlink_calls);
    try expectEqual(@as(u32, 0), fake.g_close_calls);
}
