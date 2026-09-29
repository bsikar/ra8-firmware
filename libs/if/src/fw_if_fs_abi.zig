//! C ABI surface of the portable filesystem interface (`libs/if`).
//!
//! The public headers are unchanged: this file exports the same twenty-seven
//! `fw_fs_*` symbols the two C translation units did, with the same guard
//! order, the same codes and the same handle mutation. Every decision lives
//! in `internal/root.zig`; what stays here is pointer nullability, the three
//! private backend vtables and the writes into caller-owned handles.
//!
//! There are no external symbols: this interface is pure dispatch over a
//! caller-supplied vtable, so nothing below it is a link-time dependency.

const std = @import("std");
pub const core = @import("internal/root.zig");

const Err = core.Err;

// ---------------------------------------------------------------------------
// The three private backend vtables (fw_if_fs_backend.h).
// ---------------------------------------------------------------------------

/// struct fw_fs_namespace_iface.
pub const NamespaceIface = extern struct {
    stat: ?*const fn (?*anyopaque, [*:0]const u8, *core.Stat) callconv(.c) Err = null,
    listdir: ?*const fn (
        ?*anyopaque,
        [*:0]const u8,
        u32,
        core.ListFn,
        ?*anyopaque,
        *u32,
        *bool,
    ) callconv(.c) Err = null,
    dir_open: ?*const fn (?*anyopaque, [*:0]const u8, ?*anyopaque, u32) callconv(.c) Err = null,
    dir_next: ?*const fn (
        ?*anyopaque,
        ?*anyopaque,
        *core.DirentValue,
        *bool,
    ) callconv(.c) Err = null,
    dir_close: ?*const fn (?*anyopaque, ?*anyopaque) callconv(.c) Err = null,
    mkdir: ?*const fn (?*anyopaque, [*:0]const u8) callconv(.c) Err = null,
    unlink: ?*const fn (?*anyopaque, [*:0]const u8) callconv(.c) Err = null,
    rmdir: ?*const fn (?*anyopaque, [*:0]const u8) callconv(.c) Err = null,
    rename: ?*const fn (?*anyopaque, [*:0]const u8, [*:0]const u8, bool) callconv(.c) Err = null,
    space: ?*const fn (?*anyopaque, *core.Space) callconv(.c) Err = null,
};

/// struct fw_fs_stream_iface.
pub const StreamIface = extern struct {
    open: ?*const fn (?*anyopaque, [*:0]const u8, u8, ?*anyopaque, u32) callconv(.c) Err = null,
    read: ?*const fn (?*anyopaque, ?*anyopaque, [*]u8, u32, *u32) callconv(.c) Err = null,
    write: ?*const fn (?*anyopaque, ?*anyopaque, [*]const u8, u32, *u32) callconv(.c) Err = null,
    seek: ?*const fn (?*anyopaque, ?*anyopaque, u64) callconv(.c) Err = null,
    tell: ?*const fn (?*anyopaque, ?*anyopaque, *u64) callconv(.c) Err = null,
    size: ?*const fn (?*anyopaque, ?*anyopaque, *u64) callconv(.c) Err = null,
    sync: ?*const fn (?*anyopaque, ?*anyopaque) callconv(.c) Err = null,
    close: ?*const fn (?*anyopaque, ?*anyopaque) callconv(.c) Err = null,
};

/// fw_fs_validate_fn_t.
pub const ValidateFn = ?*const fn (?*anyopaque, *File) callconv(.c) Err;

/// struct fw_fs_transaction_iface.
pub const TransactionIface = extern struct {
    begin: ?*const fn (
        ?*anyopaque,
        ?*anyopaque,
        u32,
        [*:0]const u8,
        u8,
    ) callconv(.c) Err = null,
    write: ?*const fn (?*anyopaque, ?*anyopaque, [*]const u8, u32, *u32) callconv(.c) Err = null,
    seek: ?*const fn (?*anyopaque, ?*anyopaque, u64) callconv(.c) Err = null,
    validate: ?*const fn (?*anyopaque, ?*anyopaque, ValidateFn, ?*anyopaque) callconv(.c) Err = null,
    commit: ?*const fn (?*anyopaque, ?*anyopaque, *bool) callconv(.c) Err = null,
    abort: ?*const fn (?*anyopaque, ?*anyopaque) callconv(.c) Err = null,
};

