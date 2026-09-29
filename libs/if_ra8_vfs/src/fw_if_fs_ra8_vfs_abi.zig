//! C ABI surface of the `fw_if_fs` adapter over one bound `ra8_io_vfs` mount.
//!
//! The public header is unchanged: this file exports `fw_fs_ra8_vfs_init` and
//! nothing else, exactly as the C translation unit did. The three vtables and
//! their operations stay file-private, which is what `static` bought in C.

const std = @import("std");
pub const core = @import("internal/root.zig");

// ---------------------------------------------------------------------------
// Mirrors of the types this adapter is handed across the C boundary.
// ---------------------------------------------------------------------------

/// ra8_fs_backend_t.
pub const Backend = extern struct {
    read_block: ?*const fn (?*anyopaque, u64, u32, [*]u8) callconv(.c) u16 = null,
    write_block: ?*const fn (?*anyopaque, u64, u32, [*]const u8) callconv(.c) u16 = null,
    get_capacity: ?*const fn (?*anyopaque, *u64, *u32) callconv(.c) u16 = null,
    erase_blocks: ?*const fn (?*anyopaque, u64, u64) callconv(.c) u16 = null,
    ctx: ?*anyopaque = null,
};

/// ra8_fs_mount_t. `fs_type` is the C member named `type`.
pub const Mount = extern struct {
    backend: Backend = .{},
    fs_type: u8 = 0,
    bytes_per_sector: u32 = 0,
    sectors_per_cluster: u32 = 0,
    reserved_sectors: u32 = 0,
    num_fats: u32 = 0,
    root_entries: u32 = 0,
    total_sectors: u64 = 0,
    fat_size_sectors: u32 = 0,
    root_cluster: u32 = 0,
    first_fat_lba: u64 = 0,
    first_root_lba: u64 = 0,
    first_data_lba: u64 = 0,
    count_of_clusters: u32 = 0,
    partition_base_lba: u64 = 0,
    in_use: u8 = 0,
    exfat_upcase_ok: u8 = 0,
};

/// fw_fs_namespace_iface.
pub const NamespaceIface = extern struct {
    stat: ?*const fn (?*anyopaque, [*:0]const u8, *core.Stat) callconv(.c) u16,
    listdir: ?*const fn (
        ?*anyopaque,
        [*:0]const u8,
        u32,
        core.ListFn,
        ?*anyopaque,
        *u32,
        *bool,
    ) callconv(.c) u16,
    dir_open: ?*const fn (?*anyopaque, [*:0]const u8, ?*anyopaque, u32) callconv(.c) u16,
    dir_next: ?*const fn (?*anyopaque, ?*anyopaque, *core.DirentValue, *bool) callconv(.c) u16,
    dir_close: ?*const fn (?*anyopaque, ?*anyopaque) callconv(.c) u16,
    mkdir: ?*const fn (?*anyopaque, [*:0]const u8) callconv(.c) u16,
    unlink: ?*const fn (?*anyopaque, [*:0]const u8) callconv(.c) u16,
    rmdir: ?*const fn (?*anyopaque, [*:0]const u8) callconv(.c) u16,
    rename: ?*const fn (?*anyopaque, [*:0]const u8, [*:0]const u8, bool) callconv(.c) u16,
    space: ?*const fn (?*anyopaque, *core.Space) callconv(.c) u16,
};

/// fw_fs_stream_iface.
pub const StreamIface = extern struct {
    open: ?*const fn (?*anyopaque, [*:0]const u8, u8, ?*anyopaque, u32) callconv(.c) u16,
    read: ?*const fn (?*anyopaque, ?*anyopaque, [*]u8, u32, *u32) callconv(.c) u16,
    write: ?*const fn (?*anyopaque, ?*anyopaque, [*]const u8, u32, *u32) callconv(.c) u16,
    seek: ?*const fn (?*anyopaque, ?*anyopaque, u64) callconv(.c) u16,
    tell: ?*const fn (?*anyopaque, ?*anyopaque, *u64) callconv(.c) u16,
    size: ?*const fn (?*anyopaque, ?*anyopaque, *u64) callconv(.c) u16,
    sync: ?*const fn (?*anyopaque, ?*anyopaque) callconv(.c) u16,
    close: ?*const fn (?*anyopaque, ?*anyopaque) callconv(.c) u16,
};

