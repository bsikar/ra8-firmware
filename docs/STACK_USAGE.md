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

`cmake/ra_warnings.cmake :: ra_target_enable_project_warnings()`
attaches two flags to every project-owned C target:

```
-Wstack-usage=N    -- compile-time gate (-Werror)
-fstack-usage      -- emit `<file>.su` next to each `.o`
```

`N` defaults to **2048 bytes** and may be overridden per app:

```cmake
ra_target_enable_project_warnings(${RA_APP_NAME}.elf STACK_USAGE_BYTES 2200)
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

`scripts/utils/stack_usage_check.py` walks every
`examples/<tier>/<app>/build*/**/*.su`, parses every frame, and emits:

* `build/stack_usage.csv` -- one row per function, sortable in any
  spreadsheet or `awk` pipeline.
* `build/stack_usage_<app>.txt` -- per-app human-readable report,
  sorted by frame size descending.

The script exits non-zero when any function in a **critical-path
module** -- `ra_isr`, `ra_check`, `ra_err`, `ra_mpu`, `ra_cgc`,
`ra_pfs` -- has a frame larger than **256 bytes** or carries a
`dynamic` qualifier. Soft violations elsewhere (frame > 2048 bytes,
or `dynamic` in any non-critical TU) are reported but do not fail
the script -- the per-target `-Wstack-usage=N` warning is the
build-time gate for those.

`make stack-usage` builds every EVM-tier app (everything under
`examples/ek_ra8d2/`) and then runs the aggregator with `--top 10`.

## Current top-10 worst-offender functions

Snapshot taken after a fresh `make blink` build (2026-05-02). Refresh
this section by running `make stack-usage` and pasting the script's
"Top 10" output. The intent is that this list be reviewed at every
release and never grow without justification.

```
   bytes  qualifier         app/function  (tu)
   -----  -----------       ------------  ----
    1720  static            blink/ra_rsip_protected_rsa_decrypt    (libs/ra_hal/src/ra_rsip_protected.c)
    1128  static            blink/ra_rsip_protected_aes_init       (libs/ra_hal/src/ra_rsip_protected.c)
    1104  static            blink/ra_rsip_protected_ecdsa_sign     (libs/ra_hal/src/ra_rsip_protected.c)
     824  static            blink/dec_decode_scan                  (libs/ra_hal/src/ra_jpeg_sw.c)
     816  static            blink/enc_build_codes                  (libs/ra_hal/src/ra_jpeg_sw.c)
     648  static            blink/ra_rsip_key_inject_rsa           (libs/ra_hal/src/ra_rsip_key_injection.c)
     568  static            blink/enc_block                        (libs/ra_hal/src/ra_jpeg_sw.c)
     464  static            blink/internal_sha256_32               (src/secure_app/key_vault.c)
     376  static            blink/idct8x8                          (libs/ra_hal/src/ra_jpeg_sw.c)
     376  static            blink/fdct8x8                          (libs/ra_hal/src/ra_jpeg_sw.c)
```

All entries above are `static` (no VLAs / `alloca`) and none belong to
a critical-path module. The three `ra_rsip_protected_*` frames sit
near the per-target ceiling; they are reviewed under the existing
`STACK_USAGE_BYTES 2200` override in `examples/ek_ra8d2/blink/CMakeLists.txt`.

## Deviation procedure

If a function legitimately needs more than its module's budget
(e.g. a one-shot init routine that builds a large config struct on
the stack rather than holding it in `.bss`), the deviation MUST be
recorded inline using the `RA_STACK_BUDGET(N)` annotation macro:

```c
/**
 * @brief One-shot OTA header validator
 * @details Holds a 1.5 KB working struct on the stack to avoid a
 *          permanent .bss reservation that would only be live for
 *          ~5 ms at boot. Reviewed against the 8 KB main stack
 *          reservation in linker_script.ld.
 */
RA_STACK_BUDGET(1536) /* deviation: see commit <sha> + ticket */
ra_err_t ra_ota_validate_header(const ra_ota_blob_t *blob);
```

The macro itself is a no-op marker (it expands to nothing); its
purpose is to:

1. Be greppable -- `git grep RA_STACK_BUDGET` lists every approved
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
