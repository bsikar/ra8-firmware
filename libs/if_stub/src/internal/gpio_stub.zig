//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The stub GPIO profile, and nothing else.
//!
//! `if_stub` is the proof binding of epic RA8FW-618 slice 5: the `if_<chip>`
//! set a second target supplies in place of the RA8 adapters. This unit is
//! that set's GPIO member. It reads no hardware and touches no register, so
//! the whole profile is a pure function of its call history and runs on the
//! host without a fake peripheral block.
//!
//! The profile is deliberately tiny and deliberately arbitrary: four pins in
//! dense board numbering, each with a direction, a pull and a driven level.
//! An output pin's level is what a write left there; an input pin samples the
//! board's tied level, which for a stub board is low on every pin. Those
//! semantics assert nothing about real silicon; they exist so the port's
//! configure, write and read intents can be exercised end to end through the
//! facade. A real chip port replaces this file, and the interface above it
//! never changes.
//!
//! The enums are spelled numerically so this unit stays free of the C header;
//! the smoke translation unit pins them against `fw_if_gpio.h` itself, which
//! is what keeps the two transcriptions from drifting apart.

/// `fw_gpio_direction_t` values the profile names.
pub const direction_none: u8 = 0; // k_fw_gpio_direction_none
pub const direction_input: u8 = 1; // k_fw_gpio_direction_input
pub const direction_output: u8 = 2; // k_fw_gpio_direction_output

/// `fw_gpio_pull_t` values the profile names.
pub const pull_none: u8 = 0; // k_fw_gpio_pull_none
pub const pull_up: u8 = 1; // k_fw_gpio_pull_up
pub const pull_down: u8 = 2; // k_fw_gpio_pull_down

/// How many pins the stub board carries: 0 through 3, dense numbering.
pub const pin_count: usize = 4;

/// One pin the stub board carries.
const Pin = struct {
    direction: u8 = direction_none,
    pull: u8 = pull_none,
    driven: bool = false,
};

var pins = [_]Pin{ .{}, .{}, .{}, .{} };

/// The direction and pull a configure request names.
pub const Request = struct {
    direction: u8,
    pull: u8,
};

/// Is this pin one the stub board carries?
pub fn carries(pin: u16) bool {
    return pin < pin_count;
}

/// Put one pin into a direction and pull. A direction of none is not a
/// direction, so it is rejected, and so is a pull outside the enum.
pub fn configure(pin: u16, request: Request) error{ InvalidArg, NotFound }!void {
    const slot = slot_of(pin) orelse return error.NotFound;

    if (request.direction == direction_none) return error.InvalidArg;
    if (request.direction != direction_input and request.direction != direction_output) {
        return error.InvalidArg;
    }
    if (request.pull > pull_down) return error.InvalidArg;

    pins[slot] = .{ .direction = request.direction, .pull = request.pull };
}

/// Drive one output pin. An input pin rejects this, per the port's contract.
pub fn write(pin: u16, value: bool) error{ InvalidState, NotFound }!void {
    const slot = slot_of(pin) orelse return error.NotFound;

    if (pins[slot].direction != direction_output) return error.InvalidState;
    pins[slot].driven = value;
}

/// Sample one pin's level: what an output drives, or the board's tied level
/// (low) for an input.
pub fn read(pin: u16) error{NotFound}!bool {
    const slot = slot_of(pin) orelse return error.NotFound;

    if (pins[slot].direction == direction_output) return pins[slot].driven;
    return false;
}

fn slot_of(pin: u16) ?usize {
    if (!carries(pin)) return null;
    return @as(usize, pin);
}

/// Restore the unconfigured board. Test hygiene, not product surface.
pub fn reset() void {
    pins = [_]Pin{ .{}, .{}, .{}, .{} };
}