// ---------------------------------------------------------------------------
// Caller-owned facades and handles (fw_if_fs_types.h).
// ---------------------------------------------------------------------------

/// fw_fs_namespace_t.
pub const Namespace = extern struct {
    iface: ?*const NamespaceIface = null,
    ctx: ?*anyopaque = null,
    caps: core.Caps = .{},
};

/// fw_fs_stream_port_t.
pub const StreamPort = extern struct {
    iface: ?*const StreamIface = null,
    ctx: ?*anyopaque = null,
    caps: core.Caps = .{},
};

/// fw_fs_transaction_port_t.
pub const TransactionPort = extern struct {
    iface: ?*const TransactionIface = null,
    ctx: ?*anyopaque = null,
    caps: core.Caps = .{},
};

/// fw_fs_t.
pub const Fs = extern struct {
    names: Namespace = .{},
    streams: StreamPort = .{},
    transactions: TransactionPort = .{},
    caps: core.Caps = .{},
};

/// fw_fs_file_t.
pub const File = extern struct {
    iface: ?*const StreamIface = null,
    ctx: ?*anyopaque = null,
    state: ?*anyopaque = null,
    state_bytes: u32 = 0,
    is_open: bool = false,
};

/// fw_fs_dir_t.
pub const Dir = extern struct {
    iface: ?*const NamespaceIface = null,
    ctx: ?*anyopaque = null,
    state: ?*anyopaque = null,
    state_bytes: u32 = 0,
    caps: core.Caps = .{},
    is_open: bool = false,
};

/// fw_fs_transaction_t.
pub const Transaction = extern struct {
    iface: ?*const TransactionIface = null,
    ctx: ?*anyopaque = null,
    state: ?*anyopaque = null,
    state_bytes: u32 = 0,
    active: bool = false,
    validated: bool = false,
};

comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(@sizeOf(core.Caps) == 40);
    std.debug.assert(@offsetOf(core.Caps, "path_max_bytes") == 24);
    std.debug.assert(@offsetOf(core.Caps, "file_workspace_align") == 32);
    std.debug.assert(@sizeOf(core.Datetime) == 16);
    std.debug.assert(@sizeOf(core.Timestamp) == 20);
    std.debug.assert(@offsetOf(core.Timestamp, "valid") == 16);
    std.debug.assert(@sizeOf(core.Stat) == 72);
    std.debug.assert(@offsetOf(core.Stat, "created") == 8);
    std.debug.assert(@offsetOf(core.Stat, "node_type") == 68);
    const dirent_size_offset = std.mem.alignForward(usize, ptr, 8);
    std.debug.assert(@offsetOf(core.Dirent, "size_bytes") == dirent_size_offset);
    std.debug.assert(@sizeOf(core.Dirent) == std.mem.alignForward(usize, dirent_size_offset + 11, 8));
    std.debug.assert(@offsetOf(core.DirentValue, "size_bytes") == 512);
    std.debug.assert(@sizeOf(core.DirentValue) == 528);
    std.debug.assert(@sizeOf(core.Space) == 24);
    std.debug.assert(@sizeOf(NamespaceIface) == ptr * 10);
    std.debug.assert(@sizeOf(StreamIface) == ptr * 8);
    std.debug.assert(@sizeOf(TransactionIface) == ptr * 6);
    std.debug.assert(@sizeOf(ValidateFn) == ptr);
    std.debug.assert(@sizeOf(Namespace) == ptr * 2 + 40);
    std.debug.assert(@offsetOf(Namespace, "caps") == ptr * 2);
    std.debug.assert(@sizeOf(Fs) == 3 * (ptr * 2 + 40) + 40);
    std.debug.assert(@offsetOf(File, "state_bytes") == ptr * 3);
    std.debug.assert(@sizeOf(File) == std.mem.alignForward(usize, ptr * 3 + 5, ptr));
    const dir_caps_offset = std.mem.alignForward(usize, ptr * 3 + 4, 8);
    std.debug.assert(@offsetOf(Dir, "caps") == dir_caps_offset);
    std.debug.assert(@sizeOf(Dir) == std.mem.alignForward(usize, dir_caps_offset + 41, 8));
    std.debug.assert(@sizeOf(Transaction) == std.mem.alignForward(usize, ptr * 3 + 6, ptr));
}

