//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the exported C ABI. These drive the same symbols the C suite
//! links against, over the module-local register block a test build
//! addresses in place of the MPU MMIO window, and mirror every MC/DC vector
//! set `tests/misc/src/test_ra8_mpu.c` pins down.

const std = @import("std");
const abi = @import("abi");

const Region = abi.Region;
const Config = abi.Config;

const ok: u16 = @intFromEnum(abi.MpuError.ok);
const invalid_arg: u16 = @intFromEnum(abi.MpuError.invalid_arg);
const null_ptr: u16 = @intFromEnum(abi.MpuError.null_ptr);

const perm_none: u8 = 0;
const perm_ro: u8 = 1;
const perm_rw: u8 = 2;

const ctrl_enable: u32 = 1 << 0;
const ctrl_hfnmiena: u32 = 1 << 1;
const ctrl_privdefena: u32 = 1 << 2;

// The library logs through the same symbol the real build resolves against
// libs/ra8_core/src/ra8_log.c; counting it here is the link-time
// substitution the C host build already performs.
var log_calls: usize = 0;
var last_tag: [*:0]const u8 = "";
var last_message: [*:0]const u8 = "";

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    log_calls += 1;
    last_tag = tag;
    last_message = message;
}

// The host build routes the barriers through the stub translation unit, so
// the test binary supplies the same two symbols.
var dsb_calls: usize = 0;
var isb_calls: usize = 0;

export fn ra8_hw_dsb() void {
    dsb_calls += 1;
}

export fn ra8_hw_isb() void {
    isb_calls += 1;
}

fn setup(dregion: u8) void {
    const mpu = abi.testRegs();
    mpu.* = std.mem.zeroes(@TypeOf(mpu.*));
    mpu.TYPE = @as(u32, dregion) << 8;
    abi.testShcsr().* = 0;
    log_calls = 0;
    dsb_calls = 0;
    isb_calls = 0;
}

fn region(size: u32, priv: u8, unpriv: u8) Region {
    return .{
        .base = 0x20000000,
        .size = size,
        .priv = priv,
        .unpriv = unpriv,
        .executable = 1,
        .shareable = 0,
        .attr_idx = 0,
    };
}

fn oneRegionCfg(r: *const Region) Config {
    return .{
        .regions = @ptrCast(r),
        .region_count = 1,
        .mair0 = 0,
        .mair1 = 0,
        .privdefena = 0,
        .hfnmiena = 0,
    };
}

test "configure rejects a null configuration and logs once" {
    setup(16);
    try std.testing.expectEqual(null_ptr, abi.ra8_mpu_configure(null));
    try std.testing.expectEqual(@as(usize, 1), log_calls);
    try std.testing.expectEqualStrings("MPU", std.mem.span(last_tag));
    try std.testing.expectEqualStrings("cfg must not be nullptr", std.mem.span(last_message));
}

test "configure rejects a region count above DREGION" {
    setup(8);
    var regions: [16]Region = undefined;
    for (&regions) |*r| r.* = region(0x1000, perm_rw, perm_rw);
    const cfg = Config{
        .regions = &regions,
        .region_count = 16,
        .mair0 = 0,
        .mair1 = 0,
        .privdefena = 0,
        .hfnmiena = 0,
    };
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_configure(&cfg));
    try std.testing.expectEqual(@as(u32, 0), abi.testRegs().CTRL);
}

test "configure rejects noncanonical boolean bytes before register writes" {
    setup(16);
    const mpu = abi.testRegs();
    mpu.CTRL = ctrl_enable;
    mpu.MAIR0 = 0xDEADBEEF;
    const region_value = region(0x1000, perm_rw, perm_rw);
    var cfg = oneRegionCfg(&region_value);
    cfg.privdefena = 2;
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_configure(&cfg));
    try std.testing.expectEqual(ctrl_enable, mpu.CTRL);
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), mpu.MAIR0);

    cfg.privdefena = 0;
    var invalid_region = region(0x1000, perm_rw, perm_rw);
    invalid_region.executable = 2;
    cfg.regions = @ptrCast(&invalid_region);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_configure(&cfg));
    try std.testing.expectEqual(ctrl_enable, mpu.CTRL);
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), mpu.MAIR0);

    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_set_region(0, &invalid_region));
    try std.testing.expectEqual(ctrl_enable, mpu.CTRL);
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), mpu.MAIR0);
}

test "configure rejects a non-power-of-two size" {
    setup(16);
    const r = region(0x1500, perm_rw, perm_rw);
    const cfg = oneRegionCfg(&r);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_configure(&cfg));
}

test "configure rejects a misaligned base" {
    setup(16);
    var r = region(0x1000, perm_rw, perm_rw);
    r.base = 0x20000010;
    const cfg = oneRegionCfg(&r);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_configure(&cfg));
}

