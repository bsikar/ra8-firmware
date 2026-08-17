# Stack-Usage Bounding

Static stack-overflow is the silent equivalent of dynamic allocation:
NASA Power-of-10 Rule 3 forbids `malloc` after init, but a 4 KB
`uint8_t buf[4096]` on the stack of an interrupt handler will smash
the next region of RAM just as effectively.

This document explains how this firmware *demonstrates* a per-function
stack bound -- a hard requirement for the project's IEC 61508 SIL 3 /
DO-178C Level B claim.

## Why this matters under SIL 3 / DO-178C Level B

* **IEC 61508-3 Table A.4** lists "limited use of dynamic objects" and
  "limited stack" as Highly Recommended techniques for SIL 3. The
  certifier expects evidence that the worst-case stack depth is
  bounded and fits inside the linker-reserved stack region.
* **DO-178C** Software Verification (Section 6) requires that
  resource-usage claims (memory, stack, WCET) be substantiated by
  analysis -- not by testing alone. For Level B in particular,
  structural-coverage claims are vacated if the program can corrupt
  its own stack at runtime.
* **CWE-121 / CWE-674** -- stack-based buffer overflow and
  uncontrolled recursion are routinely exploited where the bound was
  assumed rather than proved.

The compiler can give us per-function frame sizes for free; the cost
is wiring up the report and the gate.

## How the build emits the data

`cmake/ra8_warnings.cmake :: ra8_target_enable_project_warnings()`
attaches two flags to every project-owned C target:

```
-Wstack-usage=N    -- compile-time gate (-Werror)
-fstack-usage      -- emit `<file>.su` next to each `.o`
```

`N` defaults to **2048 bytes** and may be overridden per app:

```cmake
ra8_target_enable_project_warnings(${RA8_APP_NAME}.elf STACK_USAGE_BYTES 2200)
```

Both flags are gated on `COMPILE_LANG_AND_ID:C,GNU` so the host
unit-test build (which may use clang) is unaffected.

## How to read a `.su` file

Each line of `<file>.su` has the form:

```
path/to/file.c:LINE:COL:function_name<TAB>FRAME_BYTES<TAB>QUALIFIER
```

`QUALIFIER` is one of:

| Qualifier            | Meaning                                                  |
|----------------------|----------------------------------------------------------|
| `static`             | Frame size known at compile time. Safe.                  |
| `bounded`            | Variable-length but with a compiler-provable upper bound.|
| `dynamic`            | VLA or `alloca()`; size only known at runtime. Forbidden.|
| `dynamic,bounded`    | VLA whose upper bound the compiler can prove.            |

The project policy is: **no `dynamic` frames anywhere** (no VLAs,
no `alloca()`). `bounded` is acceptable when the bound is enum-derived.

## The aggregator

`scripts/checks/stack_usage_check.py` walks every `.su` file under any
per-app `build*/` tree beneath `examples/` (apps nest 2-4 levels deep,
e.g. `examples/ek_ra8d2/hw_validated/hil/<app>/build/`), parses every
frame, and emits:

* `build/stack_usage.csv` -- one row per function, sortable in any
  spreadsheet or `awk` pipeline.
* `build/stack_usage_<app>.txt` -- per-app human-readable report,
  sorted by frame size descending.

The script exits non-zero when any function in a **critical-path
module** -- `ra8_isr`, `ra8_check`, `ra8_err`, `ra8_mpu`, `ra8_cgc`,
`ra8_pfs` -- has a frame larger than **256 bytes** or carries a
`dynamic` qualifier. Soft violations elsewhere (frame > 2048 bytes,
or `dynamic` in any non-critical TU) are reported but do not fail
the script -- the per-target `-Wstack-usage=N` warning is the
build-time gate for those.

`make stack-usage` builds every EVM-tier app (everything under
`examples/ek_ra8d2/`) and then runs the aggregator over the result.

## Where the big frames are

`make stack-usage` regenerates the report. This section describes the
shape the report always has, rather than what it said on one day.

