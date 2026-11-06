//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the FAT file I/O port (`ra8_fs_fat_fileio_abi.zig`):
//! the read path positions to the cluster covering the offset (resuming the
//! chain walk from the cached waypoint on a sequential read, re-walking from
//! the head after a backward seek), serves zeros past an exFAT file's
//! ValidDataLength, and reports short reads at EOF; the write path allocates
//! the first cluster on demand, grows the chain with its own forward
//! waypoint, patches the on-disk dir entry (cluster, size, mtime, archive
//! bit), and refuses a write that would cross the 4 GiB FAT ceiling or a
//! read-only handle before touching the volume.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const fake = @import("fs_walker_fake.zig");
const st = fs.fat_state;
const fio = fs.fat_fileio;

const fat32: u8 = c.k_ra8_fs_type_fat32;
const exfat: u8 = c.k_ra8_fs_type_exfat;

const ok: u16 = c.k_ra8_ok;
const err_null_ptr: u16 = c.k_ra8_err_null_ptr;
const err_invalid_state: u16 = c.k_ra8_err_invalid_state;
const err_invalid_size: u16 = c.k_ra8_err_invalid_size;
const err_no_mem: u16 = c.k_ra8_err_no_mem;

const eoc32: u32 = 0xFFFF_FFF8;
const bps: u32 = 512;

/// A file handle with the common fields set: open for writing, mounted,
/// offset and size at zero, read waypoint cleared.
fn file() *c.ra8_fs_file_t {
    const f = &st.s_files[0];
    f.* = std.mem.zeroes(c.ra8_fs_file_t);
    f.in_use = 1;
    f.mode = c.k_ra8_fs_mode_write;
    f.mount = fake.mount();
    return f;
}

fn readonly() *c.ra8_fs_file_t {
    const f = file();
    f.mode = c.k_ra8_fs_mode_read;
    return f;
}

/// Seed the I/O scratch sector with a rising byte pattern; an unhandled
/// data-cluster LBA reads it back unchanged, which is what the read path
/// asserts against.
fn seedIO() void {
    for (&fake.io_sector, 0..) |*b, i| b.* = @truncate(i * 7 + 3);
}

fn pattern(comptime n: usize) [n]u8 {
    var buf: [n]u8 = undefined;
    for (&buf, 0..) |*b, i| b.* = @truncate(i * 7 + 3);
    return buf;
}

fn fatMount() void {
    fake.reset(fat32);
    fake.mount_store.count_of_clusters = 28;
    // The fake's FAT32 geometry: first_data_lba 7, so cluster N lands on
    // LBA 7 + (N - 2); nothing serves those LBAs, so reads return the
    // scratch sector's standing contents and writes land on io_sector.
    seedIO();
}

test "skip_clusters walks n clusters and stops on EOC" {
    fatMount();
    fake.fat[2] = 3;
    fake.fat[3] = 4;
    fake.fat[4] = eoc32;
    var out: u32 = 0;
    try std.testing.expectEqual(ok, fio.internal_skip_clusters(fake.mount(), 2, 2, &out));
    try std.testing.expectEqual(@as(u32, 4), out);
    out = 0;
    try std.testing.expectEqual(err_invalid_state, fio.internal_skip_clusters(fake.mount(), 2, 5, &out));
    try std.testing.expectEqual(@as(u32, 4), out);
}

test "read_one_chunk serves the sector slice at the offset" {
    fatMount();
    const f = file();
    f.first_cluster = 2;
    f.no_fat_chain = 0;
    var take: u32 = 0;
    var buf: [16]u8 = @splat(0xAA);
    try std.testing.expectEqual(ok, fio.internal_read_one_chunk(f, &buf, 16, &take));
    try std.testing.expectEqual(@as(u32, 16), take);
    try std.testing.expectEqualSlices(u8, &pattern(16), &buf);
    try std.testing.expectEqual(@as(u32, 2), f.cur_cluster);
    // The waypoint cache is set: cluster index 0 -> cluster 2.
    try std.testing.expectEqual(@as(u32, 0), f.walk_cache_idx);
    try std.testing.expectEqual(@as(u32, 2), f.walk_cache_cluster);
}