test "configure rejects priv RO with unpriv RW" {
    setup(16);
    const r = region(0x1000, perm_ro, perm_rw);
    const cfg = oneRegionCfg(&r);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_configure(&cfg));
}

test "a rejected configuration leaves the MPU untouched" {
    setup(16);
    const mpu = abi.testRegs();
    mpu.CTRL = ctrl_enable;
    mpu.MAIR0 = 0xDEADBEEF;
    const r = region(0x1500, perm_rw, perm_rw);
    const cfg = oneRegionCfg(&r);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_configure(&cfg));
    try std.testing.expectEqual(ctrl_enable, mpu.CTRL);
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), mpu.MAIR0);
}

test "configure programs region zero and writes MAIR" {
    setup(16);
    var r = region(0x1000, perm_rw, perm_none);
    r.executable = 0;
    r.shareable = 3;
    r.attr_idx = 2;
    const cfg = Config{
        .regions = @ptrCast(&r),
        .region_count = 1,
        .mair0 = 0x44440000,
        .mair1 = 0x00000044,
        .privdefena = 1,
        .hfnmiena = 0,
    };
    try std.testing.expectEqual(ok, abi.ra8_mpu_configure(&cfg));

    const mpu = abi.testRegs();
    try std.testing.expectEqual(@as(u32, 0x44440000), mpu.MAIR0);
    try std.testing.expectEqual(@as(u32, 0x00000044), mpu.MAIR1);
    // RNR ends on the last cleared region, DREGION - 1.
    try std.testing.expectEqual(@as(u32, 15), mpu.RNR);
    try std.testing.expectEqual(ctrl_enable | ctrl_privdefena, mpu.CTRL);
}

test "configure honours HFNMIENA" {
    setup(16);
    const r = region(0x1000, perm_rw, perm_rw);
    var cfg = oneRegionCfg(&r);
    cfg.hfnmiena = 1;
    cfg.privdefena = 1;
    try std.testing.expectEqual(ok, abi.ra8_mpu_configure(&cfg));
    try std.testing.expectEqual(
        ctrl_enable | ctrl_privdefena | ctrl_hfnmiena,
        abi.testRegs().CTRL,
    );
}

test "configure clears regions above the requested count" {
    setup(16);
    const mpu = abi.testRegs();
    mpu.RNR = 5;
    mpu.RLAR = 0xFFFFFFFF;
    const r = region(0x1000, perm_rw, perm_rw);
    const cfg = oneRegionCfg(&r);
    try std.testing.expectEqual(ok, abi.ra8_mpu_configure(&cfg));
    // The fake block has one register pair, so the last write wins: the tail
    // clear ran after the map, leaving RLAR zero on the highest region.
    try std.testing.expectEqual(@as(u32, 0), mpu.RLAR);
    try std.testing.expectEqual(@as(u32, 15), mpu.RNR);
}

test "configure enables MemManage delivery through SHCSR" {
    setup(16);
    const r = region(0x1000, perm_rw, perm_rw);
    const cfg = oneRegionCfg(&r);
    try std.testing.expectEqual(ok, abi.ra8_mpu_configure(&cfg));
    try std.testing.expectEqual(@as(u32, 1 << 16), abi.testShcsr().* & (1 << 16));
}

test "configure preserves the other SHCSR bits" {
    setup(16);
    abi.testShcsr().* = 0x00000003;
    const r = region(0x1000, perm_rw, perm_rw);
    const cfg = oneRegionCfg(&r);
    try std.testing.expectEqual(ok, abi.ra8_mpu_configure(&cfg));
    try std.testing.expectEqual(@as(u32, 0x00010003), abi.testShcsr().*);
}

test "configure accepts an empty region table" {
    setup(16);
    const cfg = Config{
        .regions = null,
        .region_count = 0,
        .mair0 = 0,
        .mair1 = 0,
        .privdefena = 0,
        .hfnmiena = 0,
    };
    try std.testing.expectEqual(ok, abi.ra8_mpu_configure(&cfg));
    try std.testing.expectEqual(ctrl_enable, abi.testRegs().CTRL);
}

test "enable and disable toggle CTRL.ENABLE" {
    setup(16);
    try std.testing.expectEqual(ok, abi.ra8_mpu_enable());
    try std.testing.expectEqual(ctrl_enable, abi.testRegs().CTRL & ctrl_enable);
    try std.testing.expectEqual(ok, abi.ra8_mpu_disable());
    try std.testing.expectEqual(@as(u32, 0), abi.testRegs().CTRL & ctrl_enable);
}

