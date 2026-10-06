//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The service txm_dual_mailbox's calls use (RA8FW-844, RA8FW-842). It began
//! as a copy of txm_rpc_cpu1's and adds `fault`: the call the M85's module
//! makes after its round trips, which asks CPU1's server module to store
//! outside its MPU regions.
//!
//! The M85's module, txm_dual_client_m33, is the client. The server is CPU1's
//! module, txm_dual_server_m33 (RA8FW-849), and every queue message crosses
//! the mailbox block between the two. Once that module has faulted, CPU1's
//! resident image answers in its place with a fault frame carrying
//! `module_gone`.

/// The methods the resident image serves.
pub const Method = struct {
    pub const add: u16 = 1;
    /// Store at `Poke.address`; the server module's MPU should stop it.
    pub const fault: u16 = 2;
};

pub const Add = struct { a: u32, b: u32 };
pub const Poke = struct { address: u32 };

/// The word the server module tries to leave at `Poke.address`: "FAUL".
pub const poke_value: u32 = 0x4641_554C;
/// The `ra8_rpc` code CPU1's resident image refuses calls with once its
/// module is gone: the first code the library leaves to applications.
pub const module_gone: u16 = 0x0100;
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
    /// The client's `fault` call came back refused, as it should once CPU1's
    /// module is gone.
    pub const gone: u32 = 4;
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
