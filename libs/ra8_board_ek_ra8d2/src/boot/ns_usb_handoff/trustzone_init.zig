//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Single-core TrustZone bring-up for a RAM-resident NS image.
//! [Ring 1 / Boot] {World: S}
//!
//! The RA8 IDAU is FIXED by address bit[28] (HUM section 51.3.3.1, p3265):
//! bit[28]=0 is Secure/NSC and the SAU cannot downgrade it, so an "NS" image
//! at the 0x02.. / 0x22.. (bit[28]=0) aliases always executes Secure. Real NS
//! lives at the bit[28]=1 aliases (0x12.. code, 0x32.. SRAM, 0x5..
//! peripherals).
//!
//! Code MRAM's secure/NS split needs persistent (brick-risky) option bytes,
//! but SRAM's split is the RUNTIME `SRAMSABARn` register, so the NS image is
//! RAM-resident: flashed into Secure MRAM (the LMA) and copied by this unit
//! into the SRAM Non-secure alias 0x3210_0000 (physical SRAM2) after
//! `SRAMSABAR2` marks SRAM2 Non-secure. No option bytes, no brick.
//!
//! Boot sequence (this unit, all in Secure state):
//!   1. Hand USB-FS (pins + PSARB NS attribution) to the NS world.
//!   2. Programme the SAU to the bit[28] model (HUM p3267): the three
//!      IDAU-NS ranges plus one NSC region over the `.gnu.sgstubs` veneers,
//!      and the SRAMSABAR boundaries that put the Secure/NS split at
//!      physical 0x10_0000 (SRAM2 NS).
//!   3. Copy the NS image from its MRAM LMA to the SRAM NS alias.
//!   4. BLXNS into the NS reset vector via `ra8_tz_secure_boot_jump_ns`.
//!
//! This unit does NOT use `ra8_tz_secure_boot_sau_init` -- that function's
//! region table is tuned for cpu1_pingpong_ipc (CPU1 is the NS core) and is
//! shared; the bit[28] model here is app-local so that validated app is
//! untouched. Only the generic `ra8_tz_secure_boot_jump_ns` primitive is
//! reused.
//!
//! The gate is the same `RA8_TRUSTZONE_ENABLE` define the C gate reads: it
//! arrives as `@import("boot_options")` (`trust_zone`), built as its own
//! object per TrustZone setting (cmake/ra8_app/zig_libs.cmake).
//!
//! TrustZone Safety:
//!   - Validates: the veneer linker symbols bound a non-empty range before
//!     region 0 is built.
//!   - Validates: `g_ra8_ns_vector_table` non-NULL + word-aligned (checked by
//!     `ra8_tz_secure_boot_jump_ns`).
//!   - Trusts: the boot ROM left the SAU disabled and the IDAU in its
//!     documented reset state (fixed bit[28] split).
//!   - Denies: treating the NS world as live on any
//!     `ra8_tz_secure_boot_jump_ns` return. On hardware a successful BLXNS
//!     leaves Secure thread mode and never returns; a returned denial verdict
//!     is latched in `g_tz_jump_ns_err` and boot falls back to the S-side
//!     `main()`.
//!
//! Ported from ns_usb_handoff/trustzone_init.c (RA8FW-365).

const boot_options = @import("boot_options");

/// `ra8_err_t` is `enum : uint16_t`; success is the only zero value.
const ErrCode = u16;

// ---------------------------------------------------------------------------
// Fixed NS-image addresses (two-project build). The NS image is a SEPARATE
// ELF (tz_nsc_cgc_usb_ns.elf), so the Secure side has none of its linker
// symbols. Its load (MRAM) and run (SRAM2 NS alias) bases are fixed by the NS
// linker script; the Secure boot copies a fixed window large enough for the
// NS image (ThreadX + USBX + ra8_usb fit well under 192 KB) and BLXNS-es to
// slot 1 of the NS vector table at the run base.
// Invariant: matches RA8_NS_MRAM_ORIGIN / RA8_NS_SRAM_ORIGIN in
// libs/ra8_board_ek_ra8d2/ld/ns_memory_map.cmake.
// ---------------------------------------------------------------------------