test "enable and disable leave the other CTRL bits alone" {
    setup(16);
    abi.testRegs().CTRL = ctrl_privdefena;
    try std.testing.expectEqual(ok, abi.ra8_mpu_enable());
    try std.testing.expectEqual(ctrl_privdefena | ctrl_enable, abi.testRegs().CTRL);
    try std.testing.expectEqual(ok, abi.ra8_mpu_disable());
    try std.testing.expectEqual(ctrl_privdefena, abi.testRegs().CTRL);
}

test "set_region rejects a null descriptor and logs once" {
    setup(16);
    try std.testing.expectEqual(null_ptr, abi.ra8_mpu_set_region(0, null));
    try std.testing.expectEqual(@as(usize, 1), log_calls);
    try std.testing.expectEqualStrings("region_cfg must not be nullptr", std.mem.span(last_message));
}

test "set_region rejects an out-of-range index" {
    setup(16);
    const r = region(0x1000, perm_rw, perm_rw);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_set_region(16, &r));
    try std.testing.expectEqual(ok, abi.ra8_mpu_set_region(15, &r));
}

test "set_region reports the null descriptor before the index range" {
    setup(16);
    try std.testing.expectEqual(null_ptr, abi.ra8_mpu_set_region(200, null));
}

test "set_region writes the RBAR and RLAR pair for the selected region" {
    setup(16);
    var r = region(0x1000, perm_ro, perm_ro);
    r.attr_idx = 3;
    try std.testing.expectEqual(ok, abi.ra8_mpu_set_region(7, &r));

    const mpu = abi.testRegs();
    try std.testing.expectEqual(@as(u32, 7), mpu.RNR);
    try std.testing.expectEqual(@as(u32, 1), mpu.RLAR & 1);
    try std.testing.expectEqual(@as(u32, 3), (mpu.RLAR & 0x0E) >> 1);
    try std.testing.expectEqual(@as(u32, 0x20000000), mpu.RBAR & 0xFFFFFFE0);
    try std.testing.expectEqual(@as(u32, 3), (mpu.RBAR & 0x06) >> 1);
    try std.testing.expectEqual(@as(u32, 0), mpu.RBAR & 1);
}

test "set_region does not touch CTRL" {
    setup(16);
    abi.testRegs().CTRL = ctrl_enable;
    const r = region(0x1000, perm_rw, perm_rw);
    try std.testing.expectEqual(ok, abi.ra8_mpu_set_region(0, &r));
    try std.testing.expectEqual(ctrl_enable, abi.testRegs().CTRL);
}

test "MC/DC is_pow2: value != 0 and (value & (value - 1)) == 0" {
    setup(16);
    // Vector 1: size 0, first condition false.
    const r0 = region(0, perm_rw, perm_rw);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_set_region(0, &r0));
    // Vector 2: size 3, first true, second false.
    const r3 = region(3, perm_rw, perm_rw);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_set_region(0, &r3));
    // Vector 3: size 32, both true.
    const r32 = region(32, perm_rw, perm_rw);
    try std.testing.expectEqual(ok, abi.ra8_mpu_set_region(0, &r32));
}

test "MC/DC size guard: !is_pow2 or size < min" {
    setup(16);
    const r_ok = region(32, perm_rw, perm_rw);
    try std.testing.expectEqual(ok, abi.ra8_mpu_set_region(0, &r_ok));
    const r_npow2 = region(3, perm_rw, perm_rw);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_set_region(0, &r_npow2));
    const r_small = region(16, perm_rw, perm_rw);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_set_region(0, &r_small));
}

test "MC/DC encode_ap: priv == rw and unpriv == rw" {
    setup(16);
    const r1 = region(0x1000, perm_ro, perm_rw);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_set_region(0, &r1));
    const r2 = region(0x1000, perm_rw, perm_ro);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_set_region(0, &r2));
    const r3 = region(0x1000, perm_rw, perm_rw);
    try std.testing.expectEqual(ok, abi.ra8_mpu_set_region(0, &r3));
}

test "MC/DC encode_ap: priv == ro and unpriv == none" {
    setup(16);
    const r1 = region(0x1000, perm_rw, perm_ro);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_set_region(0, &r1));
    const r2 = region(0x1000, perm_ro, perm_ro);
    try std.testing.expectEqual(ok, abi.ra8_mpu_set_region(0, &r2));
    const r3 = region(0x1000, perm_ro, perm_none);
    try std.testing.expectEqual(ok, abi.ra8_mpu_set_region(0, &r3));
}