/// fw_fs_transaction_iface.
pub const TransactionIface = extern struct {
    begin: ?*const fn (?*anyopaque, ?*anyopaque, u32, [*:0]const u8, u8) callconv(.c) u16,
    write: ?*const fn (?*anyopaque, ?*anyopaque, [*]const u8, u32, *u32) callconv(.c) u16,
    seek: ?*const fn (?*anyopaque, ?*anyopaque, u64) callconv(.c) u16,
    validate: ?*const fn (?*anyopaque, ?*anyopaque, ValidateFn, ?*anyopaque) callconv(.c) u16,
    commit: ?*const fn (?*anyopaque, ?*anyopaque, *bool) callconv(.c) u16,
    abort: ?*const fn (?*anyopaque, ?*anyopaque) callconv(.c) u16,
};

/// fw_fs_file_t.
pub const File = extern struct {
    iface: ?*const StreamIface = null,
    ctx: ?*anyopaque = null,
    state: ?*anyopaque = null,
    state_bytes: u32 = 0,
    is_open: bool = false,
};

/// fw_fs_validate_fn_t.
pub const ValidateFn = *const fn (?*anyopaque, *File) callconv(.c) u16;

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

/// fw_fs_t, one complete composition-root binding.
pub const Fs = extern struct {
    names: Namespace = .{},
    streams: StreamPort = .{},
    transactions: TransactionPort = .{},
    caps: core.Caps = .{},
};

/// fw_fs_ra8_vfs_cfg_t.
pub const Config = extern struct {
    mount_name: ?[*:0]const u8 = null,
    mount: ?*Mount = null,
    removable_media: bool = false,
};

/// fw_fs_ra8_vfs_state_t, the caller-owned adapter context.
pub const State = extern struct {
    mount_name: [core.io_vfs_name_max]u8,
    path_a: [core.full_path_cap]u8,
    path_b: [core.full_path_cap]u8,
    mount: ?*Mount,
    directory_workspace_bytes: u32,
    transaction_id: u32,
    max_open_directories: u16,
    directory_workspace_align: u8,
    removable_media: bool,
};

// ---------------------------------------------------------------------------
// The seams. Everything below the adapter stays C and substitutes at link time.
// ---------------------------------------------------------------------------

extern fn ra8_io_vfs_stat(path: [*:0]const u8, out: *core.NativeStat) callconv(.c) u16;
extern fn ra8_io_vfs_open(path: [*:0]const u8, mode: u8, out_file: *?*anyopaque) callconv(.c) u16;
extern fn ra8_io_vfs_rename(old_path: [*:0]const u8, new_path: [*:0]const u8) callconv(.c) u16;
extern fn ra8_io_vfs_mkdir(path: [*:0]const u8) callconv(.c) u16;
extern fn ra8_io_vfs_unlink(path: [*:0]const u8) callconv(.c) u16;
extern fn ra8_io_vfs_rmdir(path: [*:0]const u8) callconv(.c) u16;
extern fn ra8_io_vfs_listdir(
    path: [*:0]const u8,
    cb: *const fn ([*:0]const u8, u8, u64, ?*anyopaque) callconv(.c) void,
    ctx: ?*anyopaque,
) callconv(.c) u16;
extern fn ra8_io_vfs_free_space(name: [*:0]const u8, out: *core.NativeSpace) callconv(.c) u16;
extern fn ra8_io_vfs_dir_requirements(
    path: [*:0]const u8,
    out_bytes: *u32,
    out_align: *u8,
    out_max_open: *u16,
) callconv(.c) u16;
extern fn ra8_io_vfs_dir_open(
    path: [*:0]const u8,
    directory: *core.NativeDir,
    workspace: ?*anyopaque,
    workspace_bytes: u32,
) callconv(.c) u16;
extern fn ra8_io_vfs_dir_next(
    directory: *core.NativeDir,
    out: *core.NativeDirent,
    out_entry: *bool,
) callconv(.c) u16;
extern fn ra8_io_vfs_dir_close(directory: *core.NativeDir) callconv(.c) u16;

