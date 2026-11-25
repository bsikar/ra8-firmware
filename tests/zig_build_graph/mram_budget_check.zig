//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host verifier enforcing linked `.bin` MRAM bank budgets during `zig build
//! arm`. Links stay fail-closed at the linker script's `MEMORY` block too;
//! this step verifies the emitted flat binary size directly against
//! `MRAM_LENGTH` so shipping bootloaders cannot regress.

const std = @import("std");

pub const usage = "usage: mram_budget_check <app_name> <image.bin> <budget_bytes>";

/// Verify that a linked flat binary is non-empty and fits inside `budget_bytes`.
pub fn checkBudget(app_name: []const u8, actual_bytes: u64, budget_bytes: u64) !void {
    if (actual_bytes == 0) {
        std.debug.print("mram_budget: {s} emitted an empty binary\n", .{app_name});
        return error.EmptyBinary;
    }
    if (actual_bytes > budget_bytes) {
        std.debug.print(
            "mram_budget: {s} binary ({d} B) exceeds MRAM budget ({d} B)\n",
            .{ app_name, actual_bytes, budget_bytes },
        );
        return error.MramBudgetExceeded;
    }
}

pub fn main(init: std.process.Init) !u8 {
    const allocator = init.arena.allocator();
    const io = init.io;
    const argv = try init.minimal.args.toSlice(allocator);
    if (argv.len != 4) {
        std.debug.print("{s}\n", .{usage});
        return 2;
    }

    const app_name = argv[1];
    const bin_path = argv[2];
    const budget_bytes = std.fmt.parseInt(u64, argv[3], 10) catch {
        std.debug.print("mram_budget: invalid budget '{s}'\n", .{argv[3]});
        return 2;
    };

    const file = std.Io.Dir.cwd().openFile(io, bin_path, .{}) catch |err| {
        std.debug.print("mram_budget: cannot open '{s}': {s}\n", .{ bin_path, @errorName(err) });
        return 1;
    };
    defer file.close(io);

    const stat = try file.stat(io);
    checkBudget(app_name, stat.size, budget_bytes) catch return 1;
    return 0;
}
