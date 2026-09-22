//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Production `ra8_devcfg_store_t` binding over the extra-MRAM window: the
//! second and final half of the `ra8_devcfg` port, replacing
//! `src/ra8_devcfg_store_extra_mram.c`.
//!
//! Reads dereference the memory-mapped extra-MRAM (data-flash) window at
//! `k_ra8_flash_extra_start + offset`; writes go through
//! `ra8_flash_extra_mram_write` in `k_ra8_devcfg_page_bytes` program pages
//! (HUM Ch 59.7.4.5 "Program Command" Table 59.15 p 3592). The window is
//! untouched by any DFU slot program or erase, so the record survives an A/B
//! update and a rollback alike.
//!
//! The host build addresses a RAM shadow instead: the flash MACI registers are
//! modelled by the fake but the extra-MRAM *data* side is not, so the shadow
//! lets the host exercise the identical read / page-loop / offset control flow
//! without MMIO. The C selected this with `RA8_OFF_TARGET`; here it is the
//! `off-target` build option, which defaults to "on" for a hosted target and
//! "off" for freestanding, so the CMake host build and the ARM cross-build each
//! get what they got before without passing a flag.
//!
//! Blank (never-programmed) extra-MRAM reads back as 0xFF with valid ECC and
//! does not bus-fault on the corrected window (#315), so no fault-catch probe
//! is needed and a virgin unit fails the record magic and resolves cleanly to
//! UNPROVISIONED. The window is one-time-programmable (HUM Ch 59.7.4.5): a
//! commit programs a fresh copy slot rather than rewriting one in place.

const abi = @import("ra8_devcfg_abi.zig");
const build_config = @import("build_config");

/// Copy 0 offset in extra-MRAM (`k_ra8_devcfg_copy0_off`).
pub const copy0_off: u32 = 0x00000040;
/// Copy 1 offset in extra-MRAM (`k_ra8_devcfg_copy1_off`).
pub const copy1_off: u32 = 0x00000100;
/// Reserved slot pitch per copy (`k_ra8_devcfg_slot_bytes`).
pub const slot_bytes: u32 = 192;
/// Extra-MRAM program page (`k_ra8_devcfg_page_bytes`).
pub const page_bytes: u32 = 32;
/// One past the last devcfg region byte (`k_ra8_devcfg_xm_span`): copy 1 plus
/// one reserved slot. Both the RAM shadow size and the bounds guard derive
/// from it, so the two cannot disagree about the region extent.
pub const span: u32 = copy1_off + slot_bytes;
/// Value an unprogrammed byte reads back as.
pub const blank: u8 = 0xFF;
/// `k_ra8_flash_extra_start`: first legal Program target (HUM Ch 59.1).
pub const flash_extra_start: u32 = 0x02E07600;

/// Whether the backends address the RAM shadow rather than silicon.
pub const off_target: bool = build_config.off_target;

const tag: [*:0]const u8 = "DEVCFG_XM";

const err_ok: abi.RawErr = 0;
const err_out_of_range: abi.RawErr = 0x208;
const err_null_ptr: abi.RawErr = 0x504;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;

/// `ra8_flash_extra_mram_write`: program 1..32 bytes inside one page.
extern fn ra8_flash_extra_mram_write(mram_addr: u32, src: [*]const u8, len: u32) callconv(.c) abi.RawErr;

/// Host-test RAM shadow of the extra-MRAM devcfg region, blank-filled on first
/// access so an unwritten read is indistinguishable from virgin silicon.
var shadow: [span]u8 = @splat(0);
var shadow_ready: bool = false;

/// Return the RAM shadow, blank-filling it on the first call.
fn shadowBytes() *[span]u8 {
    if (!shadow_ready) {
        @memset(&shadow, blank);
        shadow_ready = true;
    }
    return &shadow;
}

/// Whether `offset .. offset + len` stays inside the devcfg region.
///
/// Widened to `u64` so a caller-supplied length near `0xFFFFFFFF` is refused
/// rather than wrapping past the guard, which the C's `uint32_t` addition
/// would have done.
pub fn inRegion(offset: u32, len: u32) bool {
    return (@as(u64, offset) + @as(u64, len)) <= @as(u64, span);
}

/// Bytes the next program command may take: the remainder, capped at one page.
///
/// The loop this drives is bounded by `len / page_bytes` (at most
/// `record_len / page_bytes` = 4) chunks, and every chunk stays inside one
/// 32-byte program page because both the offsets devcfg commits at and the
/// page size are powers of two.
pub fn chunkBytes(len: u32, done: u32) u32 {
    const remaining = len - done;
    return if (remaining > page_bytes) page_bytes else remaining;
}

/// Extra-MRAM read backend (`ra8_devcfg_read_fn_t`).
fn read(offset: u32, dst: ?[*]u8, len: u32) callconv(.c) abi.RawErr {
    const out = dst orelse {
        ra8_log_emit_error(tag, "xm read: dst null");
        return err_null_ptr;
    };
    if (!inRegion(offset, len)) return err_out_of_range;

    if (off_target) {
        @memcpy(out[0..len], shadowBytes()[offset..][0..len]);
    } else {
        // HUM Ch 59.1 "Address Map" p 3543: the extra-MRAM window is directly
        // memory-mapped for CPU reads; a virgin word returns 0xFFFFFFFF with
        // valid ECC and no BusFault (#315).
        const src: [*]const volatile u8 = @ptrFromInt(flash_extra_start + offset);
        var i: u32 = 0;
        while (i < len) : (i += 1) out[i] = src[i];
    }
    return err_ok;
}

/// Extra-MRAM write backend (`ra8_devcfg_write_fn_t`).
fn write(offset: u32, src: ?[*]const u8, len: u32) callconv(.c) abi.RawErr {
    const in = src orelse {
        ra8_log_emit_error(tag, "xm write: src null");
        return err_null_ptr;
    };
    if (!inRegion(offset, len)) return err_out_of_range;

    if (off_target) {
        @memcpy(shadowBytes()[offset..][0..len], in[0..len]);
        return err_ok;
    }

    var done: u32 = 0;
    while (done < len) {
        const chunk = chunkBytes(len, done);
        const err = ra8_flash_extra_mram_write(flash_extra_start + offset + done, in + done, chunk);
        if (err != err_ok) {
            ra8_log_emit_error(tag, "xm write: page program failed");
            ra8_log_emit_error_val(tag, "Error", err);
            return err;
        }
        done += chunk;
    }
    return err_ok;
}

/// The process-lifetime extra-MRAM-backed store, returned by
/// `ra8_devcfg_default_store`.
const default_store: abi.Store = .{ .read = &read, .write = &write };

pub export fn ra8_devcfg_default_store() callconv(.c) *const abi.Store {
    return &default_store;
}
