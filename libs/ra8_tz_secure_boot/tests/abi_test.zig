//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the exported C surface. The log sinks below are exported from
//! the test binary itself, the same link-time substitution the real build
//! performs against `libs/ra8_core/src/ra8_log.c`, so each guard's message is
//! asserted rather than assumed. Every vector the C host suite drives through
//! `tests/security/src/test_tz_secure_boot.c` is mirrored here.

const std = @import("std");
const abi = @import("abi");

var error_count: usize = 0;
var error_val_count: usize = 0;
var last_message: []const u8 = "";
var last_value: u32 = 0;
var last_tag: []const u8 = "";

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    error_count += 1;
    last_tag = std.mem.span(tag);
    last_message = std.mem.span(message);
}

export fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void {
    error_val_count += 1;
    last_tag = std.mem.span(tag);
    last_message = std.mem.span(message);
    last_value = value;
}

extern fn ra8_tz_secure_boot_sau_init() callconv(.c) u16;
extern fn ra8_tz_secure_boot_security_init(ipcsar_value: u32, ipcpar_value: u32) callconv(.c) u16;
extern fn ra8_tz_secure_boot_jump_ns(ns_vector_table: ?[*]const u32) callconv(.c) u16;
extern fn ra8_tz_secure_boot_run(
    ipcsar_value: u32,
    ipcpar_value: u32,
    ns_vector_table: ?[*]const u32,
) callconv(.c) u16;
extern fn ra8_tz_ns_signed_body_len(ns_vector_table: ?[*]const u32) callconv(.c) u32;
extern fn ra8_tz_secure_boot_host_reset() callconv(.c) void;
extern fn ra8_tz_secure_boot_host_blxns_target() callconv(.c) u32;
extern fn ra8_tz_secure_boot_get_step() callconv(.c) u8;

const ok: u16 = 0;
const invalid_arg: u16 = 0x103;
const not_supported: u16 = 0x107;
const null_ptr: u16 = 0x504;

const ipcsar_canonical: u32 = 0x00050000;
const ipcpar_canonical: u32 = 0x00000000;

fn begin() void {
    ra8_tz_secure_boot_host_reset();
    error_count = 0;
    error_val_count = 0;
    last_message = "";
    last_value = 0;
    last_tag = "";
}

fn step() u8 {
    return ra8_tz_secure_boot_get_step();
}

fn stepOf(value: abi.Step) u8 {
    return @intFromEnum(value);
}

test "host reset leaves the step counter idle" {
    begin();
    try std.testing.expectEqual(stepOf(.idle), step());
    try std.testing.expectEqual(@as(u32, 0), ra8_tz_secure_boot_host_blxns_target());
}

test "sau_init reports ok on a chip with eight regions" {
    begin();
    try std.testing.expectEqual(ok, ra8_tz_secure_boot_sau_init());
}

test "sau_init advances the step counter to sau_done" {
    begin();
    try std.testing.expectEqual(stepOf(.idle), step());
    _ = ra8_tz_secure_boot_sau_init();
    try std.testing.expectEqual(stepOf(.sau_done), step());
}

test "sau_init logs nothing on the happy path" {
    begin();
    _ = ra8_tz_secure_boot_sau_init();
    try std.testing.expectEqual(@as(usize, 0), error_count);
}

test "sau_init programmes all five regions" {
    begin();
    _ = ra8_tz_secure_boot_sau_init();
    const host = abi.testHostState();
    for (0..5) |i| {
        try std.testing.expect(host.sau_region_base[i] != 0);
        try std.testing.expect(host.sau_region_limit[i] != 0);
    }
}

test "sau_init writes the canonical bases and limits" {
    begin();
    _ = ra8_tz_secure_boot_sau_init();
    const host = abi.testHostState();
    try std.testing.expectEqual(@as(u32, 0x10000000), host.sau_region_base[0]);
    try std.testing.expectEqual(@as(u32, 0x100FFFE0), host.sau_region_limit[0]);
    try std.testing.expectEqual(@as(u32, 0x02080000), host.sau_region_base[1]);
    try std.testing.expectEqual(@as(u32, 0x020FFFE0), host.sau_region_limit[1]);
    try std.testing.expectEqual(@as(u32, 0x12000000), host.sau_region_base[2]);
    try std.testing.expectEqual(@as(u32, 0x1200FFE0), host.sau_region_limit[2]);
    try std.testing.expectEqual(@as(u32, 0x22100000), host.sau_region_base[3]);
    try std.testing.expectEqual(@as(u32, 0x221FFFE0), host.sau_region_limit[3]);
    try std.testing.expectEqual(@as(u32, 0x50000000), host.sau_region_base[4]);
    try std.testing.expectEqual(@as(u32, 0x5FFFFFE0), host.sau_region_limit[4]);
}