const ns_load_base: usize = 0x02080000; // NS image LMA (Secure MRAM).
const ns_run_base: usize = 0x32100000; // NS image VMA (SRAM2 NS alias).
const ns_copy_size: usize = 0x00030000; // Bytes copied LMA->VMA (192 KB).

// ---------------------------------------------------------------------------
// Secure-only register addresses and bit fields touched during NS bring-up.
// SAU lives in the System Control Space (Arm v8-M, mirrored in HUM section
// 51.3.3.3). SRAMSABARn and PRCR_S live in the CPSCU / SYSC windows (HUM
// sections 58.2 and 9.2.4). All values are secure-only MMIO addresses.
// ---------------------------------------------------------------------------

const psarb_addr: usize = 0x40204004; // PSCU PSARB (peripheral S/NS attr).

const sau_granule_mask: u32 = 0xFFFFFFE0; // 32-byte SAU quantum mask.
const psarb_usbfs_ns: u32 = 0x00000800; // PSARB11 = 1: USBFS0 Non-secure.
const psarb_usbhs_ns: u32 = 0x00001000; // PSARB12 = 1: USBHS Non-secure.
const psarb_usb_ns: u32 = 0x00001800; // PSARB11|12: both USB ctrls NS.

// ---------------------------------------------------------------------------
// Packed `ra8_port_pin_t` codes for the EK-RA8D2 USB pins, `(port << 8) |
// pin` (matches `ra8_port_pin_t`). Mirrored from
// ra8_board_ek_ra8d2_usb.h (`ra8_board_usbfs_pin_t` /
// `ra8_board_usbhs_pin_t`), the single source of truth for this board's USB
// pinout. The Secure side routes these to the USBFS peripheral function
// before BLXNS so the Non-secure USB stack drives a live PHY; PFS ownership
// (PMSAR) stays Secure but the muxed signal still reaches the
// (Non-secure-attributed) USBFS controller.
// Invariant: matches the EK-RA8D2 v1 User's Manual USB-FS (J11) pin map.
// ---------------------------------------------------------------------------

const usb_pin_vbus: u16 = 0x0407; // P4_07 FS VBUS. UM Table 22 p 30.
const usb_pin_vbusen: u16 = 0x0500; // P5_00 FS role.
const usb_pin_dp: u16 = 0x080E; // P8_14 FS D+.
const usb_pin_dm: u16 = 0x080F; // P8_15 FS D-.
const usb_pin_hs_vbus: u16 = 0x0408; // P4_08 HS VBUS.
const usb_pin_hs_pwr: u16 = 0x0D07; // PD07 J7 pwr.

// `ra8_psel_t` members this unit routes (ra8_gpio_constants.h, HUM 20.6).
const psel_usb_fs: u32 = 0x13; // 10011b: USB Full-Speed.
const psel_usb_hs: u32 = 0x14; // 10100b: USB High-Speed (USBHS_VBUS).

// `ra8_level_t`.
const level_low: u8 = 0;
const level_high: u8 = 1;

// `ra8_err_t` values this unit compares against (ra8_err.h).
const err_invalid_state: u16 = 0x104;
const err_busy: u16 = 0x109;

// ---------------------------------------------------------------------------
// Secure HAL / driver surface. `ra8_err_t` is `enum : uint16_t` at the C ABI.
// ---------------------------------------------------------------------------

extern fn ra8_tz_partition_apply(partition: *const TzPartition) callconv(.c) ErrCode;
extern fn ra8_tz_psar_set_ns(psar_addr: usize, ns_mask: u32, out_seen: *u32) callconv(.c) ErrCode;
extern fn ra8_pfs_route_peripheral(pin: u16, psel: u8, owner: [*:0]const u8) callconv(.c) ErrCode;
extern fn ra8_gpio_output_init(pin: u16, init_level: u8) callconv(.c) ErrCode;
extern fn ra8_pin_validator_reset() callconv(.c) void;
extern fn ra8_cgc_usbhs_pll_enable() callconv(.c) ErrCode;
extern fn ra8_board_io_expander_set_usbhs_host_mode() callconv(.c) ErrCode;
extern fn ra8_tz_secure_boot_jump_ns(ns_vector_table: *const u32) callconv(.c) ErrCode;

