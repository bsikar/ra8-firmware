//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_blockdev_xspi.h (RA8FW-1085): the OSPI NOR block-device
//! backend over the ra8_xspi HAL. Reads and programs are chunked to the
//! controller's per-transfer limit; an arbitrary write runs a whole-sector
//! read-modify-write through a module-static 4 KiB scratch buffer (read the
//! sector, overlay the touched blocks, erase, program back). Replaces
//! ra8_io_blockdev_xspi.c, which is deleted.

const blockdev = @import("ra8_io_blockdev_abi.zig");
const sdhi = @import("ra8_io_blockdev_sdhi_abi.zig");
const mram = @import("ra8_io_blockdev_mram_abi.zig");

const tag = "ra8_io_blockdev_xspi";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok = blockdev.ok;
pub const err_invalid_arg: c_int = 0x103;
pub const err_not_supported: c_int = 0x107;
pub const err_out_of_range: c_int = 0x208;
pub const err_null_ptr = blockdev.err_null_ptr;

pub const Caps = blockdev.Caps;
pub const Iface = blockdev.Iface;
pub const Device = blockdev.Device;

/// Mirror of k_ra8_io_block_size_bytes (ra8_io_blockdev.h).
pub const block_bytes: u32 = sdhi.block_bytes;

/// OSPI NOR backend layout constants (k_xspi_* in the replaced unit).
pub const sector_bytes: u32 = 4096;
pub const blocks_per_sector: u32 = 8;
pub const program_bytes: u32 = 256;
pub const max_xfer: u32 = 4096;
pub const instance_count: u8 = 2;

/// Mirror of k_ra8_io_erase_value_ones (ra8_io_blockdev.h).
pub const erase_value_ones: u8 = mram.erase_value_ones;

/// Mirror of ra8_io_blockdev_xspi_state_t (inc/ra8_io_blockdev_xspi.h).
pub const State = extern struct {
    instance: u8,
    base_off: u32,
    block_count: u32,
    read_only: bool,
};

comptime {
    if (@offsetOf(State, "base_off") != 4) @compileError("xspi state layout drifted");
    if (@offsetOf(State, "block_count") != 8) @compileError("xspi state layout drifted");
    if (@offsetOf(State, "read_only") != 12) @compileError("xspi state layout drifted");
    if (@sizeOf(State) != 16) @compileError("xspi state layout drifted");
    if (sector_bytes != blocks_per_sector * block_bytes) @compileError("xspi sector geometry drifted");
}

extern fn ra8_xspi_flash_read(instance: u8, flash_addr: u32, buf: ?[*]u8, len: u32) c_int;
extern fn ra8_xspi_flash_program(instance: u8, flash_addr: u32, data: ?[*]const u8, len: u32) c_int;
extern fn ra8_xspi_flash_erase_sector(instance: u8, flash_addr: u32) c_int;
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

/// Reject an out-of-range [lba, lba+count) block range. Split into two
/// single-condition checks (no compound decision) so the bounds test needs no
/// MC/DC vectors and never underflows block_count - count.
fn bounds(st: *const State, lba: u32, count: u32) c_int {
    if (count > st.block_count) return err_out_of_range;
    if (lba > st.block_count - count) return err_out_of_range;
    return ok;
}

/// Whole-sector read-modify-write scratch (zero-malloc, NASA Rule 3). Shared
/// across devices, matching the module static in the replaced C unit.
var sector: [sector_bytes]u8 = undefined;

/// Read len flash bytes at flash_addr, chunked to the HAL per-transfer limit;
/// stops at the first non-ok HAL status.
fn readChunked(instance: u8, flash_addr: u32, dst: [*]u8, len: u32) c_int {
    var done: u32 = 0;
    while (done < len) {
        const chunk = @min(len - done, max_xfer);
        const e = ra8_xspi_flash_read(instance, flash_addr + done, dst + done, chunk);
        if (e != ok) return e;
        done += chunk;
    }
    return ok;
}

/// Program len flash bytes at flash_addr, chunked to the HAL per-transfer
/// limit. The covering sector must already be erased to all-ones.
fn programChunked(instance: u8, flash_addr: u32, src: [*]const u8, len: u32) c_int {
    var done: u32 = 0;
    while (done < len) {
        const chunk = @min(len - done, max_xfer);
        const e = ra8_xspi_flash_program(instance, flash_addr + done, src + done, chunk);
        if (e != ok) return e;
        done += chunk;
    }
    return ok;
}

