//! ABI-membrane tests: every exported fw_fs_* symbol driven over a fake
//! backend, so guard order, handle mutation and backend-answer scrubbing are
//! all pinned host-side.

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

const namespace_table: abi.NamespaceIface = .{
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

const stream_table: abi.StreamIface = .{
    .open = fakeOpen,
    .read = fakeRead,
    .write = fakeWrite,
    .seek = fakeSeek,
    .tell = fakeTell,
    .size = fakeSize,
    .sync = fakeSync,
    .close = fakeClose,
};

const transaction_table: abi.TransactionIface = .{
    .begin = fakeBegin,
    .write = fakeTxnWrite,
    .seek = fakeTxnSeek,
    .validate = fakeValidate,
    .commit = fakeCommit,
    .abort = fakeAbort,
};

fn allCaps() core.Caps {
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

const Rig = struct {
    fake: Fake = .{},
    caps: core.Caps = allCaps(),
    workspace: [64]u8 align(8) = @splat(0),

    fn names(self: *Rig) abi.Namespace {
        return .{ .iface = &namespace_table, .ctx = self, .caps = self.caps };
    }

    fn streams(self: *Rig) abi.StreamPort {
        return .{ .iface = &stream_table, .ctx = self, .caps = self.caps };
    }

    fn transactions(self: *Rig) abi.TransactionPort {
        return .{ .iface = &transaction_table, .ctx = self, .caps = self.caps };
    }

    fn work(self: *Rig) ?*anyopaque {
        return @ptrCast(&self.workspace);
    }

    fn openFile(self: *Rig, file: *abi.File) !void {
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

    fn beginTransaction(self: *Rig, transaction: *abi.Transaction) !void {
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

// ---------------------------------------------------------------------------
// Binding.
// ---------------------------------------------------------------------------

test "bind rejects each missing argument before validating anything" {
    var rig = Rig{};
    var fs: abi.Fs = .{};
    const caps = rig.caps;
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_bind(
        null,
        &namespace_table,
        &stream_table,
        &transaction_table,
        &rig,
        &caps,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_bind(
        &fs,
        null,
        &stream_table,
        &transaction_table,
        &rig,
        &caps,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_bind(
        &fs,
        &namespace_table,
        null,
        &transaction_table,
        &rig,
        &caps,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_bind(
        &fs,
        &namespace_table,
        &stream_table,
        &transaction_table,
        null,
        &caps,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_bind(
        &fs,
        &namespace_table,
        &stream_table,
        &transaction_table,
        &rig,
        null,
    ));
}

test "a successful bind copies caps into all three facades" {
    var rig = Rig{};
    var fs: abi.Fs = .{};
    const caps = rig.caps;
    try std.testing.expectEqual(core.ok, abi.fw_fs_bind(
        &fs,
        &namespace_table,
        &stream_table,
        &transaction_table,
        &rig,
        &caps,
    ));
    try std.testing.expectEqual(&namespace_table, fs.names.iface.?);
    try std.testing.expectEqual(&stream_table, fs.streams.iface.?);
    try std.testing.expectEqual(&transaction_table, fs.transactions.iface.?);
    try std.testing.expectEqual(caps.flags, fs.names.caps.flags);
    try std.testing.expectEqual(caps.flags, fs.streams.caps.flags);
    try std.testing.expectEqual(caps.flags, fs.transactions.caps.flags);
    try std.testing.expectEqual(@as(?*anyopaque, &rig), fs.names.ctx);

    var read_back: core.Caps = .{};
    try std.testing.expectEqual(core.ok, abi.fw_fs_get_caps(&fs, &read_back));
    try std.testing.expectEqual(caps.path_max_bytes, read_back.path_max_bytes);
}

test "bind refuses an incomplete namespace table" {
    var rig = Rig{};
    var fs: abi.Fs = .{};
    var table = namespace_table;
    table.rmdir = null;
    const caps = rig.caps;
    try std.testing.expectEqual(core.err_invalid_arg, abi.fw_fs_bind(
        &fs,
        &table,
        &stream_table,
        &transaction_table,
        &rig,
        &caps,
    ));
}

test "bind refuses caps whose root path cannot validate" {
    var rig = Rig{};
    var fs: abi.Fs = .{};
    var caps = rig.caps;
    caps.path_max_bytes = 1;
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_bind(
        &fs,
        &namespace_table,
        &stream_table,
        &transaction_table,
        &rig,
        &caps,
    ));
}

test "get_caps needs a bound filesystem" {
    var fs: abi.Fs = .{};
    var out: core.Caps = .{};
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_get_caps(null, &out));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_get_caps(&fs, null));
    try std.testing.expectEqual(core.err_not_initialized, abi.fw_fs_get_caps(&fs, &out));
}

test "path validate rejects both NULLs" {
    const caps = allCaps();
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_path_validate(null, "/a"));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_path_validate(&caps, null));
    try std.testing.expectEqual(core.ok, abi.fw_fs_path_validate(&caps, "/a"));
}

// ---------------------------------------------------------------------------
// Namespace.
// ---------------------------------------------------------------------------

test "stat guard order is facade, out pointer, path" {
    var rig = Rig{};
    var names = rig.names();
    var out: core.Stat = .{};
    var detached: abi.Namespace = .{};
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_stat(null, "/a", &out));
    try std.testing.expectEqual(core.err_not_initialized, abi.fw_fs_stat(&detached, "/a", &out));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_stat(&names, "/a", null));
    try std.testing.expectEqual(core.err_access_denied, abi.fw_fs_stat(&names, "/../a", &out));
    try std.testing.expectEqual(@as(u32, 0), rig.fake.calls);
}

test "stat passes a coherent answer through and scrubs an incoherent one" {
    var rig = Rig{};
    var names = rig.names();
    var out: core.Stat = .{};
    rig.fake.stat_answer = .{ .exists = true, .node_type = core.node_file, .size_bytes = 9 };
    try std.testing.expectEqual(core.ok, abi.fw_fs_stat(&names, "/a", &out));
    try std.testing.expectEqual(@as(u64, 9), out.size_bytes);

    rig.fake.stat_answer = .{ .exists = true, .node_type = core.node_directory, .size_bytes = 3 };
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_stat(&names, "/a", &out));
    try std.testing.expectEqual(@as(u64, 0), out.size_bytes);
    try std.testing.expect(!out.exists);
}

