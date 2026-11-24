//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `epub_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("epub_abi");

const k_ra8_err_decomp_entries = sut.k_ra8_err_decomp_entries;
const k_ra8_err_null_ptr = sut.k_ra8_err_null_ptr;
const k_ra8_ok = sut.k_ra8_ok;
const mz_zip_archive = sut.mz_zip_archive;
const mz_zip_archive_file_stat = sut.mz_zip_archive_file_stat;
const priv_epub_zip_guard_archive = sut.priv_epub_zip_guard_archive;
const priv_epub_zip_guard_entry = sut.priv_epub_zip_guard_entry;
const ra8_err_t = sut.ra8_err_t;

extern var teststub_num_files: c_uint;

test "archive guard rejects a null zip" {
    try std.testing.expectEqual(k_ra8_err_null_ptr, priv_epub_zip_guard_archive(null));
}

test "entry guard rejects a null stat" {
    try std.testing.expectEqual(k_ra8_err_null_ptr, priv_epub_zip_guard_entry(null));
}

test "archive guard rejects the many-tiny-entries bomb and passes a small archive" {
    var archive_probe: u8 = 0;
    const archive: *mz_zip_archive = @ptrCast(&archive_probe);

    teststub_num_files = 5; // stub policy caps at 4
    try std.testing.expectEqual(k_ra8_err_decomp_entries, priv_epub_zip_guard_archive(archive));

    teststub_num_files = 3;
    try std.testing.expectEqual(k_ra8_ok, priv_epub_zip_guard_archive(archive));
}

test "entry guard forwards the declared sizes to the policy" {
    var stat = mz_zip_archive_file_stat{
        .m_file_index = 0,
        .m_central_dir_ofs = 0,
        .m_version_made_by = 0,
        .m_version_needed = 0,
        .m_bit_flag = 0,
        .m_method = 0,
        .m_crc32 = 0,
        .m_comp_size = 0,
        .m_uncomp_size = 0,
    };
    try std.testing.expectEqual(k_ra8_ok, priv_epub_zip_guard_entry(&stat));

    stat.m_uncomp_size = 1; // stub policy flags any nonzero declared output
    try std.testing.expectEqual(@as(ra8_err_t, 0x505), priv_epub_zip_guard_entry(&stat));
}
