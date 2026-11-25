# Paired C and Rust HIL Examples

This document defines the directory structure, build wiring, flashing
commands, shared behavior contract, and `ra8ci` HIL-discovery conventions for
paired C and Rust example applications exercising the same Zig library C ABI.

---

## Purpose

A paired example proves that one Zig library C ABI (`libs/<lib>/inc/` backed by
`libs/<lib>/src/`) delivers identical behavior across C23 and Rust firmware
clients on the EK-RA8D2 bench and in headless emulation (`tools/ra8_emulator`).
Both clients execute the exact same canonical behavior vectors rather than
maintaining separate test suites.

---

## Directory Layout

Hardware-in-the-loop (HIL) automation and app discovery live in the sibling
`bsikar/ra8ci` repository, not inside a GitHub Actions workflow in this tree.
The layout of every paired example is dictated by three repository gates:

1. `ra8ci` discovers HIL examples at depth 1 inside
   `examples/ek_ra8d2/hw_validated/hil/`.
2. `ra8ci` parity and alive-policy gates require every immediate subdirectory
   of `examples/ek_ra8d2/hw_validated/hil/` to carry its own `hil.conf`.
3. `scripts/dev/ra8_apps.py` indexes firmware apps by each directory holding a
   `CMakeLists.txt`, and source layout checks require `.c` and `.h` files inside
   `examples/` to reside under `src/` or `inc/`.

Because nesting language subdirectories under `hw_validated/hil/` would either
break discovery or trigger missing-manifest failures, every paired example uses
two peer directories at depth 1 inside the validation tier:

```text
examples/ek_ra8d2/<tier>/
  <pair>_c/
    CMakeLists.txt
    README.md
    hil.conf
    src/
      main.c
  <pair>_rust/
    Cargo.toml        (plus Rust crate/build files when target toolchain lands)
    README.md
    hil.conf
    src/
      main.rs
```

### Tier Lifecycle (`examples/ek_ra8d2/`)

1. **Bringup (`examples/ek_ra8d2/hw_pending/`)**: new clients start here until
   they pass both local builds and physical bench validation. `ra8ci` still
   validates every `hil.conf` under `examples/` against its typed manifest
   schema, so malformed manifests fail immediately even before promotion.
2. **Bench Promotion (`examples/ek_ra8d2/hw_validated/hil/`)**: once both C
   and Rust clients pass bench verification on EK-RA8D2 hardware, promote the
   peer directories together into `examples/ek_ra8d2/hw_validated/hil/` via
   `git mv` and regenerate the build-graph shape ledger.

### Shared Behavior Contract (`tests/zig_abi_pairs/`)

Behavior specifications are shared in one directory per pair rather than
duplicated inside the two example apps:

```text
tests/zig_abi_pairs/
  <pair>/
    README.md          Human-readable contract, state setup, and assertions
    vectors.json       Canonical machine-readable inputs and expected results
    inc/               Checked C23 projection verified against vectors.json
```

Both clients MUST evaluate every vector defined in the shared pair contract and
report PASS only when all vectors match expected return codes and outputs.

---

## Building

### Root Zig Build Graph

Firmware images built through the root `build.zig` graph are declared as
`CrossApp` entries in `tests/zig_build_graph/app_table_tail.zig`:

- `.name`: matches the directory basename (`<pair>_c` or `<pair>_rust`).
- `.dir`: repository-relative path to the client directory.
- `.board`: `"libs/ra8_board_ek_ra8d2"`, paired with its linker script.
- `.libraries` and `.zig_libraries`: declare the target Zig library and any
  compiled C dependencies; include-only companion headers (or shared vector
  headers under `tests/zig_abi_pairs/`) belong in `.local.include_dirs` unless
  the companion archive itself is registered in `build.zig.zon`.

Useful root Zig build commands (`just/zig.just`):

- `just zig::arm` -- cross-build every registered ARM image (ELF, HEX, BIN in
  `zig-out/`). Zig archives match CMake optimization via `RA8_ZIG_OPTIMIZE`
  (`Debug` for debug builds, `ReleaseSmall` otherwise per
  `cmake/ra8_app/zig_libs.cmake`).