test "a failing stat keeps the backend code and the zeroed answer" {
    var rig = Rig{};
    var names = rig.names();
    var out: core.Stat = .{ .size_bytes = 5 };
    rig.fake.stat_result = core.err_not_supported;
    rig.fake.stat_answer = .{ .exists = true, .node_type = 9 };
    try std.testing.expectEqual(core.err_not_supported, abi.fw_fs_stat(&names, "/a", &out));
}

test "listdir guards its three out parameters then the entry budget" {
    var rig = Rig{};
    var names = rig.names();
    var count: u32 = 0;
    var complete: u8 = 0;
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_listdir(
        &names,
        "/",
        4,
        null,
        null,
        &count,
        &complete,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_listdir(
        &names,
        "/",
        4,
        fakeListCallback,
        null,
        null,
        &complete,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_listdir(
        &names,
        "/",
        4,
        fakeListCallback,
        null,
        &count,
        null,
    ));
    try std.testing.expectEqual(core.err_invalid_arg, abi.fw_fs_listdir(
        &names,
        "/",
        0,
        fakeListCallback,
        null,
        &count,
        &complete,
    ));
}

fn fakeListCallback(
    ctx: ?*anyopaque,
    entry: *const core.Dirent,
    out_continue: *bool,
) callconv(.c) core.Err {
    _ = ctx;
    _ = entry;
    out_continue.* = true;
    return core.ok;
}

test "listdir refuses a backend that overran the budget" {
    var rig = Rig{};
    var names = rig.names();
    var count: u32 = 0;
    var complete: u8 = 0;
    rig.fake.listdir_count = 5;
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_listdir(
        &names,
        "/",
        4,
        fakeListCallback,
        null,
        &count,
        &complete,
    ));
    try std.testing.expectEqual(@as(u32, 0), count);
    try std.testing.expectEqual(@as(u8, 0), complete);

    rig.fake.listdir_count = 4;
    try std.testing.expectEqual(core.ok, abi.fw_fs_listdir(
        &names,
        "/",
        4,
        fakeListCallback,
        null,
        &count,
        &complete,
    ));
    try std.testing.expectEqual(@as(u32, 4), count);
}

