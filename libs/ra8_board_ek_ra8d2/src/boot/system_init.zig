//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Cortex-M85 / RA8D2 core bring-up, called from `Reset_Handler` after the
//! `.data` copy and `.bss` zero. [Ring 1 / Boot] {World: S}
//!
//! Everything here runs on the reset stack and touches CPU / SCB memory
//! only: VTOR at the linker-pinned MRAM vector table, the FPU grant and its
//! lazy stacking, the always-on fault diagnostics (the SHCSR fault enables
//! and the divide-by-zero trap), and behind the cache/MPU gate the
//! five-region MPU map plus the I/D caches, each with its architectural
//! invalidate before the enable bit. NVIC priority grouping lands last.
//!
//! The three C gates arrive as boot options, the per-app spellings of
//! `RA8_BOOT_ENABLE_CACHE_MPU`, `RA8_BOOT_MPU_VIA_HAL` and
//! `RA8_BOOT_CACHE_VIA_HAL`. `ra8_trustzone_init` is referenced so the
//! TrustZone unit keeps its own gate (`RA8_TRUSTZONE_ENABLE`) and its own
//! object. Ported from system_init.c (RA8FW-365).

const boot_options = @import("boot_options");

/// `ra8_err_t` is `enum : uint16_t`; success is the only zero value.
const ErrCode = u16;

// ---------------------------------------------------------------------------
// Core / SCB / NVIC register addresses (direct, no CMSIS dep).
// ---------------------------------------------------------------------------

const scb_vtor: usize = 0xE000ED08;
const scb_ccr: usize = 0xE000ED14;
const scb_shcsr: usize = 0xE000ED24;
const scb_cpacr: usize = 0xE000ED88;
const scb_iciallu: usize = 0xE000EF50;
const scb_fpu_fpccr: usize = 0xE000EF34;
const nvic_aircr: usize = 0xE000ED0C;
const mpu_type: usize = 0xE000ED90;
const mpu_ctrl: usize = 0xE000ED94;
const mpu_rnr: usize = 0xE000ED98;
const mpu_rbar: usize = 0xE000ED9C;
const mpu_rlar: usize = 0xE000EDA0;
const mpu_mair0: usize = 0xE000EDC0;
const mpu_mair1: usize = 0xE000EDC4;

/// The vector table sits at the start of the `.vectors` section, which the
/// linker pins to the start of MRAM (`0x02000000`); VTOR stores the absolute
/// address.
extern const g_ra8_vector_table_start: u32;

extern fn ra8_cache_dcache_invalidate_all() callconv(.c) void;
extern fn ra8_cache_icache_enable() callconv(.c) void;
extern fn ra8_cache_dcache_enable() callconv(.c) void;
extern fn ra8_mpu_apply_boot_map() callconv(.c) ErrCode;
extern fn ra8_trustzone_init() callconv(.c) void;

// The `ra8_boot_intrinsics.h` helpers, spelled in Zig. Boot units are only
// built for the target, so the `RA8_OFF_TARGET` host no-ops do not exist here.
fn read32(addr: usize) u32 {
    const p: *const volatile u32 = @ptrFromInt(addr);
    return p.*;
}

fn write32(addr: usize, value: u32) void {
    const p: *volatile u32 = @ptrFromInt(addr);
    p.* = value;
}

fn dsb() void {
    asm volatile ("dsb 0xF" ::: .{ .memory = true });
}

fn isb() void {
    asm volatile ("isb 0xF" ::: .{ .memory = true });
}

fn disableIrq() void {
    asm volatile ("cpsid i" ::: .{ .memory = true });
}

// ---------------------------------------------------------------------------
// Core init steps
// ---------------------------------------------------------------------------

/// Point VTOR at the physical vector table base.
fn setVtor() void {
    write32(scb_vtor, @truncate(@intFromPtr(&g_ra8_vector_table_start)));
}

const cpacr_cp10_cp11_full_access: u32 = 0x00F00000;

/// Grant full access to CP10 / CP11 (the FPU coprocessors).
fn enableFpu() void {
    write32(scb_cpacr, read32(scb_cpacr) | cpacr_cp10_cp11_full_access);
    dsb();
    isb();
}

const fpccr_lspen: u32 = 1 << 30;
const fpccr_aspen: u32 = 1 << 31;

