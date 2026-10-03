//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A byte transport over two rings in shared memory, one each way.
//!
//! The order of every step is the contract with the other core:
//!
//! * writing: copy the data, barrier, publish `head`, notify;
//! * reading: read `head`, barrier, copy the data, barrier, publish `tail`.
//!
//! So data is in place before the index that reveals it, and a byte is not
//! given back to the writer until it has been copied out.
//!
//! A ring's header is partly written by the other core, so it is checked on
//! every use. A header that fails the check is `BadMessage`, and from then on
//! every call returns it until `reset`.

const ring = @import("ring.zig");
const Ring = ring.Ring;
const Signal = @import("signal.zig").Signal;
const Transport = @import("transport.zig").Transport;

pub const RingTransport = struct {
    /// The ring this end writes. It is the only producer.
    out: Ring,
    /// The ring this end reads. It is the only consumer.
    in: Ring,
    signal: Signal,
    /// Set when a header fails its check. Every call after it returns the
    /// same error until `reset`.
    broken: ?Transport.Error = null,

    pub fn init(out: Ring, in: Ring, signal: Signal) RingTransport {
        return .{ .out = out, .in = in, .signal = signal };
    }

    /// The transport points at this `RingTransport`, so it must not move.
    pub fn transport(self: *RingTransport) Transport {
        return .{ .ctx = self, .vtable = &vtable };
    }

    /// Clear the failure. The rings themselves are not touched: see the
    /// README for what both cores do before this is called.
    pub fn reset(self: *RingTransport) void {
        self.broken = null;
    }

    const vtable: Transport.VTable = .{ .send = send, .receive = receive, .poll = poll };

    fn from(ctx: *anyopaque) *RingTransport {
        return @ptrCast(@alignCast(ctx));
    }

    fn fail(self: *RingTransport, err: Transport.Error) Transport.Error {
        self.broken = err;
        return err;
    }

    fn send(ctx: *anyopaque, bytes: []const u8) Transport.Error!void {
        const self = from(ctx);
        if (self.broken) |err| return err;

        const indices = self.out.load() catch |err| return self.fail(err);
        if (bytes.len > self.out.free(indices)) return error.LinkFull;
        if (bytes.len == 0) return;

        self.out.write(indices.head, bytes);
        self.signal.barrier();
        self.out.publishHead(self.out.advance(indices.head, @intCast(bytes.len)));
        self.signal.notify();
    }

    fn receive(ctx: *anyopaque, into: []u8) Transport.Error!usize {
        const self = from(ctx);
        if (self.broken) |err| return err;

        const loaded = self.in.load();
        self.signal.barrier();
        const indices = loaded catch |err| return self.fail(err);

        const count: u32 = @intCast(@min(into.len, self.in.used(indices)));
        if (count == 0) return 0;
        self.in.copy(indices.tail, into[0..count]);
        self.signal.barrier();
        self.in.publishTail(self.in.advance(indices.tail, count));
        return count;
    }

    /// Nonzero once broken, or if the header no longer checks, so the next
    /// receive reports the failure.
    fn poll(ctx: *anyopaque) usize {
        const self = from(ctx);
        if (self.broken != null) return 1;
        const indices = self.in.load() catch return 1;
        return self.in.used(indices);
    }
};
