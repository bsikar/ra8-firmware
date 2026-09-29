//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure PCM policy for `ra8_audio`: container widths, frame validation,
//! frame-versus-source-metadata agreement, buffer usability, the bounded
//! FIFO fill loop, and the PDM configuration gate. Nothing here touches
//! hardware, globals, or extern symbols, so it is exercised host-side with
//! no fixture at all.

const std = @import("std");

/// PCM container layouts (`ra8_audio_format_t`). Carried as a raw byte
/// because callers hand the value in by value and an out-of-range byte has
/// to stay rejectable.
pub const format_pcm_s16le: u8 = 0;
pub const format_pcm_s32le: u8 = 1;

/// `ra8_audio_frame_t`: immutable view of one interleaved PCM frame.
pub const Frame = extern struct {
    data: ?*const anyopaque = null,
    bytes: u32 = 0,
    sample_count: u32 = 0,
    sample_rate_hz: u32 = 0,
    timestamp_ms: u32 = 0,
    channels: u8 = 0,
    valid_bits: u8 = 0,
    format: u8 = 0,
};

/// `ra8_audio_source_info_t`: a source's fixed output contract.
pub const Info = extern struct {
    frame_bytes: u32 = 0,
    samples_per_frame: u32 = 0,
    sample_rate_hz: u32 = 0,
    channels: u8 = 0,
    valid_bits: u8 = 0,
    format: u8 = 0,
};

/// `ra8_audio_buffer_t`: caller-owned writable byte span.
pub const Buffer = extern struct {
    data: ?*anyopaque = null,
    capacity: u32 = 0,
};

const ptr_bytes = @sizeOf(usize);

comptime {
    // The three caller-owned layouts are the C ABI. Offsets are asserted in
    // pointer-width multiples so they hold on the 64-bit host suite and on
    // 32-bit Arm alike.
    std.debug.assert(@offsetOf(Frame, "data") == 0);
    std.debug.assert(@offsetOf(Frame, "bytes") == ptr_bytes);
    std.debug.assert(@offsetOf(Frame, "sample_count") == ptr_bytes + 4);
    std.debug.assert(@offsetOf(Frame, "sample_rate_hz") == ptr_bytes + 8);
    std.debug.assert(@offsetOf(Frame, "timestamp_ms") == ptr_bytes + 12);
    std.debug.assert(@offsetOf(Frame, "channels") == ptr_bytes + 16);
    std.debug.assert(@offsetOf(Frame, "valid_bits") == ptr_bytes + 17);
    std.debug.assert(@offsetOf(Frame, "format") == ptr_bytes + 18);
    std.debug.assert(@sizeOf(Frame) == std.mem.alignForward(usize, ptr_bytes + 19, ptr_bytes));
    std.debug.assert(@alignOf(Frame) == ptr_bytes);

    std.debug.assert(@sizeOf(Info) == 16);
    std.debug.assert(@alignOf(Info) == 4);
    std.debug.assert(@offsetOf(Info, "frame_bytes") == 0);
    std.debug.assert(@offsetOf(Info, "samples_per_frame") == 4);
    std.debug.assert(@offsetOf(Info, "sample_rate_hz") == 8);
    std.debug.assert(@offsetOf(Info, "channels") == 12);
    std.debug.assert(@offsetOf(Info, "valid_bits") == 13);
    std.debug.assert(@offsetOf(Info, "format") == 14);

    std.debug.assert(@offsetOf(Buffer, "data") == 0);
    std.debug.assert(@offsetOf(Buffer, "capacity") == ptr_bytes);
    std.debug.assert(@sizeOf(Buffer) == ptr_bytes * 2);
}

/// Storage width of one sample in bytes, 0 for an unsupported format byte.
pub fn containerBytes(format: u8) u32 {
    return switch (format) {
        format_pcm_s16le => 2,
        format_pcm_s32le => 4,
        else => 0,
    };
}

/// Why one frame view failed validation, in the C's guard order.
pub const FrameFault = enum {
    ok,
    null_data,
    bad_format,
    zero_sample_count,
    zero_sample_rate,
    zero_channels,
    zero_valid_bits,
    valid_bits_too_wide,
    size_overflow,
    bytes_mismatch,
};

/// `ra8_audio_frame_validate` minus the outer NULL check, which the ABI
/// membrane owns because it is a pointer question, not a policy one.
pub fn validateFrame(frame: *const Frame) FrameFault {
    if (frame.data == null) return .null_data;
    const container = containerBytes(frame.format);
    if (container == 0) return .bad_format;
    if (frame.sample_count == 0) return .zero_sample_count;
    if (frame.sample_rate_hz == 0) return .zero_sample_rate;
    if (frame.channels == 0) return .zero_channels;
    if (frame.valid_bits == 0) return .zero_valid_bits;
    if (@as(u32, frame.valid_bits) > container * 8) return .valid_bits_too_wide;
    const required = @as(u64, frame.sample_count) * @as(u64, frame.channels) * @as(u64, container);
    if (required > std.math.maxInt(u32)) return .size_overflow;
    if (frame.bytes != @as(u32, @intCast(required))) return .bytes_mismatch;
    return .ok;
}