// ---------------------------------------------------------------------------
// Shared guards.
// ---------------------------------------------------------------------------

fn addressOf(pointer: ?*anyopaque) usize {
    return if (pointer) |value| @intFromPtr(value) else 0;
}

/// internal_names / internal_cursor_names.
fn namesStatus(names: ?*const Namespace) Err {
    const bound = names orelse return core.err_null_ptr;
    return if (bound.iface == null) core.err_not_initialized else core.ok;
}

/// internal_file.
fn fileStatus(file: ?*const File) Err {
    const handle = file orelse return core.err_null_ptr;
    if (!handle.is_open) return core.err_invalid_state;
    return if (handle.iface == null) core.err_not_initialized else core.ok;
}

/// internal_cursor_handle.
fn dirStatus(directory: ?*const Dir) Err {
    const handle = directory orelse return core.err_null_ptr;
    if (!handle.is_open) return core.err_invalid_state;
    return if (handle.iface == null) core.err_not_initialized else core.ok;
}

/// internal_transaction.
fn transactionStatus(transaction: ?*const Transaction) Err {
    const handle = transaction orelse return core.err_null_ptr;
    if (!handle.active) return core.err_invalid_state;
    return if (handle.iface == null) core.err_not_initialized else core.ok;
}

fn validatePath(caps: *const core.Caps, path: ?[*:0]const u8) Err {
    const bytes = path orelse return core.err_null_ptr;
    return core.pathValidate(caps, bytes);
}

// ---------------------------------------------------------------------------
// Binding and capabilities.
// ---------------------------------------------------------------------------

pub export fn fw_fs_path_validate(caps: ?*const core.Caps, path: ?[*:0]const u8) callconv(.c) Err {
    const limits = caps orelse return core.err_null_ptr;
    const bytes = path orelse return core.err_null_ptr;
    return core.pathValidate(limits, bytes);
}

pub export fn fw_fs_bind(
    out: ?*Fs,
    namespace_iface: ?*const NamespaceIface,
    stream_iface: ?*const StreamIface,
    transaction_iface: ?*const TransactionIface,
    ctx: ?*anyopaque,
    caps: ?*const core.Caps,
) callconv(.c) Err {
    const target = out orelse return core.err_null_ptr;
    const names = namespace_iface orelse return core.err_null_ptr;
    const streams = stream_iface orelse return core.err_null_ptr;
    if (ctx == null) return core.err_null_ptr;
    const limits = caps orelse return core.err_null_ptr;

    const interfaces = core.interfacesStatus(
        namespacePresence(names),
        streamPresence(streams),
        if (transaction_iface) |table| transactionPresence(table) else null,
    );
    if (interfaces != core.ok) return interfaces;

    const caps_status = core.capsStatus(
        limits,
        names.space != null,
        streams.sync != null,
        transaction_iface != null,
    );
    if (caps_status != core.ok) return caps_status;

    const root_path = core.pathValidate(limits, "/");
    if (root_path != core.ok) return root_path;

    target.caps = limits.*;
    target.names.iface = names;
    target.names.ctx = ctx;
    target.names.caps = limits.*;
    target.streams.iface = streams;
    target.streams.ctx = ctx;
    target.streams.caps = limits.*;
    target.transactions.iface = transaction_iface;
    target.transactions.ctx = ctx;
    target.transactions.caps = limits.*;
    return core.ok;
}

fn namespacePresence(table: *const NamespaceIface) core.NamespacePresence {
    return .{
        .stat = table.stat != null,
        .listdir = table.listdir != null,
        .dir_open = table.dir_open != null,
        .dir_next = table.dir_next != null,
        .dir_close = table.dir_close != null,
        .mkdir = table.mkdir != null,
        .unlink = table.unlink != null,
        .rmdir = table.rmdir != null,
        .rename = table.rename != null,
        .space = table.space != null,
    };
}