test "mkdir, unlink and rmdir refuse the root and reach the backend" {
    var rig = Rig{};
    var names = rig.names();
    try std.testing.expectEqual(core.err_access_denied, abi.fw_fs_mkdir(&names, "/"));
    try std.testing.expectEqual(core.err_access_denied, abi.fw_fs_unlink(&names, "/"));
    try std.testing.expectEqual(core.err_access_denied, abi.fw_fs_rmdir(&names, "/"));
    try std.testing.expectEqual(core.ok, abi.fw_fs_mkdir(&names, "/dir"));
    try std.testing.expectEqualStrings("/dir", std.mem.sliceTo(&rig.fake.last_path, 0));
    rig.fake.name_result = core.err_busy;
    try std.testing.expectEqual(core.err_busy, abi.fw_fs_unlink(&names, "/dir/f"));
}

test "a namespace operation the backend omits answers not_supported" {
    var rig = Rig{};
    var table = namespace_table;
    table.mkdir = null;
    var names = abi.Namespace{ .iface = &table, .ctx = &rig, .caps = rig.caps };
    try std.testing.expectEqual(core.err_not_supported, abi.fw_fs_mkdir(&names, "/dir"));
}

test "rename checks capability first, then both paths, then the root" {
    var rig = Rig{};
    rig.caps.flags &= ~core.cap_atomic_replace;
    var names = rig.names();
    try std.testing.expectEqual(
        core.err_not_supported,
        abi.fw_fs_rename(&names, "/a", "/b", 1),
    );
    try std.testing.expectEqual(core.ok, abi.fw_fs_rename(&names, "/a", "/b", 0));
    try std.testing.expect(!rig.fake.last_replace);
    try std.testing.expectEqual(
        core.err_access_denied,
        abi.fw_fs_rename(&names, "/", "/b", 0),
    );
    try std.testing.expectEqual(
        core.err_access_denied,
        abi.fw_fs_rename(&names, "/a", "/", 0),
    );
    try std.testing.expectEqual(
        core.err_invalid_arg,
        abi.fw_fs_rename(&names, "/a", "b", 0),
    );
}

test "space needs the capability, the operation and a coherent answer" {
    var rig = Rig{};
    rig.caps.flags &= ~core.cap_space_query;
    var without = rig.names();
    var out: core.Space = .{};
    try std.testing.expectEqual(core.err_not_supported, abi.fw_fs_space(&without, &out));

    rig.caps = allCaps();
    var table = namespace_table;
    table.space = null;
    var missing = abi.Namespace{ .iface = &table, .ctx = &rig, .caps = rig.caps };
    try std.testing.expectEqual(core.err_not_supported, abi.fw_fs_space(&missing, &out));

    var names = rig.names();
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_space(&names, null));

    rig.fake.space_answer = .{ .total_bytes = 100, .free_bytes = 200 };
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_space(&names, &out));
    try std.testing.expectEqual(@as(u64, 0), out.total_bytes);

    rig.fake.space_answer = .{ .total_bytes = 100, .free_bytes = 40, .used_bytes = 60 };
    try std.testing.expectEqual(core.ok, abi.fw_fs_space(&names, &out));
    try std.testing.expectEqual(@as(u64, 60), out.used_bytes);
}

// ---------------------------------------------------------------------------
// Directory cursors.
// ---------------------------------------------------------------------------