extern fn ra8_fs_read(file: ?*anyopaque, buf: [*]u8, max_len: u32, got_len: *u32) callconv(.c) u16;
extern fn ra8_fs_write(file: ?*anyopaque, buf: [*]const u8, length: u32) callconv(.c) u16;
extern fn ra8_fs_seek(file: ?*anyopaque, offset_bytes: u64) callconv(.c) u16;
extern fn ra8_fs_tell(file: ?*anyopaque, out_offset: *u64) callconv(.c) u16;
extern fn ra8_fs_size(file: ?*anyopaque, out_bytes: *u64) callconv(.c) u16;
extern fn ra8_fs_close(file: ?*anyopaque) callconv(.c) u16;

extern fn fw_fs_bind(
    out: *Fs,
    namespace_iface: *const NamespaceIface,
    stream_iface: *const StreamIface,
    transaction_iface: *const TransactionIface,
    ctx: ?*anyopaque,
    caps: *const core.Caps,
) callconv(.c) u16;
extern fn fw_fs_close(file: *File) callconv(.c) u16;

// ---------------------------------------------------------------------------
// Namespace operations.
// ---------------------------------------------------------------------------

inline fn adapter(ctx: ?*anyopaque) *State {
    return @ptrCast(@alignCast(ctx.?));
}

inline fn pathA(state: *State) [*:0]const u8 {
    return @ptrCast(&state.path_a);
}

inline fn pathB(state: *State) [*:0]const u8 {
    return @ptrCast(&state.path_b);
}

fn internalStat(ctx: ?*anyopaque, path: [*:0]const u8, out: *core.Stat) callconv(.c) u16 {
    const state = adapter(ctx);
    const built = core.fullPath(&state.mount_name, path, &state.path_a);
    if (built != core.ok) return built;
    var native: core.NativeStat = .{};
    const result = ra8_io_vfs_stat(pathA(state), &native);
    if (result != core.ok) return result;
    out.exists = native.exists;
    out.size_bytes = native.size_bytes;
    out.created = core.timestamp(&native.created);
    out.modified = core.timestamp(&native.modified);
    out.accessed = core.timestamp(&native.accessed);
    out.kind = core.node_none;
    if (native.exists) {
        out.kind = if (native.is_directory) core.node_directory else core.node_file;
    }
    return core.ok;
}

fn dirCursor(directory_state: ?*anyopaque) *core.DirectoryState {
    return @ptrFromInt(core.dirCursorBase(@intFromPtr(directory_state)));
}

fn internalDirOpen(
    ctx: ?*anyopaque,
    path: [*:0]const u8,
    directory_state: ?*anyopaque,
    state_bytes: u32,
) callconv(.c) u16 {
    const state = adapter(ctx);
    const layout = core.dirLayout(@intFromPtr(directory_state), state.directory_workspace_align);
    if (@as(u64, state_bytes) < @as(u64, layout.cursor_end)) return core.err_no_mem;
    const directory: *core.DirectoryState = @ptrFromInt(layout.cursor);
    @memset(std.mem.asBytes(directory), 0);
    const built = core.fullPath(&state.mount_name, path, &state.path_a);
    if (built != core.ok) return built;
    if (layout.consumed > @as(usize, state_bytes)) return core.err_no_mem;
    if (@as(u64, state.directory_workspace_bytes) >
        (@as(u64, state_bytes) - @as(u64, layout.consumed)))
    {
        return core.err_no_mem;
    }
    const workspace: ?*anyopaque = @ptrFromInt(layout.workspace);
    return ra8_io_vfs_dir_open(
        pathA(state),
        &directory.native,
        workspace,
        state_bytes - @as(u32, @intCast(layout.consumed)),
    );
}

fn internalDirNext(
    ctx: ?*anyopaque,
    directory_state: ?*anyopaque,
    out: *core.DirentValue,
    out_entry: *bool,
) callconv(.c) u16 {
    _ = ctx;
    const directory = dirCursor(directory_state);
    var native: core.NativeDirent = std.mem.zeroes(core.NativeDirent);
    const err = ra8_io_vfs_dir_next(&directory.native, &native, out_entry);
    if (err != core.ok) return err;
    if (!out_entry.*) return err;
    const length = core.len(&native.name, core.fw_path_cap);
    if (length >= core.fw_path_cap) return core.err_invalid_size;
    const span = @as(usize, length) + 1;
    @memcpy(out.name[0..span], native.name[0..span]);
    out.name_bytes = length;
    out.size_bytes = native.size_bytes;
    out.kind = if ((native.attr & core.fs_attr_directory) != 0)
        core.node_directory
    else
        core.node_file;
    return core.ok;
}

