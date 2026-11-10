//! Tests for the FAT open/close path (`src/ra8_fs_file_abi.zig`), the port of
//! the former `ra8_fs_fat_file.c`. The shared walker fakes stand in for the
//! backend (`priv_sec_walk`, sector IO, the file-slot pool); this file adds
//! stand-ins for the resolver's directory finders, the insert planner, and the
//! stamp helpers, which stay C until their own slices.

const std = @import("std");
const c = @import("ra8_fs").c;
const st = @import("ra8_fs").fat_state;
const fake = @import("fs_walker_fake.zig");
const ra8_fs = @import("ra8_fs");

const entry_bytes: usize = @intCast(c.k_ra8_fs_dir_entry_bytes);
const off_attr: usize = @intCast(c.k_dir_off_attr);
const off_fst_clus_hi: usize = @intCast(c.k_dir_off_fst_clus_hi);
const off_fst_clus_lo: usize = @intCast(c.k_dir_off_fst_clus_lo);
const off_file_size: usize = @intCast(c.k_dir_off_file_size);
const attr_read_only: u8 = @intCast(c.k_ra8_fs_attr_read_only);
const attr_hidden: u8 = @intCast(c.k_ra8_fs_attr_hidden);
const attr_system: u8 = @intCast(c.k_ra8_fs_attr_system);
const attr_directory: u8 = @intCast(c.k_ra8_fs_attr_directory);
const attr_archive: u8 = @intCast(c.k_ra8_fs_attr_archive);
const mode_read = c.k_ra8_fs_mode_read;
const mode_write = c.k_ra8_fs_mode_write;
const mode_append = c.k_ra8_fs_mode_append;

const ok: u16 = c.k_ra8_ok;
const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
const err_invalid_state: u16 = c.k_ra8_err_invalid_state;
const err_not_found: u16 = c.k_ra8_err_not_found;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_no_mem: u16 = c.k_ra8_err_no_mem;
const err_access_denied: u16 = c.k_ra8_err_access_denied;

// ---- helpers ----

/// Lay an entry image into a plain buffer (no name needed: decode tests).
fn layBuf(out: *[entry_bytes]u8, attr: u8, cluster: u32, size: u32) void {
    out.* = @splat(0);
    out[off_attr] = attr;
    out[off_fst_clus_hi] = @intCast((cluster >> 16) & 0xFF);
    out[off_fst_clus_hi + 1] = @intCast((cluster >> 24) & 0xFF);
    out[off_fst_clus_lo] = @intCast(cluster & 0xFF);
    out[off_fst_clus_lo + 1] = @intCast((cluster >> 8) & 0xFF);
    std.mem.writeInt(u32, out[off_file_size..][0..4], size, .little);
}

/// The 11-byte name field the 8.3 find matches for "a.txt".
const a_txt = "A\x20\x20\x20\x20\x20\x20\x20TXT".*;
/// The 11-byte name field for "notes" (a directory component).
const notes = "NOTES\x20\x20\x20\x20\x20\x20".*;

/// Lay a FAT dir entry with the given attribute, first cluster and size into
/// the walk buffer at slot `at`, so the real 8.3 find matches it by name.
fn layEntry(at: usize, sfn11: *const [11]u8, attr: u8, cluster: u32, size: u32) void {
    fake.lay83(at, sfn11, attr, cluster);
    fake.sector[at * entry_bytes + off_fst_clus_hi] = @intCast((cluster >> 16) & 0xFF);
    fake.sector[at * entry_bytes + off_fst_clus_hi + 1] = @intCast((cluster >> 24) & 0xFF);
    std.mem.writeInt(u32, fake.sector[at * entry_bytes + off_file_size ..][0..4], size, .little);
}

fn setupFat() void {
    fake.reset(0); // FAT volume
}

test "entry_first_cluster decodes the hi:lo halves" {
    var e: [entry_bytes]u8 = @splat(0);
    layBuf(&e, attr_archive, 0x00012345, 0);
    try std.testing.expectEqual(@as(u32, 0x00012345), ra8_fs.file.priv_entry_first_cluster(&e));
    layBuf(&e, attr_archive, 0x00001234, 0);
    try std.testing.expectEqual(@as(u32, 0x00001234), ra8_fs.file.priv_entry_first_cluster(&e));
}

test "entry_set_cluster_size writes cluster and size in one call" {
    var e: [entry_bytes]u8 = @splat(0);
    layBuf(&e, attr_archive, 0x21, 0x999);
    ra8_fs.file.priv_entry_set_cluster_size(&e, 0x000ABCDE, 0x7777);
    const hi = std.mem.readInt(u16, e[off_fst_clus_hi..][0..2], .little);
    const lo = std.mem.readInt(u16, e[off_fst_clus_lo..][0..2], .little);
    try std.testing.expectEqual(@as(u32, 0x000ABCDE), (@as(u32, hi) << 16) | lo);
    try std.testing.expectEqual(@as(u32, 0x7777), std.mem.readInt(u32, e[off_file_size..][0..4], .little));
}

