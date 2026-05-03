# Static Analysis: Clang Static Analyzer (scan-build)

This file documents the project's **scan-build** baseline. scan-build
(a.k.a. the Clang Static Analyzer) is the path-sensitive analyzer that
ships with clang; it complements `cppcheck` (style / pattern matching)
by symbolically executing every path through every translation unit and
flagging null-deref, use-after-free, division-by-zero, dead-store,
uninitialized-read and similar logic errors.

ra8d2-firmware runs scan-build against the **host unit-test build**
(`tests/build-scan/`) -- the cross-compile firmware build cannot be
analyzed reliably because clang has no working sysroot for
`arm-none-eabi`. The host build covers the same first-party `libs/`,
`src/`, and `port/` translation units, just with mocked HAL.

## How to run

```sh
# Local: full analyzer pass + summary
make scan-build

# CI gate (fails on any first-party finding):
bash scripts/utils/scan_build.sh --check
```

Reports land under `build/scan-build-reports/<timestamp>/`. The
top-level `index.html` aggregates every finding with full path
visualisations.

## Suppression policy

Two source partitions are silenced by the wrapper (see
`scripts/utils/scan_build.sh`):

| Partition                | Why suppressed                                        |
|--------------------------|-------------------------------------------------------|
| `libs/third_party/`      | SOUP -- pre-qualified external code, see `docs/SOUP/` |
| `tests/`                 | Host-only test scaffolding, exempt per CLAUDE.md      |

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
findings against the host test build:

| Count | Checker                          | Status     |
|-------|----------------------------------|------------|
| 683   | `core.FixedAddressDereference`   | **Expected -- hardware-register accessor pattern** |
| 2     | `core.DivideZero`                | **Expected -- false positive across function boundary** |

### `core.FixedAddressDereference` (683 findings)

Every memory-mapped register access in the HAL goes through an inline
accessor that returns a pointer cast from a `uintptr_t` enum constant
(see CLAUDE.md "Hardware Register Access"):

```c
typedef enum : uintptr_t {
    k_ra_vin_base = 0x40169000,
} ra_vin_addr_t;

static inline volatile uint32_t* ra_vin_reg32(uint32_t off) {
    return (volatile uint32_t*)(k_ra_vin_base + off);
}

// ... later ...
*ra_vin_reg32(k_ra_vin_off_ints) = (uint32_t)k_ra_vin_int_fme;
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
mock register banks.

### `core.DivideZero` (2 findings, both in `ra_jpeg_sw.c`)

Both findings sit inside the JPEG decoder MCU loop:

```c
// libs/ra_hal/src/ra_jpeg_sw.c -- decode_mcu_loop()
uint16_t mcus_x = (uint16_t)((d->width  + mcu_w_px - 1U) / mcu_w_px);
uint16_t mcus_y = (uint16_t)((d->height + mcu_h_px - 1U) / mcu_h_px);
```

`mcu_w_px` and `mcu_h_px` are derived from `d->hmax` / `d->vmax`, both
of which are validated upstream in `ra_jpeg_sw_parse_sof()` -- the
parser only returns success for chroma layouts 4:4:4, 4:2:2 (h+v),
4:2:2 (h-only), 4:2:0, all of which set `hmax`/`vmax` to a non-zero
value. The analyzer cannot follow the cross-function invariant and
reports the classic intra-procedural false positive. Documented here
rather than mitigated with a redundant guard.

## CI / hook integration

The per-commit hook does **not** invoke scan-build (a full pass takes
several minutes -- too expensive to gate every commit). Instead:

* Developers run `make scan-build` locally before opening a PR.
* CI runs `bash scripts/utils/scan_build.sh --check` on every push;
  today the gate is **warn-only** because the 685 first-party findings
  (683 `core.FixedAddressDereference` + 2 `core.DivideZero` false
  positives) are fully accounted for in this file. The gate will be
  flipped to strict once those are annotated, suppressed, or
  eliminated.

## Cross-references

* `scripts/utils/scan_build.sh`  -- driver script
* `cmake/ra_warnings.cmake`      -- per-target warning flags (-Wall etc.)
* `docs/SOUP/README.md`          -- third-party qualification index
* `docs/MCDC.md`                 -- complementary MC/DC coverage gate
