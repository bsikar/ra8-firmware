//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure CEU capture-source policy, with no externs and no ABI surface: the
//! configuration bounds and format pairing the backend accepts, which CEU
//! events abandon a pending frame, the captured-byte-count rule, and the two
//! capture-entry guards. The register access that acts on these answers lives
//! in `src/source_ceu.zig`.

const std = @import("std");

/// `ra8_ceu_capture_format_t` values, mirrored from `inc/ra8_ceu_types.h`.
/// Raw bytes rather than a Zig enum: the format arrives inside a caller-built
/// descriptor, so an out-of-range byte must stay rejectable rather than become
/// undefined, the same rule the rest of this library follows for enum-by-value.
pub const capture_format = struct {
    pub const image_capture: u8 = 0;
    pub const data_synchronous: u8 = 1;
    pub const data_enable: u8 = 2;
};

/// CETCR event bits this backend classifies, mirrored from
/// `inc/ra8_ceu_regs.h`. Only the bits the policy names are listed; every
/// other bit rides through untouched in the retained diagnostic snapshot.
pub const events = struct {
    pub const cpe: u32 = 1 << 0;
    pub const igrw: u32 = 1 << 4;
    pub const hd: u32 = 1 << 8;
    pub const vd: u32 = 1 << 9;
    pub const cram_overflow: u32 = 1 << 16;
    pub const vd_error: u32 = 1 << 20;
    pub const firewall: u32 = 1 << 23;
    pub const mask_all: u32 = 0x03F7F713;

    /// Faults that make a pending capture unusable immediately. Sync-timing
    /// events are deliberately absent: HD and VD can precede a valid CPE, so
    /// they must not terminate the bounded poll.
    pub const fatal: u32 = igrw | cram_overflow | vd_error | firewall;
};

/// Eight-byte CEU buffer alignment. The peripheral DMA-writes in 64-bit beats,
/// so a capture address with any low bit set is rejected before the engine is
/// armed.
pub const buffer_alignment_mask: usize = 7;

/// Whether an observed event snapshot abandons the pending frame.
pub fn isFatal(observed: u32) bool {
    return (observed & events.fatal) != 0;
}

/// Whether an observed event snapshot reports one completed frame.
pub fn isComplete(observed: u32) bool {
    return (observed & events.cpe) != 0;
}

/// The configuration fields the policy judges, lifted out of the C descriptor
/// so this file needs none of its register layout.
pub const CfgView = struct {
    frame_bytes_max: u32,
    stride_bytes: u32,
    width: u16,
    height: u16,
    output_format: u8,
    poll_interval_ms: u32,
    poll_attempts: u32,
    ceu_capture_format: u8,
    image_area_size: u32,
};

/// Every way a CEU source configuration can be rejected, in the order the C
/// tested them. They share one `ra8_err_t`, but stay separate here so each is
/// independently reachable in a test.
pub const CfgFault = enum {
    ok,
    zero_frame_bytes,
    zero_width,
    zero_height,
    zero_poll_interval,
    zero_poll_attempts,
    format_pairing,
    jpeg_stride_set,
    jpeg_area_mismatch,
};

/// Validate capture bounds and the format pairing.
///
/// The pairing rule is one decision with two conditions: a data-enable capture
/// and a JPEG output format must agree, because only data-enable framing can
/// produce a variable-size compressed frame and only a JPEG consumer can read
/// one.
pub fn validateCfg(cfg: CfgView, jpeg_format: u8) CfgFault {
    if (cfg.frame_bytes_max == 0) return .zero_frame_bytes;
    if (cfg.width == 0) return .zero_width;
    if (cfg.height == 0) return .zero_height;
    if (cfg.poll_interval_ms == 0) return .zero_poll_interval;
    if (cfg.poll_attempts == 0) return .zero_poll_attempts;
    const data_enable = cfg.ceu_capture_format == capture_format.data_enable;
    if (data_enable != (cfg.output_format == jpeg_format)) return .format_pairing;
    if (cfg.output_format == jpeg_format) {
        if (cfg.stride_bytes != 0) return .jpeg_stride_set;
        if (cfg.image_area_size != cfg.frame_bytes_max) return .jpeg_area_mismatch;
    }
    return .ok;
}

/// Captured byte count for one completed frame.
///
/// A fixed-frame capture always reports the configured bound. A data-enable
/// capture reports CDSSR when the peripheral latched a nonzero value, and falls
/// back to the configured bound otherwise, so a format-aware caller can locate
/// its own end marker within that capacity.
pub fn frameBytes(frame_bytes_max: u32, ceu_capture_format: u8, data_size: u32) u32 {
    if (ceu_capture_format != capture_format.data_enable) return frame_bytes_max;
    if (data_size == 0) return frame_bytes_max;
    return data_size;
}

/// Capture-entry rejections, judged before any cache or register work.
pub const EntryFault = enum { ok, capacity_short, misaligned };

/// The two guards a capture applies to caller-owned storage.
pub fn captureEntryFault(capacity: u32, frame_bytes_max: u32, address: usize) EntryFault {
    if (capacity < frame_bytes_max) return .capacity_short;
    if ((address & buffer_alignment_mask) != 0) return .misaligned;
    return .ok;
}

/// Byte counts a completed capture cannot publish.
pub const CapturedFault = enum { ok, zero, exceeds_capacity };

/// A capture that produced nothing is not a frame, and one larger than the
/// supplied storage cannot be published as a view of it.
pub fn capturedBytesFault(captured: u32, capacity: u32) CapturedFault {
    if (captured == 0) return .zero;
    if (captured > capacity) return .exceeds_capacity;
    return .ok;
}

comptime {
    // The fatal set must never swallow completion or sync traffic: CPE has to
    // stay separately observable, and HD/VD must not end a bounded poll.
    std.debug.assert((events.fatal & events.cpe) == 0);
    std.debug.assert((events.fatal & (events.hd | events.vd)) == 0);
    std.debug.assert((events.fatal & events.mask_all) == events.fatal);
}
