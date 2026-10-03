//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The byte carrier a session runs over, injected as a context and a vtable.
//!
//! A transport moves bytes and knows nothing about frames. It may deliver
//! them in any pieces it likes; the session reassembles.

pub const Transport = struct {
    ctx: *anyopaque,
    vtable: *const VTable,

    pub const Error = error{
        /// The transport cannot take the whole write now. Nothing was sent.
        LinkFull,
        /// The other end is gone.
        LinkDown,
        /// The transport received something it cannot turn into bytes.
        BadMessage,
    };

    pub const VTable = struct {
        /// Queue all of `bytes`, or none of them and an error.
        send: *const fn (ctx: *anyopaque, bytes: []const u8) Error!void,
        /// Copy waiting bytes into `into` and say how many; zero if none.
        receive: *const fn (ctx: *anyopaque, into: []u8) Error!usize,
        /// Give the transport a turn and say how many bytes are waiting:
        /// zero if none, and otherwise the count or an upper bound on it.
        poll: *const fn (ctx: *anyopaque) usize,
    };

    pub fn send(self: Transport, bytes: []const u8) Error!void {
        return self.vtable.send(self.ctx, bytes);
    }

    pub fn receive(self: Transport, into: []u8) Error!usize {
        return self.vtable.receive(self.ctx, into);
    }

    pub fn poll(self: Transport) usize {
        return self.vtable.poll(self.ctx);
    }
};
