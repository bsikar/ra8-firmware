//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/adc_selfdiag.zig (RA8FW-610), on a RAM
//! register file.

const std = @import("std");
const ad = @import("adc_selfdiag");

const Fake = struct {
    regs: [0x2200 / 4]u32 = [_]u32{0} ** (0x2200 / 4),
    busy: bool = false,
    adstr_writes: usize = 0,
    diag_seen: u32 = 0,
    errors: usize = 0,
    last_err: []const u8 = "",
    last_val: u16 = 0,

    pub fn read32(self: *Fake, off: usize) u32 {
        if (off == ad.off_adsr) return if (self.busy) ad.adsr_adact0 else 0;
        return self.regs[off / 4];
    }
    pub fn write32(self: *Fake, off: usize, v: u32) void {
        if (off == ad.adstr(ad.diag_group)) {
            self.adstr_writes += 1;
            self.diag_seen = self.regs[ad.adsgdcr(ad.diag_group) / 4];
        }
        self.regs[off / 4] = v;
    }
    pub fn err(self: *Fake, msg: [*:0]const u8) void {
        self.errors += 1;
        self.last_err = std.mem.span(msg);
    }
    pub fn errVal(self: *Fake, code: u16) void {
        self.last_val = code;
    }
    fn set(self: *Fake, off: usize, v: u32) void {
        self.regs[off / 4] = v;
    }
    fn get(self: *Fake, off: usize) u32 {
        return self.regs[off / 4];
    }
};

test "self_diagnose rejects null outputs and bad mode" {
    var f = Fake{};
    var code: u16 = 7;
    var pass = true;
    try std.testing.expectEqual(ad.null_ptr, ad.selfDiagnose(&f, 1, null, &pass));
    try std.testing.expectEqualStrings("out_code must not be nullptr", f.last_err);
    try std.testing.expectEqual(ad.null_ptr, ad.selfDiagnose(&f, 1, &code, null));
    try std.testing.expectEqual(ad.invalid_arg, ad.selfDiagnose(&f, 0, &code, &pass));
    try std.testing.expectEqual(@as(u16, 0), code);
    try std.testing.expect(!pass);
    try std.testing.expectEqual(ad.invalid_arg, ad.selfDiagnose(&f, 4, &code, &pass));
    try std.testing.expectEqual(@as(usize, 0), f.adstr_writes);
}

test "self_diagnose mode 1 programs the diag slot and passes" {
    var f = Fake{};
    f.set(ad.addopcrc(ad.diag_vchan), 0xFFFF_FFFF);
    f.set(ad.adexdr(0), 0x0010);
    var code: u16 = 0;
    var pass = false;
    try std.testing.expectEqual(ad.ok, ad.selfDiagnose(&f, 1, &code, &pass));
    try std.testing.expectEqual(@as(u16, 0x0010), code);
    try std.testing.expect(pass);
    try std.testing.expectEqual(@as(u32, 0x6008 | 0x8000), f.get(ad.adchcr(ad.diag_vchan)));
    try std.testing.expectEqual(@as(u32, 0xFFEC_FFFF), f.get(ad.addopcrc(ad.diag_vchan)));
    try std.testing.expectEqual(@as(u32, 0x100), f.get(ad.off_adsger));
    try std.testing.expectEqual(ad.adstr_adst, f.get(ad.adstr(ad.diag_group)));
    try std.testing.expectEqual(@as(u32, 0x4), f.diag_seen);
    try std.testing.expectEqual(@as(u32, 0), f.get(ad.adsgdcr(ad.diag_group)) & 0x7);
}

test "self_diagnose modes 2 and 3 compare signed codes" {
    var f = Fake{};
    var code: u16 = 0;
    var pass = false;
    f.set(ad.adexdr(0), 0x8000);
    try std.testing.expectEqual(ad.ok, ad.selfDiagnose(&f, 2, &code, &pass));
    try std.testing.expect(pass);
    try std.testing.expectEqual(@as(u32, 0x5), f.diag_seen);
    f.set(ad.adexdr(0), 0x7F00);
    try std.testing.expectEqual(ad.ok, ad.selfDiagnose(&f, 3, &code, &pass));
    try std.testing.expect(pass);
    f.set(ad.adexdr(0), 0x7EFE);
    try std.testing.expectEqual(ad.ok, ad.selfDiagnose(&f, 3, &code, &pass));
    try std.testing.expect(!pass);
    try std.testing.expectEqual(@as(u16, 0x7EFE), code);
}

