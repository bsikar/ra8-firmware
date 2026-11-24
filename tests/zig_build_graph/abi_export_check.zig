//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host checker proving every public C function declared in a migrated Zig
//! library's `inc/*.h` headers is defined in its built static archive(s) (RA8FW-472).
//!
//! Usage:
//!   abi_export_check all <zig_exe> <nm_or_dash> <cache_root> <stamp_out>
//!   abi_export_check negative <nm_or_dash> <fixture_archive> <stamp_out>

const builtin = @import("builtin");
const std = @import("std");

pub const StringSet = std.StringHashMapUnmanaged(void);

pub const HeaderDecl = struct {
    header: []const u8,
    symbol: []const u8,
};

const ExternalAllowEntry = struct {
    pkg: []const u8,
    header: []const u8,
    symbol: ?[]const u8, // null exempts every declaration in that header
};

const ExtraArchiveEntry = struct {
    pkg: []const u8,
    provider: []const u8,
};

/// Libraries mid-migration whose `src/` still holds production `.c` units
/// compiled by CMake rather than `build.zig`.
const partial_c_libs = [_][]const u8{
    "libs/ra8_hal",
    "libs/ra8_nsc",
};

/// Sibling Zig packages whose archives satisfy declarations in another
/// library's `inc/` directory.
const extra_archive_providers = [_]ExtraArchiveEntry{
    .{ .pkg = "libs/ra8_dfu", .provider = "libs/ra8_rot" },
    .{ .pkg = "libs/ra8_dfu", .provider = "libs/ra8_rot_launch" },
    .{ .pkg = "apps/shared_libs/reflow", .provider = "libs/ra8_imgdec" },
};

/// Abstract port/boot headers whose definitions live in RTOS ports, HAL, or
/// per-application startup code rather than inside the owning library archive.
const external_exceptions = [_]ExternalAllowEntry{
    .{ .pkg = "libs/if", .header = "fw_os.h", .symbol = null },
    .{ .pkg = "libs/ra8_core", .header = "ra8_boot_entry.h", .symbol = "SystemInit" },
    .{ .pkg = "libs/ra8_core", .header = "ra8_boot_entry.h", .symbol = "Default_Handler" },
    .{ .pkg = "libs/ra8_core", .header = "ra8_boot_entry.h", .symbol = "main" },
    .{ .pkg = "libs/ra8_core", .header = "ra8_pin_interface.h", .symbol = "ra8_pin_interface_default" },
    .{ .pkg = "libs/ra8_board_ek_ra8d2", .header = "trustzone_init.h", .symbol = "ra8_trustzone_init" },
    .{ .pkg = "libs/ra8_board_ra8p1", .header = "trustzone_init.h", .symbol = "ra8_trustzone_init" },
};

const PackageWork = struct {
    pkg_rel: []const u8,
    slug: []const u8,
    build_file: []const u8,
    cache_dir: []const u8,
    prefix_dir: []const u8,
    decls: []const HeaderDecl,
    needs_build: bool,
    symbols: StringSet = .empty,
    build_err: ?[]const u8 = null,
};

pub fn main(init: std.process.Init) !void {
    const arena = init.arena.allocator();
    const args = try init.minimal.args.toSlice(arena);
    if (args.len < 2) {
        std.process.fatal("usage: abi_export_check <all|negative> ...", .{});
    }

    if (std.mem.eql(u8, args[1], "negative")) {
        if (args.len != 5) {
            std.process.fatal("usage: abi_export_check negative <nm_or_dash> <fixture_archive> <stamp_out>", .{});
        }
        try runNegativeMode(arena, init.io, args[2], args[3], args[4]);
        return;
    }

    if (std.mem.eql(u8, args[1], "all")) {
        if (args.len != 6) {
            std.process.fatal("usage: abi_export_check all <zig_exe> <nm_or_dash> <cache_root> <stamp_out>", .{});
        }
        try runAllMode(arena, init.io, args[2], args[3], args[4], args[5]);
        return;
    }

    std.process.fatal("abi_export_check: unknown subcommand '{s}'", .{args[1]});
}

