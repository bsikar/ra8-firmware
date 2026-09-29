//! Zig-native tests for the ABI membrane, driven against a fake VFS.
//!
//! Every seam below the adapter is exported here, so the vtables exercise the
//! same link-time substitution the C suite gets from the real `ra8_io_vfs` and
//! `ra8_fs`. `fw_fs_bind` captures what the adapter published, which is how the
//! tests reach the otherwise file-private vtables.

const std = @import("std");
const abi = @import("abi");
const core = abi.core;

const expect = std.testing.expect;
const expectEqual = std.testing.expectEqual;

// ---------------------------------------------------------------------------
// Fake VFS medium.
// ---------------------------------------------------------------------------

const max_nodes = 12;
const node_name_cap = 64;
const node_data_cap = 128;

const Node = struct {
    used: bool = false,
    name: [node_name_cap]u8 = [_]u8{0} ** node_name_cap,
    is_dir: bool = false,
    data: [node_data_cap]u8 = [_]u8{0} ** node_data_cap,
    length: u32 = 0,
};

const Handle = struct {
    used: bool = false,
    node: usize = 0,
    offset: u64 = 0,
};

const err_not_found: u16 = 0x10B;

var g_nodes: [max_nodes]Node = undefined;
var g_handles: [4]Handle = undefined;

var g_stat_rc: u16 = 0;
var g_open_rc: u16 = 0;
var g_rename_rc: u16 = 0;
var g_mkdir_rc: u16 = 0;
var g_unlink_rc: u16 = 0;
var g_rmdir_rc: u16 = 0;
var g_listdir_rc: u16 = 0;
var g_space_rc: u16 = 0;
var g_requirements_rc: u16 = 0;
var g_dir_open_rc: u16 = 0;
var g_dir_close_rc: u16 = 0;
var g_read_rc: u16 = 0;
var g_write_rc: u16 = 0;
var g_seek_rc: u16 = 0;
var g_close_rc: u16 = 0;
var g_bind_rc: u16 = 0;

var g_req_bytes: u32 = 640;
var g_req_align: u8 = 8;
var g_req_max_open: u16 = 2;

var g_last_path: [core.full_path_cap]u8 = undefined;
var g_last_path_b: [core.full_path_cap]u8 = undefined;
var g_last_dir_workspace: usize = 0;
var g_last_dir_bytes: u32 = 0;
var g_last_open_mode: u8 = 0xFF;

var g_stat_calls: u32 = 0;
var g_open_calls: u32 = 0;
var g_rename_calls: u32 = 0;
var g_unlink_calls: u32 = 0;
var g_close_calls: u32 = 0;
var g_mkdir_calls: u32 = 0;
var g_rmdir_calls: u32 = 0;

var g_bound_names: ?*const abi.NamespaceIface = null;
var g_bound_stream: ?*const abi.StreamIface = null;
var g_bound_txn: ?*const abi.TransactionIface = null;
var g_bound_ctx: ?*anyopaque = null;
var g_bound_caps: core.Caps = .{};
var g_bind_calls: u32 = 0;

var g_dir_cursor: usize = 0;

fn record(dst: *[core.full_path_cap]u8, path: [*:0]const u8) void {
    const length = core.len(path, core.full_path_cap - 1);
    @memset(dst, 0);
    @memcpy(dst[0..length], path[0..length]);
}

fn pathEq(dst: *const [core.full_path_cap]u8, want: []const u8) bool {
    return std.mem.eql(u8, dst[0..want.len], want) and dst[want.len] == 0;
}

fn findNode(path: [*:0]const u8) ?usize {
    const length = core.len(path, node_name_cap);
    for (&g_nodes, 0..) |*node, index| {
        if (!node.used) continue;
        if (core.len(&node.name, node_name_cap) != length) continue;
        if (std.mem.eql(u8, node.name[0..length], path[0..length])) return index;
    }
    return null;
}

fn addNode(name: []const u8, is_dir: bool, payload: []const u8) usize {
    for (&g_nodes, 0..) |*node, index| {
        if (node.used) continue;
        node.* = .{};
        node.used = true;
        node.is_dir = is_dir;
        @memcpy(node.name[0..name.len], name);
        @memcpy(node.data[0..payload.len], payload);
        node.length = @intCast(payload.len);
        return index;
    }
    unreachable;
}