fn internalDirClose(ctx: ?*anyopaque, directory_state: ?*anyopaque) callconv(.c) u16 {
    _ = ctx;
    const directory = dirCursor(directory_state);
    return ra8_io_vfs_dir_close(&directory.native);
}

fn internalListEntry(name: [*:0]const u8, attr: u8, size: u64, ctx: ?*anyopaque) callconv(.c) void {
    const bridge: *core.ListState = @ptrCast(@alignCast(ctx.?));
    bridge.entry(name, attr, size);
}

fn internalListdir(
    ctx: ?*anyopaque,
    path: [*:0]const u8,
    max_entries: u32,
    callback: core.ListFn,
    callback_ctx: ?*anyopaque,
    out_count: *u32,
    out_complete: *bool,
) callconv(.c) u16 {
    const state = adapter(ctx);
    const built = core.fullPath(&state.mount_name, path, &state.path_a);
    if (built != core.ok) return built;
    var bridge = core.ListState{
        .callback = callback,
        .callback_ctx = callback_ctx,
        .max_entries = max_entries,
    };
    const listed = ra8_io_vfs_listdir(pathA(state), &internalListEntry, &bridge);
    out_count.* = bridge.count;
    out_complete.* = !bridge.stopped;
    return if (bridge.callback_error == core.ok) listed else bridge.callback_error;
}

fn pathOp(
    ctx: ?*anyopaque,
    path: [*:0]const u8,
    operation: *const fn ([*:0]const u8) callconv(.c) u16,
) u16 {
    const state = adapter(ctx);
    const built = core.fullPath(&state.mount_name, path, &state.path_a);
    if (built != core.ok) return built;
    return operation(pathA(state));
}

fn internalMkdir(ctx: ?*anyopaque, path: [*:0]const u8) callconv(.c) u16 {
    return pathOp(ctx, path, &ra8_io_vfs_mkdir);
}

fn internalUnlink(ctx: ?*anyopaque, path: [*:0]const u8) callconv(.c) u16 {
    return pathOp(ctx, path, &ra8_io_vfs_unlink);
}

fn internalRmdir(ctx: ?*anyopaque, path: [*:0]const u8) callconv(.c) u16 {
    return pathOp(ctx, path, &ra8_io_vfs_rmdir);
}

fn internalRename(
    ctx: ?*anyopaque,
    old_path: [*:0]const u8,
    new_path: [*:0]const u8,
    replace: bool,
) callconv(.c) u16 {
    if (replace) return core.err_not_supported;
    const state = adapter(ctx);
    const old_built = core.fullPath(&state.mount_name, old_path, &state.path_a);
    if (old_built != core.ok) return old_built;
    const new_built = core.fullPath(&state.mount_name, new_path, &state.path_b);
    if (new_built != core.ok) return new_built;
    return ra8_io_vfs_rename(pathA(state), pathB(state));
}

fn internalSpace(ctx: ?*anyopaque, out: *core.Space) callconv(.c) u16 {
    const state = adapter(ctx);
    var native: core.NativeSpace = .{};
    const result = ra8_io_vfs_free_space(@ptrCast(&state.mount_name), &native);
    if (result != core.ok) return result;
    out.total_bytes = native.total_bytes;
    out.free_bytes = native.free_bytes;
    out.used_bytes = native.used_bytes;
    return core.ok;
}

// ---------------------------------------------------------------------------
// Stream operations.
// ---------------------------------------------------------------------------

