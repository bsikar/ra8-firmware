//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_ns_linker_script()`: the Non-Secure image's linker script, CONFIGURED
//! from the board template rather than named by the app.
//!
//! Until #759 item 2 this was two hand-maintained forks -- a shared SRAM-run
//! script and ereader's XIP variant -- that shared 78 of ~130 lines and drifted
//! on symbol names. They are one template now with one substitution axis: where
//! `.text` lives. The app says which layout it wants and gets the current
//! section list either way.
//!
//! So the NS script is a GENERATED file, and the graph cannot name it as a path
//! under the app directory: `cmake/ra8_add_ns_image.cmake:97` writes it into the
//! app's binary directory, which does not exist in this build. The template and
//! the addresses are both READ here, the same way ld_fragments.zig reads the
//! CPU1 and NS-inline maps, so the board layer stays the one definition of both.
//!
//! A separate file because CMake factored the same function out for the same
//! reason: `secure_boot_ns_hil` links its NS image by hand (no CMSE veneers to
//! hand over, so `ra8_add_ns_image()` does not fit) and still has to get this
//! script rather than a third fork.

const std = @import("std");

const cmake_vars = @import("cmake_vars.zig");

/// The board directory the template and the memory map live in.
///
/// `ra8_add_ns_image.cmake:64` spells the board out rather than deriving it
/// from the app's `BOARD`, so this spells it out too. A dual-image TrustZone
/// product is an EK-RA8D2 shape today, and a second board would have to add
/// its own `ns_image.ld.in` before the helper could serve it at all.
pub const board_dir = "libs/ra8_board_ek_ra8d2/ld";
pub const template_name = "ns_image.ld.in";
pub const memory_map_name = "ns_memory_map.cmake";

/// Where the read-only half of the image lives. The ONE axis the two layouts
/// differ on, which is why it is an enum of two rather than a pair of scripts.
pub const Layout = enum {
    /// Load home in Secure MRAM, run home in SRAM2: the Secure boot copies the
    /// window MRAM->SRAM2 and BLXNS-es. What `ra8_add_ns_image()` does without
    /// `XIP`.
    sram_run,
    /// `XIP`: the M85 fetches NS instructions straight from OSPI flash, VMA ==
    /// LMA, and only `.data` is copied. `apps/board/stand_alone/ereader` is the
    /// one app that asks for this; it is not in the cross table yet.
    xip,
};

/// The generated file's name, as `ra8_add_ns_image.cmake:97` names it.
pub fn fileName(b: *std.Build, image_name: []const u8) []const u8 {
    return b.fmt("{s}_ns_image.ld", .{image_name});
}

/// The two prose substitutions, transcribed from `ra8_add_ns_image.cmake:71-94`
/// rather than read.
///
/// The addresses are read because a wrong one links a wrong image silently.
/// These two land inside comments, so they cannot do that; what they can do is
/// go stale, which a reader notices. Parsing them out of the listfile is not an
/// option anyway: they are multi-line `set()` calls inside both arms of an
/// `if()`, and the one-line reader in cmake_vars.zig would take the last arm's
/// value for both.
const sram_run_rom_comment = "Load home in Secure MRAM (flasher writes physical MRAM here).";
const xip_rom_comment = "Execute-in-place home: OSPI flash Non-secure alias (bit[28] = 1).";
const sram_run_mode_doc = "This target runs from SRAM: the Secure boot copies the whole NS image MRAM->SRAM2 (Non-secure alias) after SRAMSABAR2 marks SRAM2 Non-secure, then BLXNS-es to the reset vector.";
const xip_mode_doc = "This target runs EXECUTE-IN-PLACE: .ns_vectors/.text/.rodata/.ARM.exidx live in OSPI at VMA == LMA, and ns_reset_handler copies only .data into SRAM. The Secure boot arms XIP and BLXNS-es; it does not copy the image.";

const Substitution = struct {
    placeholder: []const u8,
    value: []const u8,
};