fn runNegativeMode(
    arena: std.mem.Allocator,
    io: std.Io,
    nm_arg: []const u8,
    fixture_archive: []const u8,
    stamp_out: []const u8,
) !void {
    const cwd = std.Io.Dir.cwd();
    const nm_exe: ?[]const u8 = if (std.mem.eql(u8, nm_arg, "-")) null else nm_arg;
    const syms = try extractArchiveSymbols(arena, io, nm_exe, fixture_archive);

    const fixture_header = try cwd.readFileAlloc(
        io,
        "tests/zig_abi_fixture/inc/ra8_abi_fixture.h",
        arena,
        .limited(4 * 1024 * 1024),
    );
    const positive_decls = try extractHeaderDeclarations(arena, fixture_header);
    if (positive_decls.len == 0) {
        std.process.fatal("abi_export_check negative: parsed 0 declarations from fixture header", .{});
    }
    const pos_missing = try findMissing(arena, positive_decls, &syms);
    if (pos_missing.len != 0) {
        std.process.fatal(
            "abi_export_check negative: fixture archive missing '{s}'",
            .{pos_missing[0]},
        );
    }

    const removed_symbol = "ra8_abi_fixture_removed_export";
    const negative_header = try std.fmt.allocPrint(
        arena,
        "{s}\nvoid {s}(void);\n",
        .{ fixture_header, removed_symbol },
    );
    const neg_decls = try extractHeaderDeclarations(arena, negative_header);
    const neg_missing = try findMissing(arena, neg_decls, &syms);
    if (neg_missing.len != 1 or !std.mem.eql(u8, neg_missing[0], removed_symbol)) {
        std.process.fatal(
            "abi_export_check negative: expected single missing export '{s}', got {d}",
            .{ removed_symbol, neg_missing.len },
        );
    }

    try writeStamp(io, stamp_out, "negative-ok\n");
}

