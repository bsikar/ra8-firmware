//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure decision core of the FTL mount lifecycle: the config rules, the split
//! of a device into an FTL span and a reserved checkpoint tail, the blank-tail
//! scan, and the self-consistency check a mounted handle has to still pass.
//!
//! Everything here is arithmetic over values the membrane has already read.
//! `../ra8_ftl_abi.zig` owns pointer nullability and every call down into
//! `ra8_io_blockdev_*`.

const std = @import("std");
pub const core = @import("root.zig");

/// `ra8_ftl_mount_const_t` and `ra8_ftl_mount_state_t`.
pub const mount = struct {
    /// Smallest reserved checkpoint tail, in blocks.
    pub const reserved_tail_min: u32 = 1;
    /// Reserved tail was blank, so the tables cold-started.
    pub const state_cold: State = 0;
    /// A checkpoint was found and loaded, so the mapping resumed.
    pub const state_resumed: State = 1;
    /// `ra8_ftl_mount_state_t` is `enum : uint8_t`, so a caller's state is one
    /// byte; a wider store through its pointer overwrites whatever follows it.
    pub const State = u8;
};

/// The fields of a `ra8_ftl_cfg_t` the rules judge, lifted out of the pointer
/// so the decision is testable without one.
pub const CfgView = struct {
    has_raw: bool,
    has_map: bool,
    has_pblocks: bool,
    has_scratch: bool,
    has_checkpoint: bool,
    checkpoint_bytes: u32,
    logical_blocks: u32,
    reserved_tail_blocks: u32,
};

/// The fields of a mounted `ra8_ftl_t` the sync guard judges.
pub const HandleView = struct {
    has_raw: bool,
    has_checkpoint: bool,
    ck_bytes: u32,
    physical_blocks: u32,
    reserved_lba: u32,
    reserved_blocks: u32,
};

/// How a device splits into an FTL span and a reserved tail.
pub const Split = struct {
    /// Physical blocks handed to the FTL, below the tail.
    physical_blocks: u32,
    /// Reserved tail size in bytes.
    tail_bytes: u32,
};

/// `internal_validate_cfg`: single-condition checks only. Geometry against the
/// real device is judged separately, once its capabilities are known.
pub fn validateCfg(cfg: CfgView) core.Err {
    if (!cfg.has_raw) return core.err_null_ptr;
    if (!cfg.has_map) return core.err_null_ptr;
    if (!cfg.has_pblocks) return core.err_null_ptr;
    if (!cfg.has_scratch) return core.err_null_ptr;
    if (!cfg.has_checkpoint) return core.err_null_ptr;
    if (cfg.logical_blocks == 0) return core.err_invalid_size;
    if (cfg.reserved_tail_blocks < mount.reserved_tail_min) return core.err_invalid_size;
    if (cfg.checkpoint_bytes == 0) return core.err_invalid_size;
    return core.ok;
}

/// Bytes a reserved tail of `blocks` occupies. Widened to 64 bits so a tail no
/// `uint32_t` byte count could describe is rejected below rather than wrapping
/// into one that looks satisfiable.
pub fn tailBytes(blocks: u32, logical_block_bytes: u16) u64 {
    return @as(u64, blocks) * @as(u64, logical_block_bytes);
}

/// `internal_split`: the tail sits at the top of the medium, so the FTL span is
/// everything below it and the first reserved block index equals that span.
pub fn splitFor(caps: core.Caps, cfg: CfgView) core.Outcome(Split) {
    if (caps.block_count <= cfg.reserved_tail_blocks) {
        return .{ .fault = core.err_invalid_arg };
    }
    const phys = caps.block_count - cfg.reserved_tail_blocks;
    if (phys < cfg.logical_blocks + core.min_spare) {
        return .{ .fault = core.err_invalid_arg };
    }
    const tail = tailBytes(cfg.reserved_tail_blocks, caps.logical_block_bytes);
    if (@as(u64, cfg.checkpoint_bytes) < tail) {
        return .{ .fault = core.err_invalid_size };
    }
    return .{ .value = .{ .physical_blocks = phys, .tail_bytes = @intCast(tail) } };
}

/// `internal_all_erased`: a reserved tail that reads back wholly blank has
/// never been programmed, the one condition under which a mount may cold-start
/// without discarding anything.
pub fn allErased(buf: []const u8, erase: u8) bool {
    for (buf) |byte| {
        if (byte != erase) return false;
    }
    return true;
}

/// The half of `internal_mounted` that judges the handle alone: was it ever
/// mounted, and does its recorded tail still start where the FTL span ends.
pub fn mountedShape(handle: HandleView) core.Err {
    if (!handle.has_raw) return core.err_not_initialized;
    if (!handle.has_checkpoint) return core.err_invalid_state;
    if (handle.reserved_blocks == 0) return core.err_invalid_state;
    if (handle.reserved_lba != handle.physical_blocks) return core.err_invalid_state;
    return core.ok;
}

/// The half of `internal_mounted` that needs the device: the recorded tail must
/// still lie inside it, and the staging buffer must still cover a whole tail.
pub fn mountedAgainstDevice(handle: HandleView, caps: core.Caps) core.Outcome(u32) {
    if (@as(u64, handle.reserved_lba) + @as(u64, handle.reserved_blocks) > caps.block_count) {
        return .{ .fault = core.err_invalid_state };
    }
    const tail = tailBytes(handle.reserved_blocks, caps.logical_block_bytes);
    if (@as(u64, handle.ck_bytes) < tail) {
        return .{ .fault = core.err_invalid_size };
    }
    return .{ .value = @intCast(tail) };
}

/// The gate both `internal_resolve` and `ra8_ftl_sync` apply to the checkpoint
/// length the FTL reports: it has to fit the tail it is going to live in.
pub fn checkpointFits(need: u32, tail_bytes: u32) core.Err {
    return if (need > tail_bytes) core.err_invalid_size else core.ok;
}
