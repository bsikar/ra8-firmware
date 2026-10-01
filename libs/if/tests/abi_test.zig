//! ABI-membrane tests for the namespace and stream facades: the exported
//! fw_fs_* symbols of those two families driven over the shared fake backend,
//! so guard order, handle mutation and backend-answer scrubbing stay pinned
//! host-side. Transactions live in abi_txn_test.zig, and the ra8_path_* exports
//! this archive also publishes live in path_abi_test.zig.

const std = @import("std");
const abi = @import("abi");
const core = abi.core;
const fake = @import("abi_fake.zig");

// ---------------------------------------------------------------------------
// Binding.
// ---------------------------------------------------------------------------

test "bind rejects each missing argument before validating anything" {
    var rig = fake.Rig{};
    var fs: abi.Fs = .{};
    const caps = rig.caps;
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_bind(
        null,
        &fake.namespace_table,
        &fake.stream_table,
        &fake.transaction_table,
        &rig,
        &caps,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_bind(
        &fs,
        null,
        &fake.stream_table,
        &fake.transaction_table,
        &rig,
        &caps,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_bind(
        &fs,
        &fake.namespace_table,
        null,
        &fake.transaction_table,
        &rig,
        &caps,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_bind(
        &fs,
        &fake.namespace_table,
        &fake.stream_table,
        &fake.transaction_table,
        null,
        &caps,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_bind(
        &fs,
        &fake.namespace_table,
        &fake.stream_table,
        &fake.transaction_table,
        &rig,
        null,
    ));
}

test "a successful bind copies caps into all three facades" {
    var rig = fake.Rig{};
    var fs: abi.Fs = .{};
    const caps = rig.caps;
    try std.testing.expectEqual(core.ok, abi.fw_fs_bind(
        &fs,
        &fake.namespace_table,
        &fake.stream_table,
        &fake.transaction_table,
        &rig,
        &caps,
    ));
    try std.testing.expectEqual(&fake.namespace_table, fs.names.iface.?);
    try std.testing.expectEqual(&fake.stream_table, fs.streams.iface.?);
    try std.testing.expectEqual(&fake.transaction_table, fs.transactions.iface.?);
    try std.testing.expectEqual(caps.flags, fs.names.caps.flags);
    try std.testing.expectEqual(caps.flags, fs.streams.caps.flags);
    try std.testing.expectEqual(caps.flags, fs.transactions.caps.flags);
    try std.testing.expectEqual(@as(?*anyopaque, &rig), fs.names.ctx);

    var read_back: core.Caps = .{};
    try std.testing.expectEqual(core.ok, abi.fw_fs_get_caps(&fs, &read_back));
    try std.testing.expectEqual(caps.path_max_bytes, read_back.path_max_bytes);
}

test "bind refuses an incomplete namespace table" {
    var rig = fake.Rig{};
    var fs: abi.Fs = .{};
    var table = fake.namespace_table;
    table.rmdir = null;
    const caps = rig.caps;
    try std.testing.expectEqual(core.err_invalid_arg, abi.fw_fs_bind(
        &fs,
        &table,
        &fake.stream_table,
        &fake.transaction_table,
        &rig,
        &caps,
    ));
}

test "bind refuses caps whose root path cannot validate" {
    var rig = fake.Rig{};
    var fs: abi.Fs = .{};
    var caps = rig.caps;
    caps.path_max_bytes = 1;
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_bind(
        &fs,
        &fake.namespace_table,
        &fake.stream_table,
        &fake.transaction_table,
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
    const caps = fake.allCaps();
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_path_validate(null, "/a"));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_path_validate(&caps, null));
    try std.testing.expectEqual(core.ok, abi.fw_fs_path_validate(&caps, "/a"));
}

// ---------------------------------------------------------------------------
// Namespace.
// ---------------------------------------------------------------------------

test "stat guard order is facade, out pointer, path" {
    var rig = fake.Rig{};
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
    var rig = fake.Rig{};
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
    var rig = fake.Rig{};
    var names = rig.names();
    var out: core.Stat = .{ .size_bytes = 5 };
    rig.fake.stat_result = core.err_not_supported;
    rig.fake.stat_answer = .{ .exists = true, .node_type = 9 };
    try std.testing.expectEqual(core.err_not_supported, abi.fw_fs_stat(&names, "/a", &out));
}

test "listdir guards its three out parameters then the entry budget" {
    var rig = fake.Rig{};
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
        fake.fakeListCallback,
        null,
        null,
        &complete,
    ));
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_listdir(
        &names,
        "/",
        4,
        fake.fakeListCallback,
        null,
        &count,
        null,
    ));
    try std.testing.expectEqual(core.err_invalid_arg, abi.fw_fs_listdir(
        &names,
        "/",
        0,
        fake.fakeListCallback,
        null,
        &count,
        &complete,
    ));
}

test "listdir refuses a backend that overran the budget" {
    var rig = fake.Rig{};
    var names = rig.names();
    var count: u32 = 0;
    var complete: u8 = 0;
    rig.fake.listdir_count = 5;
    try std.testing.expectEqual(core.err_invalid_state, abi.fw_fs_listdir(
        &names,
        "/",
        4,
        fake.fakeListCallback,
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
        fake.fakeListCallback,
        null,
        &count,
        &complete,
    ));
    try std.testing.expectEqual(@as(u32, 4), count);
}

