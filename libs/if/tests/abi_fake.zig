//! The programmable fake backend behind the fw_fs_* ABI tests: one Fake state
//! struct, the three vtables that read it, a full capability set, and the Rig
//! helper that hands out ports over it. Shared by the namespace and stream
//! tests in abi_test.zig and the transaction tests in abi_txn_test.zig; holds
//! no test blocks of its own.

const std = @import("std");
const abi = @import("abi");
const core = abi.core;

// ---------------------------------------------------------------------------
// One programmable fake backend behind all three vtables.
// ---------------------------------------------------------------------------

const Fake = struct {
    stat_result: core.Err = core.ok,
    stat_answer: core.Stat = .{},
    listdir_result: core.Err = core.ok,
    listdir_count: u32 = 0,
    listdir_complete: bool = true,
    dir_open_result: core.Err = core.ok,
    dir_next_result: core.Err = core.ok,
    dir_next_present: bool = true,
    dir_next_entry: core.DirentValue = .{},
    dir_close_result: core.Err = core.ok,
    name_result: core.Err = core.ok,
    space_result: core.Err = core.ok,
    space_answer: core.Space = .{},
    open_result: core.Err = core.ok,
    read_result: core.Err = core.ok,
    read_count: u32 = 0,
    write_result: core.Err = core.ok,
    write_count: u32 = 0,
    seek_result: core.Err = core.ok,
    tell_result: core.Err = core.ok,
    size_result: core.Err = core.ok,
    sync_result: core.Err = core.ok,
    close_result: core.Err = core.ok,
    begin_result: core.Err = core.ok,
    txn_write_result: core.Err = core.ok,
    txn_write_count: u32 = 0,
    txn_seek_result: core.Err = core.ok,
    validate_result: core.Err = core.ok,
    commit_result: core.Err = core.ok,
    commit_published: bool = true,
    abort_result: core.Err = core.ok,

    last_path: [core.path_cap]u8 = @splat(0),
    last_replace: bool = false,
    last_mode: u8 = 0xFF,
    last_policy: u8 = 0xFF,
    last_state: ?*anyopaque = null,
    calls: u32 = 0,

    fn of(ctx: ?*anyopaque) *Fake {
        return @ptrCast(@alignCast(ctx.?));
    }

    fn note(self: *Fake, path: [*:0]const u8) void {
        self.calls += 1;
        const span = std.mem.span(path);
        const copy = @min(span.len, self.last_path.len - 1);
        self.last_path = @splat(0);
        @memcpy(self.last_path[0..copy], span[0..copy]);
    }
};

fn fakeStat(ctx: ?*anyopaque, path: [*:0]const u8, out: *core.Stat) callconv(.c) core.Err {
    const self = Fake.of(ctx);
    self.note(path);
    out.* = self.stat_answer;
    return self.stat_result;
}

fn fakeListdir(
    ctx: ?*anyopaque,
    path: [*:0]const u8,
    max_entries: u32,
    callback: core.ListFn,
    callback_ctx: ?*anyopaque,
    out_count: *u32,
    out_complete: *bool,
) callconv(.c) core.Err {
    _ = max_entries;
    _ = callback;
    _ = callback_ctx;
    const self = Fake.of(ctx);
    self.note(path);
    out_count.* = self.listdir_count;
    out_complete.* = self.listdir_complete;
    return self.listdir_result;
}

fn fakeDirOpen(
    ctx: ?*anyopaque,
    path: [*:0]const u8,
    state: ?*anyopaque,
    state_bytes: u32,
) callconv(.c) core.Err {
    _ = state_bytes;
    const self = Fake.of(ctx);
    self.note(path);
    self.last_state = state;
    return self.dir_open_result;
}

fn fakeDirNext(
    ctx: ?*anyopaque,
    state: ?*anyopaque,
    out: *core.DirentValue,
    out_entry: *bool,
) callconv(.c) core.Err {
    const self = Fake.of(ctx);
    self.calls += 1;
    self.last_state = state;
    out.* = self.dir_next_entry;
    out_entry.* = self.dir_next_present;
    return self.dir_next_result;
}

fn fakeDirClose(ctx: ?*anyopaque, state: ?*anyopaque) callconv(.c) core.Err {
    const self = Fake.of(ctx);
    self.calls += 1;
    self.last_state = state;
    return self.dir_close_result;
}

