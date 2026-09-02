<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# ra8_emulator

Boots the **real, unmodified firmware `.elf`** -- the same image that flashes to
an EK-RA8D2 -- on an emulated Cortex-M over a modelled RA8 peripheral space,
and shows it in a board view: the GLCDC panel framebuffer beside a status
sidebar carrying the LED indicators in their real colours and live USB, UART,
timer-IRQ and touch state. So a non-display app is observable graphically too,
and a display app shows its screen next to that state. Because it runs the
actual cross-compiled binary through the genuine bring-up path, what you see is
what the flashed firmware draws.

`just apps::emulator::run <app>` from the repo root builds an app and opens it;
`--help` covers the rest. The live window is a macOS Cocoa window
(`just apps::emulator::setup` provisions
the toolchain and dependencies); every other path -- headless boot, the MMIO
report, frame capture, console capture -- builds and runs headless on Linux
too, which is what lets the emulator gates run on a Linux CI runner.

## Unicorn is version-pinned, deliberately

The CPU is Unicorn, QEMU's core as a library. Its decode of Armv8.1-M
(Helium/MVE) **differs between releases**, so an unpinned emulator makes the
same commit pass on one box and fault on another (#354). The pin lives in
[`docs/TOOLCHAIN.md`](../../docs/TOOLCHAIN.md) and the emulator gates fail
loudly when the runtime library is not it. That is not caution: an earlier
mismatch had one machine raising a spurious coprocessor fault on the Helium
store family, which is exactly why the same commit faulted locally and passed
in CI.

Unicorn itself tops out at Cortex-M33 (Armv8-M), yet the M85 firmware executes
on it, because the boot path emits no v8.1-M-only opcode. An
invalid-instruction trap reports any that ever appears.

## Which part it emulates

The RA8P1 shares the RA8D2's entire register map and memory map -- the
peripheral bases are byte-identical -- so one set of peripheral models serves
both parts and an RA8P1-linked ELF boots exactly as an RA8D2 one does. The
RA8P1 is "RA8D2 plus an Arm Ethos-U55 NPU", and selecting it exposes that one
extra register window.

The NPU is modelled **honestly but not implemented**: the window is mapped so
NPU-touching firmware does not spin on a phantom ready bit, every read returns a
stable zero -- no fabricated identity register, no faked done bit, an inference
is never pretended -- writes are recorded, and the end-of-run report prints a
`MAPPED BUT UNMODELLED` line with the access tally whenever it was touched.
Closed issue #222 delivered this honest RA8P1 profile and mapped stub; a real
command-stream model remains under the open emulator-fidelity epic #67. On the
RA8D2 profile the block is gated off entirely, so that run is
byte-for-behaviour unchanged.

## How it works

**Memory map.** Code, vectors and the debug space are Unicorn-owned guest
pages; the Renesas peripheral space is callback MMIO. SRAM, SDRAM and OSPI live
in host-owned apertures, and every engine maps **both** the Secure window and
its IDAU bit[28] Non-secure alias onto those same pages. Secure/Non-secure and
CPU0/CPU1 coherence are therefore *structural*: a guest store stays on the
translator's fast path, with no write hook, mirror step or dirty-page index in
the store path. The legacy data-flash window is deliberately left unmapped,
because the silicon does not decode it either.

The apertures are anonymous host mappings, never pre-touched, so a large
logical guest memory costs resident host memory only for the pages the firmware
actually writes -- asserted in both directions with `mincore`. Closing a
workspace refuses while an engine is still bound to it, because a bound engine
holds references into the pages the close would release.

**Peripherals.** A sparse fallback covers most of the boot path: control writes
read back as written, so "configure then verify" works, and once the firmware
spins reading one address past a threshold, reads alternate all-zeros and
all-ones so a single-bit poll for either edge completes instead of hanging.
That one generic rule satisfies almost every stabilization poll there is.

Above it sit register-accurate blocks -- GPIO, the timers, the SCI_B UART, and
I2C with the touch controller on it -- each in its own file, superseding the
fallback for its own address range. The UART model captures each transmit-data
write to the console sink, serves a host receive queue, and raises the transmit
and receive interrupts through the emulated interrupt path, so interrupt-driven
serial works as well as polled.

Exactly one register needs bespoke behaviour: the clock-frequency latches strip
a write key byte on readback, which matters because the firmware polls them for
an exact value rather than an edge.

**Time.** Between emulation chunks the installed SysTick handler is
cooperatively invoked as a function, so the tick counter advances and
millisecond delays return.

**USB.** The Full-Speed device controller is modelled register by register, and
a virtual host runs the standard chapter-9 enumeration against the *real*
vendored USB device stack -- raising the controller interrupt through the same
path the silicon would, so the genuine ISR answers each SETUP. For host-mode
firmware the High-Speed host controller is unmodelled, so the emulator instead
seams the first-party host primitives to a virtual keyboard or a virtual
mass-storage disk, and the firmware's real host stack enumerates, mounts and
browses it.

## Adding a peripheral block

The model is **decentralized**: the core owns only the block registry, MMIO
dispatch and interrupt routing, and keeps no hand-maintained list of blocks. So
blocks can be added in parallel without touching the core. Two steps:

1. Add a `board_periph_<blk>.c`. Implement the block's read and write handlers,
   plus optional tick, reset and report hooks; describe it with a static
   descriptor giving its absolute register base, span and ordering; and
   self-register it from a file-scope constructor. The emulator is a host
   program, so the constructor runs before `main` and the block is registered
   by the time the core resets it.
2. Add the file to the source list in `CMakeLists.txt`.

MMIO is dispatched by disjoint address range, so registration order is
irrelevant, and the optional hooks run in ascending descriptor order, so two
blocks added in parallel cannot conflict. A block needing a board-view value
declares its getter in the core header and implements it in its own file.

## What it is for, and what it is not

It fakes hardware *handshakes*. It validates "does the firmware drive the
controller correctly", not silicon timing, and it complements the bench rather
than replacing it. `scripts/emu/smoke.sh` gates a subset in CI, and the board
view is verifiable headlessly because the sidebar is composited into the same
pixel buffer the window shows -- a frame capture carries the panel *and* the
sidebar, so an overlay assertion is a pixel check rather than a human looking.

Running the real binary for longer than a bench run does is how it earns its
keep. It found a module-stop reference leak that only faults after the counter
saturates, which no short HIL run ever reaches (#68); and tracing the USB
device worker showed two demos silently stalling because their USB memory pool
was too small to satisfy a class's cache-safe buffer, so the device never
asserted its pull-up and the failure looked like a link problem.
