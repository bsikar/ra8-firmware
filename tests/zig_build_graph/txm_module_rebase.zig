//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The start-up pass that rebases the addresses a module keeps in its
//! initialised data (RA8FW-539).
//!
//! Upstream's `gcc_setup.s` rebuilds the GOT against where the module was
//! loaded and copies `.data` byte for byte. A word of data that holds an
//! address, a constant vtable's every entry for one, keeps its link-time
//! value. This pass walks the table tools/check_txm_module_relocs
//! generated and puts each such word right.
//!
//! It is added to a generated copy of the vendored file, the way RA8FW-484
//! changes a vendored header, so no vendored file is forked. It goes in at
//! the end of `_gcc_setup`, after the data has been copied and before the
//! function returns to the thread shell, which calls the module's entry
//! next. `_gcc_setup` copies the data afresh each time it runs, so the pass
//! always starts from link-time values and never rebases a word twice.
//!
//! The registers are `_gcc_setup`'s own: r4 is the data link base, r5 the
//! code load base and r9 the data load base. r3, the code link base, is
//! loaded again because the copy helpers use it as scratch. A stored value
//! is told apart exactly as that function tells a GOT entry apart: below
//! the data link base it is a code address, otherwise a data one.

const header_patch = @import("header_patch.zig");
const cpu1_txm_hello = @import("cpu1_txm_hello.zig");

/// The vendored file the copy is made from.
pub const source = cpu1_txm_hello.gcc_setup;

/// The line the pass goes in front of: where `_gcc_setup` restores its
/// registers. It must match one line of the vendored file exactly, so an
/// upstream change to it fails the build rather than dropping the pass.
const restore_line =
    "    LDMIA   sp!, {r3, r4, r5, r6, r7, lr}       // Store other preserved registers";

const pass =
    \\    /* Rebase the addresses held in initialised data. */
    \\
    \\    ldr     r3, =__FLASH_segment_start__
    \\    ldr     r0, =__txm_rebase_start__
    \\    sub     r0, r0, r3
    \\    add     r0, r0, r5
    \\    ldr     r1, =__txm_rebase_end__
    \\    sub     r1, r1, r3
    \\    add     r1, r1, r5
    \\
    \\txm_rebase_next:
    \\    cmp     r0, r1                  // See if there are more records
    \\    beq     txm_rebase_done         // No, every word is rebased
    \\    ldr     r2, [r0]                // Link address of a data word
    \\    sub     r2, r2, r4
    \\    add     r2, r2, r9              // Where that word was loaded
    \\    ldr     r6, [r2]                // The address it holds
    \\    cmp     r6, r4                  // Is it in the code or data area?
    \\    blt     txm_rebase_code         // If less than, it is a code address
    \\    sub     r6, r6, r4
    \\    add     r6, r6, r9              // Based on the loaded data address
    \\    b       txm_rebase_store
    \\txm_rebase_code:
    \\    sub     r6, r6, r3
    \\    add     r6, r6, r5              // Based on the loaded code address
    \\txm_rebase_store:
    \\    str     r6, [r2]
    \\    add     r0, r0, #4
    \\    b       txm_rebase_next
    \\txm_rebase_done:
    \\
    \\
;

/// The one rewrite that turns the vendored file into the copy.
pub const rewrites = [_]header_patch.Rewrite{
    .{ .old = restore_line, .new = pass ++ restore_line },
};
