//! Portable filesystem interface core: the pure guard, path, workspace and
//! coherence arithmetic of `libs/if`, with no external symbols at all.
//!
//! Every function here is a total function of its arguments. The ABI file
//! owns pointer nullability, the backend vtables and the handle mutation;
//! this file owns the decisions, so the host tests can drive each one
//! directly without linking a backend.

const std = @import("std");

// ---------------------------------------------------------------------------
// ra8_err.h codes this interface can answer with (`typedef enum : uint16_t`).
// ---------------------------------------------------------------------------

pub const Err = u16;
pub const ok: Err = 0;
pub const err_no_mem: Err = 0x102;
pub const err_invalid_arg: Err = 0x103;
pub const err_invalid_state: Err = 0x104;
pub const err_invalid_size: Err = 0x105;
pub const err_not_supported: Err = 0x107;
pub const err_busy: Err = 0x109;
pub const err_not_initialized: Err = 0x10F;
pub const err_access_denied: Err = 0x112;
pub const err_null_ptr: Err = 0x504;

/// k_fw_fs_path_cap: largest portable path including its NUL.
pub const path_cap: u16 = 512;

/// fw_fs_ascii_byte_t.
pub const ascii_space: u8 = 0x20;
pub const ascii_delete: u8 = 0x7F;

/// fw_fs_node_type_t.
pub const node_none: u8 = 0;
pub const node_file: u8 = 1;
pub const node_directory: u8 = 2;
pub const node_symlink: u8 = 3;
pub const node_other: u8 = 4;

/// fw_fs_open_mode_t.
pub const open_read: u8 = 0;
pub const open_write_truncate: u8 = 1;
pub const open_append: u8 = 2;
pub const open_create_new: u8 = 3;

/// fw_fs_transaction_policy_t.
pub const txn_create_new: u8 = 0;
pub const txn_replace_atomic: u8 = 1;

/// fw_fs_capability_t.
pub const cap_namespace: u32 = 1 << 0;
pub const cap_stream: u32 = 1 << 1;
pub const cap_space_query: u32 = 1 << 2;
pub const cap_same_volume_rename: u32 = 1 << 3;
pub const cap_atomic_replace: u32 = 1 << 4;
pub const cap_atomic_noreplace: u32 = 1 << 5;
pub const cap_create_exclusive: u32 = 1 << 6;
pub const cap_file_sync: u32 = 1 << 7;
pub const cap_durable_file_sync: u32 = 1 << 8;
pub const cap_durable_directory_sync: u32 = 1 << 9;
pub const cap_transactions: u32 = 1 << 10;
pub const cap_symlinks: u32 = 1 << 11;
pub const cap_rejects_symlink_walk: u32 = 1 << 12;
pub const cap_case_sensitive: u32 = 1 << 13;
pub const cap_removable_media: u32 = 1 << 14;
pub const cap_thread_safe: u32 = 1 << 15;
pub const cap_created_time: u32 = 1 << 16;
pub const cap_modified_time: u32 = 1 << 17;
pub const cap_accessed_time: u32 = 1 << 18;

// ---------------------------------------------------------------------------
// Caller-owned value types of the public C ABI.
// ---------------------------------------------------------------------------

/// fw_fs_caps_t.
pub const Caps = extern struct {
    max_file_bytes: u64 = 0,
    flags: u32 = 0,
    file_workspace_bytes: u32 = 0,
    directory_workspace_bytes: u32 = 0,
    transaction_workspace_bytes: u32 = 0,
    path_max_bytes: u16 = 0,
    name_max_bytes: u16 = 0,
    max_open_files: u16 = 0,
    max_open_directories: u16 = 0,
    file_workspace_align: u8 = 0,
    directory_workspace_align: u8 = 0,
    transaction_workspace_align: u8 = 0,
};

/// fw_fs_datetime_t.
pub const Datetime = extern struct {
    nanosecond: u32 = 0,
    year: u16 = 0,
    utc_offset_min: i16 = 0,
    month: u8 = 0,
    day: u8 = 0,
    hour: u8 = 0,
    minute: u8 = 0,
    second: u8 = 0,
};

/// fw_fs_timestamp_t.
pub const Timestamp = extern struct {
    value: Datetime = .{},
    valid: bool = false,
    utc_offset_valid: bool = false,
};