fn internalOpen(
    ctx: ?*anyopaque,
    path: [*:0]const u8,
    mode: u8,
    file_state: ?*anyopaque,
    state_bytes: u32,
) callconv(.c) u16 {
    if (state_bytes < @sizeOf(core.FileState)) return core.err_no_mem;
    var native_mode: u8 = core.fs_mode_read;
    const mode_err = core.modeNative(mode, &native_mode);
    if (mode_err != core.ok) return mode_err;
    const state = adapter(ctx);
    const built = core.fullPath(&state.mount_name, path, &state.path_a);
    if (built != core.ok) return built;
    const file: *core.FileState = @ptrCast(@alignCast(file_state.?));
    file.native = null;
    return ra8_io_vfs_open(pathA(state), native_mode, &file.native);
}

fn internalRead(
    ctx: ?*anyopaque,
    file_state: ?*anyopaque,
    dst: [*]u8,
    cap: u32,
    out_read: *u32,
) callconv(.c) u16 {
    _ = ctx;
    const file: *core.FileState = @ptrCast(@alignCast(file_state.?));
    return ra8_fs_read(file.native, dst, cap, out_read);
}

fn internalWrite(
    ctx: ?*anyopaque,
    file_state: ?*anyopaque,
    src: [*]const u8,
    length: u32,
    out_written: *u32,
) callconv(.c) u16 {
    _ = ctx;
    const file: *core.FileState = @ptrCast(@alignCast(file_state.?));
    const result = ra8_fs_write(file.native, src, length);
    if (result == core.ok) out_written.* = length;
    return result;
}

fn internalSeek(ctx: ?*anyopaque, file_state: ?*anyopaque, offset: u64) callconv(.c) u16 {
    _ = ctx;
    const file: *core.FileState = @ptrCast(@alignCast(file_state.?));
    return ra8_fs_seek(file.native, offset);
}

fn internalTell(ctx: ?*anyopaque, file_state: ?*anyopaque, out_offset: *u64) callconv(.c) u16 {
    _ = ctx;
    const file: *core.FileState = @ptrCast(@alignCast(file_state.?));
    return ra8_fs_tell(file.native, out_offset);
}

fn internalSize(ctx: ?*anyopaque, file_state: ?*anyopaque, out_size: *u64) callconv(.c) u16 {
    _ = ctx;
    const file: *core.FileState = @ptrCast(@alignCast(file_state.?));
    return ra8_fs_size(file.native, out_size);
}

fn internalClose(ctx: ?*anyopaque, file_state: ?*anyopaque) callconv(.c) u16 {
    _ = ctx;
    const file: *core.FileState = @ptrCast(@alignCast(file_state.?));
    const result = ra8_fs_close(file.native);
    file.native = null;
    return result;
}

// ---------------------------------------------------------------------------
// Transaction operations.
// ---------------------------------------------------------------------------

/// Open an unused staging file after a bounded collision search.
fn stageOpen(state: *State, txn: *core.TransactionState) u16 {
    var attempt: u8 = 0;
    while (attempt < core.stage_attempts) : (attempt += 1) {
        state.transaction_id +%= 1;
        const named = core.stagePath(&txn.destination, state.transaction_id, &txn.stage);
        if (named != core.ok) return named;
        var stage_stat: core.Stat = .{};
        const stated = internalStat(state, @ptrCast(&txn.stage), &stage_stat);
        if (stated != core.ok) return stated;
        if (stage_stat.exists) continue;
        const opened = internalOpen(
            state,
            @ptrCast(&txn.stage),
            core.open_write_truncate,
            &txn.file_state,
            @sizeOf(core.FileState),
        );
        if (opened == core.ok) {
            txn.writer_open = true;
            txn.stage_exists = true;
            return core.ok;
        }
        if (opened != core.err_exists) return opened;
    }
    return core.err_no_mem;
}

fn internalTxnBegin(
    ctx: ?*anyopaque,
    transaction_state: ?*anyopaque,
    state_bytes: u32,
    destination: [*:0]const u8,
    policy: u8,
) callconv(.c) u16 {
    if (state_bytes < @sizeOf(core.TransactionState)) return core.err_no_mem;
    if (policy != core.txn_create_new) return core.err_not_supported;
    const state = adapter(ctx);
    var destination_stat: core.Stat = .{};
    const stated = internalStat(state, destination, &destination_stat);
    if (stated != core.ok) return stated;
    if (destination_stat.exists) return core.err_exists;
    const txn: *core.TransactionState = @ptrCast(@alignCast(transaction_state.?));
    @memset(std.mem.asBytes(txn), 0);
    txn.policy = policy;
    const copied = core.copyPath(&txn.destination, destination);
    if (copied != core.ok) return copied;
    return stageOpen(state, txn);
}

