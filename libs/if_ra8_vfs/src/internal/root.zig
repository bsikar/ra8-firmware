//! Pure adapter logic for the `fw_if_fs` binding over one `ra8_io_vfs` mount.
//!
//! Everything here is free of externs so it can be exercised without linking
//! the VFS, the filesystem, or the portable facade. The ABI layer owns every
//! call that leaves this library.

const std = @import("std");

// ---------------------------------------------------------------------------
// ra8_err_t values used by this adapter, verbatim from ra8_err.h.
// ---------------------------------------------------------------------------

pub const Err = u16;
pub const ok: Err = 0;
pub const err_no_mem: Err = 0x102;
pub const err_invalid_arg: Err = 0x103;
pub const err_invalid_state: Err = 0x104;
pub const err_invalid_size: Err = 0x105;
pub const err_not_supported: Err = 0x107;
pub const err_exists: Err = 0x10C;
pub const err_not_initialized: Err = 0x10F;
pub const err_null_ptr: Err = 0x504;

// ---------------------------------------------------------------------------
// Fixed limits. `full_path_cap` is the header's k_fw_fs_ra8_vfs_full_path_cap.
// ---------------------------------------------------------------------------

/// Mount name length including NUL (k_ra8_io_vfs_name_max).
pub const io_vfs_name_max: u16 = 16;
/// Largest portable path including NUL (k_fw_fs_path_cap).
pub const fw_path_cap: u16 = 512;
/// `name:` + path + NUL.
pub const full_path_cap: u16 = fw_path_cap + io_vfs_name_max + 2;

pub const hex_nibble_bits: u8 = 4;
pub const stage_hex_digits: u8 = 6;
pub const hex_last_digit: u8 = stage_hex_digits - 1;
pub const hex_nibble_mask: u8 = 0x0F;
/// Bounded collision search for an unused stage name.
pub const stage_attempts: u8 = 64;

/// `TX` + six hex digits + `.TMP`.
pub const stage_leaf_bytes: u16 = 12;
pub const exfat_name_max_bytes: u16 = 192;
pub const fat_name_max_bytes: u16 = 510;

pub const nanoseconds_per_centisecond: u32 = 10_000_000;
pub const transaction_id_mask: u32 = 0x00FF_FFFF;

/// k_ra8_fs_max_files.
pub const fs_max_files: u16 = 4;
/// k_ra8_fs_fat_max_file_bytes: 4 GiB - 1, the largest DIR_FileSize.
pub const fs_fat_max_file_bytes: u64 = 0xFFFF_FFFF;
/// k_ra8_fs_type_exfat.
pub const fs_type_exfat: u8 = 64;
/// k_ra8_fs_attr_directory.
pub const fs_attr_directory: u8 = 0x10;
/// Longest listed native name plus NUL (k_ra8_fs_dir_name_cap).
pub const fs_dir_name_cap: u16 = 742;

// fw_fs_node_type_t
pub const node_none: u8 = 0;
pub const node_file: u8 = 1;
pub const node_directory: u8 = 2;

// fw_fs_open_mode_t
pub const open_read: u8 = 0;
pub const open_write_truncate: u8 = 1;
pub const open_append: u8 = 2;
pub const open_create_new: u8 = 3;

// ra8_fs_mode_t
pub const fs_mode_read: u8 = 0;
pub const fs_mode_write: u8 = 1;
pub const fs_mode_append: u8 = 2;

// fw_fs_transaction_policy_t
pub const txn_create_new: u8 = 0;
pub const txn_replace_atomic: u8 = 1;

// fw_fs_capability_t
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
// Portable value types (fw_if_fs_types.h) mirrored for layout.
// ---------------------------------------------------------------------------

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

/// fw_fs_stat_t. `kind` is the C member named `type`.
pub const Stat = extern struct {
    size_bytes: u64 = 0,
    created: Timestamp = .{},
    modified: Timestamp = .{},
    accessed: Timestamp = .{},
    kind: u8 = node_none,
    exists: bool = false,
};