/// fw_fs_stat_t.
pub const Stat = extern struct {
    size_bytes: u64 = 0,
    created: Timestamp = .{},
    modified: Timestamp = .{},
    accessed: Timestamp = .{},
    node_type: u8 = node_none,
    exists: bool = false,
};

/// fw_fs_dirent_t.
pub const Dirent = extern struct {
    name: ?[*:0]const u8 = null,
    size_bytes: u64 = 0,
    name_bytes: u16 = 0,
    node_type: u8 = node_none,
};

/// fw_fs_dirent_value_t.
pub const DirentValue = extern struct {
    name: [path_cap]u8 = @splat(0),
    size_bytes: u64 = 0,
    name_bytes: u16 = 0,
    node_type: u8 = node_none,
};

/// fw_fs_space_t.
pub const Space = extern struct {
    total_bytes: u64 = 0,
    free_bytes: u64 = 0,
    used_bytes: u64 = 0,
};

/// fw_fs_list_fn_t.
pub const ListFn = ?*const fn (?*anyopaque, *const Dirent, *bool) callconv(.c) Err;

comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(@sizeOf(Caps) == 40);
    std.debug.assert(@offsetOf(Caps, "path_max_bytes") == 24);
    std.debug.assert(@offsetOf(Caps, "file_workspace_align") == 32);
    std.debug.assert(@sizeOf(Datetime) == 16);
    std.debug.assert(@sizeOf(Timestamp) == 20);
    std.debug.assert(@offsetOf(Timestamp, "valid") == 16);
    std.debug.assert(@sizeOf(Stat) == 72);
    std.debug.assert(@offsetOf(Stat, "created") == 8);
    std.debug.assert(@offsetOf(Stat, "node_type") == 68);
    const dirent_size_offset = std.mem.alignForward(usize, ptr, 8);
    std.debug.assert(@offsetOf(Dirent, "size_bytes") == dirent_size_offset);
    std.debug.assert(@sizeOf(Dirent) == std.mem.alignForward(usize, dirent_size_offset + 11, 8));
    std.debug.assert(@offsetOf(DirentValue, "size_bytes") == 512);
    std.debug.assert(@sizeOf(DirentValue) == 528);
    std.debug.assert(@sizeOf(Space) == 24);
}

// ---------------------------------------------------------------------------
// Workspace and alignment arithmetic.
// ---------------------------------------------------------------------------

/// internal_power_of_two: zero is NOT a power of two here.
pub fn powerOfTwo(value: u32) bool {
    if (value == 0) return false;
    return (value & (value - 1)) == 0;
}

/// internal_workspace / internal_cursor_workspace. The two C helpers are
/// textually different (the cursor copy spells the `align == 0` case out
/// instead of leaning on internal_power_of_two) but decide identically, so
/// one function serves both call sites.
///
/// `address` is 0 for a NULL workspace.
pub fn workspace(address: usize, bytes: u32, need: u32, alignment: u8) Err {
    if (address == 0) return err_null_ptr;
    if (bytes < need) return err_no_mem;
    if (!powerOfTwo(@as(u32, alignment))) return err_invalid_state;
    if ((address % @as(usize, alignment)) != 0) return err_invalid_arg;
    return ok;
}

// ---------------------------------------------------------------------------
// Portable path syntax.
// ---------------------------------------------------------------------------

/// internal_component: reject an empty, `.` or `..` component.
pub fn componentStatus(path: [*]const u8, start: u16, length: u16) Err {
    if (length == 0) return err_invalid_arg;
    if (length == 1) {
        if (path[start] == '.') return err_access_denied;
    }
    if (length == 2) {
        if (path[start] == '.') {
            if (path[start +% 1] == '.') return err_access_denied;
        }
    }
    return ok;
}

/// internal_fw_fs_scan_components: walk the bytes after the leading '/'.
pub fn scanComponents(caps: *const Caps, path: [*]const u8) Err {
    var component_start: u16 = 1;
    var component_len: u16 = 0;
    var i: u16 = 1;
    while (i < caps.path_max_bytes) : (i +%= 1) {
        const value = path[i];
        if (value == 0) return componentStatus(path, component_start, component_len);
        if (value == '/') {
            const component = componentStatus(path, component_start, component_len);
            if (component != ok) return component;
            component_start = i +% 1;
            component_len = 0;
            continue;
        }
        if (value == ':') return err_access_denied;
        if (value == '\\') return err_access_denied;
        if (value < ascii_space) return err_invalid_arg;
        if (value == ascii_delete) return err_invalid_arg;
        component_len +%= 1;
        if (component_len > caps.name_max_bytes) return err_invalid_size;
    }
    return err_invalid_size;
}