fn internalTxnWrite(
    ctx: ?*anyopaque,
    transaction_state: ?*anyopaque,
    src: [*]const u8,
    length: u32,
    out_written: *u32,
) callconv(.c) u16 {
    const txn: *core.TransactionState = @ptrCast(@alignCast(transaction_state.?));
    if (!txn.writer_open) return core.err_invalid_state;
    return internalWrite(ctx, &txn.file_state, src, length, out_written);
}

fn internalTxnSeek(
    ctx: ?*anyopaque,
    transaction_state: ?*anyopaque,
    offset: u64,
) callconv(.c) u16 {
    const txn: *core.TransactionState = @ptrCast(@alignCast(transaction_state.?));
    if (!txn.writer_open) return core.err_invalid_state;
    var size: u64 = 0;
    const sized = internalSize(ctx, &txn.file_state, &size);
    if (sized != core.ok) return sized;
    if (offset > size) return core.err_invalid_size;
    return internalSeek(ctx, &txn.file_state, offset);
}

fn internalTxnValidate(
    ctx: ?*anyopaque,
    transaction_state: ?*anyopaque,
    validator: ValidateFn,
    validator_ctx: ?*anyopaque,
) callconv(.c) u16 {
    const txn: *core.TransactionState = @ptrCast(@alignCast(transaction_state.?));
    if (!txn.writer_open) return core.err_invalid_state;
    const closed = internalClose(ctx, &txn.file_state);
    txn.writer_open = false;
    if (closed != core.ok) return closed;
    const opened = internalOpen(
        ctx,
        @ptrCast(&txn.stage),
        core.open_read,
        &txn.file_state,
        @sizeOf(core.FileState),
    );
    if (opened != core.ok) return opened;
    var staged = File{
        .iface = &s_stream_iface,
        .ctx = ctx,
        .state = &txn.file_state,
        .state_bytes = @sizeOf(core.FileState),
        .is_open = true,
    };
    const checked = validator(validator_ctx, &staged);
    const shut = fw_fs_close(&staged);
    if (checked != core.ok) return checked;
    return shut;
}

fn internalTxnCommit(
    ctx: ?*anyopaque,
    transaction_state: ?*anyopaque,
    out_published: *bool,
) callconv(.c) u16 {
    const txn: *core.TransactionState = @ptrCast(@alignCast(transaction_state.?));
    if (txn.writer_open) return core.err_invalid_state;
    const renamed = internalRename(ctx, @ptrCast(&txn.stage), @ptrCast(&txn.destination), false);
    if (renamed == core.ok) {
        txn.stage_exists = false;
        out_published.* = true;
    }
    return renamed;
}

fn internalTxnAbort(ctx: ?*anyopaque, transaction_state: ?*anyopaque) callconv(.c) u16 {
    const txn: *core.TransactionState = @ptrCast(@alignCast(transaction_state.?));
    var first: u16 = core.ok;
    if (txn.writer_open) {
        first = internalClose(ctx, &txn.file_state);
        txn.writer_open = false;
    }
    if (txn.stage_exists) {
        const removed = internalUnlink(ctx, @ptrCast(&txn.stage));
        if (first == core.ok) first = removed;
        if (removed == core.ok) txn.stage_exists = false;
    }
    return first;
}

// ---------------------------------------------------------------------------
// The three immutable vtables.
// ---------------------------------------------------------------------------

const s_namespace_iface = NamespaceIface{
    .stat = &internalStat,
    .listdir = &internalListdir,
    .dir_open = &internalDirOpen,
    .dir_next = &internalDirNext,
    .dir_close = &internalDirClose,
    .mkdir = &internalMkdir,
    .unlink = &internalUnlink,
    .rmdir = &internalRmdir,
    .rename = &internalRename,
    .space = &internalSpace,
};