test "dir_open guards the handle, the path and the workspace" {
    var rig = Rig{};
    var names = rig.names();
    var dir: abi.Dir = .{};
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_dir_open(
        &names,
        "/",
        null,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_invalid_arg, abi.fw_fs_dir_open(
        &names,
        "rel",
        &dir,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_dir_open(
        &names,
        "/",
        &dir,
        null,
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_no_mem, abi.fw_fs_dir_open(
        &names,
        "/",
        &dir,
        rig.work(),
        4,
    ));
    try std.testing.expectEqual(core.ok, abi.fw_fs_dir_open(
        &names,
        "/",
        &dir,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expect(dir.is_open);
    try std.testing.expectEqual(core.err_busy, abi.fw_fs_dir_open(
        &names,
        "/",
        &dir,
        rig.work(),
        rig.workspace.len,
    ));
}

test "a backend that refuses dir_open leaves the handle closed" {
    var rig = Rig{};
    var names = rig.names();
    var dir: abi.Dir = .{};
    rig.fake.dir_open_result = core.err_invalid_size;
    try std.testing.expectEqual(core.err_invalid_size, abi.fw_fs_dir_open(
        &names,
        "/",
        &dir,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expect(!dir.is_open);
    try std.testing.expectEqual(@as(?*const abi.NamespaceIface, null), dir.iface);
}

test "dir_next publishes only a coherent entry" {
    var rig = Rig{};
    var names = rig.names();
    var dir: abi.Dir = .{};
    try std.testing.expectEqual(core.ok, abi.fw_fs_dir_open(
        &names,
        "/",
        &dir,
        rig.work(),
        rig.workspace.len,
    ));

    var out: core.DirentValue = .{};
    var present: u8 = 0;
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_dir_next(&dir, null, &present));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_dir_next(&dir, &out, null));

    var entry: core.DirentValue = .{ .node_type = core.node_file, .size_bytes = 3 };
    @memcpy(entry.name[0..4], "note");
    entry.name_bytes = 4;
    rig.fake.dir_next_entry = entry;
    try std.testing.expectEqual(core.ok, abi.fw_fs_dir_next(&dir, &out, &present));
    try std.testing.expectEqual(@as(u8, 1), present);
    try std.testing.expectEqualStrings("note", std.mem.sliceTo(&out.name, 0));

    rig.fake.dir_next_entry.name_bytes = 99;
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_dir_next(&dir, &out, &present));

    rig.fake.dir_next_present = false;
    try std.testing.expectEqual(core.ok, abi.fw_fs_dir_next(&dir, &out, &present));
    try std.testing.expectEqual(@as(u8, 0), present);
}

test "a closed cursor cannot be advanced or closed twice" {
    var rig = Rig{};
    var names = rig.names();
    var dir: abi.Dir = .{};
    var out: core.DirentValue = .{};
    var present: u8 = 0;
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_dir_close(null));
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_dir_next(&dir, &out, &present));
    try std.testing.expectEqual(core.ok, abi.fw_fs_dir_open(
        &names,
        "/",
        &dir,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.ok, abi.fw_fs_dir_close(&dir));
    try std.testing.expect(!dir.is_open);
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_dir_close(&dir));
}

test "dir_close clears the handle even when the backend fails" {
    var rig = Rig{};
    var names = rig.names();
    var dir: abi.Dir = .{};
    try std.testing.expectEqual(core.ok, abi.fw_fs_dir_open(
        &names,
        "/",
        &dir,
        rig.work(),
        rig.workspace.len,
    ));
    rig.fake.dir_close_result = core.err_invalid_state;
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_dir_close(&dir));
    try std.testing.expect(!dir.is_open);
    try std.testing.expectEqual(@as(?*anyopaque, null), dir.state);
}

// ---------------------------------------------------------------------------
// Streams.
// ---------------------------------------------------------------------------