test "read resumes from the waypoint cache on a sequential read" {
    fatMount();
    fake.fat[2] = 3;
    fake.fat[3] = eoc32;
    const f = file();
    f.first_cluster = 2;
    f.offset = bps; // cluster index 1
    f.walk_cache_cluster = 2;
    f.walk_cache_idx = 0;
    var take: u32 = 0;
    var buf: [4]u8 = @splat(0);
    try std.testing.expectEqual(ok, fio.internal_read_one_chunk(f, &buf, 4, &take));
    // One hop from the cache, not a re-walk from the head.
    try std.testing.expectEqual(@as(u32, 3), f.cur_cluster);
    try std.testing.expectEqual(@as(u32, 1), f.walk_cache_idx);
    try std.testing.expectEqual(@as(u32, 3), f.walk_cache_cluster);
}

test "a backward seek re-walks from the chain head" {
    fatMount();
    fake.fat[2] = 3;
    fake.fat[3] = eoc32;
    const f = file();
    f.first_cluster = 2;
    f.offset = 4; // cluster index 0, behind the cache
    f.walk_cache_cluster = 3;
    f.walk_cache_idx = 1;
    var take: u32 = 0;
    var buf: [4]u8 = @splat(0);
    try std.testing.expectEqual(ok, fio.internal_read_one_chunk(f, &buf, 4, &take));
    try std.testing.expectEqual(@as(u32, 2), f.cur_cluster);
}

test "exFAT reads serve zeros past ValidDataLength" {
    fake.reset(exfat);
    seedIO();
    const f = file();
    f.no_fat_chain = 1;
    f.first_cluster = 6; // LBA 5: served by no fixture
    f.size_bytes = 20;
    f.valid_bytes = 10;
    var take: u32 = 0;
    var buf: [4]u8 = @splat(0xFF);
    // In the hole: offset 10 >= valid 10, zeros only.
    f.offset = 10;
    try std.testing.expectEqual(ok, fio.internal_read_span(f, &buf, 4, &take));
    try std.testing.expectEqual(@as(u32, 4), take);
    try std.testing.expectEqualSlices(u8, &@as([4]u8, @splat(0)), &buf);
    // Crossing out of the prefix: the span is clipped to valid - offset.
    f.offset = 5;
    try std.testing.expectEqual(ok, fio.internal_read_span(f, &buf, 10, &take));
    try std.testing.expectEqual(@as(u32, 5), take);
    try std.testing.expectEqualSlices(u8, pattern(5)[0..5], &buf);
}

test "a FAT handle has no hole: its size is its valid length" {
    fatMount();
    const f = file();
    f.first_cluster = 2;
    f.size_bytes = 8;
    var take: u32 = 0;
    var buf: [8]u8 = @splat(0);
    try std.testing.expectEqual(ok, fio.internal_read_span(f, &buf, 8, &take));
    try std.testing.expectEqual(@as(u32, 8), take);
    try std.testing.expectEqualSlices(u8, pattern(8), &buf);
}

test "ra8_fs_read guards and short reads" {
    fatMount();
    const f = file();
    f.first_cluster = 2;
    f.size_bytes = 8;
    var got: u32 = 7;
    var buf: [16]u8 = @splat(0);
    try std.testing.expectEqual(err_null_ptr, fio.ra8_fs_read(null, &buf, 8, &got));
    try std.testing.expectEqual(err_null_ptr, fio.ra8_fs_read(f, null, 8, &got));
    f.in_use = 0;
    try std.testing.expectEqual(err_invalid_state, fio.ra8_fs_read(f, &buf, 8, &got));
    f.in_use = 1;
    // Past EOF: nothing, no error.
    f.offset = 8;
    try std.testing.expectEqual(ok, fio.ra8_fs_read(f, &buf, 8, &got));
    try std.testing.expectEqual(@as(u32, 0), got);
    // Short read at EOF: max_len clipped to what is left.
    f.offset = 4;
    try std.testing.expectEqual(ok, fio.ra8_fs_read(f, &buf, 100, &got));
    try std.testing.expectEqual(@as(u32, 4), got);
    try std.testing.expectEqual(@as(u64, 8), f.offset);
    try std.testing.expectEqualSlices(u8, pattern(4), buf[0..4]);
}