/// The C ABI shapes of the shared TrustZone-partition descriptors
/// (ra8_sau.h / ra8_tz_partition.h): a region is base + 32-byte-quantised
/// size + attribute, and the partition carries the SAU region table, the
/// four per-bank Secure boundaries, the region count and the ALLNS bit.
const SauRegion = extern struct {
    base: usize,
    size: u32,
    attr: u8,
};

const TzPartition = extern struct {
    sau_regions: [*]const SauRegion,
    sram_boundary: ?[*]const u32,
    sau_region_count: u8,
    sau_all_ns: bool,
};

/// `ra8_sau_attr_t` (uint8_t-backed) and the SAU geometry constants
/// (ra8_sau.h).
const sau_attr_ns: u8 = 0; // Non-Secure range.
const sau_attr_nsc: u8 = 1; // Non-Secure Callable range.
const sau_region_granule: u32 = 32; // Alignment and minimum size in bytes.

/// `k_ra8_tz_partition_sram_bank_count` mirrors the four SRAMSABARn
/// registers (ra8_tz_partition.h).
const sram_bank_count: usize = 4;

/// Bounds of the NSC veneer stubs (.gnu.sgstubs) in this (Secure) image,
/// linker-placed.
extern const g_ra8_ls_sgstubs_start: u32;
extern const g_ra8_ls_sgstubs_end: u32;

// ---------------------------------------------------------------------------
// J-Link probe globals (Secure-side .bss, written once by this unit and read
// externally by a bench halt).
// ---------------------------------------------------------------------------

/// PSARB value read back after marking USBFS0 Non-secure. HUM "Security Bit
/// Write Timing" p3301 requires reading the attribution register until it
/// matches the written value; this captures that confirmed value so a bench
/// halt can verify the USBFS NS delegation landed even if the NS image later
/// faults.
export var g_tz_usb_psarb_readback: u32 = 0;

/// `ra8_tz_psar_set_ns()` result for the USB attribution.
export var g_tz_usb_psarb_err: u32 = 0;

/// First non-OK `ra8_err_t` from the deterministic USB pin + PLL setup
/// (FS/HS pins, J7 VBUS GPIO, USBHS PLL; 0 = OK). The I/O-expander is
/// tracked separately in `g_tz_usb_expander_err`.
export var g_tz_usb_pins_err: u32 = 0;

/// Last `ra8_err_t` from the U15 I/O-expander host-mode write after the
/// bounded retry loop (0 = OK). Decoupled from `g_tz_usb_pins_err` because
/// the RIIC1 BBSY flag can survive a warm reset (SYSRESETREQ), making the
/// first expander write report k_ra8_err_busy until the bus-recovery in a
/// later retry (or a cold boot) clears it. The external PI4IOE latches its
/// host-mode output, so the USBHS host role persists across the MCU warm
/// reset.
export var g_tz_usb_expander_err: u32 = 0;

/// Denial verdict from `ra8_tz_secure_boot_jump_ns` (0 = never denied). On
/// hardware it only returns when it REFUSED to branch: the NS vector table
/// failed validation or the root-of-trust gate rejected the NS image. The
/// verdict is latched here so a J-Link probe can tell "NS image denied" apart
/// from "SAU programming failed" when the S-side fallback main() is reached
/// instead of the NS world.
export var g_tz_jump_ns_err: u32 = 0;

// ---------------------------------------------------------------------------
// IDAU-NS range geometry and the per-bank SRAM boundary. The IDAU-NS ranges
// are mandated NS-in-SAU by HUM p3267, expressed as base + size because that
// is what `ra8_sau_region_t` takes; the driver derives the `base + size - 32`
// RLAR limit once so a limit computed one region short cannot leave secure
// memory reachable. The SRAMSABAR offsets place the secure/NS boundary at
// physical 0x10_0000 so SRAM2 [0x10_0000, 0x18_0000) is the NS aperture.
// ---------------------------------------------------------------------------

const ns_code_base: u32 = 0x10000000; // IDAU-NS code alias base.
const ns_code_size: u32 = 0x10000000; // IDAU-NS code alias length.
const ns_sram_base: u32 = 0x30000000; // IDAU-NS SRAM alias base.
const ns_sram_size: u32 = 0x10000000; // IDAU-NS SRAM alias length.
const ns_per_base: u32 = 0x50000000; // IDAU-NS peripheral alias base.
const ns_per_size: u32 = 0x90000000; // IDAU-NS peripheral alias length.
const sramsabar0_val: u32 = 0x00080000; // SRAM0 all Secure (>= bank end).
const sramsabar1_val: u32 = 0x00100000; // SRAM1 all Secure (>= bank end).
const sramsabar2_val: u32 = 0x00100000; // SRAM2 all NS (boundary at base).
const sramsabar3_val: u32 = 0x001A0000; // SRAM3 all Secure (>= bank end).