fn streamPresence(table: *const StreamIface) core.StreamPresence {
    return .{
        .open = table.open != null,
        .read = table.read != null,
        .write = table.write != null,
        .seek = table.seek != null,
        .tell = table.tell != null,
        .size = table.size != null,
        .sync = table.sync != null,
        .close = table.close != null,
    };
}

fn transactionPresence(table: *const TransactionIface) core.TransactionPresence {
    return .{
        .begin = table.begin != null,
        .write = table.write != null,
        .seek = table.seek != null,
        .validate = table.validate != null,
        .commit = table.commit != null,
        .abort = table.abort != null,
    };
}

pub export fn fw_fs_get_caps(fs: ?*const Fs, out: ?*core.Caps) callconv(.c) Err {
    const bound = fs orelse return core.err_null_ptr;
    const target = out orelse return core.err_null_ptr;
    if (bound.names.iface == null) return core.err_not_initialized;
    target.* = bound.caps;
    return core.ok;
}

// ---------------------------------------------------------------------------
// Namespace operations.
// ---------------------------------------------------------------------------

pub export fn fw_fs_stat(
    names: ?*const Namespace,
    path: ?[*:0]const u8,
    out: ?*core.Stat,
) callconv(.c) Err {
    const valid = namesStatus(names);
    if (valid != core.ok) return valid;
    const bound = names.?;
    const target = out orelse return core.err_null_ptr;
    const path_err = validatePath(&bound.caps, path);
    if (path_err != core.ok) return path_err;

    target.* = .{};
    const result = bound.iface.?.stat.?(bound.ctx, path.?, target);
    if (result == core.ok) {
        if (core.statIncoherent(target)) {
            target.* = .{};
            return core.err_invalid_state;
        }
    }
    return result;
}

pub export fn fw_fs_listdir(
    names: ?*const Namespace,
    path: ?[*:0]const u8,
    max_entries: u32,
    callback: core.ListFn,
    callback_ctx: ?*anyopaque,
    out_count: ?*u32,
    out_complete: ?*u8,
) callconv(.c) Err {
    const valid = namesStatus(names);
    if (valid != core.ok) return valid;
    const bound = names.?;
    if (callback == null) return core.err_null_ptr;
    const count = out_count orelse return core.err_null_ptr;
    const complete = out_complete orelse return core.err_null_ptr;
    if (max_entries == 0) return core.err_invalid_arg;
    const path_err = validatePath(&bound.caps, path);
    if (path_err != core.ok) return path_err;

    count.* = 0;
    complete.* = 0;
    var complete_value = false;
    const result = bound.iface.?.listdir.?(
        bound.ctx,
        path.?,
        max_entries,
        callback,
        callback_ctx,
        count,
        &complete_value,
    );
    complete.* = @intFromBool(complete_value);
    if (count.* > max_entries) {
        count.* = 0;
        complete.* = 0;
        return core.err_invalid_state;
    }
    return result;
}

const NameOp = ?*const fn (?*anyopaque, [*:0]const u8) callconv(.c) Err;

/// internal_name_op.
fn nameOp(names: ?*const Namespace, path: ?[*:0]const u8, operation: NameOp) Err {
    const valid = namesStatus(names);
    if (valid != core.ok) return valid;
    const bound = names.?;
    const call = operation orelse return core.err_not_supported;
    const path_err = validatePath(&bound.caps, path);
    if (path_err != core.ok) return path_err;
    if (core.isRoot(path.?)) return core.err_access_denied;
    return call(bound.ctx, path.?);
}

pub export fn fw_fs_mkdir(names: ?*const Namespace, path: ?[*:0]const u8) callconv(.c) Err {
    const valid = namesStatus(names);
    if (valid != core.ok) return valid;
    return nameOp(names, path, names.?.iface.?.mkdir);
}

pub export fn fw_fs_unlink(names: ?*const Namespace, path: ?[*:0]const u8) callconv(.c) Err {
    const valid = namesStatus(names);
    if (valid != core.ok) return valid;
    return nameOp(names, path, names.?.iface.?.unlink);
}

pub export fn fw_fs_rmdir(names: ?*const Namespace, path: ?[*:0]const u8) callconv(.c) Err {
    const valid = namesStatus(names);
    if (valid != core.ok) return valid;
    return nameOp(names, path, names.?.iface.?.rmdir);
}