test "alloc_eoc_cluster marks the new cluster EOC" {
    fatMount();
    var out: u32 = 0;
    try std.testing.expectEqual(ok, fio.priv_alloc_eoc_cluster(fake.mount(), &out));
    try std.testing.expectEqual(@as(u32, 2), out);
    try std.testing.expectEqual(eoc32, fake.fat[2]);
}

test "walk_grow extends a chain that ends early" {
    fatMount();
    fake.fat[2] = eoc32;
    var out: u32 = 0;
    try std.testing.expectEqual(ok, fio.internal_walk_grow(fake.mount(), 2, 2, &out));
    try std.testing.expectEqual(@as(u32, 4), out);
    try std.testing.expectEqual(@as(u32, 3), fake.fat[2]);
    try std.testing.expectEqual(@as(u32, 4), fake.fat[3]);
    try std.testing.expectEqual(eoc32, fake.fat[4]);
}

test "write_into_sector is a read-modify-write" {
    fatMount();
    fake.io_sector[9] = 0x5A;
    fake.io_sector[13] = 0x5A;
    var src = [3]u8{ 'X', 'Y', 'Z' };
    try std.testing.expectEqual(ok, fio.priv_write_into_sector(fake.mount(), 7, 10, &src, 3));
    try std.testing.expectEqual(@as(u8, 0x5A), fake.io_sector[9]);
    try std.testing.expectEqualSlices(u8, &src, fake.io_sector[10..13]);
    try std.testing.expectEqual(@as(u8, 0x5A), fake.io_sector[13]);
    try std.testing.expectEqual(@as(u64, 7), fake.sector_written.?);
}

test "write_position allocates the first cluster on demand" {
    fatMount();
    const f = file();
    var way = fio.WriteWay{ .cluster = 0, .index = 0 };
    var out: u32 = 0;
    try std.testing.expectEqual(ok, fio.internal_write_position(f, &way, 0, &out));
    try std.testing.expectEqual(@as(u32, 2), out);
    try std.testing.expectEqual(@as(u32, 2), f.first_cluster);
    try std.testing.expectEqual(@as(u32, 2), f.cur_cluster);
    try std.testing.expectEqual(eoc32, fake.fat[2]);
    // A cursor behind the waypoint is a caller bug, refused.
    try std.testing.expectEqual(err_invalid_state, fio.internal_write_position(f, &way, 0, &out));
}

test "write_stream grows the chain and advances the size" {
    fatMount();
    const f = file();
    var data: [1000]u8 = undefined;
    for (&data, 0..) |*b, i| b.* = @truncate(i * 5 + 1);
    try std.testing.expectEqual(ok, fio.internal_write_stream(f, &data, 1000));
    try std.testing.expectEqual(@as(u64, 1000), f.size_bytes);
    try std.testing.expectEqual(@as(u64, 1000), f.offset);
    try std.testing.expectEqual(@as(u32, 3), fake.fat[2]);
    try std.testing.expectEqual(eoc32, fake.fat[3]);
    // The second sector's slice is the last thing written to io_sector.
    try std.testing.expectEqualSlices(u8, data[512..1000], fake.io_sector[0..488]);
    // The read waypoint cache was dropped: a write may grow the chain.
    try std.testing.expectEqual(@as(u32, 0), f.walk_cache_cluster);
}