fn fakeName(ctx: ?*anyopaque, path: [*:0]const u8) callconv(.c) core.Err {
    const self = Fake.of(ctx);
    self.note(path);
    return self.name_result;
}

fn fakeRename(
    ctx: ?*anyopaque,
    old_path: [*:0]const u8,
    new_path: [*:0]const u8,
    replace: bool,
) callconv(.c) core.Err {
    _ = old_path;
    const self = Fake.of(ctx);
    self.note(new_path);
    self.last_replace = replace;
    return self.name_result;
}

fn fakeSpace(ctx: ?*anyopaque, out: *core.Space) callconv(.c) core.Err {
    const self = Fake.of(ctx);
    self.calls += 1;
    out.* = self.space_answer;
    return self.space_result;
}

fn fakeOpen(
    ctx: ?*anyopaque,
    path: [*:0]const u8,
    mode: u8,
    state: ?*anyopaque,
    state_bytes: u32,
) callconv(.c) core.Err {
    _ = state_bytes;
    const self = Fake.of(ctx);
    self.note(path);
    self.last_mode = mode;
    self.last_state = state;
    return self.open_result;
}

fn fakeRead(
    ctx: ?*anyopaque,
    state: ?*anyopaque,
    dst: [*]u8,
    cap: u32,
    out_read: *u32,
) callconv(.c) core.Err {
    _ = dst;
    _ = cap;
    const self = Fake.of(ctx);
    self.calls += 1;
    self.last_state = state;
    out_read.* = self.read_count;
    return self.read_result;
}

fn fakeWrite(
    ctx: ?*anyopaque,
    state: ?*anyopaque,
    src: [*]const u8,
    len: u32,
    out_written: *u32,
) callconv(.c) core.Err {
    _ = src;
    _ = len;
    const self = Fake.of(ctx);
    self.calls += 1;
    self.last_state = state;
    out_written.* = self.write_count;
    return self.write_result;
}

fn fakeSeek(ctx: ?*anyopaque, state: ?*anyopaque, offset: u64) callconv(.c) core.Err {
    _ = state;
    _ = offset;
    const self = Fake.of(ctx);
    self.calls += 1;
    return self.seek_result;
}

fn fakeTell(ctx: ?*anyopaque, state: ?*anyopaque, out_offset: *u64) callconv(.c) core.Err {
    _ = state;
    const self = Fake.of(ctx);
    self.calls += 1;
    out_offset.* = 77;
    return self.tell_result;
}

fn fakeSize(ctx: ?*anyopaque, state: ?*anyopaque, out_size: *u64) callconv(.c) core.Err {
    _ = state;
    const self = Fake.of(ctx);
    self.calls += 1;
    out_size.* = 4096;
    return self.size_result;
}

fn fakeSync(ctx: ?*anyopaque, state: ?*anyopaque) callconv(.c) core.Err {
    _ = state;
    const self = Fake.of(ctx);
    self.calls += 1;
    return self.sync_result;
}

fn fakeClose(ctx: ?*anyopaque, state: ?*anyopaque) callconv(.c) core.Err {
    _ = state;
    const self = Fake.of(ctx);
    self.calls += 1;
    return self.close_result;
}

fn fakeBegin(
    ctx: ?*anyopaque,
    state: ?*anyopaque,
    state_bytes: u32,
    destination: [*:0]const u8,
    policy: u8,
) callconv(.c) core.Err {
    _ = state_bytes;
    const self = Fake.of(ctx);
    self.note(destination);
    self.last_state = state;
    self.last_policy = policy;
    return self.begin_result;
}

fn fakeTxnWrite(
    ctx: ?*anyopaque,
    state: ?*anyopaque,
    src: [*]const u8,
    len: u32,
    out_written: *u32,
) callconv(.c) core.Err {
    _ = state;
    _ = src;
    _ = len;
    const self = Fake.of(ctx);
    self.calls += 1;
    out_written.* = self.txn_write_count;
    return self.txn_write_result;
}

fn fakeTxnSeek(ctx: ?*anyopaque, state: ?*anyopaque, offset: u64) callconv(.c) core.Err {
    _ = state;
    _ = offset;
    const self = Fake.of(ctx);
    self.calls += 1;
    return self.txn_seek_result;
}

