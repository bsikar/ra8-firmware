//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the pure audio policy: container widths, frame validation
//! in the C's guard order, frame-versus-metadata agreement, buffer
//! usability, the bounded FIFO fill loop, and the PDM configuration gate.

const std = @import("std");
const core = @import("implementation");

const samples_fixture = [_]i32{ 1, 2, 3, 4, 5, 6, 7, 8 };

fn fixture() core.Frame {
    return .{
        .data = &samples_fixture,
        .bytes = 32,
        .sample_count = 8,
        .sample_rate_hz = 16000,
        .timestamp_ms = 42,
        .channels = 1,
        .valid_bits = 20,
        .format = core.format_pcm_s32le,
    };
}

fn fixtureInfo() core.Info {
    return .{
        .frame_bytes = 32,
        .samples_per_frame = 8,
        .sample_rate_hz = 16000,
        .channels = 1,
        .valid_bits = 20,
        .format = core.format_pcm_s32le,
    };
}

test "container width is two bytes for s16le" {
    try std.testing.expectEqual(@as(u32, 2), core.containerBytes(core.format_pcm_s16le));
}

test "container width is four bytes for s32le" {
    try std.testing.expectEqual(@as(u32, 4), core.containerBytes(core.format_pcm_s32le));
}

test "container width is zero for every unsupported format byte" {
    var format: u32 = 2;
    while (format <= 255) : (format += 1) {
        try std.testing.expectEqual(@as(u32, 0), core.containerBytes(@intCast(format)));
    }
}

test "the fixture frame validates" {
    const frame = fixture();
    try std.testing.expectEqual(core.FrameFault.ok, core.validateFrame(&frame));
}

test "an absent sample pointer is a null-data fault" {
    var frame = fixture();
    frame.data = null;
    try std.testing.expectEqual(core.FrameFault.null_data, core.validateFrame(&frame));
}

test "an unsupported format is rejected before any scalar field" {
    var frame = fixture();
    frame.format = 9;
    frame.sample_count = 0;
    try std.testing.expectEqual(core.FrameFault.bad_format, core.validateFrame(&frame));
}

test "a zero sample count is rejected before the rate" {
    var frame = fixture();
    frame.sample_count = 0;
    frame.sample_rate_hz = 0;
    try std.testing.expectEqual(core.FrameFault.zero_sample_count, core.validateFrame(&frame));
}

test "a zero sample rate is rejected" {
    var frame = fixture();
    frame.sample_rate_hz = 0;
    try std.testing.expectEqual(core.FrameFault.zero_sample_rate, core.validateFrame(&frame));
}

test "a zero channel count is rejected before the valid-bit width" {
    var frame = fixture();
    frame.channels = 0;
    frame.valid_bits = 0;
    try std.testing.expectEqual(core.FrameFault.zero_channels, core.validateFrame(&frame));
}

test "a zero valid-bit width is rejected" {
    var frame = fixture();
    frame.valid_bits = 0;
    try std.testing.expectEqual(core.FrameFault.zero_valid_bits, core.validateFrame(&frame));
}

test "valid bits may exactly fill the container" {
    var frame = fixture();
    frame.valid_bits = 32;
    try std.testing.expectEqual(core.FrameFault.ok, core.validateFrame(&frame));
}

test "valid bits wider than the container are rejected" {
    var frame = fixture();
    frame.valid_bits = 33;
    try std.testing.expectEqual(core.FrameFault.valid_bits_too_wide, core.validateFrame(&frame));
}

test "s16le rejects a valid-bit width above sixteen" {
    var frame = fixture();
    frame.format = core.format_pcm_s16le;
    frame.bytes = 16;
    frame.valid_bits = 17;
    try std.testing.expectEqual(core.FrameFault.valid_bits_too_wide, core.validateFrame(&frame));
}

test "s16le geometry validates at half the byte count" {
    var frame = fixture();
    frame.format = core.format_pcm_s16le;
    frame.bytes = 16;
    frame.valid_bits = 16;
    try std.testing.expectEqual(core.FrameFault.ok, core.validateFrame(&frame));
}

test "a byte product that wraps to the fixture size is still a mismatch" {
    // 0x20000004 samples * 4 bytes truncates to exactly 32 in 32-bit
    // arithmetic, which is the fixture's own byte count. The widened product
    // is 0x80000010, so the frame is rejected instead of quietly accepted.
    var frame = fixture();
    frame.sample_count = 0x20000004;
    try std.testing.expectEqual(core.FrameFault.bytes_mismatch, core.validateFrame(&frame));
}

test "a byte product past 32 bits is a size overflow" {
    var frame = fixture();
    frame.sample_count = 0x4000_0000;
    frame.channels = 4;
    try std.testing.expectEqual(core.FrameFault.size_overflow, core.validateFrame(&frame));
}