/// Every `@RA8_NS_*@` the template carries, with the value this layout gives
/// it. The writable run home is the same in both layouts, which is the point:
/// `.data` always has its VMA in SRAM2 and only its LMA moves.
fn substitutions(
    b: *std.Build,
    layout: Layout,
    vars: cmake_vars.Vars,
    source: []const u8,
) []const Substitution {
    var out = std.ArrayList(Substitution).init(b.allocator);
    out.appendSlice(&.{
        .{ .placeholder = "@RA8_NS_SRAM_ORIGIN@", .value = vars.get("RA8_NS_SRAM_ORIGIN", source) },
        .{ .placeholder = "@RA8_NS_SRAM_LENGTH@", .value = vars.get("RA8_NS_SRAM_LENGTH", source) },
    }) catch @panic("OOM");

    switch (layout) {
        .sram_run => out.appendSlice(&.{
            .{ .placeholder = "@RA8_NS_ROM_NAME@", .value = "NS_LOAD" },
            .{ .placeholder = "@RA8_NS_ROM_ORIGIN@", .value = vars.get("RA8_NS_MRAM_ORIGIN", source) },
            .{ .placeholder = "@RA8_NS_ROM_LENGTH@", .value = vars.get("RA8_NS_MRAM_LENGTH", source) },
            .{ .placeholder = "@RA8_NS_ROM_COMMENT@", .value = sram_run_rom_comment },
            // The vectors live at their RUN address: the Secure boot has already
            // copied them to SRAM2 by the time VTOR_NS is pointed here.
            .{ .placeholder = "@RA8_NS_VECTOR_REGION@", .value = "NS_SRAM_RUN" },
            .{ .placeholder = "@RA8_NS_TEXT_PLACE@", .value = "> NS_SRAM_RUN AT > NS_LOAD" },
            .{ .placeholder = "@RA8_NS_DATA_PLACE@", .value = "> NS_SRAM_RUN AT > NS_LOAD" },
            .{ .placeholder = "@RA8_NS_MODE_DOC@", .value = sram_run_mode_doc },
        }) catch @panic("OOM"),
        .xip => out.appendSlice(&.{
            .{ .placeholder = "@RA8_NS_ROM_NAME@", .value = "NS_XIP" },
            .{ .placeholder = "@RA8_NS_ROM_ORIGIN@", .value = vars.get("RA8_NS_OSPI_ORIGIN", source) },
            .{ .placeholder = "@RA8_NS_ROM_LENGTH@", .value = vars.get("RA8_NS_OSPI_LENGTH", source) },
            .{ .placeholder = "@RA8_NS_ROM_COMMENT@", .value = xip_rom_comment },
            // VMA == LMA, so the vectors are fetched from flash where they lie.
            .{ .placeholder = "@RA8_NS_VECTOR_REGION@", .value = "NS_XIP" },
            .{ .placeholder = "@RA8_NS_TEXT_PLACE@", .value = "> NS_XIP" },
            .{ .placeholder = "@RA8_NS_DATA_PLACE@", .value = "> NS_SRAM_RUN AT > NS_XIP" },
            .{ .placeholder = "@RA8_NS_MODE_DOC@", .value = xip_mode_doc },
        }) catch @panic("OOM"),
    }
    return out.items;
}

fn read(b: *std.Build, file: []const u8) []const u8 {
    return b.build_root.handle.readFileAlloc(b.allocator, file, 1 << 20) catch |err|
        std.debug.panic("ns_linker_script: cannot read {s}: {s}", .{ file, @errorName(err) });
}

/// The configured script: the board template with this layout's values in it,
/// which is what `configure_file(... @ONLY)` produces.
pub fn configure(b: *std.Build, layout: Layout) []const u8 {
    const template_path = b.fmt("{s}/{s}", .{ board_dir, template_name });
    const map_path = b.fmt("{s}/{s}", .{ board_dir, memory_map_name });

    const vars = cmake_vars.parse(b.allocator, read(b, map_path));
    var text = read(b, template_path);
    for (substitutions(b, layout, vars, map_path)) |sub| {
        text = std.mem.replaceOwned(u8, b.allocator, text, sub.placeholder, sub.value) catch
            @panic("OOM");
    }

    // Fail closed on a placeholder this file does not know. `@ONLY` leaves an
    // unknown `@NAME@` in the output, and ld reads `@` as an ordinary character
    // in most positions, so a template that grows an axis would otherwise link
    // a script with a literal `@RA8_NS_...@` in it and only misbehave later.
    if (std.mem.indexOf(u8, text, "@RA8_NS_")) |at| {
        const rest = text[at..];
        const end = (std.mem.indexOfScalarPos(u8, rest, 1, '@') orelse rest.len - 1) + 1;
        std.debug.panic(
            "{s} carries {s}, which ns_linker_script.zig does not substitute",
            .{ template_path, rest[0..end] },
        );
    }
    return text;
}

/// The configured script as a file the link can take with `-T`.
pub fn path(b: *std.Build, image_name: []const u8, layout: Layout) std.Build.LazyPath {
    const files = b.addWriteFiles();
    return files.add(fileName(b, image_name), configure(b, layout));
}