fn resetMedium() void {
    for (&g_nodes) |*node| node.* = .{};
    for (&g_handles) |*handle| handle.* = .{};
    g_stat_rc = 0;
    g_open_rc = 0;
    g_rename_rc = 0;
    g_mkdir_rc = 0;
    g_unlink_rc = 0;
    g_rmdir_rc = 0;
    g_listdir_rc = 0;
    g_space_rc = 0;
    g_requirements_rc = 0;
    g_dir_open_rc = 0;
    g_dir_close_rc = 0;
    g_read_rc = 0;
    g_write_rc = 0;
    g_seek_rc = 0;
    g_close_rc = 0;
    g_bind_rc = 0;
    g_req_bytes = 640;
    g_req_align = 8;
    g_req_max_open = 2;
    @memset(&g_last_path, 0);
    @memset(&g_last_path_b, 0);
    g_last_dir_workspace = 0;
    g_last_dir_bytes = 0;
    g_last_open_mode = 0xFF;
    g_stat_calls = 0;
    g_open_calls = 0;
    g_rename_calls = 0;
    g_unlink_calls = 0;
    g_close_calls = 0;
    g_mkdir_calls = 0;
    g_rmdir_calls = 0;
    g_bound_names = null;
    g_bound_stream = null;
    g_bound_txn = null;
    g_bound_ctx = null;
    g_bound_caps = .{};
    g_bind_calls = 0;
    g_dir_cursor = 0;
}

// ---------------------------------------------------------------------------
// Exported seams.
// ---------------------------------------------------------------------------

export fn ra8_io_vfs_stat(path: [*:0]const u8, out: *core.NativeStat) callconv(.c) u16 {
    g_stat_calls += 1;
    record(&g_last_path, path);
    if (g_stat_rc != 0) return g_stat_rc;
    out.* = .{};
    out.created = .{
        .value = .{ .year = 2026, .month = 9, .day = 17, .centisecond = 25 },
        .valid = true,
        .utc_offset_valid = false,
    };
    if (findNode(path)) |index| {
        const node = &g_nodes[index];
        out.exists = true;
        out.is_directory = node.is_dir;
        out.attr = if (node.is_dir) core.fs_attr_directory else 0x20;
        out.size_bytes = node.length;
    }
    return 0;
}

export fn ra8_io_vfs_open(
    path: [*:0]const u8,
    mode: u8,
    out_file: *?*anyopaque,
) callconv(.c) u16 {
    g_open_calls += 1;
    g_last_open_mode = mode;
    record(&g_last_path, path);
    if (g_open_rc != 0) return g_open_rc;
    var index = findNode(path);
    if (index == null) {
        if (mode == core.fs_mode_read) return err_not_found;
        const length = core.len(path, node_name_cap);
        index = addNode(path[0..length], false, "");
    } else if (mode == core.fs_mode_write) {
        g_nodes[index.?].length = 0;
    }
    for (&g_handles) |*handle| {
        if (handle.used) continue;
        handle.* = .{
            .used = true,
            .node = index.?,
            .offset = if (mode == core.fs_mode_append) g_nodes[index.?].length else 0,
        };
        out_file.* = @ptrCast(handle);
        return 0;
    }
    return core.err_no_mem;
}

fn handleOf(file: ?*anyopaque) ?*Handle {
    if (file == null) return null;
    return @ptrCast(@alignCast(file.?));
}

export fn ra8_fs_read(
    file: ?*anyopaque,
    buf: [*]u8,
    max_len: u32,
    got_len: *u32,
) callconv(.c) u16 {
    if (g_read_rc != 0) return g_read_rc;
    const handle = handleOf(file) orelse return core.err_invalid_state;
    const node = &g_nodes[handle.node];
    const remaining: u32 = node.length - @as(u32, @intCast(handle.offset));
    const take = @min(max_len, remaining);
    @memcpy(buf[0..take], node.data[@intCast(handle.offset)..][0..take]);
    handle.offset += take;
    got_len.* = take;
    return 0;
}

export fn ra8_fs_write(file: ?*anyopaque, buf: [*]const u8, length: u32) callconv(.c) u16 {
    if (g_write_rc != 0) return g_write_rc;
    const handle = handleOf(file) orelse return core.err_invalid_state;
    const node = &g_nodes[handle.node];
    const start: u32 = @intCast(handle.offset);
    if (start + length > node_data_cap) return core.err_no_mem;
    @memcpy(node.data[start..][0..length], buf[0..length]);
    handle.offset += length;
    if (handle.offset > node.length) node.length = @intCast(handle.offset);
    return 0;
}

export fn ra8_fs_seek(file: ?*anyopaque, offset_bytes: u64) callconv(.c) u16 {
    if (g_seek_rc != 0) return g_seek_rc;
    const handle = handleOf(file) orelse return core.err_invalid_state;
    handle.offset = offset_bytes;
    return 0;
}

export fn ra8_fs_tell(file: ?*anyopaque, out_offset: *u64) callconv(.c) u16 {
    const handle = handleOf(file) orelse return core.err_invalid_state;
    out_offset.* = handle.offset;
    return 0;
}

export fn ra8_fs_size(file: ?*anyopaque, out_bytes: *u64) callconv(.c) u16 {
    const handle = handleOf(file) orelse return core.err_invalid_state;
    out_bytes.* = g_nodes[handle.node].length;
    return 0;
}