- `just zig::shapes` -- verify `tests/zig_build_graph/app_shape_ledger.tsv`
  matches all `ra8_add_app()` declarations in the tree (regenerate with
  `zig build shapes`).
- `just zig::abi` and `just zig::test` -- run host ABI checks and C/Zig unit
  suites governed by `config/zig_abi_policy.json`.

### CMake and `just`

C clients provide a `CMakeLists.txt` that includes `cmake/ra8_add_app.cmake`,
lists dependencies via `ra8_add_app(NAME ... STACK_BYTES ... LIBS ...)`, and
uses `void main(void)` with `#include "ra8_boot_entry.h"`. CMake links any
named `LIBS` entry that contains a `build.zig` as a Zig static archive through
`cmake/ra8_app/zig_libs.cmake`. Header-only extra include directories (such as
the pair's shared vector headers) are attached with
`target_include_directories` on the app's `.elf` target.

- Single app build: `just apps::build <app>` (emits ELF, HEX, and BIN inside
  `<app_dir>/build/` and runs `scripts/checks/check_freestanding_runtime.py`).
- Standalone CMake configure/build:

```sh
cmake -S <app_dir> -B <app_dir>/build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build <app_dir>/build
```

---

## Flashing

### Local EK-RA8D2 Bench (`just/hw.just`)

- J-Link build and flash: `just apps::hardware::flash <app>` (invokes
  `scripts/dev/flash.sh` on `<app_dir>/build/<app>.hex`).
- OpenOCD build and flash: `just apps::hardware::flash <app> ocd=1` (invokes
  `scripts/dev/openocd_flash.sh`).
- Build, flash, and stream SCI8 UART (`115200` baud):
  `just apps::hardware::monitor app=<app>`.
- Flash a prebuilt image directly (from either CMake or `zig-out/`):
  `scripts/dev/flash.sh <path-to-hex>`.

### Automated `ra8ci` Bench

When `ra8ci` executes hardware validation for `examples/ek_ra8d2/hw_validated/hil/`,
it builds each discovered app into `<app_dir>/build/` and programs the bench
board from `<app_dir>/build/<app>.hex` using J-Link before attaching
its UART/RTT/J-Link oracle.

---

## `ra8ci` HIL Discovery and Manifest Contract

`ra8ci` owns HIL test enumeration and enforcement:

- **Discovery scope**: `ra8ci` enumerates every immediate subdirectory of
  `examples/ek_ra8d2/hw_validated/hil/` and requires a `hil.conf` inside each
  one.
- **Typed manifest schema**: every `hil.conf` in `examples/` (across all tiers,
  including `hw_pending/`) must use only keys accepted by `ra8ci`'s schema
  parser (for example `HIL_MODE`, `HIL_EXPECT`, `HIL_EXPECT_NEGATIVE`, and
  `HIL_TIMEOUT_S`). Unknown keys, `..` traversals, symlinks, and backslashes
  are rejected.
- **Active verification policy**: functional pair examples must declare an
  active oracle such as `HIL_MODE=uart_scrape`, never bare `HIL_MODE=alive`
  (which `ra8ci` reserves strictly for intentional fault tests marked with
  `HIL_FAULT_EXPECTED=1`).
- **UART verdict contract**: both `<pair>_c/hil.conf` and `<pair>_rust/hil.conf`
  should assert the same UART verdict format over SCI8 at `115200` baud:
  - Keep `HIL_EXPECT` at least 12 characters long (`HIL_EXPECT_SHORT_OK` off by
    default).
  - Ensure `HIL_EXPECT` does not match any failure, panic, or error `.rodata`
    literal compiled into the image.
  - Include a failure marker covered by `HIL_EXPECT_NEGATIVE` so failing builds
    fail immediately instead of timing out.
- **Emulator parity**: before bench promotion into `hw_validated/hil/`, run the
  built ELF binaries through `tools/ra8_emulator` (`scripts/emu/eil_all.sh`
  for validated HIL apps) to verify the `HIL_EXPECT` line is emitted cleanly on
  SCI8 with zero negative matches or CPU faults.
