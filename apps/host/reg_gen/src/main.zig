//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! reg_gen: Generates C23 register headers from JSON register definitions.

const std = @import("std");

const Register = struct {
    name: []const u8,
    offset: []const u8,
    size: u32,
    description: []const u8,
};

const PeripheralDef = struct {
    peripheral: []const u8,
    base_address: []const u8,
    registers: []const Register,
};

fn toLower(allocator: std.mem.Allocator, input: []const u8) ![]u8 {
    const result = try allocator.alloc(u8, input.len);
    for (input, 0..) |c, i| {
        result[i] = std.ascii.toLower(c);
    }
    return result;
}

fn toUpper(allocator: std.mem.Allocator, input: []const u8) ![]u8 {
    const result = try allocator.alloc(u8, input.len);
    for (input, 0..) |c, i| {
        result[i] = std.ascii.toUpper(c);
    }
    return result;
}

fn determineSmallestType(max_val: u64) []const u8 {
    if (max_val <= 0xFF) {
        return "uint8_t";
    } else if (max_val <= 0xFFFF) {
        return "uint16_t";
    } else if (max_val <= 0xFFFFFFFF) {
        return "uint32_t";
    } else {
        return "uint64_t";
    }
}

fn findJsonFile(allocator: std.mem.Allocator) ![]const u8 {
    const candidates = [_][]const u8{
        "registers.json",
        "apps/host/reg_gen/registers.json",
    };

    for (candidates) |path| {
        const file = std.fs.cwd().openFile(path, .{}) catch continue;
        file.close();
        return try allocator.dupe(u8, path);
    }

    // Try relative to executable
    var bin_dir_buf: [std.fs.max_path_bytes]u8 = undefined;
    const exe_dir = std.fs.selfExeDirPath(&bin_dir_buf) catch null;
    if (exe_dir) |dir| {
        const joined = try std.fs.path.join(allocator, &[_][]const u8{ dir, "registers.json" });
        const file = std.fs.cwd().openFile(joined, .{}) catch null;
        if (file) |f| {
            f.close();
            return joined;
        }
        allocator.free(joined);
    }

    return error.FileNotFound;
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    const args = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, args);

    const json_path = if (args.len > 1)
        try allocator.dupe(u8, args[1])
    else
        try findJsonFile(allocator);
    defer allocator.free(json_path);

    const file = try std.fs.cwd().openFile(json_path, .{});
    defer file.close();

    const json_bytes = try file.readToEndAlloc(allocator, 1024 * 1024);
    defer allocator.free(json_bytes);

    const parsed = try std.json.parseFromSlice(
        PeripheralDef,
        allocator,
        json_bytes,
        .{ .ignore_unknown_fields = true },
    );
    defer parsed.deinit();

    const def = parsed.value;
    const peri_lower = try toLower(allocator, def.peripheral);
    defer allocator.free(peri_lower);
    const peri_upper = try toUpper(allocator, def.peripheral);
    defer allocator.free(peri_upper);

    // Determine smallest fitting integer type for register offsets
    var max_offset: u64 = 0;
    for (def.registers) |reg| {
        const offset_val = try std.fmt.parseInt(u64, reg.offset, 0);
        if (offset_val > max_offset) {
            max_offset = offset_val;
        }
    }
    const offset_type = determineSmallestType(max_offset);

    const stdout = std.io.getStdOut().writer();

    try stdout.print(
        \\/**
        \\ * @file {s}_regs.h
        \\ * @brief {s} Register Definitions
        \\ *
        \\ * @copyright Copyright (c) 2026 Brighton Sikarskie
        \\ * SPDX-License-Identifier: MIT
        \\ */
        \\
        \\#pragma once
        \\
        \\#include <stdint.h>
        \\
        \\#ifdef __cplusplus
        \\extern "C" {{
        \\#endif
        \\
        \\/**
        \\ * @brief {s} Base Address.
        \\ */
        \\typedef enum : uintptr_t {{
        \\  k_{s}_base_addr = {s}UL, /**< Base address for {s}. */
        \\}} {s}_addr_t;
        \\
        \\/**
        \\ * @brief {s} Register Offsets.
        \\ */
        \\typedef enum : {s} {{
        \\
    , .{
        peri_lower,
        peri_upper,
        peri_upper,
        peri_lower,
        def.base_address,
        peri_upper,
        peri_lower,
        peri_upper,
        offset_type,
    });

    for (def.registers) |reg| {
        const reg_lower = try toLower(allocator, reg.name);
        defer allocator.free(reg_lower);
        try stdout.print("  k_{s}_{s}_offset = {s}U, /**< {s} */\n", .{
            peri_lower,
            reg_lower,
            reg.offset,
            reg.description,
        });
    }

    try stdout.print(
        \\}} {s}_offset_t;
        \\
        \\/**
        \\ * @struct {s}_regs_t
        \\ * @brief {s} register block layout.
        \\ */
        \\typedef struct {{
        \\
    , .{
        peri_lower,
        peri_lower,
        peri_upper,
    });

    for (def.registers) |reg| {
        const reg_upper = try toUpper(allocator, reg.name);
        defer allocator.free(reg_upper);
        try stdout.print("  volatile uint{d}_t {s}; /**< +{s} {s} */\n", .{
            reg.size,
            reg_upper,
            reg.offset,
            reg.description,
        });
    }

    try stdout.print(
        \\}} {s}_regs_t;
        \\
        \\/**
        \\ * @brief Initialize {s} registers struct with C23 zero-initialization.
        \\ */
        \\static inline {s}_regs_t {s}_regs_init(void)
        \\{{
        \\  {s}_regs_t regs = {{}};
        \\  return regs;
        \\}}
        \\
        \\#ifdef __cplusplus
        \\}}
        \\#endif
        \\
    , .{
        peri_lower,
        peri_upper,
        peri_lower,
        peri_lower,
        peri_lower,
    });
}
