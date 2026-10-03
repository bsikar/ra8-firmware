//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A byte transport over two queues of fixed-size messages, one each way.
//!
//! Every message starts with a `u16`, little-endian: how many of the bytes
//! after it are in use. A write is cut into as many messages as it needs, all
//! of them full except possibly the last, whose tail is zero. No message is
//! ever sent with nothing in it.
//!
//! The outgoing queue must have this transport as its only sender. That is
//! what lets a write be checked against the free messages first and then sent
//! whole or not at all.

const std = @import("std");
const Queue = @import("queue.zig").Queue;
const Transport = @import("transport.zig").Transport;

/// The used-length field at the front of every message.
pub const Used = struct {
    pub const Int = u16;
    pub const bytes = @sizeOf(Int);
};

pub const QueueTransport = struct {
    out: Queue,
    in: Queue,
    /// The message being built for `out`. Caller-owned.
    tx_message: []u8,
    /// The message last taken from `in`. Caller-owned.
    rx_message: []u8,
    /// `rx_message[at..end]` is received and not yet handed on.
    at: usize = 0,
    end: usize = 0,
    /// Set by the first failure that leaves the stream unusable. Every call
    /// after it returns the same error until `reset`.
    broken: ?Transport.Error = null,

    pub const InitError = error{
        /// A message has no room for anything after its length field.
        MessageTooSmall,
        /// A message holds more than its length field can count.
        MessageTooBig,
        /// A scratch buffer is shorter than one message of its queue.
        BufferTooSmall,
    };

    /// `tx_message` and `rx_message` must each hold one message of `out` and
    /// of `in`, and must outlive the transport.
    pub fn init(
        out: Queue,
        in: Queue,
        tx_message: []u8,
        rx_message: []u8,
    ) InitError!QueueTransport {
        for ([_]Queue{ out, in }) |queue| {
            if (queue.message_bytes <= Used.bytes) return error.MessageTooSmall;
            if (room(queue) > std.math.maxInt(Used.Int)) return error.MessageTooBig;
        }
        if (tx_message.len < out.message_bytes) return error.BufferTooSmall;
        if (rx_message.len < in.message_bytes) return error.BufferTooSmall;
        return .{
            .out = out,
            .in = in,
            .tx_message = tx_message[0..out.message_bytes],
            .rx_message = rx_message[0..in.message_bytes],
        };
    }

    /// The transport points at this `QueueTransport`, so it must not move.
    pub fn transport(self: *QueueTransport) Transport {
        return .{ .ctx = self, .vtable = &vtable };
    }

    /// Start again after an error: drop what was received, empty the
    /// incoming queue and clear the failure.
    ///
    /// Only this end is reset. See the README for the whole procedure.
    pub fn reset(self: *QueueTransport) void {
        for (0..self.in.waiting()) |_| {
            if (!(self.in.receive(self.rx_message) catch break)) break;
        }
        self.at = 0;
        self.end = 0;
        self.broken = null;
    }

    const vtable: Transport.VTable = .{ .send = send, .receive = receive, .poll = poll };

    /// Bytes one message of `queue` carries after its length field.
    fn room(queue: Queue) usize {
        return queue.message_bytes - Used.bytes;
    }

    fn from(ctx: *anyopaque) *QueueTransport {
        return @ptrCast(@alignCast(ctx));
    }

    fn fail(self: *QueueTransport, err: Transport.Error) Transport.Error {
        self.broken = err;
        return err;
    }

    fn send(ctx: *anyopaque, bytes: []const u8) Transport.Error!void {
        const self = from(ctx);
        if (self.broken) |err| return err;

        const each = room(self.out);
        const count = bytes.len / each + @intFromBool(bytes.len % each != 0);
        if (count > self.out.free()) return error.LinkFull;

        var rest = bytes;
        while (rest.len != 0) {
            const used = @min(rest.len, each);
            std.mem.writeInt(Used.Int, self.tx_message[0..Used.bytes], @intCast(used), .little);
            @memcpy(self.tx_message[Used.bytes..][0..used], rest[0..used]);
            @memset(self.tx_message[Used.bytes + used ..], 0);
            // The queue refused a message it said it had room for. Part of
            // this write may already be out, so the stream is finished.
            self.out.send(self.tx_message) catch return self.fail(error.LinkDown);
            rest = rest[used..];
        }
    }

    fn receive(ctx: *anyopaque, into: []u8) Transport.Error!usize {
        const self = from(ctx);
        if (self.broken) |err| return err;

        var done: usize = 0;
        while (done < into.len) {
            if (self.at == self.end) {
                // Bytes already handed over are good. The failure is kept,
                // and the next call reports it.
                const more = self.pull() catch |err| if (done != 0) break else return err;
                if (!more) break;
            }
            const count = @min(into.len - done, self.end - self.at);
            @memcpy(into[done..][0..count], self.rx_message[self.at..][0..count]);
            self.at += count;
            done += count;
        }
        return done;
    }

    /// Take the next message from the queue. False if there is none.
    fn pull(self: *QueueTransport) Transport.Error!bool {
        const got = self.in.receive(self.rx_message) catch return self.fail(error.LinkDown);
        if (!got) return false;

        const used = std.mem.readInt(Used.Int, self.rx_message[0..Used.bytes], .little);
        if (used == 0 or used > room(self.in)) return self.fail(error.BadMessage);
        self.at = Used.bytes;
        self.end = Used.bytes + used;
        return true;
    }

    /// An upper bound: a waiting message may carry less than it has room for.
    /// Nonzero once broken, so the next receive reports the failure.
    fn poll(ctx: *anyopaque) usize {
        const self = from(ctx);
        if (self.broken != null) return 1;
        return (self.end - self.at) + self.in.waiting() * room(self.in);
    }
};