/// fw_fs_dirent_t, valid only for one callback invocation.
pub const Dirent = extern struct {
    name: ?[*:0]const u8 = null,
    size_bytes: u64 = 0,
    name_bytes: u16 = 0,
    kind: u8 = node_none,
};

/// fw_fs_dirent_value_t, the stable caller-owned copy.
pub const DirentValue = extern struct {
    name: [fw_path_cap]u8,
    size_bytes: u64,
    name_bytes: u16,
    kind: u8,
};

/// fw_fs_space_t.
pub const Space = extern struct {
    total_bytes: u64 = 0,
    free_bytes: u64 = 0,
    used_bytes: u64 = 0,
};

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

// ---------------------------------------------------------------------------
// Native value types (ra8_fs_types.h / ra8_io_vfs.h) mirrored for layout.
// ---------------------------------------------------------------------------

/// ra8_fs_datetime_t.
pub const NativeDatetime = extern struct {
    year: u16 = 0,
    utc_offset_min: i16 = 0,
    month: u8 = 0,
    day: u8 = 0,
    hour: u8 = 0,
    minute: u8 = 0,
    second: u8 = 0,
    centisecond: u8 = 0,
};

/// ra8_fs_timestamp_t.
pub const NativeTimestamp = extern struct {
    value: NativeDatetime = .{},
    valid: bool = false,
    utc_offset_valid: bool = false,
};

/// ra8_io_vfs_stat_t.
pub const NativeStat = extern struct {
    size_bytes: u64 = 0,
    created: NativeTimestamp = .{},
    modified: NativeTimestamp = .{},
    accessed: NativeTimestamp = .{},
    attr: u8 = 0,
    is_directory: bool = false,
    exists: bool = false,
};

/// ra8_fs_dirent_t.
pub const NativeDirent = extern struct {
    name: [fs_dir_name_cap]u8,
    size_bytes: u64,
    attr: u8,
};

/// ra8_fs_space_t. Only the first three fields reach the portable shape.
pub const NativeSpace = extern struct {
    total_bytes: u64 = 0,
    free_bytes: u64 = 0,
    used_bytes: u64 = 0,
    bytes_per_cluster: u32 = 0,
    total_clusters: u32 = 0,
    free_clusters: u32 = 0,
    used_clusters: u32 = 0,
};

/// ra8_io_vfs_dir_t, the format-neutral cursor.
pub const NativeDir = extern struct {
    format: ?*const anyopaque = null,
    state: ?*anyopaque = null,
    state_bytes: u32 = 0,
    is_open: bool = false,
};

// ---------------------------------------------------------------------------
// Backend state stored in caller-owned workspaces.
// ---------------------------------------------------------------------------

/// Backend state in a caller's file workspace (vfs_file_state_t).
pub const FileState = extern struct {
    native: ?*anyopaque = null,
};

/// Backend state in a caller's directory workspace (vfs_directory_state_t).
pub const DirectoryState = extern struct {
    native: NativeDir = .{},
};

/// Backend state in a caller's transaction workspace (vfs_transaction_state_t).
pub const TransactionState = extern struct {
    destination: [fw_path_cap]u8,
    stage: [fw_path_cap]u8,
    file_state: FileState,
    policy: u8,
    writer_open: bool,
    stage_exists: bool,
};

// ---------------------------------------------------------------------------
// Pure helpers.
// ---------------------------------------------------------------------------

/// Translate one decoded FAT/exFAT civil timestamp without an epoch.
///
/// An invalid native timestamp stays a fully zeroed portable value; a valid one
/// keeps UTC-offset validity independently of overall validity.
pub fn timestamp(native: *const NativeTimestamp) Timestamp {
    var portable: Timestamp = .{};
    if (native.valid) {
        portable.value.nanosecond =
            @as(u32, native.value.centisecond) * nanoseconds_per_centisecond;
        portable.value.year = native.value.year;
        portable.value.utc_offset_min = native.value.utc_offset_min;
        portable.value.month = native.value.month;
        portable.value.day = native.value.day;
        portable.value.hour = native.value.hour;
        portable.value.minute = native.value.minute;
        portable.value.second = native.value.second;
        portable.valid = true;
        portable.utc_offset_valid = native.utc_offset_valid;
    }
    return portable;
}