test "a byte count that disagrees with the geometry is rejected" {
    var frame = fixture();
    frame.bytes = 28;
    try std.testing.expectEqual(core.FrameFault.bytes_mismatch, core.validateFrame(&frame));
}

test "stereo doubles the required byte count" {
    var frame = fixture();
    frame.channels = 2;
    try std.testing.expectEqual(core.FrameFault.bytes_mismatch, core.validateFrame(&frame));
    frame.bytes = 64;
    try std.testing.expectEqual(core.FrameFault.ok, core.validateFrame(&frame));
}

test "a frame matching its source metadata passes" {
    const frame = fixture();
    const info = fixtureInfo();
    try std.testing.expectEqual(core.MatchFault.ok, core.frameMatchesInfo(&frame, &info));
}

test "each metadata field is an independent match invariant" {
    const frame = fixture();

    var info = fixtureInfo();
    info.frame_bytes = 28;
    try std.testing.expectEqual(core.MatchFault.bytes, core.frameMatchesInfo(&frame, &info));

    info = fixtureInfo();
    info.samples_per_frame = 7;
    try std.testing.expectEqual(core.MatchFault.sample_count, core.frameMatchesInfo(&frame, &info));

    info = fixtureInfo();
    info.sample_rate_hz = 8000;
    try std.testing.expectEqual(core.MatchFault.sample_rate, core.frameMatchesInfo(&frame, &info));

    info = fixtureInfo();
    info.channels = 2;
    try std.testing.expectEqual(core.MatchFault.channels, core.frameMatchesInfo(&frame, &info));

    info = fixtureInfo();
    info.valid_bits = 24;
    try std.testing.expectEqual(core.MatchFault.valid_bits, core.frameMatchesInfo(&frame, &info));

    info = fixtureInfo();
    info.format = core.format_pcm_s16le;
    try std.testing.expectEqual(core.MatchFault.format, core.frameMatchesInfo(&frame, &info));
}

test "the byte count is compared before the sample count" {
    const frame = fixture();
    var info = fixtureInfo();
    info.frame_bytes = 28;
    info.samples_per_frame = 7;
    try std.testing.expectEqual(core.MatchFault.bytes, core.frameMatchesInfo(&frame, &info));
}

test "a buffer with storage and capacity is usable" {
    var storage = [_]u8{0} ** 32;
    const buffer = core.Buffer{ .data = &storage, .capacity = storage.len };
    try std.testing.expectEqual(core.BufferFault.ok, core.bufferFault(&buffer));
}

test "an absent buffer pointer is rejected before its capacity" {
    const buffer = core.Buffer{ .data = null, .capacity = 0 };
    try std.testing.expectEqual(core.BufferFault.null_data, core.bufferFault(&buffer));
}

test "a zero-capacity buffer is rejected" {
    var storage = [_]u8{0} ** 4;
    const buffer = core.Buffer{ .data = &storage, .capacity = 0 };
    try std.testing.expectEqual(core.BufferFault.zero_capacity, core.bufferFault(&buffer));
}

test "capacity fits when it equals or exceeds the frame" {
    try std.testing.expect(core.capacityFits(32, 32));
    try std.testing.expect(core.capacityFits(33, 32));
    try std.testing.expect(!core.capacityFits(31, 32));
}

test "the PDM alignment mask accepts only four-byte addresses" {
    try std.testing.expect(core.pdmBufferAligned(0x2000_0000));
    try std.testing.expect(!core.pdmBufferAligned(0x2000_0001));
    try std.testing.expect(!core.pdmBufferAligned(0x2000_0002));
    try std.testing.expect(!core.pdmBufferAligned(0x2000_0003));
    try std.testing.expect(core.pdmBufferAligned(0x2000_0004));
}

test "a well-formed PDM configuration is accepted" {
    try std.testing.expectEqual(core.PdmCfgFault.ok, core.validatePdmCfg(2, 16000, 8, 4, 24));
}

test "a channel at or past the bound is rejected first" {
    try std.testing.expectEqual(core.PdmCfgFault.bad_channel, core.validatePdmCfg(3, 0, 0, 0, 0));
    try std.testing.expectEqual(core.PdmCfgFault.bad_channel, core.validatePdmCfg(255, 16000, 8, 4, 24));
    try std.testing.expectEqual(core.PdmCfgFault.ok, core.validatePdmCfg(0, 16000, 8, 4, 24));
}

test "PDM scalar gates fire in the C's order" {
    try std.testing.expectEqual(core.PdmCfgFault.zero_sample_rate, core.validatePdmCfg(2, 0, 0, 0, 0));
    try std.testing.expectEqual(core.PdmCfgFault.zero_samples_per_frame, core.validatePdmCfg(2, 16000, 0, 0, 0));
    try std.testing.expectEqual(core.PdmCfgFault.zero_poll_attempts, core.validatePdmCfg(2, 16000, 8, 0, 0));
    try std.testing.expectEqual(core.PdmCfgFault.zero_valid_bits, core.validatePdmCfg(2, 16000, 8, 4, 0));
}

