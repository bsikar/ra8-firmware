//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_blockdev_sdram.h (RA8FW-697): the 64 MiB SDRAM window
//! as a ramdisk. Validates the block count, brings the controller up, then
//! hands the window to the RAM backend, which owns the vtable and state.
//! Replaces ra8_io_blockdev_sdram.c, which is deleted. `bd` and `state`
//! stay opaque here; only ra8_io_blockdev_ram.c reads their layout.

const tag = "ra8_io_blockdev_sdram";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_invalid_size: c_int = 0x105;
pub const err_null_ptr: c_int = 0x504;

/// ra8_sdramc_regs.h: k_ra8_sdram_base_addr, k_ra8_sdram_size_bytes.
pub const window_base: usize = 0x68000000;
pub const window_bytes: u32 = 0x04000000;
/// ra8_io_blockdev.h: k_ra8_io_block_size_bytes.
pub const block_bytes: u32 = 512;

pub const min_blocks: u32 = 1;
pub const max_blocks: u32 = window_bytes / block_bytes;

comptime {
    if (window_bytes % block_bytes != 0) @compileError("window is not whole blocks");
    if (max_blocks != 131072) @compileError("64 MiB / 512 must be 131072");
}

extern fn ra8_sdramc_init() c_int;
extern fn ra8_io_blockdev_ram_init(
    bd: *anyopaque,
    state: *anyopaque,
    storage: [*]u8,
    block_count: u32,
    read_only: bool,
) c_int;
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;

export fn ra8_io_blockdev_sdram_init(bd: ?*anyopaque, state: ?*anyopaque, block_count: u32) c_int {
    const device = bd orelse {
        ra8_log_emit_error(tag, "bd must not be nullptr");
        return err_null_ptr;
    };
    const ram_state = state orelse {
        ra8_log_emit_error(tag, "state must not be nullptr");
        return err_null_ptr;
    };
    if (block_count < min_blocks) return err_invalid_size;
    if (block_count > max_blocks) return err_invalid_size;
    const rc = ra8_sdramc_init();
    if (rc != ok) {
        ra8_log_emit_error(tag, "sdram bring-up");
        ra8_log_emit_error_val(tag, "Error", @bitCast(rc));
        return rc;
    }
    const window: [*]u8 = @ptrFromInt(window_base);
    return ra8_io_blockdev_ram_init(device, ram_state, window, block_count, false);
}