fn fakeValidate(
    ctx: ?*anyopaque,
    state: ?*anyopaque,
    validator: abi.ValidateFn,
    validator_ctx: ?*anyopaque,
) callconv(.c) core.Err {
    _ = state;
    _ = validator;
    _ = validator_ctx;
    const self = Fake.of(ctx);
    self.calls += 1;
    return self.validate_result;
}

fn fakeCommit(ctx: ?*anyopaque, state: ?*anyopaque, out_published: *bool) callconv(.c) core.Err {
    _ = state;
    const self = Fake.of(ctx);
    self.calls += 1;
    out_published.* = self.commit_published;
    return self.commit_result;
}

fn fakeAbort(ctx: ?*anyopaque, state: ?*anyopaque) callconv(.c) core.Err {
    _ = state;
    const self = Fake.of(ctx);
    self.calls += 1;
    return self.abort_result;
}

pub const namespace_table: abi.NamespaceIface = .{
    .stat = fakeStat,
    .listdir = fakeListdir,
    .dir_open = fakeDirOpen,
    .dir_next = fakeDirNext,
    .dir_close = fakeDirClose,
    .mkdir = fakeName,
    .unlink = fakeName,
    .rmdir = fakeName,
    .rename = fakeRename,
    .space = fakeSpace,
};

pub const stream_table: abi.StreamIface = .{
    .open = fakeOpen,
    .read = fakeRead,
    .write = fakeWrite,
    .seek = fakeSeek,
    .tell = fakeTell,
    .size = fakeSize,
    .sync = fakeSync,
    .close = fakeClose,
};

pub const transaction_table: abi.TransactionIface = .{
    .begin = fakeBegin,
    .write = fakeTxnWrite,
    .seek = fakeTxnSeek,
    .validate = fakeValidate,
    .commit = fakeCommit,
    .abort = fakeAbort,
};

pub fn allCaps() core.Caps {
    return .{
        .max_file_bytes = 1 << 30,
        .flags = core.cap_namespace | core.cap_stream | core.cap_space_query |
            core.cap_atomic_replace | core.cap_atomic_noreplace | core.cap_create_exclusive |
            core.cap_file_sync | core.cap_transactions,
        .file_workspace_bytes = 8,
        .directory_workspace_bytes = 8,
        .transaction_workspace_bytes = 8,
        .path_max_bytes = 256,
        .name_max_bytes = 64,
        .max_open_files = 4,
        .max_open_directories = 2,
        .file_workspace_align = 8,
        .directory_workspace_align = 8,
        .transaction_workspace_align = 8,
    };
}

pub const Rig = struct {
    fake: Fake = .{},
    caps: core.Caps = allCaps(),
    workspace: [64]u8 align(8) = @splat(0),

    pub fn names(self: *Rig) abi.Namespace {
        return .{ .iface = &namespace_table, .ctx = self, .caps = self.caps };
    }

    pub fn streams(self: *Rig) abi.StreamPort {
        return .{ .iface = &stream_table, .ctx = self, .caps = self.caps };
    }

    pub fn transactions(self: *Rig) abi.TransactionPort {
        return .{ .iface = &transaction_table, .ctx = self, .caps = self.caps };
    }

    pub fn work(self: *Rig) ?*anyopaque {
        return @ptrCast(&self.workspace);
    }

    pub fn openFile(self: *Rig, file: *abi.File) !void {
        const port = self.streams();
        try std.testing.expectEqual(core.ok, abi.fw_fs_open(
            &port,
            "/a.txt",
            core.open_read,
            file,
            self.work(),
            self.workspace.len,
        ));
    }

    pub fn beginTransaction(self: *Rig, transaction: *abi.Transaction) !void {
        const port = self.transactions();
        try std.testing.expectEqual(core.ok, abi.fw_fs_transaction_begin(
            &port,
            "/out.bin",
            core.txn_create_new,
            transaction,
            self.work(),
            self.workspace.len,
        ));
    }
};

pub fn fakeListCallback(
    ctx: ?*anyopaque,
    entry: *const core.Dirent,
    out_continue: *bool,
) callconv(.c) core.Err {
    _ = ctx;
    _ = entry;
    out_continue.* = true;
    return core.ok;
}

pub fn fakeValidator(ctx: ?*anyopaque, staged: *abi.File) callconv(.c) core.Err {
    _ = ctx;
    _ = staged;
    return core.ok;
}