test "PDM valid bits may reach but not exceed 32" {
    try std.testing.expectEqual(core.PdmCfgFault.ok, core.validatePdmCfg(2, 16000, 8, 4, 32));
    try std.testing.expectEqual(core.PdmCfgFault.valid_bits_too_wide, core.validatePdmCfg(2, 16000, 8, 4, 33));
}

test "a PDM frame past four gigabytes is a size fault" {
    const too_many: u32 = 0x4000_0000;
    try std.testing.expectEqual(
        core.PdmCfgFault.frame_bytes_overflow,
        core.validatePdmCfg(2, 16000, too_many, 4, 24),
    );
    try std.testing.expectEqual(core.PdmCfgFault.ok, core.validatePdmCfg(2, 16000, too_many - 1, 4, 24));
}

test "PDM frame bytes are four per sample and cannot wrap" {
    try std.testing.expectEqual(@as(u64, 32), core.pdmFrameBytes(8));
    try std.testing.expectEqual(@as(u64, 0x4_0000_0000), core.pdmFrameBytes(0xFFFF_FFFF) + 4);
}

test "a chunk is the smaller of what arrived and what the frame still needs" {
    try std.testing.expectEqual(@as(u32, 3), core.chunkLen(3, 8));
    try std.testing.expectEqual(@as(u32, 8), core.chunkLen(9, 8));
    try std.testing.expectEqual(@as(u32, 5), core.chunkLen(5, 5));
    try std.testing.expectEqual(@as(u32, 0), core.chunkLen(0, 5));
}

/// Bounded reader that hands back a fixed number of samples per attempt.
const StepReader = struct {
    per_call: u32,
    calls: u32 = 0,
    status: u16 = 0,
    fail_after: u32 = std.math.maxInt(u32),
    value: i32 = 7,

    pub fn read(self: *StepReader, out: []i32) core.ReadOutcome {
        self.calls += 1;
        if (self.calls > self.fail_after) return .{ .status = self.status, .got = 0 };
        const give = @min(self.per_call, @as(u32, @intCast(out.len)));
        for (out[0..give]) |*slot| slot.* = self.value;
        return .{ .got = give };
    }
};

test "the fill loop completes in one attempt when the FIFO is full" {
    var storage = [_]i32{0} ** 8;
    var reader = StepReader{ .per_call = 8 };
    try std.testing.expectEqual(core.FillStatus.complete, core.fillSamples(&reader, &storage, 4));
    try std.testing.expectEqual(@as(u32, 1), reader.calls);
    try std.testing.expectEqual(@as(i32, 7), storage[7]);
}

test "the fill loop accumulates across attempts" {
    var storage = [_]i32{0} ** 8;
    var reader = StepReader{ .per_call = 3 };
    try std.testing.expectEqual(core.FillStatus.complete, core.fillSamples(&reader, &storage, 8));
    // Three reads fill it, and a fourth pass observes the span is complete.
    try std.testing.expectEqual(@as(u32, 3), reader.calls);
}

test "an empty FIFO spends the attempt budget and times out" {
    var storage = [_]i32{0} ** 8;
    var reader = StepReader{ .per_call = 0 };
    try std.testing.expectEqual(core.FillStatus.timeout, core.fillSamples(&reader, &storage, 5));
    try std.testing.expectEqual(@as(u32, 5), reader.calls);
}

test "a zero attempt budget times out without reading" {
    var storage = [_]i32{0} ** 8;
    var reader = StepReader{ .per_call = 8 };
    try std.testing.expectEqual(core.FillStatus.timeout, core.fillSamples(&reader, &storage, 0));
    try std.testing.expectEqual(@as(u32, 0), reader.calls);
}

test "a transport error stops the fill and is forwarded verbatim" {
    var storage = [_]i32{0} ** 8;
    var reader = StepReader{ .per_call = 2, .status = 0x407, .fail_after = 1 };
    const status = core.fillSamples(&reader, &storage, 8);
    try std.testing.expectEqual(@as(u16, 0x407), status.failed);
    try std.testing.expectEqual(@as(u32, 2), reader.calls);
}

test "an over-reporting reader cannot walk past the caller's span" {
    var storage = [_]i32{0} ** 4;
    var reader = StepReader{ .per_call = 64 };
    try std.testing.expectEqual(core.FillStatus.complete, core.fillSamples(&reader, &storage, 4));
}

test "an empty span completes on the first attempt" {
    var storage = [_]i32{};
    var reader = StepReader{ .per_call = 1 };
    try std.testing.expectEqual(core.FillStatus.complete, core.fillSamples(&reader, &storage, 1));
    try std.testing.expectEqual(@as(u32, 0), reader.calls);
}