/// fw_fs_path_validate with both pointers already known non-NULL.
pub fn pathValidate(caps: *const Caps, path: [*]const u8) Err {
    if (caps.path_max_bytes < 2) return err_invalid_state;
    if (caps.path_max_bytes > path_cap) return err_invalid_state;
    if (caps.name_max_bytes == 0) return err_invalid_state;
    if (path[0] != '/') return err_invalid_arg;
    if (path[1] == 0) return ok;
    return scanComponents(caps, path);
}

/// True when the path names the root, i.e. `path[1] == '\0'`.
pub fn isRoot(path: [*]const u8) bool {
    return path[1] == 0;
}

// ---------------------------------------------------------------------------
// Backend answer coherence at the trust boundary.
// ---------------------------------------------------------------------------

/// The four fw_fs_stat coherence predicates, kept separate so each one is
/// reachable on its own from the tests.
pub fn statTypeInvalid(value: *const Stat) bool {
    return @as(u32, value.node_type) > @as(u32, node_other);
}

pub fn statMissingInvalid(value: *const Stat) bool {
    return !value.exists and ((value.node_type != node_none) or (value.size_bytes != 0));
}

pub fn statPresentInvalid(value: *const Stat) bool {
    return value.exists and (value.node_type == node_none);
}

pub fn statDirectoryInvalid(value: *const Stat) bool {
    return (value.node_type == node_directory) and (value.size_bytes != 0);
}

/// True when a backend stat answer must be scrubbed and refused.
pub fn statIncoherent(value: *const Stat) bool {
    return statTypeInvalid(value) or statMissingInvalid(value) or
        statPresentInvalid(value) or statDirectoryInvalid(value);
}

/// True when a backend space answer must be scrubbed and refused.
pub fn spaceIncoherent(value: *const Space) bool {
    return (value.free_bytes > value.total_bytes) or (value.used_bytes > value.total_bytes);
}

/// internal_cursor_entry: bounded NUL termination, portable leaf syntax,
/// node type and the directory-size invariant.
pub fn entryStatus(caps: *const Caps, entry: *const DirentValue) Err {
    if ((entry.name_bytes == 0) or (entry.name_bytes > caps.name_max_bytes) or
        (entry.name_bytes >= path_cap))
    {
        return err_invalid_state;
    }
    if (entry.name[entry.name_bytes] != 0) return err_invalid_state;
    if (nulBoundedLen(&entry.name) != entry.name_bytes) return err_invalid_state;
    if ((entry.node_type == node_none) or
        (@as(u32, entry.node_type) > @as(u32, node_other)) or
        ((entry.node_type == node_directory) and (entry.size_bytes != 0)))
    {
        return err_invalid_state;
    }
    var path: [path_cap]u8 = @splat(0);
    path[0] = '/';
    // The C copies name_bytes + 1 bytes to &path[1]; with name_bytes == 511
    // that runs one byte past the buffer, so the copy is bounded here. Every
    // entry the guards above admit with a validatable leaf copies identically.
    const copy = @min(@as(usize, entry.name_bytes) + 1, path.len - 1);
    @memcpy(path[1 .. 1 + copy], entry.name[0..copy]);
    return if (pathValidate(caps, &path) == ok) ok else err_invalid_state;
}

/// strnlen(name, k_fw_fs_path_cap).
pub fn nulBoundedLen(name: *const [path_cap]u8) u16 {
    var i: u16 = 0;
    while (i < path_cap) : (i += 1) {
        if (name[i] == 0) return i;
    }
    return path_cap;
}

// ---------------------------------------------------------------------------
// Bind-time validation, expressed over vtable presence rather than pointers.
// ---------------------------------------------------------------------------

/// Which namespace operations a candidate vtable actually carries.
pub const NamespacePresence = struct {
    stat: bool = false,
    listdir: bool = false,
    dir_open: bool = false,
    dir_next: bool = false,
    dir_close: bool = false,
    mkdir: bool = false,
    unlink: bool = false,
    rmdir: bool = false,
    rename: bool = false,
    space: bool = false,
};