test "open guard order is port, handle, binding, busy, mode, path, workspace" {
    var rig = Rig{};
    var port = rig.streams();
    var file: abi.File = .{};
    var detached: abi.StreamPort = .{};
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_open(
        null,
        "/a",
        core.open_read,
        &file,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_open(
        &port,
        "/a",
        core.open_read,
        null,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_not_initialized, abi.fw_fs_open(
        &detached,
        "/a",
        core.open_read,
        &file,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_invalid_arg, abi.fw_fs_open(
        &port,
        "/a",
        9,
        &file,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_invalid_arg, abi.fw_fs_open(
        &port,
        "/",
        core.open_read,
        &file,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_invalid_arg, abi.fw_fs_open(
        &port,
        "/a",
        core.open_read,
        &file,
        @ptrFromInt(0x1001),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.ok, abi.fw_fs_open(
        &port,
        "/a",
        core.open_read,
        &file,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expect(file.is_open);
    try std.testing.expectEqual(core.err_busy, abi.fw_fs_open(
        &port,
        "/a",
        core.open_read,
        &file,
        rig.work(),
        rig.workspace.len,
    ));
}

test "exclusive create needs the capability" {
    var rig = Rig{};
    rig.caps.flags &= ~core.cap_create_exclusive;
    var port = rig.streams();
    var file: abi.File = .{};
    try std.testing.expectEqual(core.err_not_supported, abi.fw_fs_open(
        &port,
        "/a",
        core.open_create_new,
        &file,
        rig.work(),
        rig.workspace.len,
    ));
}

test "read and write refuse a backend that overran the caller buffer" {
    var rig = Rig{};
    var file: abi.File = .{};
    try rig.openFile(&file);

    var buffer: [8]u8 = @splat(0);
    var moved: u32 = 0;
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_read(&file, null, 8, &moved));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_read(&file, &buffer, 8, null));

    rig.fake.read_count = 9;
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_read(&file, &buffer, 8, &moved));
    try std.testing.expectEqual(@as(u32, 0), moved);
    rig.fake.read_count = 8;
    try std.testing.expectEqual(core.ok, abi.fw_fs_read(&file, &buffer, 8, &moved));
    try std.testing.expectEqual(@as(u32, 8), moved);

    rig.fake.write_count = 12;
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_write(&file, &buffer, 8, &moved),
    );
    try std.testing.expectEqual(@as(u32, 0), moved);
    rig.fake.write_count = 3;
    try std.testing.expectEqual(core.ok, abi.fw_fs_write(&file, &buffer, 8, &moved));
    try std.testing.expectEqual(@as(u32, 3), moved);
}

test "position queries clear their out parameter first" {
    var rig = Rig{};
    var file: abi.File = .{};
    try rig.openFile(&file);

    var offset: u64 = 1234;
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_tell(&file, null));
    try std.testing.expectEqual(core.ok, abi.fw_fs_tell(&file, &offset));
    try std.testing.expectEqual(@as(u64, 77), offset);

    var size: u64 = 1;
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_file_size(&file, null));
    try std.testing.expectEqual(core.ok, abi.fw_fs_file_size(&file, &size));
    try std.testing.expectEqual(@as(u64, 4096), size);

    try std.testing.expectEqual(core.ok, abi.fw_fs_seek(&file, 16));
}

test "sync is not_supported when the backend omits it" {
    var rig = Rig{};
    var table = stream_table;
    table.sync = null;
    var port = abi.StreamPort{ .iface = &table, .ctx = &rig, .caps = rig.caps };
    var file: abi.File = .{};
    try std.testing.expectEqual(core.ok, abi.fw_fs_open(
        &port,
        "/a",
        core.open_read,
        &file,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_not_supported, abi.fw_fs_sync(&file));
}

test "close detaches the handle even when the backend fails" {
    var rig = Rig{};
    var file: abi.File = .{};
    try rig.openFile(&file);
    rig.fake.close_result = core.err_busy;
    try std.testing.expectEqual(core.err_busy, abi.fw_fs_close(&file));
    try std.testing.expect(!file.is_open);
    try std.testing.expectEqual(@as(u32, 0), file.state_bytes);
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_close(&file));
}

test "every stream entry point refuses an unopened handle" {
    var file: abi.File = .{};
    var buffer: [4]u8 = @splat(0);
    var moved: u32 = 0;
    var offset: u64 = 0;
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_read(null, &buffer, 4, &moved));
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_read(&file, &buffer, 4, &moved));
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_write(&file, &buffer, 4, &moved),
    );
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_seek(&file, 0));
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_tell(&file, &offset));
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_file_size(&file, &offset));
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_sync(&file));

    file.is_open = true;
    try std.testing.expectEqual(core.err_not_initialized, abi.fw_fs_read(&file, &buffer, 4, &moved));
}

// ---------------------------------------------------------------------------
// Transactions.
// ---------------------------------------------------------------------------