test "apply_attr sets and clears independent bits" {
    var e: [entry_bytes]u8 = @splat(0);
    e[off_attr] = attr_read_only | attr_archive;
    ra8_fs.file.priv_fat_entry_apply_attr(&e, attr_system, attr_read_only);
    try std.testing.expectEqual(attr_system | attr_archive, e[off_attr]);
    ra8_fs.file.priv_fat_entry_apply_attr(&e, attr_hidden, 0);
    try std.testing.expectEqual(attr_hidden | attr_system | attr_archive, e[off_attr]);
}

test "open existing refuses a directory entry before consuming a slot" {
    setupFat();
    // the walk fake serves one buffer for every component, so both the
    // "notes" parent and the "a.txt" leaf live in it
    layEntry(0, &notes, attr_directory, 0x21, 0);
    layEntry(1, &a_txt, attr_directory, 0x21, 0);
    var f: ?*c.ra8_fs_file_t = null;
    const err = ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "notes/a.txt", mode_read, &f);
    try std.testing.expectEqual(err_invalid_arg, err);
    try std.testing.expectEqual(@as(u32, 0), fake.usedFiles());
    try std.testing.expect(f == null);
}

test "open existing refuses a write mode on a read-only file" {
    setupFat();
    layEntry(0, &a_txt, attr_read_only | attr_archive, 0x21, 0x1234);
    var f: ?*c.ra8_fs_file_t = null;
    const err = ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "a.txt", mode_write, &f);
    try std.testing.expectEqual(err_access_denied, err);
    try std.testing.expectEqual(@as(u32, 0), fake.usedFiles());
    // a read open is untouched by the bit
    const err2 = ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "a.txt", mode_read, &f);
    try std.testing.expectEqual(ok, err2);
    try std.testing.expect(f != null);
}

test "open existing populates the handle from the entry" {
    setupFat();
    layEntry(0, &a_txt, attr_archive, 0x21, 0x1234);
    var f: ?*c.ra8_fs_file_t = null;
    const err = ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "a.txt", mode_read, &f);
    try std.testing.expectEqual(ok, err);
    try std.testing.expectEqual(@as(u32, 1), fake.usedFiles());
    const h = f.?;
    try std.testing.expectEqual(@as(u32, 0x21), h.first_cluster);
    try std.testing.expectEqual(@as(u32, 0x1234), h.size_bytes);
    try std.testing.expectEqual(@as(u64, 0), h.offset);
    try std.testing.expectEqual(fake.walk_lba, h.dir_entry_lba);
    try std.testing.expectEqual(@as(u32, 0), h.dir_entry_idx);
    try std.testing.expectEqual(@as(u32, 1), h.in_use);
}

test "append mode starts the offset at the file size" {
    setupFat();
    layEntry(0, &a_txt, attr_archive, 0x21, 0x1234);
    var f: ?*c.ra8_fs_file_t = null;
    const err = ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "a.txt", mode_append, &f);
    try std.testing.expectEqual(ok, err);
    try std.testing.expectEqual(@as(u64, 0x1234), f.?.offset);
}

test "write mode truncates the entry, stamps it, and sets archive" {
    setupFat();
    layEntry(0, &a_txt, attr_archive, 0x21, 0x1234);
    var f: ?*c.ra8_fs_file_t = null;
    const err = ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "a.txt", mode_write, &f);
    try std.testing.expectEqual(ok, err);
    try std.testing.expectEqual(@as(u32, 0), f.?.first_cluster);
    try std.testing.expectEqual(@as(u32, 0), f.?.size_bytes);
    try std.testing.expectEqual(@as(u64, 0), f.?.offset);
    // the on-disk entry was rewritten: zero cluster+size, archive set, mtime stamp
    // the real find answers at slot 0: the truncate rewrote THAT entry
    try std.testing.expectEqual(@as(u32, 0), std.mem.readInt(u32, fake.sector[off_file_size..][0..4], .little));
    try std.testing.expectEqual(@as(u16, 0x21), std.mem.readInt(u16, fake.sector[24..26], .little));
    try std.testing.expectEqual(attr_archive, fake.sector[off_attr] & (attr_archive | attr_directory));
    try std.testing.expectEqual(fake.walk_lba, fake.sector_written.?);
}

test "truncate failure frees the slot and reports the error" {
    setupFat();
    layEntry(0, &a_txt, attr_archive, 0x21, 0x1234);
    fake.sector_write_err = 0x21;
    var f: ?*c.ra8_fs_file_t = null;
    const err = ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "a.txt", mode_write, &f);
    try std.testing.expectEqual(@as(u16, 0x21), err);
    try std.testing.expect(f == null);
    // every slot the open took was released
    try std.testing.expectEqual(@as(u32, 0), st.s_files[0].in_use);
    try std.testing.expectEqual(@as(u32, 0), st.s_files[1].in_use);
    try std.testing.expectEqual(@as(u32, 0), fake.usedFiles());
}