test "sau_init marks exactly the two alias regions Non-Secure Callable" {
    begin();
    _ = ra8_tz_secure_boot_sau_init();
    const host = abi.testHostState();
    try std.testing.expectEqual(@as(u8, 1), host.sau_region_nsc[0]);
    try std.testing.expectEqual(@as(u8, 0), host.sau_region_nsc[1]);
    try std.testing.expectEqual(@as(u8, 1), host.sau_region_nsc[2]);
    try std.testing.expectEqual(@as(u8, 0), host.sau_region_nsc[3]);
    try std.testing.expectEqual(@as(u8, 0), host.sau_region_nsc[4]);
}

test "sau_init enables the SAU with ALLNS clear" {
    begin();
    _ = ra8_tz_secure_boot_sau_init();
    const host = abi.testHostState();
    try std.testing.expectEqual(@as(u32, 1), host.sau_ctrl);
    try std.testing.expectEqual(@as(u32, 0), host.sau_ctrl & 0x2);
}

test "sau_init touches neither the CPSCU registers nor VTOR_NS" {
    begin();
    _ = ra8_tz_secure_boot_sau_init();
    const host = abi.testHostState();
    try std.testing.expectEqual(@as(u32, 0), host.ipcsar_value);
    try std.testing.expectEqual(@as(u32, 0), host.ipcpar_value);
    try std.testing.expectEqual(@as(u32, 0), host.vtor_ns);
}

test "security_init reports ok" {
    begin();
    try std.testing.expectEqual(ok, ra8_tz_secure_boot_security_init(ipcsar_canonical, ipcpar_canonical));
}

test "security_init lands the canonical IPCSAR and IPCPAR values" {
    begin();
    _ = ra8_tz_secure_boot_security_init(ipcsar_canonical, ipcpar_canonical);
    const host = abi.testHostState();
    try std.testing.expectEqual(ipcsar_canonical, host.ipcsar_value);
    try std.testing.expectEqual(ipcpar_canonical, host.ipcpar_value);
}

test "security_init opens the PRC4 gate exactly once and closes it once" {
    begin();
    _ = ra8_tz_secure_boot_security_init(ipcsar_canonical, ipcpar_canonical);
    const host = abi.testHostState();
    try std.testing.expectEqual(@as(u8, 1), host.prcr_unlock_count);
    try std.testing.expectEqual(@as(u8, 1), host.prcr_relock_count);
}

test "security_init leaves PRCR_S write-protected" {
    begin();
    _ = ra8_tz_secure_boot_security_init(ipcsar_canonical, ipcpar_canonical);
    const host = abi.testHostState();
    try std.testing.expectEqual(@as(u16, 0xA500), host.prcr_s_last);
    try std.testing.expectEqual(@as(u16, 0), host.prcr_s_last & 0x0010);
}

test "security_init ends at the prcr_relocked step" {
    begin();
    _ = ra8_tz_secure_boot_security_init(ipcsar_canonical, ipcpar_canonical);
    try std.testing.expectEqual(stepOf(.prcr_relocked), step());
}

test "security_init forwards whatever attribution words it is given" {
    begin();
    _ = ra8_tz_secure_boot_security_init(0xDEADBEEF, 0x0000FFFF);
    const host = abi.testHostState();
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), host.ipcsar_value);
    try std.testing.expectEqual(@as(u32, 0x0000FFFF), host.ipcpar_value);
}

test "security_init logs nothing" {
    begin();
    _ = ra8_tz_secure_boot_security_init(ipcsar_canonical, ipcpar_canonical);
    try std.testing.expectEqual(@as(usize, 0), error_count);
    try std.testing.expectEqual(@as(usize, 0), error_val_count);
}

test "jump_ns refuses a NULL vector table" {
    begin();
    try std.testing.expectEqual(null_ptr, ra8_tz_secure_boot_jump_ns(null));
}

test "jump_ns names the argument in the NULL guard's log line" {
    begin();
    _ = ra8_tz_secure_boot_jump_ns(null);
    try std.testing.expectEqual(@as(usize, 1), error_count);
    try std.testing.expectEqualStrings("ns_vector_table", last_message);
    try std.testing.expectEqualStrings("TZBOOT", last_tag);
}

