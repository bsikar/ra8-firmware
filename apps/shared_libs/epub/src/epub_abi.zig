//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig implementation of the epub decompression guard (RA8FW-1160): the C-era
//! src/epub_zip_guard.c ported verbatim behind the unchanged C headers. The
//! two guards bind every miniz ZIP consumer (EPUB open, chapter / cover /
//! resource extraction, the iterative entry reader, and the comic CBZ
//! backend through epub_miniz_alloc's shared pool) to the unified
//! decompression-limits policy: the central-directory entry cap at open, and
//! the declared-sizes check before each entry inflates. The miniz and policy
//! symbols stay extern -- miniz is SOUP and stays C, and the policy is
//! ra8_core's Zig module reached through its own C ABI -- so this TU adds no
//! policy of its own. The epub core (epub_open_abi.zig, epub_chapter_abi.zig
//! and epub_entry_abi.zig) is Zig and reaches these exports through the
//! archive link, so the C callers and the C++ tests stay untouched.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
pub const ra8_err_t = u16;
pub const k_ra8_ok: ra8_err_t = 0;
pub const k_ra8_err_null_ptr: ra8_err_t = 0x504;
pub const k_ra8_err_decomp_entries: ra8_err_t = 0x507;

// ---- miniz (SOUP, apps/shared_libs/third_party/miniz/miniz.h) --------------
// The guard only passes the two handles through and reads the declared
// sizes, so miniz stays an opaque handle plus the stat prefix the checks
// read. The layout mirrors miniz.h's mz_zip_archive_file_stat through
// m_uncomp_size.
pub const mz_zip_archive = opaque {};
pub const mz_zip_archive_file_stat = extern struct {
    m_file_index: c_uint,
    m_central_dir_ofs: u64,
    m_version_made_by: c_ushort,
    m_version_needed: c_ushort,
    m_bit_flag: c_ushort,
    m_method: c_ushort,
    m_crc32: c_uint,
    m_comp_size: u64,
    m_uncomp_size: u64,
};

extern fn mz_zip_reader_get_num_files(pZip: ?*mz_zip_archive) c_uint;

// ---- ra8_decomp_limits.h (libs/ra8_core/inc) -------------------------------
const ra8_decomp_limits_t = extern struct {
    max_output_bytes: u64,
    max_ratio: c_uint,
    ratio_grace_bytes: c_uint,
    max_entries: c_uint,
    max_iterations: c_uint,
    max_depth: u8,
};

extern fn ra8_decomp_limits_default() callconv(.c) ra8_decomp_limits_t;
extern fn ra8_decomp_check_declared(limits: ?*const ra8_decomp_limits_t, comp_size: u64, out_size: u64) callconv(.c) ra8_err_t;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

// Test-only knob, defined by teststub/zip_guard_test_stubs.c.

const s_tag_zip_guard: [:0]const u8 = "epub_zip";

pub export fn priv_epub_zip_guard_archive(zip: ?*mz_zip_archive) callconv(.c) ra8_err_t {
    if (zip == null) {
        ra8_log_emit_error(s_tag_zip_guard, "guard archive: null zip");
        return k_ra8_err_null_ptr;
    }
    const limits = ra8_decomp_limits_default();
    const num: u64 = @intCast(mz_zip_reader_get_num_files(zip));
    if (num > @as(u64, limits.max_entries)) {
        return k_ra8_err_decomp_entries;
    }
    return k_ra8_ok;
}

pub export fn priv_epub_zip_guard_entry(st: ?*const mz_zip_archive_file_stat) callconv(.c) ra8_err_t {
    if (st == null) {
        ra8_log_emit_error(s_tag_zip_guard, "guard entry: null stat");
        return k_ra8_err_null_ptr;
    }
    const limits = ra8_decomp_limits_default();
    return ra8_decomp_check_declared(&limits, st.?.m_comp_size, st.?.m_uncomp_size);
}