export fn ra8_fs_close(file: ?*anyopaque) callconv(.c) u16 {
    g_close_calls += 1;
    if (g_close_rc != 0) return g_close_rc;
    const handle = handleOf(file) orelse return core.err_invalid_state;
    handle.used = false;
    return 0;
}

export fn ra8_io_vfs_rename(old_path: [*:0]const u8, new_path: [*:0]const u8) callconv(.c) u16 {
    g_rename_calls += 1;
    record(&g_last_path, old_path);
    record(&g_last_path_b, new_path);
    if (g_rename_rc != 0) return g_rename_rc;
    const source = findNode(old_path) orelse return err_not_found;
    if (findNode(new_path) != null) return core.err_exists;
    const length = core.len(new_path, node_name_cap);
    @memset(&g_nodes[source].name, 0);
    @memcpy(g_nodes[source].name[0..length], new_path[0..length]);
    return 0;
}

export fn ra8_io_vfs_mkdir(path: [*:0]const u8) callconv(.c) u16 {
    g_mkdir_calls += 1;
    record(&g_last_path, path);
    if (g_mkdir_rc != 0) return g_mkdir_rc;
    const length = core.len(path, node_name_cap);
    _ = addNode(path[0..length], true, "");
    return 0;
}

export fn ra8_io_vfs_unlink(path: [*:0]const u8) callconv(.c) u16 {
    g_unlink_calls += 1;
    record(&g_last_path, path);
    if (g_unlink_rc != 0) return g_unlink_rc;
    const index = findNode(path) orelse return err_not_found;
    g_nodes[index].used = false;
    return 0;
}

export fn ra8_io_vfs_rmdir(path: [*:0]const u8) callconv(.c) u16 {
    g_rmdir_calls += 1;
    record(&g_last_path, path);
    return g_rmdir_rc;
}

export fn ra8_io_vfs_listdir(
    path: [*:0]const u8,
    cb: *const fn ([*:0]const u8, u8, u64, ?*anyopaque) callconv(.c) void,
    ctx: ?*anyopaque,
) callconv(.c) u16 {
    record(&g_last_path, path);
    for (&g_nodes) |*node| {
        if (!node.used) continue;
        const attr: u8 = if (node.is_dir) core.fs_attr_directory else 0x20;
        cb(@ptrCast(&node.name), attr, node.length, ctx);
    }
    return g_listdir_rc;
}

export fn ra8_io_vfs_free_space(name: [*:0]const u8, out: *core.NativeSpace) callconv(.c) u16 {
    record(&g_last_path, name);
    if (g_space_rc != 0) return g_space_rc;
    out.* = .{
        .total_bytes = 4096,
        .free_bytes = 1024,
        .used_bytes = 3072,
        .bytes_per_cluster = 512,
    };
    return 0;
}

export fn ra8_io_vfs_dir_requirements(
    path: [*:0]const u8,
    out_bytes: *u32,
    out_align: *u8,
    out_max_open: *u16,
) callconv(.c) u16 {
    record(&g_last_path, path);
    if (g_requirements_rc != 0) return g_requirements_rc;
    out_bytes.* = g_req_bytes;
    out_align.* = g_req_align;
    out_max_open.* = g_req_max_open;
    return 0;
}

export fn ra8_io_vfs_dir_open(
    path: [*:0]const u8,
    directory: *core.NativeDir,
    workspace: ?*anyopaque,
    workspace_bytes: u32,
) callconv(.c) u16 {
    record(&g_last_path, path);
    g_last_dir_workspace = @intFromPtr(workspace);
    g_last_dir_bytes = workspace_bytes;
    if (g_dir_open_rc != 0) return g_dir_open_rc;
    directory.state = workspace;
    directory.state_bytes = workspace_bytes;
    directory.is_open = true;
    g_dir_cursor = 0;
    return 0;
}

export fn ra8_io_vfs_dir_next(
    directory: *core.NativeDir,
    out: *core.NativeDirent,
    out_entry: *bool,
) callconv(.c) u16 {
    _ = directory;
    out_entry.* = false;
    while (g_dir_cursor < max_nodes) {
        const node = &g_nodes[g_dir_cursor];
        g_dir_cursor += 1;
        if (!node.used) continue;
        out.* = std.mem.zeroes(core.NativeDirent);
        const length = core.len(&node.name, node_name_cap);
        @memcpy(out.name[0..length], node.name[0..length]);
        out.size_bytes = node.length;
        out.attr = if (node.is_dir) core.fs_attr_directory else 0x20;
        out_entry.* = true;
        return 0;
    }
    return 0;
}

export fn ra8_io_vfs_dir_close(directory: *core.NativeDir) callconv(.c) u16 {
    directory.is_open = false;
    return g_dir_close_rc;
}

