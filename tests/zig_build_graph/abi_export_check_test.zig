//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the C header declaration extractor, nm/archive symbol table
//! reader, and missing-export detector in `abi_export_check.zig` (RA8FW-472).

const std = @import("std");
const checker = @import("abi_export_check.zig");

test "extractHeaderDeclarations handles C23 headers and skips inline/typedef/static_assert" {
    const header =
        \\#pragma once
        \\#ifdef __cplusplus
        \\extern "C" {
        \\#endif
        \\#define MULTILINE_MACRO(x) \
        \\    do { void not_a_decl(void); } while (0)
        \\
        \\static_assert(sizeof(uint32_t) == 4U, "width");
        \\_Static_assert(1 == 1, "c11");
        \\typedef void (*ra8_cb_t)(int status);
        \\extern void (*g_hook)(int code);
        \\typedef struct { int a; } widget_t;
        \\
        \\/* prototype-like text in comment: void ignored_comment(void); */
        \\// void ignored_line_comment(void);
        \\static inline uint32_t inline_helper(uint32_t x) {
        \\    if (x > 0U) { return x; }
        \\    return 0U;
        \\}
        \\
        \\[[nodiscard]] ra8_err_t real_fn_one(const char *path,
        \\                                    uint8_t buf[static 16]);
        \\void real_fn_two(void) __attribute__((noreturn));
        \\extern int real_fn_three(int a, int b);
        \\#ifdef __cplusplus
        \\}
        \\#endif
    ;

    const alloc = std.testing.allocator;
    const decls = try checker.extractHeaderDeclarations(alloc, header);
    defer {
        for (decls) |d| alloc.free(d);
        alloc.free(decls);
    }

    try std.testing.expectEqual(@as(usize, 3), decls.len);
    try std.testing.expectEqualStrings("real_fn_one", decls[0]);
    try std.testing.expectEqualStrings("real_fn_two", decls[1]);
    try std.testing.expectEqualStrings("real_fn_three", decls[2]);
}

test "parseNmDefinedSymbols filters undefined entries and collects defined exports" {
    const nm_out =
        \\libexample.a(example.o):
        \\0000000000000010 T ra8_box_init
        \\                 U ra8_log_emit_error
        \\0000000000000080 W ra8_box_weak_hook
        \\0000000000000100 D ra8_box_table
        \\0000000000000120 t local_helper
        \\
    ;
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();

    const syms = try checker.parseNmDefinedSymbols(arena.allocator(), nm_out, false);
    try std.testing.expect(syms.contains("ra8_box_init"));
    try std.testing.expect(syms.contains("ra8_box_weak_hook"));
    try std.testing.expect(syms.contains("ra8_box_table"));
    try std.testing.expect(!syms.contains("ra8_log_emit_error"));
    try std.testing.expect(!syms.contains("local_helper"));
}

test "parseArSymbolTable reads SysV/COFF archive symbol index" {
    // Minimal ar archive with "/" global symbol table containing two symbols.
    var buf: [256]u8 = undefined;
    var stream: std.ArrayList(u8) = .empty;
    defer stream.deinit(std.testing.allocator);

    const payload = [_]u8{
        0, 0, 0, 2, // count = 2 (BE)
        0, 0, 0, 0, // offset 0
        0,   0,   0,   0, // offset 1
        'f', 'o', 'o', '_',
        'a', 0,   'b', 'a',
        'r', '_', 'b', 0,
    };
    const hdr = try std.fmt.bufPrint(&buf, "/               0           0     0     0       {d:<10}`\n", .{payload.len});

    try stream.appendSlice(std.testing.allocator, "!<arch>\n");
    try stream.appendSlice(std.testing.allocator, hdr);
    try stream.appendSlice(std.testing.allocator, &payload);

    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const syms = try checker.parseArSymbolTable(arena.allocator(), stream.items);
    try std.testing.expect(syms.contains("foo_a"));
    try std.testing.expect(syms.contains("bar_b"));
    try std.testing.expect(!syms.contains("missing_c"));
}

test "findMissing detects missing export when symbol disappears from archive" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();

    const syms = try checker.parseNmDefinedSymbols(
        alloc,
        "0000 T fn_present\n",
        false,
    );
    const decls = [_][]const u8{ "fn_present", "fn_removed" };
    const missing = try checker.findMissing(alloc, &decls, &syms);
    try std.testing.expectEqual(@as(usize, 1), missing.len);
    try std.testing.expectEqualStrings("fn_removed", missing[0]);
}
