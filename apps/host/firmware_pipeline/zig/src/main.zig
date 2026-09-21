//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig entry point for the C, Zig, and Rust firmware pipeline.

const std = @import("std");
const c = @cImport({
    @cDefine("static_assert", "_Static_assert");
    @cDefine("alignof", "_Alignof");
    @cInclude("firmware_pipeline.h");
    @cInclude("firmware_pipeline_io_internal.h");
});

extern fn firmware_pipeline_analyze(
    config: ?*const c.firmware_pipeline_config_t,
    data: ?[*]const u8,
    size: usize,
    out_result: ?*c.firmware_pipeline_result_t,
) callconv(.c) c.firmware_pipeline_status_t;

fn fail(message: []const u8) u8 {
    std.debug.print("firmware_pipeline: {s}\n", .{message});
    return 2;
}

pub fn main() u8 {
    const allocator = std.heap.page_allocator;
    const arguments = std.process.argsAlloc(allocator) catch return fail("cannot read arguments");
    defer std.process.argsFree(allocator, arguments);
    if (arguments.len != 2 or arguments[1].len == 0) {
        std.debug.print("usage: firmware_pipeline <firmware-image>\n", .{});
        return 2;
    }

    const ops = c.priv_firmware_pipeline_host_io();
    var image = std.mem.zeroes(c.firmware_pipeline_image_t);
    if (c.priv_firmware_pipeline_read_image(ops, arguments[1].ptr, &image) != c.k_firmware_pipeline_io_ok) {
        return fail("cannot read bounded input");
    }
    defer c.priv_firmware_pipeline_release_image(ops, &image);

    const config = c.firmware_pipeline_config_t{
        .abi_version = c.k_firmware_pipeline_abi_version,
        .reserved0 = 0,
    };
    var placeholder: u8 = 0;
    const data: [*]const u8 = if (image.bytes == null) @ptrCast(&placeholder) else @ptrCast(image.bytes);
    var result = std.mem.zeroes(c.firmware_pipeline_result_t);
    const status = firmware_pipeline_analyze(&config, data, image.size, &result);
    if (status != c.k_firmware_pipeline_ok) {
        return fail(if (status == c.k_firmware_pipeline_empty_image)
            "Rust rejected empty image"
        else
            "language pipeline failed");
    }
    std.io.getStdOut().writer().print(
        "bytes={d}\nzero={d}\nerased={d}\nfnv1a64={x:0>16}\nzig_xor8={x:0>2}\nzig_stage={x:0>2}\n",
        .{
            result.byte_count,
            result.zero_count,
            result.erased_count,
            result.fnv1a64,
            result.zig_xor8,
            result.zig_stage_marker,
        },
    ) catch return fail("cannot write output");
    return 0;
}