export fn fw_fs_bind(
    out: *abi.Fs,
    namespace_iface: *const abi.NamespaceIface,
    stream_iface: *const abi.StreamIface,
    transaction_iface: *const abi.TransactionIface,
    ctx: ?*anyopaque,
    caps: *const core.Caps,
) callconv(.c) u16 {
    g_bind_calls += 1;
    g_bound_names = namespace_iface;
    g_bound_stream = stream_iface;
    g_bound_txn = transaction_iface;
    g_bound_ctx = ctx;
    g_bound_caps = caps.*;
    if (g_bind_rc != 0) return g_bind_rc;
    out.* = .{
        .names = .{ .iface = namespace_iface, .ctx = ctx, .caps = caps.* },
        .streams = .{ .iface = stream_iface, .ctx = ctx, .caps = caps.* },
        .transactions = .{ .iface = transaction_iface, .ctx = ctx, .caps = caps.* },
        .caps = caps.*,
    };
    return 0;
}

export fn fw_fs_close(file: *abi.File) callconv(.c) u16 {
    const iface = file.iface orelse return core.err_invalid_state;
    const closer = iface.close orelse return core.err_not_supported;
    const result = closer(file.ctx, file.state);
    file.is_open = false;
    return result;
}

// ---------------------------------------------------------------------------
// Fixture.
// ---------------------------------------------------------------------------

const Fixture = struct {
    fs: abi.Fs = .{},
    state: abi.State = undefined,
    mount: abi.Mount = .{},
};

fn mountedFat(fixture: *Fixture, removable: bool) u16 {
    resetMedium();
    fixture.mount = .{ .in_use = 1, .fs_type = 16 };
    const cfg = abi.Config{
        .mount_name = "ram",
        .mount = &fixture.mount,
        .removable_media = removable,
    };
    return abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg);
}

fn names() *const abi.NamespaceIface {
    return g_bound_names.?;
}

fn streams() *const abi.StreamIface {
    return g_bound_stream.?;
}

fn txns() *const abi.TransactionIface {
    return g_bound_txn.?;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

test "init: every required pointer is checked before anything is touched" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1, .fs_type = 16 };
    const cfg = abi.Config{ .mount_name = "ram", .mount = &fixture.mount };
    try expectEqual(core.err_null_ptr, abi.fw_fs_ra8_vfs_init(null, &fixture.state, &cfg));
    try expectEqual(core.err_null_ptr, abi.fw_fs_ra8_vfs_init(&fixture.fs, null, &cfg));
    try expectEqual(core.err_null_ptr, abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, null));
    try expectEqual(@as(u32, 0), g_bind_calls);
}

test "init: a missing mount name or mount is null_ptr" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1 };
    var cfg = abi.Config{ .mount_name = null, .mount = &fixture.mount };
    try expectEqual(
        core.err_null_ptr,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
    cfg = .{ .mount_name = "ram", .mount = null };
    try expectEqual(
        core.err_null_ptr,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
}

test "init: an unmounted volume is not_initialized" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 0 };
    const cfg = abi.Config{ .mount_name = "ram", .mount = &fixture.mount };
    try expectEqual(
        core.err_not_initialized,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
}

test "init: a mount name with a separator is invalid_arg" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1 };
    const cfg = abi.Config{ .mount_name = "ra/m", .mount = &fixture.mount };
    try expectEqual(
        core.err_invalid_arg,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
    try expectEqual(@as(u32, 0), g_bind_calls);
}

test "init: a directory-requirements failure is returned verbatim" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1 };
    g_requirements_rc = core.err_not_supported;
    const cfg = abi.Config{ .mount_name = "ram", .mount = &fixture.mount };
    try expectEqual(
        core.err_not_supported,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
}

test "init: an unrepresentable workspace total is invalid_size" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1 };
    g_req_bytes = 0xFFFF_FFFF;
    g_req_align = 8;
    const cfg = abi.Config{ .mount_name = "ram", .mount = &fixture.mount };
    try expectEqual(
        core.err_invalid_size,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
}

test "init: requirements are queried against the bound root" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    try expect(pathEq(&g_last_path, "ram:/"));
    try expectEqual(@as(u32, 640), fixture.state.directory_workspace_bytes);
    try expectEqual(@as(u8, 8), fixture.state.directory_workspace_align);
    try expectEqual(@as(u16, 2), fixture.state.max_open_directories);
}

test "init: FAT capabilities reach fw_fs_bind with the adapter as context" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    try expectEqual(@as(u32, 1), g_bind_calls);
    try expectEqual(@as(?*anyopaque, @ptrCast(&fixture.state)), g_bound_ctx);
    try expectEqual(core.fs_fat_max_file_bytes, g_bound_caps.max_file_bytes);
    try expectEqual(core.fat_name_max_bytes, g_bound_caps.name_max_bytes);
    try expectEqual(@as(u32, @sizeOf(core.DirectoryState) + 8 - 1 + 640), g_bound_caps.directory_workspace_bytes);
    try expectEqual(@as(u32, 0), g_bound_caps.flags & core.cap_removable_media);
}

