//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Test-private adapter imported from the authoritative public C23 header.

const c = @cImport({
    @cDefine("static_assert", "_Static_assert");
    @cDefine("alignof", "_Alignof");
    @cInclude("stdbool.h");
    @cInclude("ra8_rust_abi_fixture.h");
});

pub const Config = c.ra8_rust_abi_fixture_config_t;
pub const Handle = c.ra8_rust_abi_fixture_t;

pub const ok: u16 = c.k_ra8_ok;
pub const no_memory: u16 = c.k_ra8_err_no_mem;
pub const invalid_argument: u16 = c.k_ra8_err_invalid_arg;
pub const invalid_size: u16 = c.k_ra8_err_invalid_size;
pub const null_pointer: u16 = c.k_ra8_err_null_ptr;

comptime {
    if (@sizeOf(Config) != 8 or @alignOf(Config) != 4) {
        @compileError("Rust ABI configuration layout drifted");
    }
    if (@offsetOf(Config, "value") != 0 or @offsetOf(Config, "factor") != 4 or
        @offsetOf(Config, "enabled") != 6 or @offsetOf(Config, "reserved0") != 7)
    {
        @compileError("Rust ABI configuration offsets drifted");
    }
}

pub fn apply(config: ?*const Config, out_result: ?*u32) u16 {
    return @intCast(c.ra8_rust_abi_fixture_apply(config, out_result));
}

pub fn create(out_handle: ?*?*Handle) u16 {
    return @intCast(c.ra8_rust_abi_fixture_create(out_handle));
}

pub fn failNextAllocation() void {
    c.ra8_rust_abi_fixture_test_fail_next_allocation();
}

pub fn destroy(in_out_handle: ?*?*Handle) u16 {
    return @intCast(c.ra8_rust_abi_fixture_destroy(in_out_handle));
}