test "mkdir, unlink and rmdir refuse the root and reach the backend" {
    var rig = fake.Rig{};
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
    var rig = fake.Rig{};
    var table = fake.namespace_table;
    table.mkdir = null;
    var names = abi.Namespace{ .iface = &table, .ctx = &rig, .caps = rig.caps };
    try std.testing.expectEqual(core.err_not_supported, abi.fw_fs_mkdir(&names, "/dir"));
}

// @par MC/DC:
// Covers the production ``isRoot(old_path) or isRoot(new_path)`` decision:
// - old root, new non-root -> deny (left condition independently true)
// - old non-root, new root -> deny (right condition independently true)
// - both non-root -> reach backend (both conditions false)
test "rename checks capability first, then both paths, then the root" {
    var rig = fake.Rig{};
    rig.caps.flags &= ~core.cap_atomic_replace;
    var names = rig.names();
    try std.testing.expectEqual(
        core.err_not_supported,
        abi.fw_fs_rename(&names, "/a", "/b", 1),
    );
    try std.testing.expectEqual(core.ok, abi.fw_fs_rename(&names, "/a", "/b", 0));
    try std.testing.expect(!rig.fake.last_replace);
    try std.testing.expectEqual(@as(u32, 1), rig.fake.calls);
    try std.testing.expectEqual(
        core.err_access_denied,
        abi.fw_fs_rename(&names, "/", "/b", 0),
    );
    try std.testing.expectEqual(@as(u32, 1), rig.fake.calls);
    try std.testing.expectEqual(
        core.err_access_denied,
        abi.fw_fs_rename(&names, "/a", "/", 0),
    );
    try std.testing.expectEqual(@as(u32, 1), rig.fake.calls);
    try std.testing.expectEqual(
        core.err_invalid_arg,
        abi.fw_fs_rename(&names, "/a", "b", 0),
    );
}

// @par MC/DC:
// Covers ``result == ok and spaceIncoherent(target)``:
// - backend error + incoherent answer -> false (left condition independently false)
// - success + coherent answer -> false (right condition independently false)
// - success + incoherent answer -> true (both conditions true; answer scrubbed)
test "space needs the capability, the operation and a coherent answer" {
    var rig = fake.Rig{};
    rig.caps.flags &= ~core.cap_space_query;
    var without = rig.names();
    var out: core.Space = .{};
    try std.testing.expectEqual(core.err_not_supported, abi.fw_fs_space(&without, &out));

    rig.caps = fake.allCaps();
    var table = fake.namespace_table;
    table.space = null;
    var missing = abi.Namespace{ .iface = &table, .ctx = &rig, .caps = rig.caps };
    try std.testing.expectEqual(core.err_not_supported, abi.fw_fs_space(&missing, &out));

    var names = rig.names();
    try std.testing.expectEqual(core.err_null_ptr, abi.fw_fs_space(&names, null));

    rig.fake.space_result = core.err_busy;
    rig.fake.space_answer = .{ .total_bytes = 100, .free_bytes = 200 };
    try std.testing.expectEqual(core.err_busy, abi.fw_fs_space(&names, &out));
    try std.testing.expectEqual(@as(u64, 100), out.total_bytes);
    try std.testing.expectEqual(@as(u64, 200), out.free_bytes);

    rig.fake.space_result = core.ok;
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
    var rig = fake.Rig{};
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
    var rig = fake.Rig{};
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

// @par MC/DC:
// Covers ``result != ok or !present``:
// - error + present -> true from the left condition alone; output stays zero
// - success + absent -> true from the right condition alone; output stays zero
// - success + present -> false; coherent candidate is published
test "dir_next publishes only a coherent entry" {
    var rig = fake.Rig{};
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

    rig.fake.dir_next_result = core.err_busy;
    out.name[0] = 'x';
    present = 1;
    try std.testing.expectEqual(core.err_busy, abi.fw_fs_dir_next(&dir, &out, &present));
    try std.testing.expectEqual(@as(u8, 0), out.name[0]);
    try std.testing.expectEqual(@as(u8, 0), present);

    rig.fake.dir_next_result = core.ok;
    rig.fake.dir_next_present = false;
    out.name[0] = 'x';
    present = 1;
    try std.testing.expectEqual(core.ok, abi.fw_fs_dir_next(&dir, &out, &present));
    try std.testing.expectEqual(@as(u8, 0), out.name[0]);
    try std.testing.expectEqual(@as(u8, 0), present);

    rig.fake.dir_next_present = true;
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
    var rig = fake.Rig{};
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
    var rig = fake.Rig{};
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
    var rig = fake.Rig{};
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
    var rig = fake.Rig{};
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
    var rig = fake.Rig{};
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
    var rig = fake.Rig{};
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
    var rig = fake.Rig{};
    var table = fake.stream_table;
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
    var rig = fake.Rig{};
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