test "init: exFAT and removable media change only what they should" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1, .fs_type = core.fs_type_exfat };
    const cfg = abi.Config{
        .mount_name = "sd0",
        .mount = &fixture.mount,
        .removable_media = true,
    };
    try expectEqual(core.ok, abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg));
    try expectEqual(std.math.maxInt(u64), g_bound_caps.max_file_bytes);
    try expectEqual(core.exfat_name_max_bytes, g_bound_caps.name_max_bytes);
    try expect((g_bound_caps.flags & core.cap_removable_media) != 0);
    try expect(fixture.state.removable_media);
}

test "init: a bind failure is returned verbatim" {
    var fixture = Fixture{};
    resetMedium();
    fixture.mount = .{ .in_use = 1 };
    g_bind_rc = core.err_invalid_arg;
    const cfg = abi.Config{ .mount_name = "ram", .mount = &fixture.mount };
    try expectEqual(
        core.err_invalid_arg,
        abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg),
    );
}

// ---------------------------------------------------------------------------
// namespace
// ---------------------------------------------------------------------------

test "stat: a hit reports size, type and translated timestamps" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/books/a.cbz", false, "hello");
    var out: core.Stat = .{};
    try expectEqual(core.ok, names().stat.?(g_bound_ctx, "/books/a.cbz", &out));
    try expect(pathEq(&g_last_path, "ram:/books/a.cbz"));
    try expect(out.exists);
    try expectEqual(core.node_file, out.kind);
    try expectEqual(@as(u64, 5), out.size_bytes);
    try expect(out.created.valid);
    try expectEqual(@as(u32, 250_000_000), out.created.value.nanosecond);
    try expect(!out.modified.valid);
}

test "stat: a directory is classified as a directory" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/books", true, "");
    var out: core.Stat = .{};
    try expectEqual(core.ok, names().stat.?(g_bound_ctx, "/books", &out));
    try expectEqual(core.node_directory, out.kind);
}

test "stat: a clean miss is success with no node kind" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var out: core.Stat = .{ .kind = core.node_file, .exists = true };
    try expectEqual(core.ok, names().stat.?(g_bound_ctx, "/nope", &out));
    try expect(!out.exists);
    try expectEqual(core.node_none, out.kind);
}

test "stat: a native failure is returned without writing the output" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    g_stat_rc = core.err_invalid_state;
    var out: core.Stat = .{ .size_bytes = 77 };
    try expectEqual(core.err_invalid_state, names().stat.?(g_bound_ctx, "/x", &out));
    try expectEqual(@as(u64, 77), out.size_bytes);
}

test "mkdir, unlink and rmdir all dispatch on the prefixed path" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    try expectEqual(core.ok, names().mkdir.?(g_bound_ctx, "/books"));
    try expect(pathEq(&g_last_path, "ram:/books"));
    _ = addNode("ram:/books/a.cbz", false, "x");
    try expectEqual(core.ok, names().unlink.?(g_bound_ctx, "/books/a.cbz"));
    try expect(pathEq(&g_last_path, "ram:/books/a.cbz"));
    try expectEqual(core.ok, names().rmdir.?(g_bound_ctx, "/books"));
    try expect(pathEq(&g_last_path, "ram:/books"));
    try expectEqual(@as(u32, 1), g_mkdir_calls);
    try expectEqual(@as(u32, 1), g_unlink_calls);
    try expectEqual(@as(u32, 1), g_rmdir_calls);
}

test "rename: a replacement request is refused without touching the volume" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    try expectEqual(
        core.err_not_supported,
        names().rename.?(g_bound_ctx, "/a", "/b", true),
    );
    try expectEqual(@as(u32, 0), g_rename_calls);
}

test "rename: both paths are built in separate scratch buffers" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/a", false, "x");
    try expectEqual(core.ok, names().rename.?(g_bound_ctx, "/a", "/b", false));
    try expect(pathEq(&g_last_path, "ram:/a"));
    try expect(pathEq(&g_last_path_b, "ram:/b"));
}

test "space: the three portable fields are copied from the live mount" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var out: core.Space = .{};
    try expectEqual(core.ok, names().space.?(g_bound_ctx, &out));
    try expect(pathEq(&g_last_path, "ram"));
    try expectEqual(@as(u64, 4096), out.total_bytes);
    try expectEqual(@as(u64, 1024), out.free_bytes);
    try expectEqual(@as(u64, 3072), out.used_bytes);
}