/// Build this app's attribution map and hand it to the shared applier.
///
/// The layout is four SAU regions plus the four SRAMSABAR boundaries: region
/// 0 is NSC over the `.gnu.sgstubs` veneers (in the bit[28]=0 Secure code
/// region, so the IDAU permits NSC) and regions 1-3 mark the three IDAU-NS
/// ranges Non-secure as HUM p3267 mandates. Everything else stays Secure
/// (ALLNS = 0). The veneer bounds are linker-placed, so the descriptor is
/// built here rather than held as static data.
///
/// `ra8_tz_partition_apply` validates the whole descriptor before its first
/// write, programmes the SAU through `ra8_sau_configure` (which also clears
/// every region above the count, so an enabled window cannot be inherited
/// from the boot ROM), and then writes the SRAM boundaries inside its own
/// PRCR_S.PRC4 scope. Do not wrap this call in RA8_PROTECTED_WRITE: the
/// applier owns that window and a nested scope would re-lock every group on
/// the inner exit.
fn tzPartitionApply() ErrCode {
    // RBAR/RLAR are 32-byte quantised: round the base down and the last byte
    // of the block (end - 1) down to the same quantum, then express the pair
    // as the base + size the SAU driver takes.
    const nsc_start: usize = @intFromPtr(&g_ra8_ls_sgstubs_start) & @as(usize, sau_granule_mask);
    const nsc_end: usize = @intFromPtr(&g_ra8_ls_sgstubs_end);
    if (nsc_end <= nsc_start) {
        return err_invalid_state;
    }
    const nsc_last: usize = (nsc_end - 1) & @as(usize, sau_granule_mask);
    const nsc_base: u32 = @intCast(nsc_start);
    const nsc_size: u32 = @intCast((nsc_last - nsc_start) + sau_region_granule);

    const regions = [_]SauRegion{
        .{ .base = nsc_base, .size = nsc_size, .attr = sau_attr_nsc },
        .{ .base = ns_code_base, .size = ns_code_size, .attr = sau_attr_ns },
        .{ .base = ns_sram_base, .size = ns_sram_size, .attr = sau_attr_ns },
        .{ .base = ns_per_base, .size = ns_per_size, .attr = sau_attr_ns },
    };

    // HUM Ch 58.2 "SRAMSABARn : SRAM Security Attribute Boundary Address
    // Register" p 3527 -- boundary = start address of the NS region; below is
    // Secure, at/above is Non-secure.
    const sram_boundary = [sram_bank_count]u32{
        sramsabar0_val,
        sramsabar1_val,
        sramsabar2_val,
        sramsabar3_val,
    };

    const partition = TzPartition{
        .sau_regions = &regions,
        .sram_boundary = &sram_boundary,
        .sau_region_count = regions.len,
        .sau_all_ns = false,
    };
    return ra8_tz_partition_apply(&partition);
}

/// Copy the NS image from its MRAM LMA into the SRAM NS alias.
///
/// Plain word copy of the fixed `ns_copy_size` window from `ns_load_base`
/// (MRAM) to `ns_run_base` (0x3210_0000). Runs AFTER the SAU and SRAMSABAR
/// have marked the destination Non-secure, so the store is a (permitted)
/// Secure-side Non-secure access. A fixed window is used because the NS image
/// is a separate ELF; copying more than the image is harmless.
fn tzCopyNsImage() void {
    const src_start: usize = ns_load_base;
    const src_end: usize = src_start + ns_copy_size;
    var dst: usize = ns_run_base;
    var src: usize = src_start;
    while (src < src_end) : (src += 4) {
        const dst_p: *volatile u32 = @ptrFromInt(dst);
        const src_p: *const volatile u32 = @ptrFromInt(src);
        dst_p.* = src_p.*;
        dst += 4;
    }
}

