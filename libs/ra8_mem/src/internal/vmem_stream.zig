//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The byte-stream adapter over a page-cached object (#147/#151). A read of an
//! arbitrary `(offset, len)` span walks the covering cache frames one at a
//! time: page the frame holding the cursor in, copy the in-frame slice, release
//! the pin, advance. At most one frame is pinned at any instant, so the
//! resident set is the caller's fixed pool plus O(1), never the object size.
//!
//! The reason this file exists rather than the caller just calling the cache is
//! the short-read distinction (#764). A span can come back short for two
//! unrelated reasons, and a consumer that confuses them compiles a truncated
//! book off a dying card and calls it a success. So a read returns a `Read`,
//! carrying the byte count AND the verdict together, and there is no way to
//! take one without seeing the other.
//!
//! The cache is injected at comptime: `ra8_mem_abi.zig` supplies the real one
//! over `ra8_vmem_get`/`ra8_vmem_put`, the tests supply a fake. The calls stay
//! direct, and this file holds no `extern`, so it can be a host test root.

const std = @import("std");

const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// `ra8_vmem_cfg_t`, field for field. Only `frame_bytes` is read, at bind
/// time, but the whole struct is mirrored so a field added to the C ahead of
/// `frame_bytes` cannot silently shift the offset out from under us.
pub const Cfg = extern struct {
    frame_mem: ?[*]u8,
    frame_bytes: u32,
    frame_count: u32,
    meta: ?*anyopaque,
    keys: ?*anyopaque,
    buckets: ?[*]i32,
    bucket_count: u32,
    loader: ?*const anyopaque,
    loader_ctx: ?*anyopaque,
    protected_pct: u8,
};

/// `ra8_vmem_t` as far as this module needs it: `cfg` is its first member, and
/// nothing here touches the SLRU engine that follows. Only ever held as a
/// pointer handed in from C; Zig never allocates one.
pub const Vmem = extern struct {
    cfg: Cfg,
};

/// `ra8_vmem_stream_t`, field for field.
pub const Stream = extern struct {
    vm: ?*Vmem = null,
    object_id: u32 = 0,
    frame_bytes: u32 = 0,
    size: u64 = 0,
};

/// A frame the cache handed back, or the reason it would not.
pub const Frame = union(enum) {
    page: []const u8,
    failed: Err,
};

/// What a read produced: the bytes genuinely copied, and the verdict. `.ok`
/// with `copied < requested` is end of file; anything else is a failed frame
/// with `copied` holding what was good before it.
pub const Read = struct {
    copied: u32 = 0,
    err: Err = .ok,
};

comptime {
    std.debug.assert(@offsetOf(Stream, "object_id") == @sizeOf(usize));
    std.debug.assert(@offsetOf(Stream, "frame_bytes") == @sizeOf(usize) + 4);
    // `size` is 8-aligned on both widths, which lands it at 16 either way.
    std.debug.assert(@offsetOf(Stream, "size") == 16);
    std.debug.assert(@offsetOf(Cfg, "frame_bytes") == @sizeOf(usize));
}

pub fn init(stream: *Stream, vm: *Vmem, object_id: u32, size: u64) Err {
    if (size == 0) return .invalid_size;
    const frame_bytes = vm.cfg.frame_bytes;
    if (frame_bytes == 0) return .invalid_size;

    stream.* = .{
        .vm = vm,
        .object_id = object_id,
        .frame_bytes = frame_bytes,
        .size = size,
    };
    return .ok;
}

/// One frame-sized step of the walk.
const Step = struct {
    frame_base: u64,
    in_frame: u32,
    chunk: u32,
};

fn step(cur: u64, remaining: u32, frame_bytes: u32) Step {
    const frame_base = cur - (cur % @as(u64, frame_bytes));
    const in_frame: u32 = @intCast(cur - frame_base);
    return .{
        .frame_base = frame_base,
        .in_frame = in_frame,
        .chunk = @min(remaining, frame_bytes - in_frame),
    };
}

/// Bytes the object can actually serve from `offset`, clamped to `len`.
fn wanted(stream: *const Stream, offset: u64, len: u32) u32 {
    const avail = stream.size - offset;
    return if (len > avail) @intCast(avail) else len;
}

pub fn readChecked(comptime Cache: type, stream: *const Stream, offset: u64, buf: []u8) Read {
    if (stream.frame_bytes == 0) return .{ .err = .invalid_state };
    if (buf.len == 0) return .{ .err = .invalid_size };
    if (offset >= stream.size) return .{}; // Clean end of file.

    const want = wanted(stream, offset, @intCast(buf.len));
    var done: u32 = 0;
    var cur = offset;

    // Bounded: every pass copies at least one byte and `done` rises toward the
    // fixed `want`, so the walk runs at most `want` times.
    while (done < want) {
        const at = step(cur, want - done, stream.frame_bytes);
        const frame = switch (Cache.get(stream.vm, stream.object_id, at.frame_base, stream.frame_bytes)) {
            .failed => |err| return .{ .copied = done, .err = err },
            .page => |page| page,
        };
        @memcpy(buf[done..][0..at.chunk], frame[at.in_frame..][0..at.chunk]);

        const released = Cache.put(stream.vm, frame);
        if (released != .ok) return .{ .copied = done, .err = released };

        done += at.chunk;
        cur += at.chunk;
    }

    return .{ .copied = done };
}