test "space: a native failure leaves the output alone" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    g_space_rc = core.err_invalid_state;
    var out: core.Space = .{ .total_bytes = 9 };
    try expectEqual(core.err_invalid_state, names().space.?(g_bound_ctx, &out));
    try expectEqual(@as(u64, 9), out.total_bytes);
}

// ---------------------------------------------------------------------------
// listdir
// ---------------------------------------------------------------------------

const Collector = struct {
    seen: u32 = 0,
    stop_after: u32 = 0xFFFF_FFFF,
    fail_at: u32 = 0xFFFF_FFFF,

    fn callback(ctx: ?*anyopaque, entry: *const core.Dirent, out_continue: *bool) callconv(.c) u16 {
        const self: *Collector = @ptrCast(@alignCast(ctx.?));
        _ = entry;
        self.seen += 1;
        if (self.seen == self.fail_at) return core.err_invalid_state;
        if (self.seen >= self.stop_after) out_continue.* = false;
        return core.ok;
    }
};

test "listdir: the budget bounds delivery and reports an incomplete walk" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/a", false, "1");
    _ = addNode("ram:/b", false, "22");
    _ = addNode("ram:/c", false, "333");
    var collector = Collector{};
    var count: u32 = 0;
    var complete: bool = true;
    try expectEqual(core.ok, names().listdir.?(
        g_bound_ctx,
        "/",
        2,
        &Collector.callback,
        &collector,
        &count,
        &complete,
    ));
    try expectEqual(@as(u32, 2), count);
    try expectEqual(@as(u32, 2), collector.seen);
    try expect(!complete);
}

test "listdir: a complete walk within budget reports complete" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/a", false, "1");
    var collector = Collector{};
    var count: u32 = 0;
    var complete: bool = false;
    try expectEqual(core.ok, names().listdir.?(
        g_bound_ctx,
        "/",
        8,
        &Collector.callback,
        &collector,
        &count,
        &complete,
    ));
    try expectEqual(@as(u32, 1), count);
    try expect(complete);
}

test "listdir: a callback error outranks the native result" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/a", false, "1");
    g_listdir_rc = core.err_not_supported;
    var collector = Collector{ .fail_at = 1 };
    var count: u32 = 0;
    var complete: bool = true;
    try expectEqual(core.err_invalid_state, names().listdir.?(
        g_bound_ctx,
        "/",
        8,
        &Collector.callback,
        &collector,
        &count,
        &complete,
    ));
    try expect(!complete);
}

test "listdir: the native result stands when the callback is clean" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    g_listdir_rc = core.err_not_supported;
    var collector = Collector{};
    var count: u32 = 0;
    var complete: bool = false;
    try expectEqual(core.err_not_supported, names().listdir.?(
        g_bound_ctx,
        "/",
        8,
        &Collector.callback,
        &collector,
        &count,
        &complete,
    ));
    try expectEqual(@as(u32, 0), count);
    try expect(complete);
}

// ---------------------------------------------------------------------------
// directory cursors
// ---------------------------------------------------------------------------

test "dir_open: a span too small for the cursor is no_mem" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var span: [4096]u8 align(16) = undefined;
    try expectEqual(core.err_no_mem, names().dir_open.?(g_bound_ctx, "/", &span, 4));
}

test "dir_open: a span too small for the format workspace is no_mem" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var span: [4096]u8 align(16) = undefined;
    const just_the_cursor: u32 = @sizeOf(core.DirectoryState) + 4;
    try expectEqual(core.err_no_mem, names().dir_open.?(g_bound_ctx, "/", &span, just_the_cursor));
}

test "dir_open: the format workspace lands past the cursor, correctly aligned" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var span: [4096]u8 align(16) = undefined;
    try expectEqual(core.ok, names().dir_open.?(g_bound_ctx, "/", &span, span.len));
    try expect(pathEq(&g_last_path, "ram:/"));
    try expect(g_last_dir_workspace >= @intFromPtr(&span) + @sizeOf(core.DirectoryState));
    try expectEqual(@as(usize, 0), g_last_dir_workspace % 8);
    try expectEqual(@as(u32, span.len) - @as(u32, @intCast(g_last_dir_workspace - @intFromPtr(&span))), g_last_dir_bytes);
}

test "dir_next: an entry is copied and classified, then the walk ends cleanly" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/books", true, "");
    var span: [4096]u8 align(16) = undefined;
    try expectEqual(core.ok, names().dir_open.?(g_bound_ctx, "/", &span, span.len));
    var value: core.DirentValue = std.mem.zeroes(core.DirentValue);
    var have: bool = false;
    try expectEqual(core.ok, names().dir_next.?(g_bound_ctx, &span, &value, &have));
    try expect(have);
    try expectEqual(core.node_directory, value.kind);
    try expectEqual(@as(u16, 10), value.name_bytes);
    try expect(std.mem.eql(u8, value.name[0..10], "ram:/books"));
    try expectEqual(core.ok, names().dir_next.?(g_bound_ctx, &span, &value, &have));
    try expect(!have);
}

