# ra8d2-firmware

Bare-metal firmware for the Renesas **RA8D2** MCU group, targeting the
**EK-RA8D2** evaluation kit (part number 968-K7EKA8D2S01001BE).

Personal in-house project by Brighton Sikarskie. No vendor FSP code is
checked in -- every `.c` / `.h` in `src/` and `libs/` is hand-written
against the RA8D2 Hardware User's Manual.

## Target

| | |
|---|---|
| MCU | `R7KA8D2KFLCAC` |
| Primary core | Arm Cortex-M85 @ 1 GHz (Helium / MVE) |
| Secondary core | Arm Cortex-M33 @ 250 MHz |
| Code memory | 1 MB MRAM |
| SRAM | 2 MB (ECC) |
| Package | 289-BGA (12 mm x 12 mm, 0.65 mm pitch) |
| Debugger | On-board SEGGER J-Link OB (SWD) |

## Status

The tree currently has:

- **7 core modules** (`libs/ra_core/src/`): error codes, check macros,
  pin validator, time, exception handler, error-handler sink,
  infrastructure bring-up with stack canary, logging backend, watchdog
  refresh.
- **29 HAL driver sources** (`libs/ra_hal/src/`): GPIO, SCI UART, SPI,
  I2C, ADC, timers (AGT / GPT / SysTick), CAC, CANFD (bit-rate + TX +
  RX + error state), CGC with full PLL bring-up, CRC, DMAC, DTC, ELC,
  GLCDC, ICU, IWDT, RTC, SDRAMC, USB, xSPI (flash read / program /
  erase / ID / status), plus DOC, ACMPHS, DAC_B, ULPT, TRNG and SCE.
- **40 register header files** (`libs/ra_hal/inc/ra8d2_*_regs.h`) --
  hand-written struct layouts + inline accessor functions for every
  peripheral the drivers touch.
- **41 host unit-test binaries** (`tests/test_*.c`) -- one ctest
  target per source module, running against `ra_sim_mmap.c` which
  installs anonymous mmap backings at every RA8D2 MMIO window so the
  real driver code path runs on the x86_64 host unmodified.
- **Coverage**: gcovr gate enforces >= 90% line and branch coverage.
  Current snapshot: **98.0% line / 92.3% branch / 99.5% function**.

## Build

### Cross-compile for the MCU

```bash
# Prerequisites:
#   - CMake >= 3.20
#   - ARM GNU Toolchain (arm-none-eabi-gcc) in PATH

cmake -B build -S . \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8d2.cmake \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Artifacts land in `build/`:

```
build/ra8d2-firmware.elf    ELF with DWARF symbols
build/ra8d2-firmware.hex    Intel HEX (for flashers)
build/ra8d2-firmware.bin    Raw binary
build/ra8d2-firmware.map    Linker map
```

### Host unit tests

```bash
cmake -B build/tidy -S tests -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/tidy
ctest --test-dir build/tidy --output-on-failure
```

### Coverage gate

```bash
bash scripts/coverage.sh           # Build, run, emit HTML + text + JSON
bash scripts/coverage.sh --gate    # Same, plus fail if <90% line/branch
```

Report lands in `build/coverage/coverage/index.html`.

## Lint and format

```bash
bash scripts/format_code.sh            # Format in place
bash scripts/format_code.sh --check    # Verify formatting only
bash scripts/clang_tidy.sh             # Run clang-tidy
bash scripts/clang_tidy.sh --fix       # Apply clang-tidy auto-fixes
```

A pre-commit hook at `scripts/git/pre-commit` runs ASCII check,
C23-pattern check, clang-format, clang-tidy, `@since` tag check,
copyright header check, and cppcheck on every commit. Install with:

```bash
ln -s ../../scripts/git/pre-commit .git/hooks/pre-commit
```

## CI

`.github/workflows/firmware.yml` runs every gate on push to `main` and
on every pull request:

| Job | What |
|---|---|
| `ascii` | Reject any non-ASCII byte in source files |
| `copyright` | Require MIT SPDX + Brighton Sikarskie header |
| `since` | Require `@since` on every public function decl |
| `format` | `clang-format --dry-run -Werror` |
| `tidy` | `clang-tidy` with project `.clang-tidy` ruleset |
| `test` | `ctest` over every `test_ra_*.c` |
| `coverage` | `gcovr --fail-under-line 90 --fail-under-branch 90` |
| `cppcheck` | `cppcheck --enable=warning,style,performance,portability` |
| `build` | Cross-compile with `arm-none-eabi-gcc` |
| `docs` | Doxygen with zero-warning gate |

## Reference material

Committed under [`docs/reference/`](docs/reference/):

- `ra8d2-datasheet.pdf` -- electrical specs, pin lists
- `ra8d2-hardware-user-manual.pdf` -- primary register reference
- `ra8d2-technical-brief.pdf` -- high-level overview
- `ra8d2-high-temperature-operation.pdf` -- hi-temp application note

## License

MIT, Copyright (c) 2026 Brighton Sikarskie. See [LICENSE.txt](LICENSE.txt).