const s_stream_iface = StreamIface{
    .open = &internalOpen,
    .read = &internalRead,
    .write = &internalWrite,
    .seek = &internalSeek,
    .tell = &internalTell,
    .size = &internalSize,
    .sync = null,
    .close = &internalClose,
};

const s_transaction_iface = TransactionIface{
    .begin = &internalTxnBegin,
    .write = &internalTxnWrite,
    .seek = &internalTxnSeek,
    .validate = &internalTxnValidate,
    .commit = &internalTxnCommit,
    .abort = &internalTxnAbort,
};

// ---------------------------------------------------------------------------
// Binding.
// ---------------------------------------------------------------------------

/// Query the named format and compose aligned adapter cursor requirements.
fn dirRequirements(
    state: *State,
    out_bytes: *u32,
    out_align: *u8,
    out_max_open: *u16,
) u16 {
    var err = core.fullPath(&state.mount_name, "/", &state.path_a);
    if (err != core.ok) return err;
    var native_bytes: u32 = 0;
    var native_align: u8 = 0;
    err = ra8_io_vfs_dir_requirements(pathA(state), &native_bytes, &native_align, out_max_open);
    if (err != core.ok) return err;
    const composed = core.requirements(native_bytes, native_align) orelse
        return core.err_invalid_size;
    state.directory_workspace_bytes = native_bytes;
    state.directory_workspace_align = native_align;
    state.max_open_directories = out_max_open.*;
    out_bytes.* = composed.bytes;
    out_align.* = composed.alignment;
    return core.ok;
}

/// Bind a live named VFS mount into the portable filesystem facade.
pub export fn fw_fs_ra8_vfs_init(
    out: ?*Fs,
    state_arg: ?*State,
    cfg_arg: ?*const Config,
) callconv(.c) u16 {
    if (out == null) return core.err_null_ptr;
    if (state_arg == null) return core.err_null_ptr;
    if (cfg_arg == null) return core.err_null_ptr;
    const cfg = cfg_arg.?;
    if (cfg.mount_name == null) return core.err_null_ptr;
    if (cfg.mount == null) return core.err_null_ptr;
    if (cfg.mount.?.in_use == 0) return core.err_not_initialized;
    const state = state_arg.?;
    @memset(std.mem.asBytes(state), 0);
    const named = core.mountName(&state.mount_name, cfg.mount_name.?);
    if (named != core.ok) return named;
    state.mount = cfg.mount;
    state.removable_media = cfg.removable_media;
    var directory_bytes: u32 = 0;
    var max_directories: u16 = 0;
    var directory_alignment: u8 = 0;
    const directory_requirements =
        dirRequirements(state, &directory_bytes, &directory_alignment, &max_directories);
    if (directory_requirements != core.ok) return directory_requirements;
    const caps = core.capabilities(
        cfg.mount.?.fs_type == core.fs_type_exfat,
        cfg.removable_media,
        directory_bytes,
        directory_alignment,
        max_directories,
    );
    return fw_fs_bind(
        out.?,
        &s_namespace_iface,
        &s_stream_iface,
        &s_transaction_iface,
        state,
        &caps,
    );
}

// ---------------------------------------------------------------------------
// Layout assertions for the types the C ABI hands us.
// ---------------------------------------------------------------------------

comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(@sizeOf(Backend) == ptr * 5);
    std.debug.assert(@offsetOf(Mount, "fs_type") == ptr * 5);
    std.debug.assert(@offsetOf(Mount, "in_use") == @offsetOf(Mount, "partition_base_lba") + 8);
    std.debug.assert(@sizeOf(NamespaceIface) == ptr * 10);
    std.debug.assert(@sizeOf(StreamIface) == ptr * 8);
    std.debug.assert(@sizeOf(TransactionIface) == ptr * 6);
    std.debug.assert(@offsetOf(File, "state_bytes") == ptr * 3);
    std.debug.assert(@offsetOf(Config, "mount") == ptr);
    std.debug.assert(@offsetOf(State, "path_a") == 16);
    std.debug.assert(@offsetOf(State, "path_b") == 16 + core.full_path_cap);
    std.debug.assert(@offsetOf(State, "mount") % ptr == 0);
    std.debug.assert(@offsetOf(State, "directory_workspace_bytes") ==
        @offsetOf(State, "mount") + ptr);
}
