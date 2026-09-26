# arch/hosted/

The design note #694 asks for: **MMU / Linux-class is a degenerate architecture,
not a sibling tier.**

A hosted backend satisfies the same `arch.h` contract as a bare-metal one by
delegating to POSIX. `arch_cpu_idle()` becomes a sleep, memory protection becomes
`mprotect`, block devices and the console become syscalls, and the capability
flags turn on `ARCH_HAS_MMU` and turn off the TrustZone and cache leaves. Nothing
above the arch tier changes, which is the whole claim: MMU support costs an
adapter, not a framework.

## This is not speculative; it is most of the way built

`RA8_OFF_TARGET` appears in 351 first-party files on `dev`. The host unit-test
build already replaces MMIO with a fake map (`tests/mocks/src/ra8_fake_mmap.c`),
already runs the same driver logic the target runs, and already reaches storage
and console through seams rather than registers. That is the substance of
`arch/hosted/`, arrived at incrementally and never named.

Naming it costs something worth paying: today the host build is a pile of
conditionals threaded through Ring-1 and Ring-3 code, and every one of them is a
place where host and target can quietly diverge. As an arch backend it is one
directory implementing one contract, and the conditionals collapse into backend
selection.

## What this note is not committing to

- **No second arch backend is being written in #694.** The work here is the
  contract plus the migration of the Armv8-M primitives out of `ra8_core`. A real
  `arch/hosted/` backend is a follow-on, and it should be filed as one once the
  Armv8-M backend exists to be the second implementation of the same header.
- **No decision about the OSAL seam.** How `fw_os` splits is an architecture call
  that belongs to the owner, not to the tier layout. It is noted in #693 for the
  same reason.

## Why this shape and not a hosted "port"

The alternative is treating hosted as a `port/` backend, alongside the POSIX
adapters already in `port/posix/`. That is the wrong level. `port/` binds a
platform-neutral interface to an implementation; the things a hosted build must
replace are below any port, in C-runtime init, idle, barriers, atomics and
protection. Those are exactly the members of the `arch.h` contract. Putting them
in `port/` would mean a second, parallel notion of "the lowest thing", which is
what the epic is trying to remove.
