# arch/

The CPU-architecture tier: the lowest layer in the platform structure RA8FW-298
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
| `../scripts/checks/check_arch_caps.py` | The gate holding those answers to their word. |

## What enforces the capability contract

`arch.h` says an optional capability is a compile-time fact: a core answers
`ARCH_HAS_<CAP>` with 1 only when the operations that capability names are
actually implemented for it. Nothing checked that until now, so a core could
claim a capability and fail to link, or implement one and never advertise it.

`scripts/checks/check_arch_caps.py` reads the gated operations out of `arch.h`,
reads each `core/*/caps.h` answer, and fails in both directions: a flag set to 1
with no implementation behind it, and an implementation present under a flag
answered 0. It takes `--selftest`, which proves the detector fires and stays
quiet on sixteen constructed cases before any tree scan is trusted, and it runs
in the `arch-caps` gate in `scripts/ci/gates/checks_standalone.sh`.

```sh
python3 scripts/checks/check_arch_caps.py --selftest
python3 scripts/checks/check_arch_caps.py
```

There is no backend directory yet, and that is deliberate: this slice of RA8FW-300
fixes the target before anything moves, so each migration that follows is a move
plus an adapter rather than a design argument held one file at a time.

## The timebase target

`arch.h` now declares the monotonic timebase as a MUST: `arch_timebase_configure`,
`arch_timebase_now` and `arch_timebase_hz`. It had no slot before, which is the
concrete reason "de-middleware the SysTick" (RA8FW-299 step 0) was a design question
rather than a move: `ra8_systick.h` was the only declaration of a timebase
anywhere in the tree, so every consumer that wanted the time reached into a
Ring-1 register header, and there was nowhere else to point them.

The timebase is a MUST and not a capability because every architecture in scope
mandates a core timekeeping block: SysTick on Armv8-M, `mtime` on RISC-V, the
host clock on a hosted backend. `arch_tick_configure`, in the RTOS-gated block,
is the scheduler's claim on that same block rather than a second one; a
bare-metal build owes a monotonic `now()` and owes no scheduler tick, which is
why the two are separated by a gate instead of merged.

`arch_timebase_configure` returns the rate it achieved rather than `void`. A
24-bit SysTick reload cannot divide a fast core clock to an arbitrary tick rate,
so a refused request is a routine outcome; `ra8_systick_reload_for` already
range-checks exactly this for Armv8-M, and the contract keeps that honesty
rather than programming the nearest value silently.

## What is misfiled today

The arch primitives exist. They are in the wrong tier, which is why the "Ring 1
is host == target" claim in [`docs/RING_AND_WORLD.md`](../docs/RING_AND_WORLD.md)
is not true yet. Each count below is measured by the command the manifest at the
foot of this page names, and re-run by
[`scripts/checks/check_measured_counts.py`](../scripts/checks/check_measured_counts.py),
so the migration order is argued from the tree rather than from a number someone
pasted once:

| Header in Ring-1 `libs/ra8_core/` | First-party files including it |
|---|---:|
| `ra8_boot_entry.h` | 269 |
| `ra8_exception.h` | 12 |
| `ra8_scb.h` | 6 |
| `ra8_systick.h` | 4 |

The first of those decides the slicing. `ra8_boot_entry.h` is reached by two
orders of magnitude more files than the other three combined, so it moves on its
own, behind a compatibility header, after the small three have proved the shape.

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

## What compiles this contract

`scripts/checks/check_arch_compiles.py`, and until it existed nothing did. No
library, no test and no tool reached `arch/arch.h`, so the contract was a header
that had never been through a compiler. Two things had already gone wrong in it
unnoticed: `::K_ARCH_FAULT_RAW_MAX` was referenced in the documentation of
`arch_fault_info_t` and defined nowhere, with `raw[8]` written as a bare
literal, and `bool` appeared in five declarations with no `<stdbool.h>` behind
it. The second is latent rather than live, since `bool` is a keyword in the C23
this tree pins, but a contract header should not get its types by accident.

The gate compiles the contract at `-std=c23` with `-Wall -Wextra -Wpedantic
-Wundef -Wconversion -Werror` against four capability states:

| Compiled against | What it proves |
| --- | --- |
| `arch/core/cortex_m33/caps.h` | the contract parses for the real M33 answers, and its `static_assert`s hold for that core's values |
| `arch/core/cortex_m85/caps.h` | the same for the M85, whose cache and SIMD answers differ |
| a synthetic core, every optional capability **off** | the declaration blocks vanish cleanly, and what remains still compiles |
| a synthetic core, every optional capability **on** | every gated block parses, including ones no real core enables |

The synthetic pair is the part that earns its keep. Between them the two real
cores never exercise `ARCH_HAS_MEM_PROTECT (0)`, `ARCH_HAS_RTOS_CONTEXT (0)` or
`ARCH_HAS_TRUSTZONE_M (0)`, so a mistake inside one of those blocks would have
waited for the first backend that declines the capability. `-Wundef` is
deliberate too: a capability is consumed with `#if ARCH_HAS_X`, and a misspelled
flag evaluates to 0, which reads as a clean decline rather than as the typo it
is.

The contract also stays **freestanding**. A bare-metal target ships no
`<assert.h>`, so the assertions use the C23 `static_assert` keyword and the gate
holds the include list to the C23 freestanding set. It checks that textually
rather than by cross-compiling, so it needs no target toolchain.

This is a compiler, not a second opinion: the flags a core must ANSWER are
`scripts/checks/check_arch_caps.py`'s job, and the values that come with a set
answer are this one's.

## Status

Contract only, now compiled. There is still no backend: nothing implements the
declared symbols, no build reaches `arch/` for linking, and the migration out of
`ra8_core` has not started. Those are later slices of RA8FW-300.

## How the numbers here are measured

Every count on this page is one entry in the block below: the table row it
backs, the count it claims, and the command that produces it. The gate re-runs
all of them, so a number here cannot drift from the tree without failing, in
either direction. A count that grew means the migration went backwards; a count
that shrank is progress this page has to credit.

```sh
# MEASURED BLOCK -- re-run by scripts/checks/check_measured_counts.py
# ra8_boot_entry.h -- 269 file(s)
grep -rlE '#[ \t]*include[ \t]+"ra8_boot_entry\.h"' libs apps examples tests --include=*.c --include=*.h | grep -v /third_party/ | wc -l
# ra8_exception.h -- 12 file(s)
grep -rlE '#[ \t]*include[ \t]+"ra8_exception\.h"' libs apps examples tests --include=*.c --include=*.h | grep -v /third_party/ | wc -l
# ra8_scb.h -- 6 file(s)
grep -rlE '#[ \t]*include[ \t]+"ra8_scb\.h"' libs apps examples tests --include=*.c --include=*.h | grep -v /third_party/ | wc -l
# ra8_systick.h -- 4 file(s)
grep -rlE '#[ \t]*include[ \t]+"ra8_systick\.h"' libs apps examples tests --include=*.c --include=*.h | grep -v /third_party/ | wc -l
```

```sh
python3 scripts/checks/check_measured_counts.py --selftest
python3 scripts/checks/check_measured_counts.py --check
```