/// Measure a string without reading past `cap`; returns `cap` when unterminated.
pub fn len(text: [*]const u8, cap: u16) u16 {
    var length: u16 = 0;
    while (length < cap) {
        if (text[length] == 0) break;
        length += 1;
    }
    return length;
}

/// Prefix one portable path with the bound mount name, writing `mount:/path`.
pub fn fullPath(mount_name: *const [io_vfs_name_max]u8, path: [*]const u8, out: [*]u8) Err {
    const mount_len = len(mount_name, io_vfs_name_max);
    const path_len = len(path, fw_path_cap);
    if (mount_len >= io_vfs_name_max) return err_invalid_state;
    if (path_len >= fw_path_cap) return err_invalid_size;
    var cursor: u16 = 0;
    var i: u16 = 0;
    while (i < mount_len) : (i += 1) {
        out[cursor] = mount_name[i];
        cursor += 1;
    }
    out[cursor] = ':';
    cursor += 1;
    i = 0;
    while (i <= path_len) : (i += 1) {
        out[cursor] = path[i];
        cursor += 1;
    }
    return ok;
}

/// Where the cursor and the format workspace sit inside a caller span.
pub const DirLayout = struct {
    /// First `alignOf(DirectoryState)` base in the span.
    cursor: usize,
    /// First `native_align` base past the cursor.
    workspace: usize,
    /// Span bytes up to one past the cursor.
    cursor_end: usize,
    /// Span bytes ahead of `workspace`.
    consumed: usize,
};

/// Derive both aligned bases inside a caller directory-state span.
///
/// The arithmetic is deliberately wrapping so a degenerate `native_align` of
/// zero produces exactly the unsigned wraparound the C did, which the capacity
/// guards in `dir_open` then reject as `k_ra8_err_no_mem`.
pub fn dirLayout(base: usize, native_align: u8) DirLayout {
    const cursor_align: usize = @alignOf(DirectoryState);
    const cursor = (base +% (cursor_align -% 1)) & ~(cursor_align -% 1);
    const cursor_end = cursor +% @sizeOf(DirectoryState);
    const alignment: usize = native_align;
    const native = (cursor_end +% (alignment -% 1)) & ~(alignment -% 1);
    return .{
        .cursor = cursor,
        .workspace = native,
        .cursor_end = cursor_end -% base,
        .consumed = native -% base,
    };
}

/// Round a caller span up to the cursor's own alignment.
pub fn dirCursorBase(base: usize) usize {
    return dirLayout(base, @intCast(@alignOf(DirectoryState))).cursor;
}

/// Render a fixed-width six-digit uppercase hexadecimal field, no NUL.
pub fn hex6(out: *[stage_hex_digits]u8, value: u32) void {
    const digits = "0123456789ABCDEF";
    var i: u8 = 0;
    while (i < stage_hex_digits) : (i += 1) {
        const shift: u5 = @intCast((hex_last_digit - i) * hex_nibble_bits);
        out[i] = digits[(value >> shift) & hex_nibble_mask];
    }
}

/// Build an 8.3-compatible sibling stage path `<dir>/TXxxxxxx.TMP`.
pub fn stagePath(destination: [*]const u8, id: u32, out: [*]u8) Err {
    var last_slash: u16 = 0;
    var length: u16 = 0;
    while (length < fw_path_cap) {
        const value = destination[length];
        if (value == 0) break;
        if (value == '/') last_slash = length;
        length += 1;
    }
    if (length >= fw_path_cap) return err_invalid_size;
    const stage_length: u16 = last_slash + 1 + stage_leaf_bytes;
    if (stage_length >= fw_path_cap) return err_invalid_size;
    var i: u16 = 0;
    while (i <= last_slash) : (i += 1) out[i] = destination[i];
    var cursor: u16 = last_slash + 1;
    out[cursor] = 'T';
    cursor += 1;
    out[cursor] = 'X';
    cursor += 1;
    hex6(@ptrCast(out + cursor), id & transaction_id_mask);
    cursor += stage_hex_digits;
    out[cursor] = '.';
    cursor += 1;
    out[cursor] = 'T';
    cursor += 1;
    out[cursor] = 'M';
    cursor += 1;
    out[cursor] = 'P';
    cursor += 1;
    out[cursor] = 0;
    return ok;
}