test "dir_close: the cursor is consumed through the same span" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var span: [4096]u8 align(16) = undefined;
    try expectEqual(core.ok, names().dir_open.?(g_bound_ctx, "/", &span, span.len));
    try expectEqual(core.ok, names().dir_close.?(g_bound_ctx, &span));
    const cursor: *core.DirectoryState = @ptrFromInt(core.dirCursorBase(@intFromPtr(&span)));
    try expect(!cursor.native.is_open);
}

// ---------------------------------------------------------------------------
// streams
// ---------------------------------------------------------------------------

test "open: an undersized workspace is no_mem before any mode check" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var file: core.FileState = .{};
    try expectEqual(
        core.err_no_mem,
        streams().open.?(g_bound_ctx, "/a", core.open_create_new, &file, 0),
    );
    try expectEqual(@as(u32, 0), g_open_calls);
}

test "open: create-new has no native equivalent" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var file: core.FileState = .{};
    try expectEqual(core.err_not_supported, streams().open.?(
        g_bound_ctx,
        "/a",
        core.open_create_new,
        &file,
        @sizeOf(core.FileState),
    ));
    try expectEqual(@as(u32, 0), g_open_calls);
}

test "stream: write, seek, tell, size, read and close round trip" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var file: core.FileState = .{};
    const bytes = @sizeOf(core.FileState);
    try expectEqual(core.ok, streams().open.?(
        g_bound_ctx,
        "/a.bin",
        core.open_write_truncate,
        &file,
        bytes,
    ));
    try expectEqual(core.fs_mode_write, g_last_open_mode);
    var written: u32 = 0;
    try expectEqual(core.ok, streams().write.?(g_bound_ctx, &file, "hello", 5, &written));
    try expectEqual(@as(u32, 5), written);
    var offset: u64 = 0;
    try expectEqual(core.ok, streams().tell.?(g_bound_ctx, &file, &offset));
    try expectEqual(@as(u64, 5), offset);
    var size: u64 = 0;
    try expectEqual(core.ok, streams().size.?(g_bound_ctx, &file, &size));
    try expectEqual(@as(u64, 5), size);
    try expectEqual(core.ok, streams().seek.?(g_bound_ctx, &file, 1));
    var buf = [_]u8{0} ** 8;
    var got: u32 = 0;
    try expectEqual(core.ok, streams().read.?(g_bound_ctx, &file, &buf, buf.len, &got));
    try expectEqual(@as(u32, 4), got);
    try expect(std.mem.eql(u8, buf[0..4], "ello"));
    try expectEqual(core.ok, streams().close.?(g_bound_ctx, &file));
    try expectEqual(@as(?*anyopaque, null), file.native);
}

test "write: a failed native write publishes no byte count" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var file: core.FileState = .{};
    try expectEqual(core.ok, streams().open.?(
        g_bound_ctx,
        "/a.bin",
        core.open_write_truncate,
        &file,
        @sizeOf(core.FileState),
    ));
    g_write_rc = core.err_invalid_state;
    var written: u32 = 0xFFFF;
    try expectEqual(
        core.err_invalid_state,
        streams().write.?(g_bound_ctx, &file, "hello", 5, &written),
    );
    try expectEqual(@as(u32, 0xFFFF), written);
}

test "close: the handle is consumed even when the native close fails" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var file: core.FileState = .{};
    try expectEqual(core.ok, streams().open.?(
        g_bound_ctx,
        "/a.bin",
        core.open_write_truncate,
        &file,
        @sizeOf(core.FileState),
    ));
    g_close_rc = core.err_invalid_state;
    try expectEqual(core.err_invalid_state, streams().close.?(g_bound_ctx, &file));
    try expectEqual(@as(?*anyopaque, null), file.native);
}

test "stream: sync is deliberately absent" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    try expectEqual(@as(?*const fn (?*anyopaque, ?*anyopaque) callconv(.c) u16, null), streams().sync);
    try expectEqual(@as(u32, 0), g_bound_caps.flags & core.cap_file_sync);
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
        g_bound_ctx,
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
        g_bound_ctx,
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
        g_bound_ctx,
        &txn,
        @sizeOf(core.TransactionState),
        "/a",
        core.txn_replace_atomic,
    ));
    try expectEqual(@as(u32, 0), g_stat_calls);
}

