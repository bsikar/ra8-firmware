# arch/

The CPU-architecture tier: the lowest layer in the platform structure #692
describes. A new instruction-set architecture is a new `arch/<isa>/` directory
implementing one contract header, [`arch.h`](arch.h), and nothing above it
notices which one it was built against.

## What is here today

The contract, and only the contract.

| Path | What it is |
|---|---|
| `arch.h` | The contract every `arch/<isa>/` backend implements. |
| `core/cortex_m85/caps.h` | RA8D2 CPU0 capability answers. |
| `core/cortex_m33/caps.h` | RA8D2 CPU1 capability answers. |
| `hosted/README.md` | Why the host build is already a second architecture. |

There is no backend directory yet, and that is deliberate: this slice of #694
fixes the target before anything moves, so each migration that follows is a move
plus an adapter rather than a design argument held one file at a time.

## What is misfiled today

The arch primitives exist. They are in the wrong tier, which is why the "Ring 1
is host == target" claim in [`docs/RING_AND_WORLD.md`](../docs/RING_AND_WORLD.md)
is not true yet. Measured on `dev`:

| Header in Ring-1 `libs/ra8_core/` | First-party files including it |
|---|---:|
| `ra8_boot_entry.h` | 278 |
| `ra8_exception.h` | 30 |
| `ra8_scb.h` | 9 |
| `ra8_systick.h` | 8 |

`ra8_core` is two libraries wearing one name: pure-C utilities that genuinely do
compile identically on host and target (err, check, log, the pin validator) and
Armv8-M core wrappers that do not. The split is the point of the migration, not a
side effect of it.

Two more pieces sit outside `ra8_core` and still belong to this tier:
`libs/ra8_mpu/` (PMSAv8) and `libs/ra8_hal/src/ra8_cache.c` (L1 maintenance).
Cache in particular is filed under the HAL today as though it were a peripheral;
it is a core block, and a core block that CPU1 does not have.

## Why capabilities are per core, not per ISA

The RA8D2 settles this without needing an argument from outside the tree. CPU0 is
a Cortex-M85 with an L1 data cache and Helium; CPU1 is a Cortex-M33 with neither.
Same ISA family, same silicon, different answers. So `ARCH_HAS_CACHE` and
`ARCH_HAS_SIMD` are declared in `core/<core>/caps.h`, and the M33 answer is an
honest decline with a reason rather than a cache API that silently does nothing.

CPU1 is not hypothetical: `examples/ek_ra8d2/hw_validated/hil/blink_m33`,
`.../dualcore_mailbox`, `.../dualcore_background_m33`, `.../cpu1_pingpong` and
`.../cache_coherency_hil` all build for it.

## What is NOT arch

Three things that look like they belong here and do not:

- **The SoC event router.** `libs/ra8_hal/src/ra8_icu.c` and
  `libs/ra8_hal/src/ra8_elc.c` implement the RA8's two-level event routing. That
  routing does not generalise across vendors, let alone across ISAs. The arch
  owns the CPU-side controller (NVIC); the SoC owns what feeds it.
- **The peripheral IRQ count and the option bytes.** The arch supplies the fixed
  core slots of the trap table; how many peripheral vectors follow is a SoC fact.
- **Register layouts.** The 62 `ra8_*_regs.h` headers in `libs/ra8_hal/inc/` are
  SoC data and move to `soc/<sil>/` under the same issue, not here.

## The four genuinely un-portable leaves

Kept as leaves and labelled as such, rather than abstracted into a framework that
would be lying:

1. Context-switch and fault-entry assembly, per-ISA register save and restore.
2. TrustZone-M. Armv8-M only, no cross-ISA analogue. 18 first-party files declare
   `cmse_nonsecure_entry` today, 13 of them under `libs/ra8_nsc/`.
3. Cache maintenance sequences and SIMD instruction encodings.
4. Vendor option bytes and the exact vector count.

## Status

Contract only. No backend compiles against this header yet, no build reaches
`arch/`, and the migration out of `ra8_core` has not started. Those are later
slices of #694.