test "jump_ns refuses a misaligned vector table" {
    begin();
    // The C suite hands the exported symbol a byte-offset pointer, which a
    // typed Zig pointer cannot hold, so the call goes through the same symbol
    // viewed with the opaque-pointer signature the C ABI actually uses.
    const jumpOpaque: *const fn (?*const anyopaque) callconv(.c) u16 =
        @ptrCast(&ra8_tz_secure_boot_jump_ns);
    var scratch: [16]u8 align(4) = .{0} ** 16;
    const misaligned: *const anyopaque = @ptrFromInt(@intFromPtr(&scratch) + 1);
    try std.testing.expectEqual(invalid_arg, jumpOpaque(misaligned));
    try std.testing.expectEqualStrings("ns_vector_table misaligned", last_message);
}

test "jump_ns checks NULL before alignment" {
    begin();
    _ = ra8_tz_secure_boot_jump_ns(null);
    try std.testing.expectEqualStrings("ns_vector_table", last_message);
}

test "jump_ns MC/DC vector 1: a valid reset entry takes the happy path" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0x02080101 };
    try std.testing.expectEqual(ok, ra8_tz_secure_boot_jump_ns(&vector_table));
}

test "jump_ns MC/DC vector 2: an all-zero reset entry is refused" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0 };
    try std.testing.expectEqual(invalid_arg, ra8_tz_secure_boot_jump_ns(&vector_table));
}

test "jump_ns MC/DC vector 3: an erased reset entry is refused" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0xFFFFFFFF };
    try std.testing.expectEqual(invalid_arg, ra8_tz_secure_boot_jump_ns(&vector_table));
}

test "jump_ns logs the rejected reset vector with its value" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0xFFFFFFFF };
    _ = ra8_tz_secure_boot_jump_ns(&vector_table);
    try std.testing.expectEqual(@as(usize, 1), error_val_count);
    try std.testing.expectEqualStrings("NS reset vector invalid", last_message);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), last_value);
}

test "jump_ns captures the BLXNS target on the happy path" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0x02080101 };
    _ = ra8_tz_secure_boot_jump_ns(&vector_table);
    try std.testing.expectEqual(@as(u32, 0x02080101), ra8_tz_secure_boot_host_blxns_target());
}

test "jump_ns captures MSP_NS from the first vector slot" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0x02080101 };
    _ = ra8_tz_secure_boot_jump_ns(&vector_table);
    try std.testing.expectEqual(@as(u32, 0x22180000), abi.testHostState().blxns_msp_ns);
}

test "jump_ns arms VTOR_NS with the vector table address" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0x02080101 };
    _ = ra8_tz_secure_boot_jump_ns(&vector_table);
    const expected: u32 = @truncate(@intFromPtr(&vector_table));
    try std.testing.expectEqual(expected, abi.testHostState().vtor_ns);
}

test "jump_ns ends at the branched step on host" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0x02080101 };
    _ = ra8_tz_secure_boot_jump_ns(&vector_table);
    try std.testing.expectEqual(stepOf(.branched), step());
}

test "a refused jump_ns arms nothing" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0 };
    _ = ra8_tz_secure_boot_jump_ns(&vector_table);
    try std.testing.expectEqual(@as(u32, 0), abi.testHostState().vtor_ns);
    try std.testing.expectEqual(@as(u32, 0), ra8_tz_secure_boot_host_blxns_target());
    try std.testing.expectEqual(stepOf(.idle), step());
}

test "run drives the whole sequence to branched" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0x02080101 };
    try std.testing.expectEqual(ok, ra8_tz_secure_boot_run(ipcsar_canonical, ipcpar_canonical, &vector_table));
    try std.testing.expectEqual(stepOf(.branched), step());
    try std.testing.expectEqual(@as(u32, 0x02080101), ra8_tz_secure_boot_host_blxns_target());
}

test "run lands the IPCSAR word on its way through" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0x02080101 };
    _ = ra8_tz_secure_boot_run(ipcsar_canonical, ipcpar_canonical, &vector_table);
    try std.testing.expectEqual(ipcsar_canonical, abi.testHostState().ipcsar_value);
}

test "run programmes the SAU on its way through" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0x02080101 };
    _ = ra8_tz_secure_boot_run(ipcsar_canonical, ipcpar_canonical, &vector_table);
    try std.testing.expectEqual(@as(u32, 1), abi.testHostState().sau_ctrl);
}

test "run refuses a NULL vector table before touching the SAU" {
    begin();
    try std.testing.expectEqual(null_ptr, ra8_tz_secure_boot_run(ipcsar_canonical, ipcpar_canonical, null));
    try std.testing.expectEqual(stepOf(.idle), step());
    try std.testing.expectEqual(@as(u32, 0), abi.testHostState().sau_ctrl);
    try std.testing.expectEqualStrings("ns_vector_table", last_message);
}

