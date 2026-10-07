//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build-time witness for the FPU probe's codegen. Given the ELF32 object
//! that `zig build test` compiles from src/fpu_probe_abi.zig for the default
//! cortex_m85 (single-precision FPU) target, require undefined references to
//! __aeabi_dmul and __aeabi_dadd. Their presence proves the target lowered
//! f64 through the soft-float helpers, which is what the probe exists to
//! show. A target that silently gained fp64 would emit .f64 opcodes instead
//! and fail here.

const std = @import("std");

const required = [_][]const u8{ "__aeabi_dmul", "__aeabi_dadd" };
const sht_symtab: u32 = 2;
const shdr_size: usize = 40;
const sym_size: usize = 16;

fn rd32(bytes: []const u8, at: usize) u32 {
    return std.mem.readInt(u32, bytes[at..][0..4], .little);
}

fn rd16(bytes: []const u8, at: usize) u16 {
    return std.mem.readInt(u16, bytes[at..][0..2], .little);
}

/// True when `name` is an undefined symbol in the object's .symtab.
fn hasUndefined(elf: []const u8, name: []const u8) bool {
    const shoff = rd32(elf, 0x20);
    const shnum = rd16(elf, 0x30);
    var i: usize = 0;
    while (i < shnum) : (i += 1) {
        const sh = shoff + i * shdr_size;
        if (rd32(elf, sh + 4) != sht_symtab) continue;
        const sym_off = rd32(elf, sh + 16);
        const sym_len = rd32(elf, sh + 20);
        const str_sh = shoff + rd32(elf, sh + 24) * shdr_size;
        const str_off = rd32(elf, str_sh + 16);
        var s: usize = sym_off;
        while (s + sym_size <= sym_off + sym_len) : (s += sym_size) {
            if (rd16(elf, s + 14) != 0) continue;
            const sym_name = std.mem.sliceTo(elf[str_off + rd32(elf, s) ..], 0);
            if (std.mem.eql(u8, sym_name, name)) return true;
        }
    }
    return false;
}

pub fn main(init: std.process.Init) !void {
    const gpa = init.arena.allocator();
    const args = try init.minimal.args.toSlice(gpa);
    if (args.len != 2) return error.Usage;
    const elf = try std.Io.Dir.cwd().readFileAlloc(init.io, args[1], gpa, .limited(1 << 20));
    if (elf.len < 0x34 or !std.mem.eql(u8, elf[0..4], "\x7fELF") or elf[4] != 1) return error.NotElf32;
    for (required) |name| {
        if (!hasUndefined(elf, name)) {
            std.debug.print("fpu_probe: {s} not referenced; f64 was not lowered to soft-float helpers\n", .{name});
            return error.LoweringChanged;
        }
    }
}