test "self_diagnose ERR flag fails an in-band code" {
    var f = Fake{};
    f.set(ad.adexdr(0), 0x8000_0000);
    var code: u16 = 1;
    var pass = true;
    try std.testing.expectEqual(ad.ok, ad.selfDiagnose(&f, 1, &code, &pass));
    try std.testing.expectEqual(@as(u16, 0), code);
    try std.testing.expect(!pass);
}

test "self_diagnose timeout logs and still clears DIAGVAL" {
    var f = Fake{ .busy = true };
    f.set(ad.adsgdcr(ad.diag_group), 0xF0);
    var code: u16 = 0;
    var pass = true;
    try std.testing.expectEqual(ad.hw_timeout, ad.selfDiagnose(&f, 3, &code, &pass));
    try std.testing.expectEqualStrings("self_diagnose: conversion", f.last_err);
    try std.testing.expectEqual(ad.hw_timeout, f.last_val);
    try std.testing.expectEqual(@as(u32, 0xF6), f.diag_seen);
    try std.testing.expectEqual(@as(u32, 0xF0), f.get(ad.adsgdcr(ad.diag_group)));
    try std.testing.expect(!pass);
}

test "read_internal rejects null and unsupported channels" {
    var f = Fake{};
    var raw: u16 = 9;
    try std.testing.expectEqual(ad.null_ptr, ad.readInternalChannel(&f, ad.chan_temperature, null));
    try std.testing.expectEqualStrings("out_raw must not be nullptr", f.last_err);
    try std.testing.expectEqual(ad.invalid_arg, ad.readInternalChannel(&f, 0x60, &raw));
    try std.testing.expectEqual(@as(u16, 0), raw);
    try std.testing.expectEqual(ad.invalid_arg, ad.readInternalChannel(&f, 3, &raw));
}

test "read_internal temperature and vref read their own ADEXDR" {
    var f = Fake{};
    f.set(ad.adexdr(4), 0xABCD_0123);
    f.set(ad.adexdr(5), 0x0000_0456);
    var raw: u16 = 0;
    try std.testing.expectEqual(ad.ok, ad.readInternalChannel(&f, ad.chan_temperature, &raw));
    try std.testing.expectEqual(@as(u16, 0x0123), raw);
    try std.testing.expectEqual(@as(u32, 0x6408), f.get(ad.adchcr(ad.diag_vchan)));
    try std.testing.expectEqual(@as(u32, 0x0012_0000), f.get(ad.addopcrc(ad.diag_vchan)));
    try std.testing.expectEqual(ad.ok, ad.readInternalChannel(&f, ad.chan_int_ref_volt, &raw));
    try std.testing.expectEqual(@as(u16, 0x0456), raw);
    try std.testing.expectEqual(@as(u32, 0x6508), f.get(ad.adchcr(ad.diag_vchan)));
}

test "read_internal timeout logs and leaves zero" {
    var f = Fake{ .busy = true };
    var raw: u16 = 5;
    try std.testing.expectEqual(ad.hw_timeout, ad.readInternalChannel(&f, ad.chan_temperature, &raw));
    try std.testing.expectEqualStrings("read_internal: conversion", f.last_err);
    try std.testing.expectEqual(@as(u16, 0), raw);
}

test "mode helpers match the C tables" {
    try std.testing.expectEqual(@as(?u32, null), ad.diagvalForMode(0));
    try std.testing.expectEqual(@as(i32, 0), ad.expected(9));
    try std.testing.expect(ad.inBand(256) and ad.inBand(-256));
    try std.testing.expect(!ad.inBand(257) and !ad.inBand(-257));
    try std.testing.expectEqual(ad.out_of_range, ad.startAndWait(@as(*Fake, @constCast(&Fake{})), 9));
}
