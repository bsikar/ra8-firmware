//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ThreadX module preamble, read from the head of a module binary: the
//! words the `.ra8app` packer needs to describe the module. Upstream's
//! `txm_module_preamble.S` lays it out; each entry word is the distance from
//! the word itself to the function, with the Thumb bit set.
//!
//! The binary is the objcopy output of the module ELF, so the preamble's code
//! size is the whole file. Its data size is RAM the Module Manager allocates
//! and `gcc_setup` fills, not bytes in the file.

const std = @import("std");

/// Upstream's preamble ID, ASCII "MODU".
pub const id: u32 = 0x4D4F4455;

/// Word indexes into the preamble, as `txm_module_preamble.S` orders them.
pub const Word = struct {
    pub const id: usize = 0;
    pub const shell_entry: usize = 6;
    pub const start_stack_size: usize = 10;
    pub const code_size: usize = 14;
    pub const data_size: usize = 15;
    /// Words this reader needs before it can trust any of them.
    pub const read_count: usize = 16;
};

/// Why a binary was not taken as a module.
pub const Error = error{
    /// The image is shorter than the preamble words this reader uses.
    ShortImage,
    /// The first word is not the preamble ID.
    NotAModule,
    /// The preamble's code size is not the length of the image.
    SizeMismatch,
    /// The shell entry lies outside the module's code.
    BadEntry,
};

/// What the preamble says about a module.
pub const Module = struct {
    /// Byte offset of the thread shell entry from the start of the image.
    entry_offset: u32,
    /// Stack the start thread asks for, bytes.
    stack_size: u32,
    /// Module code bytes: the whole image.
    code_size: u32,
    /// RAM the module's data, GOT, BSS and heap need once loaded.
    ram_data_size: u32,
};

fn word(bytes: []const u8, index: usize) u32 {
    return std.mem.readInt(u32, bytes[index * 4 ..][0..4], .little);
}

/// Read and check the preamble at the head of `bytes`.
pub fn read(bytes: []const u8) Error!Module {
    if (bytes.len < Word.read_count * 4) return Error.ShortImage;
    if (word(bytes, Word.id) != id) return Error.NotAModule;

    const code_size = word(bytes, Word.code_size);
    if (code_size != bytes.len) return Error.SizeMismatch;

    const distance = word(bytes, Word.shell_entry) & ~@as(u32, 1);
    const entry = std.math.add(u32, Word.shell_entry * 4, distance) catch return Error.BadEntry;
    if (entry >= code_size) return Error.BadEntry;

    return .{
        .entry_offset = entry,
        .stack_size = word(bytes, Word.start_stack_size),
        .code_size = code_size,
        .ram_data_size = word(bytes, Word.data_size),
    };
}
