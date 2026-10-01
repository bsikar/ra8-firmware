//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The fake VFS medium every `if_ra8_vfs` test runs against, every C symbol
//! the adapter links against, and the mount fixture the tests drive. The
//! medium is a fixed array of nodes with per-call return-code overrides and
//! call counters, so each test can force one native failure and observe
//! exactly what the adapter did with it.
//!
//! Every seam below the adapter is exported here, so the vtables exercise the
//! same link-time substitution the C suite gets from the real `ra8_io_vfs` and
//! `ra8_fs`. `fw_fs_bind` captures what the adapter published, which is how
//! the tests reach the otherwise file-private vtables. No test blocks live
//! here: each test root imports this module and drives it.

const std = @import("std");
const abi = @import("abi");
const core = abi.core;

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

pub var g_stat_rc: u16 = 0;
var g_open_rc: u16 = 0;
pub var g_rename_rc: u16 = 0;
var g_mkdir_rc: u16 = 0;
pub var g_unlink_rc: u16 = 0;
var g_rmdir_rc: u16 = 0;
pub var g_listdir_rc: u16 = 0;
pub var g_space_rc: u16 = 0;
pub var g_requirements_rc: u16 = 0;
var g_dir_open_rc: u16 = 0;
var g_dir_close_rc: u16 = 0;
pub var g_read_rc: u16 = 0;
pub var g_write_rc: u16 = 0;
var g_seek_rc: u16 = 0;
pub var g_close_rc: u16 = 0;
pub var g_bind_rc: u16 = 0;

pub var g_req_bytes: u32 = 640;
pub var g_req_align: u8 = 8;
var g_req_max_open: u16 = 2;

pub var g_last_path: [core.full_path_cap]u8 = undefined;
pub var g_last_path_b: [core.full_path_cap]u8 = undefined;
pub var g_last_dir_workspace: usize = 0;
pub var g_last_dir_bytes: u32 = 0;
pub var g_last_open_mode: u8 = 0xFF;

pub var g_stat_calls: u32 = 0;
pub var g_open_calls: u32 = 0;
pub var g_rename_calls: u32 = 0;
pub var g_unlink_calls: u32 = 0;
pub var g_close_calls: u32 = 0;
pub var g_mkdir_calls: u32 = 0;
pub var g_rmdir_calls: u32 = 0;

var g_bound_names: ?*const abi.NamespaceIface = null;
var g_bound_stream: ?*const abi.StreamIface = null;
var g_bound_txn: ?*const abi.TransactionIface = null;
pub var g_bound_ctx: ?*anyopaque = null;
pub var g_bound_caps: core.Caps = .{};
pub var g_bind_calls: u32 = 0;

var g_dir_cursor: usize = 0;

fn record(dst: *[core.full_path_cap]u8, path: [*:0]const u8) void {
    const length = core.len(path, core.full_path_cap - 1);
    @memset(dst, 0);
    @memcpy(dst[0..length], path[0..length]);
}

pub fn pathEq(dst: *const [core.full_path_cap]u8, want: []const u8) bool {
    return std.mem.eql(u8, dst[0..want.len], want) and dst[want.len] == 0;
}

pub fn findNode(path: [*:0]const u8) ?usize {
    const length = core.len(path, node_name_cap);
    for (&g_nodes, 0..) |*node, index| {
        if (!node.used) continue;
        if (core.len(&node.name, node_name_cap) != length) continue;
        if (std.mem.eql(u8, node.name[0..length], path[0..length])) return index;
    }
    return null;
}

pub fn addNode(name: []const u8, is_dir: bool, payload: []const u8) usize {
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

pub fn resetMedium() void {
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

pub const Fixture = struct {
    fs: abi.Fs = .{},
    state: abi.State = undefined,
    mount: abi.Mount = .{},
};

pub fn mountedFat(fixture: *Fixture, removable: bool) u16 {
    resetMedium();
    fixture.mount = .{ .in_use = 1, .fs_type = 16 };
    const cfg = abi.Config{
        .mount_name = "ram",
        .mount = &fixture.mount,
        .removable_media = removable,
    };
    return abi.fw_fs_ra8_vfs_init(&fixture.fs, &fixture.state, &cfg);
}

pub fn names() *const abi.NamespaceIface {
    return g_bound_names.?;
}

pub fn streams() *const abi.StreamIface {
    return g_bound_stream.?;
}

pub fn txns() *const abi.TransactionIface {
    return g_bound_txn.?;
}
