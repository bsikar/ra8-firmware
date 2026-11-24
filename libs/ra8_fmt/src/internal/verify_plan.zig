//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Header-driven workspace planner for JOF verification.

const std = @import("std");
const c_abi = @import("c_abi");
const convert = @import("convert");
const source_io = @import("source_io");

const png_head_bytes: usize = 26;
const png_chunks_off: u64 = 33;
const png_trns: u32 = 0x74524E53;
const png_idat: u32 = 0x49444154;
const png_chunk_frame: u64 = 12;
const png_color_type_off: usize = 25;

const jpeg_marker: u8 = 0xFF;
const jpeg_soi: u8 = 0xD8;
const jpeg_components_off: u64 = 7;

const webp_form_off: usize = 8;

const png_sig: [8]u8 = .{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

fn probeJpegBpp(source: *const c_abi.Source, bpp: *u8) c_abi.Ra8Err {
    var offset: u64 = 2;
    while (offset < source.size) {
        var byte: [1]u8 = .{0};
        while (true) {
            const rc = source_io.readExactLoop(source, offset, &byte, c_abi.k_ra8_err_protocol_error);
            if (rc != c_abi.k_ra8_ok) return rc;
            offset += 1;
            if (byte[0] == jpeg_marker) break;
        }
        while (true) {
            const rc = source_io.readExactLoop(source, offset, &byte, c_abi.k_ra8_err_protocol_error);
            if (rc != c_abi.k_ra8_ok) return rc;
            offset += 1;
            if (byte[0] != jpeg_marker) break;
        }
        const marker = byte[0];

        var len_bytes: [2]u8 = undefined;
        var rc = source_io.readExactLoop(source, offset, &len_bytes, c_abi.k_ra8_err_protocol_error);
        if (rc != c_abi.k_ra8_ok) return rc;
        const segment = source_io.be16(&len_bytes);
        if (segment < 2) return c_abi.k_ra8_err_protocol_error;

        if (source_io.isJpegSofMarker(marker)) {
            var comp: [1]u8 = .{0};
            const comp_off = std.math.add(u64, offset, jpeg_components_off) catch return c_abi.k_ra8_err_protocol_error;
            rc = source_io.readExactLoop(source, comp_off, &comp, c_abi.k_ra8_err_protocol_error);
            if (rc != c_abi.k_ra8_ok) return rc;
            if (comp[0] != 1 and comp[0] != 3) return c_abi.k_ra8_err_not_supported;
            bpp.* = comp[0];
            return c_abi.k_ra8_ok;
        }
        offset = std.math.add(u64, offset, segment) catch return c_abi.k_ra8_err_protocol_error;
    }
    return c_abi.k_ra8_err_protocol_error;
}

fn probePngBpp(source: *const c_abi.Source, color_type: u8, bpp: *u8) c_abi.Ra8Err {
    switch (color_type) {
        0 => {
            bpp.* = 1;
            return c_abi.k_ra8_ok;
        },
        4, 6 => {
            bpp.* = 4;
            return c_abi.k_ra8_ok;
        },
        2 => {
            bpp.* = 3;
            return c_abi.k_ra8_ok;
        },
        3 => {},
        else => return c_abi.k_ra8_err_not_supported,
    }

    var offset: u64 = png_chunks_off;
    bpp.* = 3;
    while (offset <= source.size and (source.size - offset) >= png_chunk_frame) {
        var chunk: [8]u8 = undefined;
        const rc = source_io.readExactLoop(source, offset, &chunk, c_abi.k_ra8_err_protocol_error);
        if (rc != c_abi.k_ra8_ok) return rc;

        const length = source_io.be32(chunk[0..4]);
        const chunk_type = source_io.be32(chunk[4..8]);
        if (@as(u64, length) > (source.size - offset - png_chunk_frame)) {
            return c_abi.k_ra8_err_protocol_error;
        }
        if (chunk_type == png_trns) bpp.* = 4;
        if (chunk_type == png_idat) return c_abi.k_ra8_ok;
        offset += png_chunk_frame + @as(u64, length);
    }
    return c_abi.k_ra8_err_protocol_error;
}

fn probeBpp(source: *const c_abi.Source, bpp: *u8) c_abi.Ra8Err {
    var head: [png_head_bytes]u8 = undefined;
    const rc = source_io.readExactLoop(source, 0, &head, c_abi.k_ra8_err_protocol_error);
    if (rc != c_abi.k_ra8_ok) return rc;

    if (head[0] == jpeg_marker and head[1] == jpeg_soi) {
        return probeJpegBpp(source, bpp);
    }
    if (std.mem.eql(u8, head[0..8], &png_sig)) {
        return probePngBpp(source, head[png_color_type_off], bpp);
    }
    if (std.mem.eql(u8, head[0..4], "RIFF") and std.mem.eql(u8, head[webp_form_off..][0..4], "WEBP")) {
        bpp.* = 4;
        return c_abi.k_ra8_ok;
    }
    return c_abi.k_ra8_err_not_supported;
}

/// Computes exact phase-reused workspace requirements for JOF verification.
pub fn jofVerifyRequirements(
    comptime deps: c_abi.Deps,
    source_ptr: ?*const c_abi.Source,
    out_ptr: ?*c_abi.JofVerifyRequirements,
) c_abi.Ra8Err {
    const source = source_ptr orelse return c_abi.k_ra8_err_null_ptr;
    if (source.read_at == null) return c_abi.k_ra8_err_null_ptr;
    const out = out_ptr orelse return c_abi.k_ra8_err_null_ptr;

    out.* = std.mem.zeroes(c_abi.JofVerifyRequirements);
    var req = std.mem.zeroes(c_abi.JofConvertRequirements);
    var rc = convert.jofConvertRequirements(deps, source, &req);
    if (rc != c_abi.k_ra8_ok) return rc;

    var bpp: u8 = 0;
    rc = probeBpp(source, &bpp);
    if (rc != c_abi.k_ra8_ok) return rc;

    const ref_work = deps.jof_work_bytes(req.width, req.height, req.width, 1);
    const row_u64: u64 = @as(u64, req.width) * @as(u64, bpp);
    const band_u64: u64 = row_u64 * @as(u64, req.tile_height);
    if (ref_work == 0 or band_u64 > std.math.maxInt(u32)) {
        return c_abi.k_ra8_err_invalid_size;
    }

    out.width = req.width;
    out.height = req.height;
    out.bpp = bpp;
    out.band_height = req.tile_height;
    out.reference_work_bytes = ref_work;
    out.banded_work_bytes = req.work_bytes;
    out.webp_work_bytes = req.webp_work_bytes;
    out.row_bytes = @intCast(row_u64);
    out.band_tile_bytes = @intCast(band_u64);
    out.scratch_bytes = deps.jof_stored_bound(out.band_tile_bytes);
    return source_io.checkStable(source);
}