/// Route the USB-FS device pins + USBHS host pins and enable the HS PLL.
///
/// Resets the pin validator (stale claims survive warm resets), routes the
/// four USB-FS pins as the DEVICE (P5_00 LOW), the two USBHS host pins (PD07
/// HIGH for J7 VBUS, P4_08 USBHS_VBUS), then enables the USBHS UTMI PLL. A
/// warm reset leaves the PLL running, so k_ra8_err_busy is treated as
/// success. The first failing step's error is returned; later steps are
/// short-circuited.
fn tzUsbRoutePins() ErrCode {
    // Establish the pin-validator baseline. The Secure boot never runs
    // ra8_infrastructure_init (its main() is dead -- BLXNS does not return),
    // so the validator bitmap is in an uninitialised-contract state and warm
    // resets leave stale claims (SRAM survives SYSRESETREQ; PFS does not),
    // which would make the USB-pin claims spuriously conflict. Reset it
    // first.
    ra8_pin_validator_reset();

    // Route the USB-FS pins as the DEVICE (Secure owns PFS). P5_00 LOW = dev.
    var err = ra8_pfs_route_peripheral(usb_pin_vbus, psel_usb_fs, "tz_usb.fs_vbus");
    if (err == 0) {
        err = ra8_gpio_output_init(usb_pin_vbusen, level_low);
    }
    if (err == 0) {
        err = ra8_pfs_route_peripheral(usb_pin_dp, psel_usb_fs, "tz_usb.fs_dp");
    }
    if (err == 0) {
        err = ra8_pfs_route_peripheral(usb_pin_dm, psel_usb_fs, "tz_usb.fs_dm");
    }

    // USBHS host pins: PD07 HIGH (U18 supplies J7 VBUS), route P4_08
    // USBHS_VBUS. (The host-mode mux is the U15 expander -- handled later.)
    if (err == 0) {
        err = ra8_gpio_output_init(usb_pin_hs_pwr, level_high);
    }
    if (err == 0) {
        err = ra8_pfs_route_peripheral(usb_pin_hs_vbus, psel_usb_hs, "tz_usb.hs_vbus");
    }

    // Enable the USBHS UTMI PLL (Secure CGC; PLL1 already locked). A warm
    // reset leaves the CGC PLL domain running, so a re-enable reports
    // k_ra8_err_busy ("already locked"); the clock is up either way, so treat
    // busy as success.
    if (err == 0) {
        const pll_err = ra8_cgc_usbhs_pll_enable();
        if (pll_err != 0 and pll_err != err_busy) {
            err = pll_err;
        }
    }
    return err;
}

/// Mark BOTH USB controllers Non-secure in PSARB (bits 11 + 12).
///
/// Via `ra8_tz_psar_set_ns()`: opens the PRCR_S.PRC4 gate, sets PSARB11
/// (USBFS0) and PSARB12 (USBHS) so the NS image reaches them through the
/// 0x5025_0000 / 0x5035_0000 aliases, spins (bounded) on the read-back until
/// the value confirms, then re-locks PRC4. The confirmed value is stored in
/// `g_tz_usb_psarb_readback` for a bench halt to verify.
fn tzUsbMarkNs() void {
    // PSARB11 = USBFS0, PSARB12 = USBHS (HUM Ch 51.8.1 p 3284); 0 = Secure,
    // 1 = Non-secure. The PRC4 gate, the mandatory read-back and the re-lock
    // live in ra8_tz_psar_set_ns(); the confirmed value is kept for a bench
    // halt to verify.
    var seen: u32 = 0;
    const err = ra8_tz_psar_set_ns(psarb_addr, psarb_usb_ns, &seen);
    g_tz_usb_psarb_readback = seen;
    g_tz_usb_psarb_err = err;
}