/// Copy a portable path through its first NUL within the fixed path cap.
pub fn copyPath(out: [*]u8, path: [*]const u8) Err {
    var i: u16 = 0;
    while (i < fw_path_cap) : (i += 1) {
        out[i] = path[i];
        if (path[i] == 0) return ok;
    }
    return err_invalid_size;
}

/// Validate and copy a VFS mount name: non-empty, bounded, no `:` and no `/`.
pub fn mountName(out: *[io_vfs_name_max]u8, name: [*]const u8) Err {
    var length: u16 = 0;
    while (length < io_vfs_name_max) {
        const value = name[length];
        if (value == 0) break;
        if (value == ':') return err_invalid_arg;
        if (value == '/') return err_invalid_arg;
        length += 1;
    }
    if (length == 0) return err_invalid_arg;
    if (length >= io_vfs_name_max) return err_invalid_arg;
    var i: u16 = 0;
    while (i <= length) : (i += 1) out[i] = name[i];
    return ok;
}

/// Map a portable open mode to the native VFS mode set.
///
/// Only read, write-truncate and append have a faithful native equivalent, so
/// create-new is refused rather than silently downgraded.
pub fn modeNative(mode: u8, out: *u8) Err {
    if (mode == open_read) {
        out.* = fs_mode_read;
        return ok;
    }
    if (mode == open_write_truncate) {
        out.* = fs_mode_write;
        return ok;
    }
    if (mode == open_append) {
        out.* = fs_mode_append;
        return ok;
    }
    return err_not_supported;
}

/// Outer directory workspace requirement composed from the format's own.
pub const Requirements = struct {
    bytes: u32,
    alignment: u8,
};

/// Add checked space for the adapter wrapper around the format cursor.
///
/// Returns null when the checked total cannot be represented in 32 bits, which
/// the caller reports as `k_ra8_err_invalid_size`.
pub fn requirements(native_bytes: u32, native_align: u8) ?Requirements {
    const cursor_align: u8 = @intCast(@alignOf(DirectoryState));
    const outer_align: u8 = if (native_align > cursor_align) native_align else cursor_align;
    const total: u64 = @as(u64, @sizeOf(DirectoryState)) +%
        @as(u64, native_align) -% 1 +% @as(u64, native_bytes);
    if (total > std.math.maxInt(u32)) return null;
    return .{ .bytes = @intCast(total), .alignment = outer_align };
}

/// Compose the portable capability record for one mounted VFS format.
pub fn capabilities(
    exfat: bool,
    removable: bool,
    directory_bytes: u32,
    directory_alignment: u8,
    max_directories: u16,
) Caps {
    var caps: Caps = .{
        .max_file_bytes = if (exfat) std.math.maxInt(u64) else fs_fat_max_file_bytes,
        .flags = cap_namespace | cap_stream | cap_space_query | cap_same_volume_rename |
            cap_atomic_noreplace | cap_transactions | cap_rejects_symlink_walk,
        .file_workspace_bytes = @sizeOf(FileState),
        .directory_workspace_bytes = directory_bytes,
        .transaction_workspace_bytes = @sizeOf(TransactionState),
        .path_max_bytes = fw_path_cap,
        .name_max_bytes = if (exfat) exfat_name_max_bytes else fat_name_max_bytes,
        .max_open_files = fs_max_files,
        .max_open_directories = max_directories,
        .file_workspace_align = @intCast(@alignOf(FileState)),
        .directory_workspace_align = directory_alignment,
        .transaction_workspace_align = @intCast(@alignOf(TransactionState)),
    };
    caps.flags |= cap_created_time | cap_modified_time | cap_accessed_time;
    if (removable) caps.flags |= cap_removable_media;
    return caps;
}

/// fw_fs_list_fn_t.
pub const ListFn = *const fn (?*anyopaque, *const Dirent, *bool) callconv(.c) Err;

