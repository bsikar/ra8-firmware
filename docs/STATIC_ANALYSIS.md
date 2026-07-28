# Static Analysis: Clang Static Analyzer (scan-build)

This file documents the project's **scan-build** gate and baseline. scan-build
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

# Exactly what CI runs: the wrapper's own --selftest, then --strict.
bash scripts/ci.sh --gate scan-build

# Just the wrapper's both-directions selftest (seconds, no analyzer needed):
bash scripts/checks/scan_build.sh --selftest
```

The selftest is not ceremony. This script decides what counts as an actionable
finding, and a classifier that binned everything as suppressed would report a
clean tree forever. It drives the real classifier over synthetic reports in
both directions -- a first-party finding and a fixed-address finding OUTSIDE
the MMIO partitions must be REPORTED; SOUP, `tests/` and the documented MMIO
partitions must be SUPPRESSED -- plus the vacuity floor either side of its
boundary and the fail-loud path.

Every run reconfigures and rebuilds from scratch, and wipes the previous
reports first. That is not hygiene: scan-build analyses what the build
*compiles*, so an incremental rerun analyses only what changed and reports
nothing, and the summary used to read its numbers out of the newest report
directory -- which on a second run was the previous run's. Both were measured
here, in both directions.

The analyzer is pinned to **`scan-build-18`**, from the `clang-tools-18`
package the devcontainer installs. The pin is load-bearing: the checker set
differs between clang majors (see the `core.FixedAddressDereference` note
below), so a run under a different major does not stand in for this one. The
wrapper prefers `scan-build-18` and honours a `SCAN_BUILD=<path>` override;
if neither resolves it **fails**, it does not skip.

Reports land under `build/scan-build-reports/<timestamp>/`. The
top-level `index.html` aggregates every finding with full path
visualisations. When there are no findings, scan-build writes no
directory at all -- which is why the wrapper floors the *translation-unit*
and *object-file* counts rather than inferring anything from an empty
report tree.

## Suppression policy

Three classes of findings are silenced by the wrapper (see
`scripts/checks/scan_build.sh`):

| Partition / pattern                                                                                | Why suppressed                                                                                                |
|----------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------|
| `libs/third_party/`                                                                                | SOUP -- pre-qualified external code, see `docs/SOUP/`                                                          |
| `tests/`                                                                                           | Host-only test scaffolding, exempt per CLAUDE.md                                                               |
| `core.FixedAddressDereference` in `libs/ra8_hal/{src,inc}/`, `libs/ra8_mpu/{src,inc}/`, `libs/ra8_core/src/ra8_log.c`, `libs/ra8_core/src/ra8_exception.c` | Hardware-register MMIO accessor pattern -- see "MMIO suppression rationale" below. **Matches nothing under the pinned clang-18**; kept for the day the pin moves. |

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

Measured 2026-07-28 with **`scan-build-18` in the CI devcontainer image**,
over 1128 translation units:

| Class                                              | Count | Disposition                                         |
|----------------------------------------------------|-------|-----------------------------------------------------|
| Actionable first-party                             | **0** | The `scan-build` gate passes                        |
| Third-party (`libs/third_party/`)                  | 40    | Suppressed -- SOUP                                  |
| Test scaffolding (`tests/`)                        | 0     | Suppressed -- host-only test code, exempt           |
| `core.FixedAddressDereference` (HAL MMIO)          | 0     | Checker does not exist in clang-18 (see below)      |

### Why the previous table did not reproduce

The table this replaces recorded 683 MMIO findings, 869 test-scaffolding
findings and 4 third-party findings "as of 2026-05-02 (clang 18,
llvm/Homebrew)". None of those numbers reproduce in the environment CI
actually runs in, and the reason is instructive rather than mysterious:

* **`core.FixedAddressDereference` is not in clang-18.** `clang-18 -cc1
  -analyzer-checker-help` lists no such checker; it arrives in a later major.
  The 683 findings were produced by a newer Homebrew clang, and the
  path/check-id filter that suppresses them has therefore never matched
  anything on a runner. It is kept because moving the pin brings all 683
  straight back, and deleting the filter would make that the day's problem.
* **The 869 test-scaffolding findings were `deadcode.DeadStores`**, which the
  same wrapper disables globally. The old table counted a class the wrapper
  had already switched off.
* The third-party count moved 4 -> 40 with the analyzer major and with three
  months of vendored-SOUP growth.

The lesson is the one #190 keeps finding: a number transcribed once from one
machine is not evidence, and nothing noticed it had stopped being true for
three months because nothing re-derived it. The gate below re-derives all four
counts on every run.

### The one first-party finding this gate found, and its fix

Turning the gate on surfaced exactly one actionable finding:
`port/esp-hosted/src/ra8_esp_hosted_rtos_pool.c`, in
`ra8_esp_hosted_rtos_alloc` -- *"Null pointer passed to 1st parameter expecting
'nonnull'"* at the header `memcpy`.

It was real, not a false positive. `tx_byte_allocate` writes its out-parameter
only on the success path, and the host ThreadX model's fault-injection seam
could return the armed status -- which defaults to `TX_SUCCESS` -- *without*
writing it. A test arming that family with `TX_SUCCESS` would get a reported
allocation with no block, and the port would compute its header address from
zero. Fixed at both ends:

* the model honours its own documented postcondition and nulls the
  out-parameter on the injection path;
* the port validates the out-parameter after a `TX_SUCCESS`, which makes the
  cross-function invariant locally provable -- the same shape as the
  `core.DivideZero` fix below -- and `test_pool_exhaustion_reports_null` now
  drives that arm.

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

## MMIO suppression rationale

### `core.FixedAddressDereference`

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

**Under the current pin this filter matches nothing**, because clang-18 has no
such checker. Do not read the `0` in the baseline table as "the HAL is clean by
this checker" -- read it as "this checker did not run". Moving the pin to a
major that has it should reproduce roughly the 683 the old table recorded, all
of them suppressed by this filter, which is exactly why it stays.

## CI / hook integration

The per-commit hook does **not** invoke scan-build (a full analyzed rebuild
takes minutes -- too expensive to gate every commit). Instead:

* Developers run `make scan-build` locally before opening a PR.
* CI runs the **`scan-build` gate** on every push and same-repo PR: the
  `scan-build` job in `.github/workflows/firmware.yml`, which is a thin
  `bash scripts/ci.sh --gate scan-build` driver over
  `scripts/checks/scan_build.sh --strict`. It is a required job like every
  other gate; there is no warn-only mode.

  Until 2026-07-28 this section claimed CI ran `--strict` "on every push" and
  that the gate was "warn-only today". Neither was true: no workflow invoked
  the script, `RA8_GATE_REGISTRY` had no such gate, and
  `scripts/git/pre-commit` carried a comment saying exactly that -- so the tree
  contradicted itself in writing. That was #532.

## Cross-references

* `scripts/checks/scan_build.sh`  -- driver script
* `scripts/ci/gates/analysis.sh`  -- the `scan-build` gate body
* `cmake/ra8_warnings.cmake`      -- per-target warning flags (-Wall etc.)
* `docs/SOUP/README.md`          -- third-party qualification index
* `docs/MCDC.md`                 -- complementary MC/DC coverage gate