test "create_new carves an empty archive entry and stamps its creation" {
    setupFat();
    var f: ?*c.ra8_fs_file_t = null;
    const err = ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "new.bin", mode_write, &f);
    try std.testing.expectEqual(ok, err);
    const h = f.?;
    try std.testing.expectEqual(@as(u32, 0), h.first_cluster);
    try std.testing.expectEqual(@as(u32, 0), h.size_bytes);
    try std.testing.expectEqual(@as(u64, 0), h.offset);
    try std.testing.expectEqual(fake.walk_lba, h.dir_entry_lba);
    try std.testing.expectEqual(@as(u32, 0), h.dir_entry_idx);
    try std.testing.expectEqual(@as(u32, 1), h.in_use);
    // the committed template: archive attribute, empty, creation-stamped
    try std.testing.expectEqual(attr_archive, fake.sector[off_attr]);
    try std.testing.expectEqual(@as(u32, 0), std.mem.readInt(u32, fake.sector[off_file_size..][0..4], .little));
    try std.testing.expectEqual(@as(u16, 0x21), std.mem.readInt(u16, fake.sector[16..18], .little));
}

test "a read failure leaves the pool untouched" {
    setupFat();
    fake.sector_read_err = err_access_denied;
    var f: ?*c.ra8_fs_file_t = null;
    const err = ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "new.bin", mode_write, &f);
    try std.testing.expectEqual(err_access_denied, err);
    try std.testing.expectEqual(@as(u32, 0), fake.usedFiles());
}

test "not-found in read mode is not_found, not a create" {
    setupFat();
    var f: ?*c.ra8_fs_file_t = null;
    const err = ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "a.txt", mode_read, &f);
    try std.testing.expectEqual(err_not_found, err);
    try std.testing.expectEqual(@as(u32, 0), fake.usedFiles());
}

test "open_locked guards: null handle and unmounted volume" {
    setupFat();
    var f: ?*c.ra8_fs_file_t = null;
    try std.testing.expectEqual(err_null_ptr, ra8_fs.file.priv_open_locked(null, "a", mode_read, &f));
    fake.mount_store.in_use = 0;
    try std.testing.expectEqual(err_invalid_state, ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "a", mode_read, &f));
}

test "pool exhaustion is no_mem" {
    setupFat();
    st.filesFill();
    layEntry(0, &a_txt, attr_archive, 0x21, 0x1234);
    var f: ?*c.ra8_fs_file_t = null;
    try std.testing.expectEqual(err_no_mem, ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "a.txt", mode_read, &f));
}

test "close stamps a dirty handle" {
    setupFat();
    layEntry(0, &a_txt, attr_archive, 0x21, 0x1234);
    var f: ?*c.ra8_fs_file_t = null;
    _ = ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "a.txt", mode_read, &f);
    f.?.dirty = 1;
    const err = ra8_fs.file.priv_close_locked(f);
    try std.testing.expectEqual(ok, err);
    try std.testing.expectEqual(@as(u16, 0x21), std.mem.readInt(u16, fake.sector[24..26], .little));
    try std.testing.expectEqual(fake.walk_lba, fake.sector_written.?);
    try std.testing.expectEqual(@as(u32, 0), f.?.in_use);
    try std.testing.expect(f.?.mount == null);
}

test "close on a clean handle writes nothing" {
    setupFat();
    layEntry(0, &a_txt, attr_archive, 0x21, 0x1234);
    var f: ?*c.ra8_fs_file_t = null;
    _ = ra8_fs.file.priv_open_locked(@constCast(fake.mount()), "a.txt", mode_read, &f);
    const err = ra8_fs.file.priv_close_locked(f);
    try std.testing.expectEqual(ok, err);
    try std.testing.expectEqual(@as(?u64, null), fake.sector_written);
}

test "close_locked guards: null and already-closed" {
    try std.testing.expectEqual(err_null_ptr, ra8_fs.file.priv_close_locked(null));
    var f: c.ra8_fs_file_t = std.mem.zeroes(c.ra8_fs_file_t);
    f.in_use = 0;
    try std.testing.expectEqual(ok, ra8_fs.file.priv_close_locked(&f));
}

test "ra8_fs_open and ra8_fs_close run the lock brackets end to end" {
    setupFat();
    layEntry(0, &a_txt, attr_archive, 0x21, 0x1234);
    var f: ?*c.ra8_fs_file_t = null;
    const oerr = ra8_fs.file.ra8_fs_open(@constCast(fake.mount()), "a.txt", mode_read, &f);
    try std.testing.expectEqual(ok, oerr);
    try std.testing.expect(f != null);
    const cerr = ra8_fs.file.ra8_fs_close(f);
    try std.testing.expectEqual(ok, cerr);
    try std.testing.expectEqual(@as(u32, 0), f.?.in_use);
}