/// Which stream operations a candidate vtable actually carries.
pub const StreamPresence = struct {
    open: bool = false,
    read: bool = false,
    write: bool = false,
    seek: bool = false,
    tell: bool = false,
    size: bool = false,
    sync: bool = false,
    close: bool = false,
};

/// Which transaction operations a candidate vtable actually carries.
pub const TransactionPresence = struct {
    begin: bool = false,
    write: bool = false,
    seek: bool = false,
    validate: bool = false,
    commit: bool = false,
    abort: bool = false,
};

/// internal_interfaces, in the C's exact grouping order. A null transaction
/// vtable is allowed and ends the check early.
pub fn interfacesStatus(
    names: NamespacePresence,
    streams: StreamPresence,
    transactions: ?TransactionPresence,
) Err {
    if (!names.stat or !names.listdir) return err_invalid_arg;
    if (!names.dir_open or !names.dir_next or !names.dir_close) return err_invalid_arg;
    if (!names.mkdir or !names.unlink) return err_invalid_arg;
    if (!names.rmdir or !names.rename) return err_invalid_arg;
    if (!streams.open or !streams.read) return err_invalid_arg;
    if (!streams.write or !streams.close) return err_invalid_arg;
    if (!streams.seek or !streams.tell or !streams.size) return err_invalid_arg;
    const txn = transactions orelse return ok;
    if (!txn.begin or !txn.write) return err_invalid_arg;
    if (!txn.seek or !txn.validate) return err_invalid_arg;
    return if (!txn.commit or !txn.abort) err_invalid_arg else ok;
}

/// internal_fw_fs_caps_validate.
pub fn capsStatus(
    caps: *const Caps,
    has_space: bool,
    has_sync: bool,
    has_transaction_iface: bool,
) Err {
    const required: u32 = cap_namespace | cap_stream;
    if ((caps.flags & required) != required) return err_invalid_arg;
    if (((caps.flags & cap_space_query) != 0) and !has_space) return err_invalid_arg;
    if (((caps.flags & cap_file_sync) != 0) and !has_sync) return err_invalid_arg;
    if (((caps.flags & cap_durable_file_sync) != 0) and
        ((caps.flags & cap_file_sync) == 0)) return err_invalid_arg;
    if (((caps.flags & cap_transactions) != 0) and !has_transaction_iface) return err_invalid_arg;
    if (!powerOfTwo(caps.file_workspace_align)) return err_invalid_arg;
    if ((caps.directory_workspace_bytes == 0) or (caps.max_open_directories == 0) or
        !powerOfTwo(caps.directory_workspace_align)) return err_invalid_arg;
    if (!powerOfTwo(caps.transaction_workspace_align)) return err_invalid_arg;
    return ok;
}

// ---------------------------------------------------------------------------
// Mode, policy and transaction preamble arithmetic.
// ---------------------------------------------------------------------------

/// fw_fs_open's mode range and exclusive-create capability rule.
pub fn openModeStatus(mode: u8, flags: u32) Err {
    if (@as(u32, mode) > @as(u32, open_create_new)) return err_invalid_arg;
    if (mode == open_create_new) {
        if ((flags & cap_create_exclusive) == 0) return err_not_supported;
    }
    return ok;
}

/// fw_fs_rename's replace / no-replace capability rule.
pub fn renameCapabilityStatus(flags: u32, replace: bool) Err {
    if (replace) {
        if ((flags & cap_atomic_replace) == 0) return err_not_supported;
    } else if ((flags & cap_atomic_noreplace) == 0) {
        return err_not_supported;
    }
    return ok;
}

/// internal_fw_fs_transaction_preamble. A missing vtable answers
/// not_supported when the port never claimed transactions at all, and
/// not_initialized when it did.
pub fn transactionPreamble(flags: u32, has_iface: bool, active: bool, policy: u8) Err {
    if (!has_iface) {
        return if ((flags & cap_transactions) == 0) err_not_supported else err_not_initialized;
    }
    if ((flags & cap_transactions) == 0) return err_not_supported;
    if (active) return err_busy;
    if (@as(u32, policy) > @as(u32, txn_replace_atomic)) return err_invalid_arg;
    if (policy == txn_replace_atomic) {
        if ((flags & cap_atomic_replace) == 0) return err_not_supported;
    } else if ((flags & cap_atomic_noreplace) == 0) {
        return err_not_supported;
    }
    return ok;
}