/// Which independent invariant a captured frame broke against its source's
/// advertised geometry. Every one of these is `invalid_state` at the ABI.
pub const MatchFault = enum {
    ok,
    bytes,
    sample_count,
    sample_rate,
    channels,
    valid_bits,
    format,
};

/// `internal_audio_frame_matches_info`, field for field and in order.
pub fn frameMatchesInfo(frame: *const Frame, info: *const Info) MatchFault {
    if (frame.bytes != info.frame_bytes) return .bytes;
    if (frame.sample_count != info.samples_per_frame) return .sample_count;
    if (frame.sample_rate_hz != info.sample_rate_hz) return .sample_rate;
    if (frame.channels != info.channels) return .channels;
    if (frame.valid_bits != info.valid_bits) return .valid_bits;
    if (frame.format != info.format) return .format;
    return .ok;
}

/// Why a caller-owned capture buffer is unusable. Both faults answer
/// `null_ptr` at the ABI, which is the C's contract: a zero-capacity buffer
/// is treated as an absent buffer, not as a size error.
pub const BufferFault = enum { ok, null_data, zero_capacity };

pub fn bufferFault(buffer: *const Buffer) BufferFault {
    if (buffer.data == null) return .null_data;
    if (buffer.capacity == 0) return .zero_capacity;
    return .ok;
}

/// Whether a caller buffer can hold one whole source frame.
pub fn capacityFits(capacity: u32, frame_bytes: u32) bool {
    return capacity >= frame_bytes;
}

// ---------------------------------------------------------------------------
// PDM backend policy
// ---------------------------------------------------------------------------

/// `k_ra8_pdm_ch_count`: the channel selector is a bound, not an enum here,
/// because it arrives as a byte inside a caller-built configuration.
pub const pdm_channel_count: u8 = 3;

/// `k_pdm_source_alignment_mask`: PCM-S32LE buffers are 4-byte aligned.
pub const pdm_alignment_mask: usize = 3;

pub fn pdmBufferAligned(address: usize) bool {
    return (address & pdm_alignment_mask) == 0;
}

/// Why a PDM source configuration was rejected, in the C's guard order.
pub const PdmCfgFault = enum {
    ok,
    bad_channel,
    zero_sample_rate,
    zero_samples_per_frame,
    zero_poll_attempts,
    zero_valid_bits,
    valid_bits_too_wide,
    frame_bytes_overflow,
};

/// `internal_pdm_validate_cfg`. The frame size is returned separately so the
/// caller keeps the C's "compute once, store once" shape.
pub fn validatePdmCfg(
    channel: u8,
    sample_rate_hz: u32,
    samples_per_frame: u32,
    poll_attempts: u32,
    valid_bits: u8,
) PdmCfgFault {
    if (channel >= pdm_channel_count) return .bad_channel;
    if (sample_rate_hz == 0) return .zero_sample_rate;
    if (samples_per_frame == 0) return .zero_samples_per_frame;
    if (poll_attempts == 0) return .zero_poll_attempts;
    if (valid_bits == 0) return .zero_valid_bits;
    if (valid_bits > 32) return .valid_bits_too_wide;
    if (pdmFrameBytes(samples_per_frame) > std.math.maxInt(u32)) return .frame_bytes_overflow;
    return .ok;
}

/// PCM-S32LE frame size in bytes, widened so the product cannot wrap.
pub fn pdmFrameBytes(samples_per_frame: u32) u64 {
    return @as(u64, samples_per_frame) * @sizeOf(i32);
}

/// How many samples one interrupt span contributes to the frame in flight.
pub fn chunkLen(available: u32, remaining: u32) u32 {
    return if (available < remaining) available else remaining;
}

/// One bounded FIFO read: the HAL's status plus how many samples landed.
pub const ReadOutcome = struct { status: u16 = 0, got: u32 = 0 };

/// How the bounded fill loop ended.
pub const FillStatus = union(enum) { complete, timeout, failed: u16 };

/// `internal_pdm_fill`: read until the span is full or the attempt budget is
/// spent. `reader` is anything with `read(self, []i32) ReadOutcome`, so the
/// host tests drive it with no HAL at all.
///
/// One hardening: a reader claiming more samples than were asked for is
/// clamped to the remaining span. The C added `got` unchecked and would have
/// walked past the caller's buffer.
pub fn fillSamples(reader: anytype, samples: []i32, attempts: u32) FillStatus {
    const count: u32 = @intCast(samples.len);
    var filled: u32 = 0;
    var attempt: u32 = 0;
    while (attempt < attempts) : (attempt += 1) {
        if (filled == count) return .complete;
        const remaining = count - filled;
        const outcome = reader.read(samples[filled..]);
        if (outcome.status != 0) return .{ .failed = outcome.status };
        filled += if (outcome.got > remaining) remaining else outcome.got;
    }
    return .timeout;
}