/// Turn on FPU lazy stacking (FPCCR.LSPEN = 1, ASPEN = 1).
fn enableFpuLazyStack() void {
    write32(scb_fpu_fpccr, read32(scb_fpu_fpccr) | fpccr_lspen | fpccr_aspen);
}

const ccr_ic: u32 = 1 << 17;
const ccr_dc: u32 = 1 << 16;
const ccr_bp: u32 = 1 << 18;
const ccr_div_0_trp: u32 = 1 << 4;

/// Invalidate, then enable the I-cache through CCR.IC.
fn enableIcache() void {
    dsb();
    write32(scb_iciallu, 0);
    dsb();
    isb();
    write32(scb_ccr, read32(scb_ccr) | ccr_ic);
    dsb();
    isb();
}

/// Run the CCSIDR-driven set/way invalidate (libs/ra8_hal ra8_cache) BEFORE
/// setting CCR.DC, so no random power-on line content is ever treated as
/// valid once the cache is enabled.
fn enableDcache() void {
    ra8_cache_dcache_invalidate_all();
    write32(scb_ccr, read32(scb_ccr) | ccr_dc);
    dsb();
    isb();
}

/// Enable branch-target prediction through CCR.BP.
fn enableBranchPredictor() void {
    write32(scb_ccr, read32(scb_ccr) | ccr_bp);
    dsb();
    isb();
}

const shcsr_memfaultena: u32 = 1 << 16;
const shcsr_busfaultena: u32 = 1 << 17;
const shcsr_usgfaultena: u32 = 1 << 18;
const shcsr_securefaultena: u32 = 1 << 19;

/// Enable the configurable-fault handlers so faults decode by class.
///
/// SECUREFAULTENA (bit 19) is banked and exists only in the Secure view of
/// SHCSR (from Non-secure it is RES0, so a misplaced write is architecturally
/// ignored). The RA8D2 core always resets into the Secure state and flat apps
/// never leave it, so the write always lands in the Secure bank; it is set
/// unconditionally because every image in this repository carries a decoded
/// SecureFault handler.
fn enableFaultHandlers() void {
    const bits: u32 = shcsr_memfaultena | shcsr_busfaultena |
        shcsr_usgfaultena | shcsr_securefaultena;
    write32(scb_shcsr, read32(scb_shcsr) | bits);
    dsb();
    isb();
}

/// Make integer divide-by-zero trap as a decoded UsageFault.
///
/// CCR.UNALIGN_TRP (bit 3) is deliberately NOT set: the tree compiles with
/// arm-none-eabi-gcc's default `-munaligned-access`, under which the compiler
/// is entitled to emit hardware-supported unaligned word accesses, and does
/// (the vendored SOUP decoders' small memcpy idiom, packed-struct field
/// access, and the prebuilt newlib libc). Trapping unaligned accesses would
/// fault legitimate generated code; it can only be enabled together with a
/// whole-tree and libc rebuild under `-mno-unaligned-access`.
fn enableDiv0Trap() void {
    write32(scb_ccr, read32(scb_ccr) | ccr_div_0_trp);
    dsb();
    isb();
}

const aircr_vectkey: u32 = 0x05FA0000;
const aircr_prigroup_4: u32 = 0x00000300;

/// Set NVIC priority grouping to 4 preempt bits / 0 sub-priority.
fn setPriorityGrouping() void {
    write32(nvic_aircr, aircr_vectkey | aircr_prigroup_4);
}

// ---------------------------------------------------------------------------
// MPU: five regions, MAIR0 0x000444FF, enabled with PRIVDEFENA.
// ---------------------------------------------------------------------------

// RBAR attribute bits[4:0] = { SH[4:3], AP[2:1], XN[0] }. The memory TYPE is
// NOT here: it is the RLAR AttrIndx, which selects the MAIR byte.
const rbar_attr_ro_x: u32 = 0x06;
const rbar_attr_rw_xn: u32 = 0x03;
const mpu_rlar_enable: u32 = 1 << 0;
const mpu_rlar_attridx_shift: u5 = 1;

const attridx_normal_wbwa: u32 = 0;
const attridx_noncacheable: u32 = 1;
const attridx_device: u32 = 2;

const mpu_mair0_default: u32 = 0x000444FF;
const mpu_ctrl_enable: u32 = 1 << 0;
const mpu_ctrl_privdefena: u32 = 1 << 2;

/// Region base and limit addresses. RLAR limits are <region-end> minus the
/// ARMv8-M 32-byte region quantum, OR-ed with the enable bit at write time.
const Region = struct { base: u32, limit: u32, rbar_attr: u32, attridx: u32 };

