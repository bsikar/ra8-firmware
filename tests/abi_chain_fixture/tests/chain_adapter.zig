//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Test-only Zig membrane forwarding documented C ABI values to Rust.

const c = @cImport({
    @cDefine("static_assert", "_Static_assert");
    @cDefine("alignof", "_Alignof");
    @cInclude("stdbool.h");
    @cInclude("ra8_abi_chain.h");
});

const stage_marker: u32 = 0x100;

pub export fn ra8_abi_chain_apply(config: ?*const c.ra8_abi_chain_config_t, out_result: ?*u32) callconv(.c) c.ra8_err_t {
    if (out_result == null or config == null) return c.k_ra8_err_null_ptr;
    if (config.?.zig_tag != c.k_ra8_abi_chain_tag or config.?.reserved0 != 0) {
        return c.k_ra8_err_invalid_arg;
    }
    var downstream: u32 = 0;
    const result = c.ra8_rust_abi_fixture_apply(&config.?.rust, &downstream);
    if (result != c.k_ra8_ok) return result;
    const traced = @addWithOverflow(downstream, stage_marker);
    if (traced[1] != 0) return c.k_ra8_err_invalid_size;
    out_result.?.* = traced[0];
    return c.k_ra8_ok;
}

pub export fn ra8_abi_chain_create(zig_tag: u32, out_handle: ?*?*c.ra8_abi_chain_handle_t) callconv(.c) c.ra8_err_t {
    if (out_handle == null) return c.k_ra8_err_null_ptr;
    if (zig_tag != c.k_ra8_abi_chain_tag) return c.k_ra8_err_invalid_arg;
    return c.ra8_rust_abi_fixture_create(@ptrCast(out_handle));
}

pub export fn ra8_abi_chain_destroy(in_out_handle: ?*?*c.ra8_abi_chain_handle_t) callconv(.c) c.ra8_err_t {
    if (in_out_handle == null) return c.k_ra8_err_null_ptr;
    return c.ra8_rust_abi_fixture_destroy(@ptrCast(in_out_handle));
}
