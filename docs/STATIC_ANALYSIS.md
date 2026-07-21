# Static Analysis: Clang Static Analyzer (scan-build)

This file documents the project's **scan-build** baseline. scan-build
(a.k.a. the Clang Static Analyzer) is the path-sensitive analyzer that
ships with clang; it complements `cppcheck` (style / pattern matching)
by symbolically executing every path through every translation unit and
flagging null-deref, use-after-free, division-by-zero, dead-store,
uninitialized-read and similar logic errors.

ra8-firmware runs scan-build against the **host unit-test build**
(`tests/build-scan/`) -- the cross-compile firmware build cannot be
analyzed reliably because clang has no working sysroot for
`arm-none-eabi`. The host build covers the same first-party `libs/`,
`src/`, and `port/` translation units, just with mocked HAL.

## How to run

```sh
# Local: full analyzer pass + summary
make scan-build

# CI gate (fails on any first-party finding):
bash scripts/checks/scan_build.sh --check
```

Reports land under `build/scan-build-reports/<timestamp>/`. The
top-level `index.html` aggregates every finding with full path
visualisations.

## Suppression policy

Three classes of findings are silenced by the wrapper (see
`scripts/checks/scan_build.sh`):

| Partition / pattern                                                                                | Why suppressed                                                                                                |
|----------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------|
| `libs/third_party/`                                                                                | SOUP -- pre-qualified external code, see `docs/SOUP/`                                                          |
| `tests/`                                                                                           | Host-only test scaffolding, exempt per CLAUDE.md                                                               |
| `core.FixedAddressDereference` in `libs/ra8_hal/{src,inc}/`, `libs/ra8_mpu/{src,inc}/`, `libs/ra8_core/src/ra8_log.c`, `libs/ra8_core/src/ra8_exception.c` | Hardware-register MMIO accessor pattern -- see "MMIO suppression rationale" below for the full justification. |

In addition the wrapper disables two checkers globally:

* `deadcode.DeadStores` -- fires constantly on the unit-test mocks
  (every `tests/test_*.c` writes to `g_<peripheral>_regs[]` to seed a
  scenario, then the test reads back; the analyzer flags the seed write
  as dead because it doesn't see the read happen across the test
  boundary).
* `security.insecureAPI.DeprecatedOrUnsafeBufferHandling` -- fires on
  every `memcpy`/`strcpy` in the codebase. The MISRA-C 2012 / Power-of-
  10 rules already constrain string handling; this checker would drown
  out real findings.

## Current baseline

As of 2026-05-02 the analyzer reports the following first-party
findings against the host test build (clang 18, llvm/Homebrew):

| Class                                              | Count | Disposition                                         |
|----------------------------------------------------|-------|-----------------------------------------------------|
| Actionable first-party                             | **0** | CI gate (`--strict`) passes                         |
| `core.FixedAddressDereference` (HAL MMIO)          | 683   | Suppressed -- hardware-register accessor pattern    |
| Third-party (`libs/third_party/`)                  | 4     | Suppressed -- SOUP                                  |
| Test scaffolding (`tests/`)                        | 869   | Suppressed -- host-only test code, exempt           |

Historical context: prior to 2026-05-02 the wrapper reported 685
"first-party" findings (683 MMIO + 2 `core.DivideZero` false positives
in `libs/ra8_hal/src/ra8_jpeg_sw.c::dec_decode_scan`). The two
`core.DivideZero` findings were eliminated by adding an explicit
`assert(d->hmax > 0U && d->vmax > 0U)` immediately before the MCU-grid
size computation; the assertion re-states the cross-function invariant
that `dec_parse_sof0()` already enforces (it returns success only for
4:4:4, 4:2:2, 4:2:2-h-only, and 4:2:0 chroma layouts). The 683 MMIO
findings were silenced by the path/check-id filter described below.

## MMIO suppression rationale

### `core.FixedAddressDereference` (683 findings, all suppressed)

Every memory-mapped register access in the HAL goes through an inline
accessor that returns a pointer cast from a `uintptr_t` enum constant
(see CLAUDE.md "Hardware Register Access"):

```c
typedef enum : uintptr_t {
    k_ra8_vin_base = 0x40169000,
} ra8_vin_addr_t;

static inline volatile uint32_t* ra8_vin_reg32(uint32_t off) {
    return (volatile uint32_t*)(k_ra8_vin_base + off);
}

// ... later ...
*ra8_vin_reg32(k_ra8_vin_off_ints) = (uint32_t)k_ra8_vin_int_fme;
```

scan-build's `core.FixedAddressDereference` checker flags every such
write because the destination pointer comes from a literal address.
This is **the entire point** of memory-mapped I/O firmware: every PFS,
PORT, SCI, SPI, ELC, ICU, ... register write trips the same checker.
There is no way to satisfy the checker without abandoning
register-level access.

**Mitigation:** the cross-compile build (`make blink_hal`) does the
same thing on real hardware; the analyzer noise is purely an artefact
of running on the host unit-test build where the addresses point at
mock register banks. `scripts/checks/scan_build.sh` post-processes the
generated HTML reports and bins every
`core.FixedAddressDereference` finding under the suppressed-MMIO
counter when the `BUGFILE` path matches one of the documented MMIO
partitions. No source-level annotations or `// NOLINT` markers are
required: the suppression is a single check-id + path filter,
expressed in the wrapper, which keeps the source files free of
analyzer-specific noise.

### `core.DivideZero` (eliminated 2026-05-02)

Previously two findings sat inside `dec_decode_scan()` in
`libs/ra8_hal/src/ra8_jpeg_sw.c`. The MCU width/height divisions
(`(d->width + mcu_w_px - 1U) / mcu_w_px` and the height analogue)
depend on `d->hmax` / `d->vmax`, which are validated cross-function in
`dec_parse_sof0()`. Adding an explicit
`assert(d->hmax > 0U && d->vmax > 0U)` immediately before the
divisions makes the invariant locally provable and the analyzer is now
quiet on this path. The assertion also satisfies NASA Power-of-10
Rule 5 (minimum two preconditions per function).

## CI / hook integration

The per-commit hook does **not** invoke scan-build (a full pass takes
several minutes -- too expensive to gate every commit). Instead:

* Developers run `make scan-build` locally before opening a PR
  (`make scan-build-strict` mirrors the CI gate).
* CI runs `bash scripts/checks/scan_build.sh --strict` (alias for
  `--check`) on every push. The gate is **warn-only today** in
  practice -- the runner is wired but the workflow does not yet flip
  the job to required -- and is expected to flip to required once a
  second clean run on a CI builder confirms the 0-finding baseline is
  reproducible. Locally and on the development host the strict mode
  exits 0.

## Cross-references

* `scripts/checks/scan_build.sh`  -- driver script
* `cmake/ra8_warnings.cmake`      -- per-target warning flags (-Wall etc.)
* `docs/SOUP/README.md`          -- third-party qualification index
* `docs/MCDC.md`                 -- complementary MC/DC coverage gate