The largest frames in the tree belong to the vendored miniz
deflate / zip helpers, which link only into the book-reading apps. They
are SOUP: the project vendors third-party libraries unmodified and
writes integration shims around them, so the answer to a large upstream
frame is the per-app `STACK_USAGE_BYTES` override that sizes the
consuming app's main stack -- not an edit to `libs/third_party/`.

The largest first-party frames are the RSIP protected-mode routines in
`libs/ra8_hal/src/ra8_rsip_protected.c`, and that is deliberate: each
holds an unwrapped key or modulus scratch buffer plus a full
`ra8_rsip_key_handle_t` on the stack, so the secret material is scrubbed
via `p_scrub` when the frame unwinds. Moving those buffers into `.bss`
would either persist the secret across calls or require an explicit
clear-on-exit path that doubles the attack surface. Each carries an
inline `RA8_STACK_BUDGET(N)` matching its `.su` value and a
`@par Stack-budget deviation:` block that says why.

No function anywhere in the report carries a `dynamic` qualifier,
because one is a hard failure of the gate below: NASA Power-of-10
Rule 3 forbids VLAs and `alloca()` regardless of frame size.

## Pre-commit gate

`scripts/git/pre-commit` invokes
`scripts/checks/stack_usage_check.py --strict --quiet` on every commit.
Behaviour:

* If no `.su` files exist yet (fresh clone) the gate is a no-op so
  the first commit on a clean tree never blocks.
* **Soft violations in first-party TUs** (per-app frame > 2048 bytes
  in any file outside `libs/third_party/`) -- HARD FAIL. The author
  must either reduce the frame (move scratch buffers to module-static
  storage) or enroll the function in `FIRST_PARTY_EXEMPTIONS` at the
  top of `scripts/checks/stack_usage_check.py` with a written
  rationale.
* **Soft violations in third-party SOUP** (`libs/third_party/`) --
  reported but ignored. They are the miniz deflate / zip helpers,
  invoked only from a worker thread that carries a generously-sized
  stack. SOUP is qualified per `docs/SOUP/`.
* **Critical-module breaches** (`ra8_isr` / `ra8_check` / `ra8_err` /
  `ra8_mpu` / `ra8_cgc` / `ra8_pfs` > 256 bytes) -- HARD FAIL via the
  existing critical-path gate inside the script.
* A `dynamic` qualifier ANYWHERE in the report is a HARD FAIL. NASA
  Power-of-10 Rule 3 forbids VLAs and `alloca()` in this firmware
  regardless of frame size, so no deviation procedure is offered.

## Deviation procedure

If a function legitimately needs more than its module's budget
(e.g. a one-shot init routine that builds a large config struct on
the stack rather than holding it in `.bss`), the deviation MUST be
recorded inline using the `RA8_STACK_BUDGET(N)` annotation macro from
`libs/ra8_core/inc/ra8_stack_budget.h`. Place the marker as the first
statement inside the function body so the preceding Doxygen block
remains adjacent to the function signature for `doxy_audit.py`:

```c
/**
 * @brief One-shot OTA header validator
 * @details Holds a 1.5 KB working struct on the stack to avoid a
 *          permanent .bss reservation that would only be live for
 *          ~5 ms at boot. Reviewed against the 8 KB main stack
 *          reservation in linker_script.ld.
 */
ra8_err_t ra8_ota_validate_header(const ra8_ota_blob_t *blob)
{
    RA8_STACK_BUDGET(1536); /* deviation: see commit <sha> + ticket */
    /* ... */
}
```

The macro itself is a no-op marker (it expands to nothing); its
purpose is to:

1. Be greppable -- `git grep RA8_STACK_BUDGET` lists every approved
   deviation.
2. Force a code-review conversation about *why* the frame is large.
3. Pin the documented bound next to the function so the next reader
   sees it before refactoring.

A deviation must include:

* A documented justification (`@details` / commit message / ticket).
* The reviewed bound `N` matching the value seen in the `.su` file.
* A non-trivial alternative considered (move to `.bss`? split the
  function? batch the work?).

A deviation does **not** suppress the `-Wstack-usage=N` warning --
the per-app `STACK_USAGE_BYTES` budget must also be raised, and the
raise must be justified in the per-app `CMakeLists.txt`.
