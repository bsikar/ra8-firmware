//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Moves ThreadX queue messages through the mailbox block's slots
//! (RA8FW-844). Both cores import it: each moves one queue's messages out
//! through one slot and the other slot's messages into another queue. A slot
//! holds one message, and the sender waits for the receiver's ack before it
//! writes the next, so nothing is overwritten and nothing is dropped. Neither
//! side ever waits inside a call: a full queue or a busy slot is tried again
//! on the next pass.

const shared = @import("shared.zig");

extern fn _txe_queue_send(queue: *anyopaque, source: *anyopaque, wait: c_ulong) callconv(.c) c_uint;
extern fn _txe_queue_receive(queue: *anyopaque, destination: *anyopaque, wait: c_ulong) callconv(.c) c_uint;

const tx_success: c_uint = 0;
const no_wait: c_ulong = 0;
const Message = [shared.message_words]u32;

fn barrier() void {
    asm volatile ("dsb" ::: .{ .memory = true });
}

/// Move one message from `queue` into `slot`, once the other core has taken
/// the last one. Returns whether a message moved.
pub fn send(queue: *anyopaque, slot: *volatile shared.Slot) bool {
    if (slot.ack != slot.seq) return false;
    var message: Message = undefined;
    if (_txe_queue_receive(queue, &message, no_wait) != tx_success) return false;
    for (&message, 0..) |*word, i| slot.words[i] = word.*;
    barrier();
    slot.seq +%= 1;
    barrier();
    return true;
}

/// Move the message waiting in `slot`, if there is one, into `queue` and
/// ack it. Returns whether a message moved.
pub fn take(slot: *volatile shared.Slot, queue: *anyopaque) bool {
    const seq = slot.seq;
    if (slot.ack == seq) return false;
    barrier();
    var message: Message = undefined;
    for (&message, 0..) |*word, i| word.* = slot.words[i];
    if (_txe_queue_send(queue, &message, no_wait) != tx_success) return false;
    slot.ack = seq;
    barrier();
    return true;
}

/// Take the message waiting in `slot`, if there is one, and drop it. Returns
/// whether there was one.
pub fn discard(slot: *volatile shared.Slot) bool {
    const seq = slot.seq;
    if (slot.ack == seq) return false;
    slot.ack = seq;
    barrier();
    return true;
}
