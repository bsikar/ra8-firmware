//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C ABI membrane for `ra8_tz_secure_boot`.
//!
//! Every symbol `inc/` declares is exported here and nowhere else. The
//! decisions live in `src/internal/`; what this file adds is the register
//! stores, the driver calls and the protection-gate discipline that a host
//! test cannot exercise. Each export is the C prototype's shape exactly:
//! `ra8_err_t` is a `u32`, and a pointer the caller may pass as NULL is an
//! optional so the null check cannot be forgotten.

const std = @import("std");
const build_options = @import("build_options");

// Re-exported, not merely imported: the ABI tests drive the same module
// instances these exports mutate, and a second instance would be a second set
// of globals that agrees with nothing.
pub const boot = @import("internal/boot.zig");
pub const hal = @import("internal/hal.zig");
pub const ipc = @import("internal/ipc.zig");
pub const mmio = @import("internal/mmio.zig");
pub const nsimage = @import("internal/nsimage.zig");
pub const partition = @import("internal/partition.zig");
pub const psar = @import("internal/psar.zig");
pub const regs = @import("internal/regs.zig");
pub const sau = @import("internal/sau.zig");

/// Whether the NS image must authenticate before the jump. Off by default,
/// exactly as `RA8_ENABLE_ROOT_OF_TRUST` is in the C build.
const root_of_trust: bool = build_options.root_of_trust;

// ===========================================================================
// The protection gate
// ===========================================================================

/// Open PRC4 so the CPSCU block accepts writes.
fn gateOpen() void {
    mmio.write16(regs.Addr.prcr_s, regs.Prcr.open);
}

/// Restore write protection. Every `gateOpen` is paired with one of these on
/// every path, which is why the callers below leave their loops by breaking
/// rather than returning.
fn gateClose() void {
    mmio.write16(regs.Addr.prcr_s, regs.Prcr.close);
}

// ===========================================================================
// ra8_tz_secure_boot.h
// ===========================================================================

/// Read the signed body length out of the NS image's root-of-trust header.
pub export fn ra8_tz_ns_signed_body_len(ns_vector_table: ?[*]const u32) callconv(.c) u32 {
    const table = ns_vector_table orelse return 0;
    return nsimage.signedBodyLen(@ptrCast(table));
}

/// Program the canonical five-region partition.
pub export fn ra8_tz_secure_boot_sau_init() callconv(.c) u32 {
    // Checked here rather than left to the driver so a shortfall keeps
    // reporting not_supported, which is this function's published contract.
    if (!sau.supported(hal.regionCount())) return regs.Err.not_supported;

    const cfg: hal.SauCfg = .{
        .regions = &sau.regions,
        .region_count = @intCast(sau.Region.count),
        .all_ns = sau.all_ns,
    };
    const err = hal.configure(&cfg);
    if (err != regs.Err.ok) return err;

    boot.step = .sau_done;
    return regs.Err.ok;
}

/// Write the two IPC attribution words behind the protection gate.
pub export fn ra8_tz_secure_boot_security_init(ipcsar_value: u32, ipcpar_value: u32) callconv(.c) u32 {
    gateOpen();
    boot.step = .prcr_unlocked;

    mmio.write32(regs.Addr.ipcsar, ipcsar_value);
    mmio.write32(regs.Addr.ipcpar, ipcpar_value);
    boot.step = .ipcsar_written;

    gateClose();
    boot.step = .prcr_relocked;

    mmio.barrier();
    return regs.Err.ok;
}

/// Encode a descriptor and apply it. A bad descriptor returns before the
/// gate is ever opened.
pub export fn ra8_tz_secure_boot_security_init_map(cfg: ?*const ipc.Attribution) callconv(.c) u32 {
    const map = cfg orelse return regs.Err.null_ptr;
    if (!ipc.valid(map.*)) return regs.Err.invalid_arg;
    const words = ipc.encode(map.*);
    return ra8_tz_secure_boot_security_init(words.ipcsar, words.ipcpar);
}

/// Point VTOR_NS at the NS image and branch into it.
pub export fn ra8_tz_secure_boot_jump_ns(ns_vector_table: ?[*]const u32) callconv(.c) u32 {
    const table = ns_vector_table orelse return regs.Err.null_ptr;
    const base = @intFromPtr(table);
    // Kept for the C callers, who can hand over a misaligned pointer. There is
    // no test for it because a Zig caller cannot construct one: `[*]const u32`
    // carries 4-byte alignment, and `@ptrFromInt` to an odd address traps in
    // safe builds before the call is ever made.
    if (base % 4 != 0) return regs.Err.invalid_arg;

    const initial_sp = table[0];
    const reset_entry = table[1];
    // 0 is an all-zero image, 0xFFFFFFFF is erased MRAM. Neither is a vector.
    if (reset_entry == 0 or reset_entry == std.math.maxInt(u32)) return regs.Err.invalid_arg;

    if (root_of_trust and nsimage.signedBodyLen(@ptrCast(table)) == 0) {
        return regs.Err.validation_failed;
    }

    mmio.write32(regs.Addr.vtor_ns, @truncate(base));
    boot.step = .blxns_armed;

    if (comptime mmio.on_target) {
        // BLXNS switches world only when bit 0 of the target is clear: that
        // bit is the function-descriptor Secure marker, not the Thumb bit.
        // The NS reset vector carries the Thumb bit, so it MUST be cleared,
        // or the "NS" image runs Secure on the Secure stack and every cmse
        // check in the veneers misfires.
        const ns_entry = reset_entry & ~@as(u32, 1);
        asm volatile (
            \\msr msp_ns, %[sp]
            \\blxns %[entry]
            :
            : [sp] "r" (initial_sp),
              [entry] "r" (ns_entry),
            : "memory"
        );
        return regs.Err.ok; // Unreachable on target.
    }

    boot.host.blxns_msp_ns = initial_sp;
    boot.host.blxns_target = reset_entry;
    boot.step = .branched;
    return regs.Err.ok;
}