test "begin guards its arguments, capability, policy and workspace" {
    var rig = Rig{};
    var port = rig.transactions();
    var transaction: abi.Transaction = .{};
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_transaction_begin(
        null,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_transaction_begin(
        &port,
        "/o",
        core.txn_create_new,
        null,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_invalid_arg, abi.fw_fs_transaction_begin(
        &port,
        "/o",
        7,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_invalid_arg, abi.fw_fs_transaction_begin(
        &port,
        "/",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expectEqual(core.err_no_mem, abi.fw_fs_transaction_begin(
        &port,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        2,
    ));
    try std.testing.expectEqual(core.ok, abi.fw_fs_transaction_begin(
        &port,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expect(transaction.active);
    try std.testing.expect(!transaction.validated);
    try std.testing.expectEqual(core.err_busy, abi.fw_fs_transaction_begin(
        &port,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
}

test "an unbound transaction port answers by its transactions capability" {
    var rig = Rig{};
    var transaction: abi.Transaction = .{};
    var plain = abi.TransactionPort{ .iface = null, .ctx = &rig, .caps = allCaps() };
    try std.testing.expectEqual(core.err_not_initialized, abi.fw_fs_transaction_begin(
        &plain,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
    plain.caps.flags &= ~core.cap_transactions;
    try std.testing.expectEqual(core.err_not_supported, abi.fw_fs_transaction_begin(
        &plain,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
}

test "a failed begin leaves the transaction inactive" {
    var rig = Rig{};
    var port = rig.transactions();
    var transaction: abi.Transaction = .{};
    rig.fake.begin_result = core.err_access_denied;
    try std.testing.expectEqual(core.err_access_denied, abi.fw_fs_transaction_begin(
        &port,
        "/o",
        core.txn_create_new,
        &transaction,
        rig.work(),
        rig.workspace.len,
    ));
    try std.testing.expect(!transaction.active);
}

test "the staged write path closes once validation is accepted" {
    var rig = Rig{};
    var transaction: abi.Transaction = .{};
    try rig.beginTransaction(&transaction);

    var payload: [4]u8 = @splat(7);
    var written: u32 = 0;
    rig.fake.txn_write_count = 4;
    try std.testing.expectEqual(
        core.ok,
        abi.fw_fs_transaction_write(&transaction, &payload, 4, &written),
    );
    try std.testing.expectEqual(@as(u32, 4), written);
    try std.testing.expectEqual(core.ok, abi.fw_fs_transaction_seek(&transaction, 0));

    try std.testing.expectEqual(
        core.err_null_ptr,
        abi.fw_fs_transaction_validate(&transaction, null, null),
    );
    try std.testing.expectEqual(
        core.ok,
        abi.fw_fs_transaction_validate(&transaction, fakeValidator, null),
    );
    try std.testing.expect(transaction.validated);

    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_write(&transaction, &payload, 4, &written),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_seek(&transaction, 8),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_validate(&transaction, fakeValidator, null),
    );
}

fn fakeValidator(ctx: ?*anyopaque, staged: *abi.File) callconv(.c) core.Err {
    _ = ctx;
    _ = staged;
    return core.ok;
}

test "a staged write may not exceed the caller length" {
    var rig = Rig{};
    var transaction: abi.Transaction = .{};
    try rig.beginTransaction(&transaction);
    var payload: [4]u8 = @splat(1);
    var written: u32 = 0;
    rig.fake.txn_write_count = 5;
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_write(&transaction, &payload, 4, &written),
    );
    try std.testing.expectEqual(@as(u32, 0), written);
}

test "commit demands validation and a published answer" {
    var rig = Rig{};
    var transaction: abi.Transaction = .{};
    try rig.beginTransaction(&transaction);
    var published: u8 = 0;
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_commit(&transaction, &published),
    );
    try std.testing.expectEqual(
        core.ok,
        abi.fw_fs_transaction_validate(&transaction, fakeValidator, null),
    );
    try std.testing.expectEqual(
        core.err_null_ptr,
        abi.fw_fs_transaction_commit(&transaction, null),
    );

    rig.fake.commit_published = false;
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_commit(&transaction, &published),
    );
    try std.testing.expect(transaction.active);

    rig.fake.commit_published = true;
    try std.testing.expectEqual(
        core.ok,
        abi.fw_fs_transaction_commit(&transaction, &published),
    );
    try std.testing.expectEqual(@as(u8, 1), published);
    try std.testing.expect(!transaction.active);
    try std.testing.expect(!transaction.validated);
}

test "abort clears the transaction only when the backend agrees" {
    var rig = Rig{};
    var transaction: abi.Transaction = .{};
    try rig.beginTransaction(&transaction);
    rig.fake.abort_result = core.err_busy;
    try std.testing.expectEqual(core.err_busy, abi.fw_fs_transaction_abort(&transaction));
    try std.testing.expect(transaction.active);
    rig.fake.abort_result = core.ok;
    try std.testing.expectEqual(core.ok, abi.fw_fs_transaction_abort(&transaction));
    try std.testing.expect(!transaction.active);
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_abort(&transaction),
    );
}

test "every transaction entry point refuses an inactive handle" {
    var transaction: abi.Transaction = .{};
    var payload: [2]u8 = @splat(0);
    var written: u32 = 0;
    var published: u8 = 0;
    try std.testing.expectEqual(
        core.err_null_ptr,
        abi.fw_fs_transaction_write(null, &payload, 2, &written),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_write(&transaction, &payload, 2, &written),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_seek(&transaction, 0),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_validate(&transaction, fakeValidator, null),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_commit(&transaction, &published),
    );
    try std.testing.expectEqual(
        core.err_invalid_state,
        abi.fw_fs_transaction_abort(&transaction),
    );

    transaction.active = true;
    try std.testing.expectEqual(
        core.err_not_initialized,
        abi.fw_fs_transaction_abort(&transaction),
    );
}
