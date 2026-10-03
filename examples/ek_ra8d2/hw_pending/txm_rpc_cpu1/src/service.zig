//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What both halves of txm_rpc_cpu1 agree on (RA8FW-544): the one service
//! the resident image answers, the ThreadX queues the call travels over,
//! and the application requests the module reports through.
//!
//! The module is the client and the resident image the server. The module
//! creates both queues in its own memory and hands them over in `attach`.

/// The methods the resident image serves.
pub const Method = struct {
    pub const add: u16 = 1;
};

pub const Add = struct { a: u32, b: u32 };
pub const Sum = struct { value: u32 };

/// The largest call or reply body: `Add` is the largest message.
pub const max_body = 8;
/// One call is outstanding at a time.
pub const in_flight = 1;
/// No capability bits are defined for this service.
pub const caps: u32 = 0;

/// Each queue message is the largest ThreadX allows: sixteen 32-bit words.
pub const message_words = 16;
pub const message_bytes = message_words * 4;
pub const queue_depth = 4;

/// The arguments of the call on step `step`.
pub fn argsFor(step: u32) Add {
    return .{ .a = step +% 1, .b = (step +% 1) *% 100 };
}

/// The sum the resident image must answer on step `step`.
pub fn sumFor(step: u32) u32 {
    const args = argsFor(step);
    return args.a +% args.b;
}

/// The application requests the module makes. The manager passes a request
/// at or above `base` to the application, less the base.
pub const Report = struct {
    pub const base: u32 = 0x10000;
    /// The two queues: param_1 to the resident image, param_2 back.
    pub const attach: u32 = 1;
    /// A sum came back: param_1 the sum, param_2 its step.
    pub const value: u32 = 2;
    /// The module failed: param_1 the `Stage`, param_2 the detail.
    pub const failed: u32 = 3;
};

/// Where the module failed, reported with `Report.failed`.
pub const Stage = struct {
    pub const allocate: u32 = 1;
    pub const create: u32 = 2;
    pub const bind: u32 = 3;
    pub const greet: u32 = 4;
    pub const call: u32 = 5;
    pub const poll: u32 = 6;
    /// The answer was not the response to the call just made.
    pub const reply: u32 = 7;
    /// The server refused the call; the detail is its code.
    pub const refused: u32 = 8;
    /// The sum was wrong; the detail is the step.
    pub const wrong: u32 = 9;
    /// Nothing came back in `patience_ticks`.
    pub const timeout: u32 = 10;
};

/// Ticks the module waits for a frame before it reports a timeout.
pub const patience_ticks: u32 = 1000;