test "MC/DC validate_cfg: region_count > 0 and regions == null" {
    setup(16);
    const cfg_v1 = Config{
        .regions = null,
        .region_count = 0,
        .mair0 = 0,
        .mair1 = 0,
        .privdefena = 0,
        .hfnmiena = 0,
    };
    try std.testing.expectEqual(ok, abi.ra8_mpu_configure(&cfg_v1));

    const r = region(0x1000, perm_rw, perm_rw);
    const cfg_v2 = oneRegionCfg(&r);
    try std.testing.expectEqual(ok, abi.ra8_mpu_configure(&cfg_v2));

    const cfg_v3 = Config{
        .regions = null,
        .region_count = 1,
        .mair0 = 0,
        .mair1 = 0,
        .privdefena = 0,
        .hfnmiena = 0,
    };
    try std.testing.expectEqual(null_ptr, abi.ra8_mpu_configure(&cfg_v3));
}

test "boot_map publishes the five-region table" {
    setup(16);
    var count: u8 = 0;
    const map = abi.ra8_mpu_boot_map(&count) orelse return error.TestUnexpectedResult;
    try std.testing.expectEqual(@as(u8, 5), count);
    try std.testing.expectEqual(@as(usize, 0x02000000), map[0].base);
    try std.testing.expectEqual(@as(usize, 0x22100000), map[4].base);
    try std.testing.expectEqual(@as(u32, 0x000A0000), map[4].size);
}

test "boot_map with no count destination returns null" {
    setup(16);
    try std.testing.expect(abi.ra8_mpu_boot_map(null) == null);
}

test "the size-checked setter rejects the boot map's shared bank" {
    setup(16);
    var count: u8 = 0;
    const map = abi.ra8_mpu_boot_map(&count) orelse return error.TestUnexpectedResult;
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_set_region(0, &map[4]));
}

test "apply_boot_map installs the map and enables the MPU" {
    setup(16);
    try std.testing.expectEqual(ok, abi.ra8_mpu_apply_boot_map());
    try std.testing.expectEqual(@as(u8, 1), abi.ra8_mpu_is_enabled());

    const mpu = abi.testRegs();
    try std.testing.expectEqual(@as(u32, 0x000444FF), mpu.MAIR0);
    try std.testing.expectEqual(@as(u32, 0), mpu.MAIR1);
    try std.testing.expectEqual(ctrl_enable | ctrl_privdefena, mpu.CTRL);
    // The tail is cleared before the map is programmed, so region 4 is the
    // last descriptor written and its encoding stays in the register pair.
    try std.testing.expectEqual(@as(u32, 4), mpu.RNR);
    try std.testing.expectEqual(@as(u32, 0x22100003), mpu.RBAR);
    try std.testing.expectEqual(@as(u32, 0x2219FFE3), mpu.RLAR);
    try std.testing.expectEqual(@as(u32, 0x2219FFE0), mpu.RLAR & 0xFFFFFFE0);
    try std.testing.expectEqual(@as(u32, 1), (mpu.RLAR & 0x0E) >> 1);
}

test "apply_boot_map issues the barriers the reset path needs" {
    setup(16);
    try std.testing.expectEqual(ok, abi.ra8_mpu_apply_boot_map());
    try std.testing.expectEqual(@as(usize, 2), dsb_calls);
    try std.testing.expectEqual(@as(usize, 1), isb_calls);
}

test "apply_boot_map never logs" {
    setup(16);
    try std.testing.expectEqual(ok, abi.ra8_mpu_apply_boot_map());
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "apply_boot_map refuses silicon one region short" {
    setup(4);
    try std.testing.expectEqual(invalid_arg, abi.ra8_mpu_apply_boot_map());
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_mpu_is_enabled());
    try std.testing.expectEqual(@as(u32, 0), abi.testRegs().CTRL);
    try std.testing.expectEqual(@as(usize, 0), dsb_calls);
}

test "apply_boot_map accepts silicon with exactly the map's region count" {
    setup(5);
    try std.testing.expectEqual(ok, abi.ra8_mpu_apply_boot_map());
    try std.testing.expectEqual(@as(u8, 1), abi.ra8_mpu_is_enabled());
}

test "apply_boot_map does not touch SHCSR" {
    setup(16);
    try std.testing.expectEqual(ok, abi.ra8_mpu_apply_boot_map());
    try std.testing.expectEqual(@as(u32, 0), abi.testShcsr().*);
}

test "is_enabled tracks CTRL.ENABLE" {
    setup(16);
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_mpu_is_enabled());
    try std.testing.expectEqual(ok, abi.ra8_mpu_enable());
    try std.testing.expectEqual(@as(u8, 1), abi.ra8_mpu_is_enabled());
    try std.testing.expectEqual(ok, abi.ra8_mpu_disable());
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_mpu_is_enabled());
}

test "is_enabled ignores the other CTRL bits" {
    setup(16);
    abi.testRegs().CTRL = ctrl_privdefena | ctrl_hfnmiena;
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_mpu_is_enabled());
}