fn runAllMode(
    arena: std.mem.Allocator,
    io: std.Io,
    zig_exe: []const u8,
    nm_arg: []const u8,
    cache_root: []const u8,
    stamp_out: []const u8,
) !void {
    const nm_exe: ?[]const u8 = if (std.mem.eql(u8, nm_arg, "-")) null else nm_arg;
    var packages = try discoverPackages(arena, io, cache_root);

    var next_idx: std.atomic.Value(usize) = .init(0);
    const WorkerCtx = struct {
        io: std.Io,
        zig_exe: []const u8,
        nm_exe: ?[]const u8,
        items: []PackageWork,
        next: *std.atomic.Value(usize),

        fn run(ctx: *@This()) void {
            // Each worker keeps its own page-backed arena to avoid allocator locks.
            var worker_arena_state = std.heap.ArenaAllocator.init(std.heap.page_allocator);
            const alloc = worker_arena_state.allocator();
            while (true) {
                const idx = ctx.next.fetchAdd(1, .monotonic);
                if (idx >= ctx.items.len) break;
                var item = &ctx.items[idx];
                if (!item.needs_build) continue;
                buildAndCollectSymbols(alloc, ctx.io, ctx.zig_exe, ctx.nm_exe, item) catch |err| {
                    item.build_err = std.fmt.allocPrint(alloc, "{s}", .{@errorName(err)}) catch "build error";
                };
            }
        }
    };

    var ctx = WorkerCtx{
        .io = io,
        .zig_exe = zig_exe,
        .nm_exe = nm_exe,
        .items = packages,
        .next = &next_idx,
    };

    const worker_count = @min(@as(usize, 4), packages.len);
    var threads: [4]?std.Thread = .{ null, null, null, null };
    for (0..worker_count) |i| {
        threads[i] = try std.Thread.spawn(.{}, WorkerCtx.run, .{&ctx});
    }
    for (0..worker_count) |i| {
        if (threads[i]) |t| t.join();
    }

    var pkg_map: std.StringHashMapUnmanaged(usize) = .empty;
    for (packages, 0..) |pkg, i| {
        try pkg_map.put(arena, pkg.pkg_rel, i);
    }

    var failures: std.ArrayList([]const u8) = .empty;
    for (packages) |pkg| {
        if (pkg.build_err) |msg| {
            try failures.append(arena, try std.fmt.allocPrint(arena, "{s}: build/nm failed: {s}", .{ pkg.pkg_rel, msg }));
            continue;
        }
        if (pkg.decls.len == 0) continue;

        for (pkg.decls) |decl| {
            if (isExternalException(pkg.pkg_rel, decl.header, decl.symbol)) continue;
            var found = pkg.symbols.contains(decl.symbol);
            if (!found) {
                for (extra_archive_providers) |entry| {
                    if (std.mem.eql(u8, entry.pkg, pkg.pkg_rel)) {
                        if (pkg_map.get(entry.provider)) |pidx| {
                            if (packages[pidx].symbols.contains(decl.symbol)) {
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (!found) {
                try failures.append(arena, try std.fmt.allocPrint(
                    arena,
                    "{s} ({s}): declared function '{s}' missing from archive exports",
                    .{ pkg.pkg_rel, decl.header, decl.symbol },
                ));
            }
        }
    }

    if (failures.items.len > 0) {
        for (failures.items) |f| {
            std.debug.print("abi_export_check: {s}\n", .{f});
        }
        std.process.fatal("abi_export_check: {d} missing export(s)", .{failures.items.len});
    }

    try writeStamp(io, stamp_out, "all-ok\n");
}

fn buildAndCollectSymbols(
    alloc: std.mem.Allocator,
    io: std.Io,
    zig_exe: []const u8,
    nm_exe: ?[]const u8,
    item: *PackageWork,
) !void {
    const cwd = std.Io.Dir.cwd();
    try cwd.createDirPath(io, item.cache_dir);
    try cwd.createDirPath(io, item.prefix_dir);

    const res = try std.process.run(alloc, io, .{
        .argv = &.{
            zig_exe,
            "build",
            "--build-file",
            item.build_file,
            "--cache-dir",
            item.cache_dir,
            "-p",
            item.prefix_dir,
        },
        .stderr_limit = .limited(4 * 1024 * 1024),
        .stdout_limit = .limited(4 * 1024 * 1024),
    });
    if (res.term != .exited or res.term.exited != 0) {
        item.build_err = if (res.stderr.len > 0) res.stderr else "zig build exited non-zero";
        return error.ZigBuildFailed;
    }

    const lib_sub = try std.fmt.allocPrint(alloc, "{s}/lib", .{item.prefix_dir});
    var lib_dir = cwd.openDir(io, lib_sub, .{ .iterate = true }) catch |err| switch (err) {
        error.FileNotFound => return,
        else => return err,
    };
    defer lib_dir.close(io);

    var it = lib_dir.iterate();
    while (try it.next(io)) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ".a") and !std.mem.endsWith(u8, entry.name, ".lib")) continue;
        const archive_path = try std.fmt.allocPrint(alloc, "{s}/{s}", .{ lib_sub, entry.name });
        const syms = try extractArchiveSymbols(alloc, io, nm_exe, archive_path);
        var sit = syms.keyIterator();
        while (sit.next()) |k| {
            try item.symbols.put(alloc, k.*, {});
        }
    }
}

fn discoverPackages(
    arena: std.mem.Allocator,
    io: std.Io,
    cache_root: []const u8,
) ![]PackageWork {
    var rels: std.ArrayList([]const u8) = .empty;
    for ([_][]const u8{ "libs", "apps/shared_libs" }) |parent| {
        try collectBuildDirs(arena, io, parent, &rels);
    }
    std.mem.sort([]const u8, rels.items, {}, struct {
        fn lessThan(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.order(u8, a, b) == .lt;
        }
    }.lessThan);

    const cwd = std.Io.Dir.cwd();
    var items: std.ArrayList(PackageWork) = .empty;
    for (rels.items) |pkg_rel| {
        if (isPartialCLib(pkg_rel)) continue;

        const slug = try sanitizeSlug(arena, pkg_rel);
        const build_file = try std.fmt.allocPrint(arena, "{s}/build.zig", .{pkg_rel});
        const cache_dir = try std.fmt.allocPrint(arena, "{s}/abi-export/cache/{s}", .{ cache_root, slug });
        const prefix_dir = try std.fmt.allocPrint(arena, "{s}/abi-export/out/{s}", .{ cache_root, slug });

        var decls: std.ArrayList(HeaderDecl) = .empty;
        const inc_path = try std.fmt.allocPrint(arena, "{s}/inc", .{pkg_rel});
        if (cwd.openDir(io, inc_path, .{ .iterate = true })) |d_const| {
            var inc_dir = d_const;
            defer inc_dir.close(io);
            var hnames: std.ArrayList([]const u8) = .empty;
            var hit = inc_dir.iterate();
            while (try hit.next(io)) |entry| {
                if (entry.kind == .file and std.mem.endsWith(u8, entry.name, ".h")) {
                    try hnames.append(arena, try arena.dupe(u8, entry.name));
                }
            }
            std.mem.sort([]const u8, hnames.items, {}, struct {
                fn lessThan(_: void, a: []const u8, b: []const u8) bool {
                    return std.mem.order(u8, a, b) == .lt;
                }
            }.lessThan);
            for (hnames.items) |hname| {
                const h_bytes = try inc_dir.readFileAlloc(io, hname, arena, .limited(4 * 1024 * 1024));
                const fn_names = try extractHeaderDeclarations(arena, h_bytes);
                for (fn_names) |fn_name| {
                    try decls.append(arena, .{ .header = hname, .symbol = fn_name });
                }
            }
        } else |err| switch (err) {
            error.FileNotFound => {},
            else => return err,
        }

        const has_checked_decls = blk: {
            for (decls.items) |d| {
                if (!isExternalException(pkg_rel, d.header, d.symbol)) break :blk true;
            }
            break :blk false;
        };

        try items.append(arena, .{
            .pkg_rel = pkg_rel,
            .slug = slug,
            .build_file = build_file,
            .cache_dir = cache_dir,
            .prefix_dir = prefix_dir,
            .decls = try decls.toOwnedSlice(arena),
            .needs_build = has_checked_decls or isExtraProvider(pkg_rel),
        });
    }
    return items.toOwnedSlice(arena);
}

fn collectBuildDirs(
    arena: std.mem.Allocator,
    io: std.Io,
    parent: []const u8,
    out: *std.ArrayList([]const u8),
) !void {
    const cwd = std.Io.Dir.cwd();
    var dir = cwd.openDir(io, parent, .{ .iterate = true }) catch |err| switch (err) {
        error.FileNotFound => return,
        else => return err,
    };
    defer dir.close(io);

    var it = dir.iterate();
    while (try it.next(io)) |entry| {
        if (entry.kind != .directory) continue;
        const build_zig = try std.fmt.allocPrint(arena, "{s}/{s}/build.zig", .{ parent, entry.name });
        cwd.access(io, build_zig, .{}) catch continue;
        try out.append(arena, try std.fmt.allocPrint(arena, "{s}/{s}", .{ parent, entry.name }));
    }
}

fn sanitizeSlug(arena: std.mem.Allocator, rel: []const u8) ![]const u8 {
    const buf = try arena.dupe(u8, rel);
    for (buf) |*c| {
        if (c.* == '/' or c.* == '\\') c.* = '_';
    }
    return buf;
}

fn isPartialCLib(pkg: []const u8) bool {
    for (partial_c_libs) |p| {
        if (std.mem.eql(u8, p, pkg)) return true;
    }
    return false;
}

fn isExtraProvider(pkg: []const u8) bool {
    for (extra_archive_providers) |entry| {
        if (std.mem.eql(u8, entry.provider, pkg)) return true;
    }
    return false;
}

fn isExternalException(pkg: []const u8, header: []const u8, symbol: []const u8) bool {
    for (external_exceptions) |entry| {
        if (!std.mem.eql(u8, entry.pkg, pkg)) continue;
        if (!std.mem.eql(u8, entry.header, header)) continue;
        if (entry.symbol) |expected| {
            if (std.mem.eql(u8, expected, symbol)) return true;
        } else {
            return true;
        }
    }
    return false;
}

fn writeStamp(io: std.Io, path: []const u8, data: []const u8) !void {
    const cwd = std.Io.Dir.cwd();
    if (std.fs.path.dirname(path)) |parent| {
        try cwd.createDirPath(io, parent);
    }
    try cwd.writeFile(io, .{ .sub_path = path, .data = data });
}

pub fn findMissing(
    allocator: std.mem.Allocator,
    decls: []const []const u8,
    symbols: *const StringSet,
) ![]const []const u8 {
    var missing: std.ArrayList([]const u8) = .empty;
    for (decls) |decl| {
        if (!symbols.contains(decl)) {
            try missing.append(allocator, decl);
        }
    }
    return missing.toOwnedSlice(allocator);
}

pub fn extractArchiveSymbols(
    allocator: std.mem.Allocator,
    io: std.Io,
    nm_exe: ?[]const u8,
    archive_path: []const u8,
) !StringSet {
    if (nm_exe) |nm| {
        const res = try std.process.run(allocator, io, .{
            .argv = &.{ nm, "-g", "--defined-only", archive_path },
            .stderr_limit = .limited(1024 * 1024),
            .stdout_limit = .limited(16 * 1024 * 1024),
        });
        if (res.term != .exited or res.term.exited != 0) {
            return error.NmFailed;
        }
        return parseNmDefinedSymbols(allocator, res.stdout, builtin.os.tag == .macos);
    }

    const bytes = try std.Io.Dir.cwd().readFileAlloc(
        io,
        archive_path,
        allocator,
        .limited(128 * 1024 * 1024),
    );
    return parseArSymbolTable(allocator, bytes);
}

/// Parse `nm -g --defined-only` output into a set of exported symbol names.
pub fn parseNmDefinedSymbols(
    allocator: std.mem.Allocator,
    nm_stdout: []const u8,
    strip_macho_underscore: bool,
) !StringSet {
    var set: StringSet = .empty;
    var lines = std.mem.splitScalar(u8, nm_stdout, '\n');
    while (lines.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r");
        if (line.len == 0 or line[line.len - 1] == ':') continue;

        var fields: [4][]const u8 = undefined;
        var count: usize = 0;
        var tok = std.mem.tokenizeAny(u8, line, " \t");
        while (tok.next()) |t| {
            if (count < fields.len) {
                fields[count] = t;
                count += 1;
            }
        }
        if (count < 2) continue;
        const sym_type = fields[count - 2];
        const raw_name = fields[count - 1];
        if (sym_type.len != 1) continue;
        switch (sym_type[0]) {
            'T', 'D', 'R', 'B', 'W', 'V', 'C', 'S', 'G' => {},
            else => continue,
        }
        try set.put(allocator, try allocator.dupe(u8, raw_name), {});
        if (strip_macho_underscore and raw_name.len > 1 and raw_name[0] == '_') {
            try set.put(allocator, try allocator.dupe(u8, raw_name[1..]), {});
        }
    }
    return set;
}

/// Parse an `ar` archive global symbol table (`/` or `/SYM64/`).
pub fn parseArSymbolTable(allocator: std.mem.Allocator, data: []const u8) !StringSet {
    const magic = "!<arch>\n";
    if (data.len < magic.len or !std.mem.eql(u8, data[0..magic.len], magic)) {
        return error.InvalidArchiveMagic;
    }

    var pos: usize = magic.len;
    while (pos + 60 <= data.len) {
        if (pos % 2 == 1) {
            pos += 1;
            continue;
        }
        const hdr = data[pos .. pos + 60];
        if (hdr[58] != '`' or hdr[59] != '\n') return error.InvalidArchiveHeader;

        const name = hdr[0..16];
        const size_str = std.mem.trim(u8, hdr[48..58], " ");
        const size = try std.fmt.parseInt(usize, size_str, 10);
        const body_start = pos + 60;
        if (body_start + size > data.len) return error.TruncatedArchive;
        const body = data[body_start .. body_start + size];

        if (std.mem.startsWith(u8, name, "//")) {
            pos = body_start + size;
            continue;
        }
        if (std.mem.startsWith(u8, name, "/ ") or std.mem.startsWith(u8, name, "/\x00")) {
            return parseSysVSymtab(allocator, body, 4);
        }
        if (std.mem.startsWith(u8, name, "/SYM64/")) {
            return parseSysVSymtab(allocator, body, 8);
        }
        // Global symbol table is always the first member if present.
        break;
    }
    return .empty;
}

fn parseSysVSymtab(allocator: std.mem.Allocator, body: []const u8, entry_width: usize) !StringSet {
    var set: StringSet = .empty;
    if (body.len < entry_width) return set;
    const count: usize = if (entry_width == 4)
        std.mem.readInt(u32, body[0..4], .big)
    else
        @intCast(std.mem.readInt(u64, body[0..8], .big));
    const names_offset = entry_width + count * entry_width;
    if (names_offset > body.len) return error.InvalidSymbolTable;

    var cursor: usize = names_offset;
    for (0..count) |_| {
        if (cursor >= body.len) break;
        const rel_end = std.mem.indexOfScalarPos(u8, body, cursor, 0) orelse body.len;
        const sym = body[cursor..rel_end];
        if (sym.len > 0) {
            try set.put(allocator, try allocator.dupe(u8, sym), {});
        }
        cursor = rel_end + 1;
    }
    return set;
}

/// Extract non-`static`, non-`inline` C function declarations from a header.
pub fn extractHeaderDeclarations(
    allocator: std.mem.Allocator,
    source: []const u8,
) ![][]const u8 {
    var scratch_state = std.heap.ArenaAllocator.init(allocator);
    defer scratch_state.deinit();
    const scratch = scratch_state.allocator();

    const cleaned_source = try stripCommentsAndPreprocessor(scratch, source);
    var decls: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (decls.items) |d| allocator.free(d);
        decls.deinit(allocator);
    }
    var stmt: std.ArrayList(u8) = .empty;
    var extern_depths: std.ArrayList(usize) = .empty;
    var fn_body_depths: std.ArrayList(usize) = .empty;

    var depth: usize = 0;
    var i: usize = 0;
    var in_str = false;
    var in_chr = false;
    while (i < cleaned_source.len) : (i += 1) {
        const c = cleaned_source[i];
        if (in_str or in_chr) {
            try stmt.append(scratch, c);
            if (c == '\\' and i + 1 < cleaned_source.len) {
                i += 1;
                try stmt.append(scratch, cleaned_source[i]);
            } else if ((in_str and c == '"') or (in_chr and c == '\'')) {
                in_str = false;
                in_chr = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
            try stmt.append(scratch, c);
            continue;
        }
        if (c == '\'') {
            in_chr = true;
            try stmt.append(scratch, c);
            continue;
        }

        const at_file_scope = (depth == 0) or
            (extern_depths.items.len > 0 and depth == extern_depths.items[extern_depths.items.len - 1]);

        if (c == '{') {
            const trimmed = std.mem.trim(u8, stmt.items, " \t\r\n");
            if (at_file_scope and isExternCBlockPrefix(trimmed)) {
                depth += 1;
                try extern_depths.append(scratch, depth);
                stmt.clearRetainingCapacity();
                continue;
            }
            if (at_file_scope and trimmed.len > 0 and trimmed[trimmed.len - 1] == ')') {
                depth += 1;
                try fn_body_depths.append(scratch, depth);
                stmt.clearRetainingCapacity();
                continue;
            }
            depth += 1;
            try stmt.append(scratch, c);
            continue;
        }
        if (c == '}') {
            if (extern_depths.items.len > 0 and depth == extern_depths.items[extern_depths.items.len - 1]) {
                _ = extern_depths.pop();
                depth -= 1;
                stmt.clearRetainingCapacity();
                continue;
            }
            if (fn_body_depths.items.len > 0 and depth == fn_body_depths.items[fn_body_depths.items.len - 1]) {
                _ = fn_body_depths.pop();
                depth -= 1;
                stmt.clearRetainingCapacity();
                continue;
            }
            depth -|= 1;
            try stmt.append(scratch, c);
            continue;
        }
        if (c == ';' and at_file_scope) {
            if (try parseFunctionDeclName(scratch, stmt.items)) |name| {
                try decls.append(allocator, try allocator.dupe(u8, name));
            }
            stmt.clearRetainingCapacity();
            continue;
        }
        try stmt.append(scratch, c);
    }
    return decls.toOwnedSlice(allocator);
}

fn stripCommentsAndPreprocessor(allocator: std.mem.Allocator, source: []const u8) ![]u8 {
    var uncommented: std.ArrayList(u8) = .empty;
    var i: usize = 0;
    while (i < source.len) {
        if (i + 1 < source.len and source[i] == '/' and source[i + 1] == '*') {
            try uncommented.append(allocator, ' ');
            i += 2;
            while (i + 1 < source.len and !(source[i] == '*' and source[i + 1] == '/')) : (i += 1) {}
            i = @min(i + 2, source.len);
        } else if (i + 1 < source.len and source[i] == '/' and source[i + 1] == '/') {
            i += 2;
            while (i < source.len and source[i] != '\n') : (i += 1) {}
        } else if (source[i] == '"') {
            try uncommented.append(allocator, source[i]);
            i += 1;
            while (i < source.len) {
                try uncommented.append(allocator, source[i]);
                if (source[i] == '\\' and i + 1 < source.len) {
                    i += 1;
                    try uncommented.append(allocator, source[i]);
                    i += 1;
                    continue;
                }
                const ch = source[i];
                i += 1;
                if (ch == '"') break;
            }
        } else {
            try uncommented.append(allocator, source[i]);
            i += 1;
        }
    }

    var filtered: std.ArrayList(u8) = .empty;
    var lines = std.mem.splitScalar(u8, uncommented.items, '\n');
    var skip_cont = false;
    while (lines.next()) |raw_line| {
        const no_cr = std.mem.trimEnd(u8, raw_line, "\r");
        const trimmed_left = std.mem.trimStart(u8, no_cr, " \t");
        const trimmed_right = std.mem.trimEnd(u8, no_cr, " \t");
        const continues = trimmed_right.len > 0 and trimmed_right[trimmed_right.len - 1] == '\\';
        if (skip_cont) {
            skip_cont = continues;
            continue;
        }
        if (trimmed_left.len > 0 and trimmed_left[0] == '#') {
            skip_cont = continues;
            continue;
        }
        try filtered.appendSlice(allocator, no_cr);
        try filtered.append(allocator, '\n');
    }
    return filtered.toOwnedSlice(allocator);
}

fn isExternCBlockPrefix(trimmed: []const u8) bool {
    if (!std.mem.startsWith(u8, trimmed, "extern")) return false;
    if (trimmed.len <= "extern".len or !std.ascii.isWhitespace(trimmed["extern".len])) return false;
    const after = std.mem.trimStart(u8, trimmed["extern".len..], " \t\r\n");
    return std.mem.eql(u8, after, "\"C\"");
}

fn parseFunctionDeclName(allocator: std.mem.Allocator, raw_stmt: []const u8) !?[]const u8 {
    const trimmed = std.mem.trim(u8, raw_stmt, " \t\r\n");
    if (trimmed.len == 0 or std.mem.indexOfScalar(u8, trimmed, '{') != null) return null;

    var cleaned_buf: std.ArrayList(u8) = .empty;
    var i: usize = 0;
    while (i < trimmed.len) {
        if (i + 1 < trimmed.len and trimmed[i] == '[' and trimmed[i + 1] == '[') {
            i += 2;
            var bracket_depth: usize = 2;
            while (i < trimmed.len and bracket_depth > 0) : (i += 1) {
                if (trimmed[i] == '[') bracket_depth += 1 else if (trimmed[i] == ']') bracket_depth -= 1;
            }
            try cleaned_buf.append(allocator, ' ');
            continue;
        }
        try cleaned_buf.append(allocator, trimmed[i]);
        i += 1;
    }

    var work = std.mem.trim(u8, cleaned_buf.items, " \t\r\n");
    while (stripTrailingAttribute(work)) |shorter| {
        work = std.mem.trim(u8, shorter, " \t\r\n");
    }
    if (work.len == 0 or work[work.len - 1] != ')') return null;

    const open_idx = findMatchingOpenParen(work) orelse return null;
    const before = std.mem.trim(u8, work[0..open_idx], " \t\r\n");
    if (before.len == 0 or before[before.len - 1] == ')') return null;

    const skip_keywords = [_][]const u8{
        "typedef", "static", "inline", "__inline", "__inline__", "static_assert", "_Static_assert",
    };
    for (skip_keywords) |kw| {
        if (containsWord(before, kw)) return null;
    }

    var end = before.len;
    while (end > 0 and !isIdentCont(before[end - 1])) : (end -= 1) {}
    if (end != before.len) return null; // declaration ended in '*', attribute, etc.
    var start = end;
    while (start > 0 and isIdentCont(before[start - 1])) : (start -= 1) {}
    if (start == end or !isIdentStart(before[start])) return null;

    const ident = before[start..end];
    if (std.mem.eql(u8, ident, "static_assert") or
        std.mem.eql(u8, ident, "_Static_assert") or
        std.mem.eql(u8, ident, "__attribute__"))
    {
        return null;
    }
    return try allocator.dupe(u8, ident);
}

fn stripTrailingAttribute(s: []const u8) ?[]const u8 {
    if (s.len == 0 or s[s.len - 1] != ')') return null;
    const open = findMatchingOpenParen(s) orelse return null;
    const prefix = std.mem.trimEnd(u8, s[0..open], " \t\r\n");
    if (std.mem.endsWith(u8, prefix, "__attribute__")) {
        return std.mem.trimEnd(u8, prefix[0 .. prefix.len - "__attribute__".len], " \t\r\n");
    }
    return null;
}

fn findMatchingOpenParen(s: []const u8) ?usize {
    if (s.len == 0 or s[s.len - 1] != ')') return null;
    var depth: usize = 0;
    var idx = s.len;
    while (idx > 0) {
        idx -= 1;
        if (s[idx] == ')') {
            depth += 1;
        } else if (s[idx] == '(') {
            depth -= 1;
            if (depth == 0) return idx;
        }
    }
    return null;
}

fn containsWord(haystack: []const u8, word: []const u8) bool {
    var start: usize = 0;
    while (std.mem.indexOfPos(u8, haystack, start, word)) |idx| {
        const left_ok = idx == 0 or !isIdentCont(haystack[idx - 1]);
        const right_idx = idx + word.len;
        const right_ok = right_idx == haystack.len or !isIdentCont(haystack[right_idx]);
        if (left_ok and right_ok) return true;
        start = idx + 1;
    }
    return false;
}

fn isIdentStart(c: u8) bool {
    return (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or c == '_';
}

fn isIdentCont(c: u8) bool {
    return isIdentStart(c) or (c >= '0' and c <= '9');
}
