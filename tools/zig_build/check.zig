//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tool behind the RA8FW-330 check steps. Zig 0.17 builds no longer run
//! step code inside the build runner, so each check this package offers is a
//! run of this tool with what it needs on the command line.
//!
//! Usage:
//!   check bundled-stub <zig lib dir>
//!   check explain <zig lib dir> <report head> <report tail>

const std = @import("std");
const macos_host = @import("macos_host");

pub fn main(init: std.process.Init) !void {
    const arena = init.arena.allocator();
    const args = try init.minimal.args.toSlice(arena);
    if (args.len < 3) usage();
    const stub = readBundledStub(arena, init.io, args[2]);
    if (std.mem.eql(u8, args[1], "bundled-stub") and args.len == 3) {
        return verifyBundledStub(stub);
    }
    if (std.mem.eql(u8, args[1], "explain") and args.len == 5) {
        std.debug.print("{s}", .{args[3]});
        std.debug.print("  bundled:   {s}\n", .{stub.path});
        std.debug.print("  bundled finding: {s}\n", .{stub.state().explain()});
        std.debug.print("{s}", .{args[4]});
        return;
    }
    usage();
}

fn usage() noreturn {
    std.process.fatal(
        "usage: check bundled-stub <zig lib dir> | check explain <zig lib dir> <head> <tail>",
        .{},
    );
}

/// Zig's own bundled `libSystem` stub: where it was looked for and what it held.
const BundledStub = struct {
    path: []const u8,
    text: ?[]const u8,

    fn state(self: BundledStub) macos_host.BundledStubState {
        return macos_host.classifyBundledStub(self.text, true);
    }
};

/// Read the stub under `lib_dir`. A missing or unreadable file is a finding
/// (`unreadable`), not a crash, so the report can say which one it was.
fn readBundledStub(arena: std.mem.Allocator, io: std.Io, lib_dir: []const u8) BundledStub {
    const path = std.fs.path.join(arena, &.{ lib_dir, macos_host.bundled_stub_relative_path }) catch
        @panic("OOM");
    const text = std.Io.Dir.cwd().readFileAlloc(io, path, arena, .limited(8 * 1024 * 1024)) catch
        return .{ .path = path, .text = null };
    return .{ .path = path, .text = text };
}

/// Refuse a toolchain whose bundled stub cannot link the pinned target.
fn verifyBundledStub(stub: BundledStub) void {
    const state = stub.state();
    if (!state.linksRequiredTarget()) {
        std.process.fatal(
            "{s}: {s}. The RA8FW-330 workaround pins an explicit {s} target so this stub is " ++
                "linked instead of the SDK one, so a toolchain whose own stub cannot link " ++
                "{s} breaks the pinned path as well as the native one. Check the Zig version " ++
                "pin in .devcontainer/Dockerfile.",
            .{ stub.path, state.explain(), macos_host.required_target, macos_host.required_target },
        );
    }
    std.debug.print(
        "verify-bundled-stub: {s} declares {s}, so the pinned host target links against it\n",
        .{ stub.path, macos_host.required_target },
    );
}
