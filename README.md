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
| Debugger | On-board SEGGER J-Link OB (RA4M2-hosted) |

## What's in the tree

| Layer | Where | Count |
|---|---|---:|
| Core utilities (no HW deps) | `libs/ra_core/` | 7 modules |
| HAL drivers | `libs/ra_hal/src/` | 60 |
| HAL public headers | `libs/ra_hal/inc/ra_*.h` | 65 |
| Register headers (hand-written) | `libs/ra_hal/inc/ra8d2_*_regs.h` | 58 |
| NSC veneers (TrustZone bridges) | `libs/ra_nsc/` | 7 modules |
| Net / USB PALs | `libs/ra_net_pal/`, `libs/ra_usb_pal/` | 2 |
| Boot / vector table / linker | `src/boot/` | 4 files |
| Secure-app code | `src/secure_app/` | 2 files |
| Application examples | `examples/<name>/main.c` | see [examples/](examples/README.md) |
| Host unit tests | `tests/test_*.c` | 86 |

The HAL covers every peripheral the RA8D2 exposes: the full
graphics pipeline (CEU, DRW, GLCDC, MIPI-PHY/DSI/CSI, VIN), MRAM
flash programming, SRAM ECC, USB FS+HS device-mode pipes, ADC scan
modes, dual-core IPC, RSIP secure crypto, low-power modes,
voltage-detect, battery-backup, every timer family, every comm
interface (SCI / IIC / SPI / I3C / xSPI / SDHI / CANFD / Ethernet
agent + MAC + switch fabric), watchdog, reset cause introspection,
and on. See `docs/ROADMAP.md` for the per-driver status table.

## Quick start

```sh
make example-blink     # cross-compile examples/blink/main.c
make flash             # flash via on-board J-Link OB
```

The blink demo toggles `P6_00` / `P6_01` / `P6_02` / `P3_03` /
`P10_07` at 1 Hz so whichever pin actually wires to a board LED on
your EK-RA8D2 lights up.

## Make targets

```
make build           cross-compile examples/blink (default)
make example-<name>  cross-compile examples/<name>/main.c
make examples        list every available example
make flash           ./scripts/flash.sh against the J-Link OB
make ozone           open SEGGER Ozone GUI debugger
make debug           attach gdb via JLinkGDBServer
make test            host-compile + run unit tests (Linux native)
make test-docker     host-compile + run unit tests in Linux container
                     (use this on macOS where MAP_FIXED is blocked)
make tidy            run clang-tidy over the whole tree
make format          run clang-format in place
make check           run clang-format --dry-run
make docs            run doxygen
make ascii           verify ASCII-only source files
make size            arm-none-eabi-size on the ELF
make clean           remove build/ and tests/build/
```

## Cross-build artifacts

After `make build`, artifacts land in `build/`:

```
build/ra8d2-firmware.elf    ELF with DWARF symbols (debugger target)
build/ra8d2-firmware.hex    Intel HEX (for flashers)
build/ra8d2-firmware.bin    Raw binary (zero-padded; rarely needed)
build/ra8d2-firmware.map    Linker map
```

## Flashing

The on-board RA4M2 J-Link OB on the EK-RA8D2 is recognised by
`SEGGER J-Link Software Pack` (install via `brew install --cask segger-jlink`
or from segger.com).

```sh
make flash             # uses scripts/flash.sh
```

The script invokes `JLinkExe -device R7KA8D2KF_CPU0 -if SWD -NoGui 1`
and loads `build/ra8d2-firmware.hex` into MRAM at `0x02000000`.

## Debugging

```sh
make ozone             # SEGGER Ozone GUI -- best for HardFault triage
make debug             # gdb attached via JLinkGDBServer
```

The Ozone project file is committed at `scripts/ra8d2.jdebug` so
device + interface + ELF path are version-controlled.

## Host unit tests

```sh
make test              # Linux: native ctest run
make test-docker       # macOS: same suite, run inside an Ubuntu container
```

The test suite uses `tests/mocks/ra_sim_mmap.c` to install anonymous
mmap backings at every RA8D2 MMIO window, so the actual production
driver code paths run on the x86_64 host unmodified. On Apple Silicon
the kernel rejects `MAP_FIXED` at low addresses regardless of
`-pagezero_size`, so the macOS path goes through Docker.

## Lint, format, and pre-commit

```sh
make tidy              # clang-tidy with project .clang-tidy ruleset
make format            # clang-format --in-place
make check             # clang-format --dry-run -Werror
```

A pre-commit hook at `scripts/git/pre-commit` runs ASCII check, C23
pattern check, clang-format, clang-tidy, `@since` tag check,
copyright header check, world-tag check, HUM-citation check, and
cppcheck on every commit. Install with:

```sh
ln -s ../../scripts/git/pre-commit .git/hooks/pre-commit
```

## CI

`.github/workflows/firmware.yml` runs every gate on push to `main`
and on every pull request.

## Project conventions

| Topic | Doc |
|---|---|
| File-header Doxygen, naming, types, NASA Power-of-10, SOLID | [`docs/STYLE_GUIDE.md`](docs/STYLE_GUIDE.md) |
| Architectural rings + TrustZone world tagging | [`docs/RING_AND_WORLD.md`](docs/RING_AND_WORLD.md) |
| Layered architecture overview | [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) |
| Per-driver status / roadmap | [`docs/ROADMAP.md`](docs/ROADMAP.md) |
| HUM chapter -> page-range map | [`docs/reference/CHAPTER_MAP.md`](docs/reference/CHAPTER_MAP.md) |

CLAUDE.md (project root) summarises the most-violated style rules
for AI-assisted contributors; `docs/STYLE_GUIDE.md` is the
authoritative source.

## Reference material

Committed under [`docs/reference/`](docs/reference/):

- `ra8d2-datasheet.pdf` -- electrical specs, pin lists
- `ra8d2-hardware-user-manual.pdf` -- primary register reference (R01UH1065EJ)
- `ra8d2-technical-brief.pdf` -- high-level overview
- `ra8d2-high-temperature-operation.pdf` -- hi-temp application note

The HUM is also pre-split into per-page PDFs under
`docs/reference/ra8d2-hardware-user-manual/page-NNNN.pdf` so
`scripts/utils/cite_check.py` can validate every `/* HUM Ch X.Y "..."
p NNNN */` annotation in the source against the chapter's documented
page range.

## License

MIT. See `LICENSE.txt`.