test "begin: an existing destination is refused and never staged" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    _ = addNode("ram:/books/new.cbz", false, "old");
    var txn: core.TransactionState = undefined;
    try expectEqual(core.err_exists, beginTransaction(&txn));
    try expectEqual(@as(u32, 0), g_open_calls);
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
    try expectEqual(core.fs_mode_write, g_last_open_mode);
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
        txns().write.?(g_bound_ctx, &txn, "x", 1, &written),
    );
    try expectEqual(core.err_invalid_state, txns().seek.?(g_bound_ctx, &txn, 0));
}

test "seek: an offset past the stage length is invalid_size" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    var written: u32 = 0;
    try expectEqual(core.ok, txns().write.?(g_bound_ctx, &txn, "abcd", 4, &written));
    try expectEqual(core.err_invalid_size, txns().seek.?(g_bound_ctx, &txn, 5));
    try expectEqual(core.ok, txns().seek.?(g_bound_ctx, &txn, 4));
    try expectEqual(core.ok, txns().seek.?(g_bound_ctx, &txn, 0));
}

test "validate: the writer is closed, the stage reopened read-only, then closed" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    var written: u32 = 0;
    try expectEqual(core.ok, txns().write.?(g_bound_ctx, &txn, "payload", 7, &written));
    var validator = Validator{};
    try expectEqual(
        core.ok,
        txns().validate.?(g_bound_ctx, &txn, &Validator.run, &validator),
    );
    try expectEqual(@as(u32, 1), validator.calls);
    try expectEqual(@as(u32, 7), validator.read_bytes);
    try expectEqual(core.fs_mode_read, g_last_open_mode);
    try expect(!txn.writer_open);
    try expectEqual(@as(?*anyopaque, null), txn.file_state.native);
}

test "validate: a staged read failure is returned after the reader closes" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    var written: u32 = 0;
    try expectEqual(core.ok, txns().write.?(g_bound_ctx, &txn, "payload", 7, &written));
    var validator = Validator{};
    g_read_rc = core.err_invalid_state;
    try expectEqual(
        core.err_invalid_state,
        txns().validate.?(g_bound_ctx, &txn, &Validator.run, &validator),
    );
    try expectEqual(@as(u32, 1), validator.calls);
    try expectEqual(@as(u32, 0), validator.read_bytes);
    try expectEqual(@as(u32, 2), g_close_calls);
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
        txns().validate.?(g_bound_ctx, &txn, &Validator.run, &validator),
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
        txns().validate.?(g_bound_ctx, &txn, &Validator.run, &validator),
    );
    try expectEqual(@as(u32, 0), validator.calls);
}

test "commit: a validated stage is published by a no-replace rename" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    var written: u32 = 0;
    try expectEqual(core.ok, txns().write.?(g_bound_ctx, &txn, "data", 4, &written));
    var validator = Validator{};
    try expectEqual(
        core.ok,
        txns().validate.?(g_bound_ctx, &txn, &Validator.run, &validator),
    );
    var published: bool = false;
    try expectEqual(core.ok, txns().commit.?(g_bound_ctx, &txn, &published));
    try expect(published);
    try expect(!txn.stage_exists);
    try expect(pathEq(&g_last_path_b, "ram:/books/new.cbz"));
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
        txns().commit.?(g_bound_ctx, &txn, &published),
    );
    try expectEqual(@as(u32, 0), g_rename_calls);
}

test "commit: a failed rename leaves the transaction abortable" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    var validator = Validator{};
    try expectEqual(
        core.ok,
        txns().validate.?(g_bound_ctx, &txn, &Validator.run, &validator),
    );
    g_rename_rc = core.err_exists;
    var published: bool = false;
    try expectEqual(core.err_exists, txns().commit.?(g_bound_ctx, &txn, &published));
    try expect(!published);
    try expect(txn.stage_exists);
}

test "abort: the writer is closed and the stage removed" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    try expectEqual(core.ok, txns().abort.?(g_bound_ctx, &txn));
    try expect(!txn.writer_open);
    try expect(!txn.stage_exists);
    try expectEqual(@as(u32, 1), g_unlink_calls);
    try expect(findNode("ram:/books/TX000001.TMP") == null);
}

test "abort: the first failure wins and only released resources are cleared" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = undefined;
    try expectEqual(core.ok, beginTransaction(&txn));
    g_close_rc = core.err_invalid_state;
    g_unlink_rc = core.err_not_supported;
    try expectEqual(core.err_invalid_state, txns().abort.?(g_bound_ctx, &txn));
    try expect(!txn.writer_open);
    try expect(txn.stage_exists);
}

test "abort: a transaction with nothing owned is a clean no-op" {
    var fixture = Fixture{};
    try expectEqual(core.ok, mountedFat(&fixture, false));
    var txn: core.TransactionState = std.mem.zeroes(core.TransactionState);
    try expectEqual(core.ok, txns().abort.?(g_bound_ctx, &txn));
    try expectEqual(@as(u32, 0), g_unlink_calls);
    try expectEqual(@as(u32, 0), g_close_calls);
}