/// Read-modify-write one 4 KiB sector: read it into the module scratch,
/// overlay the n_blocks touched blocks starting at first_blk from src, erase
/// the sector to all-ones, then program the 4 KiB back. Keeps NOR's
/// erase-before-program contract correct for arbitrary writes.
fn writeOneSector(st: *const State, sector_lba: u32, first_blk: u32, n_blocks: u32, src: [*]const u8) c_int {
    const sec_addr = st.base_off + sector_lba * block_bytes;
    const r = readChunked(st.instance, sec_addr, &sector, sector_bytes);
    if (r != ok) return r;
    const dst_off: usize = first_blk * block_bytes;
    const n_bytes: usize = n_blocks * block_bytes;
    @memcpy(sector[dst_off .. dst_off + n_bytes], src[0..n_bytes]);
    const e = ra8_xspi_flash_erase_sector(st.instance, sec_addr);
    if (e != ok) return e;
    return programChunked(st.instance, sec_addr, &sector, sector_bytes);
}

fn read(ctx: ?*anyopaque, lba: u32, count: u32, buf: ?[*]u8) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const dst = buf orelse return nullPtr("buf must not be nullptr");
    const st: *const State = @ptrCast(@alignCast(raw));
    const b = bounds(st, lba, count);
    if (b != ok) return b;
    return readChunked(st.instance, st.base_off + lba * block_bytes, dst, count * block_bytes);
}

fn write(ctx: ?*anyopaque, lba: u32, count: u32, buf: ?[*]const u8) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const src = buf orelse return nullPtr("buf must not be nullptr");
    const st: *const State = @ptrCast(@alignCast(raw));
    if (st.read_only) return err_not_supported;
    const b = bounds(st, lba, count);
    if (b != ok) return b;
    // Iterate the 4 KiB sectors the [lba, lba+count) range spans and rewrite
    // each via the whole-sector read-modify-write.
    var done: u32 = 0;
    while (done < count) {
        const blk = lba + done;
        const first_blk = blk % blocks_per_sector;
        const n_blocks = @min(count - done, blocks_per_sector - first_blk);
        const e = writeOneSector(
            st,
            blk - first_blk,
            first_blk,
            n_blocks,
            src + @as(usize, done) * block_bytes,
        );
        if (e != ok) return e;
        done += n_blocks;
    }
    return ok;
}

/// Erase sector-aligned count blocks at lba: both must be multiples of
/// blocks_per_sector; each spanned sector is erased to all-ones via the HAL.
fn erase(ctx: ?*anyopaque, lba: u32, count: u32) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const st: *const State = @ptrCast(@alignCast(raw));
    if (st.read_only) return err_not_supported;
    if (lba % blocks_per_sector != 0) return err_invalid_arg;
    if (count % blocks_per_sector != 0) return err_invalid_arg;
    const b = bounds(st, lba, count);
    if (b != ok) return b;
    var blk = lba;
    while (blk < lba + count) : (blk += blocks_per_sector) {
        const e = ra8_xspi_flash_erase_sector(st.instance, st.base_off + blk * block_bytes);
        if (e != ok) return e;
    }
    return ok;
}

/// Report the all-ones-erase NOR medium that must be erased before
/// programming, with an 8-block (4 KiB) erase unit.
fn getCaps(ctx: ?*const anyopaque, out: ?*Caps) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const caps = out orelse return nullPtr("out must not be nullptr");
    const st: *const State = @ptrCast(@alignCast(raw));
    caps.* = .{
        .block_count = st.block_count,
        .erase_unit_blocks = blocks_per_sector,
        .program_size_bytes = program_bytes,
        .logical_block_bytes = @intCast(block_bytes),
        .erase_value = erase_value_ones,
        .must_erase_before_write = true,
        .read_only = st.read_only,
    };
    return ok;
}

/// OSPI NOR backend vtable. sync is null: programs commit on return.
const iface = Iface{
    .read = read,
    .write = write,
    .erase = erase,
    .get_caps = getCaps,
    .sync = null,
};

/// Validate the window geometry: instance range, sector-aligned base offset,
/// and a non-zero whole-sector block count. Split single-condition checks (no
/// compound decision), matching the replaced C unit.
fn checkGeom(instance: u8, base_off: u32, block_count: u32) c_int {
    if (instance >= instance_count) return err_invalid_arg;
    if (base_off % sector_bytes != 0) return err_invalid_arg;
    if (block_count == 0) return err_invalid_arg;
    if (block_count % blocks_per_sector != 0) return err_invalid_arg;
    return ok;
}

export fn ra8_io_blockdev_xspi_init(
    bd: ?*Device,
    state: ?*State,
    instance: u8,
    base_off: u32,
    block_count: u32,
    read_only: bool,
) c_int {
    const dev = bd orelse return nullPtr("bd must not be nullptr");
    const st = state orelse return nullPtr("state must not be nullptr");
    const g = checkGeom(instance, base_off, block_count);
    if (g != ok) return g;
    st.* = .{
        .instance = instance,
        .base_off = base_off,
        .block_count = block_count,
        .read_only = read_only,
    };
    dev.* = .{ .iface = &iface, .ctx = st };
    return ok;
}