test "ra8_fs_write patches the dir entry and stamps the change" {
    fatMount();
    const f = file();
    f.first_cluster = 0;
    f.dir_entry_lba = fake.dir_lba;
    f.dir_entry_idx = 0;
    var data: [600]u8 = @splat(0x42);
    try std.testing.expectEqual(ok, fio.ra8_fs_write(f, &data, 600));
    try std.testing.expectEqual(@as(u8, 1), f.dirty);
    // dir_entry_lba 4 is served from the dir fixture: the patched entry
    // names the first cluster and the new size, and carries the archive bit.
    try std.testing.expectEqual(@as(u32, 2), @as(u32, fake.sector[26]) | (@as(u32, fake.sector[27]) << 8));
    try std.testing.expectEqual(@as(u32, 600), std.mem.readInt(u32, fake.sector[28..32], .little));
    try std.testing.expectEqual(@as(u8, 0x20), fake.sector[11] & 0x20);
}

test "ra8_fs_write refuses read-only, null and oversized writes" {
    fatMount();
    const f = readonly();
    var data: [4]u8 = @splat(0);
    try std.testing.expectEqual(err_invalid_state, fio.ra8_fs_write(f, &data, 4));
    try std.testing.expectEqual(err_null_ptr, fio.ra8_fs_write(f, null, 4));
    try std.testing.expect(fake.sector_written == null);
    f.mode = c.k_ra8_fs_mode_write;
    try std.testing.expectEqual(ok, fio.ra8_fs_write(f, &data, 0));
    // A write that would push past 4 GiB - 1 cannot be recorded: refused
    // whole, nothing on disk.
    f.offset = 1;
    f.size_bytes = 1;
    try std.testing.expectEqual(err_invalid_size, fio.ra8_fs_write(f, &data, 0xFFFF_FFFF));
    try std.testing.expect(fake.sector_written == null);
}

test "write_file guards a null or unmounted handle" {
    fatMount();
    var data: [4]u8 = @splat(0);
    try std.testing.expectEqual(err_null_ptr, fio.ra8_fs_write_file(null, "/", &data, 4));
    try std.testing.expectEqual(err_null_ptr, fio.ra8_fs_write_file(fake.mount(), null, &data, 4));
    try std.testing.expectEqual(err_null_ptr, fio.ra8_fs_write_file(fake.mount(), "/", null, 4));
    fake.mount_store.in_use = 0;
    try std.testing.expectEqual(err_invalid_state, fio.ra8_fs_write_file(fake.mount(), "/", &data, 4));
}

test "seek clamps to the size; tell and size report the cursor" {
    fatMount();
    const f = file();
    f.size_bytes = 100;
    try std.testing.expectEqual(ok, fio.ra8_fs_seek(f, 150));
    try std.testing.expectEqual(@as(u64, 100), f.offset);
    try std.testing.expectEqual(ok, fio.ra8_fs_seek(f, 42));
    var out: u64 = 0;
    try std.testing.expectEqual(ok, fio.ra8_fs_tell(f, &out));
    try std.testing.expectEqual(@as(u64, 42), out);
    try std.testing.expectEqual(ok, fio.ra8_fs_size(f, &out));
    try std.testing.expectEqual(@as(u64, 100), out);
    // Guards: nulls and a closed handle.
    try std.testing.expectEqual(err_null_ptr, fio.ra8_fs_seek(null, 1));
    try std.testing.expectEqual(err_null_ptr, fio.ra8_fs_tell(f, null));
    try std.testing.expectEqual(err_null_ptr, fio.ra8_fs_size(f, null));
    f.in_use = 0;
    try std.testing.expectEqual(err_invalid_state, fio.ra8_fs_seek(f, 1));
    try std.testing.expectEqual(err_invalid_state, fio.ra8_fs_tell(f, &out));
    try std.testing.expectEqual(err_invalid_state, fio.ra8_fs_size(f, &out));
}