/// The whole secure boot, in order.
pub export fn ra8_tz_secure_boot_run(
    ipcsar_value: u32,
    ipcpar_value: u32,
    ns_vector_table: ?[*]const u32,
) callconv(.c) u32 {
    const table = ns_vector_table orelse return regs.Err.null_ptr;

    const sau_err = ra8_tz_secure_boot_sau_init();
    if (sau_err != regs.Err.ok) return sau_err;

    const init_err = ra8_tz_secure_boot_security_init(ipcsar_value, ipcpar_value);
    if (init_err != regs.Err.ok) return init_err;

    return ra8_tz_secure_boot_jump_ns(table);
}

/// How far the boot got, for a bench probe.
pub export fn ra8_tz_secure_boot_get_step() callconv(.c) u8 {
    return @intFromEnum(boot.step);
}

/// Return the host fixtures to their power-on state.
pub export fn ra8_tz_secure_boot_host_reset() callconv(.c) void {
    boot.reset();
    hal.reset();
}

/// What the off-target build would have branched to.
pub export fn ra8_tz_secure_boot_host_blxns_target() callconv(.c) u32 {
    return boot.host.blxns_target;
}

// ===========================================================================
// ra8_tz_ipc_attr.h
// ===========================================================================

/// Encode a whole-device attribution map into its two register words.
pub export fn ra8_tz_ipc_attribution_encode(
    cfg: ?*const ipc.Attribution,
    out_ipcsar: ?*u32,
    out_ipcpar: ?*u32,
) callconv(.c) u32 {
    const map = cfg orelse return regs.Err.null_ptr;
    const sar = out_ipcsar orelse return regs.Err.null_ptr;
    const par = out_ipcpar orelse return regs.Err.null_ptr;
    // Validated before either output is touched, so a refused call leaves the
    // caller's words exactly as it found them.
    if (!ipc.valid(map.*)) return regs.Err.invalid_arg;

    const words = ipc.encode(map.*);
    sar.* = words.ipcsar;
    par.* = words.ipcpar;
    return regs.Err.ok;
}

/// The map the cpu1_pingpong app runs.
pub export fn ra8_tz_ipc_attribution_cpu1_pingpong(out_cfg: ?*ipc.Attribution) callconv(.c) u32 {
    const out = out_cfg orelse return regs.Err.null_ptr;
    out.* = ipc.cpu1Pingpong();
    return regs.Err.ok;
}

// ===========================================================================
// ra8_tz_partition.h
// ===========================================================================

/// Check a descriptor without applying it.
pub export fn ra8_tz_partition_validate(cfg: ?*const partition.Partition) callconv(.c) u32 {
    const map = cfg orelse return regs.Err.null_ptr;
    return partition.validate(map, hal.regionCount());
}

/// Validate a descriptor, then program the SAU and the SRAM boundaries.
pub export fn ra8_tz_partition_apply(cfg: ?*const partition.Partition) callconv(.c) u32 {
    const map = cfg orelse return regs.Err.null_ptr;

    const check = partition.validate(map, hal.regionCount());
    if (check != regs.Err.ok) return check;

    const sau_cfg: hal.SauCfg = .{
        .regions = map.sau_regions,
        .region_count = map.sau_region_count,
        .all_ns = map.sau_all_ns,
    };
    const err = hal.configure(&sau_cfg);
    if (err != regs.Err.ok) return err;

    const boundary = map.sram_boundary orelse return regs.Err.ok;

    gateOpen();
    var failed: u32 = regs.Err.ok;
    for (0..partition.Limits.sram_bank_count) |bank| {
        const bank_err = hal.setBoundary(@intCast(bank), boundary[bank]);
        if (bank_err != regs.Err.ok) {
            failed = bank_err;
            break; // Never return from inside the gate: the close below is the re-lock.
        }
    }
    gateClose();
    return failed;
}

/// The board's own partition.
pub export fn ra8_tz_partition_board_map() callconv(.c) *const partition.Partition {
    return &partition.board_map;
}

// ===========================================================================
// ra8_tz_psar.h
// ===========================================================================

/// Hand peripheral bits to the Non-Secure world, and report what stuck.
pub export fn ra8_tz_psar_set_ns(psar_addr: usize, ns_mask: u32, out_seen: ?*u32) callconv(.c) u32 {
    switch (psar.plan(psar_addr, ns_mask)) {
        .reject => |err| return err,
        // An empty mask reports the current value without opening the gate.
        .observe => {
            if (out_seen) |seen| seen.* = mmio.read32(psar_addr);
            return regs.Err.ok;
        },
        .apply => |bits| {
            gateOpen();
            mmio.write32(psar_addr, psar.merged(mmio.read32(psar_addr), bits));

            var seen_value: u32 = 0;
            for (0..regs.Psar.readback_spins) |_| {
                seen_value = mmio.read32(psar_addr);
                if (psar.settled(seen_value, bits)) break;
            }
            gateClose();

            if (out_seen) |seen| seen.* = seen_value;
            return if (psar.settled(seen_value, bits)) regs.Err.ok else regs.Err.timeout;
        },
    }
}