/// Hand BOTH USB controllers to the Non-secure world before BLXNS.
///
/// The NS image runs the USB CDC self-loop: USBFS (J11) is the CDC-ACM
/// DEVICE, USBHS (J7) is the polled HOST, and the two jacks are cabled
/// together so the chip enumerates + echoes against itself. This unit does
/// the Secure-only bring-up the NS image cannot:
///   1. Route the four USB-FS pins (device role: P5_00 LOW).
///   2. Set USBHS to host mode: U15 I/O-expander SW4-8 -> Host, PD07 HIGH
///      (U18 supplies J7 VBUS), and route P4_08 USBHS_VBUS.
///   3. Enable the USBHS UTMI PLL (`ra8_cgc_usbhs_pll_enable` -- CGC is
///      Secure-only; the 48 MHz USBFS clock is enabled by the NS image via
///      the NSC CGC veneer).
///   4. Mark BOTH controllers Non-secure in PSARB (bits 11 + 12) under the
///      PRC4 gate, so the NS image reaches USBFS/USBHS through the
///      0x5025_0000 / 0x5035_0000 aliases and may clear their
///      MSTPCRB.MSTPB11/12 module-stop bits itself.
fn tzUsbHandoffPrepare() void {
    // 1+2+3. Route USB pins (FS device + HS host) and enable the USBHS PLL.
    const pins_err = tzUsbRoutePins();
    g_tz_usb_pins_err = pins_err;

    // 4. Set the U15 I/O-expander to USBHS host mode (SW4-8 -> Host).
    //    Decoupled from the deterministic setup above and best-effort: on a
    //    cold boot the single I2C write lands (probe -> success); after a
    //    warm reset RIIC1's BBSY can still be set, reporting k_ra8_err_busy.
    //    The external PI4IOE latches its host-mode output, so the USBHS host
    //    role persists across the MCU warm reset -- the self-loop still
    //    enumerates (the HIL gate proves it). A retry cannot help here: the
    //    expander claims the SCL1/SDA1 pins on the first try and a second try
    //    would fault the pin validator.
    const exp_err = ra8_board_io_expander_set_usbhs_host_mode();
    g_tz_usb_expander_err = exp_err;

    // 5. Mark BOTH USB controllers Non-secure in PSARB (bits 11 + 12).
    tzUsbMarkNs();
}

/// Single-core TrustZone bring-up for a RAM-resident NS image. Runs from the
/// S-side boot after `ra8_board_system_init`; on a TrustZone-off build the
/// gate compiles the body out.
pub fn ra8TrustzoneInit() callconv(.c) void {
    // Same gate as the C unit's `#ifdef RA8_TRUSTZONE_ENABLE`
    // (cmake/ra8_app/zig_libs.cmake maps the define onto `trust_zone`).
    if (!boot_options.trust_zone) return;

    // 0. Hand USB-FS (pins + PSARB NS attribution) to the NS world.
    tzUsbHandoffPrepare();

    // 1. Apply the attribution map: the bit[28] SAU layout (NSC veneers +
    //    IDAU-NS ranges) and then the SRAMSABAR boundary that carves the
    //    SRAM2 NS aperture. The SAU goes first because it is the coarse map
    //    the SRAM boundary refines, and neither is observable until step 3
    //    touches the NS alias.
    if (tzPartitionApply() != 0) {
        return; // Fall through to the S-side main() fallback.
    }

    // 3. Copy the NS image into the now-Non-secure SRAM alias.
    tzCopyNsImage();

    // 4. Clear the Secure PRIMASK before handing off. The core bring-up
    //    masked IRQs (CPSID i) for secure bring-up; a set PRIMASK_S boosts
    //    the execution priority and masks NON-secure exceptions too, so the
    //    NS ThreadX PendSV / SysTick would never fire. The NS side cannot
    //    clear PRIMASK_S (it is Secure-banked), so do it here, right before
    //    BLXNS.
    asm volatile ("cpsie i" ::: .{ .memory = true });

    // 5. BLXNS into the NS reset vector (slot 1 of the NS vector table at the
    //    fixed run base 0x3210_0000). Does not return on hardware.
    const jump_err = ra8_tz_secure_boot_jump_ns(@ptrFromInt(ns_run_base));
    if (jump_err != 0) {
        // Reached on hardware ONLY when the NS image was DENIED
        // (vector-table validation or the root-of-trust gate failed). Latch
        // the verdict for a J-Link probe and fall through to the S-side
        // main() fallback -- never treat the NS world as live.
        g_tz_jump_ns_err = jump_err;
        return;
    }

    // On host (RA8_OFF_TARGET) the library stubs BLXNS and returns k_ra8_ok;
    // on target this point is unreachable. Boot units are only built for the
    // target, so no host no-op spelling exists here.
}

comptime {
    @export(&ra8TrustzoneInit, .{ .name = "ra8_trustzone_init" });
}