/// Callback-list bridge over a format-owned enumeration.
///
/// The native listdir seam has no budget and no stop signal, so the bridge
/// carries both, plus the first callback error, and distinguishes a budget stop
/// from a callback stop from a bounded-name failure.
pub const ListState = struct {
    callback: ListFn,
    callback_ctx: ?*anyopaque,
    max_entries: u32,
    count: u32 = 0,
    callback_error: Err = ok,
    stopped: bool = false,

    /// Bound and classify one native entry, then forward it at most once.
    pub fn entry(self: *ListState, name: [*:0]const u8, attr: u8, size: u64) void {
        if (self.stopped) return;
        if (self.count >= self.max_entries) {
            self.stopped = true;
            return;
        }
        const length = len(name, fw_path_cap);
        if (length >= fw_path_cap) {
            self.callback_error = err_invalid_size;
            self.stopped = true;
            return;
        }
        const item = Dirent{
            .name = name,
            .size_bytes = size,
            .name_bytes = length,
            .kind = if ((attr & fs_attr_directory) != 0) node_directory else node_file,
        };
        var keep_going: bool = true;
        self.callback_error = self.callback(self.callback_ctx, &item, &keep_going);
        self.count += 1;
        self.stopped = self.callback_error != ok;
        if (!keep_going) self.stopped = true;
    }
};

// ---------------------------------------------------------------------------
// Layout assertions. These are the C ABI, not implementation detail.
// ---------------------------------------------------------------------------

comptime {
    std.debug.assert(full_path_cap == 530);
    std.debug.assert(@sizeOf(Datetime) == 16);
    std.debug.assert(@offsetOf(Datetime, "year") == 4);
    std.debug.assert(@offsetOf(Datetime, "utc_offset_min") == 6);
    std.debug.assert(@offsetOf(Datetime, "second") == 12);
    std.debug.assert(@sizeOf(Timestamp) == 20);
    std.debug.assert(@offsetOf(Timestamp, "valid") == 16);
    std.debug.assert(@sizeOf(Stat) == 72);
    std.debug.assert(@offsetOf(Stat, "created") == 8);
    std.debug.assert(@offsetOf(Stat, "modified") == 28);
    std.debug.assert(@offsetOf(Stat, "accessed") == 48);
    std.debug.assert(@offsetOf(Stat, "kind") == 68);
    std.debug.assert(@offsetOf(Stat, "exists") == 69);
    std.debug.assert(@sizeOf(DirentValue) == 528);
    std.debug.assert(@offsetOf(DirentValue, "size_bytes") == 512);
    std.debug.assert(@offsetOf(DirentValue, "name_bytes") == 520);
    std.debug.assert(@sizeOf(Space) == 24);
    std.debug.assert(@sizeOf(Caps) == 40);
    std.debug.assert(@offsetOf(Caps, "flags") == 8);
    std.debug.assert(@offsetOf(Caps, "path_max_bytes") == 24);
    std.debug.assert(@offsetOf(Caps, "file_workspace_align") == 32);
    std.debug.assert(@sizeOf(NativeTimestamp) == 12);
    std.debug.assert(@sizeOf(NativeStat) == 48);
    std.debug.assert(@offsetOf(NativeStat, "attr") == 44);
    std.debug.assert(@sizeOf(NativeDirent) == 760);
    std.debug.assert(@offsetOf(NativeDirent, "size_bytes") == 744);
    std.debug.assert(@offsetOf(NativeDir, "state") == @sizeOf(usize));
    std.debug.assert(@offsetOf(NativeDir, "state_bytes") == @sizeOf(usize) * 2);
    std.debug.assert(@offsetOf(NativeDir, "is_open") == @sizeOf(usize) * 2 + 4);
    std.debug.assert(@sizeOf(NativeDir) == @sizeOf(usize) * 2 + 8);
    std.debug.assert(@sizeOf(FileState) == @sizeOf(usize));
    std.debug.assert(@sizeOf(DirectoryState) == @sizeOf(NativeDir));
    std.debug.assert(@offsetOf(TransactionState, "stage") == 512);
    std.debug.assert(@offsetOf(TransactionState, "file_state") == 1024);
}