test "run stops at jump_ns when the reset vector is bogus" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0 };
    try std.testing.expectEqual(invalid_arg, ra8_tz_secure_boot_run(ipcsar_canonical, ipcpar_canonical, &vector_table));
    // The two earlier phases still ran, so the counter rests where they left it.
    try std.testing.expectEqual(stepOf(.prcr_relocked), step());
}

test "signed_body_len MC/DC vector 1: a valid header reports its body length" {
    begin();
    var image = [_]u32{0} ** 18;
    image[16] = 0x3152534E;
    image[17] = 0x1234;
    try std.testing.expectEqual(@as(u32, 0x1234), ra8_tz_ns_signed_body_len(&image));
}

test "signed_body_len MC/DC vector 2: a NULL base denies" {
    begin();
    try std.testing.expectEqual(@as(u32, 0), ra8_tz_ns_signed_body_len(null));
    try std.testing.expectEqualStrings("ns_vector_table is NULL", last_message);
}

test "signed_body_len MC/DC vector 3: a wrong magic denies" {
    begin();
    var image = [_]u32{0} ** 18;
    image[16] = 0x3152534E ^ 0xFFFFFFFF;
    image[17] = 0x1234;
    try std.testing.expectEqual(@as(u32, 0), ra8_tz_ns_signed_body_len(&image));
    try std.testing.expectEqualStrings("NS RoT header magic mismatch", last_message);
}

test "signed_body_len reads the header at the fixed 0x40 offset" {
    begin();
    var image = [_]u32{0} ** 18;
    image[16] = 0x3152534E;
    image[17] = 0xABCD;
    const bytes: [*]const u8 = @ptrCast(&image);
    const header_at = bytes + 0x40;
    try std.testing.expectEqual(@intFromPtr(&image[16]), @intFromPtr(header_at));
    try std.testing.expectEqual(@as(u32, 0xABCD), ra8_tz_ns_signed_body_len(&image));
}

test "signed_body_len reports a zero body length verbatim" {
    begin();
    var image = [_]u32{0} ** 18;
    image[16] = 0x3152534E;
    image[17] = 0;
    try std.testing.expectEqual(@as(u32, 0), ra8_tz_ns_signed_body_len(&image));
    // Magic was fine, so the deny sentinel came from the body length itself.
    try std.testing.expectEqual(@as(usize, 0), error_count);
}

test "signed_body_len logs nothing on the happy path" {
    begin();
    var image = [_]u32{0} ** 18;
    image[16] = 0x3152534E;
    image[17] = 64;
    _ = ra8_tz_ns_signed_body_len(&image);
    try std.testing.expectEqual(@as(usize, 0), error_count);
}

test "host reset clears the captures between cases" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0x02080101 };
    _ = ra8_tz_secure_boot_run(ipcsar_canonical, ipcpar_canonical, &vector_table);
    ra8_tz_secure_boot_host_reset();
    const host = abi.testHostState();
    try std.testing.expectEqual(@as(u32, 0), host.ipcsar_value);
    try std.testing.expectEqual(@as(u32, 0), host.sau_ctrl);
    try std.testing.expectEqual(@as(u32, 0), host.blxns_target);
    try std.testing.expectEqual(stepOf(.idle), step());
}

test "the step counter only moves forward across a full run" {
    begin();
    const vector_table = [_]u32{ 0x22180000, 0x02080101 };
    try std.testing.expectEqual(stepOf(.idle), step());
    _ = ra8_tz_secure_boot_sau_init();
    try std.testing.expectEqual(stepOf(.sau_done), step());
    _ = ra8_tz_secure_boot_security_init(ipcsar_canonical, ipcpar_canonical);
    try std.testing.expectEqual(stepOf(.prcr_relocked), step());
    _ = ra8_tz_secure_boot_jump_ns(&vector_table);
    try std.testing.expectEqual(stepOf(.branched), step());
}

test "not_supported is the documented code for a chip with too few regions" {
    // The host read32 always reports eight regions, so the guard is proven at
    // the predicate level; this pins the code the guard returns.
    try std.testing.expectEqual(not_supported, @intFromEnum(abi.TzError.not_supported));
    try std.testing.expectEqual(@as(u16, 0x103), @intFromEnum(abi.TzError.invalid_arg));
    try std.testing.expectEqual(@as(u16, 0x504), @intFromEnum(abi.TzError.null_ptr));
    try std.testing.expectEqual(@as(u16, 0x501), @intFromEnum(abi.TzError.validation_failed));
    try std.testing.expectEqual(@as(u16, 0x105), @intFromEnum(abi.TzError.invalid_size));
}