pub export fn fw_fs_rename(
    names: ?*const Namespace,
    old_path: ?[*:0]const u8,
    new_path: ?[*:0]const u8,
    replace: u8,
) callconv(.c) Err {
    const valid = namesStatus(names);
    if (valid != core.ok) return valid;
    const bound = names.?;
    const replace_existing = replace != 0;
    const capability = core.renameCapabilityStatus(bound.caps.flags, replace_existing);
    if (capability != core.ok) return capability;
    const old_err = validatePath(&bound.caps, old_path);
    if (old_err != core.ok) return old_err;
    const new_err = validatePath(&bound.caps, new_path);
    if (new_err != core.ok) return new_err;
    if (core.isRoot(old_path.?) or core.isRoot(new_path.?)) return core.err_access_denied;
    return bound.iface.?.rename.?(bound.ctx, old_path.?, new_path.?, replace_existing);
}

pub export fn fw_fs_space(names: ?*const Namespace, out: ?*core.Space) callconv(.c) Err {
    const valid = namesStatus(names);
    if (valid != core.ok) return valid;
    const bound = names.?;
    const target = out orelse return core.err_null_ptr;
    if ((bound.caps.flags & core.cap_space_query) == 0) return core.err_not_supported;
    const call = bound.iface.?.space orelse return core.err_not_supported;

    target.* = .{};
    const result = call(bound.ctx, target);
    if ((result == core.ok) and core.spaceIncoherent(target)) {
        target.* = .{};
        return core.err_invalid_state;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Incremental directory cursors.
// ---------------------------------------------------------------------------

pub export fn fw_fs_dir_open(
    names: ?*const Namespace,
    path: ?[*:0]const u8,
    directory: ?*Dir,
    workspace: ?*anyopaque,
    workspace_size: u32,
) callconv(.c) Err {
    const valid = namesStatus(names);
    if (valid != core.ok) return valid;
    const bound = names.?;
    const handle = directory orelse return core.err_null_ptr;
    if (handle.is_open) return core.err_busy;
    const path_err = validatePath(&bound.caps, path);
    if (path_err != core.ok) return path_err;
    const work = core.workspace(
        addressOf(workspace),
        workspace_size,
        bound.caps.directory_workspace_bytes,
        bound.caps.directory_workspace_align,
    );
    if (work != core.ok) return work;

    const opened = bound.iface.?.dir_open.?(bound.ctx, path.?, workspace, workspace_size);
    if (opened != core.ok) return opened;

    handle.* = .{
        .iface = bound.iface,
        .ctx = bound.ctx,
        .state = workspace,
        .state_bytes = workspace_size,
        .caps = bound.caps,
        .is_open = true,
    };
    return core.ok;
}

pub export fn fw_fs_dir_next(
    directory: ?*Dir,
    out: ?*core.DirentValue,
    out_entry: ?*u8,
) callconv(.c) Err {
    const valid = dirStatus(directory);
    if (valid != core.ok) return valid;
    const handle = directory.?;
    const target = out orelse return core.err_null_ptr;
    const present_out = out_entry orelse return core.err_null_ptr;

    target.* = .{};
    present_out.* = 0;
    var candidate: core.DirentValue = .{};
    var present: bool = false;
    const result = handle.iface.?.dir_next.?(handle.ctx, handle.state, &candidate, &present);
    if ((result != core.ok) or !present) return result;

    const coherent = core.entryStatus(&handle.caps, &candidate);
    if (coherent != core.ok) return coherent;

    target.* = candidate;
    present_out.* = 1;
    return core.ok;
}

pub export fn fw_fs_dir_close(directory: ?*Dir) callconv(.c) Err {
    const valid = dirStatus(directory);
    if (valid != core.ok) return valid;
    const handle = directory.?;
    const closed = handle.iface.?.dir_close.?(handle.ctx, handle.state);
    handle.* = .{};
    return closed;
}

// ---------------------------------------------------------------------------
// Byte streams.
// ---------------------------------------------------------------------------

pub export fn fw_fs_open(
    streams: ?*const StreamPort,
    path: ?[*:0]const u8,
    mode: u8,
    file: ?*File,
    workspace: ?*anyopaque,
    workspace_size: u32,
) callconv(.c) Err {
    const port = streams orelse return core.err_null_ptr;
    const handle = file orelse return core.err_null_ptr;
    const table = port.iface orelse return core.err_not_initialized;
    if (handle.is_open) return core.err_busy;
    const mode_status = core.openModeStatus(mode, port.caps.flags);
    if (mode_status != core.ok) return mode_status;
    const path_err = validatePath(&port.caps, path);
    if (path_err != core.ok) return path_err;
    if (core.isRoot(path.?)) return core.err_invalid_arg;
    const work = core.workspace(
        addressOf(workspace),
        workspace_size,
        port.caps.file_workspace_bytes,
        port.caps.file_workspace_align,
    );
    if (work != core.ok) return work;

    const opened = table.open.?(port.ctx, path.?, mode, workspace, workspace_size);
    if (opened != core.ok) return opened;

    handle.iface = table;
    handle.ctx = port.ctx;
    handle.state = workspace;
    handle.state_bytes = workspace_size;
    handle.is_open = true;
    return core.ok;
}

pub export fn fw_fs_read(
    file: ?*File,
    dst: ?[*]u8,
    cap: u32,
    out_read: ?*u32,
) callconv(.c) Err {
    const valid = fileStatus(file);
    if (valid != core.ok) return valid;
    const handle = file.?;
    const buffer = dst orelse return core.err_null_ptr;
    const read_out = out_read orelse return core.err_null_ptr;

    read_out.* = 0;
    const result = handle.iface.?.read.?(handle.ctx, handle.state, buffer, cap, read_out);
    if (read_out.* > cap) {
        read_out.* = 0;
        return core.err_invalid_state;
    }
    return result;
}

pub export fn fw_fs_write(
    file: ?*File,
    source: ?[*]const u8,
    length: u32,
    out_written: ?*u32,
) callconv(.c) Err {
    const valid = fileStatus(file);
    if (valid != core.ok) return valid;
    const handle = file.?;
    const buffer = source orelse return core.err_null_ptr;
    const written_out = out_written orelse return core.err_null_ptr;

    written_out.* = 0;
    const result = handle.iface.?.write.?(handle.ctx, handle.state, buffer, length, written_out);
    if (written_out.* > length) {
        written_out.* = 0;
        return core.err_invalid_state;
    }
    return result;
}

pub export fn fw_fs_seek(file: ?*File, absolute_offset: u64) callconv(.c) Err {
    const valid = fileStatus(file);
    if (valid != core.ok) return valid;
    const handle = file.?;
    return handle.iface.?.seek.?(handle.ctx, handle.state, absolute_offset);
}

pub export fn fw_fs_tell(file: ?*File, out_offset: ?*u64) callconv(.c) Err {
    const valid = fileStatus(file);
    if (valid != core.ok) return valid;
    const handle = file.?;
    const target = out_offset orelse return core.err_null_ptr;
    target.* = 0;
    return handle.iface.?.tell.?(handle.ctx, handle.state, target);
}

pub export fn fw_fs_file_size(file: ?*File, out_size: ?*u64) callconv(.c) Err {
    const valid = fileStatus(file);
    if (valid != core.ok) return valid;
    const handle = file.?;
    const target = out_size orelse return core.err_null_ptr;
    target.* = 0;
    return handle.iface.?.size.?(handle.ctx, handle.state, target);
}

pub export fn fw_fs_sync(file: ?*File) callconv(.c) Err {
    const valid = fileStatus(file);
    if (valid != core.ok) return valid;
    const handle = file.?;
    const call = handle.iface.?.sync orelse return core.err_not_supported;
    return call(handle.ctx, handle.state);
}

pub export fn fw_fs_close(file: ?*File) callconv(.c) Err {
    const valid = fileStatus(file);
    if (valid != core.ok) return valid;
    const handle = file.?;
    const closed = handle.iface.?.close.?(handle.ctx, handle.state);
    handle.iface = null;
    handle.ctx = null;
    handle.state = null;
    handle.state_bytes = 0;
    handle.is_open = false;
    return closed;
}

// ---------------------------------------------------------------------------
// Staged publication.
// ---------------------------------------------------------------------------

pub export fn fw_fs_transaction_begin(
    port: ?*const TransactionPort,
    destination: ?[*:0]const u8,
    policy: u8,
    transaction: ?*Transaction,
    workspace: ?*anyopaque,
    workspace_size: u32,
) callconv(.c) Err {
    const bound = port orelse return core.err_null_ptr;
    const handle = transaction orelse return core.err_null_ptr;

    const preamble = core.transactionPreamble(
        bound.caps.flags,
        bound.iface != null,
        handle.active,
        policy,
    );
    if (preamble != core.ok) return preamble;

    const path_err = validatePath(&bound.caps, destination);
    if (path_err != core.ok) return path_err;
    if (core.isRoot(destination.?)) return core.err_invalid_arg;

    const work = core.workspace(
        addressOf(workspace),
        workspace_size,
        bound.caps.transaction_workspace_bytes,
        bound.caps.transaction_workspace_align,
    );
    if (work != core.ok) return work;

    const begun = bound.iface.?.begin.?(
        bound.ctx,
        workspace,
        workspace_size,
        destination.?,
        policy,
    );
    if (begun != core.ok) return begun;

    handle.iface = bound.iface;
    handle.ctx = bound.ctx;
    handle.state = workspace;
    handle.state_bytes = workspace_size;
    handle.active = true;
    handle.validated = false;
    return core.ok;
}

pub export fn fw_fs_transaction_write(
    transaction: ?*Transaction,
    source: ?[*]const u8,
    length: u32,
    out_written: ?*u32,
) callconv(.c) Err {
    const valid = transactionStatus(transaction);
    if (valid != core.ok) return valid;
    const handle = transaction.?;
    const buffer = source orelse return core.err_null_ptr;
    const written_out = out_written orelse return core.err_null_ptr;
    if (handle.validated) return core.err_invalid_state;

    written_out.* = 0;
    const result = handle.iface.?.write.?(handle.ctx, handle.state, buffer, length, written_out);
    if (written_out.* > length) {
        written_out.* = 0;
        return core.err_invalid_state;
    }
    return result;
}

pub export fn fw_fs_transaction_seek(
    transaction: ?*Transaction,
    absolute_offset: u64,
) callconv(.c) Err {
    const valid = transactionStatus(transaction);
    if (valid != core.ok) return valid;
    const handle = transaction.?;
    if (handle.validated) return core.err_invalid_state;
    return handle.iface.?.seek.?(handle.ctx, handle.state, absolute_offset);
}

pub export fn fw_fs_transaction_validate(
    transaction: ?*Transaction,
    validator: ValidateFn,
    validator_ctx: ?*anyopaque,
) callconv(.c) Err {
    const valid = transactionStatus(transaction);
    if (valid != core.ok) return valid;
    const handle = transaction.?;
    if (validator == null) return core.err_null_ptr;
    if (handle.validated) return core.err_invalid_state;

    const checked = handle.iface.?.validate.?(
        handle.ctx,
        handle.state,
        validator,
        validator_ctx,
    );
    if (checked == core.ok) handle.validated = true;
    return checked;
}

pub export fn fw_fs_transaction_commit(
    transaction: ?*Transaction,
    out_published: ?*u8,
) callconv(.c) Err {
    const valid = transactionStatus(transaction);
    if (valid != core.ok) return valid;
    const handle = transaction.?;
    const published = out_published orelse return core.err_null_ptr;
    if (!handle.validated) return core.err_invalid_state;

    published.* = 0;
    var published_value = false;
    const result = handle.iface.?.commit.?(handle.ctx, handle.state, &published_value);
    published.* = @intFromBool(published_value);
    if ((result == core.ok) and !published_value) return core.err_invalid_state;
    if (published_value) {
        handle.active = false;
        handle.validated = false;
    }
    return result;
}

pub export fn fw_fs_transaction_abort(transaction: ?*Transaction) callconv(.c) Err {
    const valid = transactionStatus(transaction);
    if (valid != core.ok) return valid;
    const handle = transaction.?;
    const result = handle.iface.?.abort.?(handle.ctx, handle.state);
    if (result == core.ok) {
        handle.active = false;
        handle.validated = false;
    }
    return result;
}
