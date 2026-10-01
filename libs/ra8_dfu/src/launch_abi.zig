//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C membrane for the copy-to-run hand-off that performs NO authentication:
//! `ra8_dfu_launch_unverified` in `ra8_dfu.h`.
//!
//! `ra8_dfu_launch.c` carried both the authenticating and the plain hand-off
//! in one translation unit and chose between them with
//! `RA8_ENABLE_ROOT_OF_TRUST` at compile time. A prebuilt archive cannot see
//! an app's compile definitions, and two archives cannot both export
//! `ra8_dfu_launch` for an app that links both, so the flag became two
//! symbols: this one, and `ra8_dfu_launch` in `libs/ra8_rot`, which verifies
//! and then calls this. An app that deliberately launches an unauthenticated
//! image names the unauthenticated entry point.

const builtin = @import("builtin");
const image = @import("image");
const launch = @import("launch");

/// Off target there is no SRAM run base to write and no core to branch, so
/// the hand-off stops after its guards.
const off_target = builtin.target.os.tag != .freestanding;

extern fn ra8_scb_set_vtor(base: usize) void;

/// Order the image stores against the fetch that follows them. The
/// instructions only exist on the target.
fn barrier() void {
    asm volatile ("dsb 0xF\n isb 0xF\n" ::: "memory");
}

export fn ra8_dfu_launch_unverified(src: usize, img_len: u32, entry: u32) void {
    if (!launch.mayCopy(src, img_len, entry)) return;
    if (off_target) return;

    const words = img_len / @sizeOf(u32);
    const source: []const volatile u32 = @as([*]const volatile u32, @ptrFromInt(src))[0..words];
    const run: []volatile u32 = @as([*]volatile u32, @ptrFromInt(image.layout.run_base))[0..words];

    asm volatile ("cpsid i" ::: "memory");
    // Bounded by img_len, which `mayCopy` has already held to
    // `image.layout.img_max`: a statically bounded copy, NASA Rule 2.
    for (run, source) |*word, value| word.* = value;
    asm volatile ("dsb 0xF" ::: "memory"); // stores reach SRAM before the fetch

    const initial_sp = run[0];
    const reset_entry = run[1];

    // Point the Secure VTOR at the run base, then fence: the vector fetch on
    // the coming branch must see the new base.
    ra8_scb_set_vtor(image.layout.run_base);
    barrier();
    asm volatile ("msr msp, %[sp]\n bx %[entry]\n"
        :
        : [sp] "r" (initial_sp),
          [entry] "r" (reset_entry),
        : "memory"
    );
    unreachable;
}
