//! Whether a module may be granted [start, start + length) as shared memory
//! on the M85 (RA8FW-527). The board keeps its shared SRAM, slot 4 of its MPU
//! map, live as a privileged-only, non-cacheable region while a module thread
//! runs (RA8FW-484). A grant over it would overlap that region, and PMSAv8
//! turns an overlap into a MemManage at the module's first access, so the
//! grant is refused up front instead.
//!
//! The bounds are the board's (libs/ra8_board_ek_ra8d2/inc/
//! ra8_board_ek_ra8d2_dualcore.h): k_ra8_board_shared_ram_base up to the end
//! of CPU1's private bank. The module archive's include path does not reach
//! that header, so they are restated here and a host test reads the header
//! to catch drift.

pub const shared_base: u32 = 0x2210_0000;
pub const cpu1_sram_base: u32 = 0x2219_0000;
pub const cpu1_sram_size: u32 = 0x1_0000;
pub const shared_end: u32 = cpu1_sram_base + cpu1_sram_size;

pub const Verdict = enum {
    allowed,
    /// Nothing to grant, and upstream's `address + length - 1` would wrap.
    empty,
    /// The grant runs past the top of the address space.
    wraps,
    /// The grant shares at least one byte with the board's shared SRAM.
    overlaps_shared,
};

pub fn check(start: u32, length: u32) Verdict {
    if (length == 0) return .empty;
    const last = @as(u64, start) + length - 1;
    if (last > 0xFFFF_FFFF) return .wraps;
    if (start < shared_end and last >= shared_base) return .overlaps_shared;
    return .allowed;
}