const mram = Region{ .base = 0x02000000, .limit = 0x020FFFE0, .rbar_attr = rbar_attr_ro_x, .attridx = attridx_normal_wbwa };
const sram = Region{ .base = 0x22000000, .limit = 0x220FFFE0, .rbar_attr = rbar_attr_rw_xn, .attridx = attridx_normal_wbwa };
const sdram = Region{ .base = 0x68000000, .limit = 0x6BFFFFE0, .rbar_attr = rbar_attr_rw_xn, .attridx = attridx_normal_wbwa };
const peri = Region{ .base = 0x40000000, .limit = 0x47FFFFE0, .rbar_attr = rbar_attr_rw_xn, .attridx = attridx_device };

// Shared SRAM2+3 (M85<->M33 mailbox + CPU1 RAM) stays Normal NON-cacheable so
// cross-core hand-offs need no software cache maintenance (the M33 is
// cacheless). From ra8_board_ek_ra8d2_dualcore.h: the shared window starts at
// 0x22100000 and CPU1's private 64 KiB bank ends the on-chip SRAM, 640 KiB in
// total.
const mpu_region_quantum: u32 = 32;
const shram_base: u32 = 0x22100000;
const cpu1_sram_base: u32 = 0x22190000;
const cpu1_sram_size_bytes: u32 = 0x10000;
const shram = Region{
    .base = shram_base,
    .limit = cpu1_sram_base + cpu1_sram_size_bytes - mpu_region_quantum,
    .rbar_attr = rbar_attr_rw_xn,
    .attridx = attridx_noncacheable,
};

const mpu_regions = [_]Region{ mram, sram, sdram, peri, shram };

/// Program a single MPU region via RNR/RBAR/RLAR. The AttrIndx written into
/// RLAR bits[3:1] selects the region's MAIR byte and therefore its memory
/// type; that is what makes the peripheral region Device-nGnRE rather than
/// Normal cacheable.
fn mpuSetRegion(region: u32, r: Region) void {
    write32(mpu_rnr, region);
    write32(mpu_rbar, r.base | r.rbar_attr);
    write32(mpu_rlar, r.limit | mpu_rlar_enable | (r.attridx << mpu_rlar_attridx_shift));
}

/// Programme and enable the Cortex-M85 core MPU.
fn mpuInit() void {
    write32(mpu_mair0, mpu_mair0_default);
    write32(mpu_mair1, 0);
    for (mpu_regions, 0..) |r, i| mpuSetRegion(@intCast(i), r);
    dsb();
    write32(mpu_ctrl, mpu_ctrl_enable | mpu_ctrl_privdefena);
    dsb();
    isb();
}

/// Install the canonical boot memory-attribute map, HAL or raw poke. The two
/// spellings program byte-identical registers for all five regions; the HAL
/// additionally disables the MPU before reprogramming and clears stale
/// high-index regions, neither of which changes a from-reset boot. Returns
/// false when the part implements fewer regions than the map needs, so the
/// caller leaves the caches off (a D-cache enabled with no map would see
/// MMIO as Normal cacheable).
fn bootMapInstall() bool {
    if (boot_options.mpu_via_hal) return ra8_mpu_apply_boot_map() == 0;
    mpuInit();
    return true;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

fn systemInit() callconv(.c) void {
    // Mask global IRQs so the application gets a deterministic init window.
    disableIrq();
    setVtor();
    enableFpu();
    enableFpuLazyStack();
    // Always-on fault diagnostics, independent of the cache / MPU build flag.
    enableFaultHandlers();
    enableDiv0Trap();
    if (boot_options.cache_mpu) {
        // MPU memory-attribute map FIRST (so the D-cache sees Device-nGnRE
        // MMIO and the non-cacheable shared SRAM, not Normal cacheable), then
        // the caches. Each enable runs its own architectural invalidate first.
        if (bootMapInstall()) {
            if (boot_options.cache_via_hal) {
                ra8_cache_icache_enable();
                ra8_cache_dcache_enable();
            } else {
                enableIcache();
                enableDcache();
            }
            enableBranchPredictor();
        }
    }
    _ = &ra8_trustzone_init; // TrustZone has its own gate (RA8_TRUSTZONE_ENABLE).
    setPriorityGrouping();
}

comptime {
    @export(&systemInit, .{ .name = "SystemInit" });
}
